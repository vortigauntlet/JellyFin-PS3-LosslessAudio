// The Network test (Settings -> Network): handshakes to every host class the
// app talks to, then a timed bulk download with each cipher order.  It exists
// to answer one question on the console: can the PPU decrypt TLS at video
// bitrates while it also has work to do?  Portable; the caller runs it on a
// worker thread and supplies the sinks.
#pragma once
#include <stdbool.h>
#include "sx_plat.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *host;
    int         port;       // 0 = 443
    const char *label;      // shown in the result line; NULL = host
} sx_nettest_host;

typedef struct {
    const sx_nettest_host *hosts;
    int         n_hosts;
    const char *bulk_url;       // an HTTPS file at least bulk_secs long at ~30 Mbps; NULL/"" skips the throughput runs
    int         bulk_secs;      // 0 = 20
    int         rcvbuf;         // SO_RCVBUF for the bulk connection; 0 = 512 KiB like the video reader
    // Sinks.  line() is called from the worker thread with one finished line.
    void      (*line)(void *ctx, const char *text);
    // Polled between reads; return true to stop early.
    bool      (*cancelled)(void *ctx);
    // 0..100: how much of the PPU was idle since the previous call, or -1 when
    // the platform cannot tell.  Read at the start and end of each bulk run.
    int       (*idle_pct)(void *ctx);
    void       *ctx;
} sx_nettest_cfg;

typedef struct {
    int    handshakes_ok, handshakes_failed;
    double mbps_chacha, mbps_aes;       // average over the run, 0 when it did not run
    double min_chacha, min_aes;         // slowest one-second window
    int    idle_chacha, idle_aes;       // -1 unknown
    bool   clock_suspect;
    bool   ran_bulk;
} sx_nettest_result;

void sx_nettest_run(const sx_nettest_cfg *cfg, sx_nettest_result *out);

#ifdef __cplusplus
}
#endif
