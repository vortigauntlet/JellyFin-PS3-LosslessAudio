#include "sc_dec.h"
#include "fmp4.h"
#include "sc_aac.h"
#include "resample.h"

#include <stdlib.h>
#include <string.h>

#define MAX_FRAME_PAIRS 2048          // one AAC frame, 2048 with SBR
#define MAX_CONSECUTIVE_BAD 50        // frames in a row libfaad refuses before the stream is given up
#define SEEK_PREROLL_FRAMES 2         // frames decoded and thrown away before a seek target, for the overlap

struct ScDec {
    ScSegSource src;
    fmp4_init   in;
    ScAac      *aac;
    Resampler  *rs;
    int         rs_rate;

    uint8_t    *segbuf;
    size_t      seglen;
    fmp4_seg    seg;
    int         seg_idx;              // the loaded segment, -1 for none
    int         next_seg;             // the one to load when this runs out
    int         fi;                   // next frame in the loaded segment

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
    fmp4_seg_free(&d->seg);
    free(d->segbuf);
    d->segbuf = NULL;
    d->seglen = 0;
    d->seg_idx = -1;
}

ScDec *scdec_open(const ScSegSource *src) {
    if (!src || !src->fetch_init || !src->fetch_seg || src->nseg <= 0) return NULL;
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
    d->aac = scaac_open(d->in.asc, d->in.asc_len);
    if (!d->aac) { free(d); return NULL; }
    return d;
}

void scdec_close(ScDec *d) {
    if (!d) return;
    drop_segment(d);
    scaac_close(d->aac);
    resample_close(d->rs);
    free(d->out);
    free(d);
}

int scdec_rate(const ScDec *d) { return d->rate; }
int scdec_source_hz(const ScDec *d) { return d->in.rate; }
int scdec_bad_frames(const ScDec *d) { return d->bad; }

static bool load_segment(ScDec *d, int idx) {
    drop_segment(d);
    if (idx < 0 || idx >= d->src.nseg) return false;
    if (d->src.fetch_seg(d->src.ctx, idx, &d->segbuf, &d->seglen) < 0 || !d->segbuf) { d->segbuf = NULL; return false; }
    if (fmp4_parse_segment(d->segbuf, d->seglen, &d->in, &d->seg) <= 0) { drop_segment(d); return false; }
    d->seg_idx = idx;
    if (idx == 0) d->t0 = d->seg.base_time;
    d->next_seg = idx + 1;
    d->fi = 0;
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
    const int n = scaac_decode(d->aac, d->segbuf + s->off, (int)s->size, d->pcm, MAX_FRAME_PAIRS);
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
    d->fi = f0;
    scaac_reset(d->aac);
    if (d->rs) resample_reset(d->rs);
    d->drop_pairs = (target - before) * 48000 / d->in.timescale;
    return true;
}
