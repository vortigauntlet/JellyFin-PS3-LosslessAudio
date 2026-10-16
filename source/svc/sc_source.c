#include "sc_source.h"
#include "sc_hls.h"
#include "sx_plat.h"

#include <stdlib.h>
#include <string.h>

#define INIT_MAX       (64u * 1024)
#define URL_MAX_AGE_MS (90ull * 60 * 1000)      // signed URLs live about 2 h
#define TRIES          3
#define RESUMES        2                        // times a segment's connection is re-opened where it broke

struct ScSource {
    sc_net  *net;
    char     tc_url[256];
    char     playlist_url[SC_URL_MAX];          // the base for relative segment URIs
    sc_hls   hls;
    bool     have_hls;
    uint64_t resolved_ms;
    int      refreshes, retries;
    bool     stale;                             // a request was refused: re-resolve before the next one
    uint8_t *init;                              // the init segment is tiny and asked for twice (open, seek): keep it
    size_t   init_len;
};

typedef struct {
    sc_stream *st;
    int        seg;
    int64_t    pos;                             // body bytes delivered so far
} Seg;

static void drop_playlist(ScSource *s) {
    if (s->have_hls) sc_hls_free(&s->hls);
    s->have_hls = false;
}

// Resolve the transcoding and read its playlist. keep_count: the segment count the first playlist had (0 = none yet).
static int resolve(ScSource *s, int keep_count) {
    char url[SC_URL_MAX];
    int rc = sc_net_stream_url(s->net, s->tc_url, url, sizeof(url));
    if (rc != SC_NET_OK) return rc;
    sc_net_flush(s->net);                       // the API connection is done with: free its TLS state before the media one opens
    sc_stream *st = NULL;
    rc = sc_net_open(s->net, url, -1, &st);
    if (rc != SC_NET_OK) return rc;
    sc_hls h;
    const int hr = sc_hls_read(sc_stream_jread, st, &h);
    sc_stream_close(st);
    if (hr != SC_HLS_OK) return hr;
    if (keep_count && h.nseg != keep_count) { sc_hls_free(&h); return SC_NET_E_HTTP; }   // not the same track any more
    drop_playlist(s);
    s->hls = h;
    s->have_hls = true;
    memcpy(s->playlist_url, url, strlen(url) + 1);
    memset(url, 0, sizeof(url));
    s->resolved_ms = sx_now_ms();
    s->stale = false;
    return SC_NET_OK;
}

static void settle(sc_net *n) {
    // A paused or flaky link must not hold the decoder thread for a minute at a time.
    n->connect_ms = 6000; n->handshake_ms = 8000; n->first_byte_ms = 10000; n->idle_ms = 6000;
}

// Opens URL number i (< 0: the init segment) of the playlist, from byte range_start on, with the retry and refresh policy.
static int open_url(ScSource *s, int i, int64_t range_start, sc_stream **out) {
    for (int attempt = 0; attempt < TRIES; attempt++) {
        if (s->stale || sx_now_ms() - s->resolved_ms > URL_MAX_AGE_MS) {
            if (resolve(s, s->have_hls ? s->hls.nseg : 0) == SC_NET_OK) s->refreshes++;
        }
        if (!s->have_hls) { sx_sleep_ms(500); s->retries++; continue; }
        char url[SC_URL_MAX];
        if (!sc_hls_seg_url(&s->hls, i, s->playlist_url, url, sizeof(url)) || !sc_net_host_ok(url)) return -1;
        const int rc = sc_net_open(s->net, url, range_start, out);
        memset(url, 0, sizeof(url));
        if (rc == SC_NET_OK) return 0;
        s->retries++;
        const int st = s->net->last_status;
        if (rc == SC_NET_E_HTTP && (st == 401 || st == 403 || st == 404 || st == 410)) s->stale = true;
        else if (attempt + 1 < TRIES) sx_sleep_ms(attempt == 0 ? 300 : 1000);
    }
    return -1;
}

static uint32_t cb_seg_ms(void *c, int i) {
    ScSource *s = (ScSource *)c;
    return (s->have_hls && i >= 0 && i < s->hls.nseg) ? s->hls.dur_ms[i] : 0;
}

static int cb_init(void *c, uint8_t **buf, size_t *len) {
    ScSource *s = (ScSource *)c;
    if (!s->init) {
        sc_stream *st = NULL;
        if (open_url(s, -1, -1, &st) < 0) return -1;
        uint8_t *b = (uint8_t *)malloc(INIT_MAX);
        size_t n = 0;
        int r;
        while (b && n < INIT_MAX && (r = sc_stream_read(st, b + n, (int)(INIT_MAX - n))) > 0) n += (size_t)r;
        sc_stream_close(st);
        if (!b || n == 0) { free(b); return -1; }
        uint8_t *fit = (uint8_t *)realloc(b, n);
        s->init = fit ? fit : b;
        s->init_len = n;
    }
    *buf = (uint8_t *)malloc(s->init_len);                 // the decoder frees what it is given
    if (!*buf) return -1;
    memcpy(*buf, s->init, s->init_len);
    *len = s->init_len;
    return 0;
}

static int cb_seg_open(void *c, int i, void **h) {
    ScSource *s = (ScSource *)c;
    Seg *g = (Seg *)calloc(1, sizeof(*g));
    if (!g) return -1;
    g->seg = i;
    if (open_url(s, i, -1, &g->st) < 0) { free(g); return -1; }
    *h = g;
    return 0;
}

static int cb_seg_read(void *c, void *h, uint8_t *buf, int n) {
    ScSource *s = (ScSource *)c;
    Seg *g = (Seg *)h;
    for (int resumes = 0;; resumes++) {
        const int r = g->st ? sc_stream_read(g->st, buf, n) : -1;
        if (r >= 0) { g->pos += r; return r; }
        // The connection broke partway: open it again where it was.
        if (resumes >= RESUMES) return -1;
        s->retries++;
        if (g->st) { sc_stream_close(g->st); g->st = NULL; }
        sx_sleep_ms(resumes == 0 ? 200 : 800);
        if (open_url(s, g->seg, g->pos, &g->st) < 0) { g->st = NULL; return -1; }
    }
}

static void cb_seg_close(void *c, void *h) {
    (void)c;
    Seg *g = (Seg *)h;
    if (g->st) {
        // Read what is left of a fully used body (a few bytes) so the connection can be pooled; a segment dropped early is simply closed.
        char tmp[512];
        for (int k = 0; k < 8; k++) if (sc_stream_read(g->st, tmp, sizeof(tmp)) <= 0) break;
        sc_stream_close(g->st);
    }
    free(g);
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
    out->seg_open = cb_seg_open;
    out->seg_read = cb_seg_read;
    out->seg_close = cb_seg_close;
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
