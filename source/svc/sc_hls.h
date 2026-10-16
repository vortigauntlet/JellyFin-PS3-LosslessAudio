// HLS media playlist reader for SoundCloud's fragmented-MP4 streams.
//
// It reads what SoundCloud serves: EXT-X-MAP, EXTINF, EXT-X-TARGETDURATION, EXT-X-ENDLIST.  A playlist
// that declares encryption (EXT-X-KEY / EXT-X-SESSION-KEY with any METHOD other than NONE) is refused
// before any URL is looked at: the player never fetches a key and never decrypts.  A master playlist
// and byte-range segments are refused too, because the player does not use them.
//
// Segment and init-segment URIs are returned as views into the caller's text (the text must outlive
// the parse result), so a 350-segment mix costs 12 bytes a segment.
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
    SC_HLS_E_TOOMANY   = -7    // more segments than SC_HLS_MAX_SEGS
};

#define SC_HLS_MAX_SEGS 4000

typedef struct {
    uint32_t off;              // URI start in the playlist text
    uint16_t len;              // URI length
    uint16_t pad;
    uint32_t dur_ms;           // EXTINF
} sc_hls_seg;

typedef struct {
    const char *text;          // the caller's playlist text; must outlive this struct
    uint32_t    map_off, map_len;     // EXT-X-MAP URI (0 length when the stream has no init segment)
    int         nseg;
    sc_hls_seg *seg;           // heap, sc_hls_free()
    uint32_t    total_ms;
    uint32_t    target_ms;     // EXT-X-TARGETDURATION
    bool        ended;         // EXT-X-ENDLIST was present (VOD)
} sc_hls;

// 0 or an SC_HLS_E_*.  On failure nothing is allocated.
int  sc_hls_parse(const char *text, size_t len, sc_hls *out);
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
