#!/usr/bin/env python3
"""Generate src/sample_card.bin: a 1872x1404 landscape 16-grey test card,
packed 4bpp (2 px/byte, high nibble = even column, 0x0 black .. 0xF white),
936 bytes/row = 1,314,144 bytes.

Pure Python (no dependencies). The image is a 4x4 grid of the 16 grey levels
in reading order (black top-left, white bottom-right) with a black grid +
border, a black bar along the TOP edge and a black square in the TOP-LEFT
corner as orientation markers: a mirrored panel shows the square top-right and
the ramp running the wrong way, a flipped one shows the bar at the bottom.
Run from anywhere:

    python3 tools/gen_sample.py
"""
import os

W, H = 1872, 1404
GRID = 6
BAR = 24
SQUARE = 150
CW, CH = W // 4, H // 4


def grey(x, y):
    if y < BAR:
        return 0x0                                   # top orientation bar
    if x < SQUARE and y < SQUARE:
        return 0x0                                   # top-left corner square
    if x < GRID or x >= W - GRID or y < GRID or y >= H - GRID:
        return 0x0                                   # border
    if (x % CW) < GRID or (y % CH) < GRID:
        return 0x0                                   # gridlines
    return min(y // CH, 3) * 4 + min(x // CW, 3)     # 0..15


def main():
    buf = bytearray(W * H // 2)
    for y in range(H):
        base = y * (W // 2)
        for bx in range(W // 2):
            buf[base + bx] = (grey(2 * bx, y) << 4) | grey(2 * bx + 1, y)
    out = os.path.join(os.path.dirname(__file__), "..", "src", "sample_card.bin")
    with open(out, "wb") as f:
        f.write(buf)
    print("wrote", os.path.normpath(out), len(buf), "bytes")


if __name__ == "__main__":
    main()
