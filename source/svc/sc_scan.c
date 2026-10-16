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
