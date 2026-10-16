// Console check for the SoundCloud network chain -- see sc_diag.h.
#include "sc_diag.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jf_paths.h"
#include "plog.h"
#include "sc_api.h"
#include "sc_hls.h"
#include "sc_net.h"
#include "sx_http.h"
#include "sx_plat.h"
#include "sx_tls.h"

#define GATE_FILE   "jellyfin_sc_test.txt"
#define RESULT_FILE "jellyfin_sc_test_result.txt"

static FILE *s_out;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    plog(line);
    if (s_out) { fputs(line, s_out); fputc('\n', s_out); fflush(s_out); }
}

static uint64_t since(uint64_t t0) { return sx_now_ms() - t0; }

static void run(void *) {
    s_out = fopen(jf_data_path(RESULT_FILE), "w");
    say("sctest: start");
    sc_net n;
    sc_net_init(&n);
    char *b = NULL, *pl = NULL, *x = NULL;
    size_t bl = 0, pll = 0, xl = 0;
    char *m3u8 = (char *)malloc(SC_URL_MAX);
    char *su = (char *)malloc(SC_URL_MAX);
    char *url = (char *)malloc(SC_URL_MAX);
    sc_hls h;
    bool hls_ok = false;
    if (!m3u8 || !su || !url) { say("sctest: out of memory"); goto done; }

    {
        uint64_t t0 = sx_now_ms();
        const int rc = sc_net_scrape_cid(&n);
        say("sctest: client id rc=%d status=%d err=%d %llu ms", rc, n.last_status, n.last_err, (unsigned long long)since(t0));
        if (rc != SC_NET_OK) goto done;
    }
    {
        sc_url_resolve("https://soundcloud.com/forss/flickermood", SC_CID_PLACEHOLDER, url, SC_URL_MAX);
        const uint64_t t0 = sx_now_ms();
        const int rc = sc_net_api(&n, url, 256 * 1024, &b, &bl);
        say("sctest: resolve rc=%d status=%d err=%d %zu bytes %llu ms", rc, n.last_status, n.last_err, bl, (unsigned long long)since(t0));
        if (rc != SC_NET_OK) goto done;
    }
    sc_track t;
    if (!sc_parse_track(b, bl, 160, &t) || t.hidden != SC_HID_NONE) { say("sctest: track not playable (hidden=%d)", t.hidden); goto done; }
    say("sctest: track ok kind=%d kbps=%d dur=%u ms", t.tc_kind, t.tc_kbps, t.dur_ms);
    {
        const uint64_t t0 = sx_now_ms();
        const int rc = sc_net_stream_url(&n, t.tc_url, m3u8, SC_URL_MAX);
        say("sctest: stream url rc=%d status=%d %llu ms", rc, n.last_status, (unsigned long long)since(t0));
        if (rc != SC_NET_OK) goto done;
    }
    {
        const uint64_t t0 = sx_now_ms();
        const int rc = sc_net_get(&n, m3u8, 2 * 1024 * 1024, &pl, &pll);
        say("sctest: playlist rc=%d status=%d %zu bytes %llu ms", rc, n.last_status, pll, (unsigned long long)since(t0));
        if (rc != SC_NET_OK) goto done;
    }
    {
        const int rc = sc_hls_parse(pl, pll, &h);
        say("sctest: hls rc=%d segments=%d total=%u ms", rc, rc == 0 ? h.nseg : 0, rc == 0 ? h.total_ms : 0);
        if (rc != SC_HLS_OK) goto done;
        hls_ok = true;
    }
    if (sc_hls_seg_url(&h, -1, m3u8, su, SC_URL_MAX)) {
        const int rc = sc_net_get(&n, su, 64 * 1024, &x, &xl);
        say("sctest: init segment rc=%d status=%d %zu bytes", rc, n.last_status, xl);
        free(x); x = NULL;
    }
    if (sc_hls_seg_url(&h, 0, m3u8, su, SC_URL_MAX)) {
        const uint64_t t0 = sx_now_ms();
        const int rc = sc_net_get(&n, su, 1024 * 1024, &x, &xl);
        const uint64_t ms = since(t0);
        say("sctest: segment 0 rc=%d status=%d %zu bytes %llu ms (%.1f kbit/s)", rc, n.last_status, xl, (unsigned long long)ms,
            ms ? (double)xl * 8.0 / (double)ms : 0.0);
    }
done:
    if (hls_ok) sc_hls_free(&h);
    free(b); free(pl); free(x); free(m3u8); free(su); free(url);
    say("sctest: done");
    if (s_out) { fclose(s_out); s_out = NULL; }
}

extern "C" void sc_diag_start(void) {
    const char *gate = jf_data_path(GATE_FILE);
    FILE *f = fopen(gate, "r");
    if (!f) return;
    fclose(f);
    remove(gate);
    sx_thread_spawn(run, NULL, 1500, 256, "sc_diag");
}
