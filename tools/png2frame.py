#!/usr/bin/env python3
"""Convert a PNG (or any Pillow-readable image) into the IT8951 frame format:
1872x1404 landscape, 16 grey levels, packed 4bpp (2 px/byte, high nibble =
even column, 0x0 black .. 0xF white), 936 bytes/row = 1,314,144 bytes.

Requires Pillow:  pip install pillow

    python3 tools/png2frame.py photo.png            # -> src/sample_card.bin
    python3 tools/png2frame.py photo.png out.bin
    python3 tools/png2frame.py --no-dither logo.png # nearest level, for graphics

Then rebuild; src/sample_card.S embeds whatever src/sample_card.bin contains.
Default is Floyd-Steinberg dithering to the 16 evenly spaced greys, matching
what src/img_decode.c does on the device.
"""
import os
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow not installed. Run: pip install pillow")

W, H = 1872, 1404
LEVELS = [i * 17 for i in range(16)]   # 0, 17, ... 255


def cover_fit(im):
    scale = max(W / im.width, H / im.height)
    im = im.resize((round(im.width * scale), round(im.height * scale)), Image.LANCZOS)
    left = (im.width - W) // 2
    top = (im.height - H) // 2
    return im.crop((left, top, left + W, top + H))


def to_levels(im, dither):
    """Return a list of per-pixel level indices 0..15, row-major."""
    gray = im.convert("L")
    if not dither:
        return [(v + 8) // 17 for v in gray.tobytes()]
    # Pillow dithers against an explicit palette only for RGB sources.
    pal = Image.new("P", (1, 1))
    pal.putpalette(sum(([v, v, v] for v in LEVELS), []) + [0] * (768 - 48))
    q = gray.convert("RGB").quantize(palette=pal, dither=Image.Dither.FLOYDSTEINBERG)
    # Palette indices are exactly our level indices (0 black .. 15 white).
    return list(q.tobytes())


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    dither = "--no-dither" not in sys.argv
    if not args:
        sys.exit("usage: png2frame.py [--no-dither] input.png [output.bin]")
    out = args[1] if len(args) > 1 else os.path.join(
        os.path.dirname(__file__), "..", "src", "sample_card.bin")

    im = cover_fit(Image.open(args[0]).convert("RGB"))
    lv = to_levels(im, dither)
    buf = bytearray(W * H // 2)
    for i in range(0, W * H, 2):
        buf[i >> 1] = (lv[i] << 4) | lv[i + 1]
    with open(out, "wb") as f:
        f.write(buf)
    print("wrote", os.path.normpath(out), len(buf), "bytes",
          "(dithered)" if dither else "(nearest level)")


if __name__ == "__main__":
    main()
