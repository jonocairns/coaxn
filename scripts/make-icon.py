#!/usr/bin/env python3
"""Generate assets/coax.ico.

The icon is drawn procedurally rather than committed as an opaque binary so it
can be re-derived and adjusted. The mark is two spiral arms turning about one
centre, half a turn apart, on a transparent ground. It is the same mark
theme::draw_logo draws in the application, at the same proportions, and the two
are meant to stay that way.

Usage: python3 scripts/make-icon.py [output.ico]
"""

import binascii
import math
import struct
import sys
import zlib
from pathlib import Path

# Rendered at SS times the target size and box-filtered down, which is what
# keeps the curves clean at 16px.
SS = 4

# The bright arm is the application's accent. Its counterpart is a light slate
# rather than the dark navy the mark was designed against: the icon has no
# ground of its own to sit on, so both arms have to survive a taskbar that may
# be black or white, and a dark arm on a dark shelf is half a logo.
ACCENT = (0x4C, 0x7C, 0xF0)
SLATE = (0xA9, 0xB3, 0xC9)
# The same arm for a page that is not dark. A mark with no ground of its own
# cannot have one colour that works on both, so the second arm is the only
# thing that changes between the two SVGs below.
NAVY = (0x2B, 0x32, 0x42)

# Fractions of the mark's radius, matching theme::draw_spiral_arm exactly.
SWEEP = 4.7
ARM_OUTER = 0.80
ARM_INNER = 0.20
ARM_STROKE = 0.32

# The mark's radius as a fraction of the icon edge. Smaller than half, so the
# arms and their caps clear the edge on every side.
MARK_RADIUS = 0.46

# Floor on the stroke, in final device pixels. A stroke that is a constant
# fraction of the edge is a hairline at 256 and invisible at 16, so the small
# sizes are allowed to be optically heavier than the geometry asks for. This is
# the whole reason each size is rendered rather than one being scaled down.
MIN_STROKE_PX = 2.0

# BMP entries below this size, a PNG entry at or above it. Windows has read PNG
# icon entries since Vista; BMP keeps the small sizes maximally compatible.
PNG_FROM = 256

SIZES = (16, 24, 32, 48, 64, 128, 256)


def arm_distance(dx, dy, radius, stroke):
    """Distance from a point to one spiral arm's centreline, in pixels.

    The arm is r(phi) = outer -> inner over phi in [0, SWEEP], so a point at
    polar angle theta can only be near it where phi is theta plus some whole
    number of turns. Each candidate contributes |r - r(phi)|, which is the
    radial distance rather than the true perpendicular one; over this shallow a
    spiral the two differ by about three percent, which is well inside a pixel.
    """
    dist = math.hypot(dx, dy)
    if dist <= 0.0:
        theta = 0.0
    else:
        theta = math.atan2(dy, dx)

    outer = ARM_OUTER * radius
    inner = ARM_INNER * radius
    best = float("inf")

    # theta is in (-pi, pi]; the arm spans SWEEP radians from its start, so at
    # most two whole turns can bring a candidate into range.
    turn = theta
    while turn > 0.0:
        turn -= 2.0 * math.pi
    while turn <= SWEEP:
        if turn >= 0.0:
            reach = outer + (inner - outer) * (turn / SWEEP)
            best = min(best, abs(dist - reach))
        turn += 2.0 * math.pi

    # The round caps at each end, which are discs rather than part of the sweep.
    for phi in (0.0, SWEEP):
        reach = outer + (inner - outer) * (phi / SWEEP)
        cap_x = math.cos(phi) * reach
        cap_y = math.sin(phi) * reach
        best = min(best, math.hypot(dx - cap_x, dy - cap_y))

    return best - stroke * 0.5


def render(size):
    """Render one square RGBA image as a flat list of (r, g, b, a) tuples."""
    hi = size * SS
    centre = hi / 2.0
    radius = MARK_RADIUS * hi
    stroke = max(ARM_STROKE * radius, MIN_STROKE_PX * SS)

    # Both arms wind the same way from opposite starts, so the second is the
    # first read through a half-turn rotation: negating the offsets is the same
    # as adding pi to the angle, and costs nothing per pixel.
    arms = ((ACCENT, -1.0), (SLATE, 1.0))

    # Supersampled buffer, then box-downsample into the final grid. Nothing is
    # accumulated off the arms, so the ground stays transparent.
    acc = [[0.0, 0.0, 0.0, 0.0] for _ in range(size * size)]

    for py in range(hi):
        dy = py + 0.5 - centre
        out_y = (py // SS) * size

        for px in range(hi):
            dx = px + 0.5 - centre
            if dx * dx + dy * dy > (radius + stroke) ** 2:
                continue

            for colour, sign in arms:
                if arm_distance(sign * dx, sign * dy, radius, stroke) > 0.0:
                    continue
                cell = acc[out_y + (px // SS)]
                cell[0] += colour[0]
                cell[1] += colour[1]
                cell[2] += colour[2]
                cell[3] += 255.0
                break

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


def svg_points(start, radius, centre, segments=72):
    """The centreline of one arm, sampled evenly along its sweep."""
    points = []
    for step in range(segments + 1):
        t = step / segments
        angle = start + SWEEP * t
        reach = (ARM_OUTER + (ARM_INNER - ARM_OUTER) * t) * radius
        points.append((centre + math.cos(angle) * reach,
                       centre + math.sin(angle) * reach))
    return points


def svg_path(start, radius, centre):
    """The `d` attribute for one arm, as a polyline along its centreline.

    Round joins and caps are the renderer's job here rather than ours: an SVG
    stroke can ask for them, which is what the icon has to draw discs to fake.
    """
    points = svg_points(start, radius, centre)
    head = "M {:.2f} {:.2f}".format(*points[0])
    return head + "".join(" L {:.2f} {:.2f}".format(x, y) for x, y in points[1:])


def build_svg(second_arm):
    """The mark as an SVG, `second_arm` being the colour of the slate one."""
    box = 100.0
    centre = box / 2.0
    radius = MARK_RADIUS * box
    stroke = ARM_STROKE * radius
    arms = ((0.0, second_arm), (math.pi, ACCENT))

    def rgb(colour):
        return "#{:02X}{:02X}{:02X}".format(*colour)

    # Trimmed to what the arms actually cover, kept square and centred. The
    # icon needs its margin — an icon that touches its own edges reads as
    # cropped — but here the surrounding space belongs to whatever is placing
    # the mark, and a viewBox padded on its behalf just renders it small.
    reach = 0.0
    for start, _ in arms:
        for point in svg_points(start, radius, centre):
            reach = max(reach, abs(point[0] - centre), abs(point[1] - centre))
    reach += stroke / 2.0
    view = "{:.2f} {:.2f} {:.2f} {:.2f}".format(
        centre - reach, centre - reach, reach * 2.0, reach * 2.0)

    def arm(start, colour):
        return (
            '  <path d="{}" fill="none" stroke="{}" stroke-width="{:.2f}"'
            ' stroke-linecap="round" stroke-linejoin="round"/>'.format(
                svg_path(start, radius, centre), rgb(colour), stroke))

    return "\n".join((
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="{}"'
        ' role="img" aria-label="Coax">'.format(view),
        "  <title>Coax</title>",
        arm(*arms[0]),
        arm(*arms[1]),
        "</svg>",
        "",
    ))


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
    for name, second_arm in (("coax-mark-on-dark.svg", SLATE),
                             ("coax-mark-on-light.svg", NAVY)):
        path = assets / name
        path.write_text(build_svg(second_arm))
        print(f"wrote {path} ({path.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
