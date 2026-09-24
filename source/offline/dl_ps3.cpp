// Offline downloads -- the console side: dl_platform.h over libnet, lv2 fs
// and an lv2 mutex, plus the worker thread (dl_service.h).
//
// Everything testable lives in dl_manager/dl_store/dl_http/dl_model and is
// covered on the host by tests/test_offline.cpp.  This file is the thin rest:
// each function is a direct call into the system, with no decisions of its
// own beyond mapping return codes.

#include "dl_platform.h"
#include "dl_service.h"
#include "dl_manager.h"
#include "dl_store.h"
#include "dl_request.h"
#include "stream_request.h"   // stream_prefs_current

#include "../build_config.h"   // relative: source/ is not on the -I path
#include "http.h"              // http_open_socket
#include "jellyfin_api.h"      // g_token, jf_device_id
#include "jf_paths.h"
#include "plog.h"
#include "timing.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>   // S_IFDIR: lv2's st_mode uses the POSIX bits
#include <unistd.h>

#include <ppu-types.h>
#include <net/net.h>
#include <net/socket.h>
#include <sys/file.h>
#include <sys/mutex.h>
#include <sys/thread.h>
#include <sysmodule/sysmodule.h>
#include <lv2/sysfs.h>

extern u32 running;   // main.cpp

// -------------------------------------------------------------------------
// Network
// -------------------------------------------------------------------------

// Receive timeout on the download socket.  Short on purpose: it is how often
// the transfer loop gets to look at pause/cancel/suspend.  The idle and
// header deadlines are counted in dl_manager.cpp on top of it.
#define DL_RECV_TIMEOUT_MS 1000

// 512 KB -- the size stream.cpp measured as a clear win over 128 KB on this
// console (31 vs 25 Mbps median).  A download wants the same thing a stream
// does: a wide TCP window.
#define DL_RCVBUF_BYTES (512 * 1024)

int dl_plat_connect(const char *host, int port) {
    int sock = http_open_socket(host, port, DL_RCVBUF_BYTES);
    if (sock < 0) return -1;
    struct timeval tv;
    tv.tv_sec  = DL_RECV_TIMEOUT_MS / 1000;
    tv.tv_usec = (DL_RECV_TIMEOUT_MS % 1000) * 1000;
    netSetSockOpt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return sock;
}

int dl_plat_send(int h, const void *buf, int len) {
    int sent = 0;
    while (sent < len) {
        int n = netSend(h, (const char *)buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return len;
}

int dl_plat_recv(int h, void *buf, int cap) {
    int n = netRecv(h, buf, cap, 0);
    if (n > 0) return n;
    if (n == 0) return DL_RECV_CLOSED;
    // SO_RCVTIMEO expiring reports EAGAIN/EWOULDBLOCK (ETIMEDOUT on some
    // firmware paths); anything else is the connection going away.
    int e = net_errno;
    if (e == NET_EAGAIN || e == NET_ETIMEDOUT || e == NET_EINTR)
        return DL_RECV_TIMEOUT;
    return DL_RECV_ERROR;
}

void dl_plat_close(int h) { if (h >= 0) netClose(h); }

// -------------------------------------------------------------------------
// Filesystem -- lv2 syscalls, so 64-bit sizes and no PRX dependency, except
// free space, which only libsysfs reports.
// -------------------------------------------------------------------------

bool dl_plat_mkdir(const char *path) {
    if (sysLv2FsMkdir(path, 0755) == 0) return true;
    sysFSStat st;
    return sysLv2FsStat(path, &st) == 0 && (st.st_mode & S_IFDIR);
}

bool dl_plat_rmdir(const char *path) { return sysLv2FsRmdir(path) == 0; }

bool dl_plat_remove(const char *path) {
    if (sysLv2FsUnlink(path) == 0) return true;
    sysFSStat st;
    return sysLv2FsStat(path, &st) != 0;
}

bool dl_plat_rename(const char *from, const char *to) {
    return sysLv2FsRename(from, to) == 0;
}

int64_t dl_plat_file_size(const char *path) {
    sysFSStat st;
    if (sysLv2FsStat(path, &st) != 0 || (st.st_mode & S_IFDIR)) return -1;
    return (int64_t)st.st_size;
}

bool dl_plat_exists(const char *path) {
    sysFSStat st;
    return sysLv2FsStat(path, &st) == 0;
}

bool dl_plat_truncate(const char *path) {
    s32 fd = -1;
    if (sysLv2FsOpen(path, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, &fd,
                     0644, NULL, 0) != 0)
        return false;
    sysLv2FsClose(fd);
    return true;
}

// lv2 dirent types (CELL_FS_TYPE_*); PSL1GHT has no names for them.
#define LV2_DT_UNKNOWN 0
#define LV2_DT_DIR     1

int dl_plat_list_dirs(const char *path, void (*cb)(const char *, void *),
                      void *ctx) {
    s32 fd = -1;
    if (sysLv2FsOpenDir(path, &fd) != 0) return -1;
    int n = 0;
    sysFSDirent ent;
    u64 got = 0;
    while (sysLv2FsReadDir(fd, &ent, &got) == 0 && got > 0) {
        if (strcmp(ent.d_name, ".") == 0 || strcmp(ent.d_name, "..") == 0)
            continue;
        if (ent.d_type != LV2_DT_DIR) {
            if (ent.d_type != LV2_DT_UNKNOWN) continue;
            // Type not reported: ask the filesystem.
            char full[DL_PATH_MAX];
            int fl = snprintf(full, sizeof(full), "%s/%s", path, ent.d_name);
            sysFSStat st;
            if (fl < 0 || fl >= (int)sizeof(full) ||
                sysLv2FsStat(full, &st) != 0 || !(st.st_mode & S_IFDIR))
                continue;
        }
        cb(ent.d_name, ctx);
        n++;
    }
    sysLv2FsCloseDir(fd);
    return n;
}

static bool s_fs_module = false;   // libsysfs loaded (for free space)

uint64_t dl_plat_free_bytes(const char *path) {
    if (!s_fs_module) return DL_FREE_UNKNOWN;
    u32 block = 0;
    u64 blocks = 0;
    if (sysFsGetFreeSize(path, &block, &blocks) != 0 || block == 0)
        return DL_FREE_UNKNOWN;
    return (uint64_t)block * blocks;
}

int dl_plat_file_open_append(const char *path) {
    s32 fd = -1;
    if (sysLv2FsOpen(path, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_APPEND, &fd,
                     0644, NULL, 0) != 0)
        return -1;
    return fd;
}

int dl_plat_file_write(int fh, const void *buf, int len) {
    u64 written = 0;
    if (sysLv2FsWrite(fh, buf, (u64)len, &written) != 0 || written != (u64)len)
        return -1;
    return len;
}

bool dl_plat_file_sync(int fh) { return sysLv2FsFsync(fh) == 0; }
void dl_plat_file_close(int fh) { if (fh >= 0) sysLv2FsClose(fh); }

// -------------------------------------------------------------------------
// Time, lock, log
// -------------------------------------------------------------------------

static sys_mutex_t s_mtx;
static bool        s_mtx_ok = false;

uint64_t dl_plat_now_ms(void)          { return timing_get_us() / 1000ULL; }
void     dl_plat_sleep_ms(unsigned ms) { usleep(ms * 1000u); }
void     dl_plat_lock(void)   { if (s_mtx_ok) sysMutexLock(s_mtx, 0); }
void     dl_plat_unlock(void) { if (s_mtx_ok) sysMutexUnlock(s_mtx); }
void     dl_plat_log(const char *line) { plog(line); }

// -------------------------------------------------------------------------
// Worker thread
// -------------------------------------------------------------------------

static sys_ppu_thread_t s_thread = 0;
static void (*s_thread_fn)(void) = NULL;

static void thread_entry(void *arg) {
    (void)arg;
    s_thread_fn();
    sysThreadExit(0);
}

bool dl_plat_thread_start(void (*fn)(void)) {
    s_thread_fn = fn;
    static char name[] = "dl_worker";   // sysThreadCreate takes char*
    // Priority 1500: below every playback thread (700-1100), so a download
    // can never take the PPU from the player.  Same as pkgi-ps3's worker.
    s32 rc = sysThreadCreate(&s_thread, thread_entry, NULL, 1500, 65536, 0, name);
    if (rc != 0) {
        char b[48];
        snprintf(b, sizeof(b), "dl: worker create failed 0x%08x", (u32)rc);
        plog(b);
        s_thread = 0;
        return false;
    }
    return true;
}

void dl_plat_thread_join(void) {
    if (!s_thread) return;
    u64 tret;
    sysThreadJoin(s_thread, &tret);
    s_thread = 0;
}

bool dl_plat_app_running(void) { return running != 0; }

// -------------------------------------------------------------------------
// Service glue (dl_service.h)
// -------------------------------------------------------------------------

// Where the store lives.  Tried in order on the worker; the first that can
// be created AND written (dl_store_init writes a marker to prove it) wins.
//
//  Hardware: a directory of its own at the HDD root first.  Not /dev_hdd0/tmp
//  -- that is scratch space (jf_paths.cpp) and a film is not scratch -- and
//  not the game USRDIR, which jf_paths.cpp found is not reliably writable
//  while the title runs.  /dev_hdd0/tmp is the fallback only because it is
//  the one place this app has PROVEN writable on hardware.
//  RPCS3: the USRDIR, where the emulator keeps everything else of ours.
static char s_fallback_root[DL_PATH_MAX] = "";
static const char *s_roots[2] = {
#if BUILD_FOR_RPCS3
    "/dev_hdd0/game/JFPS30000/USRDIR/jellyfin_offline",
#else
    "/dev_hdd0/jellyfin_offline",
#endif
    s_fallback_root,
};

bool dl_service_start(void) {
    if (dl_svc_started()) return true;
    if (!s_mtx_ok) {
        sys_mutex_attr_t attr;
        memset(&attr, 0, sizeof(attr));
        attr.attr_protocol  = SYS_MUTEX_PROTOCOL_FIFO;
        attr.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;
        s_mtx_ok = (sysMutexCreate(&s_mtx, &attr) == 0);
        if (!s_mtx_ok) { plog("dl: mutex create failed"); return false; }
    }
    if (!s_fs_module) s_fs_module = (sysModuleLoad(SYSMODULE_FS) == 0);
    snprintf(s_fallback_root, sizeof(s_fallback_root), "%s",
             jf_data_path("jellyfin_offline"));
    // Whatever session exists already (a saved login) -- or none, which
    // keeps the queue held until login.
    dl_service_refresh_auth();
    return dl_svc_start(s_roots, 2);
}

void dl_service_stop(void) { dl_svc_stop(); }

void dl_service_refresh_auth(void) { dl_svc_set_session(g_token, jf_device_id()); }

const char *dl_service_root(void) { return dl_svc_root(); }

int dl_download_item(const JFItem *item, const XMBItemDetail *detail,
                     const JFMediaSource *source) {
    StreamPrefs prefs;
    stream_prefs_current(&prefs);
    DlRequestInput in = { g_server, jf_device_id(), item, detail, source, &prefs };
    static DlRequest rq;   // ~5 KB, UI thread
    if (!dl_request_build(&in, &rq)) return DL_E_INVALID;
    DlResult r = dl_enqueue(&rq.meta, rq.url, 0, &rq.extras);
    char b[96];
    snprintf(b, sizeof(b), "dl: request %.8s -> %d", item ? item->id : "?", (int)r);
    plog(b);
    return r;
}
