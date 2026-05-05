"""
make_hotpatch.py — generate the Nextube hotpatch ZIP for a CI build.

Always includes the complete data/web/ tree (index.html + version.txt).
Additionally includes any paths listed in hotpatch_extras.txt at the repo root.

Environment variables (set by the workflow before calling this script):
  BUILD_VER   — version string used in the output filename, e.g. "1.2.0"

The ZIP uses ZIP_STORED (no compression) — required by the firmware's
minimal ZIP parser.
"""

import os
import sys
import zipfile

build_ver = os.environ.get('BUILD_VER')
if not build_ver:
    print('ERROR: BUILD_VER environment variable is not set', file=sys.stderr)
    sys.exit(1)

zip_path = f'artifacts/nextube-hotpatch-v{build_ver}.zip'

to_include = set()

# ── 1. Always include the complete data/web/ tree ────────────────────────────
# data/web/index.html  — committed to the repo.
# data/web/version.txt — not committed; written by the workflow before this
#                        script runs (and also by CMake during idf.py build).
#                        os.walk is a filesystem scan so it picks it up from
#                        disk regardless of whether it is tracked in git.
for root, _dirs, files in os.walk('data/web'):
    for fn in files:
        abs_path = os.path.join(root, fn)
        rel = os.path.relpath(abs_path, 'data').replace(os.sep, '/')
        to_include.add(rel)

# ── 2. Extra assets declared in hotpatch_extras.txt ─────────────────────────
# Add one path per line (relative to data/), e.g.:
#   images/themes/NixieOY/Numbers/5.jpg
#   audio/bell.mp3
# Lines starting with '#' and blank lines are ignored.
# config.json is always excluded even if listed.
# Clear this file after a full LittleFS flash release.
extras_file = 'hotpatch_extras.txt'
if os.path.isfile(extras_file):
    with open(extras_file) as f:
        for line in f:
            rel = line.strip()
            if not rel or rel.startswith('#'):
                continue
            if rel == 'config.json':
                print(f'  [skip] {rel} — config is never distributed')
                continue
            full = os.path.join('data', rel)
            if os.path.isfile(full):
                to_include.add(rel)
            else:
                print(f'  [warn] hotpatch_extras.txt: {rel} not found on disk, skipping')

to_include = sorted(to_include)

print(f'Hot patch contents ({len(to_include)} files):')
for f in to_include:
    print(f'  {f}')

os.makedirs('artifacts', exist_ok=True)
with zipfile.ZipFile(zip_path, 'w', compression=zipfile.ZIP_STORED) as zf:
    for rel in to_include:
        zf.write(os.path.join('data', rel), rel)

sz = os.path.getsize(zip_path)
print(f'Hot patch ZIP written: {zip_path} ({sz:,} bytes)')
