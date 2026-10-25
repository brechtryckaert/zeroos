#!/bin/bash
# Try the Meshtastic and MeshCore apps on a desktop, no radio needed.
#
#   ./tools/sim/mesh-demo.sh            # SDL window
#   ./tools/sim/mesh-demo.sh --shots    # headless screenshots into screenshots/mesh/
#
# Starts both simulators (a meshtasticd stand-in on :4403, and three real
# pyMC MeshCore nodes on a simulated radio with a companion on :5000), then
# ZeroOS. The first run creates a Python venv in tools/sim/.venv.
set -euo pipefail
cd "$(dirname "$0")/../.."
VENV=tools/sim/.venv
if [ ! -x "$VENV/bin/python" ]; then
    python3 -m venv "$VENV"
    "$VENV/bin/pip" install -q meshtastic pymc-core
fi
STATE=$(mktemp -d)
"$VENV/bin/python" tools/sim/meshtastic_sim.py --quiet & MT=$!
"$VENV/bin/python" tools/sim/meshcore_sim.py --quiet & MC=$!
trap 'kill $MT $MC 2>/dev/null; rm -rf "$STATE"' EXIT
sleep 6   # let the MeshCore nodes exchange adverts

if [ "${1:-}" = "--shots" ]; then
    OUT=screenshots/mesh
    mkdir -p "$OUT"
    ./build/zeroos --books assets/books --state "$STATE" --demo-status --script "\
key:right*3;shot:$OUT/01-launcher.ppm;app:meshtastic;wait:6000;shot:$OUT/02-meshtastic-chats.ppm;\
key:enter;wait:300;type:Morning! Testing from the Cardputer;key:enter;wait:2500;shot:$OUT/03-meshtastic-channel.ppm;\
key:esc;key:down*2;key:enter;type:Yes it is;key:enter;wait:4000;shot:$OUT/04-meshtastic-dm.ppm;\
key:esc;key:n;wait:200;shot:$OUT/05-meshtastic-nodes.ppm;key:esc;\
app:meshcore;wait:300;shot:$OUT/06-switch-radio.ppm;key:enter;wait:5000;shot:$OUT/07-meshcore-chats.ppm;\
key:down;key:enter;type:Yes, running MeshCore over pyMC;key:enter;wait:5000;shot:$OUT/08-meshcore-dm.ppm;\
key:esc;key:esc;wait:300;shot:$OUT/09-launcher-badges.ppm"
    "$VENV/bin/python" - "$OUT" <<'EOF'
import glob, os, sys
from PIL import Image
for f in glob.glob(os.path.join(sys.argv[1], "*.ppm")):
    Image.open(f).save(f[:-4] + ".png"); os.remove(f)
EOF
    echo "screenshots in $OUT/"
else
    ./build/zeroos --books assets/books --state "$STATE"
fi
