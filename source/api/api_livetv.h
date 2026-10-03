#pragma once
#include <stdint.h>
#include "livetv.h"

// Live TV, console side: the server's channel list and guide, and closing a
// live stream.  Blocking calls with a private reply buffer each, so a worker
// thread can run them while the UI thread uses responseBuffer.  See livetv.h
// for the parsing they rest on.

#define LIVETV_MAX_CHANNELS 200

// The channel list, sorted (favourites, then number, then name), with each
// channel's current programme.  Fetched in pages so a reply never reaches the
// response cap.  Returns the count, or -1 when the server could not be asked.
int jf_fetch_channels(JFChannel *out, int max);

// Programmes airing between `from_ticks` (their end must be after it) and
// `to_ticks` (their start must be before it) for up to `n` channels, in start
// order per request.  Asks in pages of a few channels (the URL has a length
// limit).  Returns the count stored.
int jf_fetch_programs(const char *const *channel_ids, int n,
                      uint64_t from_ticks, uint64_t to_ticks,
                      JFProgram *out, int max);

// One programme's details: its overview.  False when it cannot be fetched.
bool jf_fetch_program_overview(const char *program_id, char *out, int cap);

// POST /LiveStreams/Close: the server stops the live source.  True on 2xx.
bool jf_livestream_close(const char *live_stream_id);

// The console clock as ticks (UTC).
uint64_t jf_now_ticks(void);
