# ed103tc2-pico-driver

Minimal standalone driver for the **Waveshare 10.3" e-Paper HAT** (ED103TC2
glass, IT8951 controller, 1872x1404, 16-level greyscale) on a **Pimoroni Pico
Plus 2 W** (RP2350B). It brings the panel up, draws a set of test patterns and
an embedded sample image, and advances one screen per press of BOOTSEL (or a
serial key, or an optional button). No WiFi, MQTT, deep sleep, or config — just
the panel.

Sibling of [el133-pico-driver](https://github.com/dmellok/el133-pico-driver)
(Inky Impression 13.3"): same board, same adapter, same project shape, with the
Spectra 6 dual-controller driver swapped for an IT8951 one.

## Hardware

Pico Plus 2 W on a Hard Stuff "Pico to Pi Hat" adapter, driving the Waveshare
HAT through its 40-pin header. The adapter is a 1:1 Pi-BCM-to-Pico-GP
passthrough **except it swaps clock and data** (meter-verified on the el133
build; the vendor pinout sheet is wrong).

| HAT signal | Pi BCM | Pico GP | Notes |
| --- | --- | --- | --- |
| SCK | 11 | GP10 | SPI1 SCK |
| MOSI | 10 | GP11 | SPI1 TX |
| MISO | 9 | GP12 | SPI1 RX, **not** GP9 |
| CS | 8 | GP8 | |
| RST | 17 | GP17 | active low |
| HRDY | 24 | GP1 | HIGH = ready, LOW = busy; **not** GP24 |
| 5V / GND | | VBUS / GND | the HAT draws ~1.2 W while refreshing |

These were found on real hardware with the firmware's own pin discovery: the
adapter lays the Pi's SPI0 onto the Pico's SPI1 hardware pins, and reroutes
BCM23-25 because GP23/24/25/29 belong to the Pico Plus 2 W's wireless module
(so Pi BCM24, the HAT's HRDY, arrives on GP1). SPI runs on a PIO state machine
anyway, so any GPIO works. On boot you should see:

```
it8951: trying CS=GP8 MISO=GP12 mode 0 @12000000 Hz
it8951: HRDY high 820 ms after reset
it8951: found on CS=GP8 MISO=GP12 (mode 0): panel 1872x1404, image buffer 0x00119f00, FW "SWv_0.1.", LUT "M841_TFA5210"
```

SPI runs at 12.5 MHz: 18.75 MHz returned corrupted device info through the
adapter's wiring, so init insists on three identical device-info reads before it
accepts a speed, and falls back to 9.375 MHz if they disagree. Frames stream by
DMA, double-buffered, so row generation overlaps the transfer.

If the configured pins do not answer, the firmware falls back to slower clocks,
SPI mode 3 and swapped CS/MISO, then runs a GPIO discovery (which pins are
driven, which one follows an RST pulse: that is HRDY) and a full CS/MISO/mode
sweep, printing the `-DEPD_PIN_...` flags to build with. `d` on the serial
console repeats it. Note the IT8951 holds HRDY low for ~0.8 s after a reset
while its firmware boots.

**VCOM.** Every panel has its own VCOM printed on the FPC cable (e.g. `-1.50V`).
Build with `-DEPD_VCOM_MV=1500` (millivolts, positive) or type `v1500` on the
serial console. Until you do, the firmware keeps whatever the controller
reports and nags about it on every init. Running with the wrong VCOM degrades
the panel over time and makes the greys look wrong immediately.

**Orientation.** Waveshare drives the 10.3" glass horizontally mirrored
relative to the IT8951 raster ("mode 1" in their demo); the driver un-mirrors
it. The test card has a black bar along the top and a black square top-left: if
the square shows top-**right**, build with `-DEPD_MIRROR_H=0`.

## Build / flash / monitor

PlatformIO core: `~/.platformio/penv/bin/pio` (adjust if yours is on PATH).

```sh
pio run                       # build -> .pio/build/pimoroni_pico_plus_2w/firmware.uf2
pio device monitor            # 115200 baud
```

Flashing (no `pio run -t upload` — the bundled picotool can't force this RP2350
into BOOTSEL): double-tap **RESET** to mount the `RP2350` drive, then copy
`firmware.uf2` onto it. (A 1200-baud touch on the USB CDC port also triggers
BOOTSEL.)

## What it does

On boot it identifies the controller, does an INIT-mode clear (the flashing
deghost) and paints the vertical grey-bar test pattern. From there you drive it
through a **menu drawn as an overlay** on top of whatever is on the panel:

```
+--------------------------------+
| ED103TC2 DEMO                  |
|--------------------------------|
| [vertical grey bars]           |   <- highlighted line
|  on-device JPEG                |
|  test card (.bin)              |
|  horizontal grey bands         |
|  checkerboard                  |
|  grey blocks                   |
|  solid white                   |
|  clear screen (INIT)           |
|  sleep controller              |
|--------------------------------|
| short: move  long: select      |
+--------------------------------+
```

The HAT has no buttons, so input is the Pico's **BOOTSEL** button, an optional
push button from **GP15** to GND, or the USB serial console. One-button grammar:

| press | menu closed | menu open |
| --- | --- | --- |
| short (< 600 ms) | open the menu over the current screen | move the highlight down (wraps) |
| long (>= 600 ms) | advance to the next screen | select the highlighted line |

Screens in menu order: vertical grey bars (boot), the embedded JPEG decoded and
dithered to 16 greys **on the device**, the pre-packed embedded test card,
horizontal grey bands, checkerboard, grey blocks (4x4 of the 16 levels), solid
white; then two actions, an INIT-mode clear and putting the controller to sleep.

The menu is composited onto each row as the frame streams out, so there is no
frame buffer, and only the menu's rectangle is refreshed when it opens
(GC16), moves (A2, the fast black/white waveform, ~0.3 s) or closes (GC16,
which repaints the content underneath). Selecting a screen does a full GC16
paint: ~1 s to stream the 1.3 MB frame plus ~1-2 s of refresh. If the A2 moves
look wrong on your firmware, build with `-DMENU_MOVE_MODE=2` to use GC16 for
them too. Serial logs each step and echoes the menu.

Serial console keys:

| key | action |
| --- | --- |
| `m` | open / close the menu |
| `j` / `k`, Tab, arrow keys | move the highlight (opens the menu if closed) |
| Enter, space | select (menu open) / next screen (menu closed) |
| Esc, `q` | close the menu without selecting |
| `n` | next screen |
| `1`..`7` | jump to that screen |
| `c` | INIT-mode clear (deghost, flashes) |
| `w` | white in GC16 |
| `v1500` Enter | set VCOM to -1.50 V for this session |
| `V` | read VCOM back |
| `i` | re-init the controller (re-probes pins) |
| `z` | put the controller to sleep (next paint re-inits) |
| `e` | Waveshare's "enhance driving" register poke (for blurry long-FPC panels) |
| `d` | GPIO discovery + CS/MISO/mode probe sweep (also runs automatically if init fails) |
| `?` | help |

## Web UI

With `include/wifi_secrets.h` filled in (copy `wifi_secrets.h.example`; the real
file is gitignored), the firmware joins your WiFi after the first paint, prints
`web UI: http://<ip>/` on the serial console and draws the same URL in a small
banner in the panel's bottom-left corner. The page has:

- an **emulated screen**: the exact row generator the panel is fed, menu and
  banner included, decimated to 1/2..1/6 and drawn on a canvas; it refreshes
  whenever the paint counter changes;
- **screens**, **menu** (open / up / down / select / close, or click an entry)
  and **panel** controls (next, INIT clear, white, sleep, re-init, enhance);
- **tunables** that take effect live: VCOM, horizontal mirror, the menu-move
  waveform (A2 or GC16), SPI clock (re-inits), long-press time, and the banner.

Everything is plain HTTP on the LAN with no authentication; it is a bench tool.
The server is ~300 lines of raw lwIP (`src/net.c`): requests are parsed in the
CYW43 background context, commands are queued for the main loop, and the
preview is rendered in the main loop and streamed with zero-copy `tcp_write`.
`GET /api/state` returns the JSON the page uses; `GET /api/cmd?name=value`
queues a command; `GET /frame?s=4` returns the preview (u16 w, u16 h, then
packed 4bpp rows).

## Images

There are two image paths: a frame packed on the host, and a JPEG converted on
the device itself.

### Pre-packed `.bin` (host-converted)

`src/sample_card.bin` is a landscape 1872x1404 packed-4bpp frame (1,314,144
bytes), embedded into flash via `src/sample_card.S` (`.incbin`) and shown
directly by `it8951_show_frame()` (no decode, no PSRAM).

- Regenerate the synthetic test card (no deps): `python3 tools/gen_sample.py`
- Use your own image (needs Pillow): `python3 tools/png2frame.py photo.png`
  — it cover-fits to 1872x1404, Floyd-Steinberg dithers to the 16 greys
  (`--no-dither` for graphics), and overwrites `src/sample_card.bin`. Rebuild
  and reflash.

Frame format: packed 4bpp, 2 px/byte, high nibble = even column, 936 bytes/row,
1404 rows. Grey codes 0x0 black .. 0xF white. The driver mirrors and byte-swaps
on the way out, so frames are always in logical left-to-right order.

### On-device JPEG decode + dither

`src/sample.jpg` is a 1872x1404 baseline JPEG embedded the same way
(`src/sample_jpg.S`). At paint time `img_decode_jpeg()` (`src/img_decode.c`)
decodes it with the vendored **picojpeg** into an 8-bit luma buffer in PSRAM,
then Floyd-Steinberg dithers it to the 16 levels, packing the result into a
second PSRAM buffer that `it8951_show_frame()` paints. This mirrors what a
networked client does with a server-delivered photo, no host conversion needed.

- **PSRAM is required** (the luma buffer is 2.6 MB and the packed frame 1.3 MB,
  both in the Pico Plus 2 W's 8 MB APS6404). On a board without PSRAM the screen
  is skipped with a serial note.
- The bundled sample is a royalty-free tulip photo (see `NOTICES.md`). Fetch and
  crop it reproducibly with `python3 tools/fetch_sample_jpg.py`, or point it at
  your own file: `python3 tools/fetch_sample_jpg.py --src photo.jpg`.
- For a synthetic gradient instead (needs Pillow; `numpy` optional, just faster):
  `python3 tools/make_sample_jpg.py`.
- To show your own photo directly, save it as a **1872x1404 baseline JPEG** over
  `src/sample.jpg` and rebuild — the decoder does not resize, so it clips a larger
  image and white-pads a smaller one.

## How the IT8951 is driven

Ported from Waveshare's IT8951-ePaper Raspberry Pi C demo. Every exchange is a
16-bit preamble (`0x6000` command, `0x0000` data, `0x1000` read) then 16-bit
words MSB first, with HRDY checked before the preamble and before the payload.
Init = reset, `SYS_RUN`, `GET_DEV_INFO` (panel size, image buffer address,
firmware/LUT strings), enable packed writes (`I80CPCR = 1`), set VCOM. A paint =
wait for the LUT engine (`LUTAFSR == 0`), point `LISAR` at the image buffer,
`LD_IMG_AREA` (little-endian, 4bpp, no rotation, full panel), one long data
transaction with the whole frame, `LD_IMG_END`, then `DPY_AREA` with mode GC16
(2) and wait for `LUTAFSR` again. INIT (0) clears. Partial updates
(`it8951_update_area`) are the same flow with a sub-rectangle in `LD_IMG_AREA`
and `DPY_AREA`; the menu uses them with A2 (6 on this LUT), the fast 1-bit
waveform, which the IT8951 happily applies to 4bpp data by thresholding (that
is how Waveshare's own demo clears in A2).

## Layout

- `include/epd_io.h` / `src/epd_io.c` — PIO SPI (any pins) + CS/RST/HRDY
  transport, 16-bit word helpers.
- `include/it8951.h` / `src/it8951.c` — the IT8951 driver: pin probing, init,
  VCOM, the mirror/byte-swap row transform, load + GC16 refresh.
  `it8951_show_pattern()` (per-row generator) and `it8951_show_frame()`
  (landscape buffer).
- `include/patterns.h` / `src/patterns.c` — the procedural test patterns.
- `include/menu.h` / `src/menu.c` — the overlay menu renderer (per-row
  compositing, Font24 at 2x); `include/fonts.h` / `src/vendor/font24.c` — the
  17x24 bitmap font.
- `include/images.h` / `src/sample_card.S` / `src/sample_card.bin` — the embedded
  pre-packed image; `src/sample_jpg.S` / `src/sample.jpg` — the embedded JPEG.
- `include/img_decode.h` / `src/img_decode.c` — on-device JPEG decode +
  Floyd-Steinberg dither to a 16-grey frame in PSRAM.
- `include/psram.h` / `src/psram.c` — RP2350 QMI bring-up for the 8 MB PSRAM.
- `include/picojpeg.h` / `src/vendor/picojpeg.c` — vendored picojpeg (public
  domain) baseline JPEG decoder.
- `src/main.c` — boot, PSRAM init, the menu state machine, BOOTSEL / button
  short-vs-long press, serial keys, the shared command executor, state JSON and
  preview renderer for the web UI.
- `include/net.h` / `src/net.c` — WiFi join + raw-lwIP HTTP server;
  `www/index.html` (embedded via `src/www.S`) — the page; `include/lwipopts.h`;
  `include/wifi_secrets.h.example` — credentials template.
- `tools/gen_sample.py`, `tools/png2frame.py` — pre-packed `.bin` tooling;
  `tools/make_sample_jpg.py` (synthetic gradient) and `tools/fetch_sample_jpg.py`
  (fetch/crop a real photo) — embedded-JPEG tooling.

## Status

Controller bring-up is verified on hardware (device info, LUT "M841_TFA5210",
pins above). Still to confirm by eye on the panel: mirroring (`EPD_MIRROR_H`),
the greys once VCOM is set, and the menu's A2 partial updates
(`MENU_MOVE_MODE`).

## License

AGPL-3.0-or-later (`LICENSE`). The vendored picojpeg decoder
(`src/vendor/picojpeg.c`) is public domain, the PIO SPI program and BOOTSEL
button read are BSD-3-Clause from pico-examples, the menu font
(`src/vendor/font24.c`) is BSD-3-Clause from STMicroelectronics, the IT8951 protocol sequence is
ported from Waveshare's MIT-licensed demo, and the bundled sample photo
(`src/sample.jpg`) is CC BY-SA 3.0; see `NOTICES.md`.
