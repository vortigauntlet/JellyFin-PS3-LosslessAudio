#include "sx_http.h"
#include "sx_conn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEAD_BUF       16384
#define POOL_MAX       8
#define POOL_PER_HOST  2
#define POOL_IDLE_MS   30000

enum { FR_NONE, FR_LENGTH, FR_CHUNKED, FR_CLOSE };

struct sx_http {
    sx_conn  *conn;
    bool      keepalive;     // the response allows reuse
    bool      no_pool;
    int       idle_ms;
    sx_url    url;           // the URL of the connection (pool key)
    int       mode;
    int64_t   left;
    sx_chunk  chunk;
    bool      done;          // the body was read to its end
    bool      failed;
    int       pos, len;      // unread span of buf
    uint8_t   buf[HEAD_BUF];
};

// ---- pool ------------------------------------------------------------------------

typedef struct {
    sx_conn *c;
    char     host[SX_HOST_MAX];
    int      port;
    bool     tls;
    uint64_t since;
} pool_ent;

static pool_ent  s_pool[POOL_MAX];
static sx_mutex *s_pmtx;
static char      s_ua[96] = "StremioPS3/0.1";

void sx_http_init(void) {
    if (!s_pmtx) s_pmtx = sx_mutex_create();
}

void sx_http_set_user_agent(const char *ua) {
    if (ua && *ua) snprintf(s_ua, sizeof(s_ua), "%s", ua);
}

static void pool_evict_locked(uint64_t now) {
    for (int i = 0; i < POOL_MAX; i++) {
        if (s_pool[i].c && now - s_pool[i].since > POOL_IDLE_MS) {
            sx_conn_close(s_pool[i].c);
            s_pool[i].c = NULL;
        }
    }
}

static sx_conn *pool_get(const sx_url *u) {
    if (!s_pmtx) return NULL;
    for (;;) {
        sx_conn *got = NULL;
        sx_mutex_lock(s_pmtx);
        pool_evict_locked(sx_now_ms());
        // Most recently idle first: the freshest connection is the likeliest to be alive.
        int best = -1;
        for (int i = 0; i < POOL_MAX; i++) {
            pool_ent *e = &s_pool[i];
            if (e->c && e->port == u->port && e->tls == u->tls && strcmp(e->host, u->host) == 0 &&
                (best < 0 || e->since > s_pool[best].since))
                best = i;
        }
        if (best >= 0) { got = s_pool[best].c; s_pool[best].c = NULL; }
        sx_mutex_unlock(s_pmtx);
        if (!got) return NULL;
        if (sx_conn_idle_ok(got)) return got;
        sx_conn_close(got);
    }
}

static void pool_put(sx_conn *c, const sx_url *u) {
    if (!s_pmtx) { sx_conn_close(c); return; }
    sx_conn *drop = NULL, *drop2 = NULL;
    sx_mutex_lock(s_pmtx);
    pool_evict_locked(sx_now_ms());
    int same = 0, oldest_same = -1, oldest = -1, empty = -1;
    for (int i = 0; i < POOL_MAX; i++) {
        pool_ent *e = &s_pool[i];
        if (!e->c) { if (empty < 0) empty = i; continue; }
        if (oldest < 0 || e->since < s_pool[oldest].since) oldest = i;
        if (e->port == u->port && e->tls == u->tls && strcmp(e->host, u->host) == 0) {
            same++;
            if (oldest_same < 0 || e->since < s_pool[oldest_same].since) oldest_same = i;
        }
    }
    if (same >= POOL_PER_HOST && oldest_same >= 0) {
        drop = s_pool[oldest_same].c;
        s_pool[oldest_same].c = NULL;
        if (empty < 0) empty = oldest_same;
    }
    if (empty < 0 && oldest >= 0) {
        drop2 = s_pool[oldest].c;
        s_pool[oldest].c = NULL;
        empty = oldest;
    }
    if (empty >= 0) {
        pool_ent *e = &s_pool[empty];
        e->c = c;
        snprintf(e->host, sizeof(e->host), "%s", u->host);
        e->port = u->port;
        e->tls = u->tls;
        e->since = sx_now_ms();
        c = NULL;
    }
    sx_mutex_unlock(s_pmtx);
    if (drop) sx_conn_close(drop);
    if (drop2) sx_conn_close(drop2);
    if (c) sx_conn_close(c);
}

void sx_http_pool_flush(void) {
    if (!s_pmtx) return;
    for (int i = 0; i < POOL_MAX; i++) {
        sx_conn *c = NULL;
        sx_mutex_lock(s_pmtx);
        c = s_pool[i].c;
        s_pool[i].c = NULL;
        sx_mutex_unlock(s_pmtx);
        if (c) sx_conn_close(c);
    }
}

// ---- request ------------------------------------------------------------------------

static void set_fail(sx_http_info *info, int code, const char *msg) {
    info->err.code = code;
    info->err.cert_reason = SX_CERT_NONE;
    snprintf(info->err.msg, sizeof(info->err.msg), "%s", msg);
}

static int build_request(char *out, int cap, const sx_http_req *rq, const sx_url *u,
                         bool with_extra, bool keepalive) {
    const char *method = rq->method && *rq->method ? rq->method : "GET";
    const int def = u->tls ? 443 : 80;
    int n = snprintf(out, (size_t)cap, "%s %s HTTP/1.1\r\n", method, u->path);
    if (u->port == def) n += snprintf(out + n, (size_t)(cap - n), "Host: %s\r\n", u->host);
    else                n += snprintf(out + n, (size_t)(cap - n), "Host: %s:%d\r\n", u->host, u->port);
    n += snprintf(out + n, (size_t)(cap - n),
                  "User-Agent: %s\r\nAccept: */*\r\nAccept-Encoding: identity\r\n", s_ua);
    if (rq->has_range && rq->range_start >= 0) {
        if (rq->range_end >= 0)
            n += snprintf(out + n, (size_t)(cap - n), "Range: bytes=%lld-%lld\r\n",
                          (long long)rq->range_start, (long long)rq->range_end);
        else
            n += snprintf(out + n, (size_t)(cap - n), "Range: bytes=%lld-\r\n", (long long)rq->range_start);
    }
    if (with_extra && rq->headers && *rq->headers)
        n += snprintf(out + n, (size_t)(cap - n), "%s", rq->headers);
    if (rq->body_len > 0)
        n += snprintf(out + n, (size_t)(cap - n), "Content-Type: %s\r\nContent-Length: %d\r\n",
                      rq->content_type ? rq->content_type : "application/json", rq->body_len);
    else if (strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0)
        n += snprintf(out + n, (size_t)(cap - n), "Content-Length: 0\r\n");
    n += snprintf(out + n, (size_t)(cap - n), "Connection: %s\r\n\r\n", keepalive ? "keep-alive" : "close");
    return n < cap ? n : -1;
}

static bool is_redirect(int s) { return s == 301 || s == 302 || s == 303 || s == 307 || s == 308; }

// Reads until the head parses.  Returns 1 ok, or a negative SX_E_*.
// *saw_bytes tells the caller whether anything arrived (a stale pooled
// connection fails with none).
static int read_head(sx_http *h, int first_byte_ms, int idle_ms, sx_head *head, bool *saw_bytes) {
    const uint64_t deadline = sx_now_ms() + (uint64_t)first_byte_ms;
    h->pos = h->len = 0;
    for (;;) {
        int rc = sx_head_parse((const char *)h->buf, h->len, head);
        if (rc == 0) return SX_E_PROTO;
        if (rc == 1) {
            if (head->status >= 100 && head->status < 200 && head->status != 101) {
                // Interim response (100 Continue): drop it and read the real head.
                memmove(h->buf, h->buf + head->head_len, (size_t)(h->len - head->head_len));
                h->len -= head->head_len;
                continue;
            }
            h->pos = head->head_len;
            return 1;
        }
        if (h->len >= HEAD_BUF) return SX_E_PROTO;
        const uint64_t now = sx_now_ms();
        int wait;
        if (h->len == 0) {
            if (now >= deadline) return SX_E_TIMEOUT;
            const uint64_t left = deadline - now;
            wait = left > 1000 ? 1000 : (int)left;
        } else {
            wait = idle_ms;
        }
        const int r = sx_conn_read(h->conn, h->buf + h->len, HEAD_BUF - h->len, wait);
        if (r > 0) { h->len += r; *saw_bytes = true; continue; }
        if (r == SX_E_TIMEOUT) {
            if (h->len == 0) continue;        // still inside the first-byte budget
            return SX_E_TIMEOUT;              // stalled mid-head
        }
        return r == 0 ? SX_E_CLOSED : r;
    }
}

sx_http *sx_http_open(const sx_http_req *rq, sx_http_info *info) {
    memset(info, 0, sizeof(*info));
    info->content_length = -1;
    info->range_start = info->range_end = info->range_total = -1;
    info->retry_after = -1;
    if (!rq || !rq->url) { set_fail(info, SX_E_ARG, "no URL"); return NULL; }

    const int connect_ms = rq->connect_ms > 0 ? rq->connect_ms : 8000;
    const int hs_ms = rq->handshake_ms > 0 ? rq->handshake_ms : 12000;
    const int fb_ms = rq->first_byte_ms > 0 ? rq->first_byte_ms : 24000;
    const int idle_ms = rq->idle_ms > 0 ? rq->idle_ms : 8000;
    const int max_redir = rq->max_redirects > 0 ? rq->max_redirects : 5;
    const char *method = rq->method && *rq->method ? rq->method : "GET";
    const bool follows = strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0;

    sx_url first, cur;
    if (!sx_url_parse(rq->url, &first)) { set_fail(info, SX_E_ARG, "bad URL"); return NULL; }
    cur = first;

    sx_http *h = (sx_http *)calloc(1, sizeof(*h));
    char *req = (char *)malloc(SX_URL_MAX + 4096);
    if (!h || !req) { free(h); free(req); set_fail(info, SX_E_MEM, "out of memory"); return NULL; }
    h->idle_ms = idle_ms;
    h->no_pool = rq->no_pool;

    int redirects = 0;
    for (;;) {
        const bool same_origin = (cur.tls == first.tls && cur.port == first.port &&
                                  strcmp(cur.host, first.host) == 0);
        bool reused = false;
        sx_conn *conn = rq->no_pool ? NULL : pool_get(&cur);
        if (conn) reused = true;
        int retried = 0;
        sx_head head;
        for (;;) {   // one attempt per pass; a stale pooled connection gets one fresh retry
            if (!conn) {
                const uint64_t t0 = sx_now_ms();
                conn = sx_conn_open(cur.host, cur.port, cur.tls, connect_ms, hs_ms, rq->rcvbuf, &info->err);
                if (!conn) { free(req); free(h); return NULL; }
                info->connect_ms = (int)(sx_now_ms() - t0);
                reused = false;
            } else if (reused) {
                info->connect_ms = 0;
            }
            h->conn = conn;
            const int rl = build_request(req, SX_URL_MAX + 4096, rq, &cur, same_origin, !rq->no_pool);
            if (rl < 0) { sx_conn_close(conn); set_fail(info, SX_E_ARG, "request too large"); free(req); free(h); return NULL; }

            const uint64_t t_send = sx_now_ms();
            int w = sx_conn_write(conn, req, rl, idle_ms);
            if (w == rl && rq->body_len > 0) w = sx_conn_write(conn, rq->body, rq->body_len, idle_ms);
            bool saw = false;
            int rc = (w >= 0) ? read_head(h, fb_ms, idle_ms, &head, &saw) : w;
            if (rc == 1) { info->ttfb_ms = (int)(sx_now_ms() - t_send); break; }

            sx_conn_close(conn);
            conn = NULL;
            h->conn = NULL;
            if (reused && !saw && retried == 0 && rc != SX_E_TIMEOUT) {
                retried = 1;     // the pooled connection was dead; start over on a new one
                reused = false;
                continue;
            }
            char m[96];
            snprintf(m, sizeof(m), "%.30s from %.50s", rc == SX_E_TIMEOUT ? "no answer" : "connection failed", cur.host);
            set_fail(info, rc == SX_E_PROTO ? SX_E_PROTO : rc, m);
            free(req); free(h);
            return NULL;
        }
        info->reused = reused;
        info->tls = cur.tls;
        sx_conn_tls_info(conn, &info->tls_info);

        if (follows && is_redirect(head.status) && head.location[0]) {
            sx_conn_close(conn);
            h->conn = NULL;
            if (redirects >= max_redir) {
                set_fail(info, SX_E_REDIR, "too many redirects");
                free(req); free(h);
                return NULL;
            }
            char next_text[SX_URL_MAX];
            sx_url next;
            if (!sx_url_resolve(&cur, head.location, next_text, sizeof(next_text)) ||
                !sx_url_parse(next_text, &next)) {
                set_fail(info, SX_E_REDIR, "bad redirect target");
                free(req); free(h);
                return NULL;
            }
            if (cur.tls && !next.tls) {
                set_fail(info, SX_E_REDIR, "redirect from https to http refused");
                free(req); free(h);
                return NULL;
            }
            cur = next;
            redirects++;
            continue;
        }

        // Final response.
        h->url = cur;
        info->redirects = redirects;
        info->status = head.status;
        info->content_length = head.content_length;
        info->range_start = head.range_start;
        info->range_end = head.range_end;
        info->range_total = head.range_total;
        info->accept_ranges = head.accept_ranges;
        info->retry_after = head.retry_after;
        snprintf(info->content_type, sizeof(info->content_type), "%s", head.content_type);
        sx_url_format(&cur, info->final_url, sizeof(info->final_url));

        if (rq->has_range && rq->range_start >= 0 && head.status == 206 &&
            head.range_start >= 0 && head.range_start != rq->range_start) {
            sx_conn_close(conn);
            h->conn = NULL;
            set_fail(info, SX_E_PROTO, "server answered a different range");
            free(req); free(h);
            return NULL;
        }

        if (strcmp(method, "HEAD") == 0 || head.status == 204 || head.status == 304 ||
            (head.status >= 100 && head.status < 200)) {
            h->mode = FR_NONE;
            h->done = true;
        } else if (head.chunked) {
            h->mode = FR_CHUNKED;
            sx_chunk_init(&h->chunk);
        } else if (head.content_length >= 0) {
            h->mode = FR_LENGTH;
            h->left = head.content_length;
            if (h->left == 0) h->done = true;
        } else {
            h->mode = FR_CLOSE;
        }
        h->keepalive = !head.close && h->mode != FR_CLOSE;
        h->conn = conn;
        free(req);
        return h;
    }
}

int sx_http_read(sx_http *h, void *buf, int n) {
    if (!h || n <= 0) return SX_E_ARG;
    if (h->done) return 0;
    if (h->failed) return SX_E_IO;
    switch (h->mode) {
    case FR_LENGTH: {
        int want = n;
        if ((int64_t)want > h->left) want = (int)h->left;
        int got;
        if (h->pos < h->len) {
            got = h->len - h->pos;
            if (got > want) got = want;
            memcpy(buf, h->buf + h->pos, (size_t)got);
            h->pos += got;
        } else {
            got = sx_conn_read(h->conn, buf, want, h->idle_ms);
            if (got == 0) { h->failed = true; return SX_E_CLOSED; }   // cut short
            if (got < 0) { if (got != SX_E_TIMEOUT) h->failed = true; return got; }
        }
        h->left -= got;
        if (h->left == 0) h->done = true;
        return got;
    }
    case FR_CLOSE: {
        int got;
        if (h->pos < h->len) {
            got = h->len - h->pos;
            if (got > n) got = n;
            memcpy(buf, h->buf + h->pos, (size_t)got);
            h->pos += got;
            return got;
        }
        got = sx_conn_read(h->conn, buf, n, h->idle_ms);
        if (got == 0) { h->done = true; return 0; }
        if (got < 0 && got != SX_E_TIMEOUT) h->failed = true;
        return got;
    }
    case FR_CHUNKED: {
        for (;;) {
            if (h->pos >= h->len) {
                const int r = sx_conn_read(h->conn, h->buf, HEAD_BUF, h->idle_ms);
                if (r == 0) { h->failed = true; return SX_E_CLOSED; }
                if (r < 0) { if (r != SX_E_TIMEOUT) h->failed = true; return r; }
                h->pos = 0;
                h->len = r;
            }
            int used = 0, outn = 0;
            const int rc = sx_chunk_feed(&h->chunk, h->buf + h->pos, h->len - h->pos, &used,
                                         (uint8_t *)buf, n, &outn);
            h->pos += used;
            if (rc < 0) { h->failed = true; return SX_E_PROTO; }
            if (rc == 1) h->done = true;
            if (outn > 0) return outn;
            if (h->done) return 0;
        }
    }
    default:
        return 0;
    }
}

void sx_http_close(sx_http *h) {
    if (!h) return;
    if (h->conn) {
        if (!h->failed && h->done && h->keepalive && !h->no_pool && h->pos == h->len)
            pool_put(h->conn, &h->url);
        else
            sx_conn_close(h->conn);
    }
    free(h);
}

// ---- whole-body helpers -------------------------------------------------------------------

int sx_http_fetch(const sx_http_req *rq, size_t max_body, char **body, size_t *len, sx_http_info *info) {
    *body = NULL;
    if (len) *len = 0;
    if (max_body == 0) max_body = 8u * 1024 * 1024;
    sx_http *h = sx_http_open(rq, info);
    if (!h) return info->err.code ? info->err.code : SX_E_IO;
    if (info->content_length > (int64_t)max_body) {
        sx_http_close(h);
        set_fail(info, SX_E_TOOBIG, "response too large");
        return SX_E_TOOBIG;
    }
    size_t cap = info->content_length >= 0 ? (size_t)info->content_length + 1 : 65536;
    if (cap < 1024) cap = 1024;
    char *buf = (char *)malloc(cap);
    if (!buf) { sx_http_close(h); set_fail(info, SX_E_MEM, "out of memory"); return SX_E_MEM; }
    size_t n = 0;
    int rc = SX_OK;
    for (;;) {
        if (n + 1 >= cap) {
            if (cap > max_body) { rc = SX_E_TOOBIG; break; }
            size_t nc = cap * 2;
            if (nc > max_body + 1) nc = max_body + 1;
            char *nb = (char *)realloc(buf, nc);
            if (!nb) { rc = SX_E_MEM; break; }
            buf = nb;
            cap = nc;
        }
        const int r = sx_http_read(h, buf + n, (int)(cap - 1 - n));
        if (r > 0) { n += (size_t)r; continue; }
        if (r == 0) break;
        rc = r;
        break;
    }
    sx_http_close(h);
    if (rc != SX_OK) {
        free(buf);
        set_fail(info, rc, rc == SX_E_TOOBIG ? "response too large" : "response cut short");
        return rc;
    }
    buf[n] = '\0';
    *body = buf;
    if (len) *len = n;
    return SX_OK;
}

int sx_http_fetch_buf(const sx_http_req *rq, char *out, int cap, int *out_len, sx_http_info *info) {
    if (out_len) *out_len = 0;
    if (cap < 1) return SX_E_ARG;
    out[0] = '\0';
    sx_http *h = sx_http_open(rq, info);
    if (!h) return info->err.code ? info->err.code : SX_E_IO;
    if (info->content_length >= cap) {
        sx_http_close(h);
        set_fail(info, SX_E_TOOBIG, "response too large");
        return SX_E_TOOBIG;
    }
    int n = 0, rc = SX_OK;
    for (;;) {
        if (n >= cap - 1) {
            // Room for nothing more: more body means the answer does not fit.
            char probe;
            const int r = sx_http_read(h, &probe, 1);
            if (r == 0) break;
            rc = r > 0 ? SX_E_TOOBIG : r;
            break;
        }
        const int r = sx_http_read(h, out + n, cap - 1 - n);
        if (r > 0) { n += r; continue; }
        if (r == 0) break;
        rc = r;
        break;
    }
    sx_http_close(h);
    if (rc != SX_OK) {
        out[0] = '\0';
        set_fail(info, rc, rc == SX_E_TOOBIG ? "response too large" : "response cut short");
        return rc;
    }
    out[n] = '\0';
    if (out_len) *out_len = n;
    return SX_OK;
}
