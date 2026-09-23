// Host tests for the offline download core (source/offline).
//
// The download manager, store, record format and HTTP pieces compiled here
// are the SAME files the PS3 build compiles; only dl_platform.h is backed by
// tests/dl_fake.cpp -- a real temp directory and a scripted fake server.
//
//   make -f Makefile.host test_offline && ./test_offline [-v]

#include "dl_fake.h"
#include "dl_http.h"
#include "dl_manager.h"
#include "dl_model.h"
#include "dl_platform.h"
#include "dl_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unistd.h>   // truncate

static int s_checks = 0, s_failed = 0;
static const char *s_test = "";

#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("  FAIL %s:%d [%s]: %s\n", __FILE__, __LINE__, s_test, #cond); } } while (0)

static std::string s_tmp;    // per-test temp dir
static std::string s_root;   // the store root inside it
static DlConfig    s_cfg;

static void small_config(DlConfig *c) {
    dl_config_defaults(c);
    c->reserve_bytes     = 1000;
    c->head_timeout_ms   = 10000;
    c->idle_timeout_ms   = 5000;
    c->checkpoint_ms     = 1000000;   // byte-driven in tests
    c->checkpoint_bytes  = 50000;
    c->space_check_bytes = 60000;
    c->progress_ms       = 0;         // every read visible to the tests
}

static void begin(const char *name) {
    s_test = name;
    printf("- %s\n", name);
    fake_reset();
    if (!s_tmp.empty()) fake_rmtree(s_tmp);
    s_tmp  = fake_mkdtemp();
    s_root = s_tmp + "/offline";
    small_config(&s_cfg);
    dl_set_suspended(false);
    dl_playback_end();
    dl_set_auth_header("X-Emby-Authorization: MediaBrowser Token=\"t\"");
    CHECK(dl_manager_init(s_root.c_str(), &s_cfg));
}

// Re-open the same root, as a restart of the app would.
static void restart_app(void) {
    dl_manager_shutdown();
    CHECK(dl_manager_init(s_root.c_str(), &s_cfg));
}

static DlMeta meta_for(const char *id, const char *title = "A Film") {
    DlMeta m;
    dl_meta_init(&m);
    snprintf(m.id, sizeof(m.id), "%s", id);
    snprintf(m.title, sizeof(m.title), "%s", title);
    snprintf(m.type, sizeof(m.type), "Movie");
    snprintf(m.container, sizeof(m.container), "ts");
    return m;
}

static const char *URL = "http://192.168.1.2:8096/Videos/abc/stream.ts?VideoCodec=h264";

static DlStatus status_of(const char *id) {
    DlStatus st;
    memset(&st, 0, sizeof(st));
    if (!dl_find(id, &st)) st.rec.state = DL_STATE_COUNT;   // "absent"
    return st;
}

static std::string item_file(const char *id, const char *leaf) {
    char p[DL_PATH_MAX];
    dl_store_item_file(p, sizeof(p), id, leaf);
    return p;
}

// Run the worker until nothing is due (bounded).
static int run_worker(int max_steps = 50) {
    int steps = 0;
    while (steps < max_steps && dl_manager_step()) steps++;
    return steps;
}

// Skip past any backoff and run one more attempt.
static bool step_after_backoff(void) {
    g_fake_now_ms += 400000;
    return dl_manager_step();
}

static bool last_request_has_range(uint64_t from) {
    if (g_fake_requests.empty()) return false;
    char want[64];
    snprintf(want, sizeof(want), "Range: bytes=%llu-\r\n", (unsigned long long)from);
    return g_fake_requests.back().find(want) != std::string::npos;
}

static bool last_request_has_no_range(void) {
    return !g_fake_requests.empty() &&
           g_fake_requests.back().find("Range:") == std::string::npos;
}

// =========================================================================
// State machine
// =========================================================================

static void test_state_machine(void) {
    s_test = "state machine"; printf("- %s\n", s_test);
    struct { DlState from; DlEvent ev; DlState to; } legal[] = {
        { DL_QUEUED,      DL_EV_START,       DL_DOWNLOADING },
        { DL_QUEUED,      DL_EV_PAUSE,       DL_PAUSED },
        { DL_QUEUED,      DL_EV_CANCEL,      DL_CANCELLED },
        { DL_DOWNLOADING, DL_EV_PAUSE,       DL_PAUSED },
        { DL_DOWNLOADING, DL_EV_CANCEL,      DL_CANCELLED },
        { DL_DOWNLOADING, DL_EV_COMPLETE,    DL_COMPLETED },
        { DL_DOWNLOADING, DL_EV_FAIL_RETRY,  DL_QUEUED },
        { DL_DOWNLOADING, DL_EV_INTERRUPTED, DL_QUEUED },
        { DL_DOWNLOADING, DL_EV_FAIL,        DL_FAILED },
        { DL_PAUSED,      DL_EV_RESUME,      DL_QUEUED },
        { DL_PAUSED,      DL_EV_CANCEL,      DL_CANCELLED },
        { DL_FAILED,      DL_EV_RETRY,       DL_QUEUED },
        { DL_FAILED,      DL_EV_CANCEL,      DL_CANCELLED },
        { DL_CANCELLED,   DL_EV_RETRY,       DL_QUEUED },
    };
    const int nlegal = (int)(sizeof(legal) / sizeof(legal[0]));
    for (int i = 0; i < nlegal; i++) {
        DlState out = DL_STATE_COUNT;
        CHECK(dl_next_state(legal[i].from, legal[i].ev, &out));
        CHECK(out == legal[i].to);
    }
    // Everything not in the table is refused and leaves *out untouched.
    int refused = 0;
    for (int s = 0; s < DL_STATE_COUNT; s++) {
        for (int e = DL_EV_START; e <= DL_EV_INTERRUPTED; e++) {
            bool is_legal = false;
            for (int i = 0; i < nlegal; i++)
                if (legal[i].from == s && legal[i].ev == e) is_legal = true;
            if (is_legal) continue;
            DlState out = DL_STATE_COUNT;
            CHECK(!dl_next_state((DlState)s, (DlEvent)e, &out));
            CHECK(out == DL_STATE_COUNT);
            refused++;
        }
    }
    CHECK(refused == DL_STATE_COUNT * 9 - nlegal);
    // COMPLETED has no way out at all.
    for (int e = DL_EV_START; e <= DL_EV_INTERRUPTED; e++) {
        DlState out;
        CHECK(!dl_next_state(DL_COMPLETED, (DlEvent)e, &out));
    }
    // Names round-trip.
    for (int s = 0; s < DL_STATE_COUNT; s++) {
        DlState back;
        CHECK(dl_state_from_name(dl_state_name((DlState)s), &back) && back == s);
    }
    for (int e = 0; e < DL_ERR_COUNT; e++) {
        DlError back;
        CHECK(dl_error_from_name(dl_error_name((DlError)e), &back) && back == e);
    }
    DlState dummy;
    CHECK(!dl_state_from_name("Downloading", &dummy));   // case-sensitive
    CHECK(!dl_state_from_name("", &dummy));
}

static void test_error_policy(void) {
    s_test = "error policy"; printf("- %s\n", s_test);
    CHECK(dl_error_for_http_status(200) == DL_ERR_NONE);
    CHECK(dl_error_for_http_status(206) == DL_ERR_NONE);
    CHECK(dl_error_for_http_status(401) == DL_ERR_AUTH);
    CHECK(dl_error_for_http_status(403) == DL_ERR_AUTH);
    CHECK(dl_error_for_http_status(404) == DL_ERR_NOT_FOUND);
    CHECK(dl_error_for_http_status(410) == DL_ERR_NOT_FOUND);
    CHECK(dl_error_for_http_status(400) == DL_ERR_HTTP);
    CHECK(dl_error_for_http_status(429) == DL_ERR_SERVER);
    CHECK(dl_error_for_http_status(500) == DL_ERR_SERVER);
    CHECK(dl_error_for_http_status(503) == DL_ERR_SERVER);
    CHECK(dl_error_for_http_status(302) == DL_ERR_BAD_RESPONSE);
    CHECK(dl_error_retryable(DL_ERR_UNREACHABLE) && dl_error_retryable(DL_ERR_TIMEOUT));
    CHECK(dl_error_retryable(DL_ERR_PARTIAL) && dl_error_retryable(DL_ERR_SERVER));
    CHECK(!dl_error_retryable(DL_ERR_AUTH) && !dl_error_retryable(DL_ERR_NO_SPACE));
    CHECK(!dl_error_retryable(DL_ERR_NOT_FOUND) && !dl_error_retryable(DL_ERR_DISK));
    CHECK(dl_backoff_ms(0) == 0);
    CHECK(dl_backoff_ms(1) == 2000);
    CHECK(dl_backoff_ms(2) == 4000);
    CHECK(dl_backoff_ms(3) == 8000);
    CHECK(dl_backoff_ms(8) == 256000);
    CHECK(dl_backoff_ms(9) == 300000);
    CHECK(dl_backoff_ms(4000000000u) == 300000);
    for (int e = 1; e < DL_ERR_COUNT; e++) CHECK(dl_error_text((DlError)e)[0] != '\0');
}

static void test_progress_math(void) {
    s_test = "progress math"; printf("- %s\n", s_test);
    DlRecord r;
    dl_record_init(&r);
    CHECK(dl_progress_permille(&r) == -1);            // total unknown
    r.bytes_total = 1000; r.bytes_done = 0;   CHECK(dl_progress_permille(&r) == 0);
    r.bytes_done = 820;                       CHECK(dl_progress_permille(&r) == 820);
    r.bytes_done = 1000;                      CHECK(dl_progress_permille(&r) == 1000);
    r.bytes_total = 40ull << 30; r.bytes_done = 10ull << 30;   // 40 GB film
    CHECK(dl_progress_permille(&r) == 250);
    r.bytes_total = UINT64_MAX; r.bytes_done = UINT64_MAX / 2;
    int p = dl_progress_permille(&r);
    CHECK(p >= 499 && p <= 500);
    r.state = DL_COMPLETED; r.bytes_total = 0;
    CHECK(dl_progress_permille(&r) == 1000);
}

static void test_id_validation(void) {
    s_test = "id validation"; printf("- %s\n", s_test);
    CHECK(dl_id_valid("0123456789abcdef0123456789abcdef"));
    CHECK(dl_id_valid("a-b_C"));
    CHECK(!dl_id_valid(""));
    CHECK(!dl_id_valid("../etc"));
    CHECK(!dl_id_valid("a/b"));
    CHECK(!dl_id_valid("a.b"));
    CHECK(!dl_id_valid("a b"));
    std::string longid(DL_ID_MAX, 'a');
    CHECK(!dl_id_valid(longid.c_str()));
    CHECK(dl_id_valid(longid.substr(0, DL_ID_MAX - 1).c_str()));
}

// =========================================================================
// Persistence format
// =========================================================================

static void test_record_roundtrip(void) {
    s_test = "record round-trip"; printf("- %s\n", s_test);
    DlRecord r;
    dl_record_init(&r);
    snprintf(r.id, sizeof(r.id), "abc123");
    snprintf(r.title, sizeof(r.title), "Line one\nkey=forged\\ \r end");
    snprintf(r.url, sizeof(r.url), "%s", URL);
    r.state = DL_PAUSED; r.error = DL_ERR_PARTIAL; r.http_status = 206;
    r.seq = 42; r.attempts = 3;
    r.bytes_done = 5000000000ull; r.bytes_total = 9000000000ull; r.resumable = 1;
    char text[4096];
    int n = dl_record_format(&r, text, sizeof(text));
    CHECK(n > 0 && (int)strlen(text) == n);
    CHECK(strstr(text, "state=paused\n") != NULL);          // names, not digits
    CHECK(strstr(text, "\nkey=forged") == NULL);             // newline escaped
    DlRecord b;
    CHECK(dl_record_parse(text, &b));
    CHECK(strcmp(b.title, r.title) == 0);
    CHECK(strcmp(b.url, r.url) == 0);
    CHECK(b.state == DL_PAUSED && b.error == DL_ERR_PARTIAL && b.http_status == 206);
    CHECK(b.seq == 42 && b.attempts == 3 && b.resumable == 1);
    CHECK(b.bytes_done == 5000000000ull && b.bytes_total == 9000000000ull);
    // Too small a buffer fails cleanly instead of truncating.
    char tiny[40];
    CHECK(dl_record_format(&r, tiny, sizeof(tiny)) == -1);
    // CRLF line endings (a file edited on Windows over FTP) still load.
    std::string crlf;
    for (const char *p = text; *p; p++) { if (*p == '\n') crlf += "\r\n"; else crlf += *p; }
    // ...except the title had a real \r escaped as "\r", which stays intact.
    CHECK(dl_record_parse(crlf.c_str(), &b));
    CHECK(b.bytes_done == 5000000000ull);
}

static void test_record_malformed(void) {
    s_test = "malformed records"; printf("- %s\n", s_test);
    DlRecord b;
    const char *bad[] = {
        "",                                                         // empty
        "garbage",                                                  // no magic
        "jfdl-state 2\nid=a\nstate=queued\nend\n",                 // future version
        "jfdl-state 1\nid=a\nstate=queued\n",                      // torn: no end
        "jfdl-state 1\nstate=queued\nend\n",                       // no id
        "jfdl-state 1\nid=a\nend\n",                               // no state
        "jfdl-state 1\nid=a\nstate=exploded\nend\n",               // unknown state
        "jfdl-state 1\nid=../../x\nstate=queued\nend\n",           // traversal
        "jfdl-state 1\nid=a\nstate=queued\nbytes_done=12x\nend\n", // bad number
        "jfdl-state 1\nid=a\nstate=queued\nbytes_done=-1\nend\n",  // negative
        "jfdl-state 1\nid=a\nstate=queued\nseq=99999999999\nend\n",// overflow
        "jfdl-state 1\nid=a\nstate=queued\nbytes_done=10\nbytes_total=5\nend\n",
        "jfdl-state 1\nid=a\nstate=queued\nerror=melted\nend\n",
        "jfdl-state 1\nid=a\nstate=queued\nno equals sign\nend\n",
        "jfdl-state 1\nid=a\nstate=queued\n=value\nend\n",
        "jfdl-state 1\nid=a\nstate=queued\nresumable=2\nend\n",
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        bool parsed = dl_record_parse(bad[i], &b);
        if (parsed) printf("    accepted #%u: %s\n", i, bad[i]);
        CHECK(!parsed);
    }
    // Unknown keys are tolerated (a newer build's file).
    CHECK(dl_record_parse("jfdl-state 1\nid=a\nstate=queued\nfuture_key=1\nend\n", &b));
    // An over-long url is refused, not truncated into a different request.
    std::string longurl = "jfdl-state 1\nid=a\nstate=queued\nurl=http://x/" +
                          std::string(DL_URL_MAX, 'q') + "\nend\n";
    CHECK(!dl_record_parse(longurl.c_str(), &b));
}

static void test_meta_roundtrip(void) {
    s_test = "metadata round-trip"; printf("- %s\n", s_test);
    DlMeta m = meta_for("ep01", "Pilot");
    snprintf(m.type, sizeof(m.type), "Episode");
    snprintf(m.series, sizeof(m.series), "Some Show");
    snprintf(m.series_id, sizeof(m.series_id), "show9");
    m.season = 1; m.episode = 3; m.year = 2008; m.runtime_secs = 2820;
    snprintf(m.overview, sizeof(m.overview), "Two lines\nof plot = spoilers");
    snprintf(m.video_codec, sizeof(m.video_codec), "h264");
    snprintf(m.audio_codec, sizeof(m.audio_codec), "ac3");
    m.width = 1920; m.height = 1080; m.audio_channels = 6; m.video_bitrate = 10000000;
    snprintf(m.media_source_id, sizeof(m.media_source_id), "src-a");
    m.audio_stream_index = 2;
    snprintf(m.quality, sizeof(m.quality), "High");
    snprintf(m.video_info, sizeof(m.video_info), "1080p H264 SDR");
    snprintf(m.audio_info, sizeof(m.audio_info), "English AC3 5.1");
    snprintf(m.poster, sizeof(m.poster), "poster.jpg");
    char text[4096];
    CHECK(dl_meta_format(&m, text, sizeof(text)) > 0);
    DlMeta b;
    CHECK(dl_meta_parse(text, &b));
    CHECK(strcmp(b.series, "Some Show") == 0 && b.season == 1 && b.episode == 3);
    CHECK(b.runtime_secs == 2820 && b.year == 2008);
    CHECK(strcmp(b.overview, m.overview) == 0);
    CHECK(b.width == 1920 && b.height == 1080 && b.audio_channels == 6);
    CHECK(b.video_bitrate == 10000000 && b.audio_stream_index == 2);
    CHECK(strcmp(b.poster, "poster.jpg") == 0 && b.backdrop[0] == '\0');
    // Defaults for a movie (no season/episode) survive a round trip.
    DlMeta mv = meta_for("mov1");
    CHECK(dl_meta_format(&mv, text, sizeof(text)) > 0 && dl_meta_parse(text, &b));
    CHECK(b.season == -1 && b.episode == -1 && b.audio_stream_index == -1);
}

static void test_meta_malformed(void) {
    s_test = "malformed metadata"; printf("- %s\n", s_test);
    DlMeta b;
    const char *bad[] = {
        "jfdl-meta 1\nid=a\nend\n",                               // no title
        "jfdl-meta 1\ntitle=x\nend\n",                            // no id
        "jfdl-meta 1\nid=a\ntitle=\nend\n",                       // empty title
        "jfdl-meta 1\nid=a\ntitle=x\n",                           // torn
        "jfdl-meta 1\nid=a\ntitle=x\nseason=one\nend\n",          // bad int
        "jfdl-meta 1\nid=a\ntitle=x\nposter=../../boot.jpg\nend\n", // traversal
        "jfdl-meta 1\nid=a\ntitle=x\nposter=/dev_flash/x\nend\n",
        "jfdl-meta 1\nid=a\ntitle=x\nbackdrop=a.b.c\nend\n",
        "jfdl-state 1\nid=a\ntitle=x\nend\n",                     // wrong kind
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        bool parsed = dl_meta_parse(bad[i], &b);
        if (parsed) printf("    accepted #%u\n", i);
        CHECK(!parsed);
    }
}

// =========================================================================
// HTTP pieces
// =========================================================================

static void test_url_parse(void) {
    s_test = "url parse"; printf("- %s\n", s_test);
    DlUrl u;
    CHECK(dl_url_parse("http://192.168.1.2:8096/Videos/x/stream.ts?a=1", &u));
    CHECK(strcmp(u.host, "192.168.1.2") == 0 && u.port == 8096);
    CHECK(strcmp(u.path, "/Videos/x/stream.ts?a=1") == 0);
    CHECK(dl_url_parse("http://jelly.local/", &u) && u.port == 8096);  // app default
    CHECK(dl_url_parse("http://h:80", &u) && u.port == 80 && strcmp(u.path, "/") == 0);
    CHECK(dl_url_parse("http://h?x=1", &u) && strcmp(u.path, "/?x=1") == 0);
    CHECK(!dl_url_parse("https://h/x", &u));          // no TLS on this path
    CHECK(!dl_url_parse("ftp://h/x", &u));
    CHECK(!dl_url_parse("http://", &u));
    CHECK(!dl_url_parse("http://h:0/", &u));
    CHECK(!dl_url_parse("http://h:65536/", &u));
    CHECK(!dl_url_parse("http://h:12a/", &u));
    CHECK(!dl_url_parse("http://h:/", &u));
    std::string longhost = "http://" + std::string(4096, 'x') + ".com/path";
    CHECK(!dl_url_parse(longhost.c_str(), &u));        // bounded, not overflowed
    std::string longpath = "http://h/" + std::string(DL_URL_MAX, 'p');
    CHECK(!dl_url_parse(longpath.c_str(), &u));
}

static void test_request_build(void) {
    s_test = "request build"; printf("- %s\n", s_test);
    DlUrl u;
    CHECK(dl_url_parse("http://10.0.0.5:8096/Videos/a/stream.ts?x=1", &u));
    char req[2048];
    int n = dl_http_build_get(req, sizeof(req), &u, 0, "X-Emby-Authorization: tok");
    CHECK(n > 0);
    const char *line1 = "GET /Videos/a/stream.ts?x=1 HTTP/1.1\r\n";
    CHECK(strncmp(req, line1, strlen(line1)) == 0);
    CHECK(strstr(req, "Host: 10.0.0.5:8096\r\n") != NULL);
    CHECK(strstr(req, "X-Emby-Authorization: tok\r\n") != NULL);
    CHECK(strstr(req, "Range:") == NULL);
    CHECK(strcmp(req + n - 4, "\r\n\r\n") == 0);
    n = dl_http_build_get(req, sizeof(req), &u, 5000000000ull, NULL);
    CHECK(strstr(req, "Range: bytes=5000000000-\r\n") != NULL);
    CHECK(strstr(req, "Authorization") == NULL);
    CHECK(dl_http_build_get(req, 32, &u, 0, NULL) == -1);
}

static bool head(const char *t, DlHttpHead *h) { return dl_http_parse_head(t, (int)strlen(t), h); }

static void test_head_parse(void) {
    s_test = "response head parse"; printf("- %s\n", s_test);
    DlHttpHead h;
    CHECK(head("HTTP/1.1 200 OK\r\nContent-Length: 1234\r\nContent-Type: Video/MP2T\r\n"
               "Accept-Ranges: bytes\r\n\r\n", &h));
    CHECK(h.status == 200 && h.content_length == 1234 && h.accept_ranges && !h.chunked);
    CHECK(strcmp(h.content_type, "video/mp2t") == 0);
    CHECK(head("HTTP/1.1 206 Partial\r\ncontent-range: bytes 100-199/1000\r\n"
               "CONTENT-LENGTH: 100\r\n\r\n", &h));
    CHECK(h.status == 206 && h.has_range && h.range_start == 100 && h.range_end == 199);
    CHECK(h.range_total == 1000 && h.content_length == 100);
    CHECK(head("HTTP/1.1 206 X\r\nContent-Range: bytes 0-9/*\r\n\r\n", &h));
    CHECK(h.range_total == -1);
    CHECK(head("HTTP/1.1 416 X\r\nContent-Range: bytes */5000\r\n\r\n", &h));
    CHECK(h.range_unsatisfied && h.range_total == 5000);
    // Chunked beats Content-Length (RFC 7230 3.3.3).
    CHECK(head("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n", &h));
    CHECK(h.chunked && h.content_length == -1);
    CHECK(head("HTTP/1.0 200\r\n\r\n", &h) && h.status == 200);
    // Malformed
    CHECK(!head("", &h));
    CHECK(!head("ICY 200 OK\r\n\r\n", &h));
    CHECK(!head("HTTP/1.1 2x0 OK\r\n\r\n", &h));
    CHECK(!head("HTTP/1.1 2000 OK\r\n\r\n", &h));
    CHECK(!head("HTTP/1.1 200 OK\r\nContent-Length: abc\r\n\r\n", &h));
    CHECK(!head("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n", &h));
    CHECK(!head("HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999999\r\n\r\n", &h));
    CHECK(!head("HTTP/1.1 206 X\r\nContent-Range: bytes 9-3/10\r\n\r\n", &h));
    CHECK(!head("HTTP/1.1 206 X\r\nContent-Range: bytes 0-10/10\r\n\r\n", &h));
    CHECK(!head("HTTP/1.1 206 X\r\nContent-Range: pages 0-1/10\r\n\r\n", &h));
    CHECK(!head("HTTP/1.1 206 X\r\nContent-Range: bytes */*\r\n\r\n", &h));
    // Duplicate identical Content-Length is legal.
    CHECK(head("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n", &h));
    CHECK(dl_http_content_type_is_error_page("text/html; charset=utf-8"));
    CHECK(dl_http_content_type_is_error_page("application/json"));
    CHECK(!dl_http_content_type_is_error_page("video/mp2t"));
    CHECK(!dl_http_content_type_is_error_page(""));
}

static void test_head_reader(void) {
    s_test = "head reader splits"; printf("- %s\n", s_test);
    const char *resp = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nBODY";
    const int len = (int)strlen(resp);
    const int head_len = len - 4;
    // Every split point and every read size delivers the same head.
    for (int step = 1; step <= len; step++) {
        DlHeadReader r;
        dl_head_reader_init(&r);
        int pos = 0, body_at = -1;
        while (pos < len && !r.done) {
            int n = step < len - pos ? step : len - pos;
            int used = dl_head_reader_feed(&r, (const uint8_t *)resp + pos, n);
            CHECK(used >= 0);
            pos += used;
            if (r.done) body_at = pos;
        }
        CHECK(r.done && body_at == head_len && r.n == head_len);
    }
    // A head that never ends is refused at DL_HEAD_MAX.
    static DlHeadReader r;
    dl_head_reader_init(&r);
    std::string junk(DL_HEAD_MAX + 10, 'a');
    CHECK(dl_head_reader_feed(&r, (const uint8_t *)junk.data(), (int)junk.size()) == -1);
}

static std::string make_chunked(const std::string &body, unsigned seed, bool ext) {
    std::string out;
    size_t i = 0;
    while (i < body.size()) {
        seed = seed * 1103515245u + 12345u;
        size_t n = 1 + (seed >> 16) % 300;
        if (n > body.size() - i) n = body.size() - i;
        char line[48];
        snprintf(line, sizeof(line), (seed & 1) ? "%zX" : "%zx", n);
        out += line;
        if (ext) out += ";name=val";
        out += "\r\n";
        out.append(body, i, n);
        out += "\r\n";
        i += n;
    }
    out += "0\r\nX-Trailer: 1\r\n\r\nGARBAGE-AFTER-END";
    return out;
}

static void test_chunked(void) {
    s_test = "chunked decoder"; printf("- %s\n", s_test);
    std::string body;
    for (int i = 0; i < 20000; i++) body.push_back((char)fake_byte((uint64_t)i));
    for (unsigned seed = 1; seed <= 40; seed++) {
        std::string enc = make_chunked(body, seed, seed % 3 == 0);
        // Decode in place with random read sizes.
        std::vector<uint8_t> buf(enc.begin(), enc.end());
        DlChunked c;
        dl_chunked_init(&c);
        std::string got;
        size_t pos = 0;
        unsigned rs = seed * 7;
        while (pos < buf.size() && !c.done) {
            rs = rs * 1664525u + 1013904223u;
            int n = (int)(1 + (rs >> 20) % 97);
            if ((size_t)n > buf.size() - pos) n = (int)(buf.size() - pos);
            int out = dl_chunked_decode(&c, buf.data() + pos, n, buf.data() + pos);
            CHECK(out >= 0);
            if (out < 0) break;
            got.append((const char *)buf.data() + pos, (size_t)out);
            pos += (size_t)n;
        }
        CHECK(c.done && !c.error);
        CHECK(got == body);
    }
    // Byte-at-a-time also works.
    {
        std::string enc = make_chunked(body.substr(0, 3000), 99, true);
        DlChunked c; dl_chunked_init(&c);
        std::string got;
        for (size_t i = 0; i < enc.size() && !c.done; i++) {
            uint8_t b = (uint8_t)enc[i], o = 0;
            int n = dl_chunked_decode(&c, &b, 1, &o);
            CHECK(n >= 0);
            if (n == 1) got.push_back((char)o);
        }
        CHECK(c.done && got == body.substr(0, 3000));
    }
    // Malformed framing is refused.
    const char *bad[] = { "zz\r\nhello\r\n", "\r\n", "5\r\nhelloXX", "5\rhello",
                          "ffffffffffffffffff\r\n" };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        DlChunked c; dl_chunked_init(&c);
        uint8_t out[64];
        int r = dl_chunked_decode(&c, (const uint8_t *)bad[i], (int)strlen(bad[i]), out);
        CHECK(r < 0 && c.error);
        CHECK(dl_chunked_decode(&c, (const uint8_t *)"0\r\n\r\n", 5, out) < 0);   // sticky
    }
}

// =========================================================================
// Manager: queue, transfer, persistence
// =========================================================================

static void test_enqueue_and_complete(void) {
    begin("enqueue -> download -> complete");
    DlMeta m = meta_for("item1", "The Film");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    DlStatus st = status_of("item1");
    CHECK(st.rec.state == DL_QUEUED && st.rec.seq == 1);
    // Persisted before any network activity.
    DlRecord disk;
    CHECK(dl_store_load_record("item1", &disk) && disk.state == DL_QUEUED);
    DlMeta dm;
    CHECK(dl_store_load_meta("item1", &dm) && strcmp(dm.title, "The Film") == 0);
    CHECK(strstr(disk.url, "Token") == NULL);

    CHECK(run_worker() == 1);
    st = status_of("item1");
    CHECK(st.rec.state == DL_COMPLETED && st.rec.error == DL_ERR_NONE);
    CHECK(st.rec.bytes_done == (uint64_t)g_fake_total && st.rec.bytes_total == (uint64_t)g_fake_total);
    CHECK(dl_progress_permille(&st.rec) == 1000);
    CHECK(fake_file_matches(item_file("item1", DL_FILE_MEDIA).c_str(), g_fake_total));
    CHECK(dl_plat_file_size(item_file("item1", DL_FILE_PART).c_str()) == -1);
    CHECK(dl_store_load_record("item1", &disk) && disk.state == DL_COMPLETED);
    // Token went in the header, never in the URL or the record.
    CHECK(g_fake_requests.size() == 1);
    CHECK(g_fake_requests[0].find("X-Emby-Authorization: MediaBrowser Token=\"t\"\r\n")
          != std::string::npos);
    CHECK(last_request_has_no_range());
    char path[DL_PATH_MAX];
    CHECK(dl_media_path("item1", path, sizeof(path)));
    CHECK(strstr(path, "/items/item1/media.ts") != NULL);
    CHECK(run_worker() == 0);   // nothing left to do
}

static void test_queue_order_and_duplicates(void) {
    begin("queue order, duplicates, limits");
    DlMeta a = meta_for("aaa", "A"), b = meta_for("bbb", "B"), c = meta_for("ccc", "C");
    CHECK(dl_enqueue(&b, URL, 0) == DL_OK);
    CHECK(dl_enqueue(&a, URL, 0) == DL_OK);
    CHECK(dl_enqueue(&c, URL, 0) == DL_OK);
    CHECK(dl_enqueue(&a, URL, 0) == DL_E_EXISTS);
    DlStatus list[8];
    CHECK(dl_list(list, 8, false) == 3);
    CHECK(!strcmp(list[0].rec.id, "bbb") && !strcmp(list[1].rec.id, "aaa") &&
          !strcmp(list[2].rec.id, "ccc"));
    CHECK(dl_list(list, 2, false) == 2);            // honours max
    CHECK(dl_list(list, 8, true) == 0);             // nothing complete yet
    CHECK(dl_manager_step());
    CHECK(status_of("bbb").rec.state == DL_COMPLETED);   // first in, first done
    CHECK(status_of("aaa").rec.state == DL_QUEUED);
    CHECK(dl_list(list, 8, true) == 1 && !strcmp(list[0].rec.id, "bbb"));
    CHECK(dl_enqueue(&b, URL, 0) == DL_E_EXISTS);   // already downloaded
    // Invalid input
    DlMeta bad = meta_for("../x");
    CHECK(dl_enqueue(&bad, URL, 0) == DL_E_INVALID);
    DlMeta notitle = meta_for("ok1", "");
    CHECK(dl_enqueue(&notitle, URL, 0) == DL_E_INVALID);
    DlMeta ok = meta_for("ok2");
    CHECK(dl_enqueue(&ok, "https://h/x", 0) == DL_E_INVALID);
    CHECK(dl_enqueue(&ok, "not a url", 0) == DL_E_INVALID);
    CHECK(dl_enqueue(NULL, URL, 0) == DL_E_INVALID);
    CHECK(dl_pause("nope") == DL_E_NOT_FOUND);
    // Table capacity
    int added = 3;
    for (int i = 0; added < DL_MAX_ITEMS; i++, added++) {
        char id[16]; snprintf(id, sizeof(id), "fill%d", i);
        DlMeta f = meta_for(id);
        CHECK(dl_enqueue(&f, URL, 0) == DL_OK);
    }
    DlMeta extra = meta_for("onetoomany");
    CHECK(dl_enqueue(&extra, URL, 0) == DL_E_FULL);
    CHECK(dl_remove("fill0") == DL_OK);
    CHECK(dl_enqueue(&extra, URL, 0) == DL_OK);     // room again
}

static int64_t s_hook_at = -1;
static const char *s_hook_id = "";
static int s_hook_action = 0;   // 1 pause, 2 cancel, 3 remove, 4 suspend, 5 cancel+pause
static void hook(int64_t body_sent) {
    if (s_hook_at < 0 || body_sent < s_hook_at) return;
    s_hook_at = -1;
    switch (s_hook_action) {
    case 1: CHECK(dl_pause(s_hook_id) == DL_OK); break;
    case 2: CHECK(dl_cancel(s_hook_id) == DL_OK); break;
    case 3: CHECK(dl_remove(s_hook_id) == DL_OK); break;
    case 4: dl_set_suspended(true); break;
    case 5: CHECK(dl_cancel(s_hook_id) == DL_OK); CHECK(dl_pause(s_hook_id) == DL_OK); break;
    case 6: CHECK(dl_pause(s_hook_id) == DL_OK); CHECK(dl_resume(s_hook_id) == DL_OK); break;
    }
}

static void arm_hook(const char *id, int action, int64_t at) {
    s_hook_id = id; s_hook_action = action; s_hook_at = at;
    g_fake_default.on_body = hook;
}

static void test_progress_checkpoints(void) {
    begin("progress + checkpoints");
    // Watch progress as the worker sees it, from inside the transfer.
    static std::vector<uint64_t> seen_mem, seen_disk;
    seen_mem.clear(); seen_disk.clear();
    g_fake_default.on_body = [](int64_t) {
        DlStatus st;
        if (dl_find("prog", &st)) seen_mem.push_back(st.rec.bytes_done);
        DlRecord d;
        if (dl_store_load_record("prog", &d)) seen_disk.push_back(d.bytes_done);
    };
    DlMeta m = meta_for("prog");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(status_of("prog").rec.state == DL_COMPLETED);
    CHECK(seen_mem.size() > 10);
    bool mono = true;
    for (size_t i = 1; i < seen_mem.size(); i++) if (seen_mem[i] < seen_mem[i - 1]) mono = false;
    CHECK(mono);
    // Disk lags memory, but only by one checkpoint interval.
    bool bounded = true;
    for (size_t i = 0; i < seen_mem.size() && i < seen_disk.size(); i++) {
        if (seen_disk[i] > seen_mem[i]) bounded = false;
        if (seen_mem[i] - seen_disk[i] > s_cfg.checkpoint_bytes + 7919) bounded = false;
    }
    CHECK(bounded);
    int distinct = 0;
    for (size_t i = 1; i < seen_disk.size(); i++) if (seen_disk[i] != seen_disk[i - 1]) distinct++;
    CHECK(distinct >= 4);   // 300 KB / 50 KB checkpoints
}

static void test_interrupted_resume(void) {
    begin("interrupted download resumes with Range");
    FakeResp drop; drop.drop_after = 123457;
    g_fake_queue.push_back(drop);
    DlMeta m = meta_for("res1");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    DlStatus st = status_of("res1");
    CHECK(st.rec.state == DL_QUEUED && st.rec.error == DL_ERR_PARTIAL);
    CHECK(st.rec.attempts == 1 && st.retry_in_ms == 2000);
    CHECK(st.rec.bytes_done == 123457 && st.rec.resumable == 1);
    CHECK(dl_plat_file_size(item_file("res1", DL_FILE_PART).c_str()) == 123457);
    // Not due yet: the worker leaves it alone during the backoff.
    CHECK(!dl_manager_step());
    CHECK(dl_next_wake_ms() == 1000);             // capped, so control is noticed
    g_fake_now_ms += 2000;
    CHECK(dl_next_wake_ms() == 0);
    CHECK(dl_manager_step());
    CHECK(last_request_has_range(123457));
    st = status_of("res1");
    CHECK(st.rec.state == DL_COMPLETED && st.rec.attempts == 0);
    CHECK(fake_file_matches(item_file("res1", DL_FILE_MEDIA).c_str(), g_fake_total));
}

static void test_reset_and_timeout(void) {
    begin("connection reset and idle timeout");
    FakeResp reset; reset.reset_after = 50000;
    FakeResp stall; stall.stall_after = 90000;
    g_fake_queue.push_back(reset);
    g_fake_queue.push_back(stall);
    DlMeta m = meta_for("rt");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(status_of("rt").rec.error == DL_ERR_NETWORK);
    CHECK(status_of("rt").rec.bytes_done == 50000);
    uint64_t t0 = g_fake_now_ms;
    CHECK(step_after_backoff());
    DlStatus st = status_of("rt");
    CHECK(st.rec.error == DL_ERR_TIMEOUT && st.rec.state == DL_QUEUED);
    CHECK(st.rec.bytes_done == 50000 + 90000);    // limits count per response
    CHECK(g_fake_now_ms - t0 >= 400000 + s_cfg.idle_timeout_ms);
    CHECK(step_after_backoff());
    CHECK(last_request_has_range(140000));
    CHECK(status_of("rt").rec.state == DL_COMPLETED);
    CHECK(fake_file_matches(item_file("rt", DL_FILE_MEDIA).c_str(), g_fake_total));
}

static void test_head_timeout(void) {
    begin("server never answers");
    FakeResp s; s.head_stall = true;
    g_fake_queue.push_back(s);
    DlMeta m = meta_for("ht");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    uint64_t t0 = g_fake_now_ms;
    CHECK(dl_manager_step());
    CHECK(status_of("ht").rec.error == DL_ERR_TIMEOUT);
    CHECK(g_fake_now_ms - t0 >= s_cfg.head_timeout_ms);
    CHECK(g_fake_now_ms - t0 < s_cfg.head_timeout_ms + 5000);   // bounded
}

static void test_range_ignored(void) {
    begin("server ignores Range (live transcode)");
    FakeResp drop; drop.drop_after = 100000; drop.accept_ranges = false;
    FakeResp full; full.honor_range = false; full.accept_ranges = false;
    g_fake_queue.push_back(drop);
    g_fake_queue.push_back(full);
    DlMeta m = meta_for("tr");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(status_of("tr").rec.bytes_done == 100000);
    CHECK(status_of("tr").rec.resumable == 0);
    CHECK(step_after_backoff());
    CHECK(last_request_has_range(100000));        // asked...
    DlStatus st = status_of("tr");                 // ...got 200, started over
    CHECK(st.rec.state == DL_COMPLETED);
    CHECK(fake_file_matches(item_file("tr", DL_FILE_MEDIA).c_str(), g_fake_total));
}

static void test_range_inconsistent(void) {
    begin("206 from the wrong offset / file changed");
    FakeResp drop; drop.drop_after = 100000;
    FakeResp lie;  lie.range_start_lie = 5;
    g_fake_queue.push_back(drop);
    g_fake_queue.push_back(lie);
    DlMeta m = meta_for("ri");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(step_after_backoff());
    DlStatus st = status_of("ri");
    CHECK(st.rec.error == DL_ERR_BAD_RESPONSE && st.rec.bytes_done == 0);
    CHECK(dl_plat_file_size(item_file("ri", DL_FILE_PART).c_str()) == 0);   // discarded
    CHECK(step_after_backoff());
    CHECK(last_request_has_no_range());
    CHECK(status_of("ri").rec.state == DL_COMPLETED);
    CHECK(fake_file_matches(item_file("ri", DL_FILE_MEDIA).c_str(), g_fake_total));

    // The file on the server changed size between attempts.
    begin("206 with a different total");
    FakeResp d2; d2.drop_after = 70000;
    FakeResp grown; grown.total_lie = 999999;
    g_fake_queue.push_back(d2);
    g_fake_queue.push_back(grown);
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(status_of("ri").rec.bytes_total == (uint64_t)g_fake_total);
    CHECK(step_after_backoff());
    CHECK(status_of("ri").rec.error == DL_ERR_BAD_RESPONSE);
    CHECK(status_of("ri").rec.bytes_done == 0);
}

static void test_416_already_complete(void) {
    begin("416 when everything is already here");
    // Crash after the last byte was written but before verify/rename: the
    // partial is complete, and the next request asks for bytes past the end.
    DlMeta m = meta_for("full");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    FILE *f = fopen(item_file("full", DL_FILE_PART).c_str(), "wb");
    for (int64_t i = 0; i < g_fake_total; i++) fputc(fake_byte((uint64_t)i), f);
    fclose(f);
    CHECK(dl_manager_step());
    CHECK(last_request_has_range((uint64_t)g_fake_total));
    CHECK(status_of("full").rec.state == DL_COMPLETED);
    CHECK(fake_file_matches(item_file("full", DL_FILE_MEDIA).c_str(), g_fake_total));

    // 416 that does NOT match what we have: start over.
    begin("416 that does not match");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    f = fopen(item_file("full", DL_FILE_PART).c_str(), "wb");
    for (int64_t i = 0; i < g_fake_total + 10; i++) fputc(fake_byte((uint64_t)i), f);
    fclose(f);
    CHECK(dl_manager_step());
    CHECK(status_of("full").rec.error == DL_ERR_BAD_RESPONSE);
    CHECK(status_of("full").rec.http_status == 416);
    CHECK(step_after_backoff());
    CHECK(status_of("full").rec.state == DL_COMPLETED);
    CHECK(fake_file_matches(item_file("full", DL_FILE_MEDIA).c_str(), g_fake_total));
}

static void test_chunked_transcode(void) {
    begin("chunked transcode body");
    g_fake_default.chunked = true;
    g_fake_default.accept_ranges = false;
    g_fake_default.honor_range = false;
    g_fake_total = 1500000;
    DlMeta m = meta_for("chk");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    DlStatus st = status_of("chk");
    CHECK(st.rec.state == DL_COMPLETED);
    CHECK(st.rec.bytes_total == 1500000);       // learned at the end
    CHECK(fake_file_matches(item_file("chk", DL_FILE_MEDIA).c_str(), 1500000));

    begin("chunked body cut short");
    g_fake_default.chunked = true;
    g_fake_default.honor_range = false;
    g_fake_default.accept_ranges = false;
    FakeResp cut = g_fake_default; cut.drop_after = 40000;
    g_fake_queue.push_back(cut);
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    st = status_of("chk");
    CHECK(st.rec.error == DL_ERR_PARTIAL && st.rec.state == DL_QUEUED);
    CHECK(st.rec.bytes_total == 0);             // a transcode never says
    CHECK(step_after_backoff());
    CHECK(status_of("chk").rec.state == DL_COMPLETED);
    CHECK(fake_file_matches(item_file("chk", DL_FILE_MEDIA).c_str(), g_fake_total));

    begin("close-delimited body (no length)");
    g_fake_default.no_length = true;
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(status_of("chk").rec.state == DL_COMPLETED);
    CHECK(fake_file_matches(item_file("chk", DL_FILE_MEDIA).c_str(), g_fake_total));
}

static void test_pause_resume(void) {
    begin("pause mid-transfer, resume later");
    arm_hook("pz", 1, 80000);
    DlMeta m = meta_for("pz");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    DlStatus st = status_of("pz");
    CHECK(st.rec.state == DL_PAUSED && !st.active);
    CHECK(st.rec.bytes_done >= 80000 && st.rec.bytes_done < (uint64_t)g_fake_total);
    uint64_t kept = st.rec.bytes_done;
    CHECK(dl_plat_file_size(item_file("pz", DL_FILE_PART).c_str()) == (int64_t)kept);
    CHECK(!dl_manager_step());                   // paused items are not picked
    CHECK(dl_pause("pz") == DL_E_STATE);         // already paused
    CHECK(dl_resume("pz") == DL_OK);
    CHECK(status_of("pz").rec.state == DL_QUEUED);
    CHECK(dl_manager_step());
    CHECK(last_request_has_range(kept));
    CHECK(status_of("pz").rec.state == DL_COMPLETED);
    CHECK(fake_file_matches(item_file("pz", DL_FILE_MEDIA).c_str(), g_fake_total));
    CHECK(dl_resume("pz") == DL_E_STATE);        // completed is terminal

    begin("pause then resume before the worker notices");
    arm_hook("pr", 6, 60000);
    DlMeta m2 = meta_for("pr");
    CHECK(dl_enqueue(&m2, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(status_of("pr").rec.state == DL_COMPLETED);

    begin("pause a queued item");
    DlMeta m3 = meta_for("pq");
    CHECK(dl_enqueue(&m3, URL, 0) == DL_OK);
    CHECK(dl_pause("pq") == DL_OK);
    CHECK(!dl_manager_step());
    CHECK(g_fake_connects == 0);
    DlRecord d;
    CHECK(dl_store_load_record("pq", &d) && d.state == DL_PAUSED);
}

static void test_cancel(void) {
    begin("cancel mid-transfer");
    arm_hook("cx", 2, 100000);
    DlMeta m = meta_for("cx");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    DlStatus st = status_of("cx");
    CHECK(st.rec.state == DL_CANCELLED && st.rec.bytes_done == 0);
    CHECK(dl_plat_file_size(item_file("cx", DL_FILE_PART).c_str()) == -1);   // deleted
    CHECK(dl_plat_file_size(item_file("cx", DL_FILE_MEDIA).c_str()) == -1);
    DlRecord d;
    CHECK(dl_store_load_record("cx", &d) && d.state == DL_CANCELLED);         // entry kept
    CHECK(!dl_manager_step());
    // Retry starts over from zero.
    CHECK(dl_retry("cx") == DL_OK);
    CHECK(dl_manager_step());
    CHECK(last_request_has_no_range());
    CHECK(status_of("cx").rec.state == DL_COMPLETED);

    begin("cancel is not undone by a later pause");
    arm_hook("cp", 5, 50000);
    DlMeta m2 = meta_for("cp");
    CHECK(dl_enqueue(&m2, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(status_of("cp").rec.state == DL_CANCELLED);

    begin("cancel paused / queued / failed items");
    DlMeta q = meta_for("cq");
    CHECK(dl_enqueue(&q, URL, 0) == DL_OK);
    CHECK(dl_cancel("cq") == DL_OK);
    CHECK(status_of("cq").rec.state == DL_CANCELLED);
    CHECK(dl_cancel("cq") == DL_E_STATE);
    // Re-enqueueing a cancelled item is allowed (same as retry, new url).
    CHECK(dl_enqueue(&q, URL, 0) == DL_OK);
    CHECK(status_of("cq").rec.state == DL_QUEUED);
}

static void test_failure_retry(void) {
    begin("server errors retry with backoff, then fail");
    FakeResp e503; e503.status = 503; e503.content_type = "application/json";
    g_fake_default = e503;
    DlMeta m = meta_for("f5");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    uint32_t expected_wait = 2000;
    for (int i = 1; i < DL_MAX_ATTEMPTS; i++) {
        CHECK(dl_manager_step());
        DlStatus st = status_of("f5");
        CHECK(st.rec.state == DL_QUEUED && st.rec.error == DL_ERR_SERVER);
        CHECK(st.rec.http_status == 503 && st.rec.attempts == (uint32_t)i);
        CHECK(st.retry_in_ms == expected_wait);
        CHECK(!dl_manager_step());               // waiting
        g_fake_now_ms += st.retry_in_ms;
        expected_wait = expected_wait * 2 > 300000 ? 300000 : expected_wait * 2;
    }
    CHECK(dl_manager_step());
    DlStatus st = status_of("f5");
    CHECK(st.rec.state == DL_FAILED && st.rec.attempts == DL_MAX_ATTEMPTS);
    CHECK(!step_after_backoff());                // failed items stay put
    // Manual retry resets the count; the server is back.
    g_fake_default = FakeResp();
    CHECK(dl_retry("f5") == DL_OK);
    st = status_of("f5");
    CHECK(st.rec.state == DL_QUEUED && st.rec.attempts == 0 && st.rec.error == DL_ERR_NONE);
    CHECK(dl_manager_step());
    CHECK(status_of("f5").rec.state == DL_COMPLETED);
    CHECK(dl_retry("f5") == DL_E_STATE);

    begin("progress resets the attempt count");
    for (int i = 0; i < DL_MAX_ATTEMPTS + 3; i++) {
        FakeResp d; d.drop_after = 20000;         // 20 KB per attempt, forever
        g_fake_queue.push_back(d);
    }
    DlMeta m2 = meta_for("flaky");
    CHECK(dl_enqueue(&m2, URL, 0) == DL_OK);
    for (int i = 0; i < DL_MAX_ATTEMPTS + 3; i++) {
        step_after_backoff();
        CHECK(status_of("flaky").rec.state != DL_FAILED);
        CHECK(status_of("flaky").rec.attempts <= 1);
    }
    CHECK(step_after_backoff());
    CHECK(status_of("flaky").rec.state == DL_COMPLETED);
    CHECK(fake_file_matches(item_file("flaky", DL_FILE_MEDIA).c_str(), g_fake_total));

    begin("permanent HTTP errors fail at once");
    struct { int status; DlError err; } perm[] = {
        { 401, DL_ERR_AUTH }, { 403, DL_ERR_AUTH }, { 404, DL_ERR_NOT_FOUND },
        { 400, DL_ERR_HTTP },
    };
    for (unsigned i = 0; i < sizeof(perm) / sizeof(perm[0]); i++) {
        char id[16]; snprintf(id, sizeof(id), "p%d", perm[i].status);
        FakeResp r; r.status = perm[i].status;
        g_fake_queue.push_back(r);
        DlMeta pm = meta_for(id);
        CHECK(dl_enqueue(&pm, URL, 0) == DL_OK);
        CHECK(dl_manager_step());
        DlStatus ps = status_of(id);
        CHECK(ps.rec.state == DL_FAILED && ps.rec.error == perm[i].err);
        CHECK(ps.rec.http_status == perm[i].status && ps.rec.attempts == 0);
    }
}

static void test_invalid_responses(void) {
    begin("invalid responses");
    struct { const char *name; FakeResp r; DlError err; } cases[4];
    cases[0].name = "html error page with 200";
    cases[0].r.content_type = "text/html"; cases[0].err = DL_ERR_BAD_RESPONSE;
    cases[1].name = "not HTTP at all";
    cases[1].r.raw = "SSH-2.0-OpenSSH_9.6\r\n\r\n"; cases[1].err = DL_ERR_BAD_RESPONSE;
    cases[2].name = "empty 200";
    cases[2].r.total = 0; cases[2].err = DL_ERR_BAD_RESPONSE;
    cases[3].name = "closed before any byte";
    cases[3].r.close_now = true; cases[3].err = DL_ERR_NETWORK;
    for (int i = 0; i < 4; i++) {
        char id[16]; snprintf(id, sizeof(id), "inv%d", i);
        g_fake_queue.push_back(cases[i].r);
        DlMeta m = meta_for(id);
        CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
        CHECK(dl_manager_step());
        DlStatus st = status_of(id);
        if (st.rec.error != cases[i].err)
            printf("    %s: got %s\n", cases[i].name, dl_error_name(st.rec.error));
        CHECK(st.rec.error == cases[i].err);
        CHECK(st.rec.state == DL_QUEUED);                   // retryable
        CHECK(dl_plat_file_size(item_file(id, DL_FILE_MEDIA).c_str()) == -1);
    }
    begin("more body than declared");
    FakeResp liar; liar.total_lie = 1000;   // Content-Length 1000, sends 300000
    g_fake_queue.push_back(liar);
    DlMeta m = meta_for("over");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    // Content-Length is authoritative: the first 1000 bytes are the file.
    DlStatus st = status_of("over");
    CHECK(st.rec.state == DL_COMPLETED && st.rec.bytes_done == 1000);

    // fewer body bytes than declared
    FakeResp shortr; shortr.total_lie = 400000;   // says 400000, sends 300000
    g_fake_queue.push_back(shortr);
    CHECK(dl_remove("over") == DL_OK);
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(status_of("over").rec.error == DL_ERR_PARTIAL);

    begin("https refused");
    // enqueue refuses it; a hand-edited record with one fails permanently.
    DlMeta hm = meta_for("tls");
    CHECK(dl_enqueue(&hm, URL, 0) == DL_OK);
    DlRecord r;
    CHECK(dl_store_load_record("tls", &r));
    snprintf(r.url, sizeof(r.url), "https://secure/x");
    CHECK(dl_store_save_record(&r));
    restart_app();
    CHECK(dl_manager_step());
    CHECK(status_of("tls").rec.state == DL_FAILED);
    CHECK(status_of("tls").rec.error == DL_ERR_UNSUPPORTED);
}

static void test_unreachable(void) {
    begin("server unreachable");
    g_fake_default.refuse = true;
    DlMeta m = meta_for("un");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    DlStatus st = status_of("un");
    CHECK(st.rec.state == DL_QUEUED && st.rec.error == DL_ERR_UNREACHABLE);
    CHECK(st.rec.attempts == 1);
    g_fake_default.refuse = false;
    CHECK(step_after_backoff());
    CHECK(status_of("un").rec.state == DL_COMPLETED);
}

static void test_suspend(void) {
    begin("suspend for playback");
    arm_hook("sp", 4, 90000);
    DlMeta m = meta_for("sp");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    DlStatus st = status_of("sp");
    CHECK(st.rec.state == DL_QUEUED && st.rec.error == DL_ERR_NONE);   // not a failure
    CHECK(st.rec.attempts == 0 && st.retry_in_ms == 0);
    CHECK(!dl_manager_step());                   // nothing starts while suspended
    dl_set_suspended(false);
    CHECK(dl_manager_step());
    CHECK(last_request_has_range(st.rec.bytes_done));
    CHECK(status_of("sp").rec.state == DL_COMPLETED);
}

static void test_deletion(void) {
    begin("remove a completed item");
    DlMeta m = meta_for("del1");
    snprintf(m.poster, sizeof(m.poster), DL_FILE_POSTER);
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    FILE *f = fopen(item_file("del1", DL_FILE_POSTER).c_str(), "wb");
    fputs("jpeg", f); fclose(f);
    char dir[DL_PATH_MAX];
    dl_store_item_dir(dir, sizeof(dir), "del1");
    CHECK(dl_plat_exists(dir));
    CHECK(dl_remove("del1") == DL_OK);
    CHECK(!dl_plat_exists(dir));
    CHECK(status_of("del1").rec.state == DL_STATE_COUNT);
    CHECK(dl_remove("del1") == DL_E_NOT_FOUND);
    restart_app();
    CHECK(status_of("del1").rec.state == DL_STATE_COUNT);   // stays gone

    begin("remove the active item");
    arm_hook("del2", 3, 30000);
    DlMeta m2 = meta_for("del2");
    CHECK(dl_enqueue(&m2, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    dl_store_item_dir(dir, sizeof(dir), "del2");
    CHECK(!dl_plat_exists(dir));
    CHECK(status_of("del2").rec.state == DL_STATE_COUNT);

    begin("remove never touches unknown files");
    DlMeta m3 = meta_for("del3");
    CHECK(dl_enqueue(&m3, URL, 0) == DL_OK);
    f = fopen(item_file("del3", "user-notes.txt").c_str(), "wb");
    fputs("mine", f); fclose(f);
    CHECK(dl_remove("del3") == DL_OK);
    CHECK(dl_plat_file_size(item_file("del3", "user-notes.txt").c_str()) == 4);
    CHECK(dl_plat_file_size(item_file("del3", DL_FILE_STATE).c_str()) == -1);
}

static void test_storage_limits(void) {
    begin("refused up front when it cannot fit");
    g_fake_free = 100000;   // reserve is 1000
    DlMeta m = meta_for("big");
    CHECK(dl_enqueue(&m, URL, 300000) == DL_E_NO_SPACE);
    CHECK(status_of("big").rec.state == DL_STATE_COUNT);
    char dir[DL_PATH_MAX];
    dl_store_item_dir(dir, sizeof(dir), "big");
    CHECK(!dl_plat_exists(dir));                   // nothing left behind
    CHECK(dl_enqueue(&m, URL, 99000) == DL_OK);    // fits (with reserve)

    begin("refused when the server says it is too big");
    g_fake_free = 200000;
    DlMeta m2 = meta_for("big2");
    CHECK(dl_enqueue(&m2, URL, 0) == DL_OK);       // size unknown: accepted
    CHECK(dl_manager_step());
    DlStatus st = status_of("big2");
    CHECK(st.rec.state == DL_FAILED && st.rec.error == DL_ERR_NO_SPACE);
    CHECK(st.rec.bytes_done == 0);
    g_fake_free = DL_FREE_UNKNOWN;                 // user freed space
    CHECK(dl_retry("big2") == DL_OK);
    CHECK(dl_manager_step());
    CHECK(status_of("big2").rec.state == DL_COMPLETED);

    begin("disk fills during the transfer");
    g_fake_default.chunked = true;                 // size unknown up front
    g_fake_default.accept_ranges = false;
    g_fake_write_budget = 150000;
    DlMeta m3 = meta_for("fill");
    CHECK(dl_enqueue(&m3, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    st = status_of("fill");
    CHECK(st.rec.state == DL_FAILED && st.rec.error == DL_ERR_NO_SPACE);
    CHECK(dl_plat_file_size(item_file("fill", DL_FILE_MEDIA).c_str()) == -1);

    begin("free space drops below the reserve mid-transfer");
    g_fake_free = DL_FREE_UNKNOWN;
    static bool dropped; dropped = false;
    g_fake_default.on_body = [](int64_t sent) {
        if (!dropped && sent > 30000) { dropped = true; g_fake_free = 500; }
    };
    DlMeta m4 = meta_for("drain");
    CHECK(dl_enqueue(&m4, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    st = status_of("drain");
    CHECK(st.rec.state == DL_FAILED && st.rec.error == DL_ERR_NO_SPACE);
    CHECK(st.rec.bytes_done < (uint64_t)g_fake_total);
}

// =========================================================================
// Restore / offline startup
// =========================================================================

static void test_restore_interrupted(void) {
    begin("app killed mid-download resumes after restart");
    // Stop the transfer the hard way: the app "dies" part way, leaving the
    // record at its last checkpoint (DOWNLOADING) and more bytes on disk.
    DlMeta m = meta_for("crash");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    FakeResp drop; drop.drop_after = 170000;
    g_fake_queue.push_back(drop);
    CHECK(dl_manager_step());
    DlRecord r;
    CHECK(dl_store_load_record("crash", &r));
    r.state = DL_DOWNLOADING;
    r.bytes_done = 100000;            // record lagging the file
    CHECK(dl_store_save_record(&r));
    restart_app();
    DlStatus st = status_of("crash");
    CHECK(st.rec.state == DL_QUEUED);
    CHECK(st.rec.bytes_done == 170000);            // file size wins
    CHECK(st.retry_in_ms == 0);                    // no backoff after restart
    CHECK(dl_manager_step());
    CHECK(last_request_has_range(170000));
    CHECK(status_of("crash").rec.state == DL_COMPLETED);
    CHECK(fake_file_matches(item_file("crash", DL_FILE_MEDIA).c_str(), g_fake_total));

    begin("killed between rename and record save");
    DlMeta m2 = meta_for("rn");
    CHECK(dl_enqueue(&m2, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(dl_store_load_record("rn", &r));
    r.state = DL_DOWNLOADING;                      // the save that never happened
    CHECK(dl_store_save_record(&r));
    restart_app();
    CHECK(status_of("rn").rec.state == DL_COMPLETED);

    begin("state survives restart in every state");
    DlMeta p = meta_for("s_paused"), f = meta_for("s_failed"), q = meta_for("s_queued");
    CHECK(dl_enqueue(&p, URL, 0) == DL_OK);
    CHECK(dl_enqueue(&f, URL, 0) == DL_OK);
    CHECK(dl_enqueue(&q, URL, 0) == DL_OK);
    CHECK(dl_pause("s_paused") == DL_OK);
    FakeResp nf; nf.status = 404;
    g_fake_queue.push_back(nf);
    CHECK(dl_manager_step());                      // s_failed (first unpaused)
    CHECK(status_of("s_failed").rec.state == DL_FAILED);
    restart_app();
    CHECK(status_of("s_paused").rec.state == DL_PAUSED);
    CHECK(status_of("s_failed").rec.state == DL_FAILED);
    CHECK(status_of("s_failed").rec.error == DL_ERR_NOT_FOUND);
    CHECK(status_of("s_queued").rec.state == DL_QUEUED);
    DlStatus list[16];
    int n = dl_list(list, 16, false);
    CHECK(n == 3);
    bool ordered = true;
    for (int i = 1; i < n; i++) if (list[i].rec.seq <= list[i - 1].rec.seq) ordered = false;
    CHECK(ordered);
    // New items continue the sequence rather than reusing numbers.
    DlMeta nx = meta_for("s_next");
    CHECK(dl_enqueue(&nx, URL, 0) == DL_OK);
    CHECK(status_of("s_next").rec.seq > list[n - 1].rec.seq);
}

static void write_file(const std::string &path, const char *text) {
    FILE *f = fopen(path.c_str(), "wb");
    fputs(text, f);
    fclose(f);
}

static void test_restore_malformed(void) {
    begin("damaged state.txt, media intact -> recovered");
    DlMeta m = meta_for("dmg1", "Recover Me");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    write_file(item_file("dmg1", DL_FILE_STATE), "jfdl-state 1\nid=dmg1\nstate=compl");
    restart_app();
    DlStatus st = status_of("dmg1");
    CHECK(st.rec.state == DL_COMPLETED);
    CHECK(st.rec.bytes_done == (uint64_t)g_fake_total);
    CHECK(strcmp(st.rec.title, "Recover Me") == 0);
    DlRecord disk;
    CHECK(dl_store_load_record("dmg1", &disk) && disk.state == DL_COMPLETED);   // rewritten

    begin("damaged state.txt falls back to .tmp");
    DlMeta mt = meta_for("dmgtmp");
    CHECK(dl_enqueue(&mt, URL, 0) == DL_OK);
    CHECK(dl_pause("dmgtmp") == DL_OK);
    // A crash between remove(old) and rename(tmp) leaves only the .tmp.
    std::string st_path = item_file("dmgtmp", DL_FILE_STATE);
    CHECK(rename(st_path.c_str(), (st_path + ".tmp").c_str()) == 0);
    restart_app();
    CHECK(status_of("dmgtmp").rec.state == DL_PAUSED);

    begin("damaged state and meta -> visible, removable");
    DlMeta m2 = meta_for("dmg2");
    CHECK(dl_enqueue(&m2, URL, 0) == DL_OK);
    write_file(item_file("dmg2", DL_FILE_STATE), "\x01\x02 binary junk");
    write_file(item_file("dmg2", DL_FILE_META), "");
    write_file(item_file("dmg2", DL_FILE_PART), "12345");
    restart_app();
    st = status_of("dmg2");
    CHECK(st.rec.state == DL_FAILED && st.rec.error == DL_ERR_CORRUPT);
    CHECK(st.rec.bytes_done == 5 && strcmp(st.rec.title, "dmg2") == 0);
    CHECK(dl_retry("dmg2") == DL_E_STATE);         // no request to repeat
    DlMeta fresh = meta_for("dmg2", "Fresh");
    CHECK(dl_enqueue(&fresh, URL, 0) == DL_OK);    // but can be re-downloaded
    CHECK(status_of("dmg2").rec.state == DL_QUEUED);
    // ...from scratch: a partial whose record was lost cannot be trusted.
    CHECK(status_of("dmg2").rec.bytes_done == 0);
    CHECK(dl_plat_file_size(item_file("dmg2", DL_FILE_PART).c_str()) == -1);
    CHECK(dl_cancel("dmg2") == DL_OK);
    CHECK(dl_remove("dmg2") == DL_OK);

    begin("completed record with missing media");
    DlMeta m3 = meta_for("gone");
    CHECK(dl_enqueue(&m3, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(remove(item_file("gone", DL_FILE_MEDIA).c_str()) == 0);
    restart_app();
    st = status_of("gone");
    CHECK(st.rec.state == DL_FAILED && st.rec.error == DL_ERR_CORRUPT);
    char path[DL_PATH_MAX];
    CHECK(!dl_media_path("gone", path, sizeof(path)));
    CHECK(dl_retry("gone") == DL_OK);              // url is known: fetch again
    CHECK(dl_manager_step());
    CHECK(status_of("gone").rec.state == DL_COMPLETED);

    // ...and one whose media is there but the wrong size
    CHECK(truncate(item_file("gone", DL_FILE_MEDIA).c_str(), 1000) == 0);
    restart_app();
    CHECK(status_of("gone").rec.state == DL_FAILED);
    CHECK(status_of("gone").rec.error == DL_ERR_CORRUPT);

    begin("record filed under the wrong directory");
    DlMeta a = meta_for("dira");
    CHECK(dl_enqueue(&a, URL, 0) == DL_OK);
    std::string other = item_file("dira", DL_FILE_STATE);
    write_file(other, "jfdl-state 1\nid=dirb\nstate=completed\nend\n");
    restart_app();
    CHECK(status_of("dirb").rec.state == DL_STATE_COUNT);
    CHECK(status_of("dira").rec.state == DL_FAILED);

    begin("junk directories are ignored");
    CHECK(dl_plat_mkdir((s_root + "/items/not.an.id").c_str()));
    CHECK(dl_plat_mkdir((s_root + "/items/has space").c_str()));
    restart_app();
    DlStatus list[64];
    int n = dl_list(list, 64, false);
    for (int i = 0; i < n; i++) CHECK(dl_id_valid(list[i].rec.id));

    begin("unwritable root is refused");
    dl_manager_shutdown();
    CHECK(!dl_manager_init("/proc/definitely/not/writable", &s_cfg));
    CHECK(!dl_manager_ready());
    DlMeta x = meta_for("x");
    CHECK(dl_enqueue(&x, URL, 0) == DL_E_NOT_READY);
    CHECK(dl_list(list, 64, false) == 0);
    CHECK(!dl_manager_step());
}

static void test_offline_startup(void) {
    begin("offline startup: library usable with no server");
    DlMeta a = meta_for("offa", "Downloaded A"), b = meta_for("offb", "Downloaded B");
    snprintf(a.series, sizeof(a.series), "Show");
    a.season = 2; a.episode = 5; a.runtime_secs = 1800;
    CHECK(dl_enqueue(&a, URL, 0) == DL_OK);
    CHECK(dl_enqueue(&b, URL, 0) == DL_OK);
    CHECK(run_worker() == 2);
    DlMeta pending = meta_for("offc", "Still Queued");
    CHECK(dl_enqueue(&pending, URL, 0) == DL_OK);

    // The server goes away and the console reboots.
    g_fake_default.refuse = true;
    g_fake_connects = 0;
    restart_app();
    CHECK(g_fake_connects == 0);                   // init touched no network
    DlStatus lib[8];
    int n = dl_list(lib, 8, true);
    CHECK(n == 2);
    CHECK(!strcmp(lib[0].rec.id, "offa") && !strcmp(lib[1].rec.id, "offb"));
    DlMeta got;
    CHECK(dl_load_meta("offa", &got));
    CHECK(!strcmp(got.title, "Downloaded A") && !strcmp(got.series, "Show"));
    CHECK(got.season == 2 && got.episode == 5 && got.runtime_secs == 1800);
    char path[DL_PATH_MAX];
    CHECK(dl_media_path("offa", path, sizeof(path)));
    CHECK(fake_file_matches(path, g_fake_total));

    // The queued one tries, fails as unreachable, and leaves the library alone.
    CHECK(dl_manager_step());
    CHECK(status_of("offc").rec.error == DL_ERR_UNREACHABLE);
    CHECK(dl_list(lib, 8, true) == 2);
    CHECK(dl_media_path("offb", path, sizeof(path)));
    CHECK(!dl_media_path("offc", path, sizeof(path)));
}


// =========================================================================
// Playback: downloads yield to streaming
// =========================================================================

// Stream URLs in exactly the shape build_stream_url() produces
// (player/core/player_session.cpp), one per quality step / audio mode.
static std::string stream_url(int w, int h, long vbitrate, bool hd_copy,
                              const char *acodec = "mp3", long abitrate = 192000) {
    char v[96], a[128];
    if (vbitrate > 0)
        snprintf(v, sizeof(v), "&VideoBitrate=%ld&AllowVideoStreamCopy=true", vbitrate);
    else
        snprintf(v, sizeof(v), "&AllowVideoStreamCopy=true");
    if (hd_copy)
        snprintf(a, sizeof(a), "&AudioCodec=ac3,truehd,mp3&AudioSampleRate=48000"
                               "&MaxAudioChannels=8");
    else
        snprintf(a, sizeof(a), "&AudioCodec=%s&AudioBitrate=%ld&AudioSampleRate=48000"
                               "&MaxAudioChannels=%d", acodec, abitrate,
                 strcmp(acodec, "ac3") == 0 ? 6 : 2);
    char url[768];
    snprintf(url, sizeof(url),
             "http://192.168.1.2:8096/Videos/abc/stream.ts?VideoCodec=h264"
             "&Profile=baseline&Level=31&MaxWidth=%d&MaxHeight=%d%s%s"
             "&MaxFramerate=30&AllowAudioStreamCopy=%s&DeviceId=dev"
             "&Static=false&MediaSourceId=abc&StartTimeTicks=0&PlaySessionId=ps1",
             w, h, v, a, hd_copy ? "true" : "false");
    return url;
}

static const std::string &url_480p(void)  { static std::string u = stream_url(854, 480, 1500000, false); return u; }
static const std::string &url_1080p(void) { static std::string u = stream_url(1920, 1080, 10000000, false); return u; }

static void test_stream_classifier(void) {
    s_test = "light/heavy stream classifier"; printf("- %s\n", s_test);
    CHECK(dl_stream_is_light(stream_url(854, 480, 1500000, false).c_str()));   // 480p
    CHECK(dl_stream_is_light(stream_url(640, 360, 700000, false).c_str()));    // 360p
    CHECK(dl_stream_is_light(stream_url(854, 480, 1500000, false, "ac3", 640000).c_str())); // 5.1
    CHECK(!dl_stream_is_light(stream_url(1280, 720, 4000000, false).c_str()));   // 720p
    CHECK(!dl_stream_is_light(stream_url(1920, 1080, 10000000, false).c_str())); // High
    CHECK(!dl_stream_is_light(stream_url(1920, 1080, 25000000, false).c_str())); // Max
    CHECK(!dl_stream_is_light(stream_url(1920, 1080, 0, false).c_str()));        // Original
    CHECK(!dl_stream_is_light(stream_url(854, 480, 0, false).c_str()));   // copy, no ceiling
    CHECK(!dl_stream_is_light(stream_url(854, 480, 1500000, true).c_str()));  // TrueHD copy
    CHECK(!dl_stream_is_light(stream_url(854, 576, 1500000, false).c_str())); // 576 lines
    CHECK(!dl_stream_is_light(stream_url(854, 480, 1500001, false).c_str()));
    CHECK(!dl_stream_is_light(stream_url(854, 480, 1500000, false, "flac", 1411000).c_str()));
    // Fail closed on anything odd.
    CHECK(!dl_stream_is_light(NULL));
    CHECK(!dl_stream_is_light(""));
    CHECK(!dl_stream_is_light("http://h/stream.ts"));
    CHECK(!dl_stream_is_light("http://h/s.ts?MaxHeight=480"));                   // no ceiling
    CHECK(!dl_stream_is_light("http://h/s.ts?MaxHeight=abc&VideoBitrate=700000"));
    CHECK(!dl_stream_is_light("http://h/s.ts?MaxHeight=480&VideoBitrate=0"));
    CHECK(!dl_stream_is_light("http://h/s.ts?MaxHeight=480&VideoBitrate=700000"
                              "&AllowAudioStreamCopy=yes"));
    CHECK(dl_stream_is_light("http://h/s.ts?MaxHeight=480&VideoBitrate=700000"));
    // Names match whole keys only.
    CHECK(!dl_stream_is_light("http://h/s.ts?XMaxHeight=480&MaxVideoBitrate=700000"));
    CHECK(!dl_stream_is_light("http://h/s.ts?MediaSourceId=a&MaxHeight=480"
                              "&SourceVideoBitrate=700000"));
    char v[16];
    CHECK(dl_url_query_get("http://h/p?a=1&bb=22&b=3", "b", v, sizeof(v)) && !strcmp(v, "3"));
    CHECK(dl_url_query_get("http://h/p?a=1&bb=22&b=3", "a", v, sizeof(v)) && !strcmp(v, "1"));
    CHECK(!dl_url_query_get("http://h/p?a=1", "c", v, sizeof(v)));
    CHECK(!dl_url_query_get("http://h/p?a=12345678901234567890", "a", v, sizeof(v)));
    CHECK(!dl_url_query_get("http://h/p", "a", v, sizeof(v)));
}

static void test_playback_heavy_stops(void) {
    begin("heavy stream stops downloads");
    // Nothing starts while a heavy stream plays.
    dl_playback_begin(url_1080p().c_str());
    CHECK(dl_playback_blocking());
    DlMeta m = meta_for("hv");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);        // queueing is still fine
    CHECK(!dl_manager_step());
    CHECK(g_fake_connects == 0);
    dl_playback_end();
    CHECK(!dl_playback_blocking());

    // A running download stops the moment a heavy stream opens...
    static const char *s_heavy;
    s_heavy = url_1080p().c_str();
    g_fake_default.on_body = [](int64_t sent) {
        static bool done; if (sent == 0) done = false;
        if (!done && sent >= 60000) { done = true; dl_playback_begin(s_heavy); }
    };
    CHECK(dl_manager_step());
    DlStatus st = status_of("hv");
    CHECK(st.rec.state == DL_QUEUED && st.rec.error == DL_ERR_NONE);  // not a failure
    CHECK(st.rec.attempts == 0 && st.retry_in_ms == 0);
    CHECK(st.rec.bytes_done >= 60000 && st.rec.bytes_done < (uint64_t)g_fake_total);
    // ...keeps what it had on disk...
    CHECK(dl_plat_file_size(item_file("hv", DL_FILE_PART).c_str()) == (int64_t)st.rec.bytes_done);
    // ...waits for as long as playback lasts...
    g_fake_now_ms += 3600000;
    CHECK(!dl_manager_step());
    // ...and resumes from where it stopped once playback ends.
    g_fake_default.on_body = nullptr;
    dl_playback_end();
    CHECK(dl_manager_step());
    CHECK(last_request_has_range(st.rec.bytes_done));
    CHECK(status_of("hv").rec.state == DL_COMPLETED);
    CHECK(fake_file_matches(item_file("hv", DL_FILE_MEDIA).c_str(), g_fake_total));

    begin("track change to an HD copy stops a paced download");
    static std::string keep;
    keep = stream_url(854, 480, 1500000, true);    // same 480p, TrueHD copy
    s_heavy = keep.c_str();
    dl_playback_begin(url_480p().c_str());
    CHECK(!dl_playback_blocking());
    g_fake_default.on_body = [](int64_t sent) {
        static bool done; if (sent == 0) done = false;
        if (!done && sent >= 40000) { done = true; dl_playback_begin(s_heavy); }
    };
    DlMeta m2 = meta_for("tc");
    CHECK(dl_enqueue(&m2, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(status_of("tc").rec.state == DL_QUEUED);
    CHECK(dl_playback_blocking());
    dl_playback_end();

    begin("ending playback does not lift a service suspension");
    dl_set_suspended(true);
    dl_playback_begin(url_480p().c_str());
    dl_playback_end();
    DlMeta m3 = meta_for("ss");
    CHECK(dl_enqueue(&m3, URL, 0) == DL_OK);
    CHECK(!dl_manager_step());
    dl_set_suspended(false);
    CHECK(dl_manager_step());
    CHECK(status_of("ss").rec.state == DL_COMPLETED);
}

static void test_playback_light_paced(void) {
    begin("light stream: downloads continue, paced");
    s_cfg.stream_share_bps = 800000;               // 100 KB/s, for a quick test
    restart_app();
    DlMeta m = meta_for("lt");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);

    // Unpaced baseline: 300 KB in a few fake milliseconds.
    uint64_t t0 = g_fake_now_ms;
    CHECK(dl_manager_step());
    const uint64_t unpaced = g_fake_now_ms - t0;
    CHECK(status_of("lt").rec.state == DL_COMPLETED);
    CHECK(unpaced < 500);

    // Beside a 480p stream: same file, held to ~100 KB/s => ~3 s.
    CHECK(dl_remove("lt") == DL_OK);
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    dl_playback_begin(url_480p().c_str());
    CHECK(!dl_playback_blocking());
    t0 = g_fake_now_ms;
    CHECK(dl_manager_step());
    const uint64_t paced = g_fake_now_ms - t0;
    CHECK(status_of("lt").rec.state == DL_COMPLETED);
    CHECK(fake_file_matches(item_file("lt", DL_FILE_MEDIA).c_str(), g_fake_total));
    const uint64_t ideal = (uint64_t)g_fake_total * 8000 / 800000;   // 3000 ms
    if (paced < ideal * 8 / 10 || paced > ideal * 15 / 10)
        printf("    paced %llu ms, ideal %llu ms\n", (unsigned long long)paced,
               (unsigned long long)ideal);
    CHECK(paced >= ideal * 8 / 10);     // actually held back...
    CHECK(paced <= ideal * 15 / 10);    // ...but not starved
    // Pacing never counts as the server being idle (idle timeout is 5 s,
    // the paced transfer takes 3 s of sleeps: no false timeout).
    CHECK(status_of("lt").rec.error == DL_ERR_NONE);

    begin("a long pacing wait is not an idle server");
    // One 64 KB read at 10 KB/s means a 6.5 s wait -- longer than the 5 s
    // idle timeout -- and the next read then waits on the network once.
    s_cfg.stream_share_bps = 80000;
    restart_app();
    g_fake_default.recv_chunk = 65536;
    g_fake_default.timeout_every = 1;
    g_fake_total = 200000;
    DlMeta mi = meta_for("idle");
    CHECK(dl_enqueue(&mi, URL, 0) == DL_OK);
    dl_playback_begin(url_480p().c_str());
    CHECK(dl_manager_step());
    CHECK(status_of("idle").rec.state == DL_COMPLETED);
    CHECK(status_of("idle").rec.error == DL_ERR_NONE);
    dl_playback_end();

    begin("playback ends mid-transfer: pacing lifts");
    s_cfg.stream_share_bps = 80000;                // 10 KB/s: 30 s if it stayed
    restart_app();
    DlMeta m2 = meta_for("lift");
    CHECK(dl_enqueue(&m2, URL, 0) == DL_OK);
    dl_playback_begin(url_480p().c_str());
    g_fake_default.on_body = [](int64_t sent) { if (sent >= 30000) dl_playback_end(); };
    t0 = g_fake_now_ms;
    CHECK(dl_manager_step());
    CHECK(status_of("lift").rec.state == DL_COMPLETED);
    CHECK(g_fake_now_ms - t0 < 10000);

    begin("pause lands during pacing");
    s_cfg.stream_share_bps = 8000;                 // 1 KB/s: would take minutes
    restart_app();
    DlMeta m3 = meta_for("pp");
    CHECK(dl_enqueue(&m3, URL, 0) == DL_OK);
    dl_playback_begin(url_480p().c_str());
    g_fake_default.recv_chunk = 65536;             // 64 s of pacing per read
    // Pause from "the UI" while the worker is inside a pacing sleep.
    g_fake_on_sleep = []() { dl_pause("pp"); };
    t0 = g_fake_now_ms;
    CHECK(dl_manager_step());
    CHECK(status_of("pp").rec.state == DL_PAUSED);
    CHECK(g_fake_now_ms - t0 < 2000);              // noticed within one slice
    g_fake_on_sleep = nullptr;
    dl_playback_end();
}

static void test_write_batching(void) {
    begin("writes are batched, not one per read");
    s_cfg.checkpoint_bytes = 64ull << 20;          // no checkpoint interference
    restart_app();
    g_fake_total = 3 * 1024 * 1024;
    DlMeta m = meta_for("wb");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(status_of("wb").rec.state == DL_COMPLETED);
    CHECK(fake_file_matches(item_file("wb", DL_FILE_MEDIA).c_str(), g_fake_total));
    const int writes = (int)g_fake_write_sizes.size();
    printf("    %d reads -> %d writes\n", g_fake_data_recvs, writes);
    CHECK(g_fake_data_recvs > 300);                // 7919-byte reads
    CHECK(writes <= 8);                            // ~450 KB each
    for (int i = 0; i + 1 < writes; i++) CHECK(g_fake_write_sizes[i] >= 448 * 1024);

    begin("batched bytes still land on a drop");
    s_cfg.checkpoint_bytes = 64ull << 20;
    restart_app();
    FakeResp d; d.drop_after = 200001;             // well inside one batch
    g_fake_queue.push_back(d);
    DlMeta m2 = meta_for("wd");
    CHECK(dl_enqueue(&m2, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(status_of("wd").rec.bytes_done == 200001);
    CHECK(dl_plat_file_size(item_file("wd", DL_FILE_PART).c_str()) == 200001);
    CHECK(step_after_backoff());
    CHECK(last_request_has_range(200001));
    CHECK(fake_file_matches(item_file("wd", DL_FILE_MEDIA).c_str(), g_fake_total));
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "-v") == 0) g_fake_verbose = true;

    test_state_machine();
    test_error_policy();
    test_progress_math();
    test_id_validation();
    test_record_roundtrip();
    test_record_malformed();
    test_meta_roundtrip();
    test_meta_malformed();
    test_url_parse();
    test_request_build();
    test_head_parse();
    test_head_reader();
    test_chunked();

    test_enqueue_and_complete();
    test_queue_order_and_duplicates();
    test_progress_checkpoints();
    test_interrupted_resume();
    test_reset_and_timeout();
    test_head_timeout();
    test_range_ignored();
    test_range_inconsistent();
    test_416_already_complete();
    test_chunked_transcode();
    test_pause_resume();
    test_cancel();
    test_failure_retry();
    test_invalid_responses();
    test_unreachable();
    test_suspend();
    test_deletion();
    test_storage_limits();
    test_restore_interrupted();
    test_restore_malformed();
    test_offline_startup();
    test_stream_classifier();
    test_playback_heavy_stops();
    test_playback_light_paced();
    test_write_batching();

    if (!s_tmp.empty()) fake_rmtree(s_tmp);
    printf("offline downloads: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
