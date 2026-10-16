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

#ifdef __cplusplus
}
#endif
