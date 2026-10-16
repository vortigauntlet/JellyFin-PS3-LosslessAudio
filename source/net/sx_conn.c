#include "sx_conn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct sx_conn {
    int     fd;
    sx_tls *tls;
};

const char *sx_err_name(int code) {
    switch (code) {
    case SX_OK:        return "ok";
    case SX_E_TIMEOUT: return "timeout";
    case SX_E_CLOSED:  return "closed";
    case SX_E_IO:      return "io";
    case SX_E_TLS:     return "tls";
    case SX_E_CERT:    return "certificate";
    case SX_E_DNS:     return "dns";
    case SX_E_CONNECT: return "connect";
    case SX_E_MEM:     return "memory";
    case SX_E_PROTO:   return "protocol";
    case SX_E_TOOBIG:  return "too big";
    case SX_E_REDIR:   return "redirect";
    case SX_E_ARG:     return "argument";
    default:           return "error";
    }
}

static void set_err(sx_neterr *e, int code, const char *what, const char *host) {
    if (!e) return;
    e->code = code;
    e->cert_reason = SX_CERT_NONE;
    snprintf(e->msg, sizeof(e->msg), "%s %s", what, host);
}

sx_conn *sx_conn_open(const char *host, int port, bool tls, int connect_ms,
                      int handshake_ms, int rcvbuf, sx_neterr *err) {
    if (err) { err->code = SX_OK; err->cert_reason = SX_CERT_NONE; err->msg[0] = '\0'; }
    const int fd = sx_sock_connect(host, port, rcvbuf, connect_ms);
    if (fd < 0) {
        const char *what = fd == SX_E_DNS ? "could not find" : fd == SX_E_TIMEOUT ? "timed out connecting to" : "could not connect to";
        set_err(err, fd, what, host);
        return NULL;
    }
    sx_conn *c = (sx_conn *)calloc(1, sizeof(*c));
    if (!c) { sx_sock_close(fd); set_err(err, SX_E_MEM, "out of memory for", host); return NULL; }
    c->fd = fd;
    if (tls) {
        c->tls = sx_tls_connect(fd, host, port, handshake_ms, err);
        if (!c->tls) {
            // sx_tls_connect filled err (cert reason included); name the host.
            if (err) {
                char tmp[sizeof(err->msg)];
                snprintf(tmp, sizeof(tmp), "%.60s: %.90s", host, err->msg);
                snprintf(err->msg, sizeof(err->msg), "%s", tmp);
            }
            sx_sock_close(fd);
            free(c);
            return NULL;
        }
    }
    return c;
}

int sx_conn_read(sx_conn *c, void *buf, int n, int timeout_ms) {
    if (!c) return SX_E_ARG;
    return c->tls ? sx_tls_read(c->tls, buf, n, timeout_ms)
                  : sx_sock_recv(c->fd, buf, n, timeout_ms);
}

int sx_conn_write(sx_conn *c, const void *buf, int n, int timeout_ms) {
    if (!c) return SX_E_ARG;
    return c->tls ? sx_tls_write(c->tls, buf, n, timeout_ms)
                  : sx_sock_send(c->fd, buf, n, timeout_ms);
}

void sx_conn_close(sx_conn *c) {
    if (!c) return;
    if (c->tls) sx_tls_close(c->tls);
    sx_sock_close(c->fd);
    free(c);
}

bool sx_conn_idle_ok(const sx_conn *c) { return c && sx_sock_idle_ok(c->fd) == 1; }
bool sx_conn_is_tls(const sx_conn *c) { return c && c->tls; }

void sx_conn_tls_info(const sx_conn *c, sx_tls_info *out) {
    memset(out, 0, sizeof(*out));
    if (c && c->tls) sx_tls_get_info(c->tls, out);
}
