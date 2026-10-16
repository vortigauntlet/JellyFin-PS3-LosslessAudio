#include "sc_jstream.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK 2048
#define KEY_MAX 64

enum { P_KEY, P_COLON, P_VALUE, P_AFTER };       // an object root's member states (depth 1)
enum { E_VALUE, E_AFTER };                       // an iterated array's element states

typedef struct {
    const sc_jcb *cb;
    uint8_t *w;                                  // the value being captured
    size_t cap, wlen;
    bool overflow;

    bool in_str, esc;
    bool root_known, root_array, done, stopped;
    int  depth;                                  // open containers
    int  phase;                                  // object root
    int  ephase;                                 // iterated array
    bool iter;                                   // inside an array whose elements are delivered
    char key[KEY_MAX];
    int  keylen;
    bool key_capturing;
    char iter_key[KEY_MAX];

    bool cap_active, cap_container, cap_string, cap_is_elem;
    int  cap_base;                               // the depth the captured value sits at
    sc_jstats *st;
    int  err;
} J;

static bool is_ws(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static bool is_delim(unsigned char c) { return c == ',' || c == '}' || c == ']' || is_ws(c); }

static void put(J *j, unsigned char c) {
    if (j->wlen < j->cap) j->w[j->wlen++] = c;
    else j->overflow = true;
}

static void end_cap(J *j) {
    j->cap_active = false;
    if (j->overflow) { j->st->skipped++; return; }
    if (j->cap_is_elem) {
        j->st->elements++;
        if (j->cb->elem && !j->cb->elem(j->cb->ud, j->root_array ? "" : j->iter_key, (const char *)j->w, j->wlen)) {
            j->stopped = true;
            j->st->stopped = true;
        }
    } else if (j->cb->member) {
        j->cb->member(j->cb->ud, j->key, (const char *)j->w, j->wlen);
    }
}

// The first byte of a value (a member's, or an element's) arrives.
static void start_value(J *j, bool elem, unsigned char c) {
    j->cap_active = true;
    j->cap_is_elem = elem;
    j->cap_base = j->depth;
    j->cap_container = (c == '{' || c == '[');
    j->cap_string = (c == '"');
    j->wlen = 0;
    j->overflow = false;
    put(j, c);
    if (j->cap_container) j->depth++;
    else if (j->cap_string) { j->in_str = true; j->esc = false; }
}

static void feed(J *j, unsigned char c) {
    // ---- inside a string
    if (j->in_str) {
        if (j->key_capturing) {
            if (!j->esc && c == '"') { j->key[j->keylen] = 0; j->key_capturing = false; j->in_str = false; j->phase = P_COLON; return; }
            if (j->keylen < KEY_MAX - 1) j->key[j->keylen++] = (char)c;
        } else if (j->cap_active) {
            put(j, c);
        }
        if (j->esc) j->esc = false;
        else if (c == '\\') j->esc = true;
        else if (c == '"' && !j->key_capturing) {
            j->in_str = false;
            if (j->cap_active && j->cap_string) end_cap(j);          // a scalar string ends here; a container goes on
        }
        return;
    }
    // ---- a number or literal ends at its delimiter, which is then read as structure
    if (j->cap_active && !j->cap_container && !j->cap_string) {
        if (!is_delim(c)) { put(j, c); return; }
        end_cap(j);
        if (j->stopped) return;
    }
    // ---- inside a captured container
    if (j->cap_active && j->cap_container) {
        put(j, c);
        if (c == '"') { j->in_str = true; j->esc = false; }
        else if (c == '{' || c == '[') j->depth++;
        else if (c == '}' || c == ']') {
            j->depth--;
            if (j->depth == j->cap_base) end_cap(j);
        }
        return;
    }
    if (is_ws(c)) return;

    // ---- the root
    if (!j->root_known) {
        if (c == '{') { j->root_known = true; j->depth = 1; j->phase = P_KEY; }
        else if (c == '[') { j->root_known = true; j->root_array = true; j->depth = 1; j->iter = true; j->ephase = E_VALUE; j->iter_key[0] = 0; }
        else j->err = SC_J_E_SYNTAX;
        return;
    }
    // ---- an iterated array's elements
    if (j->iter && j->depth == (j->root_array ? 1 : 2)) {
        if (j->ephase == E_VALUE) {
            if (c == ']') {
                j->depth--; j->iter = false;
                if (j->root_array) j->done = true;
            } else {
                j->ephase = E_AFTER;
                start_value(j, true, c);
            }
        } else {                                                    // E_AFTER
            if (c == ',') j->ephase = E_VALUE;
            else if (c == ']') { j->depth--; j->iter = false; if (j->root_array) j->done = true; }
            else j->err = SC_J_E_SYNTAX;
        }
        return;
    }
    // ---- an object root's members
    if (j->depth == 1 && !j->root_array) {
        switch (j->phase) {
        case P_KEY:
            if (c == '"') { j->in_str = true; j->esc = false; j->key_capturing = true; j->keylen = 0; }
            else if (c == '}') { j->depth = 0; j->done = true; }
            else j->err = SC_J_E_SYNTAX;
            break;
        case P_COLON:
            if (c == ':') j->phase = P_VALUE; else j->err = SC_J_E_SYNTAX;
            break;
        case P_VALUE:
            j->phase = P_AFTER;
            if (c == '[') {                                         // an array member: its elements are delivered
                memcpy(j->iter_key, j->key, sizeof(j->iter_key));
                j->depth = 2; j->iter = true; j->ephase = E_VALUE;
            } else {
                start_value(j, false, c);
            }
            break;
        default:                                                    // P_AFTER
            if (c == ',') j->phase = P_KEY;
            else if (c == '}') { j->depth = 0; j->done = true; }
            else j->err = SC_J_E_SYNTAX;
            break;
        }
        return;
    }
    j->err = SC_J_E_SYNTAX;
}

int sc_jstream_run(sc_jread rd, void *rctx, const sc_jcb *cb, size_t work_cap, sc_jstats *stats) {
    sc_jstats local;
    if (!stats) stats = &local;
    memset(stats, 0, sizeof(*stats));
    J *j = (J *)calloc(1, sizeof(*j));
    uint8_t *chunk = (uint8_t *)malloc(CHUNK + work_cap);
    if (!j || !chunk) { free(j); free(chunk); return SC_J_E_MEM; }
    j->cb = cb;
    j->w = chunk + CHUNK;
    j->cap = work_cap;
    j->st = stats;
    int rc = SC_J_OK;
    while (!j->done && !j->stopped && !j->err) {
        const int n = rd(rctx, chunk, CHUNK);
        if (n < 0) { rc = SC_J_E_READ; break; }
        if (n == 0) { rc = j->root_known ? SC_J_E_SYNTAX : SC_J_E_SYNTAX; break; }      // ended before the root closed
        for (int i = 0; i < n && !j->done && !j->stopped && !j->err; i++) feed(j, chunk[i]);
    }
    if (j->err) rc = j->err;
    free(chunk);
    free(j);
    return rc;
}
