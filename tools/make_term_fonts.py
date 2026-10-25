#!/usr/bin/env python3
"""Turn X11 misc-fixed bitmap fonts (public domain) into C tables for the
terminal: one byte per glyph row, bit (width-1) = leftmost pixel.

Usage: python3 tools/make_term_fonts.py src/assets/termfonts.c [preview.png]
Needs xfonts-base (/usr/share/fonts/X11/misc/*.pcf.gz).
"""
import gzip
import struct
import sys

MISC = "/usr/share/fonts/X11/misc"
FONTS = [  # (C name, file, label)
    ("term_font_6x12", "6x12.pcf.gz", "6x12"),
    ("term_font_6x10", "6x10.pcf.gz", "6x10"),
    ("term_font_5x8", "5x8.pcf.gz", "5x8"),
]

# What a shell session needs: ASCII, Latin-1/Ext-A, punctuation, arrows,
# maths, box drawing, blocks, shapes, braille (graphs in btop & co).
RANGES = [(0x20, 0x7E), (0xA0, 0x17F), (0x2010, 0x2027), (0x2030, 0x205E), (0x20AC, 0x20AC),
          (0x2190, 0x21FF), (0x2200, 0x22FF), (0x2300, 0x23FF), (0x2500, 0x259F), (0x25A0, 0x25FF),
          (0x2600, 0x26FF), (0x2713, 0x2718), (0x2800, 0x28FF)]

PCF_PROPERTIES, PCF_ACCELERATORS, PCF_METRICS, PCF_BITMAPS = 1, 2, 4, 8
PCF_BDF_ENCODINGS, PCF_BDF_ACCELERATORS = 0x20, 0x100


class PCF:
    def __init__(self, path):
        d = gzip.open(path).read()
        assert d[:4] == b"\x01fcp", path
        n = struct.unpack("<I", d[4:8])[0]
        self.d = d
        self.tables = {}
        for i in range(n):
            typ, fmt, size, off = struct.unpack("<IIII", d[8 + 16 * i:24 + 16 * i])
            self.tables[typ] = off
        self.props = self._properties()
        self._accel()
        self._metrics()
        self._bitmaps()
        self._encodings()

    def _hdr(self, typ):
        off = self.tables[typ]
        fmt = struct.unpack("<I", self.d[off:off + 4])[0]
        return off + 4, fmt, ">" if fmt & 4 else "<"

    def _properties(self):
        p, fmt, e = self._hdr(PCF_PROPERTIES)
        n = struct.unpack(e + "I", self.d[p:p + 4])[0]
        p += 4
        raw = [struct.unpack(e + "IBI", self.d[p + 9 * i:p + 9 * i + 9]) for i in range(n)]
        p += 9 * n
        if n & 3:
            p += 4 - (n & 3)
        ssz = struct.unpack(e + "I", self.d[p:p + 4])[0]
        strings = self.d[p + 4:p + 4 + ssz]

        def s(o):
            return strings[o:strings.index(b"\0", o)].decode()
        return {s(nm): (s(v) if isstr else v) for nm, isstr, v in raw}

    def _accel(self):
        typ = PCF_BDF_ACCELERATORS if PCF_BDF_ACCELERATORS in self.tables else PCF_ACCELERATORS
        p, fmt, e = self._hdr(typ)
        self.ascent, self.descent = struct.unpack(e + "ii", self.d[p + 8:p + 16])

    def _metrics(self):
        p, fmt, e = self._hdr(PCF_METRICS)
        self.metrics = []
        if fmt & 0x100:  # compressed
            n = struct.unpack(e + "H", self.d[p:p + 2])[0]
            p += 2
            for i in range(n):
                lsb, rsb, w, asc, desc = (b - 0x80 for b in self.d[p + 5 * i:p + 5 * i + 5])
                self.metrics.append((lsb, rsb, w, asc, desc))
        else:
            n = struct.unpack(e + "I", self.d[p:p + 4])[0]
            p += 4
            for i in range(n):
                lsb, rsb, w, asc, desc, _ = struct.unpack(e + "hhhhhH", self.d[p + 12 * i:p + 12 * i + 12])
                self.metrics.append((lsb, rsb, w, asc, desc))

    def _bitmaps(self):
        p, fmt, e = self._hdr(PCF_BITMAPS)
        n = struct.unpack(e + "I", self.d[p:p + 4])[0]
        p += 4
        self.offsets = struct.unpack(e + "%dI" % n, self.d[p:p + 4 * n])
        p += 4 * n
        sizes = struct.unpack(e + "4I", self.d[p:p + 16])
        p += 16
        self.bits = self.d[p:p + sizes[fmt & 3]]
        self.pad = 1 << (fmt & 3)
        self.msbit = bool(fmt & 8)
        self.msbyte = bool(fmt & 4)
        self.unit = 1 << ((fmt >> 4) & 3)

    def _encodings(self):
        p, fmt, e = self._hdr(PCF_BDF_ENCODINGS)
        min2, max2, min1, max1, self.default = struct.unpack(e + "hhhhh", self.d[p:p + 10])
        p += 10
        cnt = (max2 - min2 + 1) * (max1 - min1 + 1)
        idx = struct.unpack(e + "%dH" % cnt, self.d[p:p + 2 * cnt])
        self.enc = {}
        for i, gi in enumerate(idx):
            if gi == 0xFFFF:
                continue
            b1 = i // (max2 - min2 + 1) + min1
            b2 = i % (max2 - min2 + 1) + min2
            self.enc[(b1 << 8) | b2] = gi

    def glyph_rows(self, cp, cell_w, cell_h):
        """Cell-sized rows (list of ints, cell_w bits, MSB left)."""
        gi = self.enc.get(cp)
        if gi is None:
            return None
        lsb, rsb, w, asc, desc = self.metrics[gi]
        gw, gh = rsb - lsb, asc + desc
        stride = ((gw + 7) // 8 + self.pad - 1) // self.pad * self.pad
        base = self.offsets[gi]
        rows = [0] * cell_h
        for y in range(gh):
            row = self.bits[base + y * stride: base + (y + 1) * stride]
            if not self.msbyte and self.unit > 1:   # swap bytes within scan units
                row = b"".join(row[i:i + self.unit][::-1] for i in range(0, len(row), self.unit))
            for x in range(gw):
                byte = row[x // 8]
                bit = (byte >> (7 - x % 8)) & 1 if self.msbit else (byte >> (x % 8)) & 1
                if bit:
                    cx, cy = x + lsb, y + (self.ascent - asc)
                    if 0 <= cx < cell_w and 0 <= cy < cell_h:
                        rows[cy] |= 1 << (cell_w - 1 - cx)
        return rows


def main(out, preview=None):
    lines = ["/* Generated by tools/make_term_fonts.py from X11 misc-fixed fonts.",
             " * misc-fixed: \"Public domain terminal emulator font. Share and enjoy.\" */",
             '#include "../apps/ssh/termfont.h"', ""]
    previews = []
    for cname, fname, label in FONTS:
        f = PCF(f"{MISC}/{fname}")
        assert "Public domain" in f.props.get("COPYRIGHT", ""), fname
        cell_w = f.metrics[f.enc[ord("M")]][2]
        cell_h = f.ascent + f.descent
        cps, data = [], []
        for a, b in RANGES:
            for cp in range(a, b + 1):
                rows = f.glyph_rows(cp, cell_w, cell_h)
                if rows is not None:
                    cps.append(cp)
                    data.extend(rows)
        lines.append(f"static const uint16_t {cname}_cp[] = {{")
        for i in range(0, len(cps), 12):
            lines.append("    " + ", ".join(f"0x{c:04x}" for c in cps[i:i + 12]) + ",")
        lines.append("};")
        lines.append(f"static const uint8_t {cname}_rows[] = {{")
        for i in range(0, len(data), 16):
            lines.append("    " + ", ".join(f"0x{r:02x}" for r in data[i:i + 16]) + ",")
        lines.append("};")
        lines.append(f"const term_font_t {cname} = {{\"{label}\", {cell_w}, {cell_h}, {len(cps)}, "
                     f"{cname}_cp, {cname}_rows}};")
        lines.append("")
        print(f"{label}: {cell_w}x{cell_h}, {len(cps)} glyphs -> {320 // cell_w}x{170 // cell_h} cells")
        previews.append((f, cell_w, cell_h))
    with open(out, "w") as fh:
        fh.write("\n".join(lines))

    if preview:
        from PIL import Image
        sample = ["user@zero:~$ ls -la  ┌─┬─┐ █▓▒░ ⣿⡇ é ü ß → ✓",
                  "drwxr-xr-x  2 pi pi 4096 Oct 25 10:16 .ssh"]
        H = sum(ch * 2 + 4 for _, _, ch in previews)
        img = Image.new("L", (320, H), 0)
        y = 0
        for f, cw, ch in previews:
            for line in sample:
                x = 0
                for c in line:
                    rows = f.glyph_rows(ord(c), cw, ch) or [0] * ch
                    for ry, r in enumerate(rows):
                        for rx in range(cw):
                            if r & (1 << (cw - 1 - rx)) and x + rx < 320:
                                img.putpixel((x + rx, y + ry), 255)
                    x += cw
                y += ch
            y += 4
        img.resize((640, H * 2), Image.NEAREST).save(preview)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
