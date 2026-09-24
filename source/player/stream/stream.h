#pragma once
#include <ppu-types.h>

// Cooperative abort for stream_open()'s header wait (which can legitimately
// block for a long time while a transcode spins up).  The music player sets
// this before joining its stream thread so Back never hangs the UI; the
// video player leaves it alone.  Always cleared by stream_open on entry's
// caller — set it, join, then clear.
extern volatile bool g_stream_cancel;

// Called roughly twice a second while stream_open() is waiting for response
// headers.  Return false to abort the open.
//
// Without this the app simply stops: switching quality makes the server start
// a fresh transcode, which can take tens of seconds, and stream_open() would
// sit there for up to STREAM_HDR_DEADLINE_US with nothing drawn and no way
// out.  That is indistinguishable from a crash, and was reported as one.
typedef bool (*stream_wait_fn)(unsigned elapsed_ms);
void stream_set_wait_cb(stream_wait_fn cb);

// Open an HTTP connection to url and read the response headers.
// Returns a connected socket fd on success, -1 on failure.
int stream_open(const char *url);

// Why the last stream_open() failed, in words fit for the error screen
// ("Server returned HTTP 400", "Could not connect to 192.168.0.5:8096",
// "Server did not respond in 120s").  "Stream connection failed" on its own
// cannot tell a refused connection from a 400 caused by a bad MediaSourceId,
// and those need completely different fixes.  Valid until the next
// stream_open().
const char *stream_last_error(void);

// Read exactly 'size' bytes, transparently decoding chunked transfer encoding.
// Fully resumable across calls — all state is in static storage.
// Returns:  1 = success
//           0 = timed out (call again after checking buttons)
//          -1 = disconnect or terminal chunk
int stream_read(int sock, u8 *buf, int size);

// Cumulative receive stats since process start: bytes delivered by netRecv,
// microseconds spent blocked inside it, and how many times it was called.
// The heartbeat diffs these to report throughput and blocked-time share.
void stream_rx_stats(u64 *bytes, u64 *wait_us, u32 *calls);

// ---- Local files (offline playback, Stage 4) ------------------------------
// The player's other kind of source: a downloaded media.ts read from the HDD
// through the same buffer and the same stream_read().  The handle is tagged
// so it can never be mistaken for a socket.  offset must be a TS packet
// boundary (stream_local.h picks it).
int  stream_open_file(const char *path, u64 offset);
bool stream_is_file(int h);
// Close either kind: netClose for a socket, exactly as before.
void stream_close(int h);
// SO_RCVTIMEO for a socket, exactly as before; nothing for a file.
void stream_set_timeout(int h, u32 usec);
// The stream's 256 KB buffer as scratch -- only while no stream is being
// read (before an open, or mid-seek after the close).  Offline seeking
// probes the file with it instead of allocating.
u8  *stream_scratch(int *cap);
