#!/usr/bin/env python3
"""Draw the launcher icons and emit them as LVGL A8 (alpha-only) images.

The icons are drawn at 8x size with Pillow and downsampled, which gives clean
anti-aliased edges at 32x32. A8 images are recoloured by LVGL at draw time, so
one mask works on any tile colour.

Usage: python3 tools/make_icons.py src/assets/icons.c [preview.png]
"""
import math
import sys

from PIL import Image, ImageDraw

SIZE = 32
SS = 8                      # supersampling factor
S = SIZE * SS


def canvas():
    img = Image.new("L", (S, S), 0)
    return img, ImageDraw.Draw(img)


def u(v):
    """Icon units (0..32) to supersampled pixels."""
    return int(round(v * SS))


def box(x0, y0, x1, y1):
    return [u(x0), u(y0), u(x1), u(y1)]


def music():
    img, d = canvas()
    # two note heads
    d.ellipse(box(4, 21, 13, 29), fill=255)
    d.ellipse(box(18, 18, 27, 26), fill=255)
    # stems
    d.rectangle(box(10.5, 6, 13, 25), fill=255)
    d.rectangle(box(24.5, 3, 27, 22), fill=255)
    # beam (parallelogram)
    d.polygon([(u(10.5), u(6)), (u(27), u(2)), (u(27), u(7.5)), (u(10.5), u(11.5))], fill=255)
    return img


def video():
    img, d = canvas()
    d.rounded_rectangle(box(2, 6, 30, 26), radius=u(4), fill=255)
    # punch out a play triangle
    d.polygon([(u(13), u(11)), (u(13), u(21)), (u(21.5), u(16))], fill=0)
    return img


def book():
    img, d = canvas()
    # left and right pages of an open book, with a gap for the spine
    d.polygon([(u(2), u(7)), (u(9), u(5)), (u(15), u(7.5)), (u(15), u(27)),
               (u(9), u(25)), (u(2), u(27))], fill=255)
    d.polygon([(u(17), u(7.5)), (u(23), u(5)), (u(30), u(7)), (u(30), u(27)),
               (u(23), u(25)), (u(17), u(27))], fill=255)
    # text lines cut into the pages
    for y in (11, 15, 19):
        d.line([(u(5), u(y + 0.2)), (u(12.5), u(y + 0.8))], fill=0, width=u(1.4))
        d.line([(u(19.5), u(y + 0.8)), (u(27), u(y + 0.2))], fill=0, width=u(1.4))
    return img


def camera():
    img, d = canvas()
    d.rounded_rectangle(box(2, 9, 30, 27), radius=u(4), fill=255)
    d.rounded_rectangle(box(10, 5, 22, 11), radius=u(2), fill=255)
    d.ellipse(box(9.5, 11.5, 22.5, 24.5), fill=0)        # lens hole
    d.ellipse(box(12.5, 14.5, 19.5, 21.5), fill=255)     # lens glass
    d.ellipse(box(24.5, 11.5, 27.5, 14.5), fill=0)       # flash dot
    return img


def gear():
    img, d = canvas()
    cx = cy = 16
    teeth = 8
    pts = []
    for i in range(teeth * 4):
        a = (i / (teeth * 4)) * 2 * math.pi
        r = 14 if (i % 4) in (1, 2) else 10.5
        pts.append((u(cx + r * math.cos(a)), u(cy + r * math.sin(a))))
    d.polygon(pts, fill=255)
    d.ellipse(box(cx - 4.5, cy - 4.5, cx + 4.5, cy + 4.5), fill=0)
    return img


def mast():
    """Radio mast with signal arcs (Meshtastic app)."""
    img, d = canvas()
    # mast: an A-frame with a cross bar, topped by the antenna ball
    d.polygon([(u(14.2), u(14)), (u(8), u(30)), (u(12.2), u(30)), (u(16), u(19.5)),
               (u(19.8), u(30)), (u(24), u(30)), (u(17.8), u(14))], fill=255)
    d.rectangle(box(11, 23.5, 21, 26), fill=255)
    d.ellipse(box(12.3, 7.8, 19.7, 15.2), fill=255)
    # arcs on both sides
    for r in (7.5, 12.5):
        bb = box(16 - r, 11.5 - r, 16 + r, 11.5 + r)
        d.arc(bb, start=195, end=250, fill=255, width=u(2.8))
        d.arc(bb, start=290, end=345, fill=255, width=u(2.8))
    return img


def graph():
    """Mesh of connected nodes (MeshCore app)."""
    img, d = canvas()
    pts = [(6, 9), (24, 6), (16, 17), (7, 26), (26, 24)]
    edges = [(0, 1), (0, 2), (1, 2), (2, 3), (2, 4), (1, 4), (3, 4), (0, 3)]
    for a_, b_ in edges:
        (x0, y0), (x1, y1) = pts[a_], pts[b_]
        d.line([(u(x0), u(y0)), (u(x1), u(y1))], fill=255, width=u(1.8))
    for i, (x, y) in enumerate(pts):
        r = 4.2 if i == 2 else 3.4
        d.ellipse(box(x - r, y - r, x + r, y + r), fill=255)
    return img


def shield():
    """Shield with three linked nodes (VPN / Tailscale app)."""
    img, d = canvas()
    d.polygon([(u(16), u(2)), (u(28), u(6.5)), (u(27), u(17)), (u(22.5), u(25)), (u(16), u(30)),
               (u(9.5), u(25)), (u(5), u(17)), (u(4), u(6.5))], fill=255)
    pts = [(11, 12), (21, 12), (16, 21)]
    for (x0, y0), (x1, y1) in ((pts[0], pts[1]), (pts[1], pts[2]), (pts[2], pts[0])):
        d.line([(u(x0), u(y0)), (u(x1), u(y1))], fill=0, width=u(1.8))
    for x, y in pts:
        d.ellipse(box(x - 2.6, y - 2.6, x + 2.6, y + 2.6), fill=0)
    return img


def terminal():
    """Terminal window with a prompt (SSH app)."""
    img, d = canvas()
    d.rounded_rectangle(box(2, 4, 30, 28), radius=u(3.5), fill=255)
    d.rectangle(box(4.5, 9, 27.5, 25.5), fill=0)
    d.line([(u(8), u(13)), (u(13), u(17)), (u(8), u(21))], fill=255, width=u(2.4), joint="curve")
    d.rectangle(box(15, 19.6, 23, 22), fill=255)
    for x in (6, 9, 12):
        d.ellipse(box(x - 1, 5.6, x + 1, 7.6), fill=0)
    return img


def tv():
    """TV with a play button (Emby media app)."""
    img, d = canvas()
    d.rounded_rectangle(box(2, 5, 30, 24), radius=u(3), fill=255)
    d.rectangle(box(4.5, 7.5, 27.5, 21.5), fill=0)
    d.polygon([(u(13), u(10.5)), (u(13), u(18.5)), (u(20), u(14.5))], fill=255)
    d.rounded_rectangle(box(10, 26.5, 22, 29), radius=u(1.2), fill=255)
    d.rectangle(box(14.8, 24, 17.2, 27), fill=255)
    return img


def usb():
    """The USB trident (USB drive mode)."""
    img, d = canvas()
    w = 2.6
    d.polygon([(u(16), u(1)), (u(11.5), u(7.5)), (u(20.5), u(7.5))], fill=255)       # arrow head
    d.rectangle(box(16 - w / 2, 6, 16 + w / 2, 25), fill=255)                          # stem
    d.ellipse(box(12, 23, 20, 31), fill=255)                                           # base
    # left branch ending in a circle
    d.line([(u(16), u(19)), (u(8), u(14)), (u(8), u(10))], fill=255, width=u(w), joint="curve")
    d.ellipse(box(5, 6, 11, 12), fill=255)
    # right branch ending in a square
    d.line([(u(16), u(22)), (u(24), u(17)), (u(24), u(12))], fill=255, width=u(w), joint="curve")
    d.rectangle(box(21, 7, 27, 13), fill=255)
    return img


ICONS = [
    ("zos_icon_music", music),
    ("zos_icon_video", video),
    ("zos_icon_book", book),
    ("zos_icon_camera", camera),
    ("zos_icon_meshtastic", mast),
    ("zos_icon_meshcore", graph),
    ("zos_icon_vpn", shield),
    ("zos_icon_terminal", terminal),
    ("zos_icon_media", tv),
    ("zos_icon_settings", gear),
    ("zos_icon_usb", usb),
]


def emit_c(out_path, preview_path=None):
    lines = [
        "/* Generated by tools/make_icons.py -- do not edit by hand. */",
        '#include "lvgl.h"',
        "",
    ]
    previews = []
    for name, fn in ICONS:
        img = fn().resize((SIZE, SIZE), Image.LANCZOS)
        previews.append(img)
        data = list(img.tobytes())
        lines.append(f"static const uint8_t {name}_map[] = {{")
        for i in range(0, len(data), 16):
            lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 16]) + ",")
        lines.append("};")
        lines.append(f"const lv_image_dsc_t {name} = {{")
        lines.append("    .header.magic = LV_IMAGE_HEADER_MAGIC,")
        lines.append("    .header.cf = LV_COLOR_FORMAT_A8,")
        lines.append(f"    .header.w = {SIZE},")
        lines.append(f"    .header.h = {SIZE},")
        lines.append(f"    .header.stride = {SIZE},")
        lines.append(f"    .data_size = sizeof({name}_map),")
        lines.append(f"    .data = {name}_map,")
        lines.append("};")
        lines.append("")
    with open(out_path, "w") as f:
        f.write("\n".join(lines))
    if preview_path:
        sheet = Image.new("L", (SIZE * len(ICONS) * 4 + 8 * len(ICONS), SIZE * 4), 40)
        for i, img in enumerate(previews):
            sheet.paste(img.resize((SIZE * 4, SIZE * 4), Image.NEAREST), (i * (SIZE * 4 + 8), 0))
        sheet.save(preview_path)


if __name__ == "__main__":
    emit_c(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
