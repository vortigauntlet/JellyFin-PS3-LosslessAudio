// The Spotify Connect receiver: see sp_svc.h.
//
//   * login: a credential saved by an earlier pairing (jellyfin_spotify_creds in the app's data folder, written only by this
//     code, never logged), else the console advertises itself and the Spotify app sends one (sp_zeroconf.cpp);
//   * session: cspot's Session / SpircHandler on a worker thread, which gets the audio keys and the CDN stream and decodes
//     Vorbis with Tremor into a small ring of 44.1 kHz 16-bit stereo;
//   * output: sp_svc_pcm_read() takes frames out of that ring in real time for the music engine.
// The audio is never written to disk and the ring holds about two seconds.
#include "sp_svc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/thread.h>
#include <unistd.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "CSpotContext.h"
#include "LoginBlob.h"
#include "SpircHandler.h"
#include "TrackPlayer.h"
#include "WrappedSemaphore.h"
#include "jf_paths.h"
#include "meminfo.h"
#include "plog.h"
#include "sp_log.h"
#include "sp_collection.h"
#include "sp_lyrics_parse.h"
#include "protobuf/metadata.pb.h"
#include "NanoPBHelper.h"
#include "sp_zeroconf.h"
#include "sx_http.h"
#include "sx_plat.h"

namespace {

const size_t kRingFrames = 44100 * 2;          // two seconds

std::atomic<bool> s_run{false};
std::atomic<bool> s_thread_done{true};        // the session thread has returned (or never started)
std::atomic<int> s_state{SP_OFF};
std::atomic<unsigned> s_serial{0}, s_frames_in{0}, s_tracks{0}, s_audio_generation{0};
std::string s_name = "PS3";
char s_status[96] = "Starting";
std::mutex s_mtx;                              // guards the ring, the boundaries, the track and the status text

// ---- the PCM ring, with the track boundaries inside it -----------------------------------------------------------------
std::vector<int16_t> s_ring;                   // kRingFrames * 2 samples
size_t s_rd = 0, s_wr = 0, s_fill = 0;         // in frames
struct Boundary { size_t at_total; };          // a new track starts at this many frames after the ring's origin
std::vector<Boundary> s_bounds;
size_t s_total_in = 0, s_total_out = 0;        // frames ever written / read since the last flush
size_t s_last_hash = 0;
bool s_have_hash = false;
SpTrack s_track;
bool s_have_track = false;
unsigned s_pos_base_ms = 0;                    // position at the start of the current track's first frame read
size_t s_pos_origin_out = 0;                   // s_total_out when that position was set
bool s_paused = false, s_depleted = false;
uint64_t s_last_pos_report = 0;

std::shared_ptr<cspot::SpircHandler> s_handler;
std::shared_ptr<cspot::Context> s_ctx;
sys_ppu_thread_t s_thread_tid = 0;
std::atomic<int> s_quality{160};

// Liked Songs: the URIs, page by page from the collection service, and each row's title and artist from cspot's metadata channel,
// asked for around the selection.  All guarded by s_mtx.
struct LibRow {
  std::string uri;
  SpTrack meta;
  bool have = false, asked = false;
};
std::vector<LibRow> s_lib;
std::string s_lib_next;                         // the next page's token
bool s_lib_done = false;                        // the last page arrived
bool s_lib_want_page = false;                   // the UI is near the end of what is loaded
bool s_lib_fetching = false;
int s_lib_want_lo = 0, s_lib_want_hi = -1;      // rows whose metadata the UI wants
uint64_t s_lib_retry_at_ms = 0;
char s_library_status[96] = "";
bool s_lib_worker = false;

std::shared_ptr<cspot::SpircHandler> handler_snapshot() {
  std::lock_guard<std::mutex> g(s_mtx);
  return s_handler;
}

const char* quality_path() { return jf_data_path("jellyfin_spotify_quality.txt"); }

AudioFormat format_for(int kbps) {
  return kbps == 96 ? AudioFormat_OGG_VORBIS_96 : kbps == 320 ? AudioFormat_OGG_VORBIS_320 : AudioFormat_OGG_VORBIS_160;
}

void load_quality() {
  FILE* f = fopen(quality_path(), "r");
  if (!f) return;
  int v = 160;
  if (fscanf(f, "%d", &v) != 1) v = 160;
  fclose(f);
  s_quality = v == 96 ? 96 : v == 320 ? 320 : 160;
}

void set_status(const char* t) {
  std::lock_guard<std::mutex> g(s_mtx);
  snprintf(s_status, sizeof s_status, "%s", t);
}

// keep_track: a seek within the same song.  Its audio after the flush has the same track id, so no boundary is made for it
// (a boundary tells Spotify a new track reached the speakers, and cspot then moves its queue on to the next song).
void ring_clear_locked(bool keep_track = false) {
  s_rd = s_wr = s_fill = 0;
  s_bounds.clear();
  s_total_in = s_total_out = 0;
  if (!keep_track) s_have_hash = false;
}

void set_track_locked(const cspot::TrackInfo& ti) {
  snprintf(s_track.title, sizeof s_track.title, "%s", ti.name.c_str());
  snprintf(s_track.artist, sizeof s_track.artist, "%s", ti.artist.c_str());
  snprintf(s_track.album, sizeof s_track.album, "%s", ti.album.c_str());
  snprintf(s_track.image, sizeof s_track.image, "%s", ti.imageUrl.c_str());
  char b62[23];
  if (sp_hex_to_base62(ti.trackId.c_str(), b62)) snprintf(s_track.uri, sizeof s_track.uri, "spotify:track:%s", b62);
  else s_track.uri[0] = '\0';
  s_track.duration_ms = ti.duration;
  s_have_track = true;
}

// ---- saved credential --------------------------------------------------------------------------------------------------------

const char* creds_path() { return jf_data_path("jellyfin_spotify_creds"); }

std::string read_creds() {
  FILE* f = fopen(creds_path(), "rb");
  if (!f) return "";
  std::string s;
  char buf[512];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0 && s.size() < 16384) s.append(buf, n);
  fclose(f);
  return s;
}

void write_creds(const std::string& json) {
  FILE* f = fopen(creds_path(), "wb");
  if (!f) { plog("spotify: could not save the sign-in"); return; }
  fwrite(json.data(), 1, json.size(), f);
  fclose(f);
}

void forget_creds() { remove(creds_path()); }

// ---- cspot -> ring ---------------------------------------------------------------------------------------------------------------

size_t on_pcm(uint8_t* data, size_t bytes, std::string_view track_id) {
  std::lock_guard<std::mutex> g(s_mtx);
  if (s_paused && s_fill >= kRingFrames) return 0;
  const size_t hash = std::hash<std::string_view>()(track_id);
  if (!s_have_hash || hash != s_last_hash) {
    s_have_hash = true;
    s_last_hash = hash;
    s_bounds.push_back({s_total_in});
  }
  const size_t frames_avail = kRingFrames - s_fill;
  size_t frames = bytes / 4;
  if (frames > frames_avail) frames = frames_avail;
  const int16_t* src = (const int16_t*)data;
  for (size_t i = 0; i < frames; i++) {
    s_ring[s_wr * 2] = src[i * 2];
    s_ring[s_wr * 2 + 1] = src[i * 2 + 1];
    s_wr = (s_wr + 1) % kRingFrames;
  }
  s_fill += frames;
  s_total_in += frames;
  s_frames_in += (unsigned)frames;
  return frames * 4;                           // 0 when the ring is full: cspot waits 50 ms and tries again
}

void on_event(std::unique_ptr<cspot::SpircHandler::Event> ev) {
  using ET = cspot::SpircHandler::EventType;
  switch (ev->eventType) {
    case ET::PLAY_PAUSE: {
      const bool paused = std::get<bool>(ev->data);
      std::lock_guard<std::mutex> g(s_mtx);
      s_paused = paused;
      s_state = paused ? SP_PAUSED : SP_PLAYING;
      break;
    }
    case ET::FLUSH: {
      std::lock_guard<std::mutex> g(s_mtx);
      ring_clear_locked();
      ++s_audio_generation;
      break;
    }
    case ET::DISC: {                           // another device took the music: this one is idle again
      std::lock_guard<std::mutex> g(s_mtx);
      ring_clear_locked();
      ++s_audio_generation;
      s_have_track = false;
      s_paused = false;
      s_state = SP_READY;
      snprintf(s_status, sizeof s_status, "Playing on another device");
      break;
    }
    case ET::SEEK: {
      std::lock_guard<std::mutex> g(s_mtx);
      ring_clear_locked(true);
      s_pos_base_ms = (unsigned)std::get<int>(ev->data);
      s_pos_origin_out = 0;
      ++s_audio_generation;
      break;
    }
    case ET::PLAYBACK_START: {
      std::lock_guard<std::mutex> g(s_mtx);
      ring_clear_locked();
      s_paused = false;
      s_depleted = false;
      s_pos_base_ms = 0;
      s_pos_origin_out = 0;
      s_state = SP_PLAYING;
      ++s_audio_generation;
      break;
    }
    case ET::TRACK_INFO: {
      const cspot::TrackInfo& ti = std::get<cspot::TrackInfo>(ev->data);
      std::lock_guard<std::mutex> g(s_mtx);
      set_track_locked(ti);
      s_serial++;
      s_tracks++;
      break;
    }
    case ET::DEPLETED: {
      std::lock_guard<std::mutex> g(s_mtx);
      s_depleted = true;
      break;
    }
    default:
      break;
  }
}

// ---- the session thread ------------------------------------------------------------------------------------------------------

const char* http_failure(const char* what, int rc, int status) {
  static char out[96];
  if (rc != SX_OK) snprintf(out, sizeof out, "%s: no connection to Spotify (%d)", what, rc);
  else if (status == 401 || status == 403) snprintf(out, sizeof out, "%s: Spotify refused the request (%d)", what, status);
  else if (status == 429) snprintf(out, sizeof out, "%s: Spotify is busy, retrying shortly", what);
  else snprintf(out, sizeof out, "%s: Spotify answered %d", what, status);
  return out;
}

// One page of the collection.  Returns false (with the status line set) when it failed.
bool fetch_collection_page(const std::shared_ptr<cspot::Context>& ctx, const std::string& page_token) {
  char token[2048];
  if (!sp_svc_access_token(token, sizeof token)) {
    std::lock_guard<std::mutex> g(s_mtx);
    snprintf(s_library_status, sizeof s_library_status, "Liked Songs: waiting for Spotify sign-in");
    return false;
  }
  const std::string body = collection_page_request(ctx->config.username, "collection", page_token, 300);
  std::string headers = std::string("Authorization: Bearer ") + token +
                        "\r\nAccept: application/vnd.collection-v2.spotify.proto\r\napp-platform: WebPlayer\r\n";
  memset(token, 0, sizeof token);
  sx_http_req rq;
  memset(&rq, 0, sizeof rq);
  rq.method = "POST";
  rq.url = "https://spclient.wg.spotify.com/collection/v2/paging";
  rq.headers = headers.c_str();
  rq.body = body.data();
  rq.body_len = (int)body.size();
  rq.content_type = "application/vnd.collection-v2.spotify.proto";
  rq.range_start = -1;
  rq.range_end = -1;
  rq.connect_ms = 8000;
  rq.handshake_ms = 12000;
  rq.first_byte_ms = 15000;
  rq.idle_ms = 10000;
  sx_http_info info;
  memset(&info, 0, sizeof info);
  char* resp = NULL;
  size_t len = 0;
  const int rc = sx_http_fetch(&rq, 4u << 20, &resp, &len, &info);
  std::fill(headers.begin(), headers.end(), '\0');
  std::vector<std::string> uris;
  std::string next;
  const bool ok = rc == SX_OK && info.status == 200 && resp &&
                  collection_page_parse((const uint8_t*)resp, len, uris, next);
  free(resp);
  char line[112];
  snprintf(line, sizeof line, "spotify: liked songs page rc=%d status=%d bytes=%u tracks=%u more=%d",
           rc, info.status, (unsigned)len, (unsigned)uris.size(), next.empty() ? 0 : 1);
  plog(line);

  std::lock_guard<std::mutex> g(s_mtx);
  if (!ok) {
    snprintf(s_library_status, sizeof s_library_status, "%s",
             http_failure("Liked Songs", rc, rc == SX_OK && info.status == 200 ? -1 : info.status));
    return false;
  }
  for (auto& u : uris) {
    LibRow row;
    row.uri = u;
    s_lib.push_back(row);
  }
  s_lib_next = next;
  s_lib_done = next.empty() || uris.empty();
  s_library_status[0] = '\0';
  return true;
}

// Title, artist, album and cover of one row, through the same metadata request cspot makes for the tracks it plays.
void request_row_meta(const std::shared_ptr<cspot::Context>& ctx, int index, const std::string& uri) {
  char hex[33];
  if (!sp_uri_to_hex(uri.c_str(), hex)) return;
  std::vector<uint8_t> gid(16);
  for (int i = 0; i < 16; i++) {
    auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    gid[i] = (uint8_t)(nib(hex[i * 2]) * 16 + nib(hex[i * 2 + 1]));
  }
  const std::string url = std::string("hm://metadata/3/track/") + hex;
  ctx->session->execute(cspot::MercurySession::RequestType::GET, url,
                        [index, uri, gid](cspot::MercurySession::Response& res) {
    if (res.parts.empty()) return;
    Track* t = new Track;
    *t = Track_init_zero;
    pbDecode(*t, Track_fields, res.parts[0]);
    cspot::TrackInfo ti;
    ti.loadPbTrack(t, gid);
    pb_release(Track_fields, t);
    delete t;
    std::lock_guard<std::mutex> g(s_mtx);
    if (index < 0 || index >= (int)s_lib.size() || s_lib[(size_t)index].uri != uri) return;   // the list was reloaded
    SpTrack& m = s_lib[(size_t)index].meta;
    memset(&m, 0, sizeof m);
    snprintf(m.title, sizeof m.title, "%s", ti.name.c_str());
    snprintf(m.artist, sizeof m.artist, "%s", ti.artist.c_str());
    snprintf(m.album, sizeof m.album, "%s", ti.album.c_str());
    snprintf(m.image, sizeof m.image, "%s", ti.imageUrl.c_str());
    snprintf(m.uri, sizeof m.uri, "%s", uri.c_str());
    m.duration_ms = ti.duration;
    s_lib[(size_t)index].have = true;
  });
}

// One thread for the app's life (like the covers' and the lyrics'): pages when the UI nears the end, metadata around the selection.
void library_worker(void*) {
  for (;;) {
    usleep(40000);
    std::shared_ptr<cspot::Context> ctx;
    std::string page_token;
    bool page = false;
    std::vector<std::pair<int, std::string>> meta;
    {
      std::lock_guard<std::mutex> g(s_mtx);
      ctx = s_ctx;
      if (!ctx || s_state.load() < SP_READY || s_state.load() == SP_ERROR) continue;
      if (s_lib_want_page && !s_lib_done && sx_now_ms() >= s_lib_retry_at_ms) {
        page = true;
        page_token = s_lib_next;
        s_lib_fetching = true;
      }
      const int lo = s_lib_want_lo < 0 ? 0 : s_lib_want_lo;
      for (int i = lo; i <= s_lib_want_hi && i < (int)s_lib.size() && meta.size() < 12; i++) {
        LibRow& r = s_lib[(size_t)i];
        if (!r.asked) { r.asked = true; meta.push_back({i, r.uri}); }
      }
    }
    if (page) {
      const bool ok = fetch_collection_page(ctx, page_token);
      std::lock_guard<std::mutex> g(s_mtx);
      s_lib_fetching = false;
      s_lib_want_page = false;
      if (!ok) s_lib_retry_at_ms = sx_now_ms() + 20000;
    }
    for (auto& m : meta) request_row_meta(ctx, m.first, m.second);
  }
}

void start_library_worker() {
  if (s_lib_worker) return;
  s_lib_worker = true;
  sys_ppu_thread_t t;
  if (sysThreadCreate(&t, library_worker, nullptr, 1002, 256 * 1024, 0, (char*)"sp_library") != 0)
    plog("spotify: could not start the library thread");
}

void session_once() {
  auto blob = std::make_shared<cspot::LoginBlob>(s_name);
  bool from_file = false;
  const std::string saved = read_creds();
  if (!saved.empty()) {
    try {
      blob->loadJson(saved);
      from_file = true;
    } catch (...) {
      forget_creds();
    }
  }

  if (!from_file) {
    bell::WrappedSemaphore got(4);
    s_state = SP_PAIRING;
    set_status("Open Spotify on your phone, tap Connect to a device, pick this console");
    if (!sp_zc_start(blob.get(), s_name.c_str(), [&got]() { got.give(); })) {
      s_state = SP_ERROR;
      set_status("No network address: connect the console to your network");
      sleep(5);
      return;
    }
    while (s_run && got.twait(500) != 0) {}
    sp_zc_stop();
    if (!s_run) return;
  }

  s_state = SP_SIGNING_IN;
  set_status("Signing in to Spotify");
  auto ctx = cspot::Context::createFromBlob(blob);
  ctx->config.audioFormat = format_for(s_quality);
  { std::lock_guard<std::mutex> g(s_mtx); s_ctx = ctx; }
  ctx->session->connectWithRandomAp();
  ctx->config.authData = ctx->session->authenticate(blob);
  if (ctx->config.authData.empty()) {
    if (from_file) forget_creds();
    s_state = SP_ERROR;
    set_status(from_file ? "Spotify did not accept the saved sign-in: pair again" : "Spotify refused the sign-in");
    sleep(3);
    return;
  }
  write_creds(ctx->getCredentialsJson());     // reusable on the next start; never logged

  {
    std::lock_guard<std::mutex> g(s_mtx);
    s_handler = std::make_shared<cspot::SpircHandler>(ctx);
  }
  auto handler = handler_snapshot();
  handler->getTrackPlayer()->setDataCallback(on_pcm);
  handler->setEventHandler(on_event);
  ctx->session->startTask();
  s_state = SP_READY;
  set_status("Ready: pick this console in the Spotify app to play");
  { char b[64]; snprintf(b, sizeof b, "spotify: signed in, free=%uKB", meminfo_avail_kb()); plog(b); }

  try {
    while (s_run) ctx->session->handlePacket();
  } catch (std::exception& e) {
    plog("spotify: the session ended");
  }
  handler = handler_snapshot();
  if (handler) handler->disconnect();
  else ctx->session->disconnect();
  {
    std::lock_guard<std::mutex> g(s_mtx);
    s_handler.reset();
    s_ctx.reset();
  }
}

void session_thread(void*) {
  sp_log_install();
  while (s_run) {
    try {
      session_once();
    } catch (std::exception& e) {
      s_state = SP_ERROR;
      set_status("Could not reach Spotify: retrying");
    }
    for (int i = 0; i < 10 && s_run; i++) usleep(500000);
  }
  s_state = SP_OFF;
  s_thread_done = true;
  sysThreadExit(0);
}

}  // namespace

// ---- public ----------------------------------------------------------------------------------------------------------------------------------

void sp_svc_start(const char* device_name) {
  if (s_run) return;
  { char b[64]; snprintf(b, sizeof b, "spotify: starting, free=%uKB", meminfo_avail_kb()); plog(b); }
  s_name = device_name && *device_name ? device_name : "PS3";
  load_quality();
  s_ring.assign(kRingFrames * 2, 0);
  memset(&s_track, 0, sizeof s_track);
  {
    std::lock_guard<std::mutex> g(s_mtx);
    s_lib.clear();
    s_lib_next.clear();
    s_lib_done = s_lib_want_page = s_lib_fetching = false;
    s_lib_want_lo = 0;
    s_lib_want_hi = -1;
    s_lib_retry_at_ms = 0;
    s_library_status[0] = '\0';
  }
  s_run = true;
  s_thread_done = false;
  if (sysThreadCreate(&s_thread_tid, session_thread, nullptr, 1001, 256 * 1024,
                      THREAD_JOINABLE, (char*)"sp_session") != 0) {
    s_run = false;
    s_thread_done = true;
    plog("spotify: could not start the session thread");
  }
}

// Asks the session thread to leave and waits a few seconds for it, so the app can exit without a worker still running.  A thread
// blocked in the network is not forced: the wait is bounded and the app exits anyway (the console ends it).
void sp_svc_stop(void) {
  s_run = false;
  sp_zc_stop();

  // Wake an established packet wait before joining the session thread.
  auto handler = handler_snapshot();
  if (handler) handler->requestSessionStop();

  for (int i = 0; i < 80 && !s_thread_done; i++) usleep(100000);
  if (!s_thread_done) {
    plog("spotify: session shutdown is still waiting");
    return;
  }
  if (s_thread_tid) {
    u64 result = 0;
    sysThreadJoin(s_thread_tid, &result);
    s_thread_tid = 0;
  }
}

int sp_svc_quality(void) { return s_quality; }

void sp_svc_set_quality(int kbps) {
  s_quality = kbps == 96 ? 96 : kbps == 320 ? 320 : 160;
  FILE* f = fopen(quality_path(), "w");
  if (f) { fprintf(f, "%d\n", s_quality.load()); fclose(f); }
  std::shared_ptr<cspot::Context> c;
  { std::lock_guard<std::mutex> g(s_mtx); c = s_ctx; }
  if (c) c->config.audioFormat = format_for(s_quality);      // read when the next track is loaded
}

void sp_svc_forget(void) { forget_creds(); plog("spotify: saved account forgotten"); }

SpState sp_svc_state(void) { return (SpState)s_state.load(); }

const char* sp_svc_status_text(void) {
  static char out[96];
  std::lock_guard<std::mutex> g(s_mtx);
  snprintf(out, sizeof out, "%s", s_status);
  return out;
}

bool sp_svc_track(SpTrack* out) {
  std::lock_guard<std::mutex> g(s_mtx);
  if (!s_have_track) return false;
  if (out) *out = s_track;
  return true;
}

unsigned sp_svc_track_serial(void) { return s_serial.load(); }

bool sp_svc_access_token(char* out, size_t cap) {
  std::shared_ptr<cspot::SpircHandler> h = handler_snapshot();
  auto q = h ? h->getTrackQueue() : nullptr;
  const std::string token = q ? q->getAccessToken() : "";
  if (token.empty() || token.size() >= cap) return false;
  memcpy(out, token.c_str(), token.size() + 1);
  return true;
}
unsigned sp_svc_audio_generation(void) { return s_audio_generation.load(); }

unsigned sp_svc_queue_count(void) {
  std::shared_ptr<cspot::SpircHandler> h = handler_snapshot();
  if (!h) return 0;
  auto q = h->getTrackQueue();
  return q ? (unsigned)q->trackCount() : 0;
}

int sp_svc_queue_current(void) {
  std::shared_ptr<cspot::SpircHandler> h = handler_snapshot();
  if (!h) return -1;
  auto q = h->getTrackQueue();
  return q ? q->currentTrackIndex() : -1;
}

bool sp_svc_queue_track(unsigned index, SpTrack* out) {
  std::shared_ptr<cspot::SpircHandler> h = handler_snapshot();
  if (!h || !out) return false;
  auto q = h->getTrackQueue();
  if (!q) return false;
  const cspot::TrackInfo ti = q->trackInfoAt((int)index);
  if (ti.name.empty()) return false;
  memset(out, 0, sizeof(*out));
  snprintf(out->title, sizeof out->title, "%s", ti.name.c_str());
  snprintf(out->artist, sizeof out->artist, "%s", ti.artist.c_str());
  snprintf(out->album, sizeof out->album, "%s", ti.album.c_str());
  snprintf(out->image, sizeof out->image, "%s", ti.imageUrl.c_str());
  out->duration_ms = ti.duration;
  return true;
}

bool sp_svc_play_queue_track(unsigned index) {
  std::shared_ptr<cspot::SpircHandler> h = handler_snapshot();
  return h && h->playTrackAt((int)index);
}

void sp_svc_library_request(unsigned index) {
  start_library_worker();
  std::lock_guard<std::mutex> g(s_mtx);
  if ((size_t)index + 40 >= s_lib.size() && !s_lib_done) s_lib_want_page = true;
  s_lib_want_lo = (int)index - 6;
  s_lib_want_hi = (int)index + 14;
}

unsigned sp_svc_library_total(void) {
  std::lock_guard<std::mutex> g(s_mtx);
  return (unsigned)s_lib.size();
}

bool sp_svc_library_more(void) {
  std::lock_guard<std::mutex> g(s_mtx);
  return !s_lib_done;
}

unsigned sp_svc_library_offset(void) { return 0; }

bool sp_svc_library_loading(void) {
  std::lock_guard<std::mutex> g(s_mtx);
  return s_lib.empty() && !s_lib_done && s_library_status[0] == '\0';
}

bool sp_svc_library_track(unsigned index, SpTrack* out) {
  if (!out) return false;
  std::lock_guard<std::mutex> g(s_mtx);
  if (index >= s_lib.size() || !s_lib[index].have) return false;
  *out = s_lib[index].meta;
  return true;
}

const char* sp_svc_library_status_text(void) {
  static char out[96];
  std::lock_guard<std::mutex> g(s_mtx);
  snprintf(out, sizeof out, "%s", s_library_status);
  return out;
}

// The chosen song and up to 199 after it go to cspot as a local Load: it plays here and the phone shows it, no Web API involved.
bool sp_svc_library_play(unsigned index) {
  std::vector<std::string> uris;
  std::string context;
  std::shared_ptr<cspot::Context> ctx;
  {
    std::lock_guard<std::mutex> g(s_mtx);
    if (index >= s_lib.size()) return false;
    for (size_t i = index; i < s_lib.size() && uris.size() < 200; i++) uris.push_back(s_lib[i].uri);
    ctx = s_ctx;
  }
  auto h = handler_snapshot();
  if (!h || !ctx) return false;
  context = "spotify:user:" + ctx->config.username + ":collection";
  bool ok = false;
  try {
    ok = h->loadLocalTracks(uris, 0, context);
  } catch (std::exception&) {
    ok = false;
  }
  char line[64];
  snprintf(line, sizeof line, "spotify: play liked song n=%u ok=%d", (unsigned)uris.size(), ok ? 1 : 0);
  plog(line);
  std::lock_guard<std::mutex> g(s_mtx);
  if (!ok) snprintf(s_library_status, sizeof s_library_status, "Could not start that song");
  else s_library_status[0] = '\0';
  return ok;
}

unsigned sp_svc_position_ms(void) {
  std::lock_guard<std::mutex> g(s_mtx);
  return s_pos_base_ms + (unsigned)(((s_total_out - s_pos_origin_out) * 1000ULL) / 44100ULL);
}

size_t sp_svc_pcm_read(int16_t* dst, size_t max_frames) {
  std::shared_ptr<cspot::SpircHandler> h = handler_snapshot();
  bool reached = false, ended = false;
  unsigned report_pos = 0;
  bool report = false;
  size_t frames = 0;
  {
    std::lock_guard<std::mutex> g(s_mtx);
    if (s_paused) return 0;
    frames = s_fill < max_frames ? s_fill : max_frames;
    // stop at a track boundary so Spotify is told the new track reached the speakers exactly when its first frame is read
    if (!s_bounds.empty()) {
      const size_t at = s_bounds.front().at_total;
      if (at == s_total_out) {
        s_bounds.erase(s_bounds.begin());
        s_pos_base_ms = 0;
        s_pos_origin_out = s_total_out;
        reached = true;
      } else if (at > s_total_out && at - s_total_out < frames) {
        frames = at - s_total_out;
      }
    }
    for (size_t i = 0; i < frames; i++) {
      dst[i * 2] = s_ring[s_rd * 2];
      dst[i * 2 + 1] = s_ring[s_rd * 2 + 1];
      s_rd = (s_rd + 1) % kRingFrames;
    }
    s_fill -= frames;
    s_total_out += frames;
    if (s_fill == 0 && s_depleted) {
      s_depleted = false;
      ended = true;
    }
    const uint64_t now = sx_now_ms();
    if (frames && now - s_last_pos_report >= 1000) {
      s_last_pos_report = now;
      report = true;
      report_pos = s_pos_base_ms + (unsigned)(((s_total_out - s_pos_origin_out) * 1000ULL) / 44100ULL);
    }
  }
  if (h) {
    if (reached) h->notifyAudioReachedPlayback();
    if (report) h->updatePositionMs(report_pos);
    if (ended) h->notifyAudioEnded();
  }
  return frames;
}

void sp_svc_set_pause(bool pause) {
  std::shared_ptr<cspot::SpircHandler> h = handler_snapshot();
  if (h) h->setPause(pause);
}

void sp_svc_next(void) {
  std::shared_ptr<cspot::SpircHandler> h = handler_snapshot();
  if (h) h->nextSong();
}

void sp_svc_prev(void) {
  std::shared_ptr<cspot::SpircHandler> h = handler_snapshot();
  if (h) h->previousSong();
}

void sp_svc_counters(unsigned* pcm_frames_in, unsigned* tracks_started) {
  if (pcm_frames_in) *pcm_frames_in = s_frames_in.load();
  if (tracks_started) *tracks_started = s_tracks.load();
}
