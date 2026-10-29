/*
 * img_decode.c: decode a baseline JPEG to a 16-grey packed frame, on-device.
 *
 * Two PSRAM buffers (SRAM is far too small for either):
 *   GRAY  at PSRAM_XIP_BASE               -- IT8951_W*IT8951_H  = 2,628,288 B
 *   FRAME at PSRAM_XIP_BASE + GRAY_BYTES  -- IT8951_FRAME_BYTES = 1,314,144 B
 *
 * picojpeg (src/vendor/picojpeg.c) decodes MCU by MCU into its own small static
 * buffers; we convert each MCU's 8x8 colour blocks to luma and scatter them
 * into the GRAY image in PSRAM. Then a single Floyd-Steinberg pass reads GRAY
 * row by row and writes the dithered, 16-level packed FRAME, propagating
 * quantisation error with two int16 error rows kept in SRAM.
 */
#include "img_decode.h"
#include "picojpeg.h"
#include "psram.h"
#include "it8951.h"

#include <stdio.h>
#include <string.h>

#define GRAY_BYTES  ((size_t)IT8951_W * IT8951_H)   /* 2,628,288 */

static uint8_t *const GRAY  = PSRAM_XIP_BASE;
static uint8_t *const FRAME = PSRAM_XIP_BASE + GRAY_BYTES;

/* ---- JPEG input: feed bytes straight from the flash-resident image. ---- */
static const uint8_t *s_jpg;
static size_t         s_jpg_len, s_jpg_pos;

static unsigned char jpg_read(unsigned char *buf, unsigned char n,
                              unsigned char *got, void *cd)
{
    (void)cd;
    size_t rem = s_jpg_len - s_jpg_pos;
    unsigned char k = (rem < n) ? (unsigned char)rem : n;
    memcpy(buf, s_jpg + s_jpg_pos, k);
    s_jpg_pos += k;
    *got = k;
    return 0;
}

/* ---- Floyd-Steinberg: GRAY (PSRAM) -> 16-level packed FRAME (PSRAM). ----
 * Error rows carry a one-pixel guard on each side so x-1 / x+1 never need a
 * bounds check. Levels sit at multiples of 17 (0, 17, ... 255); the panel
 * code is the level index (0 black .. 15 white). */
static int16_t err_a[IT8951_W + 2];
static int16_t err_b[IT8951_W + 2];

static void dither(void)
{
    int16_t *cur = err_a, *nxt = err_b;
    memset(cur, 0, sizeof err_a);

    for (int y = 0; y < IT8951_H; y++) {
        memset(nxt, 0, sizeof err_b);
        const uint8_t *src  = GRAY  + (size_t)y * IT8951_W;
        uint8_t       *frow = FRAME + (size_t)y * IT8951_ROW_BYTES;

        for (int x = 0; x < IT8951_W; x++) {
            int v = src[x] + cur[x + 1];
            if (v < 0) v = 0; else if (v > 255) v = 255;
            int level = (v + 8) / 17;                 /* nearest of 16 */
            int e = v - level * 17;

            if (x & 1) frow[x >> 1] = (uint8_t)((frow[x >> 1] & 0xF0) | level);
            else       frow[x >> 1] = (uint8_t)(level << 4);   /* even col = high nibble */

            /* 7/16 -> (x+1,y), 3/16 -> (x-1,y+1), 5/16 -> (x,y+1), 1/16 -> (x+1,y+1) */
            cur[x + 2] += (int16_t)(e * 7 / 16);
            nxt[x]     += (int16_t)(e * 3 / 16);
            nxt[x + 1] += (int16_t)(e * 5 / 16);
            nxt[x + 2] += (int16_t)(e * 1 / 16);
        }
        int16_t *t = cur; cur = nxt; nxt = t;
    }
}

const uint8_t *img_decode_jpeg(const uint8_t *jpg, size_t len)
{
    s_jpg = jpg; s_jpg_len = len; s_jpg_pos = 0;

    pjpeg_image_info_t info;
    unsigned char st = pjpeg_decode_init(&info, jpg_read, NULL, 0 /* full res */);
    if (st) { printf("jpeg: init error %u\n", st); return NULL; }
    printf("jpeg: %dx%d comps=%d MCU %dx%d (%dx%d MCUs)\n",
           info.m_width, info.m_height, info.m_comps,
           info.m_MCUWidth, info.m_MCUHeight,
           info.m_MCUSPerRow, info.m_MCUSPerCol);

    /* Pre-fill white so an image smaller than the panel pads cleanly. */
    memset(GRAY, 0xFF, GRAY_BYTES);

    int mcu_x = 0, mcu_y = 0;
    for (;;) {
        st = pjpeg_decode_mcu();
        if (st) {
            if (st == PJPG_NO_MORE_BLOCKS) break;
            printf("jpeg: mcu error %u\n", st);
            return NULL;
        }
        if (mcu_y >= info.m_MCUSPerCol) break;

        /* Scatter the MCU's 8x8 blocks into the GRAY image as ITU-R 601 luma.
         * picojpeg places the blocks at fixed quadrant offsets: TL=0, TR=64,
         * BL=128, BR=192. */
        for (int by0 = 0; by0 < info.m_MCUHeight; by0 += 8) {
            for (int bx0 = 0; bx0 < info.m_MCUWidth; bx0 += 8) {
                int ofs = (bx0 >> 3) * 64 + (by0 >> 3) * 128;
                const uint8_t *pr = info.m_pMCUBufR + ofs;
                const uint8_t *pg = (info.m_comps == 1) ? pr : info.m_pMCUBufG + ofs;
                const uint8_t *pb = (info.m_comps == 1) ? pr : info.m_pMCUBufB + ofs;

                for (int j = 0; j < 8; j++) {
                    int iy = mcu_y * info.m_MCUHeight + by0 + j;
                    if (iy >= IT8951_H) break;
                    uint8_t *drow = GRAY + (size_t)iy * IT8951_W;
                    for (int i = 0; i < 8; i++) {
                        int ix = mcu_x * info.m_MCUWidth + bx0 + i;
                        if (ix >= IT8951_W) continue;
                        int k = j * 8 + i;
                        drow[ix] = (info.m_comps == 1)
                            ? pr[k]
                            : (uint8_t)((77 * pr[k] + 150 * pg[k] + 29 * pb[k]) >> 8);
                    }
                }
            }
        }

        if (++mcu_x == info.m_MCUSPerRow) { mcu_x = 0; mcu_y++; }
    }

    dither();
    return FRAME;
}
