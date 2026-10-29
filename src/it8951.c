/*
 * it8951.c: IT8951 driver for the Waveshare 10.3" HAT. See it8951.h.
 *
 * Every host->controller exchange is a 16-bit preamble followed by 16-bit
 * words, MSB first, with the HRDY line checked before the preamble and again
 * before the payload (Waveshare EPD_IT8951.c). Pixel data goes as one long
 * "write data" transaction: preamble 0x0000, then the whole frame with CS held
 * low (packed-write mode, I80CPCR = 1), pausing between rows if HRDY drops.
 */
#include "it8951.h"
#include "epd_io.h"

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"

/* I80 command set */
#define CMD_SYS_RUN       0x0001
#define CMD_STANDBY       0x0002
#define CMD_SLEEP         0x0003
#define CMD_REG_RD        0x0010
#define CMD_REG_WR        0x0011
#define CMD_LD_IMG_AREA   0x0021
#define CMD_LD_IMG_END    0x0022
#define CMD_DPY_AREA      0x0034
#define CMD_GET_DEV_INFO  0x0302
#define CMD_VCOM          0x0039

/* Registers */
#define REG_I80CPCR       0x0004   /* bit 0: packed-write enable              */
#define REG_ENHANCE       0x0038   /* 0x0602 = enhanced driving (Waveshare)   */
#define REG_LISAR         0x0208   /* load-image start address, low word; +2 = high */
#define REG_LUTAFSR       0x1224   /* LUT engine status: non-zero = refreshing */

/* SPI preambles */
#define PRE_CMD   0x6000
#define PRE_DATA  0x0000
#define PRE_READ  0x1000

/* LD_IMG_AREA argument 0: endian << 8 | bpp << 4 | rotate */
#define LDIMG_L_ENDIAN  0
#define LDIMG_4BPP      2
#define LDIMG_ROT0      0

static it8951_info_t s_info;
static bool     s_up;                   /* identified and in SYS_RUN           */
static bool     s_asleep;
static uint32_t s_hrdy_timeout = 5000;  /* ms; shortened while probing pins    */
static int32_t  s_vcom_mv = -1;         /* run-time override, -1 = none        */
static bool     s_mirror  = (EPD_MIRROR_H != 0);
static uint32_t s_spi_hz  = EPD_SPI_HZ;
static uint32_t s_paints, s_last_stream, s_last_refresh;

void     it8951_set_mirror(bool m)     { s_mirror = m; }
bool     it8951_mirror(void)           { return s_mirror; }
void     it8951_set_spi_hz(uint32_t hz){ s_spi_hz = hz; }
uint32_t it8951_spi_hz(void)           { return s_spi_hz; }
uint32_t it8951_paint_count(void)      { return s_paints; }
uint32_t it8951_last_stream_ms(void)   { return s_last_stream; }
uint32_t it8951_last_refresh_ms(void)  { return s_last_refresh; }

static inline uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

/* ---- word-level transport ------------------------------------------------ */

static bool s_blind;                    /* ignore HRDY, pace with fixed delays */

static unsigned s_stalls;               /* times HRDY was found low (controller reacted) */

static bool hrdy(void)
{
    if (s_blind) { sleep_us(200); return true; }
    if (!epd_hrdy()) s_stalls++;
    if (epd_wait_hrdy(s_hrdy_timeout)) return true;
    printf("it8951: HRDY timeout (GP%d stayed low %lu ms)\n", EPD_PIN_HRDY,
           (unsigned long)s_hrdy_timeout);
    return false;
}

static void write_cmd(uint16_t cmd)
{
    hrdy(); epd_cs(true);
    epd_spi_write16(PRE_CMD);
    hrdy();
    epd_spi_write16(cmd);
    epd_cs(false);
}

static void write_data(uint16_t d)
{
    hrdy(); epd_cs(true);
    epd_spi_write16(PRE_DATA);
    hrdy();
    epd_spi_write16(d);
    epd_cs(false);
}

static void read_words(uint16_t *buf, size_t n)
{
    hrdy(); epd_cs(true);
    epd_spi_write16(PRE_READ);
    hrdy();
    (void)epd_spi_read16();            /* dummy word */
    hrdy();
    for (size_t i = 0; i < n; i++) buf[i] = epd_spi_read16();
    epd_cs(false);
}

static uint16_t read_word(void)
{
    uint16_t v;
    read_words(&v, 1);
    return v;
}

static void write_cmd_args(uint16_t cmd, const uint16_t *args, int n)
{
    write_cmd(cmd);
    for (int i = 0; i < n; i++) write_data(args[i]);
}

static uint16_t read_reg(uint16_t reg)
{
    write_cmd(CMD_REG_RD);
    write_data(reg);
    return read_word();
}

static void write_reg(uint16_t reg, uint16_t val)
{
    write_cmd(CMD_REG_WR);
    write_data(reg);
    write_data(val);
}

/* ---- identification / init ---------------------------------------------- */

/* GET_DEV_INFO returns 20 words: W, H, addr_L, addr_H, FW[8], LUT[8]. The two
 * strings arrive byte-swapped within each word (low byte first). */
static uint16_t s_raw_info[20];         /* last GET_DEV_INFO words, for diagnostics */

static bool get_dev_info(it8951_info_t *out)
{
    uint16_t *w = s_raw_info;
    write_cmd(CMD_GET_DEV_INFO);
    if (s_blind) sleep_ms(5);
    read_words(w, 20);

    memset(out, 0, sizeof *out);
    out->w = w[0];
    out->h = w[1];
    out->img_addr = (uint32_t)w[2] | ((uint32_t)w[3] << 16);
    for (int i = 0; i < 8; i++) {
        out->fw[2 * i]      = (char)(w[4 + i] & 0xFF);
        out->fw[2 * i + 1]  = (char)(w[4 + i] >> 8);
        out->lut[2 * i]     = (char)(w[12 + i] & 0xFF);
        out->lut[2 * i + 1] = (char)(w[12 + i] >> 8);
    }
    bool clean = out->lut[0] != 0 && out->fw[0] != 0;   /* bit errors garble these first */
    for (int i = 0; i < 16; i++) {       /* keep the log printable */
        if (out->fw[i]  && (out->fw[i]  < 0x20 || out->fw[i]  > 0x7E)) { out->fw[i]  = '?'; clean = false; }
        if (out->lut[i] && (out->lut[i] < 0x20 || out->lut[i] > 0x7E)) { out->lut[i] = '?'; clean = false; }
    }
    return clean && out->w >= 16 && out->w <= 4096 && out->h >= 16 && out->h <= 4096 &&
           out->img_addr != 0 && out->img_addr != 0xFFFFFFFFu;
}

static bool bring_up(uint cs, uint miso, uint32_t hz, int mode)
{
    epd_io_set_mode(mode);
    if (!epd_io_init(cs, miso, hz)) {
        printf("it8951: no free PIO state machine\n");
        return false;
    }
    epd_reset();
    s_hrdy_timeout = 1000;
    /* The IT8951 boots its firmware after reset and can hold HRDY low for
     * ~2 s before it will take commands. */
    uint32_t t0 = now_ms();
    if (!epd_wait_hrdy(4000))
        printf("it8951: HRDY (GP%d) still low 4 s after reset\n", EPD_PIN_HRDY);
    else
        printf("it8951: HRDY high %lu ms after reset\n", (unsigned long)(now_ms() - t0));
    write_cmd(CMD_SYS_RUN);

    it8951_info_t info;
    bool ok = get_dev_info(&info);
    if (ok) {
        /* Bit errors at too high an SPI clock still give plausible numbers
         * (18.75 MHz read 2040x1406 here), so insist on three identical reads. */
        for (int k = 0; k < 2 && ok; k++) {
            it8951_info_t again;
            if (!get_dev_info(&again) || memcmp(&again, &info, sizeof info) != 0) {
                printf("it8951: device info unstable at %lu Hz (%ux%u vs %ux%u) -- SPI too fast for this wiring\n",
                       (unsigned long)hz, info.w, info.h, again.w, again.h);
                ok = false;
            }
        }
    }
    s_hrdy_timeout = 5000;
    if (!ok) {
        printf("it8951: no valid device info with CS=GP%u MISO=GP%u mode %d @%lu Hz: words %04x %04x %04x %04x %04x %04x\n",
               cs, miso, mode, (unsigned long)hz, s_raw_info[0], s_raw_info[1], s_raw_info[2],
               s_raw_info[3], s_raw_info[4], s_raw_info[5]);
        epd_io_deinit();
        return false;
    }
    s_info = info;
    return true;
}

static int32_t s_vcom_cached = -1;

uint16_t it8951_get_vcom(void)
{
    write_cmd(CMD_VCOM);
    write_data(0x0000);
    uint16_t v = read_word();
    s_vcom_cached = v;
    return v;
}

int32_t it8951_cached_vcom(void) { return s_vcom_cached; }

void it8951_set_vcom(uint16_t mv)
{
    s_vcom_mv = mv;
    write_cmd(CMD_VCOM);
    write_data(0x0001);
    write_data(mv);
    uint16_t rb = it8951_get_vcom();
    printf("it8951: VCOM set to -%u.%02uV (readback %u mV)\n", mv / 1000, (mv % 1000) / 10, rb);
}

static void apply_vcom(void)
{
    uint16_t cur = it8951_get_vcom();
    int32_t want = s_vcom_mv;
#ifdef EPD_VCOM_MV
    if (want < 0) want = EPD_VCOM_MV;
#endif
    if (want < 0) {
        printf("it8951: VCOM = -%u.%02uV (controller's own value)\n", cur / 1000, (cur % 1000) / 10);
        printf("it8951: *** EPD_VCOM_MV is not set. Read the voltage printed on the panel's FPC\n"
               "it8951: *** cable and build with -DEPD_VCOM_MV=<mV> (-1.50V -> 1500), or type\n"
               "it8951: *** v1500<Enter> here. Running with the wrong VCOM degrades the panel. ***\n");
        return;
    }
    if (cur != (uint16_t)want) it8951_set_vcom((uint16_t)want);
    else printf("it8951: VCOM = -%u.%02uV\n", cur / 1000, (cur % 1000) / 10);
}

bool it8951_init(void)
{
    /* Try the configured pins at full speed in mode 0 first, then slower, then
     * mode 3, then the CS/MISO pins swapped. */
    struct { uint8_t cs, miso; uint32_t hz; int mode; } cand[] = {
        { EPD_PIN_CS, EPD_PIN_MISO, s_spi_hz,   0 },
        { EPD_PIN_CS, EPD_PIN_MISO, 9375000,    0 },
        { EPD_PIN_CS, EPD_PIN_MISO, 1000000,    0 },
        { EPD_PIN_CS, EPD_PIN_MISO, 1000000,    3 },
        { EPD_PIN_MISO, EPD_PIN_CS, 1000000,    0 },
        { EPD_PIN_MISO, EPD_PIN_CS, 1000000,    3 },
    };
    int n = (EPD_PIN_PROBE && EPD_PIN_CS != EPD_PIN_MISO) ? 6 : 4;

    s_up = false;
    s_asleep = false;
    for (int i = 0; i < n && !s_up; i++) {
        printf("it8951: trying CS=GP%u MISO=GP%u mode %d @%lu Hz\n", cand[i].cs, cand[i].miso,
               cand[i].mode, (unsigned long)cand[i].hz);
        s_up = bring_up(cand[i].cs, cand[i].miso, cand[i].hz, cand[i].mode);
    }
    if (!s_up) {
        printf("it8951: controller not found. Check 5V to the HAT, the FPC, RST on GP%d and HRDY on GP%d,\n"
               "it8951: and that SCLK/MOSI really are GP%d/GP%d on your adapter (see epd_io.h).\n",
               EPD_PIN_RST, EPD_PIN_HRDY, EPD_PIN_SCLK, EPD_PIN_MOSI);
        return false;
    }

    printf("it8951: found on CS=GP%u MISO=GP%u (mode %d): panel %ux%u, image buffer 0x%08lx, FW \"%s\", LUT \"%s\"\n",
           epd_io_cs_pin(), epd_io_miso_pin(), epd_io_mode(), s_info.w, s_info.h,
           (unsigned long)s_info.img_addr, s_info.fw, s_info.lut);
    if (s_info.w != IT8951_W || s_info.h != IT8951_H)
        printf("it8951: WARNING: this build is laid out for %dx%d; the panel reports %ux%u\n",
               IT8951_W, IT8951_H, s_info.w, s_info.h);

    write_reg(REG_I80CPCR, 0x0001);      /* packed (bulk) data writes */
    apply_vcom();
    return true;
}

const it8951_info_t *it8951_info(void)
{
    return &s_info;
}

void it8951_probe_matrix(void)
{
    static const uint8_t cs_c[]   = { 9, 15, 8, 12 };
    static const uint8_t miso_c[] = { 8, 12, 9, 15, 0, 28 };
    printf("it8951: probe matrix using HRDY=GP%d (300 ms waits), RST=GP%d\n", EPD_PIN_HRDY, EPD_PIN_RST);
    int found = 0;
    for (int swap = 0; swap < 2 && !found; swap++)
    for (int m = 0; m < 2 && !found; m++)
    for (unsigned i = 0; i < sizeof cs_c && !found; i++)
    for (unsigned j = 0; j < sizeof miso_c && !found; j++) {
        if (cs_c[i] == miso_c[j]) continue;
        int mode = m ? 3 : 0;
        epd_io_set_swap(swap != 0);
        epd_io_set_mode(mode);
        if (!epd_io_init(cs_c[i], miso_c[j], 1000000)) continue;
        epd_reset();
        epd_wait_hrdy(2000);
        s_hrdy_timeout = 300;
        s_stalls = 0;
        write_cmd(CMD_SYS_RUN);
        it8951_info_t info;
        bool ok = get_dev_info(&info);
        unsigned stalls = s_stalls;
        bool quiet = true;
        for (int k = 0; k < 6; k++) if (s_raw_info[k] != 0x0000 && s_raw_info[k] != 0xFFFF) quiet = false;
        printf("it8951:   %s SCK=GP%u MOSI=GP%u CS=GP%u MISO=GP%u mode %d -> HRDY dips %u, words %04x %04x %04x %04x %04x %04x\n",
               ok ? "VALID" : quiet ? "quiet" : "NOISE",
               swap ? EPD_PIN_MOSI : EPD_PIN_SCLK, swap ? EPD_PIN_SCLK : EPD_PIN_MOSI,
               cs_c[i], miso_c[j], mode, stalls,
               s_raw_info[0], s_raw_info[1], s_raw_info[2], s_raw_info[3], s_raw_info[4], s_raw_info[5]);
        if (ok) {
            printf("it8951:   >>> panel %ux%u addr 0x%08lx FW \"%s\" LUT \"%s\"  -- build with -DEPD_PIN_CS=%u -DEPD_PIN_MISO=%u%s%s\n",
                   info.w, info.h, (unsigned long)info.img_addr, info.fw, info.lut, cs_c[i], miso_c[j],
                   swap ? " (and swap SCLK/MOSI)" : "", mode ? " (mode 3)" : "");
            found++;
        }
        s_hrdy_timeout = 5000;
        epd_io_deinit();
    }
    epd_io_set_swap(false);
    epd_io_set_mode(0);
    if (!found) printf("it8951:   no pairing produced device info\n");
}

bool it8951_probe_blind(unsigned cs, unsigned miso, int mode, it8951_info_t *out, uint16_t raw[4])
{
    epd_io_set_mode(mode);
    if (!epd_io_init(cs, miso, 1000000)) return false;   /* slow, forgiving */
    s_blind = true;
    epd_reset();
    sleep_ms(2500);                      /* IT8951 firmware boot */
    write_cmd(CMD_SYS_RUN);
    sleep_ms(20);
    bool ok = get_dev_info(out);
    s_blind = false;
    for (int i = 0; i < 4; i++) raw[i] = s_raw_info[i];
    epd_io_deinit();
    return ok;
}

static bool ensure_up(void)
{
    if (s_up && !s_asleep) return true;
    printf("it8951: controller %s, re-initialising\n", s_asleep ? "asleep" : "not up");
    return it8951_init();
}

/* ---- image load + refresh ------------------------------------------------ */

static void wait_display_ready(uint32_t timeout_ms)
{
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (read_reg(REG_LUTAFSR) != 0) {
        if (time_reached(deadline)) {
            printf("it8951: LUT engine still busy after %lu ms\n", (unsigned long)timeout_ms);
            return;
        }
        sleep_ms(10);
    }
}

static inline uint8_t swapnib(uint8_t b) { return (uint8_t)((b << 4) | (b >> 4)); }

/* Logical packed span (nbytes, even) -> the byte order the IT8951 sees.
 * Waveshare's 10.3" path (a) mirrors the row (Paint MIRROR_HORIZONTAL,
 * "epd_mode 1") and (b) sends each 16-bit word high byte first from a
 * little-endian buffer, i.e. swaps byte pairs. Mirroring a packed span =
 * reverse the bytes and swap the nibbles in each. */
static void row_to_wire(const uint8_t *row, int nbytes, uint8_t *wire)
{
    if (s_mirror) {
        for (int k = 0; k < nbytes; k += 2) {
            wire[k]     = swapnib(row[nbytes - 2 - k]);
            wire[k + 1] = swapnib(row[nbytes - 1 - k]);
        }
    } else {
        for (int k = 0; k < nbytes; k += 2) {
            wire[k]     = row[k + 1];
            wire[k + 1] = row[k];
        }
    }
}

/* Open a load into the image buffer for panel-space rectangle (px, y, w, h)
 * and start the single long "write data" transaction. */
static void load_begin(int px, int y, int w, int h)
{
    wait_display_ready(30000);
    write_reg(REG_LISAR + 2, (uint16_t)(s_info.img_addr >> 16));
    write_reg(REG_LISAR,     (uint16_t)(s_info.img_addr & 0xFFFF));
    const uint16_t args[5] = {
        (LDIMG_L_ENDIAN << 8) | (LDIMG_4BPP << 4) | LDIMG_ROT0,
        (uint16_t)px, (uint16_t)y, (uint16_t)w, (uint16_t)h,
    };
    write_cmd_args(CMD_LD_IMG_AREA, args, 5);

    hrdy(); epd_cs(true);
    epd_spi_write16(PRE_DATA);
    hrdy();
}

/* Two wire buffers: row N goes out by DMA while row N+1 is being built. */
static uint8_t s_wire[2][IT8951_ROW_BYTES];

static void load_row(const uint8_t *span, int nbytes, int slot)
{
    uint8_t *wire = s_wire[slot & 1];
    row_to_wire(span, nbytes, wire);
    epd_spi_write_wait();                /* previous row fully clocked out */
    if (!epd_hrdy()) hrdy();             /* controller is pacing us */
    epd_spi_write_async(wire, (size_t)nbytes);
}

static void load_end(void)
{
    epd_spi_write_wait();
    epd_cs(false);
    write_cmd(CMD_LD_IMG_END);
}

static void display(int px, int y, int w, int h, uint8_t mode)
{
    const uint16_t args[5] = { (uint16_t)px, (uint16_t)y, (uint16_t)w, (uint16_t)h, mode };
    write_cmd_args(CMD_DPY_AREA, args, 5);
    sleep_ms(20);                        /* let the LUT engine pick the job up */
    wait_display_ready(30000);
}

/* Stream rows y0..y1-1, logical columns x0..x0+w-1 (x0 and w multiples of 16),
 * then refresh that rectangle with `mode`. */
static void paint(int x, int y, int w, int h, it8951_row_fn fn, void *ctx, uint8_t mode)
{
    if (!ensure_up()) return;

    int x0 = x & ~15, x1 = (x + w + 15) & ~15;
    if (x0 < 0) x0 = 0;
    if (x1 > IT8951_W) x1 = IT8951_W;
    int y0 = y < 0 ? 0 : y, y1 = (y + h > IT8951_H) ? IT8951_H : y + h;
    if (x1 <= x0 || y1 <= y0) return;
    const int aw = x1 - x0, ah = y1 - y0;
    const int px = s_mirror ? (IT8951_W - x0 - aw) : x0;        /* panel-space x */

    uint8_t row[IT8951_ROW_BYTES];
    uint32_t t0 = now_ms();
    load_begin(px, y0, aw, ah);
    for (int yy = y0; yy < y1; yy++) {
        fn(yy, row, ctx);
        load_row(row + x0 / 2, aw / 2, yy);
    }
    load_end();
    uint32_t t1 = now_ms();
    display(px, y0, aw, ah, mode);
    uint32_t t2 = now_ms();
    s_last_stream = t1 - t0; s_last_refresh = t2 - t1; s_paints++;
    printf("it8951: %dx%d@%d,%d streamed in %lu ms, refreshed (mode %u) in %lu ms\n",
           aw, ah, x0, y0, (unsigned long)(t1 - t0), mode, (unsigned long)(t2 - t1));
}

void it8951_show_pattern(it8951_row_fn fn, void *ctx, uint8_t mode)
{
    paint(0, 0, IT8951_W, IT8951_H, fn, ctx, mode);
}

void it8951_update_area(int x, int y, int w, int h, it8951_row_fn fn, void *ctx, uint8_t mode)
{
    paint(x, y, w, h, fn, ctx, mode);
}

static void frame_row(int y, uint8_t out[IT8951_ROW_BYTES], void *ctx)
{
    memcpy(out, (const uint8_t *)ctx + (size_t)y * IT8951_ROW_BYTES, IT8951_ROW_BYTES);
}

void it8951_show_frame(const uint8_t *frame, uint8_t mode)
{
    it8951_show_pattern(frame_row, (void *)frame, mode);
}

static void white_row(int y, uint8_t out[IT8951_ROW_BYTES], void *ctx)
{
    (void)y; (void)ctx;
    memset(out, 0xFF, IT8951_ROW_BYTES);
}

void it8951_clear(uint8_t mode)
{
    it8951_show_pattern(white_row, NULL, mode);
}

void it8951_sleep(void)
{
    if (!s_up) return;
    write_cmd(CMD_SLEEP);
    s_asleep = true;
    printf("it8951: sleeping (next paint re-inits)\n");
}

void it8951_enhance_driving(void)
{
    if (!ensure_up()) return;
    uint16_t before = read_reg(REG_ENHANCE);
    write_reg(REG_ENHANCE, 0x0602);
    printf("it8951: enhanced driving: reg 0x0038 0x%04x -> 0x%04x\n", before, read_reg(REG_ENHANCE));
}
