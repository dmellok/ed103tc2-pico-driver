/*
 * net.c: WiFi + minimal HTTP server on the raw lwIP API. See net.h.
 */
#include "net.h"
#include "wifi_secrets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"
#include "lwip/netif.h"

extern const uint8_t www_index[];
extern const uint8_t www_index_end[];

#define HTTP_PORT  80
#define MAX_CONN   4
#define REQ_MAX    768

typedef struct {
    struct tcp_pcb *pcb;
    char            req[REQ_MAX];
    size_t          req_len;
    char            hdr[200];
    size_t          hdr_len;
    const uint8_t  *body;
    size_t          body_len;
    bool            body_copy;      /* small/volatile body: let lwIP copy it */
    size_t          sent, acked;
    bool            want_frame;     /* deferred to net_poll() */
    int             frame_scale;
    bool            is_frame;       /* body is the shared preview buffer */
    int             idle;           /* tcp_poll ticks without traffic */
} conn_t;

static conn_t          s_conns[MAX_CONN];
static struct tcp_pcb *s_listen;
static char            s_ip[20];
static bool            s_up;
static int             s_frame_users;
static char            s_json[2048];

bool        net_connected(void) { return s_up; }
const char *net_ip(void)        { return s_ip; }

int net_rssi(void)
{
    int32_t rssi = 0;
    if (!s_up) return 0;
    cyw43_wifi_get_rssi(&cyw43_state, &rssi);
    return (int)rssi;
}

/* ---- connection plumbing (lwIP context unless noted) ------------------------ */

static void conn_release(conn_t *c)
{
    if (c->is_frame && s_frame_users > 0) s_frame_users--;
    memset(c, 0, sizeof *c);
}

static void conn_close(conn_t *c)
{
    struct tcp_pcb *pcb = c->pcb;
    if (pcb) {
        tcp_arg(pcb, NULL); tcp_recv(pcb, NULL); tcp_sent(pcb, NULL);
        tcp_err(pcb, NULL); tcp_poll(pcb, NULL, 0);
        if (tcp_close(pcb) != ERR_OK) tcp_abort(pcb);
    }
    conn_release(c);
}

static void conn_pump(conn_t *c)
{
    size_t total = c->hdr_len + c->body_len;
    while (c->pcb && c->sent < total) {
        size_t room = tcp_sndbuf(c->pcb);
        if (room == 0) break;
        const uint8_t *src; size_t avail; u8_t flags;
        if (c->sent < c->hdr_len) {
            src = (const uint8_t *)c->hdr + c->sent; avail = c->hdr_len - c->sent; flags = TCP_WRITE_FLAG_COPY;
        } else {
            size_t off = c->sent - c->hdr_len;
            src = c->body + off; avail = c->body_len - off; flags = c->body_copy ? TCP_WRITE_FLAG_COPY : 0;
        }
        size_t n = avail < room ? avail : room;
        if (n > 2 * TCP_MSS) n = 2 * TCP_MSS;
        if (tcp_write(c->pcb, src, (u16_t)n, flags) != ERR_OK) break;
        c->sent += n;
    }
    if (c->pcb) tcp_output(c->pcb);
    if (c->pcb && c->sent >= total && c->acked >= total) conn_close(c);
}

static void respond(conn_t *c, int code, const char *ctype, const void *body, size_t len, bool copy)
{
    c->hdr_len = (size_t)snprintf(c->hdr, sizeof c->hdr,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
        "Connection: close\r\nCache-Control: no-store\r\n\r\n",
        code, code == 200 ? "OK" : code == 404 ? "Not Found" : "Bad Request", ctype, (unsigned)len);
    c->body = body; c->body_len = len; c->body_copy = copy;
    c->sent = c->acked = 0;
    conn_pump(c);
}

static int hexval(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* Copy a URL-encoded token (up to '&' or end) into out, decoding %XX and '+'. */
static void url_decode(const char *src, char *out, size_t cap)
{
    size_t o = 0;
    while (*src && *src != '&' && o + 1 < cap) {
        if (*src == '%' && hexval(src[1]) >= 0 && hexval(src[2]) >= 0) {
            out[o++] = (char)(hexval(src[1]) * 16 + hexval(src[2])); src += 3;
        } else {
            out[o++] = (*src == '+') ? ' ' : *src; src++;
        }
    }
    out[o] = 0;
}

static bool query_get(const char *q, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    while (q && *q) {
        if (!strncmp(q, key, kl) && q[kl] == '=') { url_decode(q + kl + 1, out, cap); return true; }
        q = strchr(q, '&');
        if (q) q++;
    }
    return false;
}

static void handle_request(conn_t *c)
{
    c->req[c->req_len] = 0;
    char *eol = strstr(c->req, "\r\n");
    if (eol) *eol = 0;
    char *sp = strchr(c->req, ' ');
    if (!sp) { respond(c, 400, "text/plain", "bad request", 11, true); return; }
    char *path = sp + 1;
    char *sp2 = strchr(path, ' ');
    if (sp2) *sp2 = 0;
    char *query = strchr(path, '?');
    if (query) *query++ = 0; else query = "";

    if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
        respond(c, 200, "text/html; charset=utf-8", www_index, (size_t)(www_index_end - www_index), false);
    } else if (!strcmp(path, "/api/state")) {
        size_t n = app_state_json(s_json, sizeof s_json);
        respond(c, 200, "application/json", s_json, n, true);
    } else if (!strcmp(path, "/api/cmd")) {
        char name[24] = "", value[48] = "";
        const char *eq = strchr(query, '=');
        if (eq && (size_t)(eq - query) < sizeof name) {
            memcpy(name, query, (size_t)(eq - query)); name[eq - query] = 0;
            url_decode(eq + 1, value, sizeof value);
        } else {
            url_decode(query, name, sizeof name);
        }
        app_command(name, value);
        respond(c, 200, "application/json", "{\"ok\":true}", 11, true);
    } else if (!strcmp(path, "/frame")) {
        char v[8];
        int sc = query_get(query, "s", v, sizeof v) ? atoi(v) : 4;
        if (sc < 2) sc = 2;
        if (sc > 8) sc = 8;
        c->want_frame = true;           /* rendered + answered by net_poll() */
        c->frame_scale = sc;
    } else {
        respond(c, 404, "text/plain", "not found", 9, true);
    }
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    conn_t *c = arg;
    (void)err;
    if (!p) {                                   /* peer closed */
        if (c) conn_close(c); else tcp_close(pcb);
        return ERR_OK;
    }
    if (c) {
        c->idle = 0;
        if (c->body == NULL && !c->want_frame) {
            size_t room = REQ_MAX - 1 - c->req_len;
            size_t n = p->tot_len < room ? p->tot_len : room;
            pbuf_copy_partial(p, c->req + c->req_len, (u16_t)n, 0);
            c->req_len += n;
            c->req[c->req_len] = 0;
            if (strstr(c->req, "\r\n\r\n") || c->req_len >= REQ_MAX - 1) handle_request(c);
        }
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    (void)pcb;
    conn_t *c = arg;
    if (!c) return ERR_OK;
    c->idle = 0;
    c->acked += len;
    conn_pump(c);
    return ERR_OK;
}

static void on_err(void *arg, err_t err)
{
    (void)err;
    conn_t *c = arg;
    if (c) { c->pcb = NULL; conn_release(c); }   /* pcb already freed by lwIP */
}

static err_t on_poll(void *arg, struct tcp_pcb *pcb)
{
    conn_t *c = arg;
    if (!c) { tcp_abort(pcb); return ERR_ABRT; }
    if (++c->idle > 8) { conn_close(c); return ERR_ABRT; }   /* ~16 s idle */
    if (c->body) conn_pump(c);
    return ERR_OK;
}

static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || !pcb) return ERR_VAL;
    conn_t *c = NULL;
    for (int i = 0; i < MAX_CONN; i++) if (!s_conns[i].pcb) { c = &s_conns[i]; break; }
    if (!c) { tcp_abort(pcb); return ERR_ABRT; }
    memset(c, 0, sizeof *c);
    c->pcb = pcb;
    tcp_arg(pcb, c);
    tcp_recv(pcb, on_recv);
    tcp_sent(pcb, on_sent);
    tcp_err(pcb, on_err);
    tcp_poll(pcb, on_poll, 4);                  /* every 2 s */
    return ERR_OK;
}

/* ---- public ----------------------------------------------------------------- */

bool net_init(void)
{
    if (cyw43_arch_init()) {
        printf("net: cyw43 init failed\n");
        return false;
    }
    cyw43_arch_enable_sta_mode();
    printf("net: connecting to \"%s\"...\n", WIFI_SSID);
    int rc = cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000);
#ifdef CYW43_AUTH_WPA3_WPA2_AES_PSK
    if (rc) {
        printf("net: WPA2 join failed (%d), trying WPA3/WPA2\n", rc);
        rc = cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA3_WPA2_AES_PSK, 30000);
    }
#endif
    if (rc) {
        printf("net: connect failed (%d); web UI unavailable\n", rc);
        return false;
    }
    snprintf(s_ip, sizeof s_ip, "%s", ip4addr_ntoa(netif_ip4_addr(netif_default)));

    cyw43_arch_lwip_begin();
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (pcb && tcp_bind(pcb, IP_ANY_TYPE, HTTP_PORT) == ERR_OK) {
        s_listen = tcp_listen_with_backlog(pcb, 4);
        tcp_accept(s_listen, on_accept);
    }
    cyw43_arch_lwip_end();
    if (!s_listen) { printf("net: listen failed\n"); return false; }

    s_up = true;
    printf("net: connected, web UI at http://%s/\n", s_ip);
    return true;
}

void net_poll(void)
{
    if (!s_up) return;
    cyw43_arch_poll();                          /* no-op for the background arch */

    /* Serve at most one deferred frame per call: render in main context, hand
     * over under the lwIP lock. */
    conn_t *target = NULL; int scale = 4; struct tcp_pcb *pcb = NULL;
    cyw43_arch_lwip_begin();
    for (int i = 0; i < MAX_CONN; i++) {
        if (s_conns[i].pcb && s_conns[i].want_frame) { target = &s_conns[i]; scale = target->frame_scale; pcb = target->pcb; break; }
    }
    cyw43_arch_lwip_end();
    if (!target) return;

    size_t len = 0;
    const uint8_t *buf = app_render_preview(s_frame_users == 0 ? scale : 0, &len);

    cyw43_arch_lwip_begin();
    if (target->pcb == pcb && target->want_frame) {
        target->want_frame = false;
        if (buf && len) {
            target->is_frame = true;
            s_frame_users++;
            respond(target, 200, "application/octet-stream", buf, len, false);
        } else {
            respond(target, 400, "text/plain", "no preview", 10, true);
        }
    }
    cyw43_arch_lwip_end();
}
