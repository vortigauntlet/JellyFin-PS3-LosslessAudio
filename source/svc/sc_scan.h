// Finds SoundCloud's public web client_id in the page's JavaScript while it streams in.
//
// The id sits in one 2.9 MB script as  client_id:"<32 letters or digits>"  and the same file holds
// google_client_id:"..." and apple_id_client_id:"..." (neither is 32 alphanumerics, and both are
// preceded by '_'), so a match needs the right preceding byte, exactly 32 alphanumerics and a
// closing quote.  The scanner is a byte state machine: it keeps no overlap window, so a match split
// across two reads is found the same as one inside a read.
//
// Pure C with no platform includes: the host suite runs this file as it is.
#pragma once
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SC_CID_LEN 32

typedef struct {
    int           m;               // bytes of the key matched so far; -1 while reading the id
    int           n;               // id bytes read
    unsigned char prev;            // the byte before the one being looked at
    bool          found;
    char          id[SC_CID_LEN + 1];
} sc_scan;

void sc_scan_init(sc_scan *s);

// Feeds the next chunk.  True once the id has been found (and from then on).
bool sc_scan_feed(sc_scan *s, const char *buf, size_t n);

// The id, or NULL until it is found.  Never log it.
const char *sc_scan_id(const sc_scan *s);

// ---- the page's script list -------------------------------------------------------------------------------------
// soundcloud.com's home page is 70 KB and lists its scripts as <script ... src="https://..."></script>.  This reads them
// out as the page streams in (any split of the bytes gives the same list): the src of every <script> tag, double-quoted,
// in order, at most `max` of them, each at most SC_HTML_URL_MAX-1 characters (longer ones are skipped).

#define SC_HTML_URL_MAX 200

typedef struct {
    int           st;            // 0 text, 1 after "<script", 2 inside the tag, 3 reading the src value
    int           m;             // progress through "<script" or "src=\""
    bool          prev_ws;
    unsigned char prev;
    char          url[SC_HTML_URL_MAX];
    int           ul;
    bool          overlong;
    char        (*out)[SC_HTML_URL_MAX];
    int           n, max;
} sc_html;

void sc_html_init(sc_html *f, char (*out)[SC_HTML_URL_MAX], int max);
void sc_html_feed(sc_html *f, const char *buf, size_t n);

#ifdef __cplusplus
}
#endif
