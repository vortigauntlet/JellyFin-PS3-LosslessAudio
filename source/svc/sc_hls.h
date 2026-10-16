// HLS media playlist reader for SoundCloud's fragmented-MP4 streams.
//
// It reads what SoundCloud serves: EXT-X-MAP, EXTINF, EXT-X-TARGETDURATION, EXT-X-ENDLIST.  A playlist
// that declares encryption (EXT-X-KEY / EXT-X-SESSION-KEY with any METHOD other than NONE) is refused
// before any URL is looked at: the player never fetches a key and never decrypts.  A master playlist
// and byte-range segments are refused too, because the player does not use them.
//
// The playlist is read as a stream and only what playing needs is kept, because a one-hour mix has 360 segments whose
// URIs are 820 characters each (about 300 KB) and the console has well under 200 KB free while it plays.  SoundCloud's
// segment URIs differ only in a counter (".../data000.m4s?<one long query>"), so the first is kept as a template and
// each later one is checked against it; a playlist that does not follow the pattern falls back to keeping every URI
// (up to 96 KB), and one that does not fit is refused.  Either way a segment costs 4 bytes.
//
// Pure C with no platform includes: the host suite runs this file as it is.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SC_HLS_OK          =  0,
    SC_HLS_E_NOTM3U    = -1,
    SC_HLS_E_ENCRYPTED = -2,   // declares a key: refused, never decrypted
    SC_HLS_E_MASTER    = -3,   // a master (variant) playlist, not a media playlist
    SC_HLS_E_BYTERANGE = -4,
    SC_HLS_E_EMPTY     = -5,   // no segments
    SC_HLS_E_MEM       = -6,
    SC_HLS_E_TOOMANY   = -7,   // more segments than SC_HLS_MAX_SEGS, or irregular URIs that do not fit
    SC_HLS_E_READ      = -8    // the reader failed
};

#define SC_HLS_MAX_SEGS  4000
#define SC_HLS_ARENA_MAX (96 * 1024)

typedef struct {
    int       nseg;
    uint32_t *dur_ms;            // EXTINF of each segment, heap
    uint32_t  total_ms;
    uint32_t  target_ms;         // EXT-X-TARGETDURATION
    bool      ended;             // EXT-X-ENDLIST was present (VOD)
    char     *map_uri;           // the EXT-X-MAP URI, heap; NULL when the stream has no init segment
    // segment i = tmpl_pre + number(first_no + i, zero-padded to tmpl_width) + tmpl_suf   (regular playlists)
    char     *tmpl_pre, *tmpl_suf;
    int       tmpl_width;
    long      first_no;
    // otherwise every URI, NUL-separated, with the offset of each
    char     *arena;
    size_t    arena_len, arena_cap;
    uint32_t *arena_off;
} sc_hls;

// Reads up to n bytes: > 0 bytes, 0 at the end, < 0 on a failure.
typedef int (*sc_hls_read_fn)(void *ctx, void *buf, int n);

// 0 or an SC_HLS_E_*.  On failure nothing is allocated.
int  sc_hls_read(sc_hls_read_fn rd, void *ctx, sc_hls *out);
int  sc_hls_parse(const char *text, size_t len, sc_hls *out);       // the same over a buffer
void sc_hls_free(sc_hls *h);

// Copies segment i's (or the init segment's, i < 0) URI, resolved against base, into out.
// False when it does not fit or the index is out of range.
bool sc_hls_seg_url(const sc_hls *h, int i, const char *base, char *out, size_t cap);

// Index of the segment containing t_ms (clamped), and the offset into it.
int  sc_hls_seek(const sc_hls *h, uint32_t t_ms, uint32_t *into_ms);

// RFC 3986-style join for the cases a playlist uses: absolute, //host/path, /path and relative.
bool sc_url_join(const char *base, const char *ref, size_t ref_len, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
