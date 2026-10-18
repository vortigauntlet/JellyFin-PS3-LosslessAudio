// Album covers for the Spotify app (sp_art.cpp): cspot gives each track a cover address on Spotify's image host; the thumbnail
// cache asks for covers by key, so the address becomes the key "spa:<image id>" and this provider fetches it.
#pragma once

// "spa:<hex id>" from a cover address like https://i.scdn.co/image/<hex id>.  False for anything else (nothing is built from it).
bool sp_art_key(const char *image_url, char *key, int cap);

// thumb_set_local_bytes() provider: the cover's bytes for one of our keys (> 0), 0 for a key that is not ours, -1 on failure.
int sp_art_bytes(const char *key, unsigned char *buf, int cap);
