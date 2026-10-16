// Pure HTTP/1.1 pieces: URL parsing and redirect resolution, response-head
// parsing, a streaming chunked decoder, URL redaction.  No I/O and no platform
// includes, so the host suite runs it as it is (little- and big-endian).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SX_HOST_MAX 256
#define SX_URL_MAX  2048

typedef struct {
    bool tls;
    char host[SX_HOST_MAX];
    int  port;
    char path[SX_URL_MAX];   // path + query, starts with '/', already percent-escaped
} sx_url;

// http/https only.  Spaces and bytes >= 0x80 in the path are percent-escaped;
// existing %XX sequences are left alone.  Userinfo and IPv6 literals are refused.
bool sx_url_parse(const char *url, sx_url *out);

// Writes the absolute text of `loc` (a Location header) resolved against base.
bool sx_url_resolve(const sx_url *base, const char *loc, char *out, size_t cap);

// "scheme://host[:port]path" (port omitted when it is the scheme's default).
void sx_url_format(const sx_url *u, char *out, size_t cap);

// A log-safe form: scheme://host/<token> when the path carries more than one
// segment or any query, otherwise the URL as it is.  Addon URLs carry the
// user's keys in the path, debrid links carry theirs in path and query.
void sx_redact_url(const char *url, char *out, size_t cap);

typedef struct {
    int      status;
    int64_t  content_length;                       // -1 when absent
    bool     chunked, close, accept_ranges;
    int64_t  range_start, range_end, range_total;  // Content-Range, -1 when absent
    char     location[SX_URL_MAX];
    char     content_type[64];
    int      retry_after;                          // seconds, -1 when absent
    int      head_len;                             // bytes up to and including the blank line
} sx_head;

// -1: the head is not complete yet.  0: malformed.  1: parsed.
int sx_head_parse(const char *buf, int len, sx_head *h);

typedef struct {
    int      state;
    int64_t  left;
    int      line;      // length of the current trailer line
    bool     any_digit;
} sx_chunk;

void sx_chunk_init(sx_chunk *c);
// Consumes up to in_n bytes, writes decoded bytes to out (at most out_cap).
// Returns 1 when the terminal chunk and its trailers are fully consumed,
// 0 when more input is needed (or out is full: call again), -1 on bad framing.
int  sx_chunk_feed(sx_chunk *c, const uint8_t *in, int in_n, int *in_used,
                   uint8_t *out, int out_cap, int *out_n);

#ifdef __cplusplus
}
#endif
