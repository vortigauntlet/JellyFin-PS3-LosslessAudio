// The Spotify Connect receiver (sp_svc.cpp): pairing, the session, and the decoded audio, for the UI and the music engine.
//
// The console shows up in the Spotify app's device list; the phone picks what plays and the receiver decodes it.  Nothing is
// written to disk but the login credential cspot hands back, and the audio only ever lives in a small ring in memory.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum SpState {
  SP_OFF = 0,
  SP_PAIRING,     // waiting for the Spotify app to pick this console
  SP_SIGNING_IN,  // credential received or loaded, connecting
  SP_READY,       // signed in, nothing playing
  SP_PLAYING,
  SP_PAUSED,
  SP_ERROR        // see sp_svc_status_text(); the service retries on its own
};

typedef struct {
  char title[128], artist[128], album[128], image[240];
  unsigned duration_ms;
} SpTrack;

void sp_svc_start(const char* device_name);   // spawns the session thread; safe to call once
void sp_svc_stop(void);

SpState sp_svc_state(void);
const char* sp_svc_status_text(void);          // one line for the screen: what it is waiting for, or what failed
bool sp_svc_track(SpTrack* out);               // the current track, false when there is none
unsigned sp_svc_position_ms(void);
unsigned sp_svc_track_serial(void);            // changes whenever a new track starts

// Decoded audio: 44.1 kHz, interleaved stereo, signed 16-bit.  Returns the frames copied (0 when none is ready or paused).
// The caller reads in real time; this also tells Spotify when a track reaches the speakers and how far it has played.
size_t sp_svc_pcm_read(int16_t* dst, size_t max_frames);

// The pad's controls, sent through Spotify so the phone follows.
void sp_svc_set_pause(bool pause);
void sp_svc_next(void);
void sp_svc_prev(void);

// For the check screen and logs: how many audio keys were granted and refused so far (cspot's own log line, counted).
void sp_svc_counters(unsigned* pcm_frames_in, unsigned* tracks_started);
