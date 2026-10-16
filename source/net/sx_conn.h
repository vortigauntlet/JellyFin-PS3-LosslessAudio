// One connection: plain TCP, or TCP + TLS.  Everything above (sx_http) reads
// and writes through this, so HTTP and HTTPS share one code path.
#pragma once
#include <stdbool.h>
#include "sx_plat.h"
#include "sx_tls.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sx_conn sx_conn;

// connect_ms covers resolve + TCP connect; the TLS handshake gets handshake_ms.
// rcvbuf > 0 sets SO_RCVBUF (the video reader).  NULL on failure, with *err filled.
sx_conn *sx_conn_open(const char *host, int port, bool tls, int connect_ms,
                      int handshake_ms, int rcvbuf, sx_neterr *err);

// >0 bytes, 0 = the peer closed, SX_E_TIMEOUT (still usable), or an error.
int  sx_conn_read(sx_conn *c, void *buf, int n, int timeout_ms);
// n on success, or a negative SX_E_*.
int  sx_conn_write(sx_conn *c, const void *buf, int n, int timeout_ms);
void sx_conn_close(sx_conn *c);

// A pooled connection is reusable only while the peer has not closed it.
bool sx_conn_idle_ok(const sx_conn *c);

bool sx_conn_is_tls(const sx_conn *c);
void sx_conn_tls_info(const sx_conn *c, sx_tls_info *out);   // zeroed for plain TCP

#ifdef __cplusplus
}
#endif
