// TLS 1.2 client over a connected socket, on mbedTLS 2.28.
//
// Certificates are always verified against the bundled root set and the host
// name; there is no switch that turns that off.  Sessions resume through
// tickets from a small per-host cache.
#pragma once
#include <stdbool.h>
#include "sx_plat.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sx_tls sx_tls;

typedef struct {
    char version[12];       // "TLSv1.2"
    char cipher[56];        // "TLS-ECDHE-ECDSA-WITH-AES-128-GCM-SHA256"
    bool resumed;           // the server accepted a cached session (best effort)
    int  handshake_ms;
} sx_tls_info;

// Builds the RNG and the shared configs and parses the CA bundle; done lazily
// by the first connect.  False when the RNG or the bundle could not be set up.
bool sx_tls_init(void);

// Which ciphers a new connection offers.  The two *_FIRST modes offer everything
// in that order (a server may still choose its own); the *_ONLY modes offer one
// family so a measurement knows what it measured.  Applies to new connections.
typedef enum {
    SX_TLS_CHACHA_FIRST = 0,
    SX_TLS_AES_FIRST    = 1,
    SX_TLS_CHACHA_ONLY  = 2,
    SX_TLS_AES_ONLY     = 3
} sx_tls_mode;

void        sx_tls_set_mode(sx_tls_mode m);
sx_tls_mode sx_tls_get_mode(void);
void        sx_tls_set_chacha_first(bool on);   // CHACHA_FIRST or AES_FIRST

int  sx_tls_ca_count(void);

// Handshake on a connected fd.  The fd stays owned by the caller.
sx_tls *sx_tls_connect(int fd, const char *host, int port, int handshake_ms, sx_neterr *err);

// >0 bytes, 0 = the peer closed, SX_E_TIMEOUT (connection still usable), or an error.
int  sx_tls_read(sx_tls *t, void *buf, int n, int timeout_ms);
// All n bytes or a negative SX_E_*.
int  sx_tls_write(sx_tls *t, const void *buf, int n, int timeout_ms);
void sx_tls_close(sx_tls *t);          // close_notify (best effort) and free; not the fd

void sx_tls_get_info(const sx_tls *t, sx_tls_info *out);

// Drops every cached session (Settings -> Network test uses this to time a cold handshake).
void sx_tls_flush_sessions(void);

#ifdef SX_TEST_HOOKS
// Host test builds only: adds one more trusted root.  Returns 0 on success.
int sx_tls_test_add_ca(const char *pem);
#endif

#ifdef __cplusplus
}
#endif
