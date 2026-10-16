#include "sx_nettest.h"
#include "sx_conn.h"
#include "sx_http.h"
#include "sx_tls.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void say(const sx_nettest_cfg *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void say(const sx_nettest_cfg *c, const char *fmt, ...) {
    char b[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (c->line) c->line(c->ctx, b);
}

static bool stop(const sx_nettest_cfg *c) { return c->cancelled && c->cancelled(c->ctx); }

static const char *short_cipher(const char *full, char *out, size_t cap) {
    // "TLS-ECDHE-ECDSA-WITH-CHACHA20-POLY1305-SHA256" -> "CHACHA20-POLY1305"
    const char *w = strstr(full, "WITH-");
    snprintf(out, cap, "%s", w ? w + 5 : full);
    char *sha = strstr(out, "-SHA");
    if (sha) *sha = '\0';
    return out;
}

static void handshake_one(const sx_nettest_cfg *c, const sx_nettest_host *h, sx_nettest_result *r) {
    const int port = h->port > 0 ? h->port : 443;
    const char *label = h->label ? h->label : h->host;
    sx_tls_flush_sessions();
    sx_neterr err;
    const uint64_t t0 = sx_now_ms();
    sx_conn *cn = sx_conn_open(h->host, port, true, 8000, 12000, 0, &err);
    const int total = (int)(sx_now_ms() - t0);
    if (!cn) {
        r->handshakes_failed++;
        if (err.code == SX_E_CERT && err.cert_reason == SX_CERT_DATE)
            say(c, "%s: %s - check the PS3's date and time", label, err.msg);
        else
            say(c, "%s: FAILED %s", label, err.msg);
        return;
    }
    sx_tls_info ti;
    sx_conn_tls_info(cn, &ti);
    sx_conn_close(cn);
    char cip[48];
    short_cipher(ti.cipher, cip, sizeof(cip));
    // Second connection: should resume from the ticket just stored.
    const uint64_t t1 = sx_now_ms();
    sx_conn *c2 = sx_conn_open(h->host, port, true, 8000, 12000, 0, &err);
    const int total2 = (int)(sx_now_ms() - t1);
    sx_tls_info t2;
    memset(&t2, 0, sizeof(t2));
    if (c2) { sx_conn_tls_info(c2, &t2); sx_conn_close(c2); }
    r->handshakes_ok++;
    say(c, "%s: %s %s  cold %d ms (tcp+tls)  warm %d ms%s", label, ti.version, cip, total, total2,
        t2.resumed ? " resumed" : "");
}

typedef struct { double mbps, min_mbps; int idle; bool ok; } bulk_res;

static bulk_res bulk_run(const sx_nettest_cfg *c, bool chacha, int secs, int rcvbuf) {
    bulk_res br = { 0, 0, -1, false };
    const char *name = chacha ? "ChaCha20" : "AES-GCM";
    sx_tls_set_mode(chacha ? SX_TLS_CHACHA_ONLY : SX_TLS_AES_ONLY);
    sx_tls_flush_sessions();
    sx_http_pool_flush();

    sx_http_req rq;
    memset(&rq, 0, sizeof(rq));
    rq.url = c->bulk_url;
    rq.no_pool = true;
    rq.rcvbuf = rcvbuf;
    rq.first_byte_ms = 15000;
    sx_http_info info;
    sx_http *h = sx_http_open(&rq, &info);
    if (!h) { say(c, "%s: could not start (%s)", name, info.err.msg); return br; }
    if (info.status != 200 && info.status != 206) {
        say(c, "%s: server answered %d", name, info.status);
        sx_http_close(h);
        return br;
    }
    char cip[48];
    short_cipher(info.tls_info.cipher, cip, sizeof(cip));

    static unsigned char buf[65536];
    if (c->idle_pct) (void)c->idle_pct(c->ctx);          // start the idle window now
    const uint64_t t0 = sx_now_ms();
    uint64_t win_t = t0;
    long long total = 0, win = 0;
    double slowest = 1e9;
    int windows = 0;
    bool aborted = false;
    for (;;) {
        const uint64_t now = sx_now_ms();
        if (now - t0 >= (uint64_t)secs * 1000u) break;
        if (stop(c)) { aborted = true; break; }
        const int n = sx_http_read(h, buf, sizeof(buf));
        if (n > 0) { total += n; win += n; }
        else if (n == 0) break;                       // the file ended before the time did
        else if (n != SX_E_TIMEOUT) { say(c, "%s: read failed (%s)", name, sx_err_name(n)); break; }
        const uint64_t t = sx_now_ms();
        if (t - win_t >= 1000u) {
            const double mbps = (double)win * 8.0 / 1e6 / ((double)(t - win_t) / 1000.0);
            if (windows > 0 && mbps < slowest) slowest = mbps;     // the first window includes slow start
            windows++;
            win = 0;
            win_t = t;
        }
    }
    const double secs_run = (double)(sx_now_ms() - t0) / 1000.0;
    if (c->idle_pct) br.idle = c->idle_pct(c->ctx);
    sx_http_close(h);
    if (aborted || total <= 0) { say(c, "%s: stopped", name); return br; }
    br.mbps = (double)total * 8.0 / 1e6 / (secs_run > 0.001 ? secs_run : 0.001);
    br.min_mbps = slowest > 1e8 ? br.mbps : slowest;
    br.ok = true;
    char idle[24] = "";
    if (br.idle >= 0) snprintf(idle, sizeof(idle), "  PPU idle %d%%", br.idle);
    say(c, "%s (%s): %.1f Mbps avg, slowest second %.1f, %.1f MB in %.0f s%s", name, cip, br.mbps,
        br.min_mbps, (double)total / 1e6, secs_run, idle);
    return br;
}

void sx_nettest_run(const sx_nettest_cfg *c, sx_nettest_result *out) {
    sx_nettest_result r;
    memset(&r, 0, sizeof(r));
    r.idle_chacha = r.idle_aes = -1;

    time_t now = time(NULL);
    struct tm tmv;
    char date[40] = "unknown";
    if (gmtime_r(&now, &tmv)) {
        strftime(date, sizeof(date), "%Y-%m-%d %H:%M UTC", &tmv);
        if (tmv.tm_year + 1900 < 2025) r.clock_suspect = true;
    }
    say(c, "Clock: %s%s", date, r.clock_suspect ? "  (looks wrong: certificates will fail)" : "");

    const uint64_t t0 = sx_now_ms();
    if (!sx_tls_init()) { say(c, "TLS could not start"); if (out) *out = r; return; }
    unsigned char probe[32];
    const size_t hw = sx_plat_entropy(probe, sizeof(probe));
    say(c, "TLS ready: %d trusted roots (%d ms), console RNG %s", sx_tls_ca_count(),
        (int)(sx_now_ms() - t0), hw >= sizeof(probe) ? "ok" : "unavailable (timing only)");

    const sx_tls_mode mode_before = sx_tls_get_mode();
    for (int i = 0; i < c->n_hosts && !stop(c); i++) handshake_one(c, &c->hosts[i], &r);
    say(c, "Handshakes: %d ok, %d failed", r.handshakes_ok, r.handshakes_failed);

    if (c->bulk_url && c->bulk_url[0] && !stop(c)) {
        const int secs = c->bulk_secs > 0 ? c->bulk_secs : 20;
        const int rb = c->rcvbuf > 0 ? c->rcvbuf : 512 * 1024;
        bulk_res a = bulk_run(c, true, secs, rb);
        bulk_res b = { 0, 0, -1, false };
        if (!stop(c)) b = bulk_run(c, false, secs, rb);
        r.ran_bulk = a.ok || b.ok;
        r.mbps_chacha = a.mbps; r.min_chacha = a.min_mbps; r.idle_chacha = a.idle;
        r.mbps_aes = b.mbps;    r.min_aes = b.min_mbps;    r.idle_aes = b.idle;
        // Keep the faster family as the preferred order for the rest of the session
        // when both were measured; otherwise put back what the caller had.
        if (a.ok && b.ok) sx_tls_set_mode(a.mbps >= b.mbps ? SX_TLS_CHACHA_FIRST : SX_TLS_AES_FIRST);
        else              sx_tls_set_mode(mode_before);
        if (a.ok || b.ok) {
            const double best = a.mbps > b.mbps ? a.mbps : b.mbps;
            const double slow = (a.mbps > b.mbps ? a.min_mbps : b.min_mbps);
            say(c, "Faster order: %s.  Video needs about 20-25 Mbps; this is %s",
                a.mbps >= b.mbps ? "ChaCha20 first" : "AES-GCM first",
                (best >= 35.0 && slow >= 25.0) ? "comfortable headroom" : best >= 25.0 ? "marginal" : "NOT enough");
        }
    } else if (!c->bulk_url || !c->bulk_url[0]) {
        say(c, "No download URL set: throughput not measured");
    }
    say(c, "Done");
    if (out) *out = r;
}
