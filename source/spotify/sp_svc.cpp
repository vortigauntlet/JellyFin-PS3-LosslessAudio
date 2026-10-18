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
#include "sp_zeroconf.h"
#include "sx_plat.h"

namespace {

const size_t kRingFrames = 44100 * 2;          // two seconds

std::atomic<bool> s_run{false};
std::atomic<bool> s_thread_done{true};        // the session thread has returned (or never started)
std::atomic<int> s_state{SP_OFF};
std::atomic<unsigned> s_serial{0}, s_frames_in{0}, s_tracks{0};
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
std::shared_ptr<cspot::Context> s_ctx;         // the live session's context (the quality is read from it for each track)
std::atomic<int> s_quality{160};

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

void ring_clear_locked() {
  s_rd = s_wr = s_fill = 0;
  s_bounds.clear();
  s_total_in = s_total_out = 0;
  s_have_hash = false;
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
    case ET::FLUSH:
    case ET::DISC: {
      std::lock_guard<std::mutex> g(s_mtx);
      ring_clear_locked();
      break;
    }
    case ET::SEEK: {
      std::lock_guard<std::mutex> g(s_mtx);
      ring_clear_locked();
      s_pos_base_ms = (unsigned)std::get<int>(ev->data);
      s_pos_origin_out = 0;
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
      break;
    }
    case ET::TRACK_INFO: {
      const cspot::TrackInfo& ti = std::get<cspot::TrackInfo>(ev->data);
      std::lock_guard<std::mutex> g(s_mtx);
      snprintf(s_track.title, sizeof s_track.title, "%s", ti.name.c_str());
      snprintf(s_track.artist, sizeof s_track.artist, "%s", ti.artist.c_str());
      snprintf(s_track.album, sizeof s_track.album, "%s", ti.album.c_str());
      snprintf(s_track.image, sizeof s_track.image, "%s", ti.imageUrl.c_str());
      s_track.duration_ms = ti.duration;
      s_have_track = true;
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

  s_handler = std::make_shared<cspot::SpircHandler>(ctx);
  s_handler->getTrackPlayer()->setDataCallback(on_pcm);
  s_handler->setEventHandler(on_event);
  ctx->session->startTask();
  s_state = SP_READY;
  set_status("Ready: pick this console in the Spotify app to play");
  { char b[64]; snprintf(b, sizeof b, "spotify: signed in, free=%uKB", meminfo_avail_kb()); plog(b); }

  try {
    while (s_run) ctx->session->handlePacket();
  } catch (std::exception& e) {
    plog("spotify: the session ended");
  }
  s_handler->disconnect();
  ctx->session->disconnect();
  s_handler.reset();
  { std::lock_guard<std::mutex> g(s_mtx); s_ctx.reset(); }
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
  s_run = true;
  s_thread_done = false;
  sys_ppu_thread_t t;
  if (sysThreadCreate(&t, session_thread, nullptr, 1001, 256 * 1024, 0, (char*)"sp_session") != 0) {
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
  for (int i = 0; i < 40 && !s_thread_done; i++) usleep(100000);
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

unsigned sp_svc_position_ms(void) {
  std::lock_guard<std::mutex> g(s_mtx);
  return s_pos_base_ms + (unsigned)(((s_total_out - s_pos_origin_out) * 1000ULL) / 44100ULL);
}

size_t sp_svc_pcm_read(int16_t* dst, size_t max_frames) {
  std::shared_ptr<cspot::SpircHandler> h = s_handler;
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
  std::shared_ptr<cspot::SpircHandler> h = s_handler;
  if (h) h->setPause(pause);
}

void sp_svc_next(void) {
  std::shared_ptr<cspot::SpircHandler> h = s_handler;
  if (h) h->nextSong();
}

void sp_svc_prev(void) {
  std::shared_ptr<cspot::SpircHandler> h = s_handler;
  if (h) h->previousSong();
}

void sp_svc_counters(unsigned* pcm_frames_in, unsigned* tracks_started) {
  if (pcm_frames_in) *pcm_frames_in = s_frames_in.load();
  if (tracks_started) *tracks_started = s_tracks.load();
}
