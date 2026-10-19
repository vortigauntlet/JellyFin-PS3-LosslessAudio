// See sp_lyrics.h.
//
// The worker is one thread for the life of the app (like sp_art's), polling for a new request.  A request is the track's identity;
// the result is published only when no newer track was asked for meanwhile.  Logged: the source, the HTTP status and the line count,
// never an address, a token or the words.
#include "sp_lyrics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/thread.h>
#include <unistd.h>

#include <mutex>
#include <string>
#include <vector>

#include "jf_paths.h"
#include "plog.h"
#include "sp_lyrics_parse.h"
#include "sx_http.h"

namespace {

struct Request {
  std::string uri, title, artist, album;
  unsigned duration_ms = 0;
};

std::mutex s_mtx;                       // guards everything below
Request s_want;                         // the track the screen is showing
unsigned s_want_gen = 0, s_done_gen = 0;
std::vector<LyricLine> s_lines;
SpLyricsState s_state = SPL_NONE;
const char *s_source = "";
bool s_started = false;
int s_enabled = -1;                     // -1 = not read from the file yet

const char *enabled_path() { return jf_data_path("jellyfin_spotify_lyrics.txt"); }

int fetch(const char *url, const char *headers, char **body, size_t *len) {
  sx_http_req rq;
  memset(&rq, 0, sizeof rq);
  rq.url = url;
  rq.headers = headers;
  rq.range_start = -1;
  rq.range_end = -1;
  rq.connect_ms = 8000;
  rq.handshake_ms = 12000;
  rq.first_byte_ms = 12000;
  rq.idle_ms = 8000;
  sx_http_info info;
  memset(&info, 0, sizeof info);
  *body = NULL;
  *len = 0;
  for (int attempt = 0;; attempt++) {
    const int rc = sx_http_fetch(&rq, 1u << 20, body, len, &info);
    if (rc != SX_OK) { free(*body); *body = NULL; return rc < 0 ? rc : -1; }
    // LRCLIB answers 503 when it is overloaded (seen on the console): one more try after a pause
    if (attempt == 0 && info.status >= 502 && info.status <= 504) {
      free(*body);
      *body = NULL;
      *len = 0;
      usleep(1500000);
      continue;
    }
    return info.status;
  }
}

void log_result(const char *source, int status, size_t lines, bool synced) {
  char b[96];
  snprintf(b, sizeof b, "spotify: lyrics %s status=%d lines=%u %s", source, status, (unsigned)lines, synced ? "synced" : "plain");
  plog(b);
}

// Spotify's own lyrics: the same service the web player asks.
bool from_spotify(const Request &r, std::vector<LyricLine> &out, bool *synced) {
  const char kPrefix[] = "spotify:track:";
  if (r.uri.compare(0, sizeof kPrefix - 1, kPrefix) != 0) return false;
  char token[2048];
  if (!sp_svc_access_token(token, sizeof token)) return false;
  char url[200];
  snprintf(url, sizeof url, "https://spclient.wg.spotify.com/color-lyrics/v2/track/%s?format=json&vocalRemoval=false&market=from_token",
           r.uri.c_str() + sizeof kPrefix - 1);
  std::string headers = std::string("Authorization: Bearer ") + token + "\r\napp-platform: WebPlayer\r\nAccept: application/json\r\n";
  memset(token, 0, sizeof token);
  char *body;
  size_t len;
  const int status = fetch(url, headers.c_str(), &body, &len);
  std::fill(headers.begin(), headers.end(), '\0');
  bool ok = false;
  if (status == 200 && body) ok = spotify_lyrics_parse(body, len, out, synced);
  free(body);
  log_result("spotify", status, ok ? out.size() : 0, ok && *synced);
  return ok;
}

// LRCLIB: the exact match (title, artist, album, length within a couple of seconds), then a search by title and artist.
bool from_lrclib(const Request &r, std::vector<LyricLine> &out, bool *synced) {
  if (r.title.empty() || r.artist.empty()) return false;
  const char *headers = NULL;          // LRCLIB asks only for a User-Agent naming the app (set in main.cpp)
  std::string url = "https://lrclib.net/api/get?track_name=" + url_encode(r.title.c_str()) +
                    "&artist_name=" + url_encode(r.artist.c_str());
  if (!r.album.empty()) url += "&album_name=" + url_encode(r.album.c_str());
  if (r.duration_ms) {
    char d[16];
    snprintf(d, sizeof d, "%u", (r.duration_ms + 500) / 1000);
    url += std::string("&duration=") + d;
  }
  char *body;
  size_t len;
  int status = fetch(url.c_str(), headers, &body, &len);
  bool ok = status == 200 && body && lrclib_parse(body, len, out, synced);
  free(body);
  log_result("lrclib-get", status, ok ? out.size() : 0, ok && *synced);
  if (ok) return true;
  if (status != 404 && status != 200) return false;     // the service is down or refusing: do not search as well

  url = "https://lrclib.net/api/search?track_name=" + url_encode(r.title.c_str()) +
        "&artist_name=" + url_encode(r.artist.c_str());
  status = fetch(url.c_str(), headers, &body, &len);
  ok = status == 200 && body && lrclib_parse(body, len, out, synced);
  free(body);
  log_result("lrclib-search", status, ok ? out.size() : 0, ok && *synced);
  return ok;
}

void worker(void *) {
  for (;;) {
    Request r;
    unsigned gen;
    bool idle;
    {
      std::lock_guard<std::mutex> g(s_mtx);
      gen = s_want_gen;
      idle = gen == s_done_gen;
      if (!idle) r = s_want;
      if (!idle && r.title.empty() && r.uri.empty()) { s_done_gen = gen; idle = true; }   // the track went away
    }
    if (idle) { usleep(50000); continue; }

    std::vector<LyricLine> lines;
    bool synced = false;
    const char *source = "";
    if (from_spotify(r, lines, &synced)) source = "Spotify";
    else if (from_lrclib(r, lines, &synced)) source = "LRCLIB";
    else lines.clear();

    std::lock_guard<std::mutex> g(s_mtx);
    if (gen != s_want_gen) continue;                  // the track changed while this one was fetched
    s_lines.swap(lines);
    s_source = source;
    s_state = s_lines.empty() ? SPL_MISSING : synced ? SPL_SYNCED : SPL_UNSYNCED;
    s_done_gen = gen;
  }
}

void start_worker() {
  if (s_started) return;
  s_started = true;
  sys_ppu_thread_t t;
  if (sysThreadCreate(&t, worker, nullptr, 1010, 256 * 1024, 0, (char *)"sp_lyrics") != 0) {
    plog("spotify: could not start the lyrics thread");
  }
}

}  // namespace

void sp_lyrics_follow(const SpTrack *track, bool ok) {
  std::lock_guard<std::mutex> g(s_mtx);
  if (!ok || !track) {
    if (!s_want.title.empty() || !s_want.uri.empty()) {
      s_want = Request();
      s_want_gen++;
      s_lines.clear();
      s_state = SPL_NONE;
      s_source = "";
    }
    return;
  }
  if (s_want.uri == track->uri && s_want.title == track->title && s_want.artist == track->artist) return;
  s_want.uri = track->uri;
  s_want.title = track->title;
  s_want.artist = track->artist;
  s_want.album = track->album;
  s_want.duration_ms = track->duration_ms;
  s_want_gen++;
  s_lines.clear();
  s_state = SPL_LOADING;
  s_source = "";
  start_worker();
}

SpLyricsState sp_lyrics_state(void) {
  std::lock_guard<std::mutex> g(s_mtx);
  return s_state;
}

const char *sp_lyrics_source(void) {
  std::lock_guard<std::mutex> g(s_mtx);
  return s_source;
}

int sp_lyrics_count(void) {
  std::lock_guard<std::mutex> g(s_mtx);
  return (int)s_lines.size();
}

int sp_lyrics_index_at(uint32_t ms) {
  std::lock_guard<std::mutex> g(s_mtx);
  return s_state == SPL_SYNCED ? lyric_index_at(s_lines, ms) : -1;
}

uint32_t sp_lyrics_time(int index) {
  std::lock_guard<std::mutex> g(s_mtx);
  return index >= 0 && index < (int)s_lines.size() ? s_lines[(size_t)index].ms : 0;
}

bool sp_lyrics_text(int index, char *out, size_t cap) {
  std::lock_guard<std::mutex> g(s_mtx);
  if (index < 0 || index >= (int)s_lines.size() || cap == 0) return false;
  snprintf(out, cap, "%s", s_lines[(size_t)index].text.c_str());
  return true;
}

bool sp_lyrics_enabled(void) {
  if (s_enabled < 0) {
    s_enabled = 1;
    FILE *f = fopen(enabled_path(), "r");
    if (f) {
      int c = fgetc(f);
      if (c == '0') s_enabled = 0;
      fclose(f);
    }
  }
  return s_enabled != 0;
}

void sp_lyrics_set_enabled(bool on) {
  s_enabled = on ? 1 : 0;
  FILE *f = fopen(enabled_path(), "w");
  if (f) { fputs(on ? "1\n" : "0\n", f); fclose(f); }
}
