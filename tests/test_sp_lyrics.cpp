// Host test for source/spotify/sp_lyrics_parse.cpp, from the repository root:
//   g++ -std=gnu++17 -fsanitize=address,undefined -Ithird_party/cspot -Isource/spotify tests/test_sp_lyrics.cpp
//       source/spotify/sp_lyrics_parse.cpp -o /tmp/test_sp_lyrics && /tmp/test_sp_lyrics [hex base62]...
#include <stdio.h>
#include <string.h>

#include "sp_lyrics_parse.h"

static int s_fail = 0, s_n = 0;
#define CHECK(c) do { s_n++; if (!(c)) { s_fail++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(int argc, char **argv) {
  std::vector<LyricLine> v;
  bool synced = false;

  // LRC: tags skipped, fractions of 1-3 digits, several stamps on one line, CRLF, out-of-order input sorted
  const char lrc[] = "[ar:Someone]\r\n[ti:Song]\r\n[00:12.5]Second\r\n[00:01.23]  First   line \r\n"
                     "[01:02.345][00:30]Twice\n[00:40]\nnot a line\n[xx:10]bad\n";
  CHECK(lrc_parse(lrc, strlen(lrc), v));
  CHECK(v.size() == 5);
  CHECK(v[0].ms == 1230 && v[0].text == "First line");
  CHECK(v[1].ms == 12500 && v[1].text == "Second");
  CHECK(v[2].ms == 30000 && v[2].text == "Twice");
  CHECK(v[3].ms == 40000 && v[3].text.empty());
  CHECK(v[4].ms == 62345 && v[4].text == "Twice");
  CHECK(lyric_index_at(v, 0) == -1);
  CHECK(lyric_index_at(v, 1230) == 0);
  CHECK(lyric_index_at(v, 29999) == 1);
  CHECK(lyric_index_at(v, 999999) == 4);
  CHECK(!lrc_parse("[00:01]\n[00:02]  \n", 18, v) && v.empty());
  CHECK(!lrc_parse("just words\n", 11, v));

  // Spotify color-lyrics, with the note glyph and string times
  const char sp[] = "{\"lyrics\":{\"syncType\":\"LINE_SYNCED\",\"lines\":["
                    "{\"startTimeMs\":\"960\",\"words\":\"Hello\",\"endTimeMs\":\"0\"},"
                    "{\"startTimeMs\":\"500\",\"words\":\"\xE2\x99\xAA\"},"
                    "{\"startTimeMs\":\"4000\",\"words\":\"World  again\"}]},\"colors\":{}}";
  CHECK(spotify_lyrics_parse(sp, strlen(sp), v, &synced));
  CHECK(synced && v.size() == 3);
  CHECK(v[0].ms == 500 && v[0].text == "...");
  CHECK(v[1].ms == 960 && v[1].text == "Hello");
  CHECK(v[2].text == "World again");
  const char spu[] = "{\"lyrics\":{\"syncType\":\"UNSYNCED\",\"lines\":[{\"startTimeMs\":\"0\",\"words\":\"A\"},{\"words\":\"\"},{\"words\":\"B\"}]}}";
  CHECK(spotify_lyrics_parse(spu, strlen(spu), v, &synced));
  CHECK(!synced && v.size() == 2 && v[1].text == "B");
  CHECK(!spotify_lyrics_parse("{\"error\":1}", 11, v, &synced));
  CHECK(!spotify_lyrics_parse("not json", 8, v, &synced));
  CHECK(!spotify_lyrics_parse("{\"lyrics\":{\"syncType\":\"LINE_SYNCED\",\"lines\":[{\"words\":\"\xE2\x99\xAA\"}]}}", 66, v, &synced));

  // LRCLIB: object with synced, object with only plain, search array preferring synced, instrumental
  const char lg[] = "{\"id\":1,\"syncedLyrics\":\"[00:05.00] One\\n[00:07.10] Two\",\"plainLyrics\":\"One\\nTwo\"}";
  CHECK(lrclib_parse(lg, strlen(lg), v, &synced) && synced && v.size() == 2 && v[1].ms == 7100);
  const char lp[] = "{\"syncedLyrics\":null,\"plainLyrics\":\"Alpha\\n\\nBeta\\n\"}";
  CHECK(lrclib_parse(lp, strlen(lp), v, &synced) && !synced && v.size() == 2 && v[0].ms == 0);
  const char ls[] = "[{\"syncedLyrics\":null,\"plainLyrics\":\"P\"},{\"syncedLyrics\":\"[00:01]S\"}]";
  CHECK(lrclib_parse(ls, strlen(ls), v, &synced) && synced && v[0].text == "S");
  const char li[] = "{\"instrumental\":true,\"syncedLyrics\":null,\"plainLyrics\":null}";
  CHECK(!lrclib_parse(li, strlen(li), v, &synced));
  CHECK(!lrclib_parse("[]", 2, v, &synced));

  // long line capped on a UTF-8 boundary
  std::string big = "[00:01]";
  for (int i = 0; i < 150; i++) big += "\xC3\xA9";
  CHECK(lrc_parse(big.data(), big.size(), v) && v[0].text.size() <= LYRICS_MAX_TEXT && (v[0].text.size() % 2) == 0);

  // base62: argv pairs from python (hex, expected) when given; fixed cases otherwise
  char b62[23];
  CHECK(sp_hex_to_base62("00000000000000000000000000000000", b62) && strcmp(b62, "0000000000000000000000") == 0);
  CHECK(!sp_hex_to_base62("0123", b62));
  CHECK(!sp_hex_to_base62("zz000000000000000000000000000000", b62));
  for (int i = 1; i + 1 < argc; i += 2) {
    CHECK(sp_hex_to_base62(argv[i], b62));
    if (strcmp(b62, argv[i + 1]) != 0) printf("  base62 %s -> %s, want %s\n", argv[i], b62, argv[i + 1]);
    CHECK(strcmp(b62, argv[i + 1]) == 0);
  }

  CHECK(url_encode("AC/DC & Me ü") == "AC%2FDC%20%26%20Me%20%C3%BC");

  printf("%d checks, %d failed\n", s_n, s_fail);
  return s_fail != 0;
}
