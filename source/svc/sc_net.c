#include "sc_net.h"
#include "sc_api.h"
#include "sx_http.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SC_UA "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/141.0 Safari/537.36"
#define HOME_URL "https://soundcloud.com/"
#define MAX_SCRIPTS 24

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...) {
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    sx_log(line);                       // callers pass host names and status codes only, never URLs
}

static bool is_alnum(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

static bool valid_cid(const char *s) {
    if (!s || strlen(s) != SC_CID_LEN) return false;
    for (int i = 0; i < SC_CID_LEN; i++) if (!is_alnum(s[i])) return false;
    return true;
}

static bool ends_with(const char *s, size_t n, const char *suf) {
    const size_t l = strlen(suf);
    return n >= l && memcmp(s + n - l, suf, l) == 0;
}

bool sc_net_host_ok(const char *url) {
    if (strncmp(url, "https://", 8) != 0) return false;
    const char *h = url + 8;
    size_t n = 0;
    while (h[n] && h[n] != '/' && h[n] != ':' && h[n] != '?' && h[n] != '#' && h[n] != '@') n++;
    if (h[n] == '@') return false;
    if (n == 0 || n > 120) return false;
    return ends_with(h, n, ".soundcloud.com") || ends_with(h, n, ".soundcloud.cloud") ||
           ends_with(h, n, ".sndcdn.com") || (n == 14 && memcmp(h, "soundcloud.com", 14) == 0);
}

void sc_net_init(sc_net *n) {
    memset(n, 0, sizeof(*n));
    sx_http_init();
    sx_http_set_user_agent(SC_UA);
}

void sc_net_set_cid(sc_net *n, const char *cid) {
    if (valid_cid(cid)) { memcpy(n->cid, cid, SC_CID_LEN + 1); n->cid_dirty = false; }
}

// ---- generic GET -----------------------------------------------------------------------------

static int do_get(sc_net *n, const char *url, size_t max_body, char **body, size_t *len) {
    sx_http_req rq;
    memset(&rq, 0, sizeof(rq));
    rq.url = url;
    rq.headers = "Accept: */*\r\n";
    rq.range_start = -1; rq.range_end = -1;
    rq.connect_ms = n->connect_ms; rq.handshake_ms = n->handshake_ms;
    rq.first_byte_ms = n->first_byte_ms; rq.idle_ms = n->idle_ms;
    sx_http_info info;
    memset(&info, 0, sizeof(info));
    char *b = NULL;
    size_t bl = 0;
    const int rc = sx_http_fetch(&rq, max_body, &b, &bl, &info);
    n->last_status = info.status;
    n->last_err = rc;
    if (rc == SX_E_TOOBIG) return SC_NET_E_TOOBIG;
    if (rc != SX_OK) { free(b); return SC_NET_E_NET; }
    if (info.status != 200) { free(b); return SC_NET_E_HTTP; }
    *body = b; *len = bl;
    return SC_NET_OK;
}

int sc_net_get(sc_net *n, const char *url, size_t max_body, char **body, size_t *len) {
    if (!sc_net_host_ok(url)) return SC_NET_E_ARG;
    return do_get(n, url, max_body, body, len);
}

// ---- client_id -----------------------------------------------------------------------------------

static const char *find_mem(const char *h, size_t hl, const char *nd, size_t nl) {
    if (nl == 0 || hl < nl) return NULL;
    for (size_t i = 0; i + nl <= hl; i++) if (h[i] == nd[0] && memcmp(h + i, nd, nl) == 0) return h + i;
    return NULL;
}

// Pulls the src of each <script ... src="..."> out of the page, in order.
static int script_srcs(const char *html, size_t len, char out[][200], int max) {
    int n = 0;
    const char *p = html, *end = html + len;
    while (n < max) {
        const char *s = find_mem(p, (size_t)(end - p), "<script", 7);
        if (!s) break;
        const char *gt = (const char *)memchr(s, '>', (size_t)(end - s));
        if (!gt) break;
        p = gt + 1;
        const char *src = find_mem(s, (size_t)(gt - s), " src=\"", 6);
        if (!src) continue;
        src += 6;
        const char *q = (const char *)memchr(src, '"', (size_t)(gt - src));
        if (!q || (size_t)(q - src) >= 200) continue;
        memcpy(out[n], src, (size_t)(q - src));
        out[n][q - src] = 0;
        n++;
    }
    return n;
}

static bool scan_script(sc_net *n, const char *url, sc_scan *sc) {
    sx_http_req rq;
    memset(&rq, 0, sizeof(rq));
    rq.url = url;
    rq.headers = "Accept: */*\r\n";
    rq.range_start = -1; rq.range_end = -1;
    rq.no_pool = true;                                 // we may drop this one mid-body
    sx_http_info info;
    memset(&info, 0, sizeof(info));
    sx_http *h = sx_http_open(&rq, &info);
    n->last_status = info.status;
    if (!h) { n->last_err = info.err.code; return false; }
    bool found = false;
    if (info.status == 200) {
        char buf[4096];
        int r;
        while ((r = sx_http_read(h, buf, sizeof(buf))) > 0) {
            if (sc_scan_feed(sc, buf, (size_t)r)) { found = true; break; }
        }
    }
    sx_http_close(h);                                  // closing early drops the rest of the body
    return found;
}

int sc_net_scrape_cid(sc_net *n) {
    char *page = NULL;
    size_t pl = 0;
    int rc = do_get(n, HOME_URL, 1024 * 1024, &page, &pl);
    if (rc != SC_NET_OK) { logf_("sc: home page failed rc=%d status=%d", rc, n->last_status); return rc; }
    char (*srcs)[200] = (char (*)[200])malloc(sizeof(char[200]) * MAX_SCRIPTS);
    if (!srcs) { free(page); return SC_NET_E_NET; }
    const int ns = script_srcs(page, pl, srcs, MAX_SCRIPTS);
    free(page);
    int result = SC_NET_E_CID;
    bool net_failed = false;
    for (int i = ns - 1; i >= 0 && result != SC_NET_OK; i--) {       // the id lives in the last script
        if (!sc_net_host_ok(srcs[i])) continue;
        sc_scan sc;
        sc_scan_init(&sc);
        if (scan_script(n, srcs[i], &sc)) {
            memcpy(n->cid, sc_scan_id(&sc), SC_CID_LEN + 1);
            n->cid_dirty = true;
            result = SC_NET_OK;
        } else if (n->last_status == 0) {
            net_failed = true;
        }
    }
    free(srcs);
    if (result != SC_NET_OK && net_failed) result = SC_NET_E_NET;
    logf_("sc: client id %s (%d scripts)", result == SC_NET_OK ? "found" : "NOT found", ns);
    return result;
}

// ---- API ---------------------------------------------------------------------------------------

// Swaps the placeholder for the real id in a copy of the URL.
static bool with_cid(const char *url, const char *cid, char *out, size_t cap) {
    const char *at = strstr(url, SC_CID_PLACEHOLDER);
    if (!at) return false;
    const size_t head = (size_t)(at - url), tail = strlen(at + SC_CID_LEN);
    if (head + SC_CID_LEN + tail + 1 > cap) return false;
    memcpy(out, url, head);
    memcpy(out + head, cid, SC_CID_LEN);
    memcpy(out + head + SC_CID_LEN, at + SC_CID_LEN, tail + 1);
    return true;
}

int sc_net_api(sc_net *n, const char *url, size_t max_body, char **body, size_t *len) {
    if (!sc_net_host_ok(url) || !strstr(url, SC_CID_PLACEHOLDER)) return SC_NET_E_ARG;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!n->cid[0]) {
            const int r = sc_net_scrape_cid(n);
            if (r != SC_NET_OK) return r;
        }
        char real[SC_URL_MAX];
        if (!with_cid(url, n->cid, real, sizeof(real))) return SC_NET_E_ARG;
        const int rc = do_get(n, real, max_body, body, len);
        memset(real, 0, sizeof(real));
        if (rc == SC_NET_E_HTTP && (n->last_status == 401 || n->last_status == 403) && attempt == 0) {
            logf_("sc: api %d, refreshing the client id", n->last_status);
            n->cid[0] = 0;                                  // scrape a fresh one and retry once
            continue;
        }
        return rc;
    }
    return SC_NET_E_HTTP;
}

int sc_net_stream_url(sc_net *n, const char *tc_url, char *out, size_t cap) {
    char url[512];
    if (!sc_url_with_cid(tc_url, SC_CID_PLACEHOLDER, url, sizeof(url))) return SC_NET_E_ARG;
    char *body = NULL;
    size_t bl = 0;
    const int rc = sc_net_api(n, url, 64 * 1024, &body, &bl);
    if (rc != SC_NET_OK) return rc;
    const bool ok = sc_parse_stream_url(body, bl, out, cap) && sc_net_host_ok(out);
    free(body);
    return ok ? SC_NET_OK : SC_NET_E_HTTP;
}
