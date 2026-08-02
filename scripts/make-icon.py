#!/usr/bin/env python3
"""Generate assets/coax.ico.

The icon is drawn procedurally rather than committed as an opaque binary so it
can be re-derived and adjusted. The mark is a coaxial cable cross-section: a
centre conductor inside a shield ring, on a rounded dark tile.

Usage: python3 scripts/make-icon.py [output.ico]
"""

import binascii
import struct
import sys
import zlib
from pathlib import Path

# Rendered at SS times the target size and box-filtered down, which is what
# keeps the ring edges clean at 16px.
SS = 4

BG_TOP = (0x1D, 0x24, 0x30)
BG_BOTTOM = (0x0F, 0x13, 0x19)
ACCENT = (0x5C, 0xD0, 0xFF)

# Fractions of the icon edge.
CORNER_RADIUS = 0.22
RING_OUTER = 0.375
RING_INNER = 0.265
CORE_RADIUS = 0.115

# BMP entries below this size, a PNG entry at or above it. Windows has read PNG
# icon entries since Vista; BMP keeps the small sizes maximally compatible.
PNG_FROM = 256

SIZES = (16, 24, 32, 48, 64, 128, 256)


def rounded_rect_alpha(x, y, size, radius):
    """Coverage of a rounded square filling the tile, 0.0 or 1.0 (supersampled)."""
    inner_max = size - radius
    cx = min(max(x, radius), inner_max)
    cy = min(max(y, radius), inner_max)
    dx = x - cx
    dy = y - cy
    return 1.0 if dx * dx + dy * dy <= radius * radius else 0.0


def render(size):
    """Render one square RGBA image as a flat list of (r, g, b, a) tuples."""
    hi = size * SS
    radius = CORNER_RADIUS * hi
    centre = hi / 2.0
    ring_outer = RING_OUTER * hi
    ring_inner = RING_INNER * hi
    core = CORE_RADIUS * hi

    # Supersampled buffer, then box-downsample into the final grid.
    acc = [[0.0, 0.0, 0.0, 0.0] for _ in range(size * size)]

    for py in range(hi):
        # Vertical background gradient, evaluated once per row.
        t = py / max(hi - 1, 1)
        bg = tuple(BG_TOP[i] + (BG_BOTTOM[i] - BG_TOP[i]) * t for i in range(3))
        dy = py + 0.5 - centre
        dy2 = dy * dy
        out_y = (py // SS) * size

        for px in range(hi):
            alpha = rounded_rect_alpha(px + 0.5, py + 0.5, hi, radius)
            if alpha == 0.0:
                continue

            dx = px + 0.5 - centre
            dist = (dx * dx + dy2) ** 0.5

            if dist <= core or ring_inner <= dist <= ring_outer:
                colour = ACCENT
            else:
                colour = bg

            cell = acc[out_y + (px // SS)]
            cell[0] += colour[0]
            cell[1] += colour[1]
            cell[2] += colour[2]
            cell[3] += 255.0

    samples = float(SS * SS)
    pixels = []
    for cell in acc:
        a = cell[3] / samples
        if a <= 0.0:
            pixels.append((0, 0, 0, 0))
            continue
        # Un-premultiply: colour was only accumulated on covered samples.
        covered = cell[3] / 255.0
        pixels.append((
            int(round(cell[0] / covered)),
            int(round(cell[1] / covered)),
            int(round(cell[2] / covered)),
            int(round(a)),
        ))
    return pixels


def encode_png(pixels, size):
    raw = bytearray()
    for row in range(size):
        raw.append(0)  # filter type: none
        for col in range(size):
            r, g, b, a = pixels[row * size + col]
            raw += bytes((r, g, b, a))

    def chunk(tag, payload):
        body = tag + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(
            ">I", binascii.crc32(body) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
            + chunk(b"IEND", b""))


def encode_bmp(pixels, size):
    # BITMAPINFOHEADER with doubled height: XOR bitmap then AND mask, both
    # bottom-up. The AND mask is left zeroed because the alpha channel carries
    # transparency; Windows still requires the rows to be present.
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0,
                         size * size * 4, 0, 0, 0, 0)

    xor = bytearray()
    for row in reversed(range(size)):
        for col in range(size):
            r, g, b, a = pixels[row * size + col]
            xor += bytes((b, g, r, a))

    mask_stride = ((size + 31) // 32) * 4
    return header + bytes(xor) + bytes(mask_stride * size)


def build_ico(sizes):
    images = []
    for size in sizes:
        pixels = render(size)
        if size >= PNG_FROM:
            images.append((size, encode_png(pixels, size)))
        else:
            images.append((size, encode_bmp(pixels, size)))

    out = bytearray(struct.pack("<HHH", 0, 1, len(images)))
    offset = 6 + 16 * len(images)
    for size, data in images:
        # 256 is stored as 0 in the directory's single-byte dimensions.
        dim = 0 if size >= 256 else size
        out += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    for _, data in images:
        out += data
    return bytes(out)


def main():
    dest = Path(sys.argv[1]) if len(sys.argv) > 1 else (
        Path(__file__).resolve().parent.parent / "assets" / "coax.ico")
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(build_ico(SIZES))
    print(f"wrote {dest} ({dest.stat().st_size} bytes, sizes: "
          f"{', '.join(str(s) for s in SIZES)})")


if __name__ == "__main__":
    main()
