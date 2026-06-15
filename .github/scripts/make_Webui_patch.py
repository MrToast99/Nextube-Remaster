"""
make_hotpatch.py — generate the Nextube Web UI update ZIP for a CI build.

Always packages the complete data/web/ tree so the update is safe to apply
from any firmware version, including when releases are skipped.

Additionally includes any paths listed in hotpatch_extras.txt at the repo root.

Environment variables (set by the workflow before calling this script):
  BUILD_VER   — version string used in the output filename, e.g. "1.2.0"
"""

import gzip
import os
import sys
import zipfile

# index.html is shipped gzip-compressed only.  serve_static() on the device
# serves "<path>.gz" verbatim with Content-Encoding: gzip (the browser inflates
# it), so the ~308 KB shell costs ~73 KB of flash and ~¼ the per-request CPU.
# The plain file is excluded from the zip so it never lands on LittleFS.
GZIP_ONLY = {'web/index.html'}

build_ver = os.environ.get('BUILD_VER')
if not build_ver:
    print('ERROR: BUILD_VER environment variable is not set', file=sys.stderr)
    sys.exit(1)

zip_path = f'artifacts/nextube-WebUI-v{build_ver}.zip'

to_include = set()

# Walk the complete data/web/ tree
for root, _dirs, files in os.walk('data/web'):
    for fn in files:
        abs_path = os.path.join(root, fn)
        rel = os.path.relpath(abs_path, 'data').replace(os.sep, '/')
        to_include.add(rel)

# Extra assets declared in hotpatch_extras.txt
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

print(f'Web UI ZIP contents ({len(to_include)} files, full tree):')
for f in to_include:
    print(f'  {f}')

os.makedirs('artifacts', exist_ok=True)
with zipfile.ZipFile(zip_path, 'w', compression=zipfile.ZIP_STORED) as zf:
    for rel in to_include:
        src = os.path.join('data', rel)
        if rel in GZIP_ONLY:
            # Store the gzip bytes under "<rel>.gz".  The zip itself stays
            # STORE-method (the device unzip only handles stored entries); the
            # gzip payload is already-compressed bytes written verbatim.
            with open(src, 'rb') as fh:
                raw = fh.read()
            blob = gzip.compress(raw, 9)
            zf.writestr(rel + '.gz', blob)
            print(f'  gzip   {rel} -> {rel}.gz  ({len(raw):,} -> {len(blob):,} B)')
        else:
            zf.write(src, rel)

sz = os.path.getsize(zip_path)
print(f'Web UI ZIP written: {zip_path} ({sz:,} bytes)')
