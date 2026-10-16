#include "sc_hls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_MAX_LEN 2304
#define CHUNK 1024

static bool starts(const char *p, const char *e, const char *lit) {
    const size_t n = strlen(lit);
    return (size_t)(e - p) >= n && memcmp(p, lit, n) == 0;
}

// Value of ATTR="..." or ATTR=... in an attribute list.  False when absent.
static bool attr(const char *p, const char *e, const char *name, const char **v, size_t *vl) {
    const size_t nl = strlen(name);
    for (const char *q = p; q + nl + 1 <= e; q++) {
        if ((q == p || q[-1] == ',' || q[-1] == ':') && memcmp(q, name, nl) == 0 && q[nl] == '=') {
            const char *s = q + nl + 1;
            if (s < e && *s == '"') {
                s++;
                const char *t = s;
                while (t < e && *t != '"') t++;
                *v = s; *vl = (size_t)(t - s);
            } else {
                const char *t = s;
                while (t < e && *t != ',') t++;
                *v = s; *vl = (size_t)(t - s);
            }
            return true;
        }
    }
    return false;
}

static uint32_t parse_ms(const char *p, const char *e) {       // "10.007800" -> 10008 (rounded, so 350 segments do not drift)
    uint32_t whole = 0, frac = 0, scale = 100000;               // frac in microseconds
    while (p < e && *p >= '0' && *p <= '9') { whole = whole * 10 + (uint32_t)(*p - '0'); if (whole > 86400) whole = 86400; p++; }
    if (p < e && *p == '.') {
        p++;
        while (p < e && *p >= '0' && *p <= '9' && scale > 0) { frac += (uint32_t)(*p - '0') * scale; scale /= 10; p++; }
    }
    return whole * 1000 + (frac + 500) / 1000;
}

typedef struct {
    sc_hls   *h;
    int       cap;                 // capacity of dur_ms
    uint32_t  pending;
    bool      have_dur;
    int       rc;
    bool      irregular;
} B;

// ---- URI templates ---------------------------------------------------------------------------------------------

// Splits "<pre><digits><suf>" at the last run of digits in the file name before its extension.
static bool make_template(sc_hls *h, const char *u, size_t n) {
    size_t q = n;                                   // the path ends at '?' or '#'
    for (size_t i = 0; i < n; i++) if (u[i] == '?' || u[i] == '#') { q = i; break; }
    size_t slash = 0;
    for (size_t i = 0; i < q; i++) if (u[i] == '/') slash = i + 1;
    size_t dot = q;
    for (size_t i = slash; i < q; i++) if (u[i] == '.') dot = i;
    size_t e = dot;                                  // the digits end where the extension begins
    size_t s = e;
    while (s > slash && u[s - 1] >= '0' && u[s - 1] <= '9') s--;
    if (s == e || e - s > 9) return false;
    long v = 0;
    for (size_t i = s; i < e; i++) v = v * 10 + (u[i] - '0');
    h->tmpl_pre = (char *)malloc(s + 1);
    h->tmpl_suf = (char *)malloc(n - e + 1);
    if (!h->tmpl_pre || !h->tmpl_suf) return false;
    memcpy(h->tmpl_pre, u, s); h->tmpl_pre[s] = 0;
    memcpy(h->tmpl_suf, u + e, n - e); h->tmpl_suf[n - e] = 0;
    h->tmpl_width = (int)(e - s);
    h->first_no = v;
    return true;
}

static size_t expand(const sc_hls *h, int i, char *out, size_t cap) {      // the URI of segment i from the template
    return (size_t)snprintf(out, cap, "%s%0*ld%s", h->tmpl_pre, h->tmpl_width, h->first_no + i, h->tmpl_suf);
}

static bool arena_add(sc_hls *h, const char *u, size_t n, int idx) {
    if (h->arena_len + n + 1 > SC_HLS_ARENA_MAX) return false;
    if (h->arena_len + n + 1 > h->arena_cap) {
        size_t nc = h->arena_cap ? h->arena_cap * 2 : 8192;
        while (nc < h->arena_len + n + 1) nc *= 2;
        if (nc > SC_HLS_ARENA_MAX) nc = SC_HLS_ARENA_MAX;
        char *g = (char *)realloc(h->arena, nc);
        if (!g) return false;
        h->arena = g; h->arena_cap = nc;
    }
    uint32_t *o = (uint32_t *)realloc(h->arena_off, sizeof(uint32_t) * (size_t)(idx + 1));
    if (!o) return false;
    h->arena_off = o;
    h->arena_off[idx] = (uint32_t)h->arena_len;
    memcpy(h->arena + h->arena_len, u, n);
    h->arena[h->arena_len + n] = 0;
    h->arena_len += n + 1;
    return true;
}

// Segment URI number idx arrives.
static bool add_uri(B *b, const char *u, size_t n) {
    sc_hls *h = b->h;
    const int idx = h->nseg;
    if (!b->irregular) {
        if (idx == 0) {
            if (make_template(h, u, n)) return true;
            b->irregular = true;                        // no counter to follow: keep every URI
        } else {
            char tmp[LINE_MAX_LEN + 16];
            const size_t l = expand(h, idx, tmp, sizeof(tmp));
            if (l == n && memcmp(tmp, u, n) == 0) return true;
            // The pattern broke: keep what came before (rebuilt from the template) and every URI from here on.
            b->irregular = true;
            for (int k = 0; k < idx; k++) {
                const size_t kl = expand(h, k, tmp, sizeof(tmp));
                if (!arena_add(h, tmp, kl, k)) return false;
            }
        }
    }
    return arena_add(h, u, n, idx);
}

// ---- lines -----------------------------------------------------------------------------------------------------

static void line(B *b, const char *ls, const char *le) {
    sc_hls *h = b->h;
    while (le > ls && (le[-1] == '\r' || le[-1] == ' ')) le--;
    if (le == ls || b->rc) return;
    if (*ls == '#') {
        if (starts(ls, le, "#EXT-X-KEY:") || starts(ls, le, "#EXT-X-SESSION-KEY:")) {
            const char *v; size_t vl;
            const char *a = (const char *)memchr(ls, ':', (size_t)(le - ls)) + 1;
            if (!attr(a, le, "METHOD", &v, &vl) || !(vl == 4 && memcmp(v, "NONE", 4) == 0)) b->rc = SC_HLS_E_ENCRYPTED;
        } else if (starts(ls, le, "#EXT-X-STREAM-INF") || starts(ls, le, "#EXT-X-MEDIA:")) {
            b->rc = SC_HLS_E_MASTER;
        } else if (starts(ls, le, "#EXT-X-BYTERANGE")) {
            b->rc = SC_HLS_E_BYTERANGE;
        } else if (starts(ls, le, "#EXT-X-MAP:")) {
            const char *v; size_t vl;
            if (attr(ls + 11, le, "URI", &v, &vl) && vl > 0 && vl < LINE_MAX_LEN) {
                free(h->map_uri);
                h->map_uri = (char *)malloc(vl + 1);
                if (!h->map_uri) { b->rc = SC_HLS_E_MEM; return; }
                memcpy(h->map_uri, v, vl); h->map_uri[vl] = 0;
            }
            if (attr(ls + 11, le, "BYTERANGE", &v, &vl)) b->rc = SC_HLS_E_BYTERANGE;
        } else if (starts(ls, le, "#EXTINF:")) {
            b->pending = parse_ms(ls + 8, le);
            b->have_dur = true;
        } else if (starts(ls, le, "#EXT-X-TARGETDURATION:")) {
            h->target_ms = parse_ms(ls + 22, le);
        } else if (starts(ls, le, "#EXT-X-ENDLIST")) {
            h->ended = true;
        }
        return;
    }
    if (!b->have_dur) return;                           // a stray line without EXTINF
    if (h->nseg >= SC_HLS_MAX_SEGS) { b->rc = SC_HLS_E_TOOMANY; return; }
    if (!add_uri(b, ls, (size_t)(le - ls))) { b->rc = b->irregular ? SC_HLS_E_TOOMANY : SC_HLS_E_MEM; return; }
    if (h->nseg == b->cap) {
        const int nc = b->cap ? b->cap * 2 : 64;
        uint32_t *g = (uint32_t *)realloc(h->dur_ms, sizeof(uint32_t) * (size_t)nc);
        if (!g) { b->rc = SC_HLS_E_MEM; return; }
        h->dur_ms = g; b->cap = nc;
    }
    h->dur_ms[h->nseg++] = b->pending;
    h->total_ms += b->pending;
    b->have_dur = false;
}

void sc_hls_free(sc_hls *h) {
    free(h->dur_ms); free(h->map_uri); free(h->tmpl_pre); free(h->tmpl_suf); free(h->arena); free(h->arena_off);
    memset(h, 0, sizeof(*h));
}

int sc_hls_read(sc_hls_read_fn rd, void *ctx, sc_hls *out) {
    memset(out, 0, sizeof(*out));
    B b;
    memset(&b, 0, sizeof(b));
    b.h = out;
    char *ln = (char *)malloc(LINE_MAX_LEN + 1);
    char *chunk = (char *)malloc(CHUNK);
    if (!ln || !chunk) { free(ln); free(chunk); return SC_HLS_E_MEM; }
    size_t ll = 0;
    bool first = true, magic = false, overlong = false;
    int rc = SC_HLS_OK;
    for (;;) {
        const int n = rd(ctx, chunk, CHUNK);
        if (n < 0) { rc = SC_HLS_E_READ; break; }
        if (n == 0) { if (ll && !overlong) { ln[ll] = 0; line(&b, ln, ln + ll); } break; }
        for (int i = 0; i < n && !b.rc; i++) {
            const char c = chunk[i];
            if (c == '\n') {
                if (overlong) { b.rc = SC_HLS_E_NOTM3U; break; }
                if (first) {                                  // the first line must be #EXTM3U (after an optional BOM)
                    size_t s = (ll >= 3 && (unsigned char)ln[0] == 0xEF && (unsigned char)ln[1] == 0xBB && (unsigned char)ln[2] == 0xBF) ? 3 : 0;
                    if (!starts(ln + s, ln + ll, "#EXTM3U")) { b.rc = SC_HLS_E_NOTM3U; break; }
                    first = false; magic = true;
                } else {
                    line(&b, ln, ln + ll);
                }
                ll = 0;
            } else if (ll < LINE_MAX_LEN) {
                ln[ll++] = c;
            } else {
                overlong = true;
            }
        }
        if (b.rc) break;
    }
    if (first && rc == SC_HLS_OK && !b.rc) {
        // no newline at all: the one line may still be the whole file
        size_t s = (ll >= 3 && (unsigned char)ln[0] == 0xEF && (unsigned char)ln[1] == 0xBB && (unsigned char)ln[2] == 0xBF) ? 3 : 0;
        if (!starts(ln + s, ln + ll, "#EXTM3U")) b.rc = SC_HLS_E_NOTM3U; else magic = true;
    }
    free(ln);
    free(chunk);
    if (rc == SC_HLS_OK) rc = b.rc;
    if (rc == SC_HLS_OK && !magic) rc = SC_HLS_E_NOTM3U;
    if (rc == SC_HLS_OK && out->nseg == 0) rc = SC_HLS_E_EMPTY;
    if (rc != SC_HLS_OK) { sc_hls_free(out); return rc; }
    return SC_HLS_OK;
}

typedef struct { const char *p; size_t n, pos; } MemRd;
static int mem_read(void *c, void *buf, int n) {
    MemRd *m = (MemRd *)c;
    size_t k = m->n - m->pos;
    if (k > (size_t)n) k = (size_t)n;
    memcpy(buf, m->p + m->pos, k);
    m->pos += k;
    return (int)k;
}

int sc_hls_parse(const char *text, size_t len, sc_hls *out) {
    MemRd m = { text, len, 0 };
    return sc_hls_read(mem_read, &m, out);
}

bool sc_hls_seg_url(const sc_hls *h, int i, const char *base, char *out, size_t cap) {
    if (i < 0) {
        if (!h->map_uri) return false;
        return sc_url_join(base, h->map_uri, strlen(h->map_uri), out, cap);
    }
    if (i >= h->nseg) return false;
    char tmp[LINE_MAX_LEN + 16];
    const char *u;
    size_t ul;
    if (h->arena) {
        u = h->arena + h->arena_off[i];
        ul = strlen(u);
    } else {
        ul = expand(h, i, tmp, sizeof(tmp));
        if (ul >= sizeof(tmp)) return false;
        u = tmp;
    }
    return sc_url_join(base, u, ul, out, cap);
}

int sc_hls_seek(const sc_hls *h, uint32_t t_ms, uint32_t *into_ms) {
    uint32_t acc = 0;
    for (int i = 0; i < h->nseg; i++) {
        if (t_ms < acc + h->dur_ms[i] || i == h->nseg - 1) {
            if (into_ms) *into_ms = t_ms > acc ? (t_ms - acc < h->dur_ms[i] ? t_ms - acc : h->dur_ms[i]) : 0;
            return i;
        }
        acc += h->dur_ms[i];
    }
    if (into_ms) *into_ms = 0;
    return 0;
}

// ---- URL join ------------------------------------------------------------------------------------------------------

static bool has_scheme(const char *s, size_t n) {
    return (n >= 7 && memcmp(s, "http://", 7) == 0) || (n >= 8 && memcmp(s, "https://", 8) == 0);
}

bool sc_url_join(const char *base, const char *ref, size_t rl, char *out, size_t cap) {
    if (!cap) return false;
    if (has_scheme(ref, rl)) {
        if (rl + 1 > cap) return false;
        memcpy(out, ref, rl); out[rl] = 0;
        return true;
    }
    const size_t bl = base ? strlen(base) : 0;
    if (!has_scheme(base ? base : "", bl)) return false;
    const char *after = strstr(base, "://") + 3;
    const char *path = strchr(after, '/');
    size_t origin = path ? (size_t)(path - base) : bl;            // up to the first '/' after the host
    const char *q = base + bl;                                    // the query and fragment never join
    for (const char *c = base; c < base + bl; c++) if (*c == '?' || *c == '#') { q = c; break; }
    size_t blen = (size_t)(q - base);
    size_t used;
    if (rl >= 2 && ref[0] == '/' && ref[1] == '/') {              // scheme-relative
        const size_t sch = (size_t)(after - base) - 2;             // "https:"
        if (sch + rl + 1 > cap) return false;
        memcpy(out, base, sch); memcpy(out + sch, ref, rl); used = sch + rl;
    } else if (rl >= 1 && ref[0] == '/') {                        // root-relative
        if (origin + rl + 1 > cap) return false;
        memcpy(out, base, origin); memcpy(out + origin, ref, rl); used = origin + rl;
    } else {                                                      // relative to the base's directory
        size_t dir = origin;
        for (size_t k = blen; k > origin; k--) if (base[k - 1] == '/') { dir = k; break; }
        if (dir == origin) { if (origin + 1 + rl + 1 > cap) return false; memcpy(out, base, origin); out[origin] = '/'; memcpy(out + origin + 1, ref, rl); used = origin + 1 + rl; }
        else { if (dir + rl + 1 > cap) return false; memcpy(out, base, dir); memcpy(out + dir, ref, rl); used = dir + rl; }
    }
    out[used] = 0;
    return true;
}
