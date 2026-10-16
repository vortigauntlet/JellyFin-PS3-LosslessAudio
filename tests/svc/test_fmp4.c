// Host tests for the fragmented-MP4 reader: SoundCloud's real init segment, ffmpeg-made segments cross-checked
// against ffmpeg's own demuxer (frames.adts), and truncation/corruption fuzzing.
//
//   test_fmp4 <fixtures/sc> <fixtures/sc_synth>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fmp4.h"

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

// Frames of an ADTS stream, with the headers stripped, into one buffer plus a size list.
static int adts_frames(const uint8_t *a, size_t n, uint8_t *out, int *sizes, int max) {
    size_t p = 0, o = 0;
    int k = 0;
    while (p + 7 <= n && k < max) {
        if (a[p] != 0xFF || (a[p + 1] & 0xF0) != 0xF0) return -1;
        const size_t fl = ((size_t)(a[p + 3] & 3) << 11) | ((size_t)a[p + 4] << 3) | (a[p + 5] >> 5);
        const size_t hdr = (a[p + 1] & 1) ? 7 : 9;
        if (fl < hdr || p + fl > n) return -1;
        memcpy(out + o, a + p + hdr, fl - hdr);
        sizes[k++] = (int)(fl - hdr);
        o += fl - hdr;
        p += fl;
    }
    return k;
}

static void test_real_init(const char *dir) {
    size_t len;
    uint8_t *b = slurp(dir, "init.mp4", &len);
    fmp4_init in;
    CHECK(fmp4_parse_init(b, len, &in) == 0);
    CHECK_MSG(in.timescale == 44100 || in.timescale == 48000, "timescale %u", in.timescale);
    CHECK(in.channels >= 1 && in.channels <= 2);
    CHECK(in.rate == 44100 || in.rate == 48000);
    CHECK_MSG(in.aot == 2, "aot %d", in.aot);
    CHECK(in.asc_len >= 2 && in.asc_len <= 5);
    printf("  soundcloud init: timescale %u, %d ch, %d Hz, aot %d, asc %d bytes, trex dur %u\n", in.timescale, in.channels, in.rate, in.aot, in.asc_len, in.trex_duration);
    // Truncated or damaged: never a crash, never a success with garbage.
    for (size_t cut = 0; cut < len; cut += 7) {
        fmp4_init t;
        const int rc = fmp4_parse_init(b, cut, &t);
        CHECK(rc != 0 || t.asc_len > 0);
    }
    free(b);
}

static void test_dir(const char *dir, int want_ch, int want_rate, int nseg) {
    size_t len;
    uint8_t *ib = slurp(dir, "init.mp4", &len);
    fmp4_init in;
    CHECK(fmp4_parse_init(ib, len, &in) == 0);
    CHECK_MSG(in.channels == want_ch && in.rate == want_rate, "%s: %d ch %d Hz", dir, in.channels, in.rate);
    CHECK(in.aot == 2);

    size_t alen;
    uint8_t *adts = slurp(dir, "frames.adts", &alen);
    uint8_t *ref = (uint8_t *)malloc(alen);
    int *refsz = (int *)malloc(sizeof(int) * 4096);
    const int nref = adts_frames(adts, alen, ref, refsz, 4096);
    CHECK_MSG(nref > 100, "%s: ffmpeg frames %d", dir, nref);

    int k = 0;
    size_t refoff = 0;
    uint64_t ticks = 0;
    for (int s = 1; s <= nseg; s++) {
        char name[32];
        snprintf(name, sizeof(name), "data%03d.m4s", s);
        size_t sl;
        uint8_t *sb = slurp(dir, name, &sl);
        fmp4_seg seg;
        const int n = fmp4_parse_segment(sb, sl, &in, &seg);
        CHECK_MSG(n > 0, "%s %s: parse rc %d", dir, name, n);
        if (n > 0) {
            CHECK_MSG(seg.base_time == ticks, "%s %s: base %llu want %llu", dir, name, (unsigned long long)seg.base_time, (unsigned long long)ticks);
            for (int i = 0; i < n; i++, k++) {
                CHECK_MSG(k < nref && (int)seg.s[i].size == refsz[k], "%s frame %d size %u", dir, k, seg.s[i].size);
                if (k < nref && (int)seg.s[i].size == refsz[k]) {
                    CHECK_MSG(memcmp(sb + seg.s[i].off, ref + refoff, seg.s[i].size) == 0, "%s frame %d differs from ffmpeg's", dir, k);
                    refoff += (size_t)refsz[k];
                }
                CHECK_MSG(seg.s[i].dur > 0 && seg.s[i].dur <= 1024, "%s frame %d dur %u", dir, k, seg.s[i].dur);
            }
            ticks += seg.total_ticks;
        }
        // Robustness: every truncation and a spread of single-byte corruptions.
        for (size_t cut = 0; cut < sl; cut += (sl / 61) + 1) {
            fmp4_seg t;
            const int r = fmp4_parse_segment(sb, cut, &in, &t);
            if (r > 0) { for (int i = 0; i < r; i++) CHECK(t.s[i].off + t.s[i].size <= cut); fmp4_seg_free(&t); }
        }
        for (size_t pos = 0; pos < 400 && pos < sl; pos++) {
            const uint8_t keep = sb[pos];
            for (int v = 0; v < 3; v++) {
                sb[pos] = (uint8_t)(v == 0 ? 0xFF : v == 1 ? 0x00 : keep ^ 0x5A);
                fmp4_seg t;
                const int r = fmp4_parse_segment(sb, sl, &in, &t);
                if (r > 0) { for (int i = 0; i < r; i++) CHECK(t.s[i].off + t.s[i].size <= sl); fmp4_seg_free(&t); }
            }
            sb[pos] = keep;
        }
        // The head of a segment that is still arriving: asks for more until the moof is whole, then agrees with the whole parse.
        {
            fmp4_seg whole, head;
            const int nw = fmp4_parse_segment(sb, sl, &in, &whole);
            size_t first_ok = 0;
            for (size_t avail = 0; avail <= sl && avail < 20000; avail++) {
                size_t need = 0;
                const int r = fmp4_parse_head(sb, avail, &in, &head, &need);
                if (r > 0) { first_ok = avail; fmp4_seg_free(&head); break; }
                CHECK_MSG(r == FMP4_E_SHORT && need > avail, "%s avail %zu: rc %d need %zu", name, avail, r, need);
                if (r == FMP4_E_SHORT) CHECK(need <= first_ok + 1 || first_ok == 0);
            }
            CHECK_MSG(first_ok > 100 && first_ok < 20000, "%s: head complete at %zu", name, first_ok);
            if (first_ok) {
                size_t need = 0;
                CHECK(fmp4_parse_head(sb, first_ok, &in, &head, &need) == nw);
                CHECK(head.n == whole.n && head.base_time == whole.base_time && head.total_ticks == whole.total_ticks);
                for (int i = 0; i < head.n && i < whole.n; i++) CHECK(head.s[i].off == whole.s[i].off && head.s[i].size == whole.s[i].size);
                fmp4_seg_free(&head);
                // the same with only the head in hand (the media data has not arrived)
                CHECK(fmp4_parse_head(sb, first_ok + 3, &in, &head, &need) == nw);
                fmp4_seg_free(&head);
            }
            if (nw > 0) fmp4_seg_free(&whole);
        }
        if (n > 0) fmp4_seg_free(&seg);
        free(sb);
    }
    CHECK_MSG(k == nref, "%s: %d frames from the segments, ffmpeg found %d", dir, k, nref);
    printf("  %s: %d frames, %.2f s\n", dir, k, (double)ticks / in.timescale);
    free(refsz); free(ref); free(adts); free(ib);
}

int main(int argc, char **argv) {
    if (argc < 3) { printf("usage: test_fmp4 <sc-fixtures> <synth-fixtures>\n"); return 2; }
    char d1[512], d2[512];
    test_real_init(argv[1]);
    snprintf(d1, sizeof(d1), "%s/stereo44", argv[2]);
    snprintf(d2, sizeof(d2), "%s/mono48", argv[2]);
    test_dir(d1, 2, 44100, 3);
    test_dir(d2, 1, 48000, 2);

    // A hand-built segment with 64-bit boxes, default-base-is-moof and trex defaults (no per-sample fields).
    fmp4_init in;
    memset(&in, 0, sizeof(in));
    in.timescale = 44100; in.trex_duration = 1024; in.trex_size = 4;
    uint8_t seg[256];
    memset(seg, 0, sizeof(seg));
    size_t o = 0;
    #define P32(v) do { seg[o++] = (uint8_t)((v) >> 24); seg[o++] = (uint8_t)((v) >> 16); seg[o++] = (uint8_t)((v) >> 8); seg[o++] = (uint8_t)(v); } while (0)
    #define PTAG(s) do { memcpy(seg + o, s, 4); o += 4; } while (0)
    // moof(8+mfhd16+traf(8+tfhd16+tfdt16+trun20)) = 8+16+8+16+16+20 = 84
    P32(84); PTAG("moof");
    P32(16); PTAG("mfhd"); P32(0); P32(1);
    P32(60); PTAG("traf");
    P32(16); PTAG("tfhd"); P32(0x020000); P32(1);              // default-base-is-moof, no defaults
    P32(16); PTAG("tfdt"); P32(0); P32(88200);
    P32(20); PTAG("trun"); P32(0x000001); P32(3); P32(84 + 8);  // data offset only; three samples from trex defaults
    P32(20); PTAG("mdat");
    for (int i = 0; i < 12; i++) seg[o++] = (uint8_t)(i + 1);
    fmp4_seg sg;
    const int n = fmp4_parse_segment(seg, o, &in, &sg);
    CHECK_MSG(n == 3, "trex-default segment n=%d", n);
    if (n == 3) {
        CHECK(sg.base_time == 88200);
        CHECK(sg.s[0].off == 92 && sg.s[0].size == 4 && sg.s[2].off == 100 && sg.s[0].dur == 1024);
        CHECK(sg.total_ticks == 3072);
        fmp4_seg_free(&sg);
    }
    CHECK(fmp4_parse_segment(seg, o - 1, &in, &sg) == FMP4_E_RANGE);        // the last sample runs past the buffer
    CHECK(fmp4_parse_segment(seg, 5, &in, &sg) == FMP4_E_NOMOOF);
    CHECK(fmp4_parse_segment(seg, 0, &in, &sg) == FMP4_E_NOMOOF);

    printf("test_fmp4: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
