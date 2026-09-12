#!/usr/bin/env python3
"""make_icon.py — generate res/usbmon.ico (16/32/48 px, 32bpp BMP entries).

Pure standard library: no PIL/numpy.  The glyph reuses the toast visual
language (dark slate card, white USB trident, green accent bar), drawn
with 4x supersampling so small sizes stay legible.

Run once after editing; the .ico is committed so builds never depend on
Python.  windres embeds it via res/usbmon.rc (resource id 1).
"""
import struct
import sys
import os

SIZES = (16, 32, 48)
SS = 4  # supersample factor

BG = (0x22, 0x27, 0x2E)       # dark slate (TW_BG in gui_win32.c)
BORDER = (0x3A, 0x41, 0x50)   # TW_BORDER
GLYPH = (0xF2, 0xF4, 0xF7)    # TW_TITLE near-white
ACCENT = (0x35, 0xB4, 0x6A)   # TW_ACC_ADD green

# geometry on a 48-unit grid (scaled by s/48)
G = dict(
    radius=12,
    circ_cx=24, circ_cy=12.5, circ_r=3.4,       # circle at the trident top
    stem_x=24, stem_y0=15.5, stem_y1=32, stem_hw=1.7,
    junc=(24, 32),
    branch_l=(13.5, 21.5), branch_r=(34.5, 21.5), branch_hw=1.8,
    end_l_sq=(13.5, 21.5, 2.9),                  # square end (left branch)
    end_r_c=(34.5, 21.5, 2.9),                   # circle end (right branch)
    bar_x0=12, bar_x1=36, bar_y0=40.5, bar_y1=43,
)


def lerp(a, b, t):
    return a + (b - a) * t


def dist_pt_seg(px, py, x0, y0, x1, y1):
    dx, dy = x1 - x0, y1 - y0
    L2 = dx * dx + dy * dy
    if L2 == 0:
        return ((px - x0) ** 2 + (py - y0) ** 2) ** 0.5
    t = max(0.0, min(1.0, ((px - x0) * dx + (py - y0) * dy) / L2))
    qx, qy = x0 + t * dx, y0 + t * dy
    return ((px - qx) ** 2 + (py - qy) ** 2) ** 0.5


def in_rounded_rect(px, py, size):
    r = G["radius"] * size / 48.0
    inset = 0.5 * size / 48.0 * 2  # half pixel border ring
    lo, hi = inset, size - inset
    if not (lo <= px <= hi and lo <= py <= hi):
        return "out"
    # distance to rounded-corner boundary
    cx = min(max(px, r), size - r)
    cy = min(max(py, r), size - r)
    d = ((px - cx) ** 2 + (py - cy) ** 2) ** 0.5
    if d > r - inset:
        return "edge"
    return "in"


def sample_glyph(px, py, size):
    """Return True when (px, py) is covered by the white trident."""
    k = size / 48.0
    # circle top
    if (px - G["circ_cx"] * k) ** 2 + (py - G["circ_cy"] * k) ** 2 <= (G["circ_r"] * k) ** 2:
        return True
    # stem
    if abs(px - G["stem_x"] * k) <= G["stem_hw"] * k and \
       G["stem_y0"] * k <= py <= G["stem_y1"] * k:
        return True
    # branches (junction -> ends)
    jx, jy = G["junc"]
    for ex, ey in (G["branch_l"], G["branch_r"]):
        if dist_pt_seg(px, py, jx * k, jy * k, ex * k, ey * k) <= G["branch_hw"] * k:
            return True
    # left square end
    sx, sy, sr = G["end_l_sq"]
    if abs(px - sx * k) <= sr * k and abs(py - sy * k) <= sr * k:
        return True
    # right circle end
    ccx, ccy, cr = G["end_r_c"]
    if (px - ccx * k) ** 2 + (py - ccy * k) ** 2 <= (cr * k) ** 2:
        return True
    return False


def sample_bar(px, py, size):
    k = size / 48.0
    return G["bar_x0"] * k <= px <= G["bar_x1"] * k and \
           G["bar_y0"] * k <= py <= G["bar_y1"] * k


def render(size):
    """Render one size -> (row-major RGBA pixels, size)."""
    px = [[(0, 0, 0, 0)] * size for _ in range(size)]
    for y in range(size):
        for x in range(size):
            # supersample 4x4 in this pixel
            acc = [0, 0, 0, 0]
            for sy in range(SS):
                for sx in range(SS):
                    fx = x + (sx + 0.5) / SS
                    fy = y + (sy + 0.5) / SS
                    pos = in_rounded_rect(fx, fy, size)
                    if pos == "out":
                        col = (0, 0, 0, 0)
                    elif pos == "edge":
                        col = BORDER + (255,)
                    elif sample_glyph(fx, fy, size):
                        col = GLYPH + (255,)
                    elif sample_bar(fx, fy, size):
                        col = ACCENT + (255,)
                    else:
                        col = BG + (255,)
                    for i in range(4):
                        acc[i] += col[i]
            px[y][x] = tuple(c / (SS * SS) for c in acc)
    return px


def bmp_entry(pixels, size):
    """Encode one icon image: BITMAPINFOHEADER + BGRA rows + AND mask."""
    hdr = struct.pack(
        "<IiiHHIIiiII",
        40, size, size * 2, 1, 32, 0,
        size * size * 4 + ((size + 31) // 32) * 4 * size,
        0, 0, 0, 0,
    )
    xor = bytearray()
    for y in range(size - 1, -1, -1):          # bottom-up
        for x in range(size):
            r, g, b, a = pixels[y][x]
            xor += bytes((int(b), int(g), int(r), int(a)))
    and_mask = bytearray()
    stride = ((size + 31) // 32) * 4
    for y in range(size - 1, -1, -1):          # bottom-up, 1bpp, transparent=1
        row = bytearray(stride)
        for xb in range((size + 7) // 8):
            bits = 0
            for bit in range(8):
                x = xb * 8 + bit
                if x < size and pixels[y][x][3] < 128:
                    bits |= 1 << (7 - bit)
            row[xb] = bits
        and_mask += row
    return bytes(hdr) + bytes(xor) + bytes(and_mask)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "res", "usbmon.ico")
    images = []
    for s in SIZES:
        images.append(bmp_entry(render(s), s))
    dir_header = struct.pack("<HHH", 0, 1, len(SIZES))
    entries = b""
    off = 6 + 16 * len(SIZES)
    for s, img in zip(SIZES, images):
        entries += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32,
                               len(img), off)
        off += len(img)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "wb") as f:
        f.write(dir_header + entries + b"".join(images))
    print("wrote %s (%d bytes; sizes %s)" % (out, off, list(SIZES)))


if __name__ == "__main__":
    main()
