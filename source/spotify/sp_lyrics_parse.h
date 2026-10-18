// Lyrics formats for the Spotify app, kept free of PS3 code so the host tests can run them: Spotify's color-lyrics JSON, LRCLIB's
// JSON (which carries LRC text), plain LRC, and the hex track id to base62 conversion the lyrics address needs.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

struct LyricLine {
  uint32_t ms;          // start of the line; 0 for every line of unsynced lyrics
  std::string text;
};

enum { LYRICS_MAX_LINES = 600, LYRICS_MAX_TEXT = 200 };

// "[mm:ss.xx] words" lines, several stamps per line allowed; tags such as [ar:...] are skipped.  Sorted by time.  True when at
// least one timed line was found.
bool lrc_parse(const char *text, size_t len, std::vector<LyricLine> &out);

// Untimed lyrics, one entry per non-empty line.
void plain_parse(const char *text, size_t len, std::vector<LyricLine> &out);

// {"lyrics":{"syncType":"LINE_SYNCED","lines":[{"startTimeMs":"960","words":"..."}]}}.  False when the body is not that shape.
bool spotify_lyrics_parse(const char *json, size_t len, std::vector<LyricLine> &out, bool *synced);

// LRCLIB /api/get object, or the first usable entry of an /api/search array: syncedLyrics when present, else plainLyrics.
bool lrclib_parse(const char *json, size_t len, std::vector<LyricLine> &out, bool *synced);

// The line playing at ms: -1 before the first line.
int lyric_index_at(const std::vector<LyricLine> &lines, uint32_t ms);

// 32 hex digits (a 16-byte Spotify GID) -> the 22-character base62 id used in spotify:track: URIs.  False on bad input.
bool sp_hex_to_base62(const char *hex, char out[23]);

// RFC 3986 percent-encoding of a query value.
std::string url_encode(const char *s);
