// HTTP/1.1 client over sx_conn: keep-alive pooling, redirects, Range, chunked,
// and a streaming body reader.  Used for every fetch -- JSON, images, subtitles
// and the video Range reader -- over plain TCP or TLS.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "sx_plat.h"
#include "sx_tls.h"
#include "sx_httpparse.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *method;            // NULL = "GET"
    const char *url;
    const char *headers;           // extra "Name: value\r\n" lines; sent to the original host only
    const void *body;              // POST/PUT body
    int         body_len;
    const char *content_type;      // for a body; NULL = application/json
    int64_t     range_start;       // < 0: no Range header (leave 0-init requests with -1)
    int64_t     range_end;         // < 0: open-ended
    bool        has_range;         // true when range_start is meant (so a zeroed struct means "no range")
    int         max_redirects;     // 0 = 5
    int         connect_ms;        // 0 = 8000
    int         handshake_ms;      // 0 = 12000
    int         first_byte_ms;     // 0 = 24000
    int         idle_ms;           // 0 = 8000
    int         rcvbuf;            // SO_RCVBUF for the connection, 0 = default
    bool        no_pool;           // never reuse a pooled connection and never return this one (video)
} sx_http_req;

typedef struct {
    int      status;                       // 0 until a response head arrived
    char     final_url[SX_URL_MAX];        // after redirects.  Contains secrets: never log it, use sx_redact_url
    int64_t  content_length;               // -1 unknown
    int64_t  range_start, range_end, range_total;   // Content-Range, -1 when absent
    bool     accept_ranges;
    char     content_type[64];
    int      retry_after;                  // seconds, -1 absent
    int      redirects;
    bool     reused;                       // the connection came from the pool
    bool     tls;
    sx_tls_info tls_info;                  // zeroed for plain HTTP
    int      connect_ms;                   // resolve + connect (+ handshake) of the last connection; 0 when reused
    int      ttfb_ms;                      // request sent -> response head
    sx_neterr err;                         // code != SX_OK when the call failed
} sx_http_info;

typedef struct sx_http sx_http;

// Call once, before any other thread uses the client.
void sx_http_init(void);
void sx_http_set_user_agent(const char *ua);

// Sends the request, follows redirects and returns once the response head is
// in.  NULL on a transport failure (info->err says why).  A 4xx/5xx is not a
// failure here: the caller sees info->status.
sx_http *sx_http_open(const sx_http_req *req, sx_http_info *info);

// >0 body bytes, 0 = end of body, or a negative SX_E_*.
int  sx_http_read(sx_http *h, void *buf, int n);

// Returns the connection to the pool when the body was read to its end and the
// server allows reuse; otherwise closes it.
void sx_http_close(sx_http *h);

// Whole-body fetch into a heap buffer (NUL-terminated, free() it).  max_body 0 = 8 MiB.
// Returns SX_OK when a response arrived (look at info->status); a body above
// max_body fails with SX_E_TOOBIG and no buffer.
int  sx_http_fetch(const sx_http_req *req, size_t max_body, char **body, size_t *len,
                   sx_http_info *info);

// Same, into the caller's buffer (NUL-terminated, cap includes the NUL).
int  sx_http_fetch_buf(const sx_http_req *req, char *out, int cap, int *out_len,
                       sx_http_info *info);

// Closes every pooled connection.
void sx_http_pool_flush(void);

#ifdef __cplusplus
}
#endif
