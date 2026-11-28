#!/usr/bin/env python3
"""make_icon.py — generate res/usbmon.ico (16/32/48/256 px).

Pure standard library: no PIL/numpy.  The glyph reproduces the v1.1.1
(PySide6) runtime icon: light rounded shell card with an inner USB socket
block, a deep-blue USB trident and a status badge dot at the bottom right
(status "usb" — badge carries no mark).  Geometry is traced from the
64x64 SVG in v1.1.1's usb_monitor/app.py `usb_svg()`; everything is
rasterised through signed-distance fields with 4x supersampling so small
sizes stay crisp and the 256 layer stays vector-faithful.

Run once after editing; the .ico is committed so builds never depend on
Python.  windres embeds it via res/usbmon.rc (resource id 1).
"""
import struct
import sys
import os
import zlib

SIZES = (16, 32, 48, 256)
SS = 4  # supersample factor

# light theme (usb_monitor/app.py Theme, light palette)
SHELL  = (0xDF, 0xE8, 0xF4)   # icon_shell  #dfe8f4
SOCKET = (0xF7, 0xFA, 0xFD)   # icon_socket #f7fafd
BORDER = (0xCB, 0xD8, 0xE7)   # border      #cbd8e7
ACCENT = (0x20, 0x51, 0xB0)   # accent      #2051b0
PANEL  = (0xFA, 0xFC, 0xFF)   # panel (badge ring) #fafcff

# geometry on the v1.1.1 64-unit grid
SOCKET_RECT  = (19, 5, 26, 18, 5.0)          # x y w h rx
PLUG_RECTS   = [(24, 9, 5, 7, 1.5), (35, 9, 5, 7, 1.5)]
SHELL_RECT   = (13, 20, 38, 35, 10.0)
TRIDENT_SEGS = [((32, 26), (32, 42)),        # M32 26v16
                ((24, 34), (40, 34)),        # M24 34h16
                ((24, 34), (19, 29)),        # M24 34l-5 -5
                ((40, 34), (45, 29))]        # M40 34l5 -5
TRIDENT_W    = 3.0
BADGE_C      = (50, 45, 11.0)                # cx cy r (fill)
BADGE_RING_W = 3.0                           # stroke="panel" width 3


def sd_round_rect(px, py, x, y, w, h, r):
    """Signed distance to a rounded rect (negative inside), SVG coords."""
    cx, cy = x + w / 2.0, y + h / 2.0
    hw, hh = w / 2.0, h / 2.0
    r = min(r, hw, hh)
    qx = abs(px - cx) - (hw - r)
    qy = abs(py - cy) - (hh - r)
    if qx > 0 and qy > 0:
        return (qx * qx + qy * qy) ** 0.5 - r
    return max(qx, qy) - r


def sd_segment(px, py, a, b):
    dx, dy = b[0] - a[0], b[1] - a[1]
    L2 = dx * dx + dy * dy
    if L2 == 0:
        return ((px - a[0]) ** 2 + (py - a[1]) ** 2) ** 0.5
    t = max(0.0, min(1.0, ((px - a[0]) * dx + (py - a[1]) * dy) / L2))
    qx, qy = a[0] + t * dx, a[1] + t * dy
    return ((px - qx) ** 2 + (py - qy) ** 2) ** 0.5


def over(dst, src, a):
    """Porter-Duff src-over with straight alpha."""
    if a <= 0:
        return dst
    return tuple(src[i] * a + dst[i] * (1 - a) for i in range(3))


def render(size):
    """Render one size -> row-major [(r,g,b,a), ...] pixels."""
    k = size / 64.0

    # (bbox, draw) — draw(sample) returns (color, alpha).  All coordinates
    # (bbox included) are in size space (0..size), so bboxes get scaled too.
    def rect_fill(rect, col):
        x, y, w, h, r = rect
        return (tuple(c * k for c in (x - 1, y - 1, x + w + 1, y + h + 1)),
                lambda fx, fy: (col, 1.0 if sd_round_rect(fx, fy, *(c * k for c in rect)) <= 0 else 0.0))

    def rect_stroke(rect, col, sw):
        x, y, w, h, r = rect
        half = sw / 2.0
        return (tuple(c * k for c in (x - half - 1, y - half - 1, x + w + half + 1, y + h + half + 1)),
                lambda fx, fy: (col, 1.0 if abs(sd_round_rect(fx, fy, *(c * k for c in rect))) <= half else 0.0))

    def seg_stroke(a, b, col, sw):
        half = sw / 2.0
        bb = tuple(c * k for c in (min(a[0], b[0]) - half - 1, min(a[1], b[1]) - half - 1,
                                   max(a[0], b[0]) + half + 1, max(a[1], b[1]) + half + 1))
        return (bb, lambda fx, fy: (col, 1.0 if sd_segment(fx, fy, (a[0] * k, a[1] * k), (b[0] * k, b[1] * k)) <= half else 0.0))

    def circ_fill(c, col):
        cx, cy, r = c
        return (tuple(v * k for v in (cx - r - 1, cy - r - 1, cx + r + 1, cy + r + 1)),
                lambda fx, fy: (col, 1.0 if ((fx - cx * k) ** 2 + (fy - cy * k) ** 2) ** 0.5 <= r * k else 0.0))

    def circ_ring(c, col, sw):
        cx, cy, r = c
        half = sw / 2.0
        return (tuple(v * k for v in (cx - r - half - 1, cy - r - half - 1, cx + r + half + 1, cy + r + half + 1)),
                lambda fx, fy: (col, 1.0 if abs(((fx - cx * k) ** 2 + (fy - cy * k) ** 2) ** 0.5 - r * k) <= half else 0.0))

    layers = [
        rect_fill(SOCKET_RECT, SOCKET),
        rect_stroke(SOCKET_RECT, BORDER, 2.0),
        rect_fill(PLUG_RECTS[0], ACCENT),
        rect_fill(PLUG_RECTS[1], ACCENT),
        rect_fill(SHELL_RECT, SHELL),
        rect_stroke(SHELL_RECT, BORDER, 2.0),
    ] + [seg_stroke(a, b, ACCENT, TRIDENT_W) for a, b in TRIDENT_SEGS] + [
        circ_fill(BADGE_C, ACCENT),
        circ_ring(BADGE_C, PANEL, BADGE_RING_W),
    ]

    px = [(0, 0, 0, 0)] * (size * size)
    for y in range(size):
        for x in range(size):
            acc = [0.0, 0.0, 0.0, 0.0]
            for sy in range(SS):
                for sx in range(SS):
                    fx = x + (sx + 0.5) / SS
                    fy = y + (sy + 0.5) / SS
                    col = (0.0, 0.0, 0.0)
                    alpha = 0.0
                    for bb, draw in layers:
                        if not (bb[0] <= fx <= bb[2] and bb[1] <= fy <= bb[3]):
                            continue
                        c, a = draw(fx, fy)
                        if a > 0:
                            col = over(col, c, a)
                            alpha = alpha * (1 - a) + a
                    for i in range(3):
                        acc[i] += col[i]
                    acc[3] += alpha
            base = y * size + x
            px[base] = (int(acc[0] / (SS * SS) + 0.5),
                        int(acc[1] / (SS * SS) + 0.5),
                        int(acc[2] / (SS * SS) + 0.5),
                        int(acc[3] / (SS * SS) * 255 + 0.5))
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
            r, g, b, a = pixels[y * size + x]
            xor += bytes((b, g, r, a))
    and_mask = bytearray()
    stride = ((size + 31) // 32) * 4
    for y in range(size - 1, -1, -1):          # bottom-up, 1bpp, transparent=1
        row = bytearray(stride)
        for xb in range((size + 7) // 8):
            bits = 0
            for bit in range(8):
                x = xb * 8 + bit
                if x < size and pixels[y * size + x][3] < 128:
                    bits |= 1 << (7 - bit)
            row[xb] = bits
        and_mask += row
    return bytes(hdr) + bytes(xor) + bytes(and_mask)


def png_chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + \
        struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


def png_entry(pixels, size):
    """PNG-in-ICO layer (the 256px one keeps alpha, stays compact)."""
    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    raw = bytearray()
    for y in range(size):
        raw.append(0)                       # filter: none
        for x in range(size):
            r, g, b, a = pixels[y * size + x]
            raw += bytes((r, g, b, a))
    idat = zlib.compress(bytes(raw), 9)
    return b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", ihdr) + \
        png_chunk(b"IDAT", idat) + png_chunk(b"IEND", b"")


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "res", "usbmon.ico")
    images = []
    entries = []
    off = 6 + 16 * len(SIZES)
    for s in SIZES:
        img = png_entry(render(s), s) if s > 48 else bmp_entry(render(s), s)
        entries.append(struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32,
                                   len(img), off))
        off += len(img)
        images.append(img)
    dir_header = struct.pack("<HHH", 0, 1, len(SIZES))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "wb") as f:
        f.write(dir_header + b"".join(entries) + b"".join(images))
    print("wrote %s (%d bytes; sizes %s)" % (out, off, list(SIZES)))


if __name__ == "__main__":
    main()
