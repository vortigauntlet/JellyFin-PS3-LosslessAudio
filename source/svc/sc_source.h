// The network side of one playing SoundCloud track: resolves the transcoding to a playlist, keeps the playlist,
// and fetches the init segment and media segments on demand for sc_dec.
//
// Signed segment URLs expire (about 2 hours). A fetch that is refused (401/403/404/410), or one made after the
// playlist is 90 minutes old, re-resolves the transcoding once and retries with the new signatures, so a track
// left paused for hours resumes. Transport failures are retried twice with a short wait.
//
// Streams only: segments are handed to the decoder and freed; nothing is cached or written.
#pragma once
#include "sc_dec.h"
#include "sc_net.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ScSource ScSource;

// Resolves tc_url (a transcoding URL from sc_track) to a playlist and parses it. NULL on failure; *err gets the
// SC_NET_E_* or SC_HLS_E_* code when err is not NULL. `net` must outlive the source and be used by one thread.
ScSource *scsrc_open(sc_net *net, const char *tc_url, ScSegSource *out, int *err);
void      scsrc_close(ScSource *s);

// Treats the signed URLs as expired, so the next fetch re-resolves first (what a long pause does after 90 minutes;
// used by the live test and the console diagnostic).
void      scsrc_expire(ScSource *s);

// For the log: segments, total length, how many times the signed URLs were refreshed, fetch retries so far.
int       scsrc_refreshes(const ScSource *s);
int       scsrc_retries(const ScSource *s);

#ifdef __cplusplus
}
#endif
