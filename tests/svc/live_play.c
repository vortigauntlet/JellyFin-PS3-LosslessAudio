// Plays a real SoundCloud track through the whole console chain on the host: sc_net -> sc_source -> sc_dec ->
// libfaad -> 48 kHz stereo. Nothing is written to disk; the audio is only measured (peak, RMS, zero crossings).
//
//   live_play             the free track: first sound, 3 s of audio, a seek, an expired-URL refresh
//   live_play --likes <profile>   decode 2 s of each of the first N playable likes (what the hidden share leaves)
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sc_api.h"
#include "sc_dec.h"
#include "sc_net.h"
#include "sc_source.h"
#include "sx_plat.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

typedef struct { double peak, rms; int zc; int n; } Stat;

static int decode_for(ScDec *d, double secs, Stat *st, uint64_t *first_ms, uint64_t t0) {
    memset(st, 0, sizeof(*st));
    static float buf[4800 * 2];
    int total = 0;
    double sum = 0;
    float prev = 0;
    while (total < (int)(secs * 48000)) {
        const int r = scdec_decode(d, buf, 4800);
        if (r <= 0) return r < 0 ? -1 : total;
        if (total == 0 && first_ms) *first_ms = sx_now_ms() - t0;
        for (int i = 0; i < r; i++) {
            const float v = buf[i * 2];
            if (fabsf(v) > st->peak) st->peak = fabsf(v);
            sum += (double)v * v;
            if ((v >= 0) != (prev >= 0)) st->zc++;
            prev = v;
        }
        total += r;
    }
    st->n = total;
    st->rms = sqrt(sum / total);
    return total;
}

static int play_track(sc_net *net, const sc_track *t, double secs) {
    ScSegSource ss;
    int err = 0;
    const uint64_t t0 = sx_now_ms();
    ScSource *src = scsrc_open(net, t->tc_url, &ss, &err);
    if (!src) { printf("  open failed: %d (status %d)\n", err, net->last_status); return -1; }
    ScDec *d = scdec_open(&ss);
    if (!d) { printf("  decoder open failed\n"); scsrc_close(src); return -1; }
    Stat st;
    uint64_t first = 0;
    const int n = decode_for(d, secs, &st, &first, t0);
    printf("  %-40.40s %3d kbps, %d Hz source, first sound %llu ms, %.1f s: peak %.2f rms %.3f zc/s %d, bad frames %d\n", t->title,
           t->tc_kbps, scdec_source_hz(d), (unsigned long long)first, (double)st.n / 48000, st.peak, st.rms, (int)(st.zc / secs), scdec_bad_frames(d));
    scdec_close(d);
    scsrc_close(src);
    return n;
}

int main(int argc, char **argv) {
    sc_net net;
    sc_net_init(&net);
    char url[SC_URL_MAX];
    char *b;
    size_t bl;

    if (argc >= 3 && !strcmp(argv[1], "--likes")) {
        char purl[160];
        sc_profile_url(argv[2], purl, sizeof(purl));
        sc_url_resolve(purl, SC_CID_PLACEHOLDER, url, sizeof(url));
        if (sc_net_api(&net, url, 256 * 1024, &b, &bl)) return 1;
        sc_user u;
        sc_parse_user(b, bl, &u);
        free(b);
        sc_url_user_likes(u.id, 30, SC_CID_PLACEHOLDER, url, sizeof(url));
        if (sc_net_api(&net, url, 2 * 1024 * 1024, &b, &bl)) return 1;
        sc_page p;
        sc_page_init(&p);
        sc_parse_page(b, bl, 160, &p);
        free(b);
        int ok = 0, bad = 0;
        for (int i = 0; i < p.ntr && ok + bad < 12; i++) {
            if (p.tr[i].hidden != SC_HID_NONE || p.tr[i].tc_kind != SC_TC_AAC_HLS) continue;
            if (play_track(&net, &p.tr[i], 2.0) > 0) ok++; else bad++;
        }
        printf("likes: %d played, %d failed\n", ok, bad);
        sc_page_free(&p);
        return bad ? 1 : 0;
    }

    sc_url_resolve("https://soundcloud.com/forss/flickermood", SC_CID_PLACEHOLDER, url, sizeof(url));
    CHECK(sc_net_api(&net, url, 256 * 1024, &b, &bl) == 0);
    sc_track t;
    CHECK(sc_parse_track(b, bl, 160, &t) && t.hidden == SC_HID_NONE);
    free(b);

    ScSegSource ss;
    int err = 0;
    uint64_t t0 = sx_now_ms();
    ScSource *src = scsrc_open(&net, t.tc_url, &ss, &err);
    CHECK(src != NULL);
    if (!src) return 1;
    printf("playlist: %d segments, %u ms, opened in %llu ms\n", ss.nseg, ss.total_ms, (unsigned long long)(sx_now_ms() - t0));
    ScDec *d = scdec_open(&ss);
    CHECK(d != NULL);
    if (!d) return 1;
    Stat st;
    uint64_t first = 0;
    CHECK(decode_for(d, 3.0, &st, &first, t0) >= 3 * 48000);
    printf("first sound %llu ms after the playlist request; 3 s: peak %.3f rms %.4f zero crossings/s %d, source %d Hz\n",
           (unsigned long long)first, st.peak, st.rms, st.zc / 3, scdec_source_hz(d));
    CHECK(st.peak > 0.05 && st.peak <= 1.5 && st.rms > 0.005);

    // Seek to the middle and to near the end.
    const uint32_t secs[] = { 100, 150, 212 };
    for (int k = 0; k < 3; k++) {
        t0 = sx_now_ms();
        CHECK(scdec_seek(d, secs[k]));
        first = 0;
        const int n = decode_for(d, 1.5, &st, &first, t0);
        printf("seek %us: %d pairs, first sound %llu ms, peak %.3f rms %.4f\n", secs[k], n, (unsigned long long)first, st.peak, st.rms);
        CHECK(n >= 48000 || secs[k] > 210);
        CHECK(n > 0 && st.peak > 0.01);
    }

    // A pause long enough for the signed URLs to expire: the next segment fetch re-resolves and carries on.
    scsrc_expire(src);
    CHECK(scdec_seek(d, 40));
    CHECK(decode_for(d, 2.0, &st, NULL, 0) >= 2 * 48000);
    printf("after an expired-URL refresh: refreshes=%d retries=%d, peak %.3f\n", scsrc_refreshes(src), scsrc_retries(src), st.peak);
    CHECK(scsrc_refreshes(src) >= 1 && st.peak > 0.01);

    // To the end of the track.
    CHECK(scdec_seek(d, 205));
    int total = 0;
    float buf[4800 * 2];
    int r;
    while ((r = scdec_decode(d, buf, 4800)) > 0) total += r;
    printf("205 s to the end: %.1f s of audio, final result %d\n", (double)total / 48000, r);
    CHECK(r == 0 && total > 7 * 48000 && total < 11 * 48000);

    scdec_close(d);
    scsrc_close(src);
    printf("live_play: %s\n", g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
