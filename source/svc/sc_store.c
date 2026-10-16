#include "sc_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool is_alnum(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// The value after "key " on a line, trimmed of the line ending and surrounding spaces.  False if the line is not that key.
static bool line_value(const char *ls, const char *le, const char *key, char *out, size_t cap) {
    const size_t kl = strlen(key);
    if ((size_t)(le - ls) <= kl || memcmp(ls, key, kl) != 0 || (ls[kl] != ' ' && ls[kl] != '\t')) return false;
    const char *v = ls + kl;
    while (v < le && (*v == ' ' || *v == '\t')) v++;
    while (le > v && (le[-1] == ' ' || le[-1] == '\t' || le[-1] == '\r')) le--;
    size_t n = (size_t)(le - v);
    if (n >= cap) n = cap - 1;
    memcpy(out, v, n);
    out[n] = 0;
    return true;
}

static void for_each_line(const char *text, void (*fn)(const char *, const char *, void *), void *ud) {
    const char *p = text;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
    while (*p) {
        const char *ls = p;
        while (*p && *p != '\n') p++;
        fn(ls, p, ud);
        if (*p) p++;
    }
}

static void cid_line(const char *ls, const char *le, void *ud) {
    ScCidFile *o = (ScCidFile *)ud;
    char v[64];
    if (line_value(ls, le, "cid", v, sizeof(v))) {
        bool ok = strlen(v) == 32;
        for (int i = 0; ok && i < 32; i++) ok = is_alnum(v[i]);
        if (ok) memcpy(o->cid, v, 33);
    } else if (line_value(ls, le, "at", v, sizeof(v))) {
        o->at = atol(v);
    }
}

void sc_cidfile_parse(const char *text, ScCidFile *out) {
    memset(out, 0, sizeof(*out));
    if (text) for_each_line(text, cid_line, out);
}

int sc_cidfile_format(const ScCidFile *in, char *out, size_t cap) {
    if (strlen(in->cid) != 32) return -1;
    const int n = snprintf(out, cap, "cid %s\nat %ld\n", in->cid, in->at);
    return n > 0 && (size_t)n < cap ? n : -1;
}

void sc_settings_defaults(ScSettings *out) {
    memset(out, 0, sizeof(*out));
    out->quality = 160;
}

static void set_line(const char *ls, const char *le, void *ud) {
    ScSettings *o = (ScSettings *)ud;
    char v[128];
    if (line_value(ls, le, "profile", v, sizeof(v))) {
        // A profile name has no spaces or control characters; anything else is a damaged file.
        bool ok = v[0] != 0;
        for (const char *c = v; *c && ok; c++) ok = (unsigned char)*c > ' ' && *c != '/' && *c != '?' && *c != '#';
        if (ok && strlen(v) < sizeof(o->profile)) memcpy(o->profile, v, strlen(v) + 1);
    } else if (line_value(ls, le, "quality", v, sizeof(v))) {
        const int q = atoi(v);
        if (q == 96 || q == 160) o->quality = q;
    }
}

void sc_settings_parse(const char *text, ScSettings *out) {
    sc_settings_defaults(out);
    if (text) for_each_line(text, set_line, out);
}

int sc_settings_format(const ScSettings *in, char *out, size_t cap) {
    const int n = snprintf(out, cap, "profile %s\nquality %d\n", in->profile, in->quality == 96 ? 96 : 160);
    return n > 0 && (size_t)n < cap ? n : -1;
}
