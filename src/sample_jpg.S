/*
 * Embed the 1872x1404 sample JPEG into flash (.rodata, XIP) via .incbin, so the
 * on-device decoder (src/img_decode.c) can read it straight from flash.
 *
 * Current image: "Tulip - floriade canberra" by John O'Neill, CC BY-SA 3.0, via
 * Wikimedia Commons. Fetch + crop it with  tools/fetch_sample_jpg.py , generate a
 * synthetic gradient with  tools/make_sample_jpg.py , or drop in your own
 * 1872x1404 baseline JPEG over it and rebuild.
 *
 * Path is relative to the build working directory (the project root).
 */
.section .rodata
.balign 4
.global sample_jpg
sample_jpg:
.incbin "src/sample.jpg"
.global sample_jpg_end
sample_jpg_end:
