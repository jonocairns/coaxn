#!/usr/bin/env python3
"""Generate the mark: assets/coax.ico and the two assets/coax-mark-*.svg files.

The assets are drawn procedurally rather than committed as opaque binaries so
they can be re-derived and adjusted. The mark is a hexagon cut once, the offcut
pushed clear of the body and given the accent. It is the same mark
theme::draw_logo draws in the application, at the same proportions, and the two
are meant to stay that way.

Usage: python3 scripts/make-icon.py [assets-dir]

The argument is the directory the three files are written to, not a file path;
it defaults to assets/ beside this script's parent and is created if missing.
"""

import binascii
import math
import struct
import sys
import zlib
from pathlib import Path

# Rendered at SS times the target size and box-filtered down, which is what
# keeps the hexagon's angled edges and the cut clean at 16px.
SS = 4

# The offcut is the application's accent. The body is a light slate rather than
# the dark navy the mark was designed against: the icon has no ground of its
# own to sit on, so it has to survive a taskbar that may be black or white, and
# a dark body on a dark shelf is most of a missing logo.
#
# The accent is the one colour that cannot change between the two files, since
# it is the same value in both. It sits at the lightness where it clears 4.4:1
# against near-black and against white at once — the balance point, which is
# the most any colour can manage against both grounds.
ACCENT = (0x63, 0x63, 0xFD)
SLATE = (0xA9, 0xB3, 0xC9)
# The same body for a page that is not dark. A mark with no ground of its own
# cannot have one colour that works on both, so the body is the only thing that
# changes between the two SVGs below.
NAVY = (0x2B, 0x32, 0x42)

# Fractions of the mark's radius, matching theme::draw_logo exactly. A pointy
# top hexagon rather than a circle: every circular mark sits in the most
# crowded neighbourhood there is, and the silhouette is the part that survives
# to 16px.
FACET_SIDES = 6
FACET_RADIUS = 0.95   # circumradius; the gap between the pieces takes the rest
CUT_ANGLE = math.radians(28.0)
CUT_OFFSET = 0.20     # how far the cut sits off centre, along its own normal
CUT_GAP = 0.11        # how far apart the two pieces are pushed

# The mark's radius as a fraction of the icon edge. The hexagon's lowest vertex
# plus half the gap reaches it almost exactly, so this number is also the
# margin: a little under half leaves the mark clear of every edge.
MARK_RADIUS = 0.44

# BMP entries below this size, a PNG entry at or above it. Windows has read PNG
# icon entries since Vista; BMP keeps the small sizes maximally compatible.
PNG_FROM = 256

SIZES = (16, 24, 32, 48, 64, 128, 256)


def hexagon(radius, centre):
    """A pointy top hexagon, vertex up."""
    return [(centre + math.cos(-math.pi / 2.0 + math.pi / 3.0 * k) * radius,
             centre + math.sin(-math.pi / 2.0 + math.pi / 3.0 * k) * radius)
            for k in range(FACET_SIDES)]


def _cross(a, b, side_a, side_b):
    """Where the edge a->b crosses the cut."""
    t = side_a / (side_a - side_b)
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)


def clip(polygon, normal, distance, keep):
    """The part of `polygon` on one side of a line. Sutherland-Hodgman.

    `keep` is +1 for the side the normal points at and -1 for the other. A
    convex polygon clipped by a half plane stays convex, which is what lets
    both renderings fill these without a concave path.
    """
    out = []
    previous = polygon[-1]
    was = keep * (normal[0] * previous[0] + normal[1] * previous[1] - distance)
    for point in polygon:
        now = keep * (normal[0] * point[0] + normal[1] * point[1] - distance)
        if now >= 0.0:
            if was < 0.0:
                out.append(_cross(previous, point, was, now))
            out.append(point)
        elif was >= 0.0:
            out.append(_cross(previous, point, was, now))
        previous, was = point, now
    return out


def facet(radius, centre, body_colour):
    """The mark: the body, and the offcut that carries the accent.

    One straight cut, and the two pieces slid apart along its normal rather
    than left touching — the gap is what makes it read as cut rather than as
    two colours of one shape. This is the single source of the geometry: the
    raster below fills these points and so does the SVG.
    """
    corners = hexagon(FACET_RADIUS * radius, centre)
    normal = (math.sin(CUT_ANGLE), -math.cos(CUT_ANGLE))
    distance = normal[0] * centre + normal[1] * centre + CUT_OFFSET * radius
    shift = CUT_GAP * radius / 2.0
    body = [(x - normal[0] * shift, y - normal[1] * shift)
            for x, y in clip(corners, normal, distance, -1.0)]
    offcut = [(x + normal[0] * shift, y + normal[1] * shift)
              for x, y in clip(corners, normal, distance, 1.0)]
    return [(body, body_colour), (offcut, ACCENT)]


def spans(polygon, y):
    """The x intervals where a horizontal line at `y` lies inside `polygon`.

    Scanline rather than a point-in-polygon test per pixel: the largest icon
    has a million samples, and testing every sample against every edge is work
    for an answer each row already knows. Both pieces are convex, so a row
    crosses each exactly twice and the crossings pair off in order.
    """
    crossings = []
    previous = polygon[-1]
    for point in polygon:
        (x0, y0), (x1, y1) = previous, point
        if (y0 > y) != (y1 > y):
            crossings.append(x0 + (x1 - x0) * (y - y0) / (y1 - y0))
        previous = point
    crossings.sort()
    return list(zip(crossings[0::2], crossings[1::2]))


def render(size):
    """Render one square RGBA image as a flat list of (r, g, b, a) tuples."""
    hi = size * SS
    centre = hi / 2.0
    radius = MARK_RADIUS * hi

    shapes = facet(radius, centre, SLATE)

    # Supersampled coverage, box-downsampled into the final grid. Nothing is
    # accumulated off the mark, so the ground stays transparent.
    acc = [[0.0, 0.0, 0.0, 0.0] for _ in range(size * size)]

    for py in range(hi):
        y = py + 0.5
        out_y = (py // SS) * size
        for polygon, colour in shapes:
            for x0, x1 in spans(polygon, y):
                first = max(0, int(math.floor(x0 - 0.5)))
                last = min(hi - 1, int(math.ceil(x1 - 0.5)))
                for px in range(first, last + 1):
                    if not x0 <= px + 0.5 <= x1:
                        continue
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


def svg_path(points):
    """The `d` attribute for one piece, closed."""
    head = "M {:.2f} {:.2f}".format(*points[0])
    return (head
            + "".join(" L {:.2f} {:.2f}".format(x, y) for x, y in points[1:])
            + " Z")


def build_svg(body_colour):
    """The mark as an SVG, `body_colour` being the colour of the large piece."""
    box = 100.0
    centre = box / 2.0
    radius = MARK_RADIUS * box

    def rgb(colour):
        return "#{:02X}{:02X}{:02X}".format(*colour)

    pieces = facet(radius, centre, body_colour)

    # Trimmed to what the mark actually covers, kept square and centred. The
    # icon needs its margin — an icon that touches its own edges reads as
    # cropped — but here the surrounding space belongs to whatever is placing
    # the mark, and a viewBox padded on its behalf just renders it small.
    reach = 0.0
    for points, _ in pieces:
        for x, y in points:
            reach = max(reach, abs(x - centre), abs(y - centre))
    view = "{:.2f} {:.2f} {:.2f} {:.2f}".format(
        centre - reach, centre - reach, reach * 2.0, reach * 2.0)

    lines = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="{}"'
             ' role="img" aria-label="Coax">'.format(view),
             "  <title>Coax</title>"]
    for points, colour in pieces:
        lines.append('  <path d="{}" fill="{}"/>'.format(
            svg_path(points), rgb(colour)))
    lines += ["</svg>", ""]
    return "\n".join(lines)


def main():
    assets = (Path(sys.argv[1]) if len(sys.argv) > 1
              else Path(__file__).resolve().parent.parent / "assets")
    assets.mkdir(parents=True, exist_ok=True)

    icon = assets / "coax.ico"
    icon.write_bytes(build_ico(SIZES))
    print(f"wrote {icon} ({icon.stat().st_size} bytes, sizes: "
          f"{', '.join(str(s) for s in SIZES)})")

    # Two, because the mark has no ground of its own and a README is read on
    # whichever page GitHub decides to serve.
    for name, body_colour in (("coax-mark-on-dark.svg", SLATE),
                              ("coax-mark-on-light.svg", NAVY)):
        path = assets / name
        path.write_text(build_svg(body_colour))
        print(f"wrote {path} ({path.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
