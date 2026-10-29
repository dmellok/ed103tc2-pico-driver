/*
 * it8951.h: Waveshare 10.3" e-Paper HAT driver (ED103TC2 glass behind an
 * IT8951 controller, 1872x1404, 16-level greyscale).
 *
 * Unlike the Inky, the panel is LANDSCAPE-native and a single controller scans
 * it, so a landscape frame goes straight down the wire row by row: no rotate,
 * no split. The only twists are (a) the 10.3" glass is horizontally mirrored
 * relative to the IT8951's raster (Waveshare's demo runs it in "mode 1 =
 * horizontal mirror"), and (b) the controller wants each 16-bit word's bytes
 * swapped versus the natural packed order. Both happen on the way out, so
 * callers only ever see the logical landscape frame.
 *
 * Protocol (preambles, commands, registers, init, VCOM, load/display flow) is
 * ported from Waveshare's IT8951-ePaper Raspberry Pi C demo (EPD_IT8951.c).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Landscape geometry. Frames are packed 4bpp (2 px/byte, high nibble = even
 * column), 936 bytes/row, 1404 rows. */
#define IT8951_W            1872
#define IT8951_H            1404
#define IT8951_ROW_BYTES    (IT8951_W / 2)                   /* 936       */
#define IT8951_FRAME_BYTES  (IT8951_ROW_BYTES * IT8951_H)    /* 1,314,144 */

/* Grey codes: 0x0 = black ... 0xF = white (16 levels). */
#define IT8951_BLACK  0x0
#define IT8951_WHITE  0xF

/* Waveform modes (Waveshare E-paper-mode-declaration for the 7.8/9.7/10.3"):
 * INIT clears/deghosts (flashes), GC16 is the 16-grey full refresh, A2 is the
 * fast 1-bit mode (needs 1bpp data; not used by this demo's 4bpp paths). */
#define IT8951_MODE_INIT  0
#define IT8951_MODE_GC16  2
#define IT8951_MODE_A2    6

/* Un-mirror the 10.3" glass. Set -DEPD_MIRROR_H=0 if your panel/firmware is
 * not mirrored (the test card's black corner square would show top-right). */
#ifndef EPD_MIRROR_H
#define EPD_MIRROR_H 1
#endif

/* Probe CS/MISO both ways round at boot (see epd_io.h). */
#ifndef EPD_PIN_PROBE
#define EPD_PIN_PROBE 1
#endif

typedef struct {
    uint16_t w, h;          /* panel size reported by the controller       */
    uint32_t img_addr;      /* image buffer base in the IT8951's memory    */
    char     fw[17];        /* firmware version string                     */
    char     lut[17];       /* LUT (waveform) version, e.g. "M841_TFA5210" */
} it8951_info_t;

/* Fill one 936-byte logical landscape row: out[k] packs columns (2k, 2k+1),
 * high nibble = even column, y = 0..1403 top to bottom, x = 0 at the left as
 * viewed. */
typedef void (*it8951_row_fn)(int y, uint8_t out[IT8951_ROW_BYTES], void *ctx);

/* Reset, find and identify the controller (probing CS/MISO), enable packed
 * writes, apply VCOM. Logs everything. False if no controller answered. */
bool it8951_init(void);

/* Diagnostic: try to read device info on an arbitrary CS/MISO pairing WITHOUT
 * waiting on HRDY (fixed delays, 2 MHz). raw[] gets the first four words read
 * back so "something answered but garbled" is visible. Tears the bus down
 * again afterwards; call it8951_init() to resume normal operation. */
bool it8951_probe_blind(unsigned cs, unsigned miso, int mode, it8951_info_t *out, uint16_t raw[4]);

/* Diagnostic: sweep CS/MISO/mode/SCK-MOSI-swap using HRDY as the handshake and
 * report every pairing's raw device-info words and whether HRDY reacted. */
void it8951_probe_matrix(void);

/* Controller identity from the last successful init. */
const it8951_info_t *it8951_info(void);

/* VCOM in millivolts (positive number for a negative voltage: -1.50 V = 1500).
 * Build with -DEPD_VCOM_MV=<mV> to apply at init, or set it at run time; a
 * run-time value sticks across re-inits for the rest of the session. */
uint16_t it8951_get_vcom(void);          /* reads the controller */
void     it8951_set_vcom(uint16_t mv);
int32_t  it8951_cached_vcom(void);       /* last value read/set, no bus traffic */

/* Stream a procedural pattern via the row generator, then refresh with `mode`.
 * Blocks until the controller's LUT engine is idle again (~1-2 s). */
void it8951_show_pattern(it8951_row_fn fn, void *ctx, uint8_t mode);

/* Same for a packed-4bpp landscape frame (IT8951_FRAME_BYTES), e.g. an image
 * embedded in flash or decoded into PSRAM. */
void it8951_show_frame(const uint8_t *frame, uint8_t mode);

/* Repaint a rectangle only: rows y..y+h-1, columns x..x+w-1, taken from a
 * FULL-row generator (the driver slices the span, so overlays can share one
 * generator with the full paint). x and w are widened to 16-px boundaries.
 * A2 gives a fast black/white-only update of the area, GC16 a clean 16-grey
 * one; the IT8951 only refreshes the pixels inside the rectangle. */
void it8951_update_area(int x, int y, int w, int h, it8951_row_fn fn, void *ctx, uint8_t mode);

/* Run-time tunables (defaults from EPD_MIRROR_H / EPD_SPI_HZ). A new SPI
 * speed takes effect at the next it8951_init(). */
void     it8951_set_mirror(bool mirror);
bool     it8951_mirror(void);
void     it8951_set_spi_hz(uint32_t hz);
uint32_t it8951_spi_hz(void);

/* Stats for the UI: paints done since boot, and the last paint's timings. */
uint32_t it8951_paint_count(void);
uint32_t it8951_last_stream_ms(void);
uint32_t it8951_last_refresh_ms(void);

/* Paint the whole panel white with `mode` (INIT for a proper deghost). */
void it8951_clear(uint8_t mode);

/* Put the controller to sleep (SLEEP command). The next show_* brings it back
 * with a full re-init. */
void it8951_sleep(void);

/* Waveshare's "enhance driving capability" register poke, for panels that look
 * blurred because of a long FPC. */
void it8951_enhance_driving(void);
