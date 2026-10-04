// Offline playback: a downloaded media.ts through the existing player.
//
// Everything that decides something is portable and host-tested: which
// items are playable and how to set the player up (offline/dl_library.cpp),
// and where to enter the file for a start or a seek
// (player/stream/stream_local.cpp).  This file is the console glue: a read-at
// over lfs for the probes, the reopen, and the entry point.

#include <stdio.h>
#include <string.h>

#include <ppu-types.h>

#include "player.h"
#include "player_internal.h"
#include "stream.h"
#include "stream_local.h"
#include "dl_library.h"
#include "lfs.h"
#include "plog.h"

// Probes read through their own descriptor, so the stream's position (and
// its buffer contents, which a probe borrows as scratch only while no
// stream is open) are never disturbed.
static int local_read_at(void *ctx, uint64_t offset, uint8_t *buf, int len) {
    const int h = *(const int *)ctx;
    const int got = lfs_read(h, offset, buf, (uint32_t)len);
    return got < 0 ? -1 : got;
}

static bool probe_open(const char *path, int *h) {
    *h = lfs_open(path);
    return *h >= 0;
}

bool player_local_open(PlayerState *ps, u64 target_us) {
    const PlayerLocal *lp = ps->local;
    int fd;
    if (!probe_open(lp->path, &fd)) return false;
    int cap = 0;
    u8 *scratch = stream_scratch(&cap);
    // Probe in 256 KB reads: small enough to be quick on the HDD, big
    // enough to hold a PAT/PMT/keyframe run in one go.
    uint64_t off = 0, at_us = 0;
    const bool ok = stream_local_seek(local_read_at, &fd, &lp->idx, target_us,
                                      scratch, cap, &off, &at_us);
    lfs_close(fd);
    if (!ok) return false;
    ps->sock = stream_open_file(lp->path, off);
    if (ps->sock < 0) return false;
    ps->play_base_us = at_us;
    char b[112];
    snprintf(b, sizeof(b), "local: target=%llus entry=%llus offset=%llu",
             (unsigned long long)(target_us / 1000000ULL),
             (unsigned long long)(at_us / 1000000ULL), (unsigned long long)off);
    plog(b);
    return true;
}

bool show_player_offline(const char *item_id, u32 resume_secs) {
    // Verified at the moment of playing, not when some list was drawn: a
    // file removed or truncated since then must not reach the decoder.
    static DlLibraryEntry e;          // ~3 KB: off the stack
    if (!dl_library_get(item_id, &e)) {
        plog("offline: not playable (not completed, or media missing/changed)");
        return false;
    }
    static PlayerLocal local;
    memset(&local, 0, sizeof(local));
    snprintf(local.path, sizeof(local.path), "%s", e.media_path);
    snprintf(local.label, sizeof(local.label), "Offline");
    dl_library_plan(&e, &local.plan);

    // Measure the file itself: its first keyframe and its real duration.
    // A stale meta.txt cannot then mislead the HUD or the seek bar.
    int fd;
    if (!probe_open(local.path, &fd)) return false;
    int cap = 0;
    u8 *scratch = stream_scratch(&cap);
    const int64_t size = (int64_t)e.bytes;
    const bool indexed = stream_local_index(local_read_at, &fd, (uint64_t)size,
                                            scratch, cap, &local.idx);
    lfs_close(fd);
    if (!indexed) {
        plog("offline: media.ts has no video to enter");
        return false;
    }
    {
        char b[112];
        snprintf(b, sizeof(b), "offline: %.8s %ux%u dur=%us meta=%d light=%d",
                 item_id, local.plan.req_w, local.plan.req_h,
                 local.idx.duration_secs, (int)e.meta_ok, (int)local.plan.light);
        plog(b);
    }

    static JFItem item;
    memset(&item, 0, sizeof(item));
    snprintf(item.id,   sizeof(item.id),   "%s", e.meta.id);
    snprintf(item.name, sizeof(item.name), "%s", e.meta.title);
    snprintf(item.type, sizeof(item.type), "%s", e.meta.type);
    show_player_run(&item, resume_secs, NULL, &local);
    return true;
}
