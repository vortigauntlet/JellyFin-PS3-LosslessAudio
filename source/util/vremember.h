#pragma once
// -------------------------------------------------------------------------
//  Remembered version per title
// -------------------------------------------------------------------------
//  The MediaSourceId that last actually opened a stream for a title, so the
//  next play goes straight to it -- on this server's debrid library many
//  versions are dead links or trip Gelato's '|' bug, and finding a working
//  one costs a fallback every time.  The details page pre-selects it.
//  Persisted as "item_id source_id" lines in jellyfin_versions.txt, newest
//  first, capped; a stale entry just costs one fallback (show_player tries
//  the others when the remembered one fails).

// The remembered source for item_id, or NULL.  The pointer is to a static
// buffer, valid until the next call.
const char *vremember_get(const char *item_id);
void        vremember_put(const char *item_id, const char *source_id);

// -------------------------------------------------------------------------
//  Warm the server (api/api_warm.cpp)
// -------------------------------------------------------------------------
//  Ask for PlaybackInfo in the background the moment a details page opens,
//  so the server's first-time stream lookup (Gelato: 6-8 s) is done by the
//  time Play is pressed.  Fire-and-forget, one at a time, own buffer.
void jellyfin_warm_playback(const char *item_id);
