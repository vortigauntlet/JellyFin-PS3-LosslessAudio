// Pull decoder for a SoundCloud stream: segments of fragmented MP4 in, 48 kHz stereo float pairs out.
// A segment is read as a stream through a 20 KB window; only the head (styp, sidx, moof) and one frame are held.
//
// It has the shape of local/local_audio's la_open / la_decode / la_seek, so the music engine drives a
// SoundCloud track the way it drives a file.  Where the bytes come from is a ScSegSource: the network
// source (sc_source) on the console, a directory of fixtures in the host test.
//
// One decoder is used from one thread.  Fetches block, so the engine calls it from its stream thread.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ScSegSource {
    void *ctx;
    int      nseg;                      // number of media segments
    uint32_t total_ms;                  // from the playlist
    // Segment i's duration (the playlist's EXTINF), used to find a seek target.
    uint32_t (*seg_ms)(void *ctx, int i);
    // The init segment as a heap buffer the decoder frees (it is under a kilobyte).  0 on success, < 0 on failure
    // (after the source's own retries).
    int (*fetch_init)(void *ctx, uint8_t **buf, size_t *len);
    // Media segment i as a stream: the console has well under 200 KB free while it plays and a segment is 200 KB, so
    // the decoder reads it a few kilobytes at a time and never holds more than one frame.  seg_open starts it (0 on
    // success), seg_read gives > 0 bytes, 0 at its end, < 0 when it failed for good (the source has already retried,
    // resuming from where it was), seg_close ends it, early or not.
    int  (*seg_open)(void *ctx, int i, void **h);
    int  (*seg_read)(void *ctx, void *h, uint8_t *buf, int n);
    void (*seg_close)(void *ctx, void *h);
} ScSegSource;

typedef struct ScDec ScDec;

// Fetches and parses the init segment and opens the AAC decoder.  The source must outlive the decoder.
ScDec *scdec_open(const ScSegSource *src);
void   scdec_close(ScDec *d);

// Up to max_pairs frames of 48 kHz stereo, interleaved L R.  Returns the pairs made (> 0), 0 at the end of
// the stream, or < 0 when a fetch failed for good (everything decoded before it has been handed out).
int    scdec_decode(ScDec *d, float *lr, int max_pairs);

// Moves to `secs` into the track (clamped).  False when the segment there could not be fetched.
bool   scdec_seek(ScDec *d, uint32_t secs);

// Stream facts for the log and the Now Playing line.
int    scdec_rate(const ScDec *d);         // the AAC output rate before conversion (0 until the first frame)
int    scdec_source_hz(const ScDec *d);    // the rate the init segment declares
int    scdec_bad_frames(const ScDec *d);   // frames libfaad refused so far

#ifdef __cplusplus
}
#endif
