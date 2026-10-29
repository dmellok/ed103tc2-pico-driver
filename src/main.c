/*
 * ed103tc2-pico-driver: minimal demo that drives the Waveshare 10.3" e-Paper
 * HAT (IT8951, 1872x1404, 16 greys) on a Pico Plus 2 W. Paints the vertical
 * grey-bar test pattern on boot, then lets you pick screens -- an embedded JPEG
 * decoded + dithered on the device, a pre-packed embedded image, the rest of
 * the patterns -- from a menu drawn as an overlay on top of the current screen.
 *
 * Inputs: the Pico's BOOTSEL/BOOT button (flash-CS trick and GP45), an
 * optional push button on GP15, the USB serial console (see help()), and the
 * web UI served over WiFi (src/net.c, www/index.html) which also exposes the
 * run-time tunables and an emulated screen.
 *
 * One-button grammar:  short press = open the menu / move down a line,
 *                      long press  = select the highlighted line / (menu closed)
 *                                    just advance to the next screen.
 *
 * Everything on the panel goes through one row generator, composite_row():
 * the current content, then the URL banner, then the menu. Overlays therefore
 * need no frame buffer, partial updates can slice any rectangle out of it,
 * and the web UI's preview is the same generator decimated.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/stdio.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"

#include "it8951.h"
#include "epd_io.h"
#include "patterns.h"
#include "images.h"
#include "psram.h"
#include "img_decode.h"
#include "menu.h"
#include "net.h"

#ifndef BTN_PIN
#define BTN_PIN 15                  /* optional external button to GND; idle high */
#endif
#ifndef BOOT_BTN_PIN
#define BOOT_BTN_PIN 45             /* Pico Plus 2 W wires its BOOT button to GP45 too */
#endif
#ifndef LONG_PRESS_MS
#define LONG_PRESS_MS 600
#endif
#ifndef MENU_MOVE_MODE
#define MENU_MOVE_MODE IT8951_MODE_A2   /* fast B/W update when the highlight moves */
#endif

static bool have_psram;             /* set at boot; the JPEG screen needs PSRAM */

/* Run-time tunables (web UI / serial). */
static uint8_t  tune_move_mode     = MENU_MOVE_MODE;
static uint32_t tune_long_press_ms = LONG_PRESS_MS;
static bool     tune_banner        = true;

static volatile bool s_busy;        /* a paint is in progress (for the UI) */

/* ---- screens ---------------------------------------------------------------- */

#define SLOT_CARD    (IT8951_PATTERN_COUNT)       /* pre-packed sample_card.bin */
#define SLOT_JPEG    (IT8951_PATTERN_COUNT + 1)   /* decode + dither sample.jpg */
#define SCREEN_COUNT (IT8951_PATTERN_COUNT + 2)

/* Display order: the boot pattern, then the on-device JPEG, then the pre-packed
 * card, then the rest of the patterns. */
static int slot_at(int pos)
{
    switch (pos) {
    case 0:  return 0;
    case 1:  return SLOT_JPEG;
    case 2:  return SLOT_CARD;
    default: return pos - 2;
    }
}

static const char *slot_name(int slot)
{
    if (slot < IT8951_PATTERN_COUNT) return IT8951_PATTERNS[slot].name;
    return slot == SLOT_CARD ? "test card (.bin)" : "on-device JPEG";
}

/* What is currently under the overlays: a full-row generator + context. */
static it8951_row_fn content_fn;
static void         *content_ctx;
static int           cur_pos;

static void frame_row(int y, uint8_t out[IT8951_ROW_BYTES], void *ctx)
{
    memcpy(out, (const uint8_t *)ctx + (size_t)y * IT8951_ROW_BYTES, IT8951_ROW_BYTES);
}

/* ---- overlays: banner + menu ------------------------------------------------- */

static char banner_text[40];        /* "http://a.b.c.d/" once WiFi is up */

#define MENU_CLEAR  (SCREEN_COUNT)
#define MENU_SLEEP  (SCREEN_COUNT + 1)
#define MENU_COUNT  (SCREEN_COUNT + 2)

static const char *menu_items[MENU_COUNT];
static menu_t menu = {
    .title  = "ED103TC2 DEMO",
    .items  = menu_items,
    .count  = MENU_COUNT,
    .footer = "short: move  long: select",
};
static bool menu_open;

static void composite_row(int y, uint8_t out[IT8951_ROW_BYTES], void *ctx)
{
    (void)ctx;
    content_fn(y, out, content_ctx);
    if (tune_banner && banner_text[0]) banner_overlay_row(banner_text, y, out);
    if (menu_open) menu_overlay_row(&menu, y, out);
}

static void banner_update(void)
{
    if (!banner_text[0]) return;
    int x, y, w, h;
    banner_rect(banner_text, &x, &y, &w, &h);
    s_busy = true;
    it8951_update_area(x, y, w, h, composite_row, NULL, IT8951_MODE_GC16);
    s_busy = false;
}

static void menu_paint(uint8_t mode)
{
    int x, y, w, h;
    menu_rect(&menu, &x, &y, &w, &h);
    s_busy = true;
    it8951_update_area(x, y, w, h, composite_row, NULL, mode);
    s_busy = false;
}

static void menu_log(void)
{
    printf("menu: ");
    for (int i = 0; i < menu.count; i++)
        printf("%s%s%s", i == menu.selected ? "[" : " ", menu.items[i], i == menu.selected ? "]" : " ");
    printf("\n");
}

static void menu_show(void)
{
    menu.selected = cur_pos;
    menu_open = true;
    menu_log();
    menu_paint(IT8951_MODE_GC16);
}

static void menu_set(int sel)
{
    if (!menu_open) { menu_show(); if (sel == cur_pos) return; }
    menu.selected = ((sel % menu.count) + menu.count) % menu.count;
    menu_log();
    menu_paint(tune_move_mode);
}

static void menu_move(int delta)
{
    if (!menu_open) { menu_show(); return; }
    menu_set(menu.selected + delta);
}

static void menu_close(void)
{
    if (!menu_open) return;
    menu_open = false;
    printf("menu: closed\n");
    menu_paint(IT8951_MODE_GC16);
}

/* ---- painting --------------------------------------------------------------- */

static void show_pos(int pos)
{
    if (pos < 0 || pos >= SCREEN_COUNT) return;
    int idx = slot_at(pos);
    menu_open = false;
    if (idx < IT8951_PATTERN_COUNT) {
        printf("paint: pattern \"%s\"\n", IT8951_PATTERNS[idx].name);
        content_fn = IT8951_PATTERNS[idx].fn;
        content_ctx = NULL;
    } else if (idx == SLOT_CARD) {
        printf("paint: image \"sample_card\" (pre-packed .bin)\n");
        content_fn = frame_row;
        content_ctx = (void *)sample_card;
    } else {
        if (!have_psram) { printf("paint: JPEG skipped (no PSRAM)\n"); return; }
        printf("paint: decoding embedded JPEG on-device...\n");
        s_busy = true;
        uint32_t t0 = to_ms_since_boot(get_absolute_time());
        const uint8_t *frame = img_decode_jpeg(sample_jpg, (size_t)(sample_jpg_end - sample_jpg));
        if (!frame) { printf("paint: JPEG decode failed\n"); s_busy = false; return; }
        printf("paint: decoded + dithered in %lu ms\n",
               (unsigned long)(to_ms_since_boot(get_absolute_time()) - t0));
        content_fn = frame_row;
        content_ctx = (void *)frame;
    }
    cur_pos = pos;
    s_busy = true;
    it8951_show_pattern(composite_row, NULL, IT8951_MODE_GC16);
    s_busy = false;
}

static void show_next(void)
{
    show_pos((cur_pos + 1) % SCREEN_COUNT);
}

static void repaint(void)
{
    s_busy = true;
    it8951_show_pattern(composite_row, NULL, IT8951_MODE_GC16);
    s_busy = false;
}

static void clear_screen(uint8_t mode)
{
    printf("paint: %s clear\n", mode == IT8951_MODE_INIT ? "INIT" : "GC16");
    menu_open = false;
    content_fn = IT8951_PATTERNS[IT8951_PATTERN_COUNT - 1].fn;   /* "solid white" is last */
    content_ctx = NULL;
    cur_pos = SCREEN_COUNT - 1;
    s_busy = true;
    it8951_clear(mode);
    s_busy = false;
}

static void menu_select(void)
{
    int s = menu.selected;
    if (s < SCREEN_COUNT)       show_pos(s);
    else if (s == MENU_CLEAR)   clear_screen(IT8951_MODE_INIT);
    else                      { menu_close(); it8951_sleep(); }
}

/* ---- pin discovery ----------------------------------------------------------- */

#define DIAG_PINS 30

static void diag_sample(uint32_t *out)
{
    *out = 0;
    for (uint p = 0; p < DIAG_PINS; p++) if (gpio_get(p)) *out |= 1u << p;
}

/* Classify GP0..GP29 as driven HIGH, driven LOW or floating by sampling each
 * under a pull-down and then a pull-up, then pulse RST and report every pin
 * that changes: HRDY drops while RST is low and rises again ~0.8 s after
 * release, which finds it wherever the adapter routes it. */
static void diag_pins(void)
{
    uint32_t dn = 0, up = 0;
    printf("diag: scanning GP0..GP%d (pull-down, then pull-up)\n", DIAG_PINS - 1);
    epd_io_deinit();
    for (uint p = 0; p < DIAG_PINS; p++) {
        if (p == EPD_PIN_RST) continue;
        gpio_init(p);
        gpio_pull_down(p);
    }
    sleep_ms(5);
    diag_sample(&dn);
    for (uint p = 0; p < DIAG_PINS; p++) if (p != EPD_PIN_RST) gpio_pull_up(p);
    sleep_ms(5);
    diag_sample(&up);

    printf("diag: driven HIGH:");
    for (uint p = 0; p < DIAG_PINS; p++) if (p != EPD_PIN_RST && (dn & up & (1u << p))) printf(" GP%u", p);
    printf("\ndiag: driven LOW :");
    for (uint p = 0; p < DIAG_PINS; p++) if (p != EPD_PIN_RST && !((dn | up) & (1u << p))) printf(" GP%u", p);
    printf("\ndiag: floating   :");
    for (uint p = 0; p < DIAG_PINS; p++) if (p != EPD_PIN_RST && !(dn & (1u << p)) && (up & (1u << p))) printf(" GP%u", p);
    printf("\n");

    for (uint p = 0; p < DIAG_PINS; p++) if (p != EPD_PIN_RST) gpio_pull_down(p);
    gpio_init(EPD_PIN_RST); gpio_put(EPD_PIN_RST, 1); gpio_set_dir(EPD_PIN_RST, GPIO_OUT);
    sleep_ms(50);
    uint32_t before, during, a5, a50, a500, a2000;
    diag_sample(&before);
    gpio_put(EPD_PIN_RST, 0); sleep_ms(10); diag_sample(&during);
    gpio_put(EPD_PIN_RST, 1);
    sleep_ms(5);    diag_sample(&a5);
    sleep_ms(45);   diag_sample(&a50);
    sleep_ms(450);  diag_sample(&a500);
    sleep_ms(1500); diag_sample(&a2000);
    uint32_t changed = (before ^ during) | (before ^ a5) | (before ^ a50) | (before ^ a500) | (before ^ a2000);
    changed &= ~(1u << EPD_PIN_RST);
    printf("diag: RST pulse on GP%d -> pins that moved (before/during/+5ms/+50ms/+500ms/+2s):", EPD_PIN_RST);
    if (!changed) printf(" none");
    for (uint p = 0; p < DIAG_PINS; p++) {
        if (!(changed & (1u << p))) continue;
        printf(" GP%u=%u%u%u%u%u%u", p, (before >> p) & 1, (during >> p) & 1, (a5 >> p) & 1,
               (a50 >> p) & 1, (a500 >> p) & 1, (a2000 >> p) & 1);
    }
    printf("\ndiag: a pin that is low during reset and rises within ~2 s afterwards is HRDY\n"
           "diag: (build with -DEPD_PIN_HRDY=<n>); nothing moving means the HAT is unpowered,\n"
           "diag: RST is not on GP%d, or the HAT's interface switch is on I80 rather than SPI.\n",
           EPD_PIN_RST);

    for (uint p = 0; p < DIAG_PINS; p++) if (p != EPD_PIN_RST) gpio_disable_pulls(p);
    gpio_pull_up(BTN_PIN);
}

/* ---- inputs ----------------------------------------------------------------- */

/* Read the BOOTSEL button (pico-examples picoboard/button, BSD-3-Clause): float
 * the flash CS for a moment and sample it. Runs from RAM, interrupts off. */
static bool __no_inline_not_in_flash_func(bootsel_down)(void)
{
    const uint CS_PIN_INDEX = 1;
    uint32_t flags = save_and_disable_interrupts();
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    for (volatile int i = 0; i < 1000; ++i) {}
#if PICO_RP2040
    bool pressed = !(sio_hw->gpio_hi_in & (1u << 1));
#else
    bool pressed = !(sio_hw->gpio_hi_in & SIO_GPIO_HI_IN_QSPI_CSN_BITS);
#endif
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    restore_interrupts(flags);
    return pressed;
}

static bool button_down(void)
{
    return !gpio_get(BTN_PIN) || !gpio_get(BOOT_BTN_PIN);   /* both active low */
}

typedef enum { PRESS_NONE, PRESS_SHORT, PRESS_LONG } press_t;

static press_t poll_press(bool (*down)(void))
{
    if (!down()) return PRESS_NONE;
    sleep_ms(20);
    if (!down()) return PRESS_NONE;
    uint32_t t0 = to_ms_since_boot(get_absolute_time());
    while (down()) sleep_ms(10);
    uint32_t held = to_ms_since_boot(get_absolute_time()) - t0;
    return held >= tune_long_press_ms ? PRESS_LONG : PRESS_SHORT;
}

static void on_press(press_t p)
{
    if (p == PRESS_SHORT)      { if (menu_open) menu_move(+1); else menu_show(); }
    else if (p == PRESS_LONG)  { if (menu_open) menu_select(); else show_next(); }
}

/* ---- commands (serial + web share this) --------------------------------------- */

static void exec_command(const char *name, const char *value)
{
    int v = atoi(value);
    if      (!strcmp(name, "next"))      show_next();
    else if (!strcmp(name, "show"))      show_pos(v);
    else if (!strcmp(name, "menu")) {
        if      (!strcmp(value, "open"))   { if (!menu_open) menu_show(); }
        else if (!strcmp(value, "close"))  menu_close();
        else if (!strcmp(value, "up"))     menu_move(-1);
        else if (!strcmp(value, "down"))   menu_move(+1);
        else if (!strcmp(value, "select")) { if (menu_open) menu_select(); }
    }
    else if (!strcmp(name, "menusel"))   menu_set(v);
    else if (!strcmp(name, "clear"))     clear_screen(IT8951_MODE_INIT);
    else if (!strcmp(name, "white"))     clear_screen(IT8951_MODE_GC16);
    else if (!strcmp(name, "sleep"))     { menu_close(); it8951_sleep(); }
    else if (!strcmp(name, "init"))      it8951_init();
    else if (!strcmp(name, "enhance"))   it8951_enhance_driving();
    else if (!strcmp(name, "vcom"))      { if (v >= 500 && v <= 5000) it8951_set_vcom((uint16_t)v); else printf("vcom: expected 500..5000 mV\n"); }
    else if (!strcmp(name, "mirror"))    { it8951_set_mirror(v != 0); printf("tune: mirror %d\n", v != 0); repaint(); }
    else if (!strcmp(name, "movemode"))  { tune_move_mode = (v == IT8951_MODE_GC16) ? IT8951_MODE_GC16 : IT8951_MODE_A2; printf("tune: menu move mode %u\n", tune_move_mode); }
    else if (!strcmp(name, "spihz"))     { if (v >= 1000000 && v <= 24000000) { it8951_set_spi_hz((uint32_t)v); printf("tune: SPI %d Hz, re-init\n", v); it8951_init(); } }
    else if (!strcmp(name, "longpress")) { if (v >= 150 && v <= 5000) { tune_long_press_ms = (uint32_t)v; printf("tune: long press %d ms\n", v); } }
    else if (!strcmp(name, "banner"))    { tune_banner = (v != 0); printf("tune: banner %d\n", tune_banner); banner_update(); }
    else if (!strcmp(name, "diag"))      { diag_pins(); it8951_probe_matrix(); it8951_init(); }
    else printf("cmd: unknown \"%s\"\n", name);
}

/* Single-slot queue filled from the lwIP context, drained by the main loop. */
static struct { char name[24]; char value[48]; volatile bool pending; } s_cmd;

void app_command(const char *name, const char *value)
{
    snprintf(s_cmd.name, sizeof s_cmd.name, "%s", name);
    snprintf(s_cmd.value, sizeof s_cmd.value, "%s", value ? value : "");
    __dmb();
    s_cmd.pending = true;
}

static void drain_commands(void)
{
    if (!s_cmd.pending) return;
    char name[24], value[48];
    memcpy(name, s_cmd.name, sizeof name);
    memcpy(value, s_cmd.value, sizeof value);
    s_cmd.pending = false;
    printf("web: %s=%s\n", name, value);
    exec_command(name, value);
}

/* ---- web UI glue --------------------------------------------------------------- */

size_t app_state_json(char *buf, size_t cap)
{
    const it8951_info_t *in = it8951_info();
    size_t n = 0;
#define ADD(...) do { int k = snprintf(buf + n, cap > n ? cap - n : 0, __VA_ARGS__); if (k > 0) n += (size_t)k; } while (0)
    ADD("{\"panel\":{\"w\":%u,\"h\":%u,\"lut\":\"%s\",\"fw\":\"%s\",\"addr\":\"0x%08lx\"},",
        in->w, in->h, in->lut, in->fw, (unsigned long)in->img_addr);
    ADD("\"vcom_mv\":%ld,\"mirror\":%d,\"move_mode\":%u,\"spi_hz\":%lu,\"long_press_ms\":%lu,\"banner\":%d,",
        (long)it8951_cached_vcom(), it8951_mirror() ? 1 : 0, tune_move_mode,
        (unsigned long)it8951_spi_hz(), (unsigned long)tune_long_press_ms, tune_banner ? 1 : 0);
    ADD("\"ip\":\"%s\",\"rssi\":%d,\"psram\":%d,\"busy\":%d,\"paints\":%lu,\"last_stream_ms\":%lu,\"last_refresh_ms\":%lu,\"uptime_s\":%lu,",
        net_ip(), net_rssi(), have_psram ? 1 : 0, s_busy ? 1 : 0, (unsigned long)it8951_paint_count(),
        (unsigned long)it8951_last_stream_ms(), (unsigned long)it8951_last_refresh_ms(),
        (unsigned long)(to_ms_since_boot(get_absolute_time()) / 1000));
    ADD("\"screen\":%d,\"screens\":[", cur_pos);
    for (int i = 0; i < SCREEN_COUNT; i++) ADD("%s\"%s\"", i ? "," : "", slot_name(slot_at(i)));
    ADD("],\"menu_open\":%d,\"menu_sel\":%d,\"menu_items\":[", menu_open ? 1 : 0, menu.selected);
    for (int i = 0; i < MENU_COUNT; i++) ADD("%s\"%s\"", i ? "," : "", menu_items[i]);
    ADD("]}");
#undef ADD
    return n < cap ? n : cap - 1;
}

/* Preview: the composite generator decimated by `scale`. Lives in PSRAM past
 * the JPEG buffers when we have it (any scale), else a small SRAM buffer
 * (scale >= 4). */
#define PREVIEW_SRAM_BYTES ((IT8951_W / 4 / 2) * (IT8951_H / 4))      /* 82 KB */
static uint8_t  preview_sram[PREVIEW_SRAM_BYTES];
static size_t   preview_len;
static uint8_t *preview_buf = preview_sram;

const uint8_t *app_render_preview(int scale, size_t *len)
{
    if (scale == 0) { *len = preview_len; return preview_len ? preview_buf : NULL; }
    if (scale < 2) scale = 2;
    if (scale > 8) scale = 8;
    if (!have_psram && scale < 4) scale = 4;
    preview_buf = have_psram ? PSRAM_XIP_BASE + 4u * 1024 * 1024 : preview_sram;

    int w = (IT8951_W / scale) & ~1, h = IT8951_H / scale;
    uint8_t *dst = preview_buf;
    dst[0] = (uint8_t)w; dst[1] = (uint8_t)(w >> 8); dst[2] = (uint8_t)h; dst[3] = (uint8_t)(h >> 8);
    dst += 4;
    static uint8_t row[IT8951_ROW_BYTES];
    for (int j = 0; j < h; j++) {
        composite_row(j * scale, row, NULL);
        uint8_t *o = dst + (size_t)j * (size_t)(w / 2);
        for (int i = 0; i < w; i += 2) {
            int xa = i * scale, xb = (i + 1) * scale;
            uint8_t ga = (uint8_t)((row[xa >> 1] >> ((xa & 1) ? 0 : 4)) & 0x0F);
            uint8_t gb = (uint8_t)((row[xb >> 1] >> ((xb & 1) ? 0 : 4)) & 0x0F);
            o[i >> 1] = (uint8_t)((ga << 4) | gb);
        }
    }
    preview_len = 4 + (size_t)(w / 2) * (size_t)h;
    *len = preview_len;
    return preview_buf;
}

/* ---- serial ------------------------------------------------------------------- */

static void help(void)
{
    printf("keys: m menu | j/k or arrows move | Enter/space select (menu closed: next) | Esc/q close\n"
           "      n next | 1-%d jump | c clear (INIT) | w white (GC16) | v<mV>Enter set VCOM (e.g. v1500)\n"
           "      V read VCOM | i re-init | z sleep | e enhance driving | d pin discovery+sweep | ? help\n"
           "button (BOOTSEL or GP%d): short = menu/move, long (>%lu ms) = select/next\n",
           SCREEN_COUNT, BTN_PIN, (unsigned long)tune_long_press_ms);
    if (net_connected()) printf("web UI: http://%s/\n", net_ip());
}

static void handle_key(int ch)
{
    static bool collecting_vcom;
    static int  vcom_digits;
    static unsigned vcom_val;

    if (collecting_vcom) {
        if (ch >= '0' && ch <= '9') { vcom_val = vcom_val * 10 + (unsigned)(ch - '0'); vcom_digits++; putchar(ch); return; }
        collecting_vcom = false;
        putchar('\n');
        if (vcom_digits > 0 && vcom_val >= 500 && vcom_val <= 5000) it8951_set_vcom((uint16_t)vcom_val);
        else printf("vcom: expected v<500..5000> (millivolts, e.g. v1500)\n");
        return;
    }
    if (ch == 0x1B) {
        int c2 = getchar_timeout_us(5000);
        if (c2 == '[') {
            int c3 = getchar_timeout_us(5000);
            if (c3 == 'A') menu_move(-1);
            else if (c3 == 'B') menu_move(+1);
            return;
        }
        menu_close();
        return;
    }
    switch (ch) {
    case 'm': if (menu_open) menu_close(); else menu_show(); return;
    case 'j': case '\t': menu_move(+1); return;
    case 'k':            menu_move(-1); return;
    case 'q':            menu_close(); return;
    case '\r': case '\n': case ' ':
        if (menu_open) menu_select(); else show_next();
        return;
    case 'n': show_next(); return;
    case 'c': clear_screen(IT8951_MODE_INIT); return;
    case 'w': clear_screen(IT8951_MODE_GC16); return;
    case 'v': collecting_vcom = true; vcom_digits = 0; vcom_val = 0; printf("vcom: v"); return;
    case 'V': { uint16_t v = it8951_get_vcom(); printf("vcom: -%u.%02uV (%u mV)\n", v / 1000, (v % 1000) / 10, v); return; }
    case 'i': it8951_init(); return;
    case 'z': menu_close(); it8951_sleep(); return;
    case 'e': it8951_enhance_driving(); return;
    case 'd': diag_pins(); it8951_probe_matrix(); it8951_init(); return;
    case '?': case 'h': help(); return;
    default:
        if (ch >= '1' && ch <= '0' + SCREEN_COUNT) show_pos(ch - '1');
        return;
    }
}

/* ---- main --------------------------------------------------------------------- */

int main(void)
{
    stdio_init_all();
    sleep_ms(1500);
    printf("\ned103tc2-pico-driver: Waveshare 10.3\" IT8951 HAT, %d patterns + .bin image + on-device JPEG + web UI\n",
           IT8951_PATTERN_COUNT);

    size_t psram = psram_init(PSRAM_CS_PIN_PLUS2);
    have_psram = (psram > 0);
    printf("psram: %s (%u bytes)\n", have_psram ? "ready" : "none", (unsigned)psram);

    gpio_init(BTN_PIN);      gpio_set_dir(BTN_PIN, GPIO_IN);      gpio_pull_up(BTN_PIN);
    gpio_init(BOOT_BTN_PIN); gpio_set_dir(BOOT_BTN_PIN, GPIO_IN); gpio_pull_up(BOOT_BTN_PIN);

    for (int i = 0; i < SCREEN_COUNT; i++) menu_items[i] = slot_name(slot_at(i));
    menu_items[MENU_CLEAR] = "clear screen (INIT)";
    menu_items[MENU_SLEEP] = "sleep controller";

    if (!it8951_init()) {
        printf("no controller; running the pin discovery (press d to repeat, i to retry init)\n");
        diag_pins();
        it8951_probe_matrix();
    } else {
        printf("paint: INIT clear (deghost)\n");
        it8951_clear(IT8951_MODE_INIT);
    }

    show_pos(0);

    /* WiFi after the first paint so the panel is not blank while we associate. */
    if (net_init()) {
        snprintf(banner_text, sizeof banner_text, "http://%s/", net_ip());
        banner_update();
    }

    help();
    printf("short-press BOOTSEL (or press m) for the menu...\n");

    while (true) {
        press_t p = poll_press(bootsel_down);
        if (p == PRESS_NONE) p = poll_press(button_down);
        if (p != PRESS_NONE) on_press(p);

        int ch = getchar_timeout_us(0);
        if (ch != PICO_ERROR_TIMEOUT) handle_key(ch);

        drain_commands();
        net_poll();
        sleep_ms(10);
    }
}
