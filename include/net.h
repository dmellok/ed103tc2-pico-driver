/*
 * net.h: WiFi (CYW43 on the Pico Plus 2 W) + a tiny raw-lwIP HTTP server for
 * the web UI. Credentials come from include/wifi_secrets.h (gitignored).
 *
 * Threading: the PlatformIO picosdk builder links the "threadsafe background"
 * cyw43 arch, so lwIP callbacks run from an interrupt-driven context. They
 * therefore only parse, queue a command for the main loop (app_command) or
 * fill a small JSON buffer; the preview frame is rendered by net_poll() in the
 * main loop and handed to lwIP under cyw43_arch_lwip_begin/end.
 *
 * HTTP API (all GET, POST accepted too):
 *   /                 the UI page (www/index.html)
 *   /api/state        JSON snapshot of panel, menu, tunables, timings
 *   /api/cmd?k=v      queue a command (see app_command in main.c)
 *   /frame?s=N        preview: 4-byte header (u16 w, u16 h, little-endian) then
 *                     packed 4bpp rows (w/2 bytes each), the panel content
 *                     decimated by N (2..8)
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Join the network (blocks up to ~30 s) and start the HTTP server. */
bool        net_init(void);
/* Service deferred work; call often from the main loop. */
void        net_poll(void);
bool        net_connected(void);
const char *net_ip(void);
int         net_rssi(void);

/* ---- implemented by main.c ---------------------------------------------- */

/* Queue a command (copied; executed later in the main loop). */
void app_command(const char *name, const char *value);
/* Write the state JSON; returns its length. */
size_t app_state_json(char *buf, size_t cap);
/* Render the preview at 1/scale into the app's buffer and return it; scale 0
 * returns the existing buffer without re-rendering. */
const uint8_t *app_render_preview(int scale, size_t *len);
