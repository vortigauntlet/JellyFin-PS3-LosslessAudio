// Host test for source/spotify/sp_collection.cpp, from the repository root:
//   g++ -std=gnu++17 -fsanitize=address,undefined -Ithird_party/cspot -Isource/spotify tests/test_sp_collection.cpp
//       source/spotify/sp_collection.cpp source/spotify/sp_lyrics_parse.cpp -o /tmp/test_sp_collection && /tmp/test_sp_collection
#include <stdio.h>
#include <string.h>

#include "sp_collection.h"
#include "sp_lyrics_parse.h"

static int s_fail = 0, s_n = 0;
#define CHECK(c) do { s_n++; if (!(c)) { s_fail++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static void put(std::string &o, int field, const std::string &s) {
  o += (char)((field << 3) | 2);
  o += (char)s.size();
  o += s;
}

int main() {
  // 0a 05 "user1" 12 0a "collection" 20 ac 02  (limit 300)
  const std::string rq = collection_page_request("user1", "collection", "", 300);
  const unsigned char want[] = {0x0a, 5, 'u', 's', 'e', 'r', '1', 0x12, 10, 'c', 'o', 'l', 'l', 'e', 'c', 't', 'i', 'o', 'n',
                                0x20, 0xac, 0x02};
  CHECK(rq.size() == sizeof want && memcmp(rq.data(), want, sizeof want) == 0);
  CHECK(collection_page_request("u", "collection", "tok", 5).find(std::string("\x1a\x03tok", 5)) != std::string::npos);

  // a page: one track (added_at varint), one removed track, one episode, the next token, a sync token
  std::string resp, a, b, c;
  put(a, 1, "spotify:track:4cOdK2wGLETKBW3PvgPWqT");
  a += (char)0x10; a += (char)0x7b;
  put(b, 1, "spotify:track:0000000000000000000001");
  b += (char)0x18; b += (char)1;
  put(c, 1, "spotify:episode:abc");
  put(resp, 1, a); put(resp, 1, b); put(resp, 1, c);
  put(resp, 2, "NEXT"); put(resp, 3, "sync");
  std::vector<std::string> uris;
  std::string next;
  CHECK(collection_page_parse((const uint8_t *)resp.data(), resp.size(), uris, next));
  CHECK(uris.size() == 1 && uris[0] == "spotify:track:4cOdK2wGLETKBW3PvgPWqT" && next == "NEXT");
  CHECK(!collection_page_parse((const uint8_t *)"\x0a\x50zz", 4, uris, next));     // length past the end
  CHECK(collection_page_parse((const uint8_t *)"", 0, uris, next) && uris.empty() && next.empty());

  // URI -> hex -> base62 round trip
  char hex[33], b62[23];
  CHECK(sp_uri_to_hex("spotify:track:4cOdK2wGLETKBW3PvgPWqT", hex));
  CHECK(sp_hex_to_base62(hex, b62) && strcmp(b62, "4cOdK2wGLETKBW3PvgPWqT") == 0);
  CHECK(!sp_uri_to_hex("spotify:track:short", hex));
  CHECK(!sp_uri_to_hex("spotify:track:4cOdK2wGLETKBW3PvgPW!T", hex));
  CHECK(!sp_uri_to_hex("spotify:track:zzzzzzzzzzzzzzzzzzzzzz", hex));    // above 128 bits
  CHECK(!sp_uri_to_hex("spotify:episode:4cOdK2wGLETKBW3PvgPWqT", hex));

  printf("%d checks, %d failed\n", s_n, s_fail);
  return s_fail != 0;
}
