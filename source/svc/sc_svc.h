#pragma once

// SoundCloud on the console: the state that outlives a screen (the user's profile name, the audio quality, the
// shared client id) and the thumbnail cache's provider for SoundCloud artwork.  The protocol is sc_*.c; this is the
// part that touches the console's disk and threads.
//
// Files (see sc_store.h): /dev_hdd0/tmp/jellyfin_sc.txt (the client id) and jellyfin_sc_settings.txt.

#include "sc_net.h"

#ifdef __cplusplus
extern "C" {
#endif

// Reads both files. Call once at boot (before any other thread uses SoundCloud).
void sc_svc_init(void);

const char *sc_svc_profile(void);               // "" until the user has entered one
void        sc_svc_set_profile(const char *name);   // saved at once; the caller has checked it with sc_profile_url()
int         sc_svc_quality(void);               // 160 or 96
void        sc_svc_set_quality(int kbps);

// A network context for one thread: sc_net_init, plus the client id found so far. Pair every begin with an end;
// a client id the context found or refreshed is published (and saved) by the end.
void sc_svc_net_begin(sc_net *n);
void sc_svc_net_end(sc_net *n);

// The thumbnail cache's provider for keys made by sc_art_key(): the picture's bytes (> 0), 0 when the key is not
// SoundCloud's, < 0 when it is and the picture could not be fetched. Runs on the thumbnail thread.
int  sc_art_bytes(const char *key, unsigned char *buf, int cap);

// What main.cpp installs with thumb_set_local_bytes(): covers of music files on a drive first, then SoundCloud's.
int  music_art_bytes(const char *key, unsigned char *buf, int cap);

#ifdef __cplusplus
}
#endif
