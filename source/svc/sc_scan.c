#include "sc_scan.h"
#include <string.h>

static const char KEY[] = "client_id:\"";
#define KEY_LEN ((int)(sizeof(KEY) - 1))

static bool is_alnum(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

void sc_scan_init(sc_scan *s) {
    memset(s, 0, sizeof(*s));
}

const char *sc_scan_id(const sc_scan *s) {
    return s->found ? s->id : NULL;
}

bool sc_scan_feed(sc_scan *s, const char *buf, size_t n) {
    for (size_t i = 0; i < n && !s->found; i++) {
        const unsigned char c = (unsigned char)buf[i];
        for (;;) {                                   // a byte that ends one attempt may begin the next
            if (s->m < 0) {                          // reading the id
                if (is_alnum(c) && s->n < SC_CID_LEN) {
                    s->id[s->n++] = (char)c;
                } else if (c == '"' && s->n == SC_CID_LEN) {
                    s->id[s->n] = 0;
                    s->found = true;
                } else {
                    s->m = 0; s->n = 0;              // too short, too long or not alphanumeric: start over
                    continue;
                }
            } else if (s->m > 0 && c == (unsigned char)KEY[s->m]) {
                if (++s->m == KEY_LEN) { s->m = -1; s->n = 0; }
            } else if (s->m > 0) {                   // the key has no proper prefix that is also a suffix
                s->m = 0;
                continue;
            } else if (c == (unsigned char)KEY[0] && !is_alnum(s->prev) && s->prev != '_') {
                s->m = 1;
            }
            break;
        }
        s->prev = c;
    }
    return s->found;
}

// ---- the page's script list -------------------------------------------------------------------------------------

static const char TAG[] = "<script";
static const char SRC[] = "src=\"";

void sc_html_init(sc_html *f, char (*out)[SC_HTML_URL_MAX], int max) {
    memset(f, 0, sizeof(*f));
    f->out = out;
    f->max = max;
}

static bool ws(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

void sc_html_feed(sc_html *f, const char *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)buf[i];
        switch (f->st) {
        case 0:                                              // text: look for "<script"
            if (f->m > 0 && c == (unsigned char)TAG[f->m]) { if (++f->m == (int)sizeof(TAG) - 1) { f->st = 1; f->m = 0; } }
            else f->m = (c == '<') ? 1 : 0;
            break;
        case 1:                                              // the tag name must end here (not "<scripts")
            if (ws(c)) { f->st = 2; f->m = 0; f->prev_ws = true; }
            else if (c == '>') f->st = 0;
            else { f->st = 0; f->m = (c == '<') ? 1 : 0; }
            break;
        case 2:                                              // inside the tag: look for ' src="'
            if (c == '>') { f->st = 0; f->m = 0; break; }
            if (f->m > 0) {
                if (c == (unsigned char)SRC[f->m]) {
                    if (++f->m == (int)sizeof(SRC) - 1) { f->st = 3; f->m = 0; f->ul = 0; f->overlong = false; }
                    f->prev_ws = false;
                    break;
                }
                f->m = 0;
            }
            if (f->prev_ws && c == 's') f->m = 1;
            f->prev_ws = ws(c);
            break;
        default:                                             // the value, up to the closing quote
            if (c == '"') {
                if (!f->overlong && f->ul > 0 && f->n < f->max) {
                    f->url[f->ul] = 0;
                    memcpy(f->out[f->n++], f->url, (size_t)f->ul + 1);
                }
                f->st = 2; f->m = 0; f->prev_ws = false;
            } else if (f->ul < SC_HTML_URL_MAX - 1) {
                f->url[f->ul++] = (char)c;
            } else {
                f->overlong = true;
            }
            break;
        }
    }
}
