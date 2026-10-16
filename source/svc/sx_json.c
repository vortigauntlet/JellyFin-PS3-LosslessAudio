#include "sx_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *skip_ws(const char *p, const char *e) {
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

// ---- extent ---------------------------------------------------------------------

static const char *skip_string(const char *p, const char *e) {
    // p at the opening quote
    p++;
    while (p < e) {
        if (*p == '\\') { p += 2; continue; }
        if (*p == '"') return p + 1;
        p++;
    }
    return NULL;
}

static const char *skip_value(const char *p, const char *e, int depth) {
    p = skip_ws(p, e);
    if (p >= e || depth > SXJ_MAX_DEPTH) return NULL;
    const char c = *p;
    if (c == '"') return skip_string(p, e);
    if (c == '{' || c == '[') {
        const char close = c == '{' ? '}' : ']';
        p = skip_ws(p + 1, e);
        if (p < e && *p == close) return p + 1;
        for (;;) {
            if (c == '{') {
                p = skip_ws(p, e);
                if (p >= e || *p != '"') return NULL;
                p = skip_string(p, e);
                if (!p) return NULL;
                p = skip_ws(p, e);
                if (p >= e || *p != ':') return NULL;
                p++;
            }
            p = skip_value(p, e, depth + 1);
            if (!p) return NULL;
            p = skip_ws(p, e);
            if (p >= e) return NULL;
            if (*p == ',') { p++; continue; }
            if (*p == close) return p + 1;
            return NULL;
        }
    }
    if (c == 't') return (e - p >= 4 && memcmp(p, "true", 4) == 0) ? p + 4 : NULL;
    if (c == 'f') return (e - p >= 5 && memcmp(p, "false", 5) == 0) ? p + 5 : NULL;
    if (c == 'n') return (e - p >= 4 && memcmp(p, "null", 4) == 0) ? p + 4 : NULL;
    if (c == '-' || (c >= '0' && c <= '9')) {
        const char *q = p + 1;
        while (q < e && ((*q >= '0' && *q <= '9') || *q == '.' || *q == 'e' || *q == 'E' || *q == '+' || *q == '-')) q++;
        return q;
    }
    return NULL;
}

sx_jv sx_j_root(const char *buf, size_t len) {
    sx_jv v = { NULL, NULL };
    if (!buf) return v;
    const char *p = buf, *e = buf + len;
    if (e - p >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
    p = skip_ws(p, e);
    if (p >= e) return v;
    v.p = p; v.e = e;
    return v;
}

int sx_j_type(sx_jv v) {
    if (!v.p || v.p >= v.e) return SXJ_NONE;
    switch (*v.p) {
    case '"': return SXJ_STR;
    case '{': return SXJ_OBJ;
    case '[': return SXJ_ARR;
    case 't': case 'f': return SXJ_BOOL;
    case 'n': return SXJ_NULL;
    default:
        return (*v.p == '-' || (*v.p >= '0' && *v.p <= '9')) ? SXJ_NUM : SXJ_NONE;
    }
}

bool sx_j_present(sx_jv v) { return v.p != NULL; }

const char *sx_j_end(sx_jv v) {
    if (!v.p) return NULL;
    return skip_value(v.p, v.e, 0);
}

bool sx_j_valid(const char *buf, size_t len) {
    sx_jv r = sx_j_root(buf, len);
    if (!r.p) return false;
    const char *end = sx_j_end(r);
    if (!end) return false;
    return skip_ws(end, r.e) == r.e;
}

// ---- iteration -------------------------------------------------------------------

sx_jit sx_j_iter(sx_jv c) {
    sx_jit it = { NULL, NULL, false, true };
    const int t = sx_j_type(c);
    if (t != SXJ_OBJ && t != SXJ_ARR) return it;
    it.obj = (t == SXJ_OBJ);
    it.p = skip_ws(c.p + 1, c.e);
    it.e = c.e;
    it.done = (it.p >= it.e || *it.p == (it.obj ? '}' : ']'));
    return it;
}

bool sx_j_next(sx_jit *it, sx_jv *key, sx_jv *val) {
    if (!it || it->done || !it->p) return false;
    const char *p = skip_ws(it->p, it->e);
    sx_jv k = { NULL, NULL };
    if (it->obj) {
        if (p >= it->e || *p != '"') { it->done = true; return false; }
        const char *ke = skip_string(p, it->e);
        if (!ke) { it->done = true; return false; }
        k.p = p; k.e = ke;
        p = skip_ws(ke, it->e);
        if (p >= it->e || *p != ':') { it->done = true; return false; }
        p = skip_ws(p + 1, it->e);
    }
    const char *ve = skip_value(p, it->e, 1);
    if (!ve) { it->done = true; return false; }
    if (key) *key = k;
    if (val) { val->p = p; val->e = ve; }
    p = skip_ws(ve, it->e);
    if (p < it->e && *p == ',') it->p = p + 1;
    else { it->p = p; it->done = true; }
    return true;
}

// Compares a member name (a string view, quotes included) with a plain key.
static bool key_eq(sx_jv k, const char *key) {
    const char *p = k.p + 1;
    const char *e = k.e - 1;
    const size_t kl = strlen(key);
    if ((size_t)(e - p) == kl && memcmp(p, key, kl) == 0) return true;
    // A name that is escaped differently from the key we hold: decode and compare.
    bool esc = false;
    for (const char *q = p; q < e; q++) if (*q == '\\') { esc = true; break; }
    if (!esc) return false;
    char buf[128];
    if (!sx_j_str(k, buf, sizeof(buf))) return false;
    return strcmp(buf, key) == 0;
}

bool sx_j_get(sx_jv obj, const char *key, sx_jv *out) {
    if (sx_j_type(obj) != SXJ_OBJ) return false;
    sx_jit it = sx_j_iter(obj);
    sx_jv k, v;
    while (sx_j_next(&it, &k, &v)) {
        if (key_eq(k, key)) { if (out) *out = v; return true; }
    }
    return false;
}

bool sx_j_idx(sx_jv arr, int i, sx_jv *out) {
    if (sx_j_type(arr) != SXJ_ARR || i < 0) return false;
    sx_jit it = sx_j_iter(arr);
    sx_jv v;
    for (int n = 0; sx_j_next(&it, NULL, &v); n++) {
        if (n == i) { if (out) *out = v; return true; }
    }
    return false;
}

int sx_j_len(sx_jv c) {
    sx_jit it = sx_j_iter(c);
    int n = 0;
    while (sx_j_next(&it, NULL, NULL)) n++;
    return n;
}

// ---- decoding ---------------------------------------------------------------------

static int hex4(const char *p, const char *e, unsigned *out) {
    if (e - p < 4) return 0;
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        const char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

// Appends one code point if it fits whole.  False when it does not.
static bool put_utf8(char *out, size_t *n, size_t cap, unsigned cp) {
    unsigned char b[4];
    int k;
    if (cp < 0x80)          { b[0] = (unsigned char)cp; k = 1; }
    else if (cp < 0x800)    { b[0] = (unsigned char)(0xC0 | (cp >> 6)); b[1] = (unsigned char)(0x80 | (cp & 0x3F)); k = 2; }
    else if (cp < 0x10000)  { b[0] = (unsigned char)(0xE0 | (cp >> 12)); b[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F)); b[2] = (unsigned char)(0x80 | (cp & 0x3F)); k = 3; }
    else                    { b[0] = (unsigned char)(0xF0 | (cp >> 18)); b[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F)); b[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (unsigned char)(0x80 | (cp & 0x3F)); k = 4; }
    if (*n + (size_t)k + 1 > cap) return false;
    for (int i = 0; i < k; i++) out[(*n)++] = (char)b[i];
    return true;
}

bool sx_j_str(sx_jv v, char *out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (sx_j_type(v) != SXJ_STR) return false;
    const char *p = v.p + 1;
    const char *end = skip_string(v.p, v.e);
    if (!end) return false;
    const char *e = end - 1;          // the closing quote
    size_t n = 0;
    bool full = false;
    while (p < e && !full) {
        unsigned char c = (unsigned char)*p;
        if (c != '\\') {
            // Copy a whole UTF-8 sequence or nothing, so truncation never splits one.
            size_t sl = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
            if (p + sl > e) sl = (size_t)(e - p);
            if (n + sl + 1 > cap) { full = true; break; }
            memcpy(out + n, p, sl);
            n += sl; p += sl;
            continue;
        }
        p++;
        if (p >= e) break;
        const char esc = *p++;
        unsigned cp = 0;
        switch (esc) {
        case 'n': cp = '\n'; break;
        case 't': cp = '\t'; break;
        case 'r': cp = '\r'; break;
        case 'b': cp = '\b'; break;
        case 'f': cp = '\f'; break;
        case 'u': {
            unsigned u;
            if (!hex4(p, e, &u)) { cp = '?'; break; }
            p += 4;
            if (u >= 0xD800 && u <= 0xDBFF) {              // high surrogate: needs a \uDC00..DFFF
                unsigned lo;
                if (e - p >= 6 && p[0] == '\\' && p[1] == 'u' && hex4(p + 2, e, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                    p += 6;
                    u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                } else u = 0xFFFD;
            } else if (u >= 0xDC00 && u <= 0xDFFF) u = 0xFFFD;
            cp = u;
            break;
        }
        default: cp = (unsigned char)esc; break;            // " \ / and anything unknown
        }
        if (!put_utf8(out, &n, cap, cp)) { full = true; break; }
    }
    out[n] = '\0';
    return true;
}

bool sx_j_dbl(sx_jv v, double *out) {
    char tmp[48];
    const int t = sx_j_type(v);
    if (t == SXJ_NUM) {
        const char *end = skip_value(v.p, v.e, 0);
        if (!end) return false;
        size_t n = (size_t)(end - v.p);
        if (n >= sizeof(tmp)) return false;
        memcpy(tmp, v.p, n); tmp[n] = '\0';
    } else if (t == SXJ_STR) {
        if (!sx_j_str(v, tmp, sizeof(tmp))) return false;
    } else return false;
    char *ep = NULL;
    const double d = strtod(tmp, &ep);
    if (ep == tmp || *ep != '\0') return false;
    if (out) *out = d;
    return true;
}

bool sx_j_int(sx_jv v, int64_t *out) {
    char tmp[32];
    const int t = sx_j_type(v);
    if (t == SXJ_NUM) {
        const char *end = skip_value(v.p, v.e, 0);
        if (!end) return false;
        size_t n = (size_t)(end - v.p);
        if (n >= sizeof(tmp)) return false;
        memcpy(tmp, v.p, n); tmp[n] = '\0';
    } else if (t == SXJ_STR) {
        if (!sx_j_str(v, tmp, sizeof(tmp))) return false;
    } else return false;
    char *ep = NULL;
    const long long x = strtoll(tmp, &ep, 10);
    if (ep == tmp) return false;
    if (*ep == '.' || *ep == 'e' || *ep == 'E') {            // 12.0 and 1e3 are whole numbers; 12.5 is not
        double d;
        if (!sx_j_dbl(v, &d)) return false;
        if (d != (double)(int64_t)d) return false;
        if (out) *out = (int64_t)d;
        return true;
    }
    if (*ep != '\0') return false;
    if (out) *out = (int64_t)x;
    return true;
}

bool sx_j_bool(sx_jv v, bool *out) {
    if (sx_j_type(v) != SXJ_BOOL) return false;
    if (out) *out = (*v.p == 't');
    return true;
}

bool sx_j_get_str(sx_jv obj, const char *key, char *out, size_t cap) {
    sx_jv v;
    if (!sx_j_get(obj, key, &v)) { if (out && cap) out[0] = '\0'; return false; }
    return sx_j_str(v, out, cap);
}

int64_t sx_j_get_int(sx_jv obj, const char *key, int64_t def) {
    sx_jv v;
    int64_t x;
    if (sx_j_get(obj, key, &v) && sx_j_int(v, &x)) return x;
    return def;
}

bool sx_j_get_bool(sx_jv obj, const char *key, bool def) {
    sx_jv v;
    bool b;
    if (sx_j_get(obj, key, &v) && sx_j_bool(v, &b)) return b;
    return def;
}

bool sx_json_escape(const char *in, char *out, size_t cap) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        char tmp[8];
        size_t k;
        switch (*p) {
        case '"':  memcpy(tmp, "\\\"", 2); k = 2; break;
        case '\\': memcpy(tmp, "\\\\", 2); k = 2; break;
        case '\n': memcpy(tmp, "\\n", 2); k = 2; break;
        case '\r': memcpy(tmp, "\\r", 2); k = 2; break;
        case '\t': memcpy(tmp, "\\t", 2); k = 2; break;
        default:
            if (*p < 0x20) { snprintf(tmp, sizeof(tmp), "\\u%04x", *p); k = 6; }
            else { tmp[0] = (char)*p; k = 1; }
        }
        if (n + k + 1 > cap) { if (cap) out[0] = '\0'; return false; }
        memcpy(out + n, tmp, k);
        n += k;
    }
    if (n + 1 > cap) return false;
    out[n] = '\0';
    return true;
}
