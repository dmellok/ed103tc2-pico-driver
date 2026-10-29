# Third-party notices

This project bundles third-party source:

- **picojpeg** (`src/vendor/picojpeg.c`, `include/picojpeg.h`) — a baseline JPEG
  decoder by Rich Geldreich, released into the **public domain**. Used for the
  on-device JPEG decode. https://github.com/richgel999/picojpeg

This project bundles third-party media:

- **`src/sample.jpg`** — "Tulip - floriade canberra" by John O'Neill, licensed
  **CC BY-SA 3.0**, via Wikimedia Commons. Cropped to 1872x1404 for the
  on-device JPEG demo; replace it with your own image to remove this dependency.
  https://commons.wikimedia.org/wiki/File:Tulip_-_floriade_canberra.jpg

Ported code:

- The IT8951 command/register sequence, preambles, init, VCOM handling and the
  load-image / display-area flow in `src/it8951.c` are ported from Waveshare's
  `IT8951-ePaper` Raspberry Pi demo (`Raspberry/lib/e-Paper/EPD_IT8951.c`),
  **MIT** licence, Copyright (c) Waveshare. https://github.com/waveshare/IT8951-ePaper
- The PIO SPI program in `src/epd_io.c` (`spi_cpha0`) and the BOOTSEL button
  read in `src/main.c` are from Raspberry Pi's `pico-examples`
  (`pio/spi/spi.pio`, `picoboard/button/button.c`), **BSD-3-Clause**,
  Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
- **Font24** (`src/vendor/font24.c`), the 17x24 bitmap font the menu overlay
  draws with, is from STMicroelectronics' STM32xx-EVAL LCD examples as shipped
  in Waveshare's IT8951 demo, **BSD-3-Clause**, Copyright (c) 2014
  STMicroelectronics.
- The RP2350 PSRAM QMI bring-up (`src/psram.c`) is ported from MicroPython's
  `ports/rp2/rp2_psram.c` (**MIT**).
- Project shape, PSRAM code, picojpeg glue and tooling come from the sibling
  `el133-pico-driver` (AGPL-3.0-or-later, same author).
