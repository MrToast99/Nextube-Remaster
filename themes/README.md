# Theme packs

Downloadable themes for the clock. The clock's **Theme Manager** reads `catalog.json` from this folder and installs or removes
the packs listed in it, so themes that are not worth shipping inside every firmware (colour variants, community themes) can still
be loaded and unloaded whenever you like.

```
themes/
  catalog.json          list of every pack, with versions (the clock reads this first)
  packs/<id>-r<rev>.zip one ZIP per theme; the revision is part of the name, so a given file never changes
  previews/<id>.jpg     strip of the theme's digits 0-9, shown in the Theme Manager (built by the packer from the pack's Numbers/0-9)
  previews/full/<id>.jpg  the same ten digits at their real tube size (80 x 160), two rows of five; shown when the thumbnail is clicked
```

## Versions

| Field | Meaning |
|---|---|
| `pack_version`, `updated` (top level) | Raised by one / set to today by `stamp`, and only when the catalog's theme data differs from the fingerprint (`data_sha256`) saved at the last stamp. The Theme Manager shows `updated` as "catalog updated <date>". |
| `rev` (per theme) | Revision of that theme's content, starting at 1. Goes up only when the theme's files really change. |
| `content_sha256` | Fingerprint of the theme's files, independent of how they are zipped. Same files, same fingerprint, no new revision. |
| `sha256` / `size` | Fingerprint and size of the ZIP itself; the clock checks both before it extracts anything. |
| `min_fw` | Oldest firmware that can use the pack (optional). |
| `bundled` | `true` when the same theme also ships inside the firmware. Use `--bundled` with `add` on the theme's folder under `data/images/themes/`: the packer then also writes the `.pack.json` into that folder, so the built-in copy knows its revision and the Theme Manager can offer an update when the catalog has a newer one. |
| `notes` | Short "what changed" text shown next to an update. |

Each installed theme keeps a small `.pack.json` inside its folder on the clock (`id`, `rev`, `content_sha256`). The clock compares
it with the catalog: a higher `rev` in the catalog means **Update available**.

## Adding or updating a theme

From the repository root:

```bash
python helpers/theme_pack/pack_theme.py add <theme folder> --id Nixie --name "Nixie" --family Nixie --variant Orange --notes "What changed"
python helpers/theme_pack/pack_theme.py check
```

Run `stamp` once after a batch of changes (`python helpers/theme_pack/pack_theme.py stamp`): the version and date move only if a theme was added, removed or changed, and `check` fails until you do. `add` builds the ZIP (files stored uncompressed under `images/themes/<id>/`, which is what the clock's extractor reads), builds the preview strip from the theme's `Numbers/0-9` images (needs `pip install pillow`), works out
the revision itself, deletes the superseded ZIP and updates `catalog.json`. Running it again on an
unchanged folder does nothing. `check` verifies that every ZIP matches its catalog entry (size, hash, contents) and that no stray
ZIP is left behind; run it before committing. `remove <id>` takes a theme out of the catalog.

GitHub runs the same `check` on every push or pull request that touches `themes/`, `helpers/theme_pack/` or `data/images/themes/` (workflow *Check theme packs*), and it also verifies that a `--bundled` theme still matches its copy in `data/images/themes/`. A themes-only change does not start the firmware build.

Keep each pack under 1 MB and `catalog.json` under 64 KB, about 110 themes (the clock refuses bigger downloads, and `check` fails above 64 KB). The clock saves a local copy of the preview for the first 64 themes; any further ones still work, their thumbnails just load from the repository. Commit the ZIP, the preview and `catalog.json` together. The Theme Manager adds `?v=<rev>` to the preview address, so a changed preview shows up as soon as the revision goes up.

## Folder names

`id` becomes the theme's folder name on the clock, so it may only use letters, digits, `_` and `-`, up to 31 characters. Use `name` for what people see
in the list, and `family` / `variant` to group variants of the same design. The entry with an empty `variant` is the family's own (base) theme and is shown as the parent row; the others are listed indented under it as *Variants of <family>*, labelled with their `variant` text (for example family `Nixie`, base `Nixie`, variants `Wire` and `Cage`). A family with a single theme is just a normal row. Set `category` to `legacy` (`add ... --category legacy`) to move a theme out of the main list into a **Legacy** section at the bottom, for older designs that are kept available but no longer promoted (for example `NixieOY`).
