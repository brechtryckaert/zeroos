#!/bin/sh
# Drive the shell headlessly and save PNG screenshots to ./screenshots.
# Handy for checking UI changes without a window or the device.
set -e
BIN=${BIN:-./build/zeroos}
OUT=${OUT:-screenshots}
STATE=$(mktemp -d)
mkdir -p "$OUT"

"$BIN" --books assets/books --state "$STATE" --demo-status --script "\
shot:$OUT/01-launcher.ppm;\
key:enter;shot:$OUT/02-library.ppm;\
key:enter;wait:500;shot:$OUT/03-page.ppm;\
key:right*7;wait:1500;shot:$OUT/04-page-later.ppm;\
key:c;shot:$OUT/05-contents.ppm;key:esc;\
key:t;key:t;wait:1500;shot:$OUT/06-night.ppm;\
key:esc;app:music;shot:$OUT/07-stub-app.ppm"

python3 - "$OUT" <<'EOF'
import glob, os, sys
from PIL import Image
for f in glob.glob(os.path.join(sys.argv[1], "*.ppm")):
    Image.open(f).save(f[:-4] + ".png")
    os.remove(f)
EOF
rm -rf "$STATE"
echo "screenshots in $OUT/"
