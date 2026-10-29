/*
 * patterns.h: procedural test patterns for the 10.3" panel (landscape, drawn
 * per-row so they need no 1.3 MB frame buffer). Each is an it8951_row_fn
 * passed to it8951_show_pattern().
 */
#pragma once

#include "it8951.h"

typedef struct {
    const char    *name;
    it8951_row_fn  fn;
} it8951_pattern_t;

/* Number of patterns, as a compile-time constant so screen tables can be sized
 * from it; patterns.c static-asserts it against the table. */
#define IT8951_PATTERN_COUNT 5

extern const it8951_pattern_t IT8951_PATTERNS[IT8951_PATTERN_COUNT];
