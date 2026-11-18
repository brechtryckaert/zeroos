#!/bin/bash
# Try Music, Video, Camera and Settings on a desktop.
#
#   ./tools/sim/media-demo.sh            # SDL window
#   ./tools/sim/media-demo.sh --shots    # headless screenshots into screenshots/media/
#
# Generates test media (tagged songs with cover art, an MKV with two audio
# and two subtitle tracks, an MP4 with an .srt next to it, photos) and runs
# ZeroOS with stand-ins for rpicam (camera), nmcli and amixer, a fake
# backlight and battery, and a simulated 1280x720 HDMI monitor. Needs ffmpeg,
# mpv and Pillow.
set -euo pipefail
cd "$(dirname "$0")/../.."
for t in ffmpeg mpv; do command -v $t >/dev/null || { echo "needs $t"; exit 1; }; done
python3 -c "import PIL" 2>/dev/null || { echo "needs Pillow: pip install pillow"; exit 1; }

W=$(mktemp -d)
trap 'pkill -P $$ mpv 2>/dev/null; rm -rf "$W"' EXIT
M=$W/media
mkdir -p "$M"/{Music,Videos/Series,Pictures,art} "$W/state" "$W/fake/bl/panel" "$W/fake/ps/BAT0" "$W/fake/nm"
echo 255 > "$W/fake/bl/panel/max_brightness"; echo 180 > "$W/fake/bl/panel/brightness"
echo 78 > "$W/fake/ps/BAT0/capacity"; echo Discharging > "$W/fake/ps/BAT0/status"
echo "generating test media..."
python3 - "$M" <<'PY'
import sys
from PIL import Image, ImageDraw
M = sys.argv[1]
for name, c1, c2 in [("north", (229,72,77), (40,20,60)), ("tide", (18,165,148), (10,30,60)), ("neon", (142,78,198), (250,200,40))]:
    im = Image.new("RGB", (600, 600), c2); d = ImageDraw.Draw(im)
    for i in range(0, 600, 40): d.ellipse((300-i/1.2, 300-i/1.2, 300+i/1.2, 300+i/1.2), outline=c1, width=6)
    d.rectangle((40, 470, 560, 560), fill=c1); im.save(f"{M}/art/{name}.jpg", quality=88)
for i in range(3):
    im = Image.new("RGB", (1640, 1232), (30+i*40, 90, 140-i*20)); d = ImageDraw.Draw(im)
    d.ellipse((400, 200, 1200, 1000), fill=(240, 200-i*30, 80)); im.save(f"{M}/Pictures/IMG_2026111{i}_120000.jpg", quality=85)
PY
song() {   # file artist album title track [cover]
    local args=(-f lavfi -i "sine=frequency=$((300 + $5 * 60)):duration=25")
    if [ -n "${6:-}" ]; then args+=(-i "$6" -map 0:a -map 1:v -c:v mjpeg -disposition:v attached_pic -id3v2_version 3); fi
    ffmpeg -loglevel error -y "${args[@]}" -metadata artist="$2" -metadata album="$3" -metadata title="$4" \
        -metadata track="$5" -metadata date=2024 "$1"
}
mkdir -p "$M/Music/Aurora Lines/Northbound" "$M/Music/Aurora Lines/Low Tide" "$M/Music/Kid Neon/Static Bloom"
song "$M/Music/Aurora Lines/Northbound/01 Ice Road.mp3" "Aurora Lines" Northbound "Ice Road" 1 "$M/art/north.jpg"
song "$M/Music/Aurora Lines/Northbound/02 Polar Night.mp3" "Aurora Lines" Northbound "Polar Night" 2 "$M/art/north.jpg"
song "$M/Music/Aurora Lines/Low Tide/01 Undertow.flac" "Aurora Lines" "Low Tide" Undertow 1
cp "$M/art/tide.jpg" "$M/Music/Aurora Lines/Low Tide/cover.jpg"
song "$M/Music/Kid Neon/Static Bloom/01 Voltage.m4a" "Kid Neon" "Static Bloom" Voltage 1 "$M/art/neon.jpg"
song "$M/Music/Kid Neon/Static Bloom/02 Afterglow.ogg" "Kid Neon" "Static Bloom" Afterglow 2
printf '1\n00:00:01,000 --> 00:00:50,000\nHello from the English subtitles.\n' > "$W/en.srt"
printf '1\n00:00:01,000 --> 00:00:50,000\nHallo vanuit de Nederlandse ondertitels.\n' > "$W/nl.srt"
ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=1280x720:rate=25:duration=60 \
    -f lavfi -i "sine=frequency=440:duration=60" -f lavfi -i "sine=frequency=660:duration=60" -i "$W/en.srt" -i "$W/nl.srt" \
    -map 0:v -map 1:a -map 2:a -map 3 -map 4 -c:v libx264 -preset ultrafast -c:a aac -c:s srt \
    -metadata:s:a:0 language=eng -metadata:s:a:0 title="English 2.0" -metadata:s:a:1 language=nld -metadata:s:a:1 title=Nederlands \
    -metadata:s:s:0 language=eng -metadata:s:s:0 title=English -metadata:s:s:1 language=nld -metadata:s:s:1 title=Nederlands \
    "$M/Videos/Big Test Movie (2026).mkv"
ffmpeg -loglevel error -y -f lavfi -i testsrc=size=640x360:rate=25:duration=30 -f lavfi -i "sine=frequency=330:duration=30" \
    -c:v libx264 -preset ultrafast -c:a aac "$M/Videos/Series/Episode 01.mp4"
cp "$W/en.srt" "$M/Videos/Series/Episode 01.srt"

export PATH="$PWD/tools/sim/fake-rpicam:$PWD/tools/sim/fake-system:$PATH"
export ZOS_MUSIC=$M/Music ZOS_VIDEOS=$M/Videos ZOS_PICTURES=$M/Pictures
export ZOS_FAKE_STATE=$W/fake/nm ZOS_BACKLIGHT=$W/fake/bl/panel ZOS_POWER_SUPPLY=$W/fake/ps
export ZOS_FAKE_HDMI=${ZOS_FAKE_HDMI:-1280x720}

if [ "${1:-}" = "--shots" ]; then
    OUT=screenshots/media
    mkdir -p "$OUT"
    export ZOS_MPV_ARGS="--ao=null --vo=null"
    ./build/zeroos --books assets/books --state "$W/state" --demo-status --script "\
app:music;wait:1500;shot:$OUT/01-music-home.ppm;key:down*2;key:enter;wait:600;shot:$OUT/02-music-albums.ppm;\
key:down;key:enter;wait:300;key:enter;wait:2000;shot:$OUT/03-music-playing.ppm;\
app:video;wait:400;shot:$OUT/04-video-browse.ppm;key:down;key:enter;wait:2500;shot:$OUT/05-video-remote.ppm;\
key:s;wait:300;shot:$OUT/06-video-subtitles.ppm;key:esc;key:esc;wait:800;\
app:camera;wait:2000;shot:$OUT/07-camera.ppm;key:g;wait:1500;shot:$OUT/08-gallery.ppm;key:esc;wait:200;\
app:settings;wait:1500;shot:$OUT/09-settings.ppm;key:down;key:enter;wait:300;shot:$OUT/10-settings-display.ppm;\
key:esc;key:esc;wait:300;hdmishot:$OUT/11-hdmi-mirror.ppm"
    python3 - "$OUT" <<'PY'
import glob, os, sys
from PIL import Image
for f in glob.glob(os.path.join(sys.argv[1], "*.ppm")):
    Image.open(f).save(f[:-4] + ".png"); os.remove(f)
PY
    echo "screenshots in $OUT/"
else
    ./build/zeroos --books assets/books --state "$W/state"
fi
