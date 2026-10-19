// Liked Songs through Spotify's collection service (spclient /collection/v2/paging), the route Spotify's own clients use.  The Web
// API's /v1/me/tracks answers 429 to this app's token, so it is not used.  Pure encode/decode here, for the host tests.
//
//   PageRequest  { string username = 1; string set = 2; string pagination_token = 3; int32 limit = 4; }
//   PageResponse { repeated CollectionItem items = 1; string next_page_token = 2; string sync_token = 3; }
//   CollectionItem { string uri = 1; int32 added_at = 2; bool is_removed = 3; }
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

std::string collection_page_request(const std::string &username, const char *set, const std::string &token, int limit);

// Track URIs in the page (removed items and anything that is not spotify:track: skipped) and the token of the next page ("" at
// the end).  False when the bytes are not a PageResponse.
bool collection_page_parse(const uint8_t *data, size_t len, std::vector<std::string> &uris, std::string &next_token);

// spotify:track:<base62> -> the 32 hex digits cspot's metadata request takes.  False on a malformed id.
bool sp_uri_to_hex(const char *uri, char out[33]);
