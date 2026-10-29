/*
 * menu.h: a demo menu drawn as an overlay on top of whatever is on the panel.
 *
 * Nothing is buffered: menu_overlay_row() composites the menu onto a logical
 * landscape row as it streams out, so it sits on top of any content generator
 * (pattern or frame). menu_rect() gives the box so callers can repaint just
 * that area with it8951_update_area() when the menu opens, moves or closes.
 */
#pragma once

#include <stdint.h>
#include "it8951.h"

typedef struct {
    const char        *title;
    const char *const *items;
    int                count;
    int                selected;    /* highlighted entry */
    const char        *footer;      /* one-line hint under the list */
} menu_t;

/* Bounding box of the overlay, centred on the panel; x and w are 16-px aligned
 * so it8951_update_area() does not have to widen it. */
void menu_rect(const menu_t *m, int *x, int *y, int *w, int *h);

/* Composite the menu onto one logical row (no-op for rows outside the box). */
void menu_overlay_row(const menu_t *m, int y, uint8_t row[IT8951_ROW_BYTES]);

/* A one-line banner box in the bottom-left corner (used for the web UI URL):
 * its rectangle (16-px aligned) and the per-row compositor. */
void banner_rect(const char *text, int *x, int *y, int *w, int *h);
void banner_overlay_row(const char *text, int y, uint8_t row[IT8951_ROW_BYTES]);
