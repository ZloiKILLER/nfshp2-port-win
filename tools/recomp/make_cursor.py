#!/usr/bin/env python3
"""A 32 x 32 picture as Windows' .cur: make_cursor.py <in.dds|png> <out.cur> [hot x] [hot y]

Without a hot spot it is the top-most opaque pixel, the left-most of that row: the tip of an arrow.
speed.exe loads data/ui/cursor.cur (window.cpp, menu_cursor).
"""
import struct
import sys

from PIL import Image


def main():
    im = Image.open(sys.argv[1]).convert('RGBA')
    w, h = im.size
    if len(sys.argv) > 4:
        hx, hy = int(sys.argv[3]), int(sys.argv[4])
    else:
        hx, hy = next((x, y) for y in range(h) for x in range(w) if im.getpixel((x, y))[3] > 128)
    px = bytearray()
    for y in range(h - 1, -1, -1):
        for x in range(w):
            r, g, b, a = im.getpixel((x, y))
            px += bytes((b, g, r, a))
    # the AND mask: 1 where the pixel is wholly transparent, rows of 32 bits, bottom up
    mask = bytearray()
    for y in range(h - 1, -1, -1):
        row = 0
        for x in range(w):
            row = row << 1 | (1 if im.getpixel((x, y))[3] == 0 else 0)
        row <<= (32 - w % 32) % 32
        mask += row.to_bytes(((w + 31) // 32) * 4, 'big')
    dib = struct.pack('<IiiHHIIiiII', 40, w, h * 2, 1, 32, 0, len(px) + len(mask), 0, 0, 0, 0) + bytes(px) + bytes(mask)
    head = struct.pack('<HHH', 0, 2, 1) + struct.pack('<BBBBHHII', w & 255, h & 255, 0, 0, hx, hy, len(dib), 22)
    with open(sys.argv[2], 'wb') as f:
        f.write(head + dib)
    print(f'{sys.argv[2]}: {w} x {h}, hot spot {hx}, {hy}')


main()
