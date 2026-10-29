/*
 * patterns.c: test patterns. See patterns.h. Drawn in logical landscape space
 * (1872 wide x 1404 tall, x = 0 at the left as viewed): out[k] packs columns
 * (2k, 2k+1), high nibble = even column, grey 0x0 black .. 0xF white.
 */
#include "patterns.h"

#define W IT8951_W
#define H IT8951_H

static inline uint8_t pack2(uint8_t hi, uint8_t lo) { return (uint8_t)((hi << 4) | (lo & 0x0F)); }

/* 16 vertical grey bars, black at the left, white at the right. */
static void pat_vbars(int y, uint8_t out[IT8951_ROW_BYTES], void *ctx)
{
    (void)y; (void)ctx;
    for (int k = 0; k < IT8951_ROW_BYTES; k++) {
        int x0 = 2 * k;
        out[k] = pack2((uint8_t)(x0 * 16 / W), (uint8_t)((x0 + 1) * 16 / W));
    }
}

/* 16 horizontal grey bands, black at the top. */
static void pat_hbands(int y, uint8_t out[IT8951_ROW_BYTES], void *ctx)
{
    (void)ctx;
    uint8_t g = (uint8_t)(y * 16 / H);
    uint8_t b = pack2(g, g);
    for (int k = 0; k < IT8951_ROW_BYTES; k++) out[k] = b;
}

/* 117 px black/white checkerboard (16 x 12 cells exactly). */
static void pat_checker(int y, uint8_t out[IT8951_ROW_BYTES], void *ctx)
{
    (void)ctx;
    int ry = y / 117;
    for (int k = 0; k < IT8951_ROW_BYTES; k++) {
        int x0 = 2 * k;
        uint8_t a = ((x0 / 117 + ry) & 1) ? IT8951_BLACK : IT8951_WHITE;
        uint8_t b = (((x0 + 1) / 117 + ry) & 1) ? IT8951_BLACK : IT8951_WHITE;
        out[k] = pack2(a, b);
    }
}

/* 4 x 4 grid of the 16 greys, reading order (black top-left, white bottom-right). */
static void pat_blocks(int y, uint8_t out[IT8951_ROW_BYTES], void *ctx)
{
    (void)ctx;
    int row = y / (H / 4);
    if (row > 3) row = 3;
    for (int k = 0; k < IT8951_ROW_BYTES; k++) {
        int c0 = (2 * k) / (W / 4), c1 = (2 * k + 1) / (W / 4);
        if (c0 > 3) c0 = 3;
        if (c1 > 3) c1 = 3;
        out[k] = pack2((uint8_t)(row * 4 + c0), (uint8_t)(row * 4 + c1));
    }
}

/* Solid white (a clean clear). */
static void pat_white(int y, uint8_t out[IT8951_ROW_BYTES], void *ctx)
{
    (void)y; (void)ctx;
    for (int k = 0; k < IT8951_ROW_BYTES; k++) out[k] = 0xFF;
}

const it8951_pattern_t IT8951_PATTERNS[IT8951_PATTERN_COUNT] = {
    { "vertical grey bars",   pat_vbars   },
    { "horizontal grey bands", pat_hbands },
    { "checkerboard",         pat_checker },
    { "grey blocks",          pat_blocks  },
    { "solid white",          pat_white   },
};
_Static_assert(sizeof IT8951_PATTERNS / sizeof IT8951_PATTERNS[0] == IT8951_PATTERN_COUNT,
               "IT8951_PATTERN_COUNT must match the table");
