// SoundCloud web-API model: the request URLs the player builds, the parsers for what comes back,
// and the one rule that decides what may be played.
//
// What may be played (rules in HANDOFF-music-channels.md section 1):
//   - only transcodings whose protocol is exactly "hls" (AAC in fragmented MP4) or "progressive" MP3;
//   - "cbc-encrypted-hls" / "ctr-encrypted-hls" are never used, and their key URIs are never fetched:
//     a track that has nothing else is hidden (SC_HID_ENCRYPTED);
//   - snipped (30 s preview) transcodings are skipped, so a Go+ track with only a preview is hidden.
//
// Pure C with no platform includes: the host suite runs this file as it is.  Input is whatever the
// server sent, so every parser is bounds-checked and treats malformed input as "absent".
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SC_API_HOST "api-v2.soundcloud.com"

// Why a track is not offered.
enum {
    SC_HID_NONE = 0,        // playable
    SC_HID_STUB,            // only an id so far (a playlist stub): fill it with tracks?ids=
    SC_HID_ENCRYPTED,       // every transcoding is cbc-/ctr-encrypted
    SC_HID_PREVIEW,         // only a 30 s snippet
    SC_HID_UNSUPPORTED,     // no transcoding this player can decode (opus, mp3 over hls, ...)
    SC_HID_NOT_STREAMABLE,  // streamable:false or state != finished
    SC_HID_COUNT
};

enum { SC_TC_NONE = 0, SC_TC_AAC_HLS = 1, SC_TC_MP3_PROGRESSIVE = 2 };

typedef struct {
    int64_t  id;
    uint32_t dur_ms;
    int      tc_kind;            // SC_TC_*
    int      tc_kbps;            // 160 / 96 / 128 (mp3)
    int      hidden;             // SC_HID_*
    char     title[144];
    char     artist[80];
    char     art[168];           // artwork_url, else the uploader's avatar_url, as sent
    char     permalink[168];     // permalink_url
    char     tc_url[208];        // the chosen transcoding's URL (no query)
} sc_track;

typedef struct {
    int64_t id;
    int     track_count;
    char    title[128];
    char    owner[80];
    char    art[168];
    char    permalink[168];
} sc_playlist;

typedef struct {
    int64_t id;
    char    name[80];
    char    permalink[96];       // the part after soundcloud.com/
    char    avatar[168];
} sc_user;

// One page of a listing.  Tracks and playlists keep the server's order within their kind.
typedef struct {
    sc_track    *tr;   int ntr, cap_tr;
    sc_playlist *pl;   int npl, cap_pl;
    char         next_href[320];          // "" at the end
    int          hidden[SC_HID_COUNT];    // how many tracks of this page were hidden, by reason
} sc_page;

void sc_page_init(sc_page *p);
void sc_page_free(sc_page *p);

// ---- parsers -------------------------------------------------------------------------------------

// quality_pref: 160 or 96.  Returns false only when `obj` is not a track object at all.
bool sc_parse_track(const char *json, size_t len, int quality_pref, sc_track *out);

// A listing: {"collection":[...],"next_href":...}.  Items may be tracks, playlists, or wrappers
// that carry one under "track" / "playlist" (likes, reposts).  Returns the number of items taken,
// or -1 when the document is not a listing.
int  sc_parse_page(const char *json, size_t len, int quality_pref, sc_page *out);

// A single playlist document with its tracks (full or stubs) appended to out->tr.
bool sc_parse_playlist(const char *json, size_t len, int quality_pref, sc_playlist *meta, sc_page *out);

// tracks?ids=...  (a bare JSON array of tracks).
int  sc_parse_tracks_array(const char *json, size_t len, int quality_pref, sc_page *out);

bool sc_parse_user(const char *json, size_t len, sc_user *out);

// {"url":"<m3u8 or mp3>"} from a transcoding URL.
bool sc_parse_stream_url(const char *json, size_t len, char *out, size_t cap);

// Fills stub tracks in `p` from a tracks?ids= result, matching by id.  Returns how many it filled.
int  sc_fill_stubs(sc_page *p, const sc_page *full);

// ---- URL building (all return false when out is too small) ------------------------------------------

// "forss", "@forss", "soundcloud.com/forss/" or a full URL -> "https://soundcloud.com/forss".
// False when no plausible profile name is in `in`.
bool sc_profile_url(const char *in, char *out, size_t cap);

// Percent-encodes everything outside the unreserved set.
bool sc_urlenc(const char *in, char *out, size_t cap);

bool sc_url_resolve(const char *page_url, const char *client_id, char *out, size_t cap);
bool sc_url_user_likes(int64_t uid, int limit, const char *client_id, char *out, size_t cap);
bool sc_url_user_playlists(int64_t uid, int limit, const char *client_id, char *out, size_t cap);
bool sc_url_user_tracks(int64_t uid, int limit, const char *client_id, char *out, size_t cap);
bool sc_url_user_reposts(int64_t uid, int limit, const char *client_id, char *out, size_t cap);
bool sc_url_playlist(int64_t pid, const char *client_id, char *out, size_t cap);
bool sc_url_search_tracks(const char *q, int limit, const char *client_id, char *out, size_t cap);
bool sc_url_tracks_ids(const int64_t *ids, int n, const char *client_id, char *out, size_t cap);
bool sc_url_related(int64_t tid, int limit, const char *client_id, char *out, size_t cap);

// A transcoding URL or a next_href with the client id appended.  Refuses any host but SC_API_HOST
// and any scheme but https, so the client id cannot be sent anywhere else.
bool sc_url_with_cid(const char *url, const char *client_id, char *out, size_t cap);

// artwork_url with its size suffix replaced ("-large" -> "-t500x500").  Works for any "-<name>.ext"
// suffix SoundCloud serves; a URL without one is copied unchanged.
bool sc_art_url(const char *art, int px, char *out, size_t cap);

// ---- artwork keys --------------------------------------------------------------------------------
// The thumbnail cache identifies a picture by a string of at most 63 characters, so an artwork URL is folded into a key
// that sc_art_key_url() unfolds into the fetch URL at the size wanted:
//   "sca:<host prefix>:<s|u>:<ext>:<stem>"   s = the file name carries a size suffix ("-large"), u = it does not
// Only https://<prefix>.sndcdn.com/ pictures qualify, so a key can never point the console at another host.
bool sc_art_key(const char *art_url, char *key, size_t cap);
bool sc_art_key_url(const char *key, int px, char *url, size_t cap);
bool sc_is_art_key(const char *key);

#ifdef __cplusplus
}
#endif
