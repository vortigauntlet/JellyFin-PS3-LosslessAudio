#pragma once

// Console check for the SoundCloud network chain (gate SC0), opt-in like net_selftest.
//
// Does nothing unless /dev_hdd0/tmp/jellyfin_sc_test.txt exists.  Then, on a worker thread (so a
// stalled network can never hold the boot), it walks the same chain the SoundCloud tab uses:
// find the client id, resolve a public track, pick a transcoding, fetch the playlist, the init
// segment and the first media segment.  Each step and its time goes to
// /dev_hdd0/tmp/jellyfin_sc_test_result.txt (and to player_log.txt when logging is on).  The gate
// file is deleted when the run starts, so a stale one cannot repeat it on every boot.
//
// Secrets (client id, signed URLs) are never written.

#ifdef __cplusplus
extern "C" {
#endif

void sc_diag_start(void);

#ifdef __cplusplus
}
#endif
