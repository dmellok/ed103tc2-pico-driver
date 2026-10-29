#!/usr/bin/env python3
"""Fetch a royalty-free photo and write src/sample.jpg for the on-device JPEG
decode + Floyd-Steinberg dither demo. A real photo with natural tonal range
dithers to 16 greys far more convincingly than a synthetic gradient.

Default image: a field of pink tulips (Floriade, Canberra) by John O'Neill,
Creative Commons Attribution-ShareAlike 3.0, via Wikimedia Commons -- the same
image the el133 sibling project ships, so the licence notice carries over.
  https://commons.wikimedia.org/wiki/File:Tulip_-_floriade_canberra.jpg

The picojpeg decoder on-device wants a *baseline* JPEG and does not resize, so we
cover-crop to exactly 1872x1404 and re-save as baseline 4:4:4 (subsampling=0).

    python3 tools/fetch_sample_jpg.py [--url URL] [--src localfile]

Requires Pillow and (for the default download) requests.
"""
import argparse
import io
import os

from PIL import Image, ImageOps

W, H = 1872, 1404
HERE = os.path.dirname(__file__)
OUT = os.path.normpath(os.path.join(HERE, "..", "src", "sample.jpg"))

DEFAULT_URL = (
    "https://commons.wikimedia.org/wiki/Special:FilePath/"
    "Tulip_-_floriade_canberra.jpg?width=2200"
)


def load(args):
    if args.src:
        return Image.open(args.src)
    import requests

    ua = "Mozilla/5.0 (compatible; ed103tc2-pico-driver demo image fetch)"
    r = requests.get(args.url, headers={"User-Agent": ua}, timeout=60)
    r.raise_for_status()
    return Image.open(io.BytesIO(r.content))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default=DEFAULT_URL, help="image URL to fetch")
    ap.add_argument("--src", help="use a local image file instead of downloading")
    args = ap.parse_args()

    im = load(args).convert("RGB")
    # Cover-crop (centre) to fill the panel exactly -- no white bars, no resize
    # work left for the on-device decoder.
    im = ImageOps.fit(im, (W, H), method=Image.LANCZOS, centering=(0.5, 0.5))
    im.save(OUT, "JPEG", quality=90, subsampling=0)  # baseline 4:4:4
    print("wrote", OUT, os.path.getsize(OUT), "bytes")


if __name__ == "__main__":
    main()
