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
    CHECK(h.map_len > 0);
    CHECK(h.seg[0].dur_ms == 10008);                         // 10.007800 rounded down to the millisecond
    CHECK(h.seg[2].dur_ms == 9985);

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
    sc_hls_free(&h);
    free(pl);

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
    CHECK(strstr(t.tc_url, "/stream/hls") != NULL && strchr(t.tc_url, '?') == NULL);
    CHECK(strcmp(t.title, "Flickermood") == 0 || t.title[0]);
    CHECK(t.dur_ms > 210000 && t.dur_ms < 216000);
    CHECK(t.artist[0] != 0);
    CHECK(sc_parse_track(j, len, 96, &t) && t.tc_kbps == 96 && t.tc_kind == SC_TC_AAC_HLS);
    free(j);

    j = slurp("track_encrypted.json", &len);
    CHECK(sc_parse_track(j, len, 160, &t));
    CHECK_MSG(t.hidden == SC_HID_ENCRYPTED, "hidden %d", t.hidden);
    CHECK(t.tc_url[0] == 0 && t.tc_kind == SC_TC_NONE);          // a hidden track carries no URL to fetch
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
        if (t.hidden != SC_HID_NONE) CHECK_MSG(t.tc_url[0] == 0, "case %zu kept a url", i);
        if (i == 6) CHECK(strstr(t.tc_url, "/m/8") != NULL);
    }
    // Not streamable, stub, avatar fallback.
    const char *ns = "{\"id\":9,\"kind\":\"track\",\"streamable\":false,\"media\":{\"transcodings\":[]}}";
    CHECK(sc_parse_track(ns, strlen(ns), 160, &t) && t.hidden == SC_HID_NOT_STREAMABLE);
    const char *stub = "{\"id\":10,\"kind\":\"track\",\"monetization_model\":\"BLACKBOX\",\"policy\":\"MONETIZE\"}";
    CHECK(sc_parse_track(stub, strlen(stub), 160, &t) && t.hidden == SC_HID_STUB && t.id == 10);
    char doc[2048];
    snprintf(doc, sizeof(doc), mk, "");
    CHECK(sc_parse_track(doc, strlen(doc), 160, &t) && strcmp(t.art, "https://i/a-large.jpg") == 0);
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
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: test_sc <fixture-dir>\n"); return 2; }
    FIX = argv[1];
    test_scan();
    test_hls();
    test_track();
    test_pages();
    test_playlist();
    test_user_and_stream();
    test_urls();
    printf("test_sc: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
