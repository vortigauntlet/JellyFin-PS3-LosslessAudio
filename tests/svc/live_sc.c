// Drives the real SoundCloud chain with the same code the console runs (sc_net over sx_http/mbedTLS 2.28):
// scrape the client_id, resolve a track, pick a transcoding, fetch the playlist, init segment and the
// first media segment, and check TLS against the bundled root store.  Prints no secrets.
//
//   live_sc                       one free track end to end
//   live_sc --stats <profile>     what share of a profile's likes is hidden, and why
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sc_net.h"
#include "sc_api.h"
#include "sc_hls.h"
#include "sx_http.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const char *HID_NAME[SC_HID_COUNT] = { "playable", "stub", "encrypted", "preview", "unsupported", "not-streamable" };

static int stats(sc_net *n, const char *profile) {
    char purl[160], url[640];
    if (!sc_profile_url(profile, purl, sizeof(purl))) { printf("bad profile\n"); return 2; }
    sc_url_resolve(purl, SC_CID_PLACEHOLDER, url, sizeof(url));
    char *b; size_t bl;
    int rc = sc_net_api(n, url, 256 * 1024, &b, &bl);
    if (rc) { printf("resolve rc=%d status=%d\n", rc, n->last_status); return 1; }
    sc_user u;
    if (!sc_parse_user(b, bl, &u)) { printf("not a user\n"); free(b); return 1; }
    free(b);
    int total[SC_HID_COUNT] = { 0 };
    int pages = 0, pls = 0;
    sc_url_user_likes(u.id, 50, SC_CID_PLACEHOLDER, url, sizeof(url));
    while (url[0] && pages < 40) {
        rc = sc_net_api(n, url, 2 * 1024 * 1024, &b, &bl);
        if (rc) { printf("likes rc=%d status=%d\n", rc, n->last_status); return 1; }
        sc_page p;
        sc_page_init(&p);
        sc_parse_page(b, bl, 160, &p);
        free(b);
        for (int i = 0; i < SC_HID_COUNT; i++) total[i] += p.hidden[i];
        pls += p.npl;
        url[0] = 0;
        if (p.next_href[0]) sc_url_with_cid(p.next_href, SC_CID_PLACEHOLDER, url, sizeof(url));
        sc_page_free(&p);
        pages++;
    }
    int all = 0;
    for (int i = 0; i < SC_HID_COUNT; i++) all += total[i];
    printf("profile likes: %d tracks in %d pages (+%d liked playlists)\n", all, pages, pls);
    for (int i = 0; i < SC_HID_COUNT; i++) printf("  %-15s %5d  %5.1f%%\n", HID_NAME[i], total[i], all ? 100.0 * total[i] / all : 0.0);
    return 0;
}

int main(int argc, char **argv) {
    sc_net n;
    sc_net_init(&n);
    if (argc >= 3 && !strcmp(argv[1], "--stats")) return stats(&n, argv[2]);

    uint64_t t0 = sx_now_ms();
    int rc = sc_net_scrape_cid(&n);
    printf("client id: rc=%d in %llu ms (length %zu)\n", rc, (unsigned long long)(sx_now_ms() - t0), strlen(n.cid));
    CHECK(rc == SC_NET_OK);
    if (rc) return 1;

    char url[640];
    sc_url_resolve("https://soundcloud.com/forss/flickermood", SC_CID_PLACEHOLDER, url, sizeof(url));
    char *b; size_t bl;
    t0 = sx_now_ms();
    rc = sc_net_api(&n, url, 256 * 1024, &b, &bl);
    printf("resolve: rc=%d status=%d %zu bytes in %llu ms\n", rc, n.last_status, bl, (unsigned long long)(sx_now_ms() - t0));
    CHECK(rc == SC_NET_OK);
    if (rc) return 1;
    sc_track t;
    CHECK(sc_parse_track(b, bl, 160, &t));
    free(b);
    printf("track: hidden=%s kind=%d kbps=%d dur=%u ms\n", HID_NAME[t.hidden], t.tc_kind, t.tc_kbps, t.dur_ms);
    CHECK(t.hidden == SC_HID_NONE && t.tc_kind == SC_TC_AAC_HLS);

    char m3u8[SC_URL_MAX];
    t0 = sx_now_ms();
    rc = sc_net_stream_url(&n, t.tc_url, m3u8, sizeof(m3u8));
    printf("stream url: rc=%d status=%d len=%zu in %llu ms\n", rc, n.last_status, strlen(m3u8), (unsigned long long)(sx_now_ms() - t0));
    CHECK(rc == SC_NET_OK);
    if (rc) return 1;

    char *pl; size_t pll;
    t0 = sx_now_ms();
    rc = sc_net_get(&n, m3u8, 2 * 1024 * 1024, &pl, &pll);
    printf("playlist: rc=%d %zu bytes in %llu ms\n", rc, pll, (unsigned long long)(sx_now_ms() - t0));
    CHECK(rc == SC_NET_OK);
    if (rc) return 1;
    sc_hls h;
    CHECK(sc_hls_parse(pl, pll, &h) == SC_HLS_OK);
    printf("hls: %d segments, %u ms total, target %u ms\n", h.nseg, h.total_ms, h.target_ms);

    char su[SC_URL_MAX];
    CHECK(sc_hls_seg_url(&h, -1, m3u8, su, sizeof(su)));
    char *init; size_t il;
    rc = sc_net_get(&n, su, 64 * 1024, &init, &il);
    printf("init: rc=%d %zu bytes %.4s\n", rc, il, rc == 0 && il > 8 ? init + 4 : "????");
    CHECK(rc == SC_NET_OK && il > 100 && !memcmp(init + 4, "ftyp", 4));
    if (rc == 0) free(init);

    CHECK(sc_hls_seg_url(&h, 0, m3u8, su, sizeof(su)));
    char *seg; size_t sl;
    t0 = sx_now_ms();
    rc = sc_net_get(&n, su, 1024 * 1024, &seg, &sl);
    printf("segment 0: rc=%d %zu bytes in %llu ms\n", rc, sl, (unsigned long long)(sx_now_ms() - t0));
    CHECK(rc == SC_NET_OK && sl > 1000);
    if (rc == 0) { CHECK(!memcmp(seg + 4, "styp", 4) || !memcmp(seg + 4, "moof", 4)); free(seg); }

    // An encrypted-only track is hidden and nothing for it is ever requested.
    sc_hls_free(&h);
    free(pl);
    printf("live_sc: %s\n", g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
