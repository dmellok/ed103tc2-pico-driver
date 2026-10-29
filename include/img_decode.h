/*
 * img_decode.h: on-device JPEG -> 16-grey frame conversion.
 *
 * Decodes a baseline JPEG (in flash) at its native resolution into a full
 * 8-bit greyscale buffer in PSRAM, then Floyd-Steinberg dithers it to the
 * panel's 16 levels, packed 4bpp, into a second PSRAM buffer that
 * it8951_show_frame() can paint directly. This is what the device would do
 * with a server-delivered photo instead of a pre-packed .bin.
 *
 * The JPEG is expected to be ~1872x1404 (IT8951_W x IT8951_H): larger is
 * clipped, smaller is padded with white. No on-device resize. psram_init()
 * must have succeeded first (the buffers live in PSRAM; SRAM is far too small).
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

/*
 * Decode + dither `len` bytes of JPEG at `jpg`. Returns a pointer to the packed
 * IT8951_FRAME_BYTES landscape frame in PSRAM (pass straight to
 * it8951_show_frame), or NULL on a decode error / missing PSRAM.
 */
const uint8_t *img_decode_jpeg(const uint8_t *jpg, size_t len);
