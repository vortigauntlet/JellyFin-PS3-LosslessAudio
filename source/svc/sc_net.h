// SoundCloud network side: finds the web client_id, calls the API, resolves a transcoding to a
// playlist URL.  Everything blocks, so callers run it on worker threads, never the render thread.
//
// Portable over the sx_http client (source/net), so the host test drives the same code the console
// runs.  The client_id never leaves this module in a URL that is logged: callers build API URLs with
// SC_CID_PLACEHOLDER in the client_id slot and sc_net_api() substitutes the real value for the one
// request.  After a 401 or 403 it scrapes a fresh id once and retries.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "sc_scan.h"

#ifdef __cplusplus
extern "C" {
#endif

// Longest signed media URL SoundCloud sends (measured about 780 characters), with room to spare.
#define SC_URL_MAX 1536

// 32 alphanumerics, so it passes every length check a real id would.
#define SC_CID_PLACEHOLDER "CIDCIDCIDCIDCIDCIDCIDCIDCIDCIDCI"

enum {
    SC_NET_OK       =  0,
    SC_NET_E_NET    = -1,    // transport failure (DNS, connect, TLS, timeout)
    SC_NET_E_HTTP   = -2,    // the server answered with a status other than 200
    SC_NET_E_CID    = -3,    // no client_id could be found
    SC_NET_E_TOOBIG = -4,
    SC_NET_E_ARG    = -5
};

typedef struct {
    char cid[SC_CID_LEN + 1];       // "" until known
    bool cid_dirty;                 // set when the id changed and the caller should store it
    int  last_status;               // HTTP status of the last request, 0 if none arrived
    int  last_err;                  // sx error code of the last transport failure
    // Per-request timeouts in ms; 0 = the HTTP client's defaults (8 s connect, 12 s handshake, 24 s first byte, 8 s idle).
    int  connect_ms, handshake_ms, first_byte_ms, idle_ms;
} sc_net;

void sc_net_init(sc_net *n);

// Restores a stored id (ignored unless it is 32 alphanumerics).
void sc_net_set_cid(sc_net *n, const char *cid);

// Reads soundcloud.com, then streams its scripts last-first through the scanner until the id turns
// up.  SC_NET_OK, SC_NET_E_NET or SC_NET_E_CID.
int  sc_net_scrape_cid(sc_net *n);

// GET an API URL that carries SC_CID_PLACEHOLDER.  On SC_NET_OK *body is a NUL-terminated heap
// buffer to free().  Scrapes the id first when there is none, and again once after a 401/403.
int  sc_net_api(sc_net *n, const char *url, size_t max_body, char **body, size_t *len);

// GET any URL (playlists, segments).  No client_id handling.
int  sc_net_get(sc_net *n, const char *url, size_t max_body, char **body, size_t *len);

// Resolves a transcoding URL to the media playlist / MP3 URL.
int  sc_net_stream_url(sc_net *n, const char *tc_url, char *out, size_t cap);

// ---- streams: the console has well under 200 KB free while it plays, so a page of 50 tracks (200 KB of JSON), a
// segment (200 KB) and a long playlist are read as streams and never held whole ----

typedef struct sc_stream sc_stream;

// Opens a GET on a SoundCloud host and returns once the response head is in (status 200, or 206 when range_start > 0).
int  sc_net_open(sc_net *n, const char *url, int64_t range_start, sc_stream **out);
// The same for an API URL that carries SC_CID_PLACEHOLDER (client id substituted; one refresh-and-retry on 401/403).
int  sc_net_open_api(sc_net *n, const char *url, sc_stream **out);
int  sc_stream_read(sc_stream *s, void *buf, int n);       // > 0 bytes, 0 at the end, < 0 on a failure
void sc_stream_close(sc_stream *s);                        // pools the connection when the body was read to its end
int  sc_stream_jread(void *ctx, void *buf, int n);         // sc_jread / sc_hls_read_fn adaptor: ctx is an sc_stream *

// Drops every idle pooled connection (each holds about 40 KB of TLS state).
void sc_net_flush(sc_net *n);

// Which hosts a media URL may name.  The playback CDN and API are SoundCloud's; anything else in a
// playlist is refused.
bool sc_net_host_ok(const char *url);

#ifdef __cplusplus
}
#endif
