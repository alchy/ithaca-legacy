#!/usr/bin/env python3
"""ppm2png.py - prevede P6 PPM na PNG. Jen stdlib (zlib), zadny Pillow.

Slouzi k prohlizeni snimku, ktere vysype tests/test_gui_render_golden
pri ITHACA_GOLDEN_DUMP=<scenar>. Diky tomu jde posuzovat vzhled panelu
bez displeje - vcetne stroje, kde zadny panel neni.

Pouziti: python tools/ppm2png.py vstup.ppm [vystup.png]
"""
import sys
import zlib
import struct


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    # Hlavicka: P6 <w> <h> <maxval>, oddelovace jsou libovolne bile znaky.
    fields, pos = [], 0
    while len(fields) < 4:
        while pos < len(data) and data[pos : pos + 1].isspace():
            pos += 1
        if data[pos : pos + 1] == b"#":                  # komentar do konce radku
            while pos < len(data) and data[pos] != 0x0A:
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos : pos + 1].isspace():
            pos += 1
        fields.append(data[start:pos])
    pos += 1                                             # jeden bily znak za maxval
    magic, w, h = fields[0], int(fields[1]), int(fields[2])
    if magic != b"P6":
        raise SystemExit(f"ceka se P6, dostal jsem {magic!r}")
    return w, h, data[pos : pos + w * h * 3]


def write_png(path, w, h, rgb):
    raw = b"".join(b"\x00" + rgb[y * w * 3 : (y + 1) * w * 3] for y in range(h))

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 6))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else src.rsplit(".", 1)[0] + ".png"
    w, h, rgb = read_ppm(src)
    write_png(dst, w, h, rgb)
    print(f"{dst} ({w}x{h})")
