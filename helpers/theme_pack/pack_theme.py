#!/usr/bin/env python3
"""Build a Nextube theme pack and keep themes/catalog.json in step with it.

    pack_theme.py add <source_dir> --id Nixie --name "Nixie" [--family Nixie] [--variant Orange]
                  [--category legacy] [--bundled | --no-bundled] [--min-fw 1.20.0] [--notes "what changed"]
    pack_theme.py remove <id>
    pack_theme.py stamp [--version N]
    pack_theme.py previews [--id Nixie]
    pack_theme.py check

<source_dir> is the theme's own folder (the one that holds Numbers/, AMPM/, ...). The pack is a ZIP with every file stored
uncompressed (the device's extractor only reads STORE entries; JPEGs don't compress anyway) under images/themes/<id>/, plus a
small .pack.json the device reads back to know which revision is installed.

Revisions: the packer hashes the theme's content. Same content -> same revision, nothing changes. New content -> rev + 1, a new
immutable <id>-r<rev>.zip and the previous zip is deleted.

Catalog version: add / remove never touch pack_version or the date. Run `stamp` once after a batch of changes: it fingerprints the
catalog's theme data and, only if that differs from the fingerprint saved at the last stamp, raises pack_version by one and sets
`updated` to today. `check` fails while the theme data and the saved fingerprint disagree, so a change can't be published unstamped.
"""
import argparse, datetime, hashlib, io, json, os, re, struct, sys, zipfile

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
THEMES = os.path.join(REPO, "themes")
PACKS = os.path.join(THEMES, "packs")
PREVIEWS = os.path.join(THEMES, "previews")
PREVIEW_H = 48            # px; ten digits side by side come to about 260 x 48 (6-8 KB)
CATALOG = os.path.join(THEMES, "catalog.json")
SCHEMA = 1
MAX_PACK = 1024 * 1024          # the device refuses bigger downloads
ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,31}$")
SKIP = {".DS_Store", "Thumbs.db", ".pack.json"}


def load_catalog():
    if os.path.exists(CATALOG):
        with open(CATALOG, encoding="utf-8") as f:
            return json.load(f)
    return {"schema": SCHEMA, "pack_version": 0, "updated": "", "data_sha256": "", "themes": []}


def themes_hash(cat):
    """Fingerprint of the catalog's theme data (everything the clock reads about a theme)."""
    return hashlib.sha256(json.dumps(cat["themes"], sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")).hexdigest()


def save_catalog(cat):
    os.makedirs(THEMES, exist_ok=True)
    with open(CATALOG, "w", encoding="utf-8", newline="\n") as f:
        json.dump(cat, f, indent=2, ensure_ascii=False)
        f.write("\n")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


def theme_files(src):
    out = []
    for root, dirs, files in os.walk(src):
        dirs[:] = sorted(d for d in dirs if not d.startswith("."))
        for name in sorted(files):
            if name in SKIP or name.startswith("."):
                continue
            full = os.path.join(root, name)
            out.append((os.path.relpath(full, src).replace(os.sep, "/"), full))
    return sorted(out)


def content_hash(files):
    """Identity of the theme's content, independent of how it is zipped."""
    h = hashlib.sha256()
    for rel, full in files:
        with open(full, "rb") as f:
            data = f.read()
        h.update(rel.encode("utf-8") + b"\0" + struct.pack("<I", len(data)) + hashlib.sha256(data).digest())
    return h.hexdigest()


def make_previews(zpath, tid):
    """Two preview images built from the pack's own Numbers/0-9 files (so they always match the pack):
    previews/<id>.jpg      thumbnail strip of the ten digits, about 258 x 48
    previews/full/<id>.jpg every digit at its real tube size (80 x 160) in two rows of five, for the click-to-enlarge view
    Needs Pillow. Returns (strip, full) as catalog-relative paths, or ("", "") if they cannot be built."""
    try:
        from PIL import Image
    except ImportError:
        print("note: Pillow is not installed, so no previews were built (pip install pillow)")
        return "", ""
    digits = []
    with zipfile.ZipFile(zpath) as z:
        names = set(z.namelist())
        for d in range(10):
            for ext in ("jpg", "png"):
                n = f"images/themes/{tid}/Numbers/{d}.{ext}"
                if n in names:
                    digits.append(Image.open(io.BytesIO(z.read(n))).convert("RGB"))
                    break
    if len(digits) != 10:
        print("note: Numbers/0-9 not all found in the pack, so no previews were built")
        return "", ""
    scaled = [im.resize((max(1, round(im.width * PREVIEW_H / im.height)), PREVIEW_H), Image.LANCZOS) for im in digits]
    gap = 2
    strip = Image.new("RGB", (sum(i.width for i in scaled) + gap * 9, PREVIEW_H), (0, 0, 0))
    x = 0
    for im in scaled:
        strip.paste(im, (x, 0))
        x += im.width + gap
    w, h = digits[0].size
    g = 4
    sheet = Image.new("RGB", (5 * w + 4 * g, 2 * h + g), (0, 0, 0))
    for i, im in enumerate(digits):
        sheet.paste(im, ((i % 5) * (w + g), (i // 5) * (h + g)))
    os.makedirs(os.path.join(PREVIEWS, "full"), exist_ok=True)
    strip.save(os.path.join(PREVIEWS, f"{tid}.jpg"), "JPEG", quality=80, optimize=True)
    sheet.save(os.path.join(PREVIEWS, "full", f"{tid}.jpg"), "JPEG", quality=88, optimize=True)
    return f"previews/{tid}.jpg", f"previews/full/{tid}.jpg"


def write_zip(path, tid, files, marker):
    """Deterministic STORE zip: sorted entries, fixed timestamps, so equal content gives an equal file."""
    with zipfile.ZipFile(path, "w", zipfile.ZIP_STORED) as z:
        entries = [(f"images/themes/{tid}/{rel}", full) for rel, full in files]
        for arc, full in entries:
            zi = zipfile.ZipInfo(arc, date_time=(1980, 1, 1, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_STORED
            zi.external_attr = 0o644 << 16
            with open(full, "rb") as f:
                z.writestr(zi, f.read())
        zi = zipfile.ZipInfo(f"images/themes/{tid}/.pack.json", date_time=(1980, 1, 1, 0, 0, 0))
        zi.compress_type = zipfile.ZIP_STORED
        zi.external_attr = 0o644 << 16
        z.writestr(zi, json.dumps(marker, sort_keys=True, separators=(",", ":")))


def cmd_add(a):
    if not ID_RE.match(a.id):
        sys.exit("id may only contain letters, digits, '_' and '-' (it becomes the folder name on the clock)")
    src = os.path.abspath(a.source_dir)
    files = theme_files(src)
    if not files:
        sys.exit(f"no files found in {src}")
    chash = content_hash(files)
    cat = load_catalog()
    old = next((t for t in cat["themes"] if t["id"] == a.id), None)
    if old and old.get("content_sha256") == chash and not (a.notes or a.name or a.family or a.variant or a.min_fw or a.category is not None or a.no_bundled):
        print(f"{a.id}: unchanged (rev {old['rev']}), nothing to do")
        return
    changed_content = not old or old.get("content_sha256") != chash
    rev = (old["rev"] + 1) if old and changed_content else (old["rev"] if old else 1)
    marker = {"id": a.id, "rev": rev, "content_sha256": chash, "files": len(files)}
    os.makedirs(PACKS, exist_ok=True)
    zname = f"{a.id}-r{rev}.zip"
    zpath = os.path.join(PACKS, zname)
    write_zip(zpath, a.id, files, marker)
    if a.bundled:
        # The copy that ships inside the firmware carries the same marker, so a clock can tell which revision it has.
        with open(os.path.join(src, ".pack.json"), "w", encoding="utf-8", newline="") as f:
            f.write(json.dumps(marker, sort_keys=True, separators=(",", ":")))
    preview, preview_full = make_previews(zpath, a.id)
    if not preview and old:
        preview, preview_full = old.get("preview", ""), old.get("preview_full", "")
    size = os.path.getsize(zpath)
    if size > MAX_PACK:
        os.remove(zpath)
        sys.exit(f"{zname} would be {size} bytes; the device accepts packs up to {MAX_PACK}")
    entry = {
        "id": a.id,
        "name": a.name or (old or {}).get("name") or a.id,
        "family": a.family or (old or {}).get("family") or a.id,
        "variant": a.variant if a.variant is not None else (old or {}).get("variant", ""),
        "category": a.category if a.category is not None else (old or {}).get("category", ""),
        "rev": rev,
        "zip": f"packs/{zname}",
        "preview": preview,
        "preview_full": preview_full,
        "size": size,
        "sha256": sha256_file(zpath),
        "content_sha256": chash,
        "files": len(files),
        "bundled": bool(a.bundled) if (a.bundled or a.no_bundled or not old) else old.get("bundled", False),
        "min_fw": a.min_fw or (old or {}).get("min_fw", ""),
        "notes": a.notes or ((old or {}).get("notes", "") if not changed_content else ""),
    }
    if old:
        if changed_content:
            prev = os.path.join(THEMES, old["zip"])
            if os.path.exists(prev) and os.path.abspath(prev) != os.path.abspath(zpath):
                os.remove(prev)
        cat["themes"][cat["themes"].index(old)] = entry
    else:
        cat["themes"].append(entry)
    cat["themes"].sort(key=lambda t: (t["family"].lower(), t["variant"].lower(), t["id"].lower()))
    cat["schema"] = SCHEMA
    save_catalog(cat)
    print(f"{a.id}: rev {rev}, {len(files)} files, {size} bytes -> packs/{zname}" + ("" if themes_hash(cat) == cat.get("data_sha256") else " (catalog changed: run stamp)"))


def cmd_remove(a):
    cat = load_catalog()
    t = next((t for t in cat["themes"] if t["id"] == a.id), None)
    if not t:
        sys.exit(f"{a.id} is not in the catalog")
    for rel in (t["zip"], t.get("preview", ""), t.get("preview_full", "")):
        p = os.path.join(THEMES, rel) if rel else ""
        if p and os.path.exists(p):
            os.remove(p)
    cat["themes"].remove(t)
    save_catalog(cat)
    print(f"{a.id}: removed (catalog changed: run stamp)")


def cmd_previews(a):
    """Rebuild the preview images of every catalog theme (or one, with --id) from its pack. Content is unchanged, so no new revision."""
    cat = load_catalog()
    n = 0
    for t in cat["themes"]:
        if a.id and t["id"] != a.id:
            continue
        prev, full = make_previews(os.path.join(THEMES, t["zip"]), t["id"])
        if prev:
            t["preview"], t["preview_full"] = prev, full
            n += 1
    save_catalog(cat)
    print(f"previews rebuilt for {n} theme(s)" + ("" if themes_hash(cat) == cat.get("data_sha256") else " (catalog changed: run stamp)"))


def cmd_stamp(a):
    cat = load_catalog()
    h = themes_hash(cat)
    if a.version is None and h == cat.get("data_sha256"):
        print(f"no theme data changed since the last stamp: pack_version stays {cat.get('pack_version')}, updated {cat.get('updated')}")
        return
    cat["pack_version"] = a.version if a.version is not None else int(cat.get("pack_version", 0)) + 1
    cat["updated"] = datetime.date.today().isoformat()
    cat["data_sha256"] = h
    save_catalog(cat)
    print(f"stamped: pack_version {cat['pack_version']}, updated {cat['updated']}")


def cmd_check(_a):
    cat = load_catalog()
    bad = []
    if themes_hash(cat) != cat.get("data_sha256"):
        bad.append("the theme data in catalog.json changed since the last stamp (run 'stamp' once after your changes)")
    seen = set()
    seen_prev = set()
    for t in cat["themes"]:
        p = os.path.join(THEMES, t["zip"])
        seen.add(os.path.abspath(p))
        for key in ("preview", "preview_full"):
            if t.get(key):
                pv = os.path.join(THEMES, t[key])
                seen_prev.add(os.path.abspath(pv))
                if not os.path.exists(pv):
                    bad.append(f"{t['id']}: {t[key]} is missing")
        if not os.path.exists(p):
            bad.append(f"{t['id']}: {t['zip']} is missing")
            continue
        if t["zip"] != f"packs/{t['id']}-r{t['rev']}.zip":
            bad.append(f"{t['id']}: zip name should be packs/{t['id']}-r{t['rev']}.zip")
        if os.path.getsize(p) != t["size"]:
            bad.append(f"{t['id']}: size differs from the catalog")
        if sha256_file(p) != t["sha256"]:
            bad.append(f"{t['id']}: sha256 differs from the catalog (rebuild with 'add')")
        if t["size"] > MAX_PACK:
            bad.append(f"{t['id']}: pack is larger than {MAX_PACK} bytes")
        if t.get("bundled"):
            d = os.path.join(REPO, "data", "images", "themes", t["id"])
            if not os.path.isdir(d):
                bad.append(f"{t['id']}: marked bundled but data/images/themes/{t['id']} does not exist")
            else:
                if content_hash(theme_files(d)) != t["content_sha256"]:
                    bad.append(f"{t['id']}: data/images/themes/{t['id']} no longer matches the catalog (run 'add --bundled' on it)")
                try:
                    with open(os.path.join(d, ".pack.json"), encoding="utf-8") as f:
                        mk = json.load(f)
                    if mk.get("rev") != t["rev"] or mk.get("content_sha256") != t["content_sha256"]:
                        bad.append(f"{t['id']}: the .pack.json in data/images/themes/{t['id']} is out of date (run 'add --bundled' on it)")
                except Exception:               # noqa: BLE001
                    bad.append(f"{t['id']}: data/images/themes/{t['id']}/.pack.json is missing (run 'add --bundled' on it)")
        try:
            with zipfile.ZipFile(p) as z:
                if any(i.compress_type != zipfile.ZIP_STORED for i in z.infolist()):
                    bad.append(f"{t['id']}: pack has compressed entries (the device reads STORE only)")
                names = [i.filename for i in z.infolist()]
                if any(not n.startswith(f"images/themes/{t['id']}/") for n in names):
                    bad.append(f"{t['id']}: pack has files outside images/themes/{t['id']}/")
                m = json.loads(z.read(f"images/themes/{t['id']}/.pack.json"))
                if m.get("rev") != t["rev"] or m.get("content_sha256") != t["content_sha256"]:
                    bad.append(f"{t['id']}: .pack.json inside the zip does not match the catalog")
        except Exception as e:                      # noqa: BLE001
            bad.append(f"{t['id']}: cannot read the pack ({e})")
    if os.path.isdir(PACKS):
        for n in os.listdir(PACKS):
            if n.startswith("."):
                continue          # .gitkeep
            if os.path.abspath(os.path.join(PACKS, n)) not in seen:
                bad.append(f"packs/{n} is not referenced by the catalog")
    for d, label in ((PREVIEWS, "previews"), (os.path.join(PREVIEWS, "full"), "previews/full")):
        if os.path.isdir(d):
            for n in os.listdir(d):
                full = os.path.join(d, n)
                if n.startswith(".") or os.path.isdir(full):
                    continue
                if os.path.abspath(full) not in seen_prev:
                    bad.append(f"{label}/{n} is not referenced by the catalog")
    for b in bad:
        print("ERROR:", b)
    print("catalog OK" if not bad else f"{len(bad)} problem(s)")
    sys.exit(1 if bad else 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("add", help="build / update a theme pack")
    p.add_argument("source_dir")
    p.add_argument("--id", required=True)
    p.add_argument("--name")
    p.add_argument("--family")
    p.add_argument("--variant")
    p.add_argument("--category", choices=["", "legacy"], help="'legacy' lists the theme under Legacy in the Theme Manager; '' puts it back in the main list")
    p.add_argument("--bundled", action="store_true", help="the theme also ships inside the firmware (its .pack.json is written into <source_dir> so the shipped copy knows its revision)")
    p.add_argument("--no-bundled", dest="no_bundled", action="store_true", help="the theme no longer ships inside the firmware (catalog download only); delete data/images/themes/<id> yourself")
    p.add_argument("--min-fw", dest="min_fw")
    p.add_argument("--notes")
    p.set_defaults(fn=cmd_add)
    p = sub.add_parser("remove")
    p.add_argument("id")
    p.set_defaults(fn=cmd_remove)
    p = sub.add_parser("previews", help="rebuild the preview images of every theme (or --id) from its pack")
    p.add_argument("--id")
    p.set_defaults(fn=cmd_previews)
    p = sub.add_parser("stamp", help="raise pack_version and set the date, only if the theme data changed since the last stamp")
    p.add_argument("--version", type=int, help="set this pack_version explicitly (and stamp), e.g. 1 for a first publish")
    p.set_defaults(fn=cmd_stamp)
    p = sub.add_parser("check", help="verify the catalog against the zips (exit 1 on any problem)")
    p.set_defaults(fn=cmd_check)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
