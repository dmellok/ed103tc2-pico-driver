/*
 * fonts.h: the one bitmap font the menu overlay uses. Font24 is the 17x24
 * ASCII (32..126) font from STMicroelectronics' STM32 LCD examples as shipped
 * in Waveshare's IT8951 demo (src/vendor/font24.c, BSD-3-Clause): 3 bytes per
 * glyph row, MSB = leftmost pixel, 72 bytes per glyph.
 */
#pragma once

#include <stdint.h>

typedef struct _tFont {
    const uint8_t *table;
    uint16_t Width;
    uint16_t Height;
} sFONT;

extern sFONT Font24;
