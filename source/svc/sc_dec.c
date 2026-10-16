#include "sc_dec.h"
#include "fmp4.h"
#include "sc_aac.h"
#include "resample.h"

#include <stdlib.h>
#include <string.h>

#define MAX_FRAME_PAIRS 2048          // one AAC frame, 2048 with SBR
#define MAX_CONSECUTIVE_BAD 50        // frames in a row libfaad refuses before the stream is given up
#define SEEK_PREROLL_FRAMES 2         // frames decoded and thrown away before a seek target, for the overlap
#define WINDOW 20480                  // holds the head of a segment (a moof of 430 samples is about 4 KB) and one frame
#define MAX_FRAME_BYTES 8192          // an AAC frame is at most 6144 bytes a channel

struct ScDec {
    ScSegSource src;
    fmp4_init   in;
    ScAac      *aac;
    Resampler  *rs;
    int         rs_rate;

    // the segment being read
    void       *sh;                   // the source's handle, NULL when none is open
    uint8_t    *win;                  // the bytes read so far that are still wanted
    size_t      win_len;
    uint64_t    win_off;              // the segment offset of win[0]
    fmp4_seg    seg;
    int         seg_idx;              // the open segment, -1 for none
    int         next_seg;             // the one to open when this runs out
    int         fi;                   // next frame in the segment

    float       pcm[MAX_FRAME_PAIRS * 2];
    float      *out;                  // resampler output for one frame
    int         out_cap;
    int         out_n, out_pos;       // staged pairs, and how many of them were handed out
    uint64_t    drop_pairs;           // pairs still to discard (the seek target lies inside the first frames)

    bool        eof, failed;
    int         bad, bad_run;
    int         rate;
    uint64_t    t0;                   // tfdt of the first segment: the timeline's origin in ticks
};

static void drop_segment(ScDec *d) {
    if (d->sh) d->src.seg_close(d->src.ctx, d->sh);
    d->sh = NULL;
    fmp4_seg_free(&d->seg);
    d->win_len = 0;
    d->win_off = 0;
    d->seg_idx = -1;
}

ScDec *scdec_open(const ScSegSource *src) {
    if (!src || !src->fetch_init || !src->seg_open || !src->seg_read || !src->seg_close || src->nseg <= 0) return NULL;
    ScDec *d = (ScDec *)calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->src = *src;
    d->seg_idx = -1;
    uint8_t *ib = NULL;
    size_t il = 0;
    if (src->fetch_init(src->ctx, &ib, &il) < 0 || !ib) { free(d); return NULL; }
    const int rc = fmp4_parse_init(ib, il, &d->in);
    free(ib);
    if (rc != 0) { free(d); return NULL; }
    d->win = (uint8_t *)malloc(WINDOW);
    d->aac = scaac_open(d->in.asc, d->in.asc_len);
    if (!d->win || !d->aac) { scaac_close(d->aac); free(d->win); free(d); return NULL; }
    return d;
}

void scdec_close(ScDec *d) {
    if (!d) return;
    drop_segment(d);
    scaac_close(d->aac);
    resample_close(d->rs);
    free(d->out);
    free(d->win);
    free(d);
}

int scdec_rate(const ScDec *d) { return d->rate; }
int scdec_source_hz(const ScDec *d) { return d->in.rate; }
int scdec_bad_frames(const ScDec *d) { return d->bad; }

// Opens segment idx and reads its head (styp, sidx, moof) into the window.
static bool load_segment(ScDec *d, int idx) {
    drop_segment(d);
    if (idx < 0 || idx >= d->src.nseg) return false;
    if (d->src.seg_open(d->src.ctx, idx, &d->sh) < 0 || !d->sh) { d->sh = NULL; return false; }
    for (;;) {
        size_t need = 0;
        const int rc = fmp4_parse_head(d->win, d->win_len, &d->in, &d->seg, &need);
        if (rc > 0) break;
        if (rc != FMP4_E_SHORT || need > WINDOW || d->win_len >= WINDOW) { drop_segment(d); return false; }
        const int r = d->src.seg_read(d->src.ctx, d->sh, d->win + d->win_len, (int)(WINDOW - d->win_len));
        if (r <= 0) { drop_segment(d); return false; }
        d->win_len += (size_t)r;
    }
    d->seg_idx = idx;
    if (idx == 0) d->t0 = d->seg.base_time;
    d->next_seg = idx + 1;
    d->fi = 0;
    return true;
}

// Brings the bytes [off, off+size) to the front of the window; the bytes before off are read past and dropped.
static bool window_at(ScDec *d, uint64_t off, size_t size) {
    if (off < d->win_off || size == 0 || size > MAX_FRAME_BYTES) return false;
    while (d->win_off < off) {
        const uint64_t want = off - d->win_off;
        if (d->win_len > 0) {
            const size_t drop = want < d->win_len ? (size_t)want : d->win_len;
            memmove(d->win, d->win + drop, d->win_len - drop);
            d->win_len -= drop;
            d->win_off += drop;
        } else {                                                    // nothing held: read and discard
            const int r = d->src.seg_read(d->src.ctx, d->sh, d->win, want < WINDOW ? (int)want : WINDOW);
            if (r <= 0) return false;
            d->win_off += (uint64_t)r;
        }
    }
    while (d->win_len < size) {
        const int r = d->src.seg_read(d->src.ctx, d->sh, d->win + d->win_len, (int)(WINDOW - d->win_len));
        if (r <= 0) return false;
        d->win_len += (size_t)r;
    }
    return true;
}

// Decodes the next frame into the staging buffer.  False at the end of the stream or on a failure (eof and
// failed say which); true otherwise, even when the frame made no output.
static bool next_frame(ScDec *d) {
    d->out_n = d->out_pos = 0;
    if (d->seg_idx < 0 || d->fi >= d->seg.n) {
        if (d->next_seg >= d->src.nseg) { d->eof = true; return false; }
        if (!load_segment(d, d->next_seg)) { d->eof = d->failed = true; return false; }
    }
    const fmp4_sample *s = &d->seg.s[d->fi++];
    if (!window_at(d, s->off, s->size)) { d->eof = d->failed = true; return false; }
    const int n = scaac_decode(d->aac, d->win, (int)s->size, d->pcm, MAX_FRAME_PAIRS);
    if (n < 0) {
        d->bad++;
        if (++d->bad_run >= MAX_CONSECUTIVE_BAD) { d->eof = d->failed = true; return false; }
        return true;
    }
    d->bad_run = 0;
    if (n == 0) return true;

    const int rate = scaac_rate(d->aac);
    d->rate = rate;
    if (!d->rs || d->rs_rate != rate) {
        resample_close(d->rs);
        free(d->out);
        d->rs = NULL; d->out = NULL;
        if (!resample_rate_supported(rate)) { d->eof = d->failed = true; return false; }
        d->rs = resample_open(rate, 2);
        if (!d->rs) { d->eof = d->failed = true; return false; }
        d->rs_rate = rate;
        d->out_cap = resample_max_out(d->rs, MAX_FRAME_PAIRS);
        d->out = (float *)malloc(sizeof(float) * 2 * (size_t)d->out_cap);
        if (!d->out) { d->eof = d->failed = true; return false; }
    }
    int m = resample_process(d->rs, d->pcm, n, d->out, d->out_cap);
    int skip = 0;
    if (d->drop_pairs) {
        skip = d->drop_pairs < (uint64_t)m ? (int)d->drop_pairs : m;
        d->drop_pairs -= (uint64_t)skip;
    }
    d->out_pos = skip;
    d->out_n = m;
    return true;
}

int scdec_decode(ScDec *d, float *lr, int max_pairs) {
    int done = 0;
    while (done < max_pairs) {
        if (d->out_pos < d->out_n) {
            int k = d->out_n - d->out_pos;
            if (k > max_pairs - done) k = max_pairs - done;
            memcpy(lr + (size_t)done * 2, d->out + (size_t)d->out_pos * 2, sizeof(float) * 2 * (size_t)k);
            d->out_pos += k;
            done += k;
            continue;
        }
        if (d->eof || !next_frame(d)) break;
    }
    if (done == 0) return d->failed ? -1 : 0;
    return done;
}

bool scdec_seek(ScDec *d, uint32_t secs) {
    uint64_t ms = (uint64_t)secs * 1000;
    if (d->src.total_ms && ms >= d->src.total_ms) ms = d->src.total_ms > 1000 ? d->src.total_ms - 1000 : 0;
    int idx = 0;
    uint64_t acc = 0;
    for (; idx < d->src.nseg - 1; idx++) {
        const uint32_t sm = d->src.seg_ms(d->src.ctx, idx);
        if (ms < acc + sm) break;
        acc += sm;
    }
    d->eof = d->failed = false;
    d->bad_run = 0;
    d->out_n = d->out_pos = 0;
    d->drop_pairs = 0;
    if (!load_segment(d, idx)) { d->eof = d->failed = true; return false; }

    // The segment's own decode time places the target exactly; the playlist's rounded durations only pick the segment.
    uint64_t target = (ms - acc) * d->in.timescale / 1000;
    {
        const uint64_t abs_t = ms * d->in.timescale / 1000 + d->t0;
        if (abs_t >= d->seg.base_time && abs_t - d->seg.base_time < d->seg.total_ticks) target = abs_t - d->seg.base_time;
    }
    uint64_t cum = 0, before = 0;
    int f = 0;
    for (; f < d->seg.n - 1; f++) {
        if (target < cum + d->seg.s[f].dur) break;
        cum += d->seg.s[f].dur;
    }
    int f0 = f - SEEK_PREROLL_FRAMES;
    if (f0 < 0) f0 = 0;
    for (int i = 0; i < f0; i++) before += d->seg.s[i].dur;
    d->fi = f0;                                   // the first read skips (reads and drops) the bytes before this frame
    scaac_reset(d->aac);
    if (d->rs) resample_reset(d->rs);
    d->drop_pairs = (target - before) * 48000 / d->in.timescale;
    return true;
}
