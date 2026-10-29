/*
 * images.h: embedded sample frame(s).
 *
 * `sample_card` is a landscape 1872x1404 16-grey packed-4bpp frame
 * (IT8951_FRAME_BYTES), embedded from src/sample_card.bin via
 * src/sample_card.S. Pass it to it8951_show_frame(). Add more images by
 * converting them with tools/png2frame.py and embedding them the same way.
 *
 * `sample_jpg` is a 1872x1404 baseline JPEG (src/sample.jpg via
 * src/sample_jpg.S), decoded and dithered to a 16-grey frame on the device
 * itself by img_decode_jpeg(); `sample_jpg_end` marks the end so the length is
 * (sample_jpg_end - sample_jpg).
 */
#pragma once

#include <stdint.h>

extern const uint8_t sample_card[];
extern const uint8_t sample_jpg[];
extern const uint8_t sample_jpg_end[];
