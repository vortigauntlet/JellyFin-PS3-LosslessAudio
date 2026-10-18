// Album covers for the Spotify app: see sp_art.h.
//
// The thumbnail thread has a 64 KB stack, which is little for a TLS handshake, so the fetch runs on a worker of its own (128 KB) and
// the provider waits for it; one request at a time, since only the thumbnail thread asks.  Only addresses of the form
// https://i.scdn.co/image/<hex> are ever built or fetched: the key carries the hex id and nothing else.
#include "sp_art.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sx_http.h"
#include "sx_plat.h"

namespace {

const char kHost[] = "https://i.scdn.co/image/";

bool hex_id(const char *s, int *len) {
  int n = 0;
  while (s[n]) {
    const char c = s[n];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    n++;
  }
  if (n < 16 || n > 64) return false;
  *len = n;
  return true;
}

struct {
  volatile int state;       // 0 idle, 1 asked, 2 answered
  char url[120];
  int cap;
  int rc;
  int status;
  char *body;
  size_t len;
} s_art;
bool s_started;

void art_worker(void *) {
  for (;;) {
    if (s_art.state != 1) { usleep(15000); continue; }
    __sync_synchronize();
    sx_http_req rq;
    memset(&rq, 0, sizeof rq);
    rq.url = s_art.url;
    rq.range_start = -1;
    rq.range_end = -1;
    rq.connect_ms = 6000;
    rq.handshake_ms = 8000;
    rq.first_byte_ms = 10000;
    rq.idle_ms = 6000;
    sx_http_info info;
    memset(&info, 0, sizeof info);
    s_art.body = NULL;
    s_art.len = 0;
    s_art.rc = sx_http_fetch(&rq, (size_t)s_art.cap, &s_art.body, &s_art.len, &info);
    s_art.status = info.status;
    memset(s_art.url, 0, sizeof s_art.url);
    __sync_synchronize();
    s_art.state = 2;
  }
}

}  // namespace

bool sp_art_key(const char *image_url, char *key, int cap) {
  if (!image_url || strncmp(image_url, kHost, sizeof kHost - 1) != 0) return false;
  const char *id = image_url + sizeof kHost - 1;
  int n;
  if (!hex_id(id, &n) || cap < n + 5) return false;
  snprintf(key, (size_t)cap, "spa:%s", id);
  return true;
}

int sp_art_bytes(const char *key, unsigned char *buf, int cap) {
  if (!key || strncmp(key, "spa:", 4) != 0) return 0;
  int n;
  if (!hex_id(key + 4, &n)) return -1;
  char url[120];
  snprintf(url, sizeof url, "%s%s", kHost, key + 4);
  if (!s_started) {
    s_started = true;
    if (!sx_thread_spawn(art_worker, NULL, 1500, 128, "sp_art")) { s_started = false; return -1; }
  }
  // An earlier request may still be running, or have finished after its caller gave up: settle it first.
  for (int t = 0; t < 1500 && s_art.state != 0; t++) {
    if (s_art.state == 2) { free(s_art.body); s_art.body = NULL; s_art.state = 0; break; }
    usleep(10000);
  }
  if (s_art.state != 0) return -1;
  snprintf(s_art.url, sizeof s_art.url, "%s", url);
  s_art.cap = cap;
  __sync_synchronize();
  s_art.state = 1;
  for (int t = 0; t < 1300 && s_art.state != 2; t++) usleep(10000);
  if (s_art.state != 2) return -1;                     // still busy: the next call settles it
  __sync_synchronize();
  int r = -1;
  if (s_art.rc == SX_OK && s_art.status == 200 && s_art.body && s_art.len > 0 && s_art.len <= (size_t)cap) {
    memcpy(buf, s_art.body, s_art.len);
    r = (int)s_art.len;
  }
  free(s_art.body);
  s_art.body = NULL;
  s_art.state = 0;
  return r;
}
