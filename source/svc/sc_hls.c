#include "sc_hls.h"
#include <stdlib.h>
#include <string.h>

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
    uint32_t whole = 0, frac = 0, scale = 100000;                 // frac in microseconds
    while (p < e && *p >= '0' && *p <= '9') { whole = whole * 10 + (uint32_t)(*p - '0'); if (whole > 86400) whole = 86400; p++; }
    if (p < e && *p == '.') {
        p++;
        while (p < e && *p >= '0' && *p <= '9' && scale > 0) { frac += (uint32_t)(*p - '0') * scale; scale /= 10; p++; }
    }
    return whole * 1000 + (frac + 500) / 1000;
}

int sc_hls_parse(const char *text, size_t len, sc_hls *out) {
    memset(out, 0, sizeof(*out));
    const char *p = text, *end = text + len;
    if (len > 0xFFFFFFF0u) return SC_HLS_E_NOTM3U;
    if (len >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
    if (!starts(p, end, "#EXTM3U")) return SC_HLS_E_NOTM3U;

    int cap = 64, n = 0;
    sc_hls_seg *seg = (sc_hls_seg *)malloc(sizeof(*seg) * (size_t)cap);
    if (!seg) return SC_HLS_E_MEM;
    uint32_t pending = 0, total = 0, target = 0, map_off = 0, map_len = 0;
    bool have_dur = false, ended = false;
    int rc = SC_HLS_OK;

    while (p < end && rc == SC_HLS_OK) {
        const char *ls = p;
        while (p < end && *p != '\n') p++;
        const char *le = p;
        if (p < end) p++;
        while (le > ls && (le[-1] == '\r' || le[-1] == ' ')) le--;
        if (le == ls) continue;

        if (*ls == '#') {
            if (starts(ls, le, "#EXT-X-KEY:") || starts(ls, le, "#EXT-X-SESSION-KEY:")) {
                const char *v; size_t vl;
                const char *a = (const char *)memchr(ls, ':', (size_t)(le - ls)) + 1;
                if (!attr(a, le, "METHOD", &v, &vl) || !(vl == 4 && memcmp(v, "NONE", 4) == 0)) rc = SC_HLS_E_ENCRYPTED;
            } else if (starts(ls, le, "#EXT-X-STREAM-INF") || starts(ls, le, "#EXT-X-MEDIA:")) {
                rc = SC_HLS_E_MASTER;
            } else if (starts(ls, le, "#EXT-X-BYTERANGE")) {
                rc = SC_HLS_E_BYTERANGE;
            } else if (starts(ls, le, "#EXT-X-MAP:")) {
                const char *v; size_t vl;
                if (attr(ls + 11, le, "URI", &v, &vl) && vl < 0xFFFF) { map_off = (uint32_t)(v - text); map_len = (uint32_t)vl; }
                const char *b;
                if (attr(ls + 11, le, "BYTERANGE", &b, &vl)) rc = SC_HLS_E_BYTERANGE;
            } else if (starts(ls, le, "#EXTINF:")) {
                pending = parse_ms(ls + 8, le);
                have_dur = true;
            } else if (starts(ls, le, "#EXT-X-TARGETDURATION:")) {
                target = parse_ms(ls + 22, le);
            } else if (starts(ls, le, "#EXT-X-ENDLIST")) {
                ended = true;
            }
            continue;
        }

        // A URI line.
        if (!have_dur) continue;                       // a stray line without EXTINF
        if (le - ls >= 0xFFFF) { rc = SC_HLS_E_NOTM3U; break; }
        if (n >= SC_HLS_MAX_SEGS) { rc = SC_HLS_E_TOOMANY; break; }
        if (n == cap) {
            cap *= 2;
            sc_hls_seg *g = (sc_hls_seg *)realloc(seg, sizeof(*seg) * (size_t)cap);
            if (!g) { rc = SC_HLS_E_MEM; break; }
            seg = g;
        }
        seg[n].off = (uint32_t)(ls - text);
        seg[n].len = (uint16_t)(le - ls);
        seg[n].pad = 0;
        seg[n].dur_ms = pending;
        n++;
        total += pending;
        have_dur = false;
    }

    if (rc == SC_HLS_OK && n == 0) rc = SC_HLS_E_EMPTY;
    if (rc != SC_HLS_OK) { free(seg); return rc; }

    out->text = text;
    out->map_off = map_off; out->map_len = map_len;
    out->nseg = n; out->seg = seg;
    out->total_ms = total; out->target_ms = target; out->ended = ended;
    return SC_HLS_OK;
}

void sc_hls_free(sc_hls *h) {
    free(h->seg);
    memset(h, 0, sizeof(*h));
}

bool sc_hls_seg_url(const sc_hls *h, int i, const char *base, char *out, size_t cap) {
    if (i < 0) {
        if (!h->map_len) return false;
        return sc_url_join(base, h->text + h->map_off, h->map_len, out, cap);
    }
    if (i >= h->nseg) return false;
    return sc_url_join(base, h->text + h->seg[i].off, h->seg[i].len, out, cap);
}

int sc_hls_seek(const sc_hls *h, uint32_t t_ms, uint32_t *into_ms) {
    uint32_t acc = 0;
    for (int i = 0; i < h->nseg; i++) {
        if (t_ms < acc + h->seg[i].dur_ms || i == h->nseg - 1) {
            if (into_ms) *into_ms = t_ms > acc ? (t_ms - acc < h->seg[i].dur_ms ? t_ms - acc : h->seg[i].dur_ms) : 0;
            return i;
        }
        acc += h->seg[i].dur_ms;
    }
    if (into_ms) *into_ms = 0;
    return 0;
}

// ---- URL join ------------------------------------------------------------------------------

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
