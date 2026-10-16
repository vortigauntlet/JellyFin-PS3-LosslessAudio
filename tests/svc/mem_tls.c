// How much heap does sx_tls_init() take, and one TLS connection? (mallinfo on the host; the PS3's mbedTLS config and
// the same code, so within a few percent of what the console allocates.)
//
//   mem_tls
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sx_http.h"
#include "sx_tls.h"

static size_t used(void) { struct mallinfo2 m = mallinfo2(); return m.uordblks + m.hblkhd; }

int main(void) {
    const size_t a = used();
    sx_http_init();
    const bool ok = sx_tls_init();
    const size_t b = used();
    printf("sx_tls_init: %s, %zu KB held afterwards (%d trusted roots)\n", ok ? "ok" : "FAILED", (b - a) / 1024, sx_tls_ca_count());
    sx_http_req rq;
    memset(&rq, 0, sizeof(rq));
    rq.url = "https://api-v2.soundcloud.com/";
    rq.range_start = -1; rq.range_end = -1;
    sx_http_info info;
    memset(&info, 0, sizeof(info));
    sx_http *h = sx_http_open(&rq, &info);
    const size_t c = used();
    printf("one open HTTPS request (status %d): %zu KB more\n", info.status, (c - b) / 1024);
    char buf[512];
    while (h && sx_http_read(h, buf, sizeof(buf)) > 0) {}
    sx_http_close(h);                                     // pooled: the connection stays open
    const size_t d = used();
    printf("pooled after the body: %zu KB above the start\n", (d - b) / 1024);
    sx_http_pool_flush();
    const size_t e = used();
    printf("after sx_http_pool_flush: %zu KB above the start\n", (e - b) / 1024);
    return 0;
}
