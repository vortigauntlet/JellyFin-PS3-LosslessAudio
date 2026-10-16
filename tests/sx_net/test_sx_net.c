// sx_conn / sx_tls / sx_http against tests/sx_net_server.py (plain + TLS), and
// optionally against the real hosts (--live).  Build with -DSX_TEST_HOOKS.
//
// usage: test_sx_net <portfile> <ca.pem>            local server suite
//        test_sx_net --live                          real hosts, bundled roots only
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "sx_http.h"
#include "sx_tls.h"
#include "sx_nettest.h"

static int g_fail, g_run;
#define CHECK(c) do { g_run++; if (!(c)) { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_MSG(c, ...) do { g_run++; if (!(c)) { g_fail++; printf("FAIL %s:%d  %s  -- ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int P_PLAIN, P_GOOD, P_EXPIRED, P_WRONG, P_SELF, P_CHACHA_RSA, P_AES_EC, P_CLIENTPREF;

static unsigned char pat(long long i) { return (unsigned char)(((i * 7) + (i >> 8)) & 255); }

static void url_plain(char *o, size_t n, const char *path) { snprintf(o, n, "http://127.0.0.1:%d%s", P_PLAIN, path); }
static void url_tls(char *o, size_t n, int port, const char *path) { snprintf(o, n, "https://localhost:%d%s", port, path); }

typedef struct { int rc; sx_http_info info; char *body; size_t len; } res;

static res fetch(const char *url, const sx_http_req *proto) {
    res r; memset(&r, 0, sizeof(r));
    sx_http_req rq; memset(&rq, 0, sizeof(rq));
    if (proto) rq = *proto;
    rq.url = url;
    r.rc = sx_http_fetch(&rq, 0, &r.body, &r.len, &r.info);
    return r;
}
static void done(res *r) { free(r->body); r->body = NULL; }

static void test_basic(void) {
    char u[256];
    url_plain(u, sizeof(u), "/ok");
    res r = fetch(u, NULL);
    CHECK(r.rc == SX_OK && r.info.status == 200);
    CHECK(r.len == 11 && r.body && strcmp(r.body, "hello world") == 0);
    CHECK(r.info.content_length == 11);
    CHECK(!r.info.tls);
    done(&r);

    url_plain(u, sizeof(u), "/json");
    r = fetch(u, NULL);
    CHECK(r.rc == SX_OK && r.len > 5000 && r.body[0] == '{');
    done(&r);

    url_plain(u, sizeof(u), "/chunked");
    r = fetch(u, NULL);
    char want[1000] = "alpha";
    for (int i = 0; i < 100; i++) strcat(want, "beta-");
    strcat(want, "gamma");
    CHECK(r.rc == SX_OK && r.body && strcmp(r.body, want) == 0);
    CHECK(r.info.content_length == -1);
    done(&r);

    url_plain(u, sizeof(u), "/close");
    r = fetch(u, NULL);
    CHECK(r.rc == SX_OK && r.body && strcmp(r.body, "until-close-body") == 0);
    done(&r);

    url_plain(u, sizeof(u), "/status/404");
    r = fetch(u, NULL);
    CHECK(r.rc == SX_OK && r.info.status == 404 && r.body && strcmp(r.body, "status 404") == 0);
    done(&r);
    url_plain(u, sizeof(u), "/status/429");
    r = fetch(u, NULL);
    CHECK(r.info.status == 429 && r.info.retry_after == 7);
    done(&r);

    url_plain(u, sizeof(u), "/space here");
    r = fetch(u, NULL);
    CHECK(r.info.status == 200 && r.body && strcmp(r.body, "escaped ok") == 0);
    done(&r);

    url_plain(u, sizeof(u), "/cut");
    r = fetch(u, NULL);
    CHECK(r.rc != SX_OK && r.body == NULL);              // a body cut short is an error, never a short success
    done(&r);
}

static void test_post_headers(void) {
    char u[256];
    url_plain(u, sizeof(u), "/post");
    sx_http_req rq; memset(&rq, 0, sizeof(rq));
    rq.method = "POST"; rq.body = "hello"; rq.body_len = 5; rq.content_type = "text/plain";
    res r = fetch(u, &rq);
    CHECK(r.info.status == 200 && r.body && strstr(r.body, "\"len\": 5") && strstr(r.body, "text/plain"));
    done(&r);

    sx_http_set_user_agent("StremioPS3/test");
    url_plain(u, sizeof(u), "/echo-hdr");
    memset(&rq, 0, sizeof(rq));
    rq.headers = "X-Test: one\r\n";
    rq.has_range = true; rq.range_start = 5; rq.range_end = 9;
    r = fetch(u, &rq);
    CHECK(r.body && strstr(r.body, "X-Test: one"));
    CHECK(r.body && strstr(r.body, "User-Agent: StremioPS3/test"));
    CHECK(r.body && strstr(r.body, "Accept-Encoding: identity"));
    CHECK(r.body && strstr(r.body, "Range: bytes=5-9"));
    char host[64]; snprintf(host, sizeof(host), "Host: 127.0.0.1:%d", P_PLAIN);
    CHECK(r.body && strstr(r.body, host));
    done(&r);

    // Custom headers belong to the original host: a redirect to another host name must drop them.
    char to[300], u2[600];
    snprintf(to, sizeof(to), "http://localhost:%d/echo-hdr", P_PLAIN);
    snprintf(u2, sizeof(u2), "http://127.0.0.1:%d/cross?to=%s", P_PLAIN, to);
    memset(&rq, 0, sizeof(rq));
    rq.headers = "X-Test: one\r\n";
    r = fetch(u2, &rq);
    CHECK(r.info.status == 200 && r.body && !strstr(r.body, "X-Test") && strstr(r.body, "GET"));
    CHECK(r.info.redirects == 1);
    done(&r);
}

static void test_redirects(void) {
    char u[256];
    url_plain(u, sizeof(u), "/redir/3");
    res r = fetch(u, NULL);
    CHECK(r.rc == SX_OK && r.info.status == 200 && r.body && strcmp(r.body, "hello world") == 0);
    CHECK(r.info.redirects == 3);
    char want[256]; snprintf(want, sizeof(want), "http://127.0.0.1:%d/ok", P_PLAIN);
    CHECK(strcmp(r.info.final_url, want) == 0);
    done(&r);

    sx_http_req rq; memset(&rq, 0, sizeof(rq)); rq.max_redirects = 2;
    r = fetch(u, &rq);
    CHECK(r.rc == SX_E_REDIR && r.body == NULL);
    done(&r);

    url_plain(u, sizeof(u), "/loop");
    r = fetch(u, NULL);
    CHECK(r.rc == SX_E_REDIR);
    done(&r);

    // http -> https is allowed (the certificate is still verified); https -> http is refused.
    char to[300], u2[700];
    snprintf(to, sizeof(to), "https://localhost:%d/ok", P_GOOD);
    snprintf(u2, sizeof(u2), "http://127.0.0.1:%d/cross?to=%s", P_PLAIN, to);
    r = fetch(u2, NULL);
    CHECK_MSG(r.rc == SX_OK && r.info.status == 200 && r.info.tls, "rc=%d %s", r.rc, r.info.err.msg);
    done(&r);

    snprintf(to, sizeof(to), "http://127.0.0.1:%d/ok", P_PLAIN);
    snprintf(u2, sizeof(u2), "https://localhost:%d/cross?to=%s", P_GOOD, to);
    r = fetch(u2, NULL);
    CHECK(r.rc == SX_E_REDIR);
    CHECK(strstr(r.info.err.msg, "https to http") != NULL);
    done(&r);
}

// Reads a range through the streaming API and checks every byte.
static void check_range(const char *base, long long a, long long b, int expect_status) {
    sx_http_req rq; memset(&rq, 0, sizeof(rq));
    char u[300]; snprintf(u, sizeof(u), "%s/range", base);
    rq.url = u; rq.has_range = true; rq.range_start = a; rq.range_end = b; rq.no_pool = true;
    sx_http_info info;
    sx_http *h = sx_http_open(&rq, &info);
    CHECK(h != NULL);
    if (!h) return;
    CHECK_MSG(info.status == expect_status, "status %d", info.status);
    if (info.status == 206) {
        CHECK(info.range_start == a);
        const long long end = (b >= 0 && b < (1 << 20) - 1) ? b : (1 << 20) - 1;
        CHECK(info.range_end == end);
        CHECK(info.range_total == (1 << 20));
        CHECK(info.content_length == end - a + 1);
        long long pos = a;
        unsigned char buf[777];
        int bad = 0;
        for (;;) {
            const int n = sx_http_read(h, buf, sizeof(buf));
            if (n <= 0) { CHECK(n == 0); break; }
            for (int i = 0; i < n; i++) if (buf[i] != pat(pos + i)) bad++;
            pos += n;
        }
        CHECK(bad == 0);
        CHECK(pos == end + 1);
    }
    sx_http_close(h);
}

static void test_range(const char *base) {
    check_range(base, 100, 299, 206);
    check_range(base, 0, 0, 206);
    check_range(base, 1048500, -1, 206);
    check_range(base, 70000, 140000, 206);
    check_range(base, 2000000, -1, 416);
}

static void test_range_edge(void) {
    char u[256];
    sx_http_req rq; memset(&rq, 0, sizeof(rq));
    url_plain(u, sizeof(u), "/norange");
    rq.has_range = true; rq.range_start = 10; rq.range_end = -1;
    res r = fetch(u, &rq);
    CHECK(r.info.status == 200 && !r.info.accept_ranges && r.len == (1u << 20));   // the caller can see the range was ignored
    done(&r);

    url_plain(u, sizeof(u), "/badrange");
    memset(&rq, 0, sizeof(rq));
    rq.has_range = true; rq.range_start = 50; rq.range_end = -1;
    r = fetch(u, &rq);
    CHECK(r.rc == SX_E_PROTO);                               // 206 for a different start is refused
    done(&r);
}

static void test_timeouts_and_sizes(void) {
    char u[256]; sx_http_req rq;
    url_plain(u, sizeof(u), "/slow=1500");
    memset(&rq, 0, sizeof(rq)); rq.first_byte_ms = 5000;
    res r = fetch(u, &rq);
    CHECK(r.rc == SX_OK && r.body && strcmp(r.body, "slow ok") == 0);
    CHECK(r.info.ttfb_ms >= 1400);
    done(&r);

    memset(&rq, 0, sizeof(rq)); rq.first_byte_ms = 500; rq.no_pool = true;
    r = fetch(u, &rq);
    CHECK(r.rc == SX_E_TIMEOUT);
    done(&r);

    url_plain(u, sizeof(u), "/json");
    r = fetch(u, NULL);                                      // warm the path
    done(&r);
    char *body = NULL; size_t len = 0; sx_http_info info;
    memset(&rq, 0, sizeof(rq)); rq.url = u;
    CHECK(sx_http_fetch(&rq, 100, &body, &len, &info) == SX_E_TOOBIG && body == NULL);
    char small[100]; int n;
    CHECK(sx_http_fetch_buf(&rq, small, sizeof(small), &n, &info) == SX_E_TOOBIG && small[0] == 0);
    char fit[8000];
    CHECK(sx_http_fetch_buf(&rq, fit, sizeof(fit), &n, &info) == SX_OK && n > 5000 && fit[n] == 0);
    CHECK(sx_http_fetch_buf(&rq, fit, 5004, &n, &info) == SX_E_TOOBIG);   // fails closed, never truncates

    // A connect to a closed port, and a name that does not resolve.
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof(a)); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(s, (struct sockaddr *)&a, sizeof(a));
    socklen_t al = sizeof(a); getsockname(s, (struct sockaddr *)&a, &al);
    const int closed_port = ntohs(a.sin_port);
    close(s);
    char cu[100]; snprintf(cu, sizeof(cu), "http://127.0.0.1:%d/", closed_port);
    r = fetch(cu, NULL);
    CHECK(r.rc == SX_E_CONNECT);
    done(&r);
    r = fetch("http://no-such-host.invalid/", NULL);
    CHECK(r.rc == SX_E_DNS);
    done(&r);
    r = fetch("not a url", NULL);
    CHECK(r.rc == SX_E_ARG);
    done(&r);
}

static void test_big(const char *base) {
    char u[300]; snprintf(u, sizeof(u), "%s/big", base);
    sx_http_req rq; memset(&rq, 0, sizeof(rq)); rq.url = u;
    sx_http_info info;
    sx_http *h = sx_http_open(&rq, &info);
    CHECK(h != NULL);
    if (!h) return;
    unsigned char buf[4096];
    long long pos = 0; int bad = 0, n;
    while ((n = sx_http_read(h, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) if (buf[i] != pat((pos + i) % (1 << 20))) bad++;
        pos += n;
    }
    CHECK(n == 0);
    CHECK(pos == 3LL * (1 << 20));
    CHECK(bad == 0);
    sx_http_close(h);
}

static int conn_count(const char *base) {
    char u[300]; snprintf(u, sizeof(u), "%s/conn", base);
    res r = fetch(u, NULL);
    int n = r.body ? atoi(r.body) : -1;
    done(&r);
    return n;
}

static void test_pool(const char *base) {
    sx_http_pool_flush();
    const int c1 = conn_count(base);
    int reused = 0;
    for (int i = 0; i < 5; i++) {
        char u[300]; snprintf(u, sizeof(u), "%s/ok", base);
        res r = fetch(u, NULL);
        CHECK(r.rc == SX_OK && r.info.status == 200);
        if (r.info.reused) reused++;
        done(&r);
    }
    const int c2 = conn_count(base);
    CHECK_MSG(c2 == c1, "connections %d -> %d (keep-alive pooling should reuse one)", c1, c2);
    CHECK(reused == 5);

    // A pooled connection the server closed under us is detected and replaced.
    char u[300]; snprintf(u, sizeof(u), "%s/stale", base);
    res r = fetch(u, NULL);
    CHECK(r.rc == SX_OK && r.body && strcmp(r.body, "stale") == 0);
    done(&r);
    usleep(50000);
    snprintf(u, sizeof(u), "%s/ok", base);
    r = fetch(u, NULL);
    CHECK_MSG(r.rc == SX_OK && r.info.status == 200, "rc=%d %s", r.rc, r.info.err.msg);
    done(&r);
    sx_http_pool_flush();
}

static void test_tls(void) {
    char u[256];
    sx_http_pool_flush();
    sx_tls_flush_sessions();
    url_tls(u, sizeof(u), P_GOOD, "/ok");
    sx_http_req rq; memset(&rq, 0, sizeof(rq)); rq.no_pool = true;
    res r = fetch(u, &rq);
    CHECK_MSG(r.rc == SX_OK && r.info.status == 200, "rc=%d %s", r.rc, r.info.err.msg);
    CHECK(r.info.tls && strcmp(r.info.tls_info.version, "TLSv1.2") == 0);
    CHECK(r.info.tls_info.cipher[0] != 0);
    CHECK(!r.info.tls_info.resumed);
    printf("  tls cold: %s %s %d ms\n", r.info.tls_info.version, r.info.tls_info.cipher, r.info.tls_info.handshake_ms);
    done(&r);
    r = fetch(u, &rq);
    CHECK(r.rc == SX_OK);
    CHECK_MSG(r.info.tls_info.resumed, "second handshake to the same host did not resume");
    printf("  tls warm: resumed=%d %d ms\n", r.info.tls_info.resumed, r.info.tls_info.handshake_ms);
    done(&r);

    struct { int port; int reason; const char *what; } bad[] = {
        { P_EXPIRED, SX_CERT_DATE, "expired" },
        { P_WRONG, SX_CERT_HOST, "wrong host" },
        { P_SELF, SX_CERT_UNTRUSTED, "self-signed" },
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        url_tls(u, sizeof(u), bad[i].port, "/ok");
        r = fetch(u, &rq);
        CHECK_MSG(r.rc == SX_E_CERT && r.info.err.cert_reason == bad[i].reason && r.body == NULL,
                  "%s: rc=%d reason=%d msg=%s", bad[i].what, r.rc, r.info.err.cert_reason, r.info.err.msg);
        CHECK(strstr(r.info.err.msg, "localhost") != NULL);
        done(&r);
    }
    // The right certificate for the wrong name: reaching the server by IP must not pass for "localhost".
    char ip[100]; snprintf(ip, sizeof(ip), "https://127.0.0.1:%d/ok", P_GOOD);
    r = fetch(ip, &rq);
    CHECK(r.rc == SX_E_CERT);
    done(&r);

    url_tls(u, sizeof(u), P_CHACHA_RSA, "/ok");
    r = fetch(u, &rq);
    CHECK_MSG(r.rc == SX_OK && strstr(r.info.tls_info.cipher, "CHACHA20"), "rc=%d %s %s", r.rc, r.info.tls_info.cipher, r.info.err.msg);
    done(&r);
    url_tls(u, sizeof(u), P_AES_EC, "/ok");
    r = fetch(u, &rq);
    CHECK_MSG(r.rc == SX_OK && strstr(r.info.tls_info.cipher, "AES-128-GCM"), "rc=%d %s %s", r.rc, r.info.tls_info.cipher, r.info.err.msg);
    CHECK(!r.info.tls_info.resumed);          // another server on the same host name: a different session cache key
    done(&r);

    url_tls(u, sizeof(u), P_CLIENTPREF, "/ok");
    sx_tls_set_chacha_first(true);
    r = fetch(u, &rq);
    CHECK_MSG(r.rc == SX_OK && strstr(r.info.tls_info.cipher, "CHACHA20"), "%s", r.info.tls_info.cipher);
    done(&r);
    sx_tls_set_chacha_first(false);
    r = fetch(u, &rq);
    CHECK_MSG(r.rc == SX_OK && strstr(r.info.tls_info.cipher, "AES-128-GCM"), "%s", r.info.tls_info.cipher);
    done(&r);
    sx_tls_set_chacha_first(true);

    char base[100]; snprintf(base, sizeof(base), "https://localhost:%d", P_GOOD);
    test_range(base);
    test_big(base);
    test_pool(base);
    url_tls(u, sizeof(u), P_GOOD, "/chunked");
    r = fetch(u, NULL);
    CHECK(r.rc == SX_OK && r.body && strncmp(r.body, "alpha", 5) == 0 && r.len == 5 + 500 + 5);
    done(&r);
}

static char g_lines[40][160];
static int  g_nlines;
static void nt_line(void *ctx, const char *t) {
    (void)ctx;
    if (g_nlines < 40) snprintf(g_lines[g_nlines++], 160, "%s", t);
    printf("    | %s\n", t);
}
static bool nt_cancel(void *ctx) { (void)ctx; return false; }
static int  nt_idle(void *ctx) { (void)ctx; return 77; }

static void test_nettest(void) {
    sx_nettest_host hosts[] = {
        { "localhost", P_GOOD, "good" },
        { "localhost", P_CHACHA_RSA, "chacha-rsa" },
        { "localhost", P_EXPIRED, "expired" },
    };
    char url[200]; snprintf(url, sizeof(url), "https://localhost:%d/big", P_GOOD);
    sx_nettest_cfg cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.hosts = hosts; cfg.n_hosts = 3; cfg.bulk_url = url; cfg.bulk_secs = 1;
    cfg.line = nt_line; cfg.cancelled = nt_cancel; cfg.idle_pct = nt_idle;
    sx_nettest_result r;
    g_nlines = 0;
    sx_nettest_run(&cfg, &r);
    CHECK(r.handshakes_ok == 2 && r.handshakes_failed == 1);
    CHECK(r.ran_bulk);
    CHECK(r.mbps_chacha > 1.0 && r.mbps_aes > 1.0);
    CHECK(sx_tls_get_mode() == SX_TLS_CHACHA_FIRST || sx_tls_get_mode() == SX_TLS_AES_FIRST);
    CHECK(r.idle_chacha == 77 && r.idle_aes == 77);
    CHECK(!r.clock_suspect);
    int saw_done = 0, saw_clock = 0, saw_resumed = 0, saw_expired = 0;
    for (int i = 0; i < g_nlines; i++) {
        if (!strcmp(g_lines[i], "Done")) saw_done = 1;
        if (!strncmp(g_lines[i], "Clock:", 6)) saw_clock = 1;
        if (strstr(g_lines[i], "resumed")) saw_resumed = 1;
        if (strstr(g_lines[i], "date and time")) saw_expired = 1;
    }
    CHECK(saw_done && saw_clock && saw_resumed && saw_expired);
}

static int run_local(const char *portfile, const char *ca_path) {
    FILE *f = fopen(portfile, "r");
    if (!f) { printf("cannot read %s\n", portfile); return 2; }
    char name[64]; int port;
    while (fscanf(f, "%63s %d", name, &port) == 2) {
        if (!strcmp(name, "plain")) P_PLAIN = port;
        else if (!strcmp(name, "tls_good")) P_GOOD = port;
        else if (!strcmp(name, "tls_expired")) P_EXPIRED = port;
        else if (!strcmp(name, "tls_wronghost")) P_WRONG = port;
        else if (!strcmp(name, "tls_self")) P_SELF = port;
        else if (!strcmp(name, "tls_chacha_rsa")) P_CHACHA_RSA = port;
        else if (!strcmp(name, "tls_aes_ec")) P_AES_EC = port;
        else if (!strcmp(name, "tls_clientpref")) P_CLIENTPREF = port;
    }
    fclose(f);
    FILE *c = fopen(ca_path, "r");
    if (!c) { printf("cannot read %s\n", ca_path); return 2; }
    static char pem[8192]; size_t n = fread(pem, 1, sizeof(pem) - 1, c); pem[n] = 0; fclose(c);

    sx_http_init();
    CHECK(sx_tls_init());
    CHECK(sx_tls_ca_count() > 100);
    CHECK(sx_tls_test_add_ca(pem) == 0);

    test_basic();
    test_post_headers();
    test_redirects();
    test_range_edge();
    char base[100]; snprintf(base, sizeof(base), "http://127.0.0.1:%d", P_PLAIN);
    test_range(base);
    test_big(base);
    test_pool(base);
    test_timeouts_and_sizes();
    test_tls();
    test_nettest();
    return 0;
}

// Real hosts, bundled roots only (no test CA).
static int run_live(void) {
    sx_http_init();
    CHECK(sx_tls_init());
    printf("  roots: %d\n", sx_tls_ca_count());
    const char *urls[] = {
        "https://v3-cinemeta.strem.io/manifest.json",
        "https://v3-cinemeta.strem.io/catalog/movie/top.json",       // 307 to cinemeta-catalogs.strem.io
        "https://link.stremio.com/api/v2/create?type=Create",
        "https://api.strem.io/api/addonCollectionGet",
        "https://images.metahub.space/poster/small/tt0133093/img",
        "https://opensubtitles-v3.strem.io/manifest.json",
        "https://github.com/",
    };
    for (unsigned i = 0; i < sizeof(urls) / sizeof(urls[0]); i++) {
        sx_http_req rq; memset(&rq, 0, sizeof(rq));
        rq.url = urls[i];
        char *body = NULL; size_t len = 0; sx_http_info info;
        const int rc = sx_http_fetch(&rq, 4u << 20, &body, &len, &info);
        char red[200]; sx_redact_url(urls[i], red, sizeof(red));
        printf("  %-62s rc=%d status=%d %s %s hs=%dms redirects=%d len=%zu\n", red, rc, info.status,
               info.tls_info.version, info.tls_info.cipher, info.tls_info.handshake_ms, info.redirects, len);
        CHECK_MSG(rc == SX_OK && info.status > 0 && info.status < 500, "%s", info.err.msg);
        free(body);
    }
    return 0;
}

int main(int argc, char **argv) {
    int rc;
    if (argc == 2 && !strcmp(argv[1], "--live")) rc = run_live();
    else if (argc == 3) rc = run_local(argv[1], argv[2]);
    else { printf("usage: test_sx_net <portfile> <ca.pem> | --live\n"); return 2; }
    printf("test_sx_net: %d checks, %d failed\n", g_run, g_fail);
    return rc ? rc : (g_fail ? 1 : 0);
}
