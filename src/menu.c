/*
 * menu.c: menu overlay renderer. See menu.h.
 *
 * Layout (all in logical landscape pixels, box-relative):
 *   PAD | title line | rule | count x item lines | rule | footer line | PAD
 * Text is Font24 (17x24) drawn at 2x, i.e. 34 px advance, 48 px tall, on
 * 56 px lines; the highlighted item is a black bar with white text. Each call
 * renders one scan line of the box into an 8-bit grey scratch line and packs
 * it over the caller's row, so no frame buffer is needed.
 */
#include "menu.h"
#include "fonts.h"

#include <stdbool.h>
#include <string.h>

#define SCALE   2
#define GW      (17 * SCALE)        /* glyph advance, px */
#define GH      (24 * SCALE)        /* glyph height, px  */
#define LINE_H  56
#define PAD     24
#define BORDER  6
#define RULE    12                  /* gap that carries a 2-px rule */
#define INSET   (BORDER + 8)        /* bars/rules stop short of the border */
#define MENU_W  912                 /* 25 glyphs + padding; multiple of 16 */

#define G_WHITE 0xF
#define G_BLACK 0x0
#define G_GREY  0x7

static int menu_height(const menu_t *m)
{
    return PAD + LINE_H + RULE + m->count * LINE_H + RULE + LINE_H + PAD;
}

void menu_rect(const menu_t *m, int *x, int *y, int *w, int *h)
{
    int mh = menu_height(m);
    *w = MENU_W;
    *h = mh;
    *x = ((IT8951_W - MENU_W) / 2) & ~15;
    *y = (IT8951_H - mh) / 2;
}

/* Paint scan line `grow` (0..GH-1) of `text` into line[] starting at x. */
static void text_row(uint8_t *line, int w, int x, const char *text, int grow, uint8_t colour)
{
    if (grow < 0 || grow >= GH || text == NULL) return;
    const uint8_t *tbl = Font24.table + (grow / SCALE) * 3;
    for (; *text; text++, x += GW) {
        int c = (unsigned char)*text;
        if (c < 32 || c > 126) c = '?';
        const uint8_t *g = tbl + (c - 32) * 72;
        for (int col = 0; col < 17; col++) {
            if (!(g[col >> 3] & (0x80 >> (col & 7)))) continue;
            for (int s = 0; s < SCALE; s++) {
                int px = x + col * SCALE + s;
                if (px >= 0 && px < w) line[px] = colour;
            }
        }
    }
}

/* Pack an 8-bit grey scratch line over the row at column x0: even column =
 * high nibble. */
static void pack_line(uint8_t *row, int x0, const uint8_t *line, int n)
{
    for (int px = 0; px < n; px++) {
        int x = x0 + px;
        uint8_t g = line[px];
        if (x & 1) row[x >> 1] = (uint8_t)((row[x >> 1] & 0xF0) | g);
        else       row[x >> 1] = (uint8_t)((row[x >> 1] & 0x0F) | (g << 4));
    }
}

void menu_overlay_row(const menu_t *m, int y, uint8_t row[IT8951_ROW_BYTES])
{
    int mx, my, mw, mh;
    menu_rect(m, &mx, &my, &mw, &mh);
    if (y < my || y >= my + mh) return;

    static uint8_t line[MENU_W];
    const int ly = y - my;
    memset(line, G_WHITE, (size_t)mw);

    /* frame */
    if (ly < BORDER || ly >= mh - BORDER) {
        memset(line, G_BLACK, (size_t)mw);
    } else {
        memset(line, G_BLACK, BORDER);
        memset(line + mw - BORDER, G_BLACK, BORDER);
    }

    const int tx    = PAD + BORDER;                 /* text left edge        */
    const int vpad  = (LINE_H - GH) / 2;            /* centre glyphs in line */
    const int ty    = PAD;                          /* title line            */
    const int iy0   = ty + LINE_H + RULE;           /* first item line       */
    const int fy    = iy0 + m->count * LINE_H + RULE;   /* footer line       */

    text_row(line, mw, tx, m->title, ly - ty - vpad, G_BLACK);

    int r1 = ty + LINE_H + RULE / 2 - 1;            /* rule under the title  */
    int r2 = fy - RULE / 2 - 1;                     /* rule above the footer */
    if (ly == r1 || ly == r1 + 1 || ly == r2 || ly == r2 + 1)
        memset(line + INSET, G_BLACK, (size_t)(mw - 2 * INSET));

    if (ly >= iy0 && ly < iy0 + m->count * LINE_H) {
        int i = (ly - iy0) / LINE_H;
        bool sel = (i == m->selected);
        if (sel) memset(line + INSET, G_BLACK, (size_t)(mw - 2 * INSET));
        text_row(line, mw, tx, m->items[i], ly - (iy0 + i * LINE_H) - vpad,
                 sel ? G_WHITE : G_BLACK);
    }

    text_row(line, mw, tx, m->footer, ly - fy - vpad, G_GREY);

    pack_line(row, mx, line, mw);
}

/* ---- banner ----------------------------------------------------------------- */

#define BAN_PAD     24
#define BAN_BORDER  4
#define BAN_H       (GH + 24)

void banner_rect(const char *text, int *x, int *y, int *w, int *h)
{
    int tw = (int)strlen(text) * GW + 2 * BAN_PAD;
    if (tw > IT8951_W - 32) tw = IT8951_W - 32;
    *w = (tw + 15) & ~15;
    *h = BAN_H;
    *x = 16;
    *y = IT8951_H - 16 - BAN_H;
}

void banner_overlay_row(const char *text, int y, uint8_t row[IT8951_ROW_BYTES])
{
    int bx, by, bw, bh;
    banner_rect(text, &bx, &by, &bw, &bh);
    if (y < by || y >= by + bh) return;
    static uint8_t line[IT8951_W];
    int ly = y - by;
    memset(line, G_WHITE, (size_t)bw);
    if (ly < BAN_BORDER || ly >= bh - BAN_BORDER) memset(line, G_BLACK, (size_t)bw);
    else { memset(line, G_BLACK, BAN_BORDER); memset(line + bw - BAN_BORDER, G_BLACK, BAN_BORDER); }
    text_row(line, bw, BAN_PAD, text, ly - 12, G_BLACK);
    pack_line(row, bx, line, bw);
}
