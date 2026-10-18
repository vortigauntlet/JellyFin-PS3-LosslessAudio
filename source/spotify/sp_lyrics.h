// Synced lyrics for the Spotify app's Now Playing.
//
// One worker thread fetches the lyrics of the current track: Spotify's own lyrics service first (with the session's token), then
// LRCLIB (lrclib.net, public, no account) by title, artist, album and length.  The screen asks for the line at the playback position.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sp_svc.h"

enum SpLyricsState {
  SPL_NONE,       // no track, or not asked yet
  SPL_LOADING,
  SPL_SYNCED,     // timed lines
  SPL_UNSYNCED,   // words without times
  SPL_MISSING,    // neither service has lyrics for this song
};

// Follows the current track: call every frame with sp_svc_track()'s result (ok=false when there is none).  Cheap when unchanged.
void sp_lyrics_follow(const SpTrack *track, bool ok);

SpLyricsState sp_lyrics_state(void);
const char *sp_lyrics_source(void);         // "Spotify" or "LRCLIB" once loaded, else ""
int sp_lyrics_count(void);
int sp_lyrics_index_at(uint32_t ms);        // -1 before the first line (synced only)
uint32_t sp_lyrics_time(int index);
bool sp_lyrics_text(int index, char *out, size_t cap);

// Shown or hidden on Now Playing (Triangle); remembered in jellyfin_spotify_lyrics.txt.
bool sp_lyrics_enabled(void);
void sp_lyrics_set_enabled(bool on);
