/*
 * epd_io.h: low-level transport for the Waveshare 10.3" e-Paper HAT (IT8951).
 *
 * The HAT speaks SPI mode 0 and, unlike the Inky, needs MISO (device info,
 * register reads, VCOM readback) plus an HRDY handshake line. Control lines on
 * the Hard Stuff "Pico to Pi Hat", as found by this firmware's pin discovery
 * and probe sweep on real hardware (2026-10-29):
 *
 *   Pi SCLK (BCM11) -> GP10 (SPI1 SCK)    Pi MOSI (BCM10) -> GP11 (SPI1 TX)
 *   Pi MISO (BCM9)  -> GP12 (SPI1 RX)     Pi CE0  (BCM8)  -> GP8
 *   RST     (BCM17) -> GP17               HRDY    (BCM24) -> GP1
 *
 * So the adapter is mostly 1:1 BCM-to-GP but lays the Pi's SPI0 onto the
 * Pico's SPI1 hardware pins (hence MISO on GP12, not GP9), and reroutes
 * BCM23-25 because GP23/24/25/29 are the Pico Plus 2 W's wireless-module pins
 * (HRDY lands on GP1). SPI is bit-clocked by a PIO state machine, which works
 * on any GPIO, so none of this matters to the code; override any pin with
 * -DEPD_PIN_xxx=N in platformio.ini. If the configured pins do not answer,
 * it8951_init() falls back to a sweep and prints the pairing that does.
 *
 * HRDY is HIGH when the controller is ready and LOW while it is busy.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pico/stdlib.h"

#ifndef EPD_PIN_SCLK
#define EPD_PIN_SCLK  10      /* adapter routes Pi SCLK (BCM11) here        */
#endif
#ifndef EPD_PIN_MOSI
#define EPD_PIN_MOSI  11      /* adapter routes Pi MOSI (BCM10) here        */
#endif
#ifndef EPD_PIN_MISO
#define EPD_PIN_MISO  12      /* Pi MISO (BCM9) -> GP12 (SPI1 RX); verified  */
#endif
#ifndef EPD_PIN_CS
#define EPD_PIN_CS    8       /* Pi CE0 (BCM8) -> GP8; verified              */
#endif
#ifndef EPD_PIN_RST
#define EPD_PIN_RST   17      /* active low                                 */
#endif
#ifndef EPD_PIN_HRDY
#define EPD_PIN_HRDY  1       /* input; HIGH = ready, LOW = busy. Pi BCM24 lands
                                 on GP1: GP23-25/29 are the Pico Plus 2 W's
                                 wireless pins, so the adapter reroutes them.
                                 Found with the firmware's pin discovery.      */
#endif
#ifndef EPD_SPI_HZ
#define EPD_SPI_HZ    12500000  /* sys_clk 150 MHz / (4 * 3). The PIO divider is
                                   rounded UP to an integer (fractional ones
                                   jitter SCK below the IT8951's minimum), so
                                   the steps are 18.75 / 12.5 / 9.375 / 7.5 MHz.
                                   18.75 MHz read back corrupted device info
                                   through the adapter; 12.5 MHz is clean. */
#endif

/* Bring up the PIO SPI on SCLK/MOSI/miso_pin plus the CS/RST/HRDY GPIOs. CS
 * and MISO are parameters so the controller can be probed on either pairing.
 * Returns false if no PIO state machine is free. Safe to call again with
 * different pins (tears down the previous instance first). */
bool epd_io_init(uint cs_pin, uint miso_pin, uint32_t spi_hz);

/* SPI mode for the next epd_io_init(): 0 (CPOL=0,CPHA=0, Waveshare's setting)
 * or 3 (CPOL=1,CPHA=1). Diagnostic knob; default 0. */
void epd_io_set_mode(int mode);
int  epd_io_mode(void);

/* Diagnostic: swap the SCLK and MOSI pin numbers for the next epd_io_init(). */
void epd_io_set_swap(bool swap);
bool epd_io_swap(void);

/* Release the PIO state machine; CS is left deasserted (high). */
void epd_io_deinit(void);

uint epd_io_cs_pin(void);
uint epd_io_miso_pin(void);

/* Hardware reset, Waveshare timing: 200 ms high, 10 ms low, 200 ms high. */
void epd_reset(void);

/* HRDY level: true = ready. */
bool epd_hrdy(void);

/* Block until HRDY is high; false on timeout. */
bool epd_wait_hrdy(uint32_t timeout_ms);

/* Chip select (active low): assert = true pulls CS low. */
void epd_cs(bool assert);

/* Raw full-duplex transfers, MSB first, mode 0. Return once every bit has
 * been clocked, so CS may be released immediately afterwards. */
void     epd_spi_write(const uint8_t *src, size_t len);
void     epd_spi_read(uint8_t *dst, size_t len);
void     epd_spi_write16(uint16_t v);     /* big-endian on the wire */
uint16_t epd_spi_read16(void);

/* DMA-driven write: returns immediately, `src` must stay valid until
 * epd_spi_write_wait() returns. Lets the caller build the next row while this
 * one is on the wire. A second DMA channel drains the RX FIFO so the PIO never
 * stalls on it. */
void epd_spi_write_async(const uint8_t *src, size_t len);
void epd_spi_write_wait(void);
