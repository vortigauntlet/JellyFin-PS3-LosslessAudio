#include "sc_source.h"
#include "sc_hls.h"
#include "sx_plat.h"

#include <stdlib.h>
#include <string.h>

#define PLAYLIST_MAX   (2u * 1024 * 1024)
#define SEGMENT_MAX    (3u * 1024 * 1024)       // 10 s of AAC at 160 kbit/s is 200 KB
#define URL_MAX_AGE_MS (90ull * 60 * 1000)      // signed URLs live about 2 h
#define TRIES          3

struct ScSource {
    sc_net  *net;
    char     tc_url[256];
    char     playlist_url[SC_URL_MAX];          // the base for relative segment URIs
    char    *text;                              // the playlist, which sc_hls views into
    size_t   text_len;
    sc_hls   hls;
    bool     have_hls;
    uint64_t resolved_ms;
    int      refreshes, retries;
    bool     stale;                             // a fetch was refused: re-resolve before the next one
    uint8_t *init;                              // the init segment is tiny and asked for twice (open, seek): keep it
    size_t   init_len;
};

static void drop_playlist(ScSource *s) {
    if (s->have_hls) sc_hls_free(&s->hls);
    s->have_hls = false;
    free(s->text);
    s->text = NULL;
}

// Resolve the transcoding and read its playlist. keep_count: the segment count the first playlist had (0 = none yet).
static int resolve(ScSource *s, int keep_count) {
    char url[SC_URL_MAX];
    int rc = sc_net_stream_url(s->net, s->tc_url, url, sizeof(url));
    if (rc != SC_NET_OK) return rc;
    char *text = NULL;
    size_t len = 0;
    rc = sc_net_get(s->net, url, PLAYLIST_MAX, &text, &len);
    if (rc != SC_NET_OK) return rc;
    sc_hls h;
    rc = sc_hls_parse(text, len, &h);
    if (rc != SC_HLS_OK) { free(text); return rc; }
    if (keep_count && h.nseg != keep_count) { sc_hls_free(&h); free(text); return SC_NET_E_HTTP; }   // not the same track any more
    drop_playlist(s);
    s->text = text; s->text_len = len;
    s->hls = h;
    s->have_hls = true;
    memcpy(s->playlist_url, url, strlen(url) + 1);
    s->resolved_ms = sx_now_ms();
    s->stale = false;
    return SC_NET_OK;
}

static void settle(sc_net *n) {
    // A paused or flaky link must not hold the decoder thread for a minute at a time.
    n->connect_ms = 6000; n->handshake_ms = 8000; n->first_byte_ms = 10000; n->idle_ms = 6000;
}

// Fetch one URL of the playlist (i < 0 = the init segment) with the retry and refresh policy.
static int fetch(ScSource *s, int i, uint8_t **buf, size_t *len) {
    for (int attempt = 0; attempt < TRIES; attempt++) {
        if (s->stale || sx_now_ms() - s->resolved_ms > URL_MAX_AGE_MS) {
            if (resolve(s, s->have_hls ? s->hls.nseg : 0) == SC_NET_OK) s->refreshes++;
        }
        if (!s->have_hls) { sx_sleep_ms(500); s->retries++; continue; }
        char url[SC_URL_MAX];
        if (!sc_hls_seg_url(&s->hls, i, s->playlist_url, url, sizeof(url)) || !sc_net_host_ok(url)) return -1;
        char *b = NULL;
        size_t l = 0;
        const int rc = sc_net_get(s->net, url, SEGMENT_MAX, &b, &l);
        memset(url, 0, sizeof(url));
        if (rc == SC_NET_OK && l > 0) { *buf = (uint8_t *)b; *len = l; return 0; }
        free(b);
        s->retries++;
        const int st = s->net->last_status;
        if (rc == SC_NET_E_HTTP && (st == 401 || st == 403 || st == 404 || st == 410)) s->stale = true;
        else if (attempt + 1 < TRIES) sx_sleep_ms(attempt == 0 ? 300 : 1000);
    }
    return -1;
}

static uint32_t cb_seg_ms(void *c, int i) {
    ScSource *s = (ScSource *)c;
    return (s->have_hls && i >= 0 && i < s->hls.nseg) ? s->hls.seg[i].dur_ms : 0;
}

static int cb_init(void *c, uint8_t **buf, size_t *len) {
    ScSource *s = (ScSource *)c;
    if (!s->init) {
        if (fetch(s, -1, &s->init, &s->init_len) < 0) return -1;
    }
    *buf = (uint8_t *)malloc(s->init_len);                 // the decoder frees what it is given
    if (!*buf) return -1;
    memcpy(*buf, s->init, s->init_len);
    *len = s->init_len;
    return 0;
}

static int cb_seg(void *c, int i, uint8_t **buf, size_t *len) {
    return fetch((ScSource *)c, i, buf, len);
}

ScSource *scsrc_open(sc_net *net, const char *tc_url, ScSegSource *out, int *err) {
    if (err) *err = 0;
    if (!net || !tc_url || strlen(tc_url) >= sizeof(((ScSource *)0)->tc_url)) { if (err) *err = SC_NET_E_ARG; return NULL; }
    ScSource *s = (ScSource *)calloc(1, sizeof(*s));
    if (!s) { if (err) *err = SC_NET_E_NET; return NULL; }
    s->net = net;
    settle(net);
    memcpy(s->tc_url, tc_url, strlen(tc_url) + 1);
    const int rc = resolve(s, 0);
    if (rc != SC_NET_OK) { if (err) *err = rc; free(s); return NULL; }
    out->ctx = s;
    out->nseg = s->hls.nseg;
    out->total_ms = s->hls.total_ms;
    out->seg_ms = cb_seg_ms;
    out->fetch_init = cb_init;
    out->fetch_seg = cb_seg;
    return s;
}

void scsrc_close(ScSource *s) {
    if (!s) return;
    drop_playlist(s);
    free(s->init);
    memset(s->playlist_url, 0, sizeof(s->playlist_url));
    free(s);
}

void scsrc_expire(ScSource *s) { s->stale = true; }

int scsrc_refreshes(const ScSource *s) { return s->refreshes; }
int scsrc_retries(const ScSource *s) { return s->retries; }
