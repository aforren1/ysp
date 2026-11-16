"""Writes the web site's favicon.ico from ysp/screen.h's window icon.

    python3 tools/web_favicon.py OUT.ico

The demo pages get the icon from SDL once their window opens; the index
opens no window. The site uses the header's own icon so that the two
agree, and reads it from the header so that they stay the same. Standard
library only: examples/web/build.sh runs it on the Pages runner.
"""
import re
import struct
import sys
import zlib
from pathlib import Path

HEADER = Path(__file__).resolve().parent.parent / "include" / "ysp" / "screen.h"


def numbers(src, name):
    m = re.search(r"\b" + name + r"\b[^=]*=\s*\{(.*?)\};", src, re.S)
    if not m:
        sys.exit(f"web_favicon.py: {name} not found in {HEADER}")
    return [int(x, 0) for x in re.findall(r"0x[0-9a-fA-F]+|\d+", m.group(1))]


def decode(pal, rle, side, off, n):
    px = bytearray()
    for b in rle[off:off + n]:
        if (b & 15) >= len(pal):
            sys.exit("web_favicon.py: a palette index past the palette")
        px += bytes(pal[b & 15]) * ((b >> 4) + 1)
    if len(px) != side * side * 4:
        sys.exit(f"web_favicon.py: the {side} px runs do not fill the square")
    return bytes(px)


def png(side, rgba):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    rows = b"".join(b"\x00" + rgba[y * side * 4:(y + 1) * side * 4] for y in range(side))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", side, side, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: python3 tools/web_favicon.py OUT.ico")
    src = HEADER.read_text(encoding="utf-8")
    flat = numbers(src, "yscr__icon_pal")
    pal = [flat[i:i + 4] for i in range(0, len(flat), 4)]
    rle = numbers(src, "yscr__icon_rle")
    sizes = numbers(src, "yscr__icon_size")
    images = [(sizes[i], png(sizes[i], decode(pal, rle, *sizes[i:i + 3]))) for i in range(0, len(sizes), 3)]
    # ICO with PNG images: a directory entry per size, then the images
    out = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for side, data in images:
        out += struct.pack("<BBBBHHII", side % 256, side % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    out += b"".join(data for _, data in images)
    Path(sys.argv[1]).write_bytes(out)


if __name__ == "__main__":
    main()
