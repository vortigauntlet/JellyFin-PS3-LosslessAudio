#include "sx_httpparse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// ---------------------------------------------------------------------------
// URLs
// ---------------------------------------------------------------------------

static bool ci_prefix(const char *s, const char *prefix) {
    for (; *prefix; s++, prefix++) {
        if (tolower((unsigned char)*s) != *prefix) return false;
    }
    return true;
}

static bool path_char_needs_escape(unsigned char c) {
    if (c <= 0x20 || c >= 0x7f) return true;
    switch (c) {
        case '"': case '<': case '>': case '\\': case '^': case '`':
        case '{': case '|': case '}':
            return true;
        default:
            return false;
    }
}

// Copies src[0..n) into dst at *len with percent-escaping; false on overflow.
static bool put_escaped(char *dst, size_t cap, size_t *len, const char *src, size_t n) {
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)src[i];
        if (path_char_needs_escape(c)) {
            if (*len + 3 >= cap) return false;
            dst[(*len)++] = '%';
            dst[(*len)++] = hex[c >> 4];
            dst[(*len)++] = hex[c & 15];
        } else {
            if (*len + 1 >= cap) return false;
            dst[(*len)++] = (char)c;
        }
    }
    return true;
}

bool sx_url_parse(const char *url, sx_url *out) {
    if (!url || !out) return false;
    memset(out, 0, sizeof(*out));
    const char *p;
    if (ci_prefix(url, "https://"))      { out->tls = true;  p = url + 8; }
    else if (ci_prefix(url, "http://"))  { out->tls = false; p = url + 7; }
    else return false;

    const char *auth = p;
    while (*p && *p != '/' && *p != '?' && *p != '#') p++;
    size_t alen = (size_t)(p - auth);
    if (alen == 0) return false;
    for (size_t i = 0; i < alen; i++) {
        if (auth[i] == '@' || auth[i] == '[' || auth[i] == ' ') return false;
    }
    const char *colon = NULL;
    for (size_t i = 0; i < alen; i++) if (auth[i] == ':') colon = auth + i;
    size_t hlen = colon ? (size_t)(colon - auth) : alen;
    out->port = out->tls ? 443 : 80;
    if (colon) {
        const char *d = colon + 1;
        if (d == auth + alen) return false;
        long port = 0;
        for (; d < auth + alen; d++) {
            if (*d < '0' || *d > '9') return false;
            port = port * 10 + (*d - '0');
            if (port > 65535) return false;
        }
        if (port < 1) return false;
        out->port = (int)port;
    }
    if (hlen == 0 || hlen >= SX_HOST_MAX) return false;
    for (size_t i = 0; i < hlen; i++) out->host[i] = (char)tolower((unsigned char)auth[i]);
    out->host[hlen] = '\0';

    // Path and query, up to the fragment.
    const char *rest = p;
    const char *end = rest;
    while (*end && *end != '#') end++;
    size_t len = 0;
    if (rest == end || *rest == '?') out->path[len++] = '/';
    if (!put_escaped(out->path, sizeof(out->path), &len, rest, (size_t)(end - rest))) return false;
    out->path[len] = '\0';
    return true;
}

void sx_url_format(const sx_url *u, char *out, size_t cap) {
    const int def = u->tls ? 443 : 80;
    if (u->port == def)
        snprintf(out, cap, "%s://%s%s", u->tls ? "https" : "http", u->host, u->path);
    else
        snprintf(out, cap, "%s://%s:%d%s", u->tls ? "https" : "http", u->host, u->port, u->path);
}

// Removes "." and ".." segments from the path part of `path` (query kept).
static void remove_dot_segments(char *path) {
    char *q = strchr(path, '?');
    char query[SX_URL_MAX] = "";
    if (q) { snprintf(query, sizeof(query), "%s", q); *q = '\0'; }

    char out[SX_URL_MAX];
    size_t olen = 0;
    const char *s = path;
    while (*s) {
        // s points at '/'
        const char *seg = s + 1;
        const char *e = strchr(seg, '/');
        size_t slen = e ? (size_t)(e - seg) : strlen(seg);
        if (slen == 1 && seg[0] == '.') {
            // skip
        } else if (slen == 2 && seg[0] == '.' && seg[1] == '.') {
            while (olen > 0 && out[olen - 1] != '/') olen--;
            if (olen > 0) olen--;                       // drop the '/' too
        } else if (olen + 1 + slen < sizeof(out)) {
            out[olen++] = '/';
            memcpy(out + olen, seg, slen);
            olen += slen;
        }
        s = e ? e : seg + slen;
    }
    if (olen == 0) out[olen++] = '/';
    else if (path[strlen(path) - 1] == '/' && out[olen - 1] != '/' && olen + 1 < sizeof(out)) out[olen++] = '/';
    out[olen] = '\0';
    const size_t qlen = strlen(query);
    if (olen + qlen >= SX_URL_MAX) {         // cannot happen for a path that fitted; never overflow
        path[0] = '/'; path[1] = '\0';
        return;
    }
    memcpy(path, out, olen);
    memcpy(path + olen, query, qlen + 1);
}

bool sx_url_resolve(const sx_url *base, const char *loc, char *out, size_t cap) {
    if (!base || !loc || !*loc || !out || cap == 0) return false;
    sx_url u;
    if (ci_prefix(loc, "http://") || ci_prefix(loc, "https://")) {
        if (!sx_url_parse(loc, &u)) return false;
        sx_url_format(&u, out, cap);
        return true;
    }
    if (loc[0] == '/' && loc[1] == '/') {
        char tmp[SX_URL_MAX + 16];
        snprintf(tmp, sizeof(tmp), "%s:%s", base->tls ? "https" : "http", loc);
        if (!sx_url_parse(tmp, &u)) return false;
        sx_url_format(&u, out, cap);
        return true;
    }
    u = *base;
    char merged[SX_URL_MAX * 2];
    if (loc[0] == '/') {
        snprintf(merged, sizeof(merged), "%s", loc);
    } else if (loc[0] == '?') {
        char *q = strchr(u.path, '?');
        if (q) *q = '\0';
        snprintf(merged, sizeof(merged), "%s%s", u.path, loc);
    } else {
        char dir[SX_URL_MAX];
        snprintf(dir, sizeof(dir), "%s", u.path);
        char *q = strchr(dir, '?');
        if (q) *q = '\0';
        char *slash = strrchr(dir, '/');
        if (slash) slash[1] = '\0';
        snprintf(merged, sizeof(merged), "%s%s", dir, loc);
    }
    // Strip the fragment, then escape and normalise.
    char *h = strchr(merged, '#');
    if (h) *h = '\0';
    size_t len = 0;
    if (!put_escaped(u.path, sizeof(u.path), &len, merged, strlen(merged))) return false;
    u.path[len] = '\0';
    remove_dot_segments(u.path);
    sx_url_format(&u, out, cap);
    return true;
}

void sx_redact_url(const char *url, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!url) return;
    sx_url u;
    if (!sx_url_parse(url, &u)) { snprintf(out, cap, "<url>"); return; }
    const char *scheme = u.tls ? "https" : "http";
    char hp[SX_HOST_MAX + 8];
    const int def = u.tls ? 443 : 80;
    if (u.port == def) snprintf(hp, sizeof(hp), "%s", u.host);
    else               snprintf(hp, sizeof(hp), "%s:%d", u.host, u.port);

    const char *q = strchr(u.path, '?');
    size_t plen = q ? (size_t)(q - u.path) : strlen(u.path);
    int segs = 0;
    for (size_t i = 0; i < plen; i++) if (u.path[i] == '/' && i + 1 < plen) segs++;
    const bool plain = !q && segs <= 1;
    if (plain) {
        snprintf(out, cap, "%s://%s%s", scheme, hp, u.path);
        return;
    }
    const char *last = u.path;
    for (size_t i = 0; i < plen; i++) if (u.path[i] == '/') last = u.path + i + 1;
    if (!q && strcmp(last, "manifest.json") == 0)
        snprintf(out, cap, "%s://%s/<token>/manifest.json", scheme, hp);
    else
        snprintf(out, cap, "%s://%s/<token>", scheme, hp);
}

// ---------------------------------------------------------------------------
// Response head
// ---------------------------------------------------------------------------

static const char *find_head_end(const char *buf, int len) {
    for (int i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return buf + i;
    }
    return NULL;
}

static bool ci_eq_n(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}

static bool ci_contains(const char *s, size_t n, const char *needle) {
    size_t m = strlen(needle);
    for (size_t i = 0; i + m <= n; i++)
        if (ci_eq_n(s + i, needle, m)) return true;
    return false;
}

#define SX_HEAD_MAX 16384

int sx_head_parse(const char *buf, int len, sx_head *h) {
    if (!buf || !h || len < 0) return 0;
    const char *end = find_head_end(buf, len);
    if (!end) return len >= SX_HEAD_MAX ? 0 : -1;
    memset(h, 0, sizeof(*h));
    h->content_length = -1;
    h->range_start = h->range_end = h->range_total = -1;
    h->retry_after = -1;
    h->head_len = (int)(end - buf) + 4;

    // Status line: "HTTP/1.x SSS reason"
    if (h->head_len < 12 || !ci_prefix(buf, "http/1.")) return 0;
    const bool http10 = buf[7] == '0';
    const char *sp = memchr(buf, ' ', (size_t)h->head_len);
    if (!sp) return 0;
    char *ep = NULL;
    long st = strtol(sp + 1, &ep, 10);
    if (ep == sp + 1 || st < 100 || st > 599) return 0;
    h->status = (int)st;
    h->close = http10;

    const char *p = memchr(buf, '\n', (size_t)h->head_len);
    p = p ? p + 1 : end;
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end + 2 - p));
        if (!eol) eol = end + 2;
        size_t ll = (size_t)(eol - p);
        if (ll && p[ll - 1] == '\r') ll--;
        const char *colon = memchr(p, ':', ll);
        if (colon) {
            size_t nl = (size_t)(colon - p);
            const char *v = colon + 1;
            const char *ve = p + ll;
            while (v < ve && (*v == ' ' || *v == '\t')) v++;
            while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
            size_t vl = (size_t)(ve - v);

            if (nl == 14 && ci_eq_n(p, "content-length", 14)) {
                char tmp[24];
                size_t n = vl < sizeof(tmp) - 1 ? vl : sizeof(tmp) - 1;
                memcpy(tmp, v, n); tmp[n] = '\0';
                char *e2 = NULL;
                long long cl = strtoll(tmp, &e2, 10);
                if (e2 == tmp || cl < 0) return 0;
                if (h->content_length >= 0 && h->content_length != cl) return 0;   // conflicting lengths
                h->content_length = cl;
            } else if (nl == 17 && ci_eq_n(p, "transfer-encoding", 17)) {
                if (ci_contains(v, vl, "chunked")) h->chunked = true;
            } else if (nl == 10 && ci_eq_n(p, "connection", 10)) {
                if (ci_contains(v, vl, "close")) h->close = true;
                else if (ci_contains(v, vl, "keep-alive")) h->close = false;
            } else if (nl == 13 && ci_eq_n(p, "accept-ranges", 13)) {
                if (ci_contains(v, vl, "bytes")) h->accept_ranges = true;
            } else if (nl == 13 && ci_eq_n(p, "content-range", 13)) {
                // "bytes a-b/total" or "bytes */total"
                char tmp[96];
                size_t n = vl < sizeof(tmp) - 1 ? vl : sizeof(tmp) - 1;
                memcpy(tmp, v, n); tmp[n] = '\0';
                long long a = -1, b = -1, t = -1;
                if (sscanf(tmp, "bytes %lld-%lld/%lld", &a, &b, &t) == 3) {
                    h->range_start = a; h->range_end = b; h->range_total = t;
                } else if (sscanf(tmp, "bytes %lld-%lld/", &a, &b) == 2) {
                    h->range_start = a; h->range_end = b;
                } else if (sscanf(tmp, "bytes */%lld", &t) == 1) {
                    h->range_total = t;
                }
            } else if (nl == 8 && ci_eq_n(p, "location", 8)) {
                size_t n = vl < sizeof(h->location) - 1 ? vl : sizeof(h->location) - 1;
                memcpy(h->location, v, n); h->location[n] = '\0';
            } else if (nl == 12 && ci_eq_n(p, "content-type", 12)) {
                size_t n = vl < sizeof(h->content_type) - 1 ? vl : sizeof(h->content_type) - 1;
                memcpy(h->content_type, v, n); h->content_type[n] = '\0';
            } else if (nl == 11 && ci_eq_n(p, "retry-after", 11)) {
                char tmp[16];
                size_t n = vl < sizeof(tmp) - 1 ? vl : sizeof(tmp) - 1;
                memcpy(tmp, v, n); tmp[n] = '\0';
                char *e2 = NULL;
                long s = strtol(tmp, &e2, 10);
                if (e2 != tmp && s >= 0 && s < 86400) h->retry_after = (int)s;
            }
        }
        p = eol + 1;
    }
    if (h->chunked) h->content_length = -1;   // chunked wins over a length (RFC 9112 6.3)
    return 1;
}

// ---------------------------------------------------------------------------
// Chunked decoding
// ---------------------------------------------------------------------------

enum { CK_SIZE, CK_EXT, CK_LF1, CK_DATA, CK_DATA_CR, CK_DATA_LF, CK_TRAILER, CK_DONE };

void sx_chunk_init(sx_chunk *c) { memset(c, 0, sizeof(*c)); c->state = CK_SIZE; }

static int hex_val(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int sx_chunk_feed(sx_chunk *c, const uint8_t *in, int in_n, int *in_used,
                  uint8_t *out, int out_cap, int *out_n) {
    int i = 0, o = 0;
    while (i < in_n && c->state != CK_DONE) {
        const uint8_t b = in[i];
        switch (c->state) {
        case CK_SIZE: {
            const int hv = hex_val(b);
            if (hv >= 0) {
                if (c->left > ((int64_t)1 << 40)) { *in_used = i; *out_n = o; return -1; }
                c->left = c->left * 16 + hv;
                c->any_digit = true;
                i++;
            } else if (b == ';' || b == ' ' || b == '\t') {
                if (!c->any_digit) { *in_used = i; *out_n = o; return -1; }
                c->state = CK_EXT; i++;
            } else if (b == '\r') {
                if (!c->any_digit) { *in_used = i; *out_n = o; return -1; }
                c->state = CK_LF1; i++;
            } else {
                *in_used = i; *out_n = o; return -1;
            }
            break;
        }
        case CK_EXT:
            if (b == '\r') c->state = CK_LF1;
            i++;
            break;
        case CK_LF1:
            if (b != '\n') { *in_used = i; *out_n = o; return -1; }
            i++;
            c->any_digit = false;
            if (c->left == 0) { c->state = CK_TRAILER; c->line = 0; }
            else c->state = CK_DATA;
            break;
        case CK_DATA: {
            int room = out_cap - o;
            if (room <= 0) { *in_used = i; *out_n = o; return 0; }
            int64_t take = in_n - i;
            if (take > c->left) take = c->left;
            if (take > room) take = room;
            memcpy(out + o, in + i, (size_t)take);
            o += (int)take; i += (int)take; c->left -= take;
            if (c->left == 0) c->state = CK_DATA_CR;
            break;
        }
        case CK_DATA_CR:
            if (b != '\r') { *in_used = i; *out_n = o; return -1; }
            c->state = CK_DATA_LF; i++;
            break;
        case CK_DATA_LF:
            if (b != '\n') { *in_used = i; *out_n = o; return -1; }
            c->state = CK_SIZE; c->left = 0; i++;
            break;
        case CK_TRAILER:
            if (b == '\n') {
                if (c->line == 0) c->state = CK_DONE;
                c->line = 0;
            } else if (b != '\r') {
                c->line++;
            }
            i++;
            break;
        }
    }
    *in_used = i;
    *out_n = o;
    return c->state == CK_DONE ? 1 : 0;
}
