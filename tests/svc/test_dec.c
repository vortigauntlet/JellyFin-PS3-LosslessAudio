// Host test for the SoundCloud pull decoder: ffmpeg-made fragmented-MP4 segments -> libfaad -> 48 kHz stereo,
// checked against ffmpeg's own decode of the same bytes, with seeks, a failing fetch and corrupted segments.
//
//   test_dec <fixtures/sc_synth>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fmp4.h"
#include "sc_dec.h"
#include "resample.h"

static int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_MSG(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t *slurp(const char *dir, const char *name, size_t *len) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = (uint8_t *)malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
    fclose(f);
    *len = (size_t)n;
    return b;
}

// ffmpeg's reference decode is float32 little-endian; assemble the values byte by byte so a big-endian host reads them right.
static float *load_ref(const char *dir, size_t *bytes) {
    uint8_t *raw = slurp(dir, "ref.f32", bytes);
    const size_t n = *bytes / 4;
    float *f = (float *)malloc(sizeof(float) * (n ? n : 1));
    for (size_t i = 0; i < n; i++) {
        const uint32_t u = (uint32_t)raw[4 * i] | ((uint32_t)raw[4 * i + 1] << 8) | ((uint32_t)raw[4 * i + 2] << 16) | ((uint32_t)raw[4 * i + 3] << 24);
        memcpy(&f[i], &u, 4);
    }
    free(raw);
    return f;
}

typedef struct {
    const char *dir;
    int nseg;
    uint32_t ms[16];
    int fail_at;                // fetch_seg fails from this index on (-1 never)
    int corrupt;                // flip bytes in every segment (fuzz)
    unsigned seed;
    int fetches;
} FileSrc;

static uint32_t f_ms(void *c, int i) { return ((FileSrc *)c)->ms[i]; }
static int f_init(void *c, uint8_t **b, size_t *l) { *b = slurp(((FileSrc *)c)->dir, "init.mp4", l); return 0; }
static int f_seg(void *c, int i, uint8_t **b, size_t *l) {
    FileSrc *f = (FileSrc *)c;
    f->fetches++;
    if (f->fail_at >= 0 && i >= f->fail_at) return -1;
    char name[32];
    snprintf(name, sizeof(name), "data%03d.m4s", i + 1);
    *b = slurp(f->dir, name, l);
    if (f->corrupt) {
        for (int k = 0; k < f->corrupt; k++) {
            f->seed = f->seed * 1103515245u + 12345u;
            (*b)[(f->seed >> 8) % *l] ^= (uint8_t)(f->seed >> 20);
        }
    }
    return 0;
}

static void src_init(FileSrc *f, ScSegSource *s, const char *dir, int nseg) {
    memset(f, 0, sizeof(*f));
    f->dir = dir; f->nseg = nseg; f->fail_at = -1;
    size_t il;
    uint8_t *ib = slurp(dir, "init.mp4", &il);
    fmp4_init in;
    if (fmp4_parse_init(ib, il, &in) != 0) { printf("bad init\n"); exit(2); }
    free(ib);
    uint32_t total = 0;
    for (int i = 0; i < nseg; i++) {
        char name[32];
        snprintf(name, sizeof(name), "data%03d.m4s", i + 1);
        size_t sl;
        uint8_t *sb = slurp(dir, name, &sl);
        fmp4_seg sg;
        if (fmp4_parse_segment(sb, sl, &in, &sg) <= 0) { printf("bad segment\n"); exit(2); }
        f->ms[i] = (uint32_t)(sg.total_ticks * 1000 / in.timescale);
        total += f->ms[i];
        fmp4_seg_free(&sg);
        free(sb);
    }
    s->ctx = f; s->nseg = nseg; s->total_ms = total;
    s->seg_ms = f_ms; s->fetch_init = f_init; s->fetch_seg = f_seg;
}

static float *decode_all(ScDec *d, size_t *pairs) {
    size_t cap = 48000 * 40, n = 0;
    float *out = (float *)malloc(sizeof(float) * 2 * cap);
    for (;;) {
        const int r = scdec_decode(d, out + n * 2, 777);        // an odd size on purpose
        if (r <= 0) break;
        n += (size_t)r;
        if (n + 777 > cap) break;
    }
    *pairs = n;
    return out;
}

// The best lag L in [0, maxlag] such that ours[i + L] ~ ref[i], over a window; and the worst difference over
// everything that overlaps.  Mono references are compared on the left channel.
static int best_lag(const float *ours, size_t on, const float *ref, size_t rn, int ref_ch, int maxlag) {
    int best = 0;
    double bd = 1e30;
    const size_t w = 6000, start = 9000;
    for (int lag = 0; lag <= maxlag; lag++) {
        double d = 0;
        for (size_t i = start; i < start + w && i + (size_t)lag < on && i < rn; i++) {
            const double a = ours[(i + (size_t)lag) * 2], b = ref[i * (size_t)ref_ch];
            d += (a - b) * (a - b);
        }
        if (d < bd) { bd = d; best = lag; }
    }
    return best;
}

static double max_diff(const float *ours, size_t on, const float *ref, size_t rn, int ref_ch, int lag, size_t skip_tail) {
    double m = 0;
    for (size_t i = 0; i + (size_t)lag < on && i < rn - skip_tail; i++) {
        const double a = ours[(i + (size_t)lag) * 2], b = ref[i * (size_t)ref_ch];
        const double dd = fabs(a - b);
        if (dd > m) m = dd;
    }
    return m;
}

static void test_mono48(const char *synth) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/mono48", synth);
    FileSrc fs;
    ScSegSource ss;
    src_init(&fs, &ss, dir, 2);
    ScDec *d = scdec_open(&ss);
    CHECK(d != NULL);
    if (!d) return;
    size_t n;
    float *all = decode_all(d, &n);
    CHECK_MSG(scdec_rate(d) == 48000 && scdec_source_hz(d) == 48000, "rate %d/%d", scdec_rate(d), scdec_source_hz(d));
    CHECK(scdec_bad_frames(d) == 0);
    size_t rb;
    float *ref = load_ref(dir, &rb);
    const size_t rn = rb / 4;
    CHECK_MSG(n > 48000 * 11 && n < 48000 * 13, "decoded %zu pairs", n);
    const int lag = best_lag(all, n, ref, rn, 1, 4096);
    const double md = max_diff(all, n, ref, rn, 1, lag, 2048);
    printf("  mono48: %zu pairs, ffmpeg %zu, lag %d, max diff vs ffmpeg %.2e\n", n, rn, lag, md);
    CHECK_MSG(md < 2e-3, "max diff %.3e", md);
    // The right channel is the left one (mono).
    int dup = 1;
    for (size_t i = 0; i < n; i++) if (all[i * 2] != all[i * 2 + 1]) { dup = 0; break; }
    CHECK(dup);
    // A tone at amplitude 0.5: the decode is not silent and not clipped.
    float peak = 0;
    for (size_t i = 24000; i < 48000; i++) if (fabsf(all[i * 2]) > peak) peak = fabsf(all[i * 2]);
    CHECK_MSG(peak > 0.03f && peak < 0.1f, "peak %.3f", peak);

    // Seeks land where the sequential decode is (48 kHz needs no resampling, so samples compare one to one).
    static const uint32_t secs[] = { 0, 1, 3, 9, 10, 11 };
    for (size_t k = 0; k < sizeof(secs) / sizeof(secs[0]); k++) {
        CHECK(scdec_seek(d, secs[k]));
        float buf[4800 * 2];
        int got = 0;
        while (got < 4800) {
            const int r = scdec_decode(d, buf + got * 2, 4800 - got);
            if (r <= 0) break;
            got += r;
        }
        CHECK_MSG(got == 4800, "seek %us: %d pairs", secs[k], got);
        const size_t at = (size_t)secs[k] * 48000;
        double m = 0;
        for (int i = 0; i < got && at + (size_t)i < n; i++) m = fmax(m, fabs((double)buf[i * 2] - all[(at + (size_t)i) * 2]));
        // Not always zero: libfaad's noise substitution (PNS) draws from a random sequence that runs on through the
        // stream, so a seek into the middle re-rolls the noise of the bands that use it.  Position, not noise, is what
        // this checks: the error must stay far below the 0.0625 tone.
        CHECK_MSG(m < 5e-4, "seek %us differs from the sequential decode by %.3e", secs[k], m);
    }
    free(ref); free(all);
    scdec_close(d);
}

static void test_stereo44(const char *synth) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/stereo44", synth);
    FileSrc fs;
    ScSegSource ss;
    src_init(&fs, &ss, dir, 3);
    ScDec *d = scdec_open(&ss);
    CHECK(d != NULL);
    if (!d) return;
    size_t n;
    float *all = decode_all(d, &n);
    CHECK(scdec_rate(d) == 44100);
    CHECK_MSG(n > 48000 * 24 && n < 48000 * 26, "decoded %zu pairs", n);

    // Reference: ffmpeg's decode at 44.1 kHz, converted by the same resampler.
    size_t rb;
    float *ref44 = load_ref(dir, &rb);
    const size_t rn44 = rb / 8;
    Resampler *rs = resample_open(44100, 2);
    const int cap = resample_max_out(rs, (int)rn44);
    float *ref = (float *)malloc(sizeof(float) * 2 * (size_t)cap);
    const int rn = resample_process(rs, ref44, (int)rn44, ref, cap);
    resample_close(rs);
    const int lag = best_lag(all, n, ref, (size_t)rn, 2, 4096);
    const double md = max_diff(all, n, ref, (size_t)rn, 2, lag, 4096);
    printf("  stereo44: %zu pairs at 48k, ffmpeg %d, lag %d, max diff vs ffmpeg %.2e\n", n, rn, lag, md);
    CHECK_MSG(md < 3e-3, "max diff %.3e", md);
    // Left and right carry different tones.
    int differ = 0;
    for (size_t i = 24000; i < 24100; i++) if (fabsf(all[i * 2] - all[i * 2 + 1]) > 0.01f) differ++;
    CHECK(differ > 20);

    // Seeks: same audio as the sequential decode, to within the resampler's phase (a fraction of a sample).
    static const uint32_t secs[] = { 0, 2, 9, 10, 11, 19, 21, 24 };
    for (size_t k = 0; k < sizeof(secs) / sizeof(secs[0]); k++) {
        CHECK(scdec_seek(d, secs[k]));
        float buf[4800 * 2];
        int got = 0;
        while (got < 4800) {
            const int r = scdec_decode(d, buf + got * 2, 4800 - got);
            if (r <= 0) break;
            got += r;
        }
        const size_t at = (size_t)secs[k] * 48000;
        if (at + 4800 + 4 > n) { CHECK(got > 0 || secs[k] >= 24); continue; }
        CHECK_MSG(got == 4800, "seek %us: %d pairs", secs[k], got);
        double best = 1e9;
        for (int sh = -2; sh <= 2; sh++) {
            double e = 0, en = 0;
            for (int i = 8; i < got - 8; i++) {
                const double a = buf[i * 2], b = all[(at + (size_t)(i + sh)) * 2];
                e += (a - b) * (a - b);
                en += b * b;
            }
            if (en > 0 && sqrt(e / en) < best) best = sqrt(e / en);
        }
        CHECK_MSG(best < 0.05, "seek %us: relative error %.3f", secs[k], best);
    }
    free(ref); free(ref44); free(all);
    scdec_close(d);
}

static void test_failures(const char *synth) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/stereo44", synth);
    // A fetch that fails for good partway through: everything before it is delivered, then an error.
    {
        FileSrc fs;
        ScSegSource ss;
        src_init(&fs, &ss, dir, 3);
        fs.fail_at = 1;
        ScDec *d = scdec_open(&ss);
        CHECK(d != NULL);
        size_t n;
        float *all = decode_all(d, &n);
        CHECK_MSG(n > 48000 * 9 && n < 48000 * 11, "decoded %zu pairs before the failed fetch", n);
        float b[64];
        CHECK(scdec_decode(d, b, 32) < 0);
        CHECK(!scdec_seek(d, 15));
        free(all);
        scdec_close(d);
    }
    // Corrupted segments never crash and never hand out NaN or out-of-range samples.
    for (unsigned seed = 1; seed <= 12; seed++) {
        FileSrc fs;
        ScSegSource ss;
        src_init(&fs, &ss, dir, 3);
        fs.corrupt = (int)seed * 5;
        fs.seed = seed * 7919u;
        ScDec *d = scdec_open(&ss);
        if (!d) continue;
        size_t n = 0;
        float buf[1024 * 2];
        int r;
        while ((r = scdec_decode(d, buf, 1024)) > 0 && n < 48000 * 30) {
            n += (size_t)r;
            for (int i = 0; i < r * 2; i++) if (!(fabsf(buf[i]) < 64.f)) { CHECK_MSG(0, "seed %u: sample %f", seed, buf[i]); break; }
        }
        scdec_close(d);
    }
    // Bad sources.
    ScSegSource none;
    memset(&none, 0, sizeof(none));
    CHECK(scdec_open(&none) == NULL);
    CHECK(scdec_open(NULL) == NULL);
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: test_dec <sc_synth-dir>\n"); return 2; }
    test_mono48(argv[1]);
    test_stereo44(argv[1]);
    test_failures(argv[1]);
    printf("test_dec: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
