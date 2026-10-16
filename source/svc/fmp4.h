// Fragmented-MP4 reader for one audio track, as SoundCloud serves it over HLS: an init segment
// (ftyp + moov) and media segments (styp, sidx, moof, mdat).
//
// Each segment is handed over whole, so this reads from memory and is a pure function of its input:
// no I/O, no allocation beyond the sample table.  It reads what the format guarantees (tfhd/trex
// defaults, tfdt, trun with every optional field) rather than the one layout SoundCloud happens to use.
// The approach follows the fragment handling in the Yo! Player PSL1GHT port's demux-mp4.c (Apache-2.0,
// github.com/openlewa/yo-player-psl1ght); that code reads through a random-access video source and
// cannot be lifted out, so this is a rewrite for in-memory segments.
//
// Pure C with no platform includes: the host suite runs this file as it is.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FMP4_MAX_ASC 32
#define FMP4_MAX_SAMPLES 4096          // per segment (a 10 s segment of 1024-sample frames has about 470)

typedef struct {
    uint32_t timescale;                // mdhd: ticks per second of the audio track
    int      channels;                 // mp4a sample entry
    int      rate;                     // mp4a sample entry (an SBR stream reports its output rate here)
    int      aot;                      // AudioObjectType from the ASC: 2 = AAC-LC, 5 = SBR, 29 = PS; -1 unknown
    int      asc_len;
    uint8_t  asc[FMP4_MAX_ASC];        // AudioSpecificConfig, for libfaad's NeAACDecInit2
    uint32_t trex_duration, trex_size, trex_flags;
} fmp4_init;

// 0 on success, or a negative code.  Requires a 'soun' track with an mp4a entry and an esds.
int fmp4_parse_init(const uint8_t *buf, size_t len, fmp4_init *out);

typedef struct {
    uint32_t off;                      // byte offset of the sample inside the segment buffer
    uint32_t size;
    uint32_t dur;                      // ticks of fmp4_init.timescale
} fmp4_sample;

typedef struct {
    fmp4_sample *s;                    // heap, fmp4_seg_free()
    int          n;
    uint64_t     base_time;            // tfdt: decode time of the first sample, in ticks
    uint64_t     total_ticks;          // sum of the durations
} fmp4_seg;

// Number of samples, or a negative code.  Every sample lies inside buf.
int  fmp4_parse_segment(const uint8_t *buf, size_t len, const fmp4_init *init, fmp4_seg *out);
void fmp4_seg_free(fmp4_seg *s);

enum {
    FMP4_E_SHORT   = -1,               // truncated or malformed box
    FMP4_E_NOMOOV  = -2,
    FMP4_E_NOTRACK = -3,               // no audio track with an mp4a entry
    FMP4_E_NOESDS  = -4,
    FMP4_E_NOMOOF  = -5,
    FMP4_E_RANGE   = -6,               // a sample points outside the segment
    FMP4_E_MEM     = -7,
    FMP4_E_NOSAMPLES = -8
};

#ifdef __cplusplus
}
#endif
