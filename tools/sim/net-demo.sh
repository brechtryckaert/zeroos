#!/bin/bash
# Try the Tailscale, SSH and Emby apps on a desktop.
#
#   ./tools/sim/net-demo.sh            # SDL window (Emby video opens an mpv window)
#   ./tools/sim/net-demo.sh --shots    # headless screenshots into screenshots/net/
#
# Starts a tailscaled LocalAPI stand-in and an Emby stand-in (sign in with
# demo / demo at 127.0.0.1:8096). SSH needs no stand-in: try any host you
# can reach, e.g. localhost.
set -euo pipefail
cd "$(dirname "$0")/../.."
command -v ffmpeg >/dev/null || { echo "needs ffmpeg (for the Emby test media)"; exit 1; }
python3 -c "import PIL" 2>/dev/null || { echo "needs Pillow: pip install pillow"; exit 1; }

STATE=$(mktemp -d)
SOCK="$STATE/tailscaled.sock"
python3 tools/sim/tailscale_sim.py --socket "$SOCK" --state "${TS_STATE:-needs-login}" > "$STATE/ts.log" 2>&1 & TS=$!
python3 tools/sim/emby_sim.py --port 8096 > "$STATE/emby.log" 2>&1 & EM=$!
trap 'kill $TS $EM 2>/dev/null; rm -rf "$STATE"' EXIT
for _ in $(seq 1 60); do curl -s -o /dev/null http://127.0.0.1:8096/System/Info/Public && break; sleep 0.5; done
export ZOS_TAILSCALE_SOCKET="$SOCK"

if [ "${1:-}" = "--shots" ]; then
    OUT=screenshots/net
    mkdir -p "$OUT"
    ZOS_MPV_ARGS="--vo=null --ao=null" ./build/zeroos --books assets/books --state "$STATE" --demo-status --script "\
key:right*5;shot:$OUT/01-launcher.ppm;app:tailscale;wait:800;key:enter;wait:1500;shot:$OUT/02-tailscale-qr.ppm;\
wait:7000;shot:$OUT/03-tailscale-connected.ppm;key:e;wait:1200;shot:$OUT/04-tailscale-exit-node.ppm;\
key:down*2;key:enter;wait:800;shot:$OUT/05-ssh-from-tailnet.ppm;key:esc;wait:300;shot:$OUT/06-ssh-hosts.ppm;\
app:emby;wait:300;type:127.0.0.1;key:enter;type:demo;key:enter;type:demo;key:enter;\
wait:2500;shot:$OUT/07-emby-home.ppm;key:down;key:enter;wait:2500;key:down;wait:1500;shot:$OUT/08-emby-movies.ppm;\
key:esc;key:up;key:enter;wait:1500;shot:$OUT/09-emby-detail.ppm;key:enter;wait:3500;shot:$OUT/10-emby-playing.ppm;\
key:esc;wait:1000"
    python3 - "$OUT" <<'EOF'
import glob, os, sys
from PIL import Image
for f in glob.glob(os.path.join(sys.argv[1], "*.ppm")):
    Image.open(f).save(f[:-4] + ".png"); os.remove(f)
EOF
    echo "screenshots in $OUT/"
else
    ./build/zeroos --books assets/books --state "$STATE"
fi
