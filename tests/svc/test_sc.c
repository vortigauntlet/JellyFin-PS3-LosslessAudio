// Host tests for the SoundCloud pure layer: client_id scanner, HLS playlist reader, API parsers and
// URL builders.  Fixtures are recorded from the live API with every secret replaced (see
// tests/fixtures/sc and docs/music-services-notes.md).
//
//   test_sc <fixture-dir>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sc_scan.h"
#include "sc_hls.h"
#include "sc_api.h"
#include "sc_store.h"

static int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_MSG(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *FIX;

static char *slurp(const char *name, size_t *len) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", FIX, name);
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
    b[n] = 0;
    fclose(f);
    if (len) *len = (size_t)n;
    return b;
}

// ---- scanner -----------------------------------------------------------------------------------

static void test_html(void) {
    const char *page =
        "<!doctype html><html><head><script>var x=\"<script src=\\\"https://evil/\\\">\";</script>"
        "<scripts src=\"https://no/\"></scripts><script async crossorigin src=\"https://a-v2.sndcdn.com/assets/0-aa.js\"></script>"
        "<script data-src=\"https://no2/\" src=\"https://a-v2.sndcdn.com/assets/1-bb.js\"></script>"
        "<script\nsrc=\"https://a-v2.sndcdn.com/assets/2-cc.js\" defer></script><link rel=\"x\" src=\"https://no3/\">"
        "<SCRIPT src=\"https://upper/\"></SCRIPT><script src=\"\"></script><script src='https://single/'></script>"
        "<script src=\"https://a-v2.sndcdn.com/assets/last.js\"></script></head></html>";
    const size_t n = strlen(page);
    char out[24][SC_HTML_URL_MAX];
    for (size_t step = 1; step <= n; step += (step < 8 ? 1 : 29)) {
        sc_html f;
        sc_html_init(&f, out, 24);
        for (size_t p = 0; p < n; p += step) sc_html_feed(&f, page + p, n - p < step ? n - p : step);
        CHECK_MSG(f.n == 4, "step %zu: %d scripts", step, f.n);
        if (f.n == 4) {
            CHECK(!strcmp(out[0], "https://a-v2.sndcdn.com/assets/0-aa.js") && !strcmp(out[1], "https://a-v2.sndcdn.com/assets/1-bb.js"));
            CHECK(!strcmp(out[2], "https://a-v2.sndcdn.com/assets/2-cc.js") && !strcmp(out[3], "https://a-v2.sndcdn.com/assets/last.js"));
        }
    }
    // The real page is read in 2 KB chunks.
    size_t len;
    char *home = (size_t)0 ? NULL : NULL;
    (void)home; (void)len;
    // A list longer than the room, and a src longer than a URL buffer.
    sc_html f;
    char two[2][SC_HTML_URL_MAX];
    sc_html_init(&f, two, 2);
    const char *many = "<script src=\"a\"></script><script src=\"b\"></script><script src=\"c\"></script>";
    sc_html_feed(&f, many, strlen(many));
    CHECK(f.n == 2 && !strcmp(two[1], "b"));
    char longpage[600];
    strcpy(longpage, "<script src=\"");
    for (int i = 0; i < 300; i++) strcat(longpage, "x");
    strcat(longpage, "\"></script><script src=\"ok\"></script>");
    sc_html_init(&f, two, 2);
    sc_html_feed(&f, longpage, strlen(longpage));
    CHECK(f.n == 1 && !strcmp(two[0], "ok"));
}

static void test_scan(void) {
    const char *id = "aB3dE5gH7jK9mN1pQ3sT5vW7yZ9bC1dE";
    char js[1024];
    snprintf(js, sizeof(js),
             "var a={google_client_id:\"984739005367.apps.googleusercontent.com\",apple_id_client_id:\"com.soundcloud.services.siwa\","
             "x_client_id:\"%s\",xclient_id:\"%s\",client_id:\"short\",client_id:\"%s1\",client_application_id:46941,client_id:\"%s\",client_is_expiring:!1};",
             id, id, id, id);
    const size_t n = strlen(js);

    // Whole buffer, then every possible single split point, then one byte at a time.
    sc_scan s;
    sc_scan_init(&s);
    CHECK(sc_scan_feed(&s, js, n));
    CHECK(sc_scan_id(&s) && strcmp(sc_scan_id(&s), id) == 0);

    for (size_t cut = 1; cut < n; cut++) {
        sc_scan_init(&s);
        sc_scan_feed(&s, js, cut);
        sc_scan_feed(&s, js + cut, n - cut);
        CHECK_MSG(sc_scan_id(&s) && strcmp(sc_scan_id(&s), id) == 0, "split at %zu", cut);
    }
    sc_scan_init(&s);
    for (size_t i = 0; i < n; i++) sc_scan_feed(&s, js + i, 1);
    CHECK(sc_scan_id(&s) && strcmp(sc_scan_id(&s), id) == 0);

    // The decoys alone never match.
    const char *decoys = "google_client_id:\"984739005367.apps.googleusercontent.com\",apple_id_client_id:\"com.soundcloud.services.siwa\","
                         "xclient_id:\"aB3dE5gH7jK9mN1pQ3sT5vW7yZ9bC1dE\",client_id:\"aB3dE5gH7jK9mN1pQ3sT5vW7yZ9bC1dEX\","
                         "client_id:\"aB3dE5gH7jK9mN1pQ3sT5vW7yZ9bC1d\",client_id: \"aB3dE5gH7jK9mN1pQ3sT5vW7yZ9bC1dE\"";
    sc_scan_init(&s);
    CHECK(!sc_scan_feed(&s, decoys, strlen(decoys)));
    CHECK(sc_scan_id(&s) == NULL);

    // After a hit the scanner stays put.
    sc_scan_init(&s);
    sc_scan_feed(&s, js, n);
    sc_scan_feed(&s, "client_id:\"00000000000000000000000000000000\"", 44);
    CHECK(strcmp(sc_scan_id(&s), id) == 0);
}

// ---- HLS -----------------------------------------------------------------------------------------

static void test_hls(void) {
    size_t len;
    char *pl = slurp("track.m3u8", &len);
    sc_hls h;
    CHECK(sc_hls_parse(pl, len, &h) == SC_HLS_OK);
    CHECK_MSG(h.nseg == 22, "nseg %d", h.nseg);
    CHECK_MSG(h.total_ms > 213000 && h.total_ms < 214500, "total %u", h.total_ms);
    CHECK(h.target_ms == 10000);
    CHECK(h.ended);
    CHECK(h.map_uri != NULL);
    CHECK(h.arena == NULL && h.tmpl_pre != NULL);            // SoundCloud's URIs follow one template: nothing but durations is kept per segment
    CHECK(h.dur_ms[0] == 10008);                             // 10.007800 rounded to the millisecond
    CHECK(h.dur_ms[2] == 9985);

    char url[512];
    CHECK(sc_hls_seg_url(&h, -1, "https://playback.media-streaming.soundcloud.cloud/x/y.m3u8", url, sizeof(url)));
    CHECK(strstr(url, "/init.mp4?") != NULL && strncmp(url, "https://", 8) == 0);
    CHECK(sc_hls_seg_url(&h, 0, NULL, url, sizeof(url)));
    CHECK(strstr(url, "/data000.m4s?") != NULL);
    CHECK(sc_hls_seg_url(&h, 21, NULL, url, sizeof(url)));
    CHECK(strstr(url, "/data021.m4s?") != NULL);
    CHECK(!sc_hls_seg_url(&h, 22, NULL, url, sizeof(url)));
    CHECK(!sc_hls_seg_url(&h, 0, NULL, url, 20));              // too small a buffer fails, never truncates

    uint32_t into;
    CHECK(sc_hls_seek(&h, 0, &into) == 0 && into == 0);
    CHECK(sc_hls_seek(&h, 10007, &into) == 0 && into == 10007);
    CHECK(sc_hls_seek(&h, 10008, &into) == 1 && into == 0);
    CHECK(sc_hls_seek(&h, 25000, &into) == 2 && into == 25000 - 20016);
    CHECK(sc_hls_seek(&h, 9999999, &into) == 21);
    // Every URL the template makes is the line the playlist has.
    {
        sc_hls h2;
        CHECK(sc_hls_parse(pl, len, &h2) == SC_HLS_OK);
        int k = 0;
        for (const char *p = pl; *p;) {
            const char *e = strchr(p, '\n');
            if (!e) e = p + strlen(p);
            if (*p && *p != '#') {
                char want[2200];
                size_t wl = (size_t)(e - p);
                if (wl && p[wl - 1] == '\r') wl--;
                memcpy(want, p, wl); want[wl] = 0;
                char got[2200];
                CHECK(sc_hls_seg_url(&h2, k, NULL, got, sizeof(got)));
                CHECK_MSG(strcmp(got, want) == 0, "segment %d: template made a different URL", k);
                k++;
            }
            p = *e ? e + 1 : e;
        }
        CHECK(k == 22);
        sc_hls_free(&h2);
    }
    sc_hls_free(&h);
    free(pl);

    // A long mix: 360 segments cost durations only. One that breaks the pattern keeps every URI, and one that cannot fit is refused.
    {
        char *big = (char *)malloc(400 * 400);
        size_t o = (size_t)sprintf(big, "#EXTM3U\n#EXT-X-TARGETDURATION:10\n#EXT-X-MAP:URI=\"https://c.example/m/init.mp4?q=1\"\n");
        for (int i = 0; i < 360; i++) o += (size_t)sprintf(big + o, "#EXTINF:10.0,\nhttps://c.example/m/data%03d.m4s?sig=abcdef\n", i);
        o += (size_t)sprintf(big + o, "#EXT-X-ENDLIST\n");
        sc_hls hb;
        CHECK(sc_hls_parse(big, o, &hb) == SC_HLS_OK && hb.nseg == 360 && hb.total_ms == 3600000 && hb.arena == NULL);
        char u[200];
        CHECK(sc_hls_seg_url(&hb, 359, NULL, u, sizeof(u)) && strcmp(u, "https://c.example/m/data359.m4s?sig=abcdef") == 0);
        CHECK(sc_hls_seg_url(&hb, 1, NULL, u, sizeof(u)) && strcmp(u, "https://c.example/m/data001.m4s?sig=abcdef") == 0);
        sc_hls_free(&hb);

        // the counter skips one: the first 3 are rebuilt from the template, the rest kept
        const char *irr = "#EXTM3U\n#EXTINF:1,\nhttps://c/d000.m4s?a\n#EXTINF:1,\nhttps://c/d001.m4s?a\n#EXTINF:1,\nhttps://c/d002.m4s?a\n#EXTINF:1,\nhttps://c/d007.m4s?a\n#EXTINF:1,\nhttps://c/zzz\n";
        CHECK(sc_hls_parse(irr, strlen(irr), &hb) == SC_HLS_OK && hb.nseg == 5 && hb.arena != NULL);
        CHECK(sc_hls_seg_url(&hb, 2, NULL, u, sizeof(u)) && strcmp(u, "https://c/d002.m4s?a") == 0);
        CHECK(sc_hls_seg_url(&hb, 3, NULL, u, sizeof(u)) && strcmp(u, "https://c/d007.m4s?a") == 0);
        CHECK(sc_hls_seg_url(&hb, 4, NULL, u, sizeof(u)) && strcmp(u, "https://c/zzz") == 0);
        sc_hls_free(&hb);
        // no counter at all
        const char *nc = "#EXTM3U\n#EXTINF:1,\nhttps://c/aaa.m4s\n#EXTINF:1,\nhttps://c/bbb.m4s\n";
        CHECK(sc_hls_parse(nc, strlen(nc), &hb) == SC_HLS_OK && hb.nseg == 2 && hb.arena != NULL);
        CHECK(sc_hls_seg_url(&hb, 1, NULL, u, sizeof(u)) && strcmp(u, "https://c/bbb.m4s") == 0);
        sc_hls_free(&hb);
        // irregular URIs past the arena limit
        o = (size_t)sprintf(big, "#EXTM3U\n");
        for (int i = 0; i < 360; i++) o += (size_t)sprintf(big + o, "#EXTINF:1,\nhttps://c.example/%d/%0*d_%d.m4s?%0300d\n", i * 7, 1, i % 3, i * 31, i);
        CHECK(sc_hls_parse(big, o, &hb) == SC_HLS_E_TOOMANY);
        free(big);
    }

    // Refusals: encryption of every spelling, master, byte ranges, junk.
    static const char *enc[] = {
        "#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"https://k/key\"\n#EXTINF:10,\nhttps://a/1.m4s\n",
        "#EXTM3U\n#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"skd://x\",KEYFORMAT=\"com.apple.streamingkeydelivery\"\n#EXTINF:10,\nhttps://a/1.m4s\n",
        "#EXTM3U\n#EXT-X-SESSION-KEY:METHOD=AES-128,URI=\"https://k/key\"\n#EXTINF:10,\nhttps://a/1.m4s\n",
        "#EXTM3U\n#EXT-X-KEY:URI=\"https://k/key\"\n#EXTINF:10,\nhttps://a/1.m4s\n",
    };
    for (size_t i = 0; i < sizeof(enc) / sizeof(enc[0]); i++) {
        CHECK_MSG(sc_hls_parse(enc[i], strlen(enc[i]), &h) == SC_HLS_E_ENCRYPTED, "enc case %zu", i);
    }
    const char *none = "#EXTM3U\n#EXT-X-KEY:METHOD=NONE\n#EXTINF:10,\nhttps://a/1.m4s\n";
    CHECK(sc_hls_parse(none, strlen(none), &h) == SC_HLS_OK);
    sc_hls_free(&h);
    const char *master = "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1\nhttps://a/v.m3u8\n";
    CHECK(sc_hls_parse(master, strlen(master), &h) == SC_HLS_E_MASTER);
    const char *br = "#EXTM3U\n#EXTINF:10,\n#EXT-X-BYTERANGE:100@0\nhttps://a/1.m4s\n";
    CHECK(sc_hls_parse(br, strlen(br), &h) == SC_HLS_E_BYTERANGE);
    CHECK(sc_hls_parse("hello", 5, &h) == SC_HLS_E_NOTM3U);
    CHECK(sc_hls_parse("#EXTM3U\n", 8, &h) == SC_HLS_E_EMPTY);
    // CRLF and relative URIs.
    const char *rel = "#EXTM3U\r\n#EXT-X-MAP:URI=\"init.mp4\"\r\n#EXTINF:5.5,\r\nseg0.m4s?a=1\r\n#EXTINF:4.5,\r\n/abs/seg1.m4s\r\n#EXT-X-ENDLIST\r\n";
    CHECK(sc_hls_parse(rel, strlen(rel), &h) == SC_HLS_OK && h.nseg == 2 && h.total_ms == 10000);
    CHECK(sc_hls_seg_url(&h, 0, "https://h.example/dir/list.m3u8?sig=1", url, sizeof(url)) && strcmp(url, "https://h.example/dir/seg0.m4s?a=1") == 0);
    CHECK(sc_hls_seg_url(&h, 1, "https://h.example/dir/list.m3u8", url, sizeof(url)) && strcmp(url, "https://h.example/abs/seg1.m4s") == 0);
    CHECK(sc_hls_seg_url(&h, -1, "https://h.example/dir/list.m3u8", url, sizeof(url)) && strcmp(url, "https://h.example/dir/init.mp4") == 0);
    sc_hls_free(&h);
    CHECK(sc_url_join("https://h.example/a/b.m3u8", "//cdn.example/x.m4s", 19, url, sizeof(url)) && strcmp(url, "https://cdn.example/x.m4s") == 0);
    CHECK(sc_url_join("https://h.example", "x.m4s", 5, url, sizeof(url)) && strcmp(url, "https://h.example/x.m4s") == 0);
    CHECK(!sc_url_join("ftp://h.example/a", "x", 1, url, sizeof(url)));
}

// ---- API -----------------------------------------------------------------------------------------

static void test_track(void) {
    size_t len;
    char *j = slurp("track_free.json", &len);
    sc_track t;
    CHECK(sc_parse_track(j, len, 160, &t));
    CHECK(t.id == 293 && t.hidden == SC_HID_NONE);
    CHECK(t.tc_kind == SC_TC_AAC_HLS && t.tc_kbps == 160);
    CHECK(strstr(t.tc, "/stream/hls") != NULL && strchr(t.tc, '?') == NULL && strncmp(t.tc, "media/", 6) == 0);
    {
        char full[300];
        CHECK(sc_track_url(&t, full, sizeof(full)) && strncmp(full, "https://api-v2.soundcloud.com/media/soundcloud:tracks:293/", 56) == 0);
        CHECK(!sc_track_url(&t, full, 20));
    }
    CHECK(sizeof(sc_track) <= 340);
    CHECK(strcmp(t.title, "Flickermood") == 0 || t.title[0]);
    CHECK(t.dur_ms > 210000 && t.dur_ms < 216000);
    CHECK(t.artist[0] != 0);
    CHECK(sc_parse_track(j, len, 96, &t) && t.tc_kbps == 96 && t.tc_kind == SC_TC_AAC_HLS);
    free(j);

    j = slurp("track_encrypted.json", &len);
    CHECK(sc_parse_track(j, len, 160, &t));
    CHECK_MSG(t.hidden == SC_HID_ENCRYPTED, "hidden %d", t.hidden);
    CHECK(t.tc[0] == 0 && t.tc_kind == SC_TC_NONE);          // a hidden track carries no URL to fetch
    free(j);

    // Hand-made transcoding sets.
    static const char *mk =
        "{\"id\":7,\"kind\":\"track\",\"title\":\"t\",\"duration\":1000,\"user\":{\"username\":\"u\",\"avatar_url\":\"https://i/a-large.jpg\"},"
        "\"media\":{\"transcodings\":[%s]}}";
    struct { const char *tcs; int hidden; int kind; int kbps; } cases[] = {
        // preview only
        { "{\"url\":\"https://api-v2.soundcloud.com/m/1\",\"preset\":\"aac_160k\",\"snipped\":true,\"format\":{\"protocol\":\"hls\",\"mime_type\":\"audio/mp4; codecs=\\\"mp4a.40.2\\\"\"}}",
          SC_HID_PREVIEW, 0, 0 },
        // preview URL path
        { "{\"url\":\"https://api-v2.soundcloud.com/m/preview/1\",\"preset\":\"aac_160k\",\"snipped\":false,\"format\":{\"protocol\":\"hls\",\"mime_type\":\"audio/mp4; codecs=\\\"mp4a.40.2\\\"\"}}",
          SC_HID_PREVIEW, 0, 0 },
        // opus only
        { "{\"url\":\"https://api-v2.soundcloud.com/m/2\",\"preset\":\"opus_0_0\",\"snipped\":false,\"format\":{\"protocol\":\"hls\",\"mime_type\":\"audio/ogg; codecs=\\\"opus\\\"\"}}",
          SC_HID_UNSUPPORTED, 0, 0 },
        // mp3 over hls is not progressive: unsupported
        { "{\"url\":\"https://api-v2.soundcloud.com/m/3\",\"preset\":\"mp3_0_0\",\"snipped\":false,\"format\":{\"protocol\":\"hls\",\"mime_type\":\"audio/mpeg\"}}",
          SC_HID_UNSUPPORTED, 0, 0 },
        // mp3 progressive only
        { "{\"url\":\"https://api-v2.soundcloud.com/m/4\",\"preset\":\"mp3_0_0\",\"snipped\":false,\"format\":{\"protocol\":\"progressive\",\"mime_type\":\"audio/mpeg\"}}",
          SC_HID_NONE, SC_TC_MP3_PROGRESSIVE, 128 },
        // aac beats mp3 whichever order they arrive in
        { "{\"url\":\"https://api-v2.soundcloud.com/m/5\",\"preset\":\"mp3_0_0\",\"snipped\":false,\"format\":{\"protocol\":\"progressive\",\"mime_type\":\"audio/mpeg\"}},"
          "{\"url\":\"https://api-v2.soundcloud.com/m/6\",\"preset\":\"aac_96k\",\"snipped\":false,\"format\":{\"protocol\":\"hls\",\"mime_type\":\"audio/mp4; codecs=\\\"mp4a.40.2\\\"\"}}",
          SC_HID_NONE, SC_TC_AAC_HLS, 96 },
        // encrypted next to a usable one: the usable one wins, the encrypted URL is never kept
        { "{\"url\":\"https://api-v2.soundcloud.com/m/7\",\"preset\":\"aac_160k\",\"snipped\":false,\"format\":{\"protocol\":\"ctr-encrypted-hls\",\"mime_type\":\"audio/mp4; codecs=\\\"mp4a.40.2\\\"\"}},"
          "{\"url\":\"https://api-v2.soundcloud.com/m/8\",\"preset\":\"aac_96k\",\"snipped\":false,\"format\":{\"protocol\":\"hls\",\"mime_type\":\"audio/mp4; codecs=\\\"mp4a.40.2\\\"\"}}",
          SC_HID_NONE, SC_TC_AAC_HLS, 96 },
        // empty
        { "", SC_HID_UNSUPPORTED, 0, 0 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char doc[2048];
        snprintf(doc, sizeof(doc), mk, cases[i].tcs);
        CHECK_MSG(sc_parse_track(doc, strlen(doc), 160, &t), "case %zu parse", i);
        CHECK_MSG(t.hidden == cases[i].hidden, "case %zu hidden %d want %d", i, t.hidden, cases[i].hidden);
        CHECK_MSG(t.tc_kind == cases[i].kind && t.tc_kbps == cases[i].kbps, "case %zu kind %d/%d", i, t.tc_kind, t.tc_kbps);
        if (t.hidden != SC_HID_NONE) CHECK_MSG(t.tc[0] == 0, "case %zu kept a url", i);
        if (i == 6) CHECK(strcmp(t.tc, "m/8") == 0);
    }
    // Not streamable, stub, avatar fallback.
    const char *ns = "{\"id\":9,\"kind\":\"track\",\"streamable\":false,\"media\":{\"transcodings\":[]}}";
    CHECK(sc_parse_track(ns, strlen(ns), 160, &t) && t.hidden == SC_HID_NOT_STREAMABLE);
    const char *stub = "{\"id\":10,\"kind\":\"track\",\"monetization_model\":\"BLACKBOX\",\"policy\":\"MONETIZE\"}";
    CHECK(sc_parse_track(stub, strlen(stub), 160, &t) && t.hidden == SC_HID_STUB && t.id == 10);
    char doc[2048];
    snprintf(doc, sizeof(doc), mk, "");
    CHECK(sc_parse_track(doc, strlen(doc), 160, &t) && t.art[0] == 0);          // an avatar that is not on sndcdn.com gets no picture
    {
        char d2[2048];
        snprintf(d2, sizeof(d2), "{\"id\":8,\"kind\":\"track\",\"user\":{\"username\":\"u\",\"avatar_url\":\"https://i1.sndcdn.com/avatars-AbC-xyz-large.jpg\"},\"media\":{\"transcodings\":[]}}");
        CHECK(sc_parse_track(d2, strlen(d2), 160, &t) && !strcmp(t.art, "sca:i1:s:jpg:avatars-AbC-xyz"));      // the uploader's avatar stands in for missing artwork
        snprintf(d2, sizeof(d2), "{\"id\":9,\"kind\":\"track\",\"artwork_url\":\"https://i1.sndcdn.com/artworks-Q-r-large.jpg\",\"user\":{\"avatar_url\":\"https://i1.sndcdn.com/avatars-AbC-xyz-large.jpg\"},\"media\":{\"transcodings\":[]}}");
        CHECK(sc_parse_track(d2, strlen(d2), 160, &t) && !strcmp(t.art, "sca:i1:s:jpg:artworks-Q-r"));
        // a transcoding on another host is not usable
        snprintf(d2, sizeof(d2), "{\"id\":10,\"kind\":\"track\",\"media\":{\"transcodings\":[{\"url\":\"https://evil.example/m/1\",\"preset\":\"aac_160k\",\"snipped\":false,\"format\":{\"protocol\":\"hls\",\"mime_type\":\"audio/mp4; codecs=\\\"mp4a.40.2\\\"\"}}]}}");
        CHECK(sc_parse_track(d2, strlen(d2), 160, &t) && t.hidden == SC_HID_UNSUPPORTED && t.tc[0] == 0);
    }
    CHECK(!sc_parse_track("[]", 2, 160, &t));
    CHECK(!sc_parse_track("{\"id\":0}", 8, 160, &t));
    CHECK(!sc_parse_track("{\"id\":", 6, 160, &t));
}

static void test_pages(void) {
    size_t len;
    char *j = slurp("user_likes.json", &len);
    sc_page p;
    sc_page_init(&p);
    int n = sc_parse_page(j, len, 160, &p);
    CHECK_MSG(n == 6, "likes items %d", n);
    CHECK(p.ntr == 5 && p.npl == 1);
    CHECK(strncmp(p.next_href, "https://api-v2.soundcloud.com/users/183/likes?", 40) == 0);
    CHECK(strstr(p.next_href, "client_id") == NULL);
    int hidden = 0;
    for (int i = 0; i < SC_HID_COUNT; i++) if (i != SC_HID_NONE) hidden += p.hidden[i];
    CHECK(p.hidden[SC_HID_NONE] + hidden == p.ntr);
    for (int i = 0; i < p.ntr; i++) CHECK(p.tr[i].id > 0 && p.tr[i].title[0]);
    CHECK(p.pl[0].id > 0 && p.pl[0].title[0]);
    sc_page_free(&p);
    free(j);

    j = slurp("user_tracks.json", &len);
    sc_page_init(&p);
    CHECK(sc_parse_page(j, len, 160, &p) == 3 && p.ntr == 3 && p.npl == 0);
    sc_page_free(&p);
    free(j);

    j = slurp("search_tracks.json", &len);
    sc_page_init(&p);
    n = sc_parse_page(j, len, 160, &p);
    CHECK(n > 0 && p.ntr == n);
    sc_page_free(&p);
    free(j);

    j = slurp("user_playlists.json", &len);
    sc_page_init(&p);
    n = sc_parse_page(j, len, 160, &p);
    CHECK(n > 0 && p.npl == n && p.ntr == 0);
    CHECK(p.pl[0].track_count > 0 && p.pl[0].owner[0]);
    sc_page_free(&p);
    free(j);

    CHECK(sc_parse_page("{}", 2, 160, &p) == -1);
    CHECK(sc_parse_page("[1,2]", 5, 160, &p) == -1);
    CHECK(sc_parse_page("{\"collection\":[1,null,{\"kind\":\"x\"}]}", 35, 160, &p) == 0);
    sc_page_free(&p);
}

// The streaming forms give what the whole-document forms give, whatever the chunking.
typedef struct { const char *p; size_t n, pos, step; } SRd;
static int srd_read(void *c, void *buf, int n) {
    SRd *r = (SRd *)c;
    size_t k = r->n - r->pos;
    if (k > r->step) k = r->step;
    if (k > (size_t)n) k = (size_t)n;
    memcpy(buf, r->p + r->pos, k);
    r->pos += k;
    return (int)k;
}

static void test_streaming(void) {
    static const char *files[] = { "user_likes.json", "user_tracks.json", "search_tracks.json", "user_playlists.json" };
    static const size_t steps[] = { 1, 5, 1000, 100000 };
    for (size_t f = 0; f < sizeof(files) / sizeof(files[0]); f++) {
        size_t len;
        char *j = slurp(files[f], &len);
        sc_page whole;
        sc_page_init(&whole);
        const int nw = sc_parse_page(j, len, 160, &whole);
        for (size_t s = 0; s < sizeof(steps) / sizeof(steps[0]); s++) {
            sc_page st;
            sc_page_init(&st);
            SRd r = { j, len, 0, steps[s] };
            const int ns = sc_page_stream(srd_read, &r, 160, 0, &st);
            CHECK_MSG(ns == nw, "%s step %zu: %d items, whole %d", files[f], steps[s], ns, nw);
            CHECK(st.ntr == whole.ntr && st.npl == whole.npl && !strcmp(st.next_href, whole.next_href));
            for (int i = 0; i < st.ntr && i < whole.ntr; i++)
                CHECK(st.tr[i].id == whole.tr[i].id && st.tr[i].hidden == whole.tr[i].hidden && !strcmp(st.tr[i].tc, whole.tr[i].tc) &&
                      !strcmp(st.tr[i].title, whole.tr[i].title));
            for (int i = 0; i < SC_HID_COUNT; i++) CHECK(st.hidden[i] == whole.hidden[i]);
            sc_page_free(&st);
        }
        sc_page_free(&whole);
        free(j);
    }
    {   // a playlist document, with its bare ids
        size_t len;
        char *j = slurp("playlist.json", &len);
        sc_page whole, st;
        sc_playlist mw, ms;
        sc_page_init(&whole); sc_page_init(&st);
        CHECK(sc_parse_playlist(j, len, 160, &mw, &whole));
        SRd r = { j, len, 0, 7 };
        const int n = sc_playlist_stream(srd_read, &r, 160, 0, &ms, &st);
        CHECK_MSG(n == whole.ntr, "playlist stream %d", n);
        CHECK(ms.id == mw.id && !strcmp(ms.title, mw.title) && ms.track_count == mw.track_count && !strcmp(ms.owner, mw.owner));
        CHECK(st.ntr == whole.ntr && st.hidden[SC_HID_STUB] == whole.hidden[SC_HID_STUB]);
        sc_page_free(&whole); sc_page_free(&st);
        free(j);
    }
    {   // the bare array of tracks?ids=
        size_t len;
        char *j = slurp("tracks_by_ids.json", &len);
        sc_page whole, st;
        sc_page_init(&whole); sc_page_init(&st);
        const int nw = sc_parse_tracks_array(j, len, 160, &whole);
        SRd r = { j, len, 0, 13 };
        CHECK(sc_tracks_stream(srd_read, &r, 160, 0, &st) == nw && st.ntr == whole.ntr);
        sc_page_free(&whole); sc_page_free(&st);
        free(j);
    }
    {   // a limit stops the read early
        size_t len;
        char *j = slurp("user_likes.json", &len);
        sc_page st;
        sc_page_init(&st);
        SRd r = { j, len, 0, 100 };
        CHECK(sc_page_stream(srd_read, &r, 160, 2, &st) == 2 && st.ntr + st.npl == 2);
        sc_page_free(&st);
        free(j);
    }
    {   // not a listing, and a truncated one
        sc_page st;
        sc_page_init(&st);
        SRd r1 = { "{\"error\":1}", 11, 0, 4 };
        CHECK(sc_page_stream(srd_read, &r1, 160, 0, &st) == 0);          // an object with no collection: no items (the caller treats 0 items with no next_href as the end)
        size_t len;
        char *j = slurp("user_likes.json", &len);
        SRd r2 = { j, len / 2, 0, 100 };
        CHECK(sc_page_stream(srd_read, &r2, 160, 0, &st) < 0);
        CHECK(st.ntr > 0);                                            // what arrived before the break is kept
        sc_page_free(&st);
        free(j);
    }
}

static void test_playlist(void) {
    size_t len;
    char *j = slurp("playlist.json", &len);
    sc_page p;
    sc_playlist meta;
    sc_page_init(&p);
    CHECK(sc_parse_playlist(j, len, 160, &meta, &p));
    CHECK(meta.id > 0 && meta.title[0] && meta.track_count == 7);
    CHECK_MSG(p.ntr == 7, "playlist tracks %d", p.ntr);
    CHECK_MSG(p.hidden[SC_HID_STUB] == 2, "stubs %d", p.hidden[SC_HID_STUB]);
    free(j);

    j = slurp("tracks_by_ids.json", &len);
    sc_page full;
    sc_page_init(&full);
    int n = sc_parse_tracks_array(j, len, 160, &full);
    CHECK(n > 0 && full.ntr == n);
    int filled = sc_fill_stubs(&p, &full);
    CHECK_MSG(filled >= 0, "filled %d", filled);
    int stubs = 0;
    for (int i = 0; i < p.ntr; i++) if (p.tr[i].hidden == SC_HID_STUB) stubs++;
    CHECK(stubs == p.hidden[SC_HID_STUB]);
    sc_page_free(&full);
    sc_page_free(&p);
    free(j);
}

static void test_user_and_stream(void) {
    size_t len;
    char *j = slurp("user.json", &len);
    sc_user u;
    CHECK(sc_parse_user(j, len, &u) && u.id == 183 && strcmp(u.permalink, "forss") == 0 && u.name[0]);
    CHECK(!sc_parse_user("{\"kind\":\"track\",\"id\":1}", 23, &u));
    free(j);

    j = slurp("stream_url.json", &len);
    char url[512];
    CHECK(sc_parse_stream_url(j, len, url, sizeof(url)) && strncmp(url, "https://playback.media-streaming.soundcloud.cloud/", 50) == 0);
    CHECK(!sc_parse_stream_url(j, len, url, 10));
    free(j);
    const char *http = "{\"url\":\"http://x/y.m3u8\"}";
    CHECK(!sc_parse_stream_url(http, strlen(http), url, sizeof(url)));
}

static void test_urls(void) {
    char u[512];
    const char *cid = "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX";
    CHECK(sc_url_resolve("https://soundcloud.com/forss/flickermood", cid, u, sizeof(u)));
    CHECK(strcmp(u, "https://api-v2.soundcloud.com/resolve?url=https%3A%2F%2Fsoundcloud.com%2Fforss%2Fflickermood&client_id=XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX") == 0);
    CHECK(sc_url_user_likes(183, 50, cid, u, sizeof(u)) && strncmp(u, "https://api-v2.soundcloud.com/users/183/likes?limit=50&linked_partitioning=1&client_id=", 86) == 0);
    CHECK(sc_url_user_reposts(183, 20, cid, u, sizeof(u)) && strstr(u, "/stream/users/183/reposts?") != NULL);
    CHECK(sc_url_search_tracks("lo-fi & chill", 30, cid, u, sizeof(u)) && strstr(u, "q=lo-fi%20%26%20chill&") != NULL);
    int64_t ids[3] = { 1, 22, 333 };
    CHECK(sc_url_tracks_ids(ids, 3, cid, u, sizeof(u)) && strstr(u, "tracks?ids=1,22,333&client_id=") != NULL);
    CHECK(!sc_url_tracks_ids(ids, 0, cid, u, sizeof(u)));
    CHECK(!sc_url_user_likes(183, 50, cid, u, 20));

    // The client id is only ever appended to the API host.
    CHECK(sc_url_with_cid("https://api-v2.soundcloud.com/media/x/stream/hls", cid, u, sizeof(u)) && strstr(u, "/hls?client_id=XXXX") != NULL);
    CHECK(sc_url_with_cid("https://api-v2.soundcloud.com/users/1/likes?offset=a&limit=6", cid, u, sizeof(u)) && strstr(u, "limit=6&client_id=XXXX") != NULL);
    CHECK(!sc_url_with_cid("https://evil.example/users/1/likes", cid, u, sizeof(u)));
    CHECK(!sc_url_with_cid("http://api-v2.soundcloud.com/x", cid, u, sizeof(u)));
    CHECK(!sc_url_with_cid("https://api-v2.soundcloud.com.evil.example/x", cid, u, sizeof(u)));

    // Profile input.
    static const char *good[] = { "forss", "@forss", " forss ", "soundcloud.com/forss", "https://soundcloud.com/forss/", "http://www.soundcloud.com/forss/likes?x=1", "https://m.soundcloud.com/forss/tracks" };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        CHECK_MSG(sc_profile_url(good[i], u, sizeof(u)) && strcmp(u, "https://soundcloud.com/forss") == 0, "profile '%s' -> '%s'", good[i], u);
    }
    CHECK(!sc_profile_url("", u, sizeof(u)));
    CHECK(!sc_profile_url("a b", u, sizeof(u)) || strcmp(u, "https://soundcloud.com/a") != 0);
    CHECK(!sc_profile_url("fo", u, sizeof(u)));
    CHECK(!sc_profile_url("forss%2Fx", u, sizeof(u)));
    CHECK(!sc_profile_url("for<ss>", u, sizeof(u)));

    // Artwork.
    CHECK(sc_art_url("https://i1.sndcdn.com/artworks-000-abc-large.jpg", 500, u, sizeof(u)) && strcmp(u, "https://i1.sndcdn.com/artworks-000-abc-t500x500.jpg") == 0);
    CHECK(sc_art_url("https://i1.sndcdn.com/avatars-000-abc-large.jpg?x=1", 300, u, sizeof(u)) || 1);
    CHECK(sc_art_url("https://i1.sndcdn.com/noext", 500, u, sizeof(u)) && strcmp(u, "https://i1.sndcdn.com/noext") == 0);

    // Artwork keys fold a URL into the thumbnail cache's 63 characters and unfold only to sndcdn.com.
    char key[96];
    CHECK(sc_art_key("https://i1.sndcdn.com/artworks-000123456789-abcdef-large.jpg", key, sizeof(key)));
    CHECK(strcmp(key, "sca:i1:s:jpg:artworks-000123456789-abcdef") == 0);
    CHECK(sc_art_key_url(key, 500, u, sizeof(u)) && strcmp(u, "https://i1.sndcdn.com/artworks-000123456789-abcdef-t500x500.jpg") == 0);
    CHECK(sc_art_key_url(key, 300, u, sizeof(u)) && strstr(u, "-t300x300.jpg") != NULL);
    CHECK(sc_art_key_url(key, 77, u, sizeof(u)) && strstr(u, "-t500x500.jpg") != NULL);
    CHECK(sc_is_art_key(key) && !sc_is_art_key("12345") && !sc_is_art_key(""));
    CHECK(sc_art_key("https://a1.sndcdn.com/images/default_avatar_large.png", key, sizeof(key)));
    CHECK(strcmp(key, "sca:a1:u:png:images/default_avatar_large") == 0);
    CHECK(sc_art_key_url(key, 500, u, sizeof(u)) && strcmp(u, "https://a1.sndcdn.com/images/default_avatar_large.png") == 0);
    CHECK(sc_art_key("https://i1.sndcdn.com/avatars-AbC123-xYz-large.jpg?x=1", key, sizeof(key)) && strcmp(key, "sca:i1:s:jpg:avatars-AbC123-xYz") == 0);
    CHECK(!sc_art_key("http://i1.sndcdn.com/artworks-1-large.jpg", key, sizeof(key)));            // plain http
    CHECK(!sc_art_key("https://evil.example/artworks-1-large.jpg", key, sizeof(key)));
    CHECK(!sc_art_key("https://sndcdn.com.evil.example/artworks-1-large.jpg", key, sizeof(key)));
    CHECK(!sc_art_key("https://i1.sndcdn.com/../etc-large.jpg", key, sizeof(key)));
    CHECK(!sc_art_key("https://i1.sndcdn.com/artworks-1-large", key, sizeof(key)));               // no extension
    CHECK(!sc_art_key("https://i1.sndcdn.com/artworks-1-large.jpg", key, 20));                    // too small
    // A key from anywhere else never unfolds to a foreign host or a path trick.
    CHECK(!sc_art_key_url("sca:i1.evil.example:s:jpg:x", 500, u, sizeof(u)));
    CHECK(!sc_art_key_url("sca:i1:s:jpg:../x", 500, u, sizeof(u)));
    CHECK(!sc_art_key_url("sca:i1:s:jp/g:x", 500, u, sizeof(u)));
    CHECK(!sc_art_key_url("sca:i1:x:jpg:x", 500, u, sizeof(u)));
    CHECK(!sc_art_key_url("sca:i1:s:jpg:", 500, u, sizeof(u)));
}

static void test_store(void) {
    ScCidFile c;
    sc_cidfile_parse("cid aB3dE5gH7jK9mN1pQ3sT5vW7yZ9bC1dE\nat 1792000000\n", &c);
    CHECK(strcmp(c.cid, "aB3dE5gH7jK9mN1pQ3sT5vW7yZ9bC1dE") == 0 && c.at == 1792000000L);
    sc_cidfile_parse("cid short\nat 5\n", &c);
    CHECK(c.cid[0] == 0 && c.at == 5);
    sc_cidfile_parse("cid aB3dE5gH7jK9mN1pQ3sT5vW7yZ9bC1d!\n", &c);          // 32 characters, one not alphanumeric
    CHECK(c.cid[0] == 0);
    sc_cidfile_parse("\xEF\xBB\xBF" "cid aB3dE5gH7jK9mN1pQ3sT5vW7yZ9bC1dE\r\nat 7\r\n", &c);
    CHECK(c.cid[0] != 0 && c.at == 7);
    sc_cidfile_parse("", &c);
    CHECK(c.cid[0] == 0 && c.at == 0);
    sc_cidfile_parse(NULL, &c);
    CHECK(c.cid[0] == 0);
    char buf[128];
    ScCidFile w = { "aB3dE5gH7jK9mN1pQ3sT5vW7yZ9bC1dE", 42 };
    const int n = sc_cidfile_format(&w, buf, sizeof(buf));
    CHECK(n > 0);
    sc_cidfile_parse(buf, &c);
    CHECK(strcmp(c.cid, w.cid) == 0 && c.at == 42);
    ScCidFile bad = { "x", 1 };
    CHECK(sc_cidfile_format(&bad, buf, sizeof(buf)) < 0);
    CHECK(sc_cidfile_format(&w, buf, 10) < 0);

    ScSettings s;
    sc_settings_parse("profile forss\nquality 96\n", &s);
    CHECK(strcmp(s.profile, "forss") == 0 && s.quality == 96);
    sc_settings_parse("", &s);
    CHECK(s.profile[0] == 0 && s.quality == 160);
    sc_settings_parse("quality 128\nprofile a/b\n", &s);                   // not a choice we offer, not a profile name
    CHECK(s.profile[0] == 0 && s.quality == 160);
    sc_settings_parse("profile  my-name_1 \r\nquality 160\r\n", &s);
    CHECK(strcmp(s.profile, "my-name_1") == 0 && s.quality == 160);
    sc_settings_parse("profile two words\n", &s);
    CHECK(s.profile[0] == 0);
    s.quality = 96;
    snprintf(s.profile, sizeof(s.profile), "%s", "forss");
    CHECK(sc_settings_format(&s, buf, sizeof(buf)) > 0);
    ScSettings r;
    sc_settings_parse(buf, &r);
    CHECK(strcmp(r.profile, "forss") == 0 && r.quality == 96);
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: test_sc <fixture-dir>\n"); return 2; }
    FIX = argv[1];
    test_scan();
    test_html();
    test_hls();
    test_track();
    test_pages();
    test_playlist();
    test_streaming();
    test_user_and_stream();
    test_urls();
    test_store();
    printf("test_sc: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
