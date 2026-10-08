#!/usr/bin/env python3
"""Prints pixels of a PNG (8-bit, non-interlaced, as written by macOS) as R,G,B.

usage: png_pixel.py <file.png> <x> <y> [<x> <y> ...]

x and y are pixels, or fractions of the width and height when they contain a '.'
(0.5 0.5 is the centre).
"""
import struct
import sys
import zlib


def read_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        sys.exit(f"{path}: not a PNG")
    pos, idat, header = 8, b"", None
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
    width, height, depth, color_type, _, _, interlace = header
    if depth != 8 or interlace or color_type not in (2, 6):
        sys.exit(f"{path}: unsupported PNG (depth {depth}, colour type {color_type}, interlace {interlace})")
    channels = 3 if color_type == 2 else 4
    stride = width * channels
    raw = zlib.decompress(idat)
    rows, previous = [], bytearray(stride)
    for y in range(height):
        filter_type = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = previous[i]
            c = previous[i - channels] if i >= channels else 0
            if filter_type == 1:
                line[i] = (line[i] + a) & 255
            elif filter_type == 2:
                line[i] = (line[i] + b) & 255
            elif filter_type == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif filter_type == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                predictor = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + predictor) & 255
        rows.append(line)
        previous = line
    return width, height, channels, rows


def main():
    if len(sys.argv) < 4 or len(sys.argv) % 2:
        sys.exit(__doc__)
    width, height, channels, rows = read_png(sys.argv[1])
    for arg_x, arg_y in zip(sys.argv[2::2], sys.argv[3::2]):
        x = int(float(arg_x) * width) if "." in arg_x else int(arg_x)
        y = int(float(arg_y) * height) if "." in arg_y else int(arg_y)
        x, y = min(x, width - 1), min(y, height - 1)
        offset = x * channels
        print(f"{rows[y][offset]},{rows[y][offset + 1]},{rows[y][offset + 2]}")


if __name__ == "__main__":
    main()
