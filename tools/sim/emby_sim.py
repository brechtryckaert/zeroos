#!/usr/bin/env python3
"""A small Emby server stand-in for the ZeroOS simulator.

Implements the endpoints ZeroOS uses, with Emby's JSON field names:
  POST /Users/AuthenticateByName            (user "demo", password "demo")
  GET  /System/Info/Public
  GET  /Users/{uid}/Views, /Users/{uid}/Items, /Users/{uid}/Items/Resume
  GET  /Items/{id}/Images/Primary           (generated posters, baseline JPEG)
  GET  /Videos/{id}/master.m3u8, /Videos/{id}/stream   (real media made with ffmpeg)
  GET  /Audio/{id}/universal
  POST /Sessions/Playing, /Sessions/Playing/Progress, /Sessions/Playing/Stopped
  DELETE /Videos/ActiveEncodings
Playback reports update resume positions, so "Continue watching" behaves.

    python3 tools/sim/emby_sim.py [--port 8096]    (needs ffmpeg and Pillow)
"""
import argparse
import hashlib
import io
import json
import os
import re
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

from PIL import Image, ImageDraw, ImageFont

TICKS = 10_000_000
USER_ID = "4f8a2c1e9b7d4e3f8a6b5c4d3e2f1a0b"
TOKEN = "e9b1d77f2c0a4c51a8b0f6e3d2c1b0a9"
SERVER_ID = "0b9c8d7e6f5a4b3c2d1e0f9a8b7c6d5e"
MEDIA_SECONDS = 40

ITEMS = {}


def add(item):
    ITEMS[item["Id"]] = item
    item.setdefault("UserData", {"PlaybackPositionTicks": 0, "Played": False, "PlayedPercentage": 0})
    item.setdefault("ImageTags", {"Primary": hashlib.md5(item["Id"].encode()).hexdigest()[:12]})
    return item


def build_library():
    add({"Id": "v_movies", "Name": "Movies", "Type": "CollectionFolder", "CollectionType": "movies", "IsFolder": True})
    add({"Id": "v_shows", "Name": "TV Shows", "Type": "CollectionFolder", "CollectionType": "tvshows", "IsFolder": True})
    add({"Id": "v_music", "Name": "Music", "Type": "CollectionFolder", "CollectionType": "music", "IsFolder": True})
    movies = [
        ("m1", "Lanterns Over the Lowlands", 2023, "A lighthouse keeper's daughter maps the storms that swallowed her father's boat, one radio log at a time."),
        ("m2", "The Copper Orchard", 2019, "Two estranged brothers inherit a failing cider farm and one very stubborn mule."),
        ("m3", "Night Shift at Platform 9", 2021, "A railway signal operator receives messages from a train that was retired forty years ago."),
        ("m4", "Small Engines", 2024, "A documentary about the people who keep old mopeds running in a city that has moved on."),
    ]
    for mid, name, year, ov in movies:
        add({"Id": mid, "Name": name, "Type": "Movie", "IsFolder": False, "MediaType": "Video",
             "ProductionYear": year, "RunTimeTicks": MEDIA_SECONDS * TICKS, "Overview": ov, "ParentId": "v_movies",
             "OfficialRating": "PG"})
    add({"Id": "s1", "Name": "Harbour Lights", "Type": "Series", "IsFolder": True, "ProductionYear": 2022,
         "Overview": "Life, weather and gossip in a small fishing port.", "ParentId": "v_shows", "ChildCount": 1})
    add({"Id": "s1_1", "Name": "Season 1", "Type": "Season", "IsFolder": True, "IndexNumber": 1,
         "SeriesName": "Harbour Lights", "ParentId": "s1", "ChildCount": 4})
    for i, ep in enumerate(["The Fog Bell", "Low Tide", "Ferry Strike", "Lights Out"], 1):
        add({"Id": f"e{i}", "Name": ep, "Type": "Episode", "IsFolder": False, "MediaType": "Video",
             "SeriesName": "Harbour Lights", "ParentIndexNumber": 1, "IndexNumber": i,
             "RunTimeTicks": MEDIA_SECONDS * TICKS, "ParentId": "s1_1",
             "Overview": f"Episode {i} of the first season."})
    ITEMS["e2"]["UserData"] = {"PlaybackPositionTicks": 14 * TICKS, "Played": False, "PlayedPercentage": 35}
    ITEMS["e1"]["UserData"] = {"PlaybackPositionTicks": 0, "Played": True, "PlayedPercentage": 100}
    add({"Id": "a1", "Name": "Night Ferry", "Type": "MusicAlbum", "IsFolder": True, "AlbumArtist": "The Tidelines",
         "ProductionYear": 2020, "ParentId": "v_music", "ChildCount": 3})
    for i, t in enumerate(["Departure Hall", "Deck Lights", "Arrival, 6 a.m."], 1):
        add({"Id": f"t{i}", "Name": t, "Type": "Audio", "IsFolder": False, "MediaType": "Audio",
             "Album": "Night Ferry", "AlbumArtist": "The Tidelines", "Artists": ["The Tidelines"],
             "IndexNumber": i, "RunTimeTicks": 25 * TICKS, "ParentId": "a1"})


class Media:
    def __init__(self):
        self.dir = tempfile.mkdtemp(prefix="emby-sim-")
        v = os.path.join(self.dir, "video.mp4")
        subprocess.run(["ffmpeg", "-loglevel", "error", "-f", "lavfi", "-i",
                        f"testsrc2=size=640x360:rate=25:duration={MEDIA_SECONDS}", "-f", "lavfi", "-i",
                        f"sine=frequency=440:duration={MEDIA_SECONDS}", "-c:v", "libx264", "-preset", "veryfast",
                        "-g", "50", "-c:a", "aac", "-shortest", v], check=True)
        hls = os.path.join(self.dir, "hls")
        os.makedirs(hls)
        subprocess.run(["ffmpeg", "-loglevel", "error", "-i", v, "-c", "copy", "-f", "hls", "-hls_time", "4",
                        "-hls_playlist_type", "vod", "-hls_segment_filename", os.path.join(hls, "seg%03d.ts"),
                        os.path.join(hls, "main.m3u8")], check=True)
        subprocess.run(["ffmpeg", "-loglevel", "error", "-f", "lavfi", "-i", "sine=frequency=330:duration=25",
                        "-c:a", "libmp3lame", "-b:a", "96k", os.path.join(self.dir, "audio.mp3")], check=True)


def poster(item, max_w, max_h):
    w, h = (300, 300) if item["Type"] in ("MusicAlbum", "Audio", "CollectionFolder") else (200, 300)
    seed = int(hashlib.md5(item["Id"].encode()).hexdigest()[:6], 16)
    c1 = ((seed >> 16) & 0xFF, (seed >> 8) & 0xFF, seed & 0xFF)
    c2 = tuple(max(0, x - 110) for x in c1)
    img = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(img)
    for y in range(h):
        t = y / h
        d.line([(0, y), (w, y)], fill=tuple(int(a * (1 - t) + b * t) for a, b in zip(c1, c2)))
    try:
        font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 30)
    except OSError:
        font = ImageFont.load_default()
    words, lines, cur = item["Name"].split(), [], ""
    for wd in words:
        if len(cur) + len(wd) > 10:
            lines.append(cur)
            cur = wd
        else:
            cur = (cur + " " + wd).strip()
    lines.append(cur)
    y = h - 40 * len(lines) - 20
    for ln in lines:
        d.text((16, y), ln, fill=(255, 255, 255), font=font)
        y += 40
    img.thumbnail((max_w or w, max_h or h))
    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=85, progressive=False)
    return buf.getvalue()


def view(item):
    out = {k: v for k, v in item.items() if k not in ("ParentId",)}
    out["ServerId"] = SERVER_ID
    if item["IsFolder"]:
        out["ChildCount"] = item.get("ChildCount", sum(1 for i in ITEMS.values() if i.get("ParentId") == item["Id"]))
    return out


def make_handler(media, verbose):
    events = []

    class H(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *a):
            if verbose:
                print("[emby-sim]", self.command, self.path.split("?")[0], flush=True)

        def send(self, code, body=b"", ctype="application/json", extra=None):
            if isinstance(body, (dict, list)):
                body = json.dumps(body).encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            for k, v in (extra or {}).items():
                self.send_header(k, v)
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)

        def authed(self, q):
            tok = self.headers.get("X-Emby-Token") or q.get("api_key", [""])[0] or q.get("ApiKey", [""])[0]
            if tok != TOKEN:
                self.send(401, b"Access token is invalid or expired.", "text/plain")
                return False
            return True

        def body(self):
            n = int(self.headers.get("Content-Length") or 0)
            return json.loads(self.rfile.read(n) or b"{}")

        def route(self):
            u = urlparse(self.path)
            path = re.sub(r"^/emby", "", u.path)
            q = parse_qs(u.query)
            return path, q

        def do_HEAD(self):
            self.do_GET()

        def do_GET(self):
            path, q = self.route()
            if path == "/System/Info/Public":
                return self.send(200, {"ServerName": "Basement Emby (sim)", "Version": "4.9.0.0", "Id": SERVER_ID,
                                       "LocalAddress": "http://127.0.0.1:8096"})
            if path.startswith("/Videos/hls/"):     # HLS media playlist + segments (sub-requests)
                f = os.path.join(media.dir, "hls", os.path.basename(path))
                if not os.path.exists(f):
                    return self.send(404)
                ctype = "application/vnd.apple.mpegurl" if f.endswith(".m3u8") else "video/mp2t"
                return self.send(200, open(f, "rb").read(), ctype)
            if not self.authed(q):
                return
            m = re.match(r"^/Users/(\w+)/Views$", path)
            if m:
                views = [view(ITEMS[i]) for i in ("v_movies", "v_shows", "v_music")]
                return self.send(200, {"Items": views, "TotalRecordCount": len(views)})
            m = re.match(r"^/Users/(\w+)/Items/Resume$", path)
            if m:
                res = [view(i) for i in ITEMS.values()
                       if not i["IsFolder"] and i["UserData"]["PlaybackPositionTicks"] > 0 and not i["UserData"]["Played"]]
                return self.send(200, {"Items": res, "TotalRecordCount": len(res)})
            m = re.match(r"^/Users/(\w+)/Items/(\w+)$", path)
            if m and m.group(2) in ITEMS:
                return self.send(200, view(ITEMS[m.group(2)]))
            m = re.match(r"^/Users/(\w+)/Items$", path)
            if m:
                parent = q.get("ParentId", q.get("parentId", [""]))[0]
                kids = [i for i in ITEMS.values() if i.get("ParentId") == parent]
                kids.sort(key=lambda i: (i.get("ParentIndexNumber") or 0, i.get("IndexNumber") or 0, i["Name"]))
                start = int(q.get("StartIndex", ["0"])[0])
                limit = int(q.get("Limit", ["1000"])[0])
                page = [view(i) for i in kids[start:start + limit]]
                return self.send(200, {"Items": page, "TotalRecordCount": len(kids)})
            m = re.match(r"^/Items/(\w+)/Images/Primary$", path)
            if m and m.group(1) in ITEMS:
                mw = int(q.get("MaxWidth", q.get("maxWidth", ["0"]))[0])
                mh = int(q.get("MaxHeight", q.get("maxHeight", ["0"]))[0])
                return self.send(200, poster(ITEMS[m.group(1)], mw, mh), "image/jpeg",
                                 {"Cache-Control": "public, max-age=31536000"})
            m = re.match(r"^/Videos/(\w+)/master\.m3u8$", path)
            if m and m.group(1) in ITEMS:
                events.append(("hls", m.group(1), dict((k, v[0]) for k, v in q.items())))
                master = ("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1200000,RESOLUTION=640x360,"
                          'CODECS="avc1.64001e,mp4a.40.2"\n/Videos/hls/main.m3u8\n')
                return self.send(200, master.encode(), "application/vnd.apple.mpegurl")
            m = re.match(r"^/Videos/(\w+)/stream(\.\w+)?$", path)
            if m and m.group(1) in ITEMS:
                return self.send(200, open(os.path.join(media.dir, "video.mp4"), "rb").read(), "video/mp4")
            m = re.match(r"^/Audio/(\w+)/(universal|stream(\.\w+)?)$", path)
            if m and m.group(1) in ITEMS:
                return self.send(200, open(os.path.join(media.dir, "audio.mp3"), "rb").read(), "audio/mpeg")
            self.send(404, b"not found", "text/plain")

        def do_POST(self):
            path, q = self.route()
            if path == "/Users/AuthenticateByName":
                auth = self.headers.get("X-Emby-Authorization") or self.headers.get("Authorization") or ""
                if 'Client="' not in auth or 'DeviceId="' not in auth:
                    return self.send(400, b"missing X-Emby-Authorization", "text/plain")
                b = self.body()
                if b.get("Username", "").lower() != "demo" or b.get("Pw") != "demo":
                    return self.send(401, b"Invalid username or password entered.", "text/plain")
                return self.send(200, {"User": {"Id": USER_ID, "Name": "demo", "ServerId": SERVER_ID},
                                       "AccessToken": TOKEN, "ServerId": SERVER_ID,
                                       "SessionInfo": {"Client": "ZeroOS", "DeviceName": "CardputerZero"}})
            if not self.authed(q):
                return
            if path.startswith("/Sessions/Playing"):
                b = self.body()
                kind = path.rsplit("/", 1)[-1]
                item = ITEMS.get(b.get("ItemId", ""))
                pos = int(b.get("PositionTicks") or 0)
                print(f"[emby-sim] {kind}: item={b.get('ItemId')} pos={pos / TICKS:.1f}s "
                      f"paused={b.get('IsPaused')} method={b.get('PlayMethod')} event={b.get('EventName', '-')}", flush=True)
                if item:
                    ud = item["UserData"]
                    rt = item.get("RunTimeTicks") or 1
                    if kind == "Stopped":
                        if pos >= rt * 0.9:
                            ud.update(PlaybackPositionTicks=0, Played=True, PlayedPercentage=100)
                        else:
                            ud.update(PlaybackPositionTicks=pos, PlayedPercentage=100 * pos / rt)
                return self.send(204)
            self.send(404, b"not found", "text/plain")

        def do_DELETE(self):
            path, q = self.route()
            if not self.authed(q):
                return
            if path == "/Videos/ActiveEncodings":
                print(f"[emby-sim] ActiveEncodings stopped for device {q.get('DeviceId', ['?'])[0]}", flush=True)
                return self.send(204)
            self.send(404)

    return H


class Server(ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address):
        pass   # mpv drops HLS connections on stop; that's normal


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8096)
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()
    build_library()
    media = Media()
    srv = Server(("127.0.0.1", a.port), make_handler(media, a.verbose))
    print(f"[emby-sim] Emby stand-in on http://127.0.0.1:{a.port}  (user demo / password demo)", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
