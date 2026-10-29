/*
 * epd_io.c: PIO-SPI + GPIO transport for the IT8951 HAT. See epd_io.h.
 *
 * The SPI engine is the spi_cpha0 program from pico-examples (pio/spi/spi.pio,
 * BSD-3-Clause), pre-assembled here so the project has no .pio build step:
 *
 *   .program spi_cpha0
 *   .side_set 1                 ; SCK is the side-set pin
 *       out pins, 1 side 0 [1]  ; MOSI = OUT pin 0, stalls (SCK low) on empty
 *       in  pins, 1 side 1 [1]  ; MISO = IN  pin 0
 *
 * One SCK period is 4 PIO cycles, so clkdiv = sys_clk / (4 * spi_hz).
 */
#include "epd_io.h"

#include <stdio.h>
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/dma.h"

static const uint16_t spi_cpha0_insns[] = { 0x6101, 0x5101 };
static const struct pio_program spi_cpha0_program = {
    .instructions = spi_cpha0_insns,
    .length       = 2,
    .origin       = -1,
};

/* spi_cpha1 from the same file (data changes on the leading edge, sampled on
 * the trailing edge):
 *       out x, 1    side 0
 *       mov pins, x side 1 [1]
 *       in  pins, 1 side 0                      */
static const uint16_t spi_cpha1_insns[] = { 0x6021, 0xB101, 0x4001 };
static const struct pio_program spi_cpha1_program = {
    .instructions = spi_cpha1_insns,
    .length       = 3,
    .origin       = -1,
};

static int s_mode;                       /* 0 or 3 */
static const struct pio_program *s_prog = &spi_cpha0_program;

void epd_io_set_mode(int mode) { s_mode = (mode == 3) ? 3 : 0; }
int  epd_io_mode(void)         { return s_mode; }

static bool s_swap;
static uint s_sclk = EPD_PIN_SCLK, s_mosi = EPD_PIN_MOSI;
void epd_io_set_swap(bool swap) { s_swap = swap; }
bool epd_io_swap(void)          { return s_swap; }

static PIO  s_pio = pio0;
static int  s_sm  = -1;
static uint s_offset;
static uint s_cs, s_miso;
static bool s_ready;

bool epd_io_init(uint cs_pin, uint miso_pin, uint32_t spi_hz)
{
    if (s_ready) epd_io_deinit();
    s_cs = cs_pin;
    s_miso = miso_pin;

    s_sclk = s_swap ? EPD_PIN_MOSI : EPD_PIN_SCLK;
    s_mosi = s_swap ? EPD_PIN_SCLK : EPD_PIN_MOSI;
    s_prog = (s_mode == 3) ? &spi_cpha1_program : &spi_cpha0_program;
    if (!pio_can_add_program(s_pio, s_prog)) return false;
    s_sm = pio_claim_unused_sm(s_pio, false);
    if (s_sm < 0) return false;
    s_offset = pio_add_program(s_pio, s_prog);

    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, s_offset, s_offset + s_prog->length - 1);
    sm_config_set_sideset(&c, 1, false, false);
    sm_config_set_out_pins(&c, s_mosi, 1);
    sm_config_set_in_pins(&c, miso_pin);
    sm_config_set_sideset_pins(&c, s_sclk);
    sm_config_set_out_shift(&c, false, true, 8);   /* MSB first, autopull at 8 */
    sm_config_set_in_shift(&c, false, true, 8);    /* MSB first, autopush at 8 */
    /* Integer divider only: a fractional one jitters SCK half-periods down to
     * 1-2 sys cycles, below the IT8951's minimum. */
    uint32_t idiv = (clock_get_hz(clk_sys) + 4 * spi_hz - 1) / (4 * spi_hz);
    if (idiv < 1) idiv = 1;
    float div = (float)idiv;
    sm_config_set_clkdiv_int_frac(&c, (uint16_t)idiv, 0);

    const uint32_t outs = (1u << s_sclk) | (1u << s_mosi);
    pio_sm_set_pins_with_mask(s_pio, s_sm, 0, outs);                    /* SCK, MOSI low */
    pio_sm_set_pindirs_with_mask(s_pio, s_sm, outs, outs | (1u << miso_pin));
    pio_gpio_init(s_pio, s_mosi);
    pio_gpio_init(s_pio, miso_pin);
    pio_gpio_init(s_pio, s_sclk);
    /* CPOL=1 by inverting the SCK output (pico-examples' trick). */
    gpio_set_outover(s_sclk, s_mode == 3 ? GPIO_OVERRIDE_INVERT : GPIO_OVERRIDE_NORMAL);
    gpio_pull_down(miso_pin);            /* an unconnected MISO reads 0, not noise */
    hw_set_bits(&s_pio->input_sync_bypass, 1u << miso_pin);   /* synchronous: skip the input sync */

    pio_sm_init(s_pio, s_sm, s_offset, &c);
    pio_sm_set_enabled(s_pio, s_sm, true);

    gpio_init(cs_pin);  gpio_put(cs_pin, 1);       gpio_set_dir(cs_pin, GPIO_OUT);
    gpio_init(EPD_PIN_RST); gpio_put(EPD_PIN_RST, 1); gpio_set_dir(EPD_PIN_RST, GPIO_OUT);
    gpio_init(EPD_PIN_HRDY);
    gpio_set_dir(EPD_PIN_HRDY, GPIO_IN);
    gpio_pull_down(EPD_PIN_HRDY);        /* unconnected HRDY reads "busy" -> visible timeout */

    s_ready = true;
    printf("epd_io: PIO SPI mode %d, %lu Hz (clkdiv %u) SCLK=GP%u MOSI=GP%u MISO=GP%u CS=GP%u RST=GP%d HRDY=GP%d\n",
           s_mode, (unsigned long)(clock_get_hz(clk_sys) / (4 * idiv)), (unsigned)div, s_sclk,
           s_mosi, miso_pin, cs_pin, EPD_PIN_RST, EPD_PIN_HRDY);
    return true;
}

void epd_io_deinit(void)
{
    if (!s_ready) return;
    pio_sm_set_enabled(s_pio, s_sm, false);
    pio_sm_unclaim(s_pio, s_sm);
    pio_remove_program(s_pio, s_prog, s_offset);
    gpio_set_outover(s_sclk, GPIO_OVERRIDE_NORMAL);
    hw_clear_bits(&s_pio->input_sync_bypass, 1u << s_miso);
    gpio_init(s_miso);                   /* plain input again */
    gpio_disable_pulls(s_miso);
    gpio_init(s_cs);                     /* keep the old CS parked high, not floating */
    gpio_put(s_cs, 1);
    gpio_set_dir(s_cs, GPIO_OUT);
    s_sm = -1;
    s_ready = false;
}

uint epd_io_cs_pin(void)   { return s_cs; }
uint epd_io_miso_pin(void) { return s_miso; }

void epd_reset(void)
{
    gpio_put(EPD_PIN_RST, 1); sleep_ms(200);
    gpio_put(EPD_PIN_RST, 0); sleep_ms(10);
    gpio_put(EPD_PIN_RST, 1); sleep_ms(200);
}

bool epd_hrdy(void)
{
    return gpio_get(EPD_PIN_HRDY) != 0;
}

bool epd_wait_hrdy(uint32_t timeout_ms)
{
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (!gpio_get(EPD_PIN_HRDY)) {
        if (time_reached(deadline)) return false;
        tight_loop_contents();
    }
    return true;
}

void epd_cs(bool assert)
{
    gpio_put(s_cs, assert ? 0 : 1);
}

/* 8-bit FIFO accesses: a narrow write is byte-replicated across the 32-bit
 * word, which left-justifies the data for MSB-first shift-out; a narrow read
 * picks the low byte, where the 8-bit left-shifted ISR lands. Counting RX as
 * well as TX guarantees every bit has been clocked before we return. */
void __time_critical_func(epd_spi_write)(const uint8_t *src, size_t len)
{
    io_rw_8 *txfifo = (io_rw_8 *)&s_pio->txf[s_sm];
    io_rw_8 *rxfifo = (io_rw_8 *)&s_pio->rxf[s_sm];
    size_t tx = len, rx = len;
    while (tx || rx) {
        if (tx && !pio_sm_is_tx_fifo_full(s_pio, s_sm)) { *txfifo = *src++; --tx; }
        if (rx && !pio_sm_is_rx_fifo_empty(s_pio, s_sm)) { (void)*rxfifo; --rx; }
    }
}

void __time_critical_func(epd_spi_read)(uint8_t *dst, size_t len)
{
    io_rw_8 *txfifo = (io_rw_8 *)&s_pio->txf[s_sm];
    io_rw_8 *rxfifo = (io_rw_8 *)&s_pio->rxf[s_sm];
    size_t tx = len, rx = len;
    while (tx || rx) {
        if (tx && !pio_sm_is_tx_fifo_full(s_pio, s_sm)) { *txfifo = 0; --tx; }
        if (rx && !pio_sm_is_rx_fifo_empty(s_pio, s_sm)) { *dst++ = *rxfifo; --rx; }
    }
}

void epd_spi_write16(uint16_t v)
{
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    epd_spi_write(b, 2);
}

uint16_t epd_spi_read16(void)
{
    uint8_t b[2];
    epd_spi_read(b, 2);
    return (uint16_t)((b[0] << 8) | b[1]);
}

/* ---- DMA write path ------------------------------------------------------ */

static int     s_dma_tx = -1, s_dma_rx = -1;
static uint8_t s_rx_sink;

void epd_spi_write_async(const uint8_t *src, size_t len)
{
    if (s_dma_tx < 0) {
        s_dma_tx = dma_claim_unused_channel(true);
        s_dma_rx = dma_claim_unused_channel(true);
    }
    /* RX drain first, so the FIFO can never fill and stall the state machine. */
    dma_channel_config c = dma_channel_get_default_config(s_dma_rx);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(s_pio, (uint)s_sm, false));
    dma_channel_configure(s_dma_rx, &c, &s_rx_sink, (const io_rw_8 *)&s_pio->rxf[s_sm], len, true);

    c = dma_channel_get_default_config(s_dma_tx);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);   /* byte writes replicate -> left-justified */
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(s_pio, (uint)s_sm, true));
    dma_channel_configure(s_dma_tx, &c, (io_rw_8 *)&s_pio->txf[s_sm], src, len, true);
}

void epd_spi_write_wait(void)
{
    if (s_dma_tx < 0) return;
    dma_channel_wait_for_finish_blocking(s_dma_tx);
    dma_channel_wait_for_finish_blocking(s_dma_rx);   /* every bit clocked in = clocked out */
}
