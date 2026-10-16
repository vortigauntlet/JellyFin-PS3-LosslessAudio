// The few platform services the portable network code (sx_conn, sx_tls,
// sx_http) needs.  sx_plat_ps3.cpp implements them on lv2/libnet,
// sx_plat_posix.c on a host so the same TLS and HTTP code runs under test.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Result codes shared by the whole sx_* network layer.  Positive = bytes.
enum {
    SX_OK        =  0,
    SX_E_TIMEOUT = -1,   // nothing arrived / could not send in time; link still usable
    SX_E_CLOSED  = -2,   // orderly close by the peer
    SX_E_IO      = -3,   // socket error
    SX_E_TLS     = -4,   // handshake or record failure
    SX_E_CERT    = -5,   // certificate did not verify (see sx_tls_last_cert_reason)
    SX_E_DNS     = -6,
    SX_E_CONNECT = -7,
    SX_E_MEM     = -8,
    SX_E_PROTO   = -9,   // malformed HTTP
    SX_E_TOOBIG  = -10,  // body above the caller's cap
    SX_E_REDIR   = -11,  // too many redirects, or a refused downgrade
    SX_E_ARG     = -12
};

const char *sx_err_name(int code);

// Why a certificate was refused, for the message shown to the user.
enum { SX_CERT_NONE = 0, SX_CERT_DATE, SX_CERT_HOST, SX_CERT_UNTRUSTED, SX_CERT_OTHER };

// Filled by every call that can fail.  msg never contains a URL path or query.
typedef struct {
    int  code;          // SX_OK or an SX_E_*
    int  cert_reason;   // SX_CERT_* when code == SX_E_CERT
    char msg[160];
} sx_neterr;

// Monotonic microseconds / milliseconds, and sleep.
uint64_t sx_now_us(void);
uint64_t sx_now_ms(void);
void     sx_sleep_ms(unsigned ms);

// One log line (no trailing newline).  Callers must never pass a URL that has
// not been through sx_redact_url().
void sx_log(const char *line);

// Mutex for the few globals shared between threads (RNG, session cache, pool).
typedef struct sx_mutex sx_mutex;
sx_mutex *sx_mutex_create(void);
void      sx_mutex_lock(sx_mutex *m);
void      sx_mutex_unlock(sx_mutex *m);

// A detached worker thread (it is never joined: it ends when fn returns).  Workers that can block
// on the network must read only state guarded by a generation counter or a lock.  False on failure.
bool sx_thread_spawn(void (*fn)(void *), void *arg, int priority, int stack_kb, const char *name);

// Sockets.  fd >= 0 is a connected, non-blocking-capable socket.
// connect: resolves (IPv4) and connects within connect_ms; rcvbuf > 0 sets
// SO_RCVBUF before connect.  Returns fd, or a negative SX_E_*.
int  sx_sock_connect(const char *host, int port, int rcvbuf, int connect_ms);
// recv: >0 bytes, 0 orderly close, SX_E_TIMEOUT, or SX_E_IO.
int  sx_sock_recv(int fd, void *buf, int n, int timeout_ms);
// send: all n bytes or a negative SX_E_*.
int  sx_sock_send(int fd, const void *buf, int n, int timeout_ms);
void sx_sock_close(int fd);
// An idle keep-alive socket is healthy only if nothing is readable: readable
// means the peer closed it (or sent something unexpected).  1 = healthy.
int  sx_sock_idle_ok(int fd);

// Entropy for the TLS RNG: fills buf with up to n bytes of platform entropy
// and returns the count (0 when the platform has none beyond the jitter the
// caller adds itself).
size_t sx_plat_entropy(unsigned char *buf, size_t n);

#ifdef __cplusplus
}
#endif
