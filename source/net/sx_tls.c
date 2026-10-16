#include "sx_tls.h"
#include "sx_cacert.h"
#include "sx_httpparse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/sha256.h"
#include "mbedtls/error.h"

#ifndef MBEDTLS_ERR_NET_RECV_FAILED
#define MBEDTLS_ERR_NET_RECV_FAILED (-0x004C)
#endif
#ifndef MBEDTLS_ERR_NET_SEND_FAILED
#define MBEDTLS_ERR_NET_SEND_FAILED (-0x004E)
#endif

#define SESS_N 8

struct sx_tls {
    mbedtls_ssl_context ssl;
    int  fd;
    int  io_timeout_ms;
    bool dead;
    sx_tls_info info;
};

typedef struct {
    bool                used;
    char                host[SX_HOST_MAX + 8];   // "host:port"
    mbedtls_ssl_session sess;
    uint64_t            stamp;
} sess_slot;

// ---- shared state, built once --------------------------------------------

static sx_mutex             *s_mtx;
static volatile int          s_state;          // 0 untried, 1 ready, -1 failed
static mbedtls_ctr_drbg_context s_drbg;
static mbedtls_x509_crt      s_ca;
static int                   s_ca_n;
static mbedtls_ssl_config    s_conf[4];        // indexed by sx_tls_mode
static volatile int          s_mode = SX_TLS_CHACHA_FIRST;
static sess_slot             s_sess[SESS_N];
static uint64_t              s_sess_clock;

// Held across init so two threads never build the bundle twice.
static sx_mutex *init_mutex(void) {
    // The first caller creates it; creation itself is not raced because the
    // app calls sx_tls_init() from its single-threaded start (net_init) before
    // any worker exists.  A late first call from a worker is still safe on
    // PS3 because s_mtx is created before s_state is published.
    if (!s_mtx) s_mtx = sx_mutex_create();
    return s_mtx;
}

// ---- entropy ---------------------------------------------------------------

// Mixes whatever the platform offers with timing jitter through SHA-256.  The
// platform source (the console's RNG) is the real entropy; the jitter only
// stops two identical boots from producing identical output if that source is
// refused.
static int entropy_cb(void *ctx, unsigned char *out, size_t len) {
    (void)ctx;
    static int warned;
    static uint64_t counter;
    size_t done = 0;
    while (done < len) {
        unsigned char hw[64];
        size_t hn = sx_plat_entropy(hw, sizeof(hw));
        if (hn == 0 && !warned) {
            warned = 1;
            sx_log("tls: platform RNG unavailable, seeding from timing jitter only");
        }
        uint64_t jit[32];
        uint64_t acc = sx_now_us();
        for (int i = 0; i < 32; i++) {
            volatile unsigned spin = 0;
            for (int k = 0; k < 64; k++) spin += (unsigned)k * 2654435761u;
            const uint64_t t = sx_now_us();
            acc = acc * 6364136223846793005ULL + t + spin;
            jit[i] = acc ^ (t << 17);
        }
        mbedtls_sha256_context sh;
        mbedtls_sha256_init(&sh);
        mbedtls_sha256_starts_ret(&sh, 0);
        mbedtls_sha256_update_ret(&sh, hw, hn);
        mbedtls_sha256_update_ret(&sh, (const unsigned char *)jit, sizeof(jit));
        counter++;
        mbedtls_sha256_update_ret(&sh, (const unsigned char *)&counter, sizeof(counter));
        const void *sp = &sh;
        mbedtls_sha256_update_ret(&sh, (const unsigned char *)&sp, sizeof(sp));
        unsigned char dig[32];
        mbedtls_sha256_finish_ret(&sh, dig);
        mbedtls_sha256_free(&sh);
        const size_t n = (len - done < sizeof(dig)) ? len - done : sizeof(dig);
        memcpy(out + done, dig, n);
        done += n;
    }
    return 0;
}

static int rng_cb(void *ctx, unsigned char *out, size_t len) {
    (void)ctx;
    sx_mutex_lock(s_mtx);
    const int r = mbedtls_ctr_drbg_random(&s_drbg, out, len);
    sx_mutex_unlock(s_mtx);
    return r;
}

// ---- configs -----------------------------------------------------------------

static const int k_suites_chacha[] = {
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
    0
};
static const int k_suites_aes[] = {
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
    0
};
// Single-family lists for the Network test, which must be sure which cipher a
// run measured: a server with its own preference ignores the client's order.
static const int k_suites_chacha_only[] = {
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
    0
};
static const int k_suites_aes_only[] = {
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    0
};
static const mbedtls_ecp_group_id k_curves[] = {
    MBEDTLS_ECP_DP_CURVE25519, MBEDTLS_ECP_DP_SECP256R1, MBEDTLS_ECP_DP_SECP384R1,
    MBEDTLS_ECP_DP_NONE
};

static bool build_conf(mbedtls_ssl_config *c, const int *suites) {
    mbedtls_ssl_config_init(c);
    if (mbedtls_ssl_config_defaults(c, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) return false;
    mbedtls_ssl_conf_min_version(c, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_max_version(c, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_authmode(c, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(c, &s_ca, NULL);
    mbedtls_ssl_conf_rng(c, rng_cb, NULL);
    mbedtls_ssl_conf_ciphersuites(c, suites);
    mbedtls_ssl_conf_curves(c, k_curves);
    mbedtls_ssl_conf_session_tickets(c, MBEDTLS_SSL_SESSION_TICKETS_ENABLED);
    return true;
}

bool sx_tls_init(void) {
    if (s_state == 1) return true;
    if (s_state < 0) return false;
    sx_mutex *m = init_mutex();
    if (!m) { s_state = -1; return false; }
    sx_mutex_lock(m);
    if (s_state != 0) { sx_mutex_unlock(m); return s_state == 1; }

    const uint64_t t0 = sx_now_ms();
    bool ok = false;
    mbedtls_ctr_drbg_init(&s_drbg);
    mbedtls_x509_crt_init(&s_ca);
    do {
        if (mbedtls_ctr_drbg_seed(&s_drbg, entropy_cb, NULL, (const unsigned char *)"sx", 2) != 0) {
            sx_log("tls: RNG seed failed");
            break;
        }
        // The bundle is NUL-terminated PEM; the return value is how many
        // certificates failed to parse (a few exotic ones are expected).
        const int failed = mbedtls_x509_crt_parse(&s_ca, (const unsigned char *)sx_cacert_pem,
                                                  sx_cacert_pem_len + 1);
        if (failed < 0) { sx_log("tls: CA bundle parse failed"); break; }
        int n = 0;
        for (const mbedtls_x509_crt *c = &s_ca; c && c->raw.p; c = c->next) n++;
        s_ca_n = n;
        if (n == 0) { sx_log("tls: CA bundle holds no certificates"); break; }
        if (!build_conf(&s_conf[SX_TLS_CHACHA_FIRST], k_suites_chacha)) break;
        if (!build_conf(&s_conf[SX_TLS_AES_FIRST], k_suites_aes)) break;
        if (!build_conf(&s_conf[SX_TLS_CHACHA_ONLY], k_suites_chacha_only)) break;
        if (!build_conf(&s_conf[SX_TLS_AES_ONLY], k_suites_aes_only)) break;
        ok = true;
    } while (0);

    char b[96];
    snprintf(b, sizeof(b), "tls: init %s, %d roots, %u ms", ok ? "ok" : "FAILED", s_ca_n,
             (unsigned)(sx_now_ms() - t0));
    sx_log(b);
    s_state = ok ? 1 : -1;
    sx_mutex_unlock(m);
    return ok;
}

void sx_tls_set_mode(sx_tls_mode m) {
    if ((int)m < 0 || (int)m > SX_TLS_AES_ONLY || s_mode == (int)m) return;
    s_mode = (int)m;
    sx_tls_flush_sessions();     // a resumed session keeps the cipher it was negotiated with
}
sx_tls_mode sx_tls_get_mode(void) { return (sx_tls_mode)s_mode; }
void sx_tls_set_chacha_first(bool on) { sx_tls_set_mode(on ? SX_TLS_CHACHA_FIRST : SX_TLS_AES_FIRST); }
int  sx_tls_ca_count(void) { return s_ca_n; }

// ---- sessions ------------------------------------------------------------------

static sess_slot *sess_find(const char *key) {
    for (int i = 0; i < SESS_N; i++)
        if (s_sess[i].used && strcmp(s_sess[i].host, key) == 0) return &s_sess[i];
    return NULL;
}

static void sess_store(const char *key, const mbedtls_ssl_context *ssl) {
    sx_mutex_lock(s_mtx);
    sess_slot *slot = sess_find(key);
    if (!slot) {
        slot = &s_sess[0];
        for (int i = 0; i < SESS_N; i++) {
            if (!s_sess[i].used) { slot = &s_sess[i]; break; }
            if (s_sess[i].stamp < slot->stamp) slot = &s_sess[i];
        }
    }
    if (slot->used) mbedtls_ssl_session_free(&slot->sess);
    mbedtls_ssl_session_init(&slot->sess);
    if (mbedtls_ssl_get_session(ssl, &slot->sess) == 0) {
        slot->used = true;
        snprintf(slot->host, sizeof(slot->host), "%s", key);
        slot->stamp = ++s_sess_clock;
    } else {
        mbedtls_ssl_session_free(&slot->sess);
        slot->used = false;
    }
    sx_mutex_unlock(s_mtx);
}

static void sess_drop(const char *key) {
    sx_mutex_lock(s_mtx);
    sess_slot *slot = sess_find(key);
    if (slot) { mbedtls_ssl_session_free(&slot->sess); slot->used = false; }
    sx_mutex_unlock(s_mtx);
}

void sx_tls_flush_sessions(void) {
    if (s_state != 1) return;
    sx_mutex_lock(s_mtx);
    for (int i = 0; i < SESS_N; i++)
        if (s_sess[i].used) { mbedtls_ssl_session_free(&s_sess[i].sess); s_sess[i].used = false; }
    sx_mutex_unlock(s_mtx);
}

// ---- BIO ---------------------------------------------------------------------

static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    sx_tls *t = (sx_tls *)ctx;
    const int r = sx_sock_recv(t->fd, buf, (int)len, t->io_timeout_ms);
    if (r > 0) return r;
    if (r == 0) return 0;
    if (r == SX_E_TIMEOUT) return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    sx_tls *t = (sx_tls *)ctx;
    const int r = sx_sock_send(t->fd, buf, (int)len, t->io_timeout_ms);
    if (r > 0) return r;
    if (r == SX_E_TIMEOUT) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

// ---- connect -----------------------------------------------------------------

static void fill_err(sx_neterr *e, int code, int cert, const char *msg) {
    if (!e) return;
    e->code = code;
    e->cert_reason = cert;
    snprintf(e->msg, sizeof(e->msg), "%s", msg);
}

sx_tls *sx_tls_connect(int fd, const char *host, int port, int handshake_ms, sx_neterr *err) {
    if (!sx_tls_init()) {
        fill_err(err, SX_E_TLS, SX_CERT_NONE, "TLS is not available");
        return NULL;
    }
    sx_tls *t = (sx_tls *)calloc(1, sizeof(*t));
    if (!t) { fill_err(err, SX_E_MEM, SX_CERT_NONE, "out of memory"); return NULL; }
    t->fd = fd;
    t->io_timeout_ms = 1000;
    mbedtls_ssl_init(&t->ssl);

    char key[SX_HOST_MAX + 8];
    snprintf(key, sizeof(key), "%s:%d", host, port);
    bool tried_resume = false;
    unsigned char master_before[48];
    int ret = mbedtls_ssl_setup(&t->ssl, &s_conf[s_mode]);
    if (ret == 0) ret = mbedtls_ssl_set_hostname(&t->ssl, host);
    if (ret != 0) {
        char eb[80], m[128];
        mbedtls_strerror(ret, eb, sizeof(eb));
        snprintf(m, sizeof(m), "TLS setup failed (-0x%04X: %s)", -ret, eb);
        fill_err(err, SX_E_TLS, SX_CERT_NONE, m);
        mbedtls_ssl_free(&t->ssl); free(t);
        return NULL;
    }
    mbedtls_ssl_set_bio(&t->ssl, t, bio_send, bio_recv, NULL);

    sx_mutex_lock(s_mtx);
    sess_slot *slot = sess_find(key);
    if (slot && mbedtls_ssl_set_session(&t->ssl, &slot->sess) == 0) {
        tried_resume = true;
        memcpy(master_before, slot->sess.master, sizeof(master_before));
    }
    sx_mutex_unlock(s_mtx);

    const uint64_t t0 = sx_now_ms();
    const uint64_t deadline = t0 + (uint64_t)handshake_ms;
    for (;;) {
        const uint64_t now = sx_now_ms();
        if (now >= deadline) { ret = MBEDTLS_ERR_SSL_TIMEOUT; break; }
        const uint64_t left = deadline - now;
        t->io_timeout_ms = left > 1000 ? 1000 : (int)left;
        ret = mbedtls_ssl_handshake(&t->ssl);
        if (ret == 0) break;
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) break;
    }

    if (ret != 0) {
        if (tried_resume) sess_drop(key);
        if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
            const uint32_t flags = mbedtls_ssl_get_verify_result(&t->ssl);
            int reason = SX_CERT_OTHER;
            if (flags & (MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE)) reason = SX_CERT_DATE;
            else if (flags & MBEDTLS_X509_BADCERT_CN_MISMATCH) reason = SX_CERT_HOST;
            else if (flags & (MBEDTLS_X509_BADCERT_NOT_TRUSTED | MBEDTLS_X509_BADCRL_NOT_TRUSTED)) reason = SX_CERT_UNTRUSTED;
            char info[120];
            mbedtls_x509_crt_verify_info(info, sizeof(info), "", flags);
            for (char *p = info; *p; p++) if (*p == '\n') *p = ' ';
            char m[160];
            snprintf(m, sizeof(m), "certificate refused: %s", info);
            fill_err(err, SX_E_CERT, reason, m);
        } else if (ret == MBEDTLS_ERR_SSL_TIMEOUT) {
            fill_err(err, SX_E_TIMEOUT, SX_CERT_NONE, "TLS handshake timed out");
        } else {
            char eb[80], m[128];
            mbedtls_strerror(ret, eb, sizeof(eb));
            snprintf(m, sizeof(m), "TLS handshake failed (-0x%04X: %s)", -ret, eb);
            fill_err(err, SX_E_TLS, SX_CERT_NONE, m);
        }
        mbedtls_ssl_free(&t->ssl);
        free(t);
        return NULL;
    }

    t->info.handshake_ms = (int)(sx_now_ms() - t0);
    snprintf(t->info.version, sizeof(t->info.version), "%s", mbedtls_ssl_get_version(&t->ssl));
    snprintf(t->info.cipher, sizeof(t->info.cipher), "%s", mbedtls_ssl_get_ciphersuite(&t->ssl));
    // A resumed session keeps its master secret; a full handshake always derives a new one.
    if (tried_resume && t->ssl.session && memcmp(t->ssl.session->master, master_before, sizeof(master_before)) == 0)
        t->info.resumed = true;
    sess_store(key, &t->ssl);
    return t;
}

// ---- I/O -----------------------------------------------------------------------

int sx_tls_read(sx_tls *t, void *buf, int n, int timeout_ms) {
    if (!t || t->dead) return SX_E_CLOSED;
    t->io_timeout_ms = timeout_ms;
    for (int tries = 0; tries < 2; tries++) {
        const int r = mbedtls_ssl_read(&t->ssl, (unsigned char *)buf, (size_t)n);
        if (r > 0) return r;
        if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == MBEDTLS_ERR_SSL_CONN_EOF) {
            t->dead = true;
            return 0;
        }
        if (r == MBEDTLS_ERR_SSL_WANT_READ) return SX_E_TIMEOUT;
        if (r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        t->dead = true;
        return SX_E_TLS;
    }
    return SX_E_TIMEOUT;
}

int sx_tls_write(sx_tls *t, const void *buf, int n, int timeout_ms) {
    if (!t || t->dead) return SX_E_CLOSED;
    const uint64_t deadline = sx_now_ms() + (uint64_t)timeout_ms;
    int sent = 0;
    while (sent < n) {
        const uint64_t now = sx_now_ms();
        if (now >= deadline) return SX_E_TIMEOUT;
        const uint64_t left = deadline - now;
        t->io_timeout_ms = left > 1000 ? 1000 : (int)left;
        const int r = mbedtls_ssl_write(&t->ssl, (const unsigned char *)buf + sent, (size_t)(n - sent));
        if (r > 0) { sent += r; continue; }
        if (r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_WANT_READ) continue;
        t->dead = true;
        return SX_E_TLS;
    }
    return n;
}

void sx_tls_close(sx_tls *t) {
    if (!t) return;
    if (!t->dead) {
        t->io_timeout_ms = 200;
        mbedtls_ssl_close_notify(&t->ssl);
    }
    mbedtls_ssl_free(&t->ssl);
    free(t);
}

void sx_tls_get_info(const sx_tls *t, sx_tls_info *out) {
    if (t && out) *out = t->info;
}

#ifdef SX_TEST_HOOKS
// Test builds only: trust one more root (never disables verification).
int sx_tls_test_add_ca(const char *pem) {
    if (!sx_tls_init()) return -1;
    sx_mutex_lock(s_mtx);
    const int r = mbedtls_x509_crt_parse(&s_ca, (const unsigned char *)pem, strlen(pem) + 1);
    sx_mutex_unlock(s_mtx);
    return r;
}
#endif
