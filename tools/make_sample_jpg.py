#!/usr/bin/env python3
"""Generate src/sample.jpg: a 1872x1404 smooth gradient, saved as a baseline
4:4:4 JPEG (subsampling=0 -> simple 8x8 MCUs), for the on-device decode +
Floyd-Steinberg dither demo. The gradient's luma sweeps the full range so the
16-level dithering is clearly visible; the colour is there only to exercise the
on-device RGB -> luma path.

Requires Pillow (pip install pillow). To show your own image instead, save any
photo as a 1872x1404 JPEG over src/sample.jpg and rebuild (the on-device decoder
does not resize -- it expects ~1872x1404, clipping/padding anything else).

    python3 tools/make_sample_jpg.py
"""
import os
from PIL import Image

W, H = 1872, 1404
out = os.path.join(os.path.dirname(__file__), "..", "src", "sample.jpg")

try:
    import numpy as np
    xr = (np.linspace(0, 255, W)).astype(int)
    yr = (np.linspace(0, 255, H)).astype(int)
    R = np.tile(xr, (H, 1))
    G = np.tile(yr[:, None], (1, W))
    B = (255 - (R + G) // 2).clip(0, 255)
    arr = np.dstack([R, G, B]).astype("uint8")
    im = Image.fromarray(arr, "RGB")
except ImportError:
    data = bytearray(W * H * 3)
    for y in range(H):
        g = y * 255 // (H - 1)
        base = y * W * 3
        for x in range(W):
            r = x * 255 // (W - 1)
            o = base + x * 3
            data[o] = r
            data[o + 1] = g
            data[o + 2] = max(0, 255 - (r + g) // 2)
    im = Image.frombytes("RGB", (W, H), bytes(data))

im.save(out, "JPEG", quality=90, subsampling=0)
print("wrote", os.path.normpath(out), os.path.getsize(out), "bytes")
