// lfs: see lfs.h.  The rules (what is mounted, in which order, what a path means, what a
// listing shows) are all here and portable; what touches the console is behind LfsPort
// (lfs_port_ps3.cpp) and the mohasi storage seam (third_party/mohasi_fs/jf_port_ps3.c), so
// tests/test_lfs.cpp runs this file on the host over directory trees and NTFS/exFAT images.

#include "lfs.h"
#include "lfs_port.h"
#include "lfs_path.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "ntfs.h"
#include "exfat.h"
#include "jf_port.h"            // readStorageRaw: the "can the drive still be read" probe
}

#define NPORTS            8
#define MAX_HANDLES       8
#define MAX_ENTRIES       4096            // entries kept of one directory
#define SETTLE_US         2500000ULL      // a device is left alone this long after it appears (lv2 mounts FAT32 by then)
#define RAW_TRIES         5               // "not ready" answers before a device is called unreadable
#define READ_SLICE        (256u * 1024u)  // one lock hold: a listing is never kept waiting longer than this
#define BENCH_BYTES       (64ull * 1024 * 1024)
#define NATIVE_HDD_ROOT   "/dev_hdd0"
#define MAX_NATIVE_PATH   512

enum { LOCK_TABLE = 0, LOCK_IO = 1 };

static LfsPort P;                         // the platform (zeroed until lfs_set_port)
static bool    s_have_port = false;

void lfs_set_port(const LfsPort *port) { P = *port; s_have_port = true; }

static void log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_line(const char *fmt, ...) {
    if (!P.log) return;
    char b[160];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    P.log(b);
}

// ---- drives ---------------------------------------------------------------------

enum SlotState { SLOT_EMPTY = 0, SLOT_SETTLING, SLOT_MOUNTED, SLOT_UNSUPPORTED };

struct Slot {
    SlotState state;
    lfs_kind  kind;
    uint64_t  seen_us;        // when the device was first seen
    int       tries;
    uint32_t  epoch;          // changes at every mount; a handle keeps the one it opened under
    char      label[48];
    uint64_t  total, free;
    uint64_t  rd_bytes, rd_us;
    bool      bench_logged;
};

static Slot       s_slot[NPORTS];
static NtfsVolume s_ntfs[NPORTS];
static ExfatVolume s_exfat[NPORTS];
static uint32_t   s_gen = 1;
static uint32_t   s_epoch_counter = 0;
static uint64_t   s_hdd_free = 0, s_hdd_total = 0;
static int        s_poll_count = 0;
static bool       s_hdd_ready = false;

static void lock(int w)   { if (P.lock)   P.lock(w); }
static void unlock(int w) { if (P.unlock) P.unlock(w); }

struct Guard {
    int w;
    explicit Guard(int which) : w(which) { lock(w); }
    ~Guard() { unlock(w); }
};

const char *lfs_kind_name(lfs_kind k) {
    switch (k) {
    case LFS_HDD:         return "Internal";
    case LFS_USB_FAT:     return "FAT32";
    case LFS_USB_NTFS:    return "NTFS";
    case LFS_USB_EXFAT:   return "exFAT";
    default:              return "Unsupported";
    }
}

uint32_t lfs_generation(void) {
    Guard g(LOCK_TABLE);
    return s_gen;
}

static void native_fat(int port, const char *rest, char *out, size_t cap) {
    snprintf(out, cap, "/dev_usb%03d%s", port, rest);
}

static void refresh_space(int port) {          // FAT only (NTFS and exFAT have it from the mount)
    char nat[MAX_NATIVE_PATH];
    native_fat(port, "/", nat, sizeof nat);
    uint64_t f = 0, t = 0;
    if (P.fs_space && P.fs_space(nat, &f, &t)) {
        Guard g(LOCK_TABLE);
        s_slot[port].free = f; s_slot[port].total = t;
    }
}

static void refresh_hdd_space(void) {
    uint64_t f = 0, t = 0;
    if (P.fs_space && P.fs_space(NATIVE_HDD_ROOT, &f, &t)) {
        Guard g(LOCK_TABLE);
        s_hdd_free = f; s_hdd_total = t;
    }
}

int lfs_drives(lfs_drive *out, int max) {
    int n = 0;
    Guard g(LOCK_TABLE);
    if (n < max) {
        lfs_drive *d = &out[n++];
        memset(d, 0, sizeof *d);
        snprintf(d->id, sizeof d->id, "hdd");
        snprintf(d->label, sizeof d->label, "Internal disk");
        d->kind = LFS_HDD;
        d->total = s_hdd_total; d->free = s_hdd_free;
    }
    for (int p = 0; p < NPORTS && n < max; p++) {
        const Slot &s = s_slot[p];
        if (s.state != SLOT_MOUNTED && s.state != SLOT_UNSUPPORTED) continue;
        lfs_drive *d = &out[n++];
        memset(d, 0, sizeof *d);
        snprintf(d->id, sizeof d->id, "usb%d", p);
        if (s.label[0]) snprintf(d->label, sizeof d->label, "%s", s.label);
        else            snprintf(d->label, sizeof d->label, "USB drive %d", p + 1);
        d->kind = s.state == SLOT_UNSUPPORTED ? LFS_USB_UNSUPPORTED : s.kind;
        d->total = s.total; d->free = s.free;
    }
    return n;
}

static void drop_volume(int p) {               // io lock held
    if (s_slot[p].kind == LFS_USB_NTFS && s_ntfs[p].mounted)   unmountNtfs(&s_ntfs[p]);
    if (s_slot[p].kind == LFS_USB_EXFAT && s_exfat[p].mounted) unmountExfat(&s_exfat[p]);
}

static void set_label(Slot *s, int port, const char *vol_label) {
    if (vol_label && vol_label[0]) snprintf(s->label, sizeof s->label, "%s", vol_label);
    else                           snprintf(s->label, sizeof s->label, "USB drive %d", port + 1);
}

// A device that has been present long enough: say what it is.  Called without locks.
static void try_mount(int p) {
    if (P.fat_mounted && P.fat_mounted(p)) {
        char nat[MAX_NATIVE_PATH];
        native_fat(p, "/", nat, sizeof nat);
        uint64_t f = 0, t = 0;
        if (P.fs_space) P.fs_space(nat, &f, &t);
        Guard g(LOCK_TABLE);
        Slot &s = s_slot[p];
        s.state = SLOT_MOUNTED; s.kind = LFS_USB_FAT; s.epoch = ++s_epoch_counter;
        set_label(&s, p, NULL);
        s.free = f; s.total = t; s.rd_bytes = s.rd_us = 0; s.bench_logged = false;
        s_gen++;
        log_line("lfs: usb%d FAT32 (lv2)", p);
        return;
    }

    lfs_kind kind = LFS_USB_UNSUPPORTED;
    bool not_ready = false;
    char label[64] = "";
    uint64_t f = 0, t = 0;
    const uint64_t t0 = P.now_us ? P.now_us() : 0;
    {
        Guard io(LOCK_IO);
        int r = mountNtfs(&s_ntfs[p], p);
        if (r == NTFS_MOUNT_OK) {
            kind = LFS_USB_NTFS;
            snprintf(label, sizeof label, "%s", s_ntfs[p].label);
            getNtfsFree(&s_ntfs[p], &f, &t);
        } else if (r == NTFS_MOUNT_NOT_READY) {
            not_ready = true;
        } else {
            r = mountExfat(&s_exfat[p], p);
            if (r == EXFAT_MOUNT_OK) {
                kind = LFS_USB_EXFAT;
                snprintf(label, sizeof label, "%s", s_exfat[p].label);
                getExfatFree(&s_exfat[p], &f, &t);
            } else if (r == EXFAT_MOUNT_NOT_READY) {
                not_ready = true;
            }
        }
    }
    Guard g(LOCK_TABLE);
    Slot &s = s_slot[p];
    if (kind == LFS_USB_NTFS || kind == LFS_USB_EXFAT) {
        s.state = SLOT_MOUNTED; s.kind = kind; s.epoch = ++s_epoch_counter;
        set_label(&s, p, label);
        s.free = f; s.total = t; s.rd_bytes = s.rd_us = 0; s.bench_logged = false;
        s_gen++;
        log_line("lfs: usb%d %s \"%s\" mounted in %llu ms", p, lfs_kind_name(kind), s.label,
                 (unsigned long long)(P.now_us ? (P.now_us() - t0) / 1000ULL : 0));
    } else if (not_ready && ++s.tries < RAW_TRIES) {
        // stay SETTLING: the next poll asks again
    } else {
        s.state = SLOT_UNSUPPORTED; s.kind = LFS_USB_UNSUPPORTED; s.label[0] = '\0'; s.free = s.total = 0;
        s_gen++;
        log_line("lfs: usb%d has no file system this app can read", p);
    }
}

void lfs_poll_once(void) {
    if (!s_have_port) return;
    if (!s_hdd_ready) {
        s_hdd_ready = true;
        if (P.fs_mkdir) P.fs_mkdir(NATIVE_HDD_ROOT "/jellyfin_local");
        refresh_hdd_space();
    }
    const uint64_t now = P.now_us ? P.now_us() : 0;
    const bool slow_tick = (++s_poll_count % 5) == 0;
    for (int p = 0; p < NPORTS; p++) {
        const bool present = P.usb_present && P.usb_present(p);
        SlotState st; lfs_kind kind;
        { Guard g(LOCK_TABLE); st = s_slot[p].state; kind = s_slot[p].kind; }
        if (!present) {
            if (st == SLOT_EMPTY) continue;
            { Guard io(LOCK_IO); drop_volume(p); }
            Guard g(LOCK_TABLE);
            const bool shown = s_slot[p].state == SLOT_MOUNTED || s_slot[p].state == SLOT_UNSUPPORTED;
            const lfs_kind was = s_slot[p].kind;
            memset(&s_slot[p], 0, sizeof s_slot[p]);
            s_slot[p].epoch = ++s_epoch_counter;       // handles opened on the old volume now fail
            if (shown) { s_gen++; log_line("lfs: usb%d (%s) removed", p, lfs_kind_name(was)); }
            continue;
        }
        switch (st) {
        case SLOT_EMPTY: {
            Guard g(LOCK_TABLE);
            s_slot[p].state = SLOT_SETTLING; s_slot[p].seen_us = now; s_slot[p].tries = 0;
            break;
        }
        case SLOT_SETTLING: {
            uint64_t seen;
            { Guard g(LOCK_TABLE); seen = s_slot[p].seen_us; }
            if (now - seen >= SETTLE_US) try_mount(p);
            break;
        }
        case SLOT_UNSUPPORTED:
            if (P.fat_mounted && P.fat_mounted(p)) try_mount(p);   // lv2 mounted it after all
            break;
        case SLOT_MOUNTED:
            if (slow_tick && kind == LFS_USB_FAT) refresh_space(p);
            break;
        }
    }
    if (slow_tick) refresh_hdd_space();
}

// ---- paths ----------------------------------------------------------------------

enum TargetKind { T_NONE, T_NATIVE, T_NTFS, T_EXFAT, T_HDD_ROOT };

struct Target {
    TargetKind kind;
    int  port;                       // T_NTFS / T_EXFAT, and T_NATIVE on a USB drive (-1 on the disk)
    char native[MAX_NATIVE_PATH];    // T_NATIVE: the lv2 path
    char rest[1024];                 // T_NTFS / T_EXFAT: the path inside the volume
};

static bool has_dotdot(const char *p) {
    for (const char *s = p; *s; ) {
        while (*s == '/') s++;
        const char *e = s;
        while (*e && *e != '/') e++;
        if (e - s == 2 && s[0] == '.' && s[1] == '.') return true;
        s = e;
    }
    return false;
}

// Maps a path to where it lives.  Returns 0 or a LFS_E_ code.
static int resolve(const char *path, Target *t) {
    memset(t, 0, sizeof *t);
    t->port = -1;
    if (!path) return LFS_E_BADPATH;
    if (strncmp(path, NATIVE_HDD_ROOT "/", strlen(NATIVE_HDD_ROOT) + 1) == 0) {   // where downloads live
        if (has_dotdot(path) || strlen(path) >= sizeof t->native) return LFS_E_BADPATH;
        t->kind = T_NATIVE;
        snprintf(t->native, sizeof t->native, "%s", path);
        return 0;
    }
    char drive[16], rest[1024];
    if (!lfs_path_split(path, drive, sizeof drive, rest, sizeof rest)) return LFS_E_BADPATH;
    if (strcmp(drive, "hdd") == 0) {
        if (strcmp(rest, "/") == 0) { t->kind = T_HDD_ROOT; return 0; }
        static const char *const roots[] = { "/video", "/music", "/jellyfin_local" };
        bool ok = false;
        for (size_t i = 0; i < sizeof roots / sizeof roots[0]; i++) {
            const size_t n = strlen(roots[i]);
            if (strncmp(rest, roots[i], n) == 0 && (rest[n] == '\0' || rest[n] == '/')) ok = true;
        }
        if (!ok) return LFS_E_NOTFOUND;
        t->kind = T_NATIVE;
        snprintf(t->native, sizeof t->native, NATIVE_HDD_ROOT "%s", rest);
        return 0;
    }
    const int p = drive[3] - '0';
    Guard g(LOCK_TABLE);
    const Slot &s = s_slot[p];
    if (s.state != SLOT_MOUNTED) return LFS_E_NOTFOUND;
    t->port = p;
    switch (s.kind) {
    case LFS_USB_FAT:
        t->kind = T_NATIVE;
        native_fat(p, rest, t->native, sizeof t->native);
        return 0;
    case LFS_USB_NTFS:  t->kind = T_NTFS;  break;
    case LFS_USB_EXFAT: t->kind = T_EXFAT; break;
    default: return LFS_E_NOTFOUND;
    }
    snprintf(t->rest, sizeof t->rest, "%s", rest);
    return 0;
}

static bool target_alive(const Target &t, uint32_t epoch) {   // table lock NOT held
    if (t.port < 0) return true;
    Guard g(LOCK_TABLE);
    return s_slot[t.port].state == SLOT_MOUNTED && s_slot[t.port].epoch == epoch;
}

// One sector of the volume, straight from the device: can the drive still be read at all?
static bool volume_readable(const Target &t) {
    static uint8_t sector[4096] __attribute__((aligned(32)));
    uint32_t got = 0;
    int handle; uint64_t lba;
    if (t.kind == T_NTFS) { handle = s_ntfs[t.port].storageHandle;  lba = s_ntfs[t.port].partitionOffset; }
    else                  { handle = s_exfat[t.port].storageHandle; lba = s_exfat[t.port].partitionOffset; }
    return readStorageRaw(handle, lba, 1, sector, &got) == 0 && got == 1;
}

// Why a call on an NTFS/exFAT volume came back empty-handed (io lock held): the drive is gone
// (LFS_E_REMOVED), it is there but cannot be read (LFS_E_IO), or the thing asked for is not on it
// (LFS_E_NOTFOUND).
static int why_failed(const Target &t, uint32_t epoch) {
    if (!target_alive(t, epoch)) return LFS_E_REMOVED;
    if (P.usb_present && !P.usb_present(t.port)) return LFS_E_REMOVED;
    return volume_readable(t) ? LFS_E_NOTFOUND : LFS_E_IO;
}

static uint32_t port_epoch(int port) {
    Guard g(LOCK_TABLE);
    return s_slot[port].epoch;
}

// ---- a directory, read once -------------------------------------------------------

struct Rec { uint32_t name_off; bool is_dir; uint64_t size, mtime; };

#define SNAP_TTL_US  10000000ULL     // a directory is read again after this long (files come and go)

static struct Snap {
    bool     valid;
    char     path[1100];
    uint32_t gen;
    uint64_t built_us;
    int      n;
    Rec     *rec;           // sorted
    char    *pool;
    size_t   pool_len, pool_cap;
    int      cap;
} s_snap;

static void snap_free(void) {
    free(s_snap.rec); free(s_snap.pool);
    memset(&s_snap, 0, sizeof s_snap);
}

static bool snap_add(const char *name, bool is_dir, uint64_t size, uint64_t mtime) {
    const size_t len = strlen(name);
    if (len == 0 || len >= 256) return true;              // not representable: skipped
    if (s_snap.n >= MAX_ENTRIES) return false;
    if (s_snap.n >= s_snap.cap) {
        const int cap = s_snap.cap ? s_snap.cap * 2 : 128;
        Rec *r = (Rec *)realloc(s_snap.rec, (size_t)cap * sizeof(Rec));
        if (!r) return false;
        s_snap.rec = r; s_snap.cap = cap;
    }
    if (s_snap.pool_len + len + 1 > s_snap.pool_cap) {
        const size_t cap = s_snap.pool_cap ? s_snap.pool_cap * 2 : 8192;
        char *pl = (char *)realloc(s_snap.pool, cap);
        if (!pl) return false;
        s_snap.pool = pl; s_snap.pool_cap = cap;
    }
    memcpy(s_snap.pool + s_snap.pool_len, name, len + 1);
    Rec &r = s_snap.rec[s_snap.n++];
    r.name_off = (uint32_t)s_snap.pool_len;
    r.is_dir = is_dir; r.size = size; r.mtime = mtime;
    s_snap.pool_len += len + 1;
    return true;
}

static bool listed(const char *name, bool is_dir) {         // what a listing shows
    if (lfs_name_hidden(name)) return false;
    return is_dir || lfs_media_kind_of(name) != LFS_KIND_OTHER;
}

struct NativeCtx { const char *dir; bool full; };
static void native_cb(const char *name, bool is_dir, void *ctx) {
    NativeCtx *c = (NativeCtx *)ctx;
    if (!listed(name, is_dir)) return;
    uint64_t size = 0, mtime = 0;
    if (!is_dir) {
        char full[MAX_NATIVE_PATH];
        snprintf(full, sizeof full, "%s/%s", c->dir, name);
        bool d = false;
        if (P.fs_stat) P.fs_stat(full, &d, &size, &mtime);
    }
    if (!snap_add(name, is_dir, size, mtime)) c->full = true;
}

static int sort_cmp(const void *pa, const void *pb) {
    const Rec *a = (const Rec *)pa, *b = (const Rec *)pb;
    return lfs_entry_cmp(a->is_dir, s_snap.pool + a->name_off, b->is_dir, s_snap.pool + b->name_off);
}

#define ATTR_HIDDEN  0x2
#define ATTR_SYSTEM  0x4

// Reads the directory into s_snap (io lock held).  0 or a LFS_E_ code.
static int snap_read(const Target &t, uint32_t epoch) {
    if (t.kind == T_HDD_ROOT) {
        static const char *const dirs[] = { "video", "music", "jellyfin_local" };
        for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
            char nat[64];
            snprintf(nat, sizeof nat, NATIVE_HDD_ROOT "/%s", dirs[i]);
            bool d = false; uint64_t sz = 0, mt = 0;
            if (P.fs_stat && P.fs_stat(nat, &d, &sz, &mt) == 0 && d) snap_add(dirs[i], true, 0, 0);
        }
        return 0;
    }
    if (t.kind == T_NATIVE) {
        bool d = false; uint64_t sz = 0, mt = 0;
        if (!P.fs_stat || P.fs_stat(t.native, &d, &sz, &mt) != 0) return LFS_E_NOTFOUND;
        if (!d) return LFS_E_BADPATH;
        NativeCtx c = { t.native, false };
        if (!P.fs_list || P.fs_list(t.native, native_cb, &c) != 0) return target_alive(t, epoch) ? LFS_E_IO : LFS_E_REMOVED;
        return 0;
    }
    char name[768];
    if (t.kind == T_NTFS) {
        NtfsVolume *v = &s_ntfs[t.port];
        NtfsInfo di;
        if (statNtfs(v, t.rest, &di) != 0) return why_failed(t, epoch);
        if (!di.isDir) return LFS_E_BADPATH;
        NtfsDir dir;
        openNtfsDir(&dir, v, di.mftReference);
        NtfsInfo ei;
        int rc;
        while ((rc = readNtfsDir(&dir, name, sizeof name, &ei)) == 1) {
            if (ei.attributes & (ATTR_HIDDEN | ATTR_SYSTEM)) continue;
            if (!listed(name, ei.isDir)) continue;
            if (!snap_add(name, ei.isDir != 0, ei.isDir ? 0 : ei.size, ei.mtime)) break;
        }
        closeNtfsDir(&dir);
        if (rc < 0) return why_failed(t, epoch) == LFS_E_REMOVED ? LFS_E_REMOVED : LFS_E_IO;
        return 0;
    }
    ExfatVolume *v = &s_exfat[t.port];
    ExfatInfo di;
    if (statExfat(v, t.rest, &di) != 0) return why_failed(t, epoch);
    if (!di.isDir) return LFS_E_BADPATH;
    ExfatDir dir;
    openExfatDir(&dir, v, di.firstCluster, di.noFatChain, di.size);
    ExfatInfo ei;
    int rc;
    while ((rc = readExfatDir(&dir, name, sizeof name, &ei)) == 1) {
        if (!listed(name, ei.isDir)) continue;
        if (!snap_add(name, ei.isDir != 0, ei.isDir ? 0 : ei.size, ei.mtime)) break;
    }
    closeExfatDir(&dir);
    return rc < 0 ? (why_failed(t, epoch) == LFS_E_REMOVED ? LFS_E_REMOVED : LFS_E_IO) : 0;
}

int lfs_list(const char *path, lfs_entry *out, int max, int offset) {
    Target t;
    int rc = resolve(path, &t);
    if (rc < 0) return rc;
    char clean[1100];
    {   // the snapshot's key: the cleaned path
        char d[16], r[1024];
        if (strncmp(path, NATIVE_HDD_ROOT "/", strlen(NATIVE_HDD_ROOT) + 1) == 0) snprintf(clean, sizeof clean, "%s", path);
        else { lfs_path_split(path, d, sizeof d, r, sizeof r); snprintf(clean, sizeof clean, "%s:%s", d, r); }
    }
    const uint32_t epoch = t.port >= 0 ? port_epoch(t.port) : 0;
    Guard io(LOCK_IO);
    const uint32_t gen = lfs_generation();
    const uint64_t now = P.now_us ? P.now_us() : 0;
    if (!(s_snap.valid && s_snap.gen == gen && now - s_snap.built_us < SNAP_TTL_US &&
          strcmp(s_snap.path, clean) == 0)) {
        snap_free();
        rc = snap_read(t, epoch);
        if (rc < 0) { snap_free(); return rc; }
        if (s_snap.n > 1) qsort(s_snap.rec, (size_t)s_snap.n, sizeof(Rec), sort_cmp);
        s_snap.valid = true; s_snap.gen = gen; s_snap.built_us = now;
        snprintf(s_snap.path, sizeof s_snap.path, "%s", clean);
    }
    for (int i = 0; i < max && offset + i < s_snap.n; i++) {
        const Rec &r = s_snap.rec[offset + i];
        lfs_entry *e = &out[i];
        memset(e, 0, sizeof *e);
        snprintf(e->name, sizeof e->name, "%s", s_snap.pool + r.name_off);
        e->is_dir = r.is_dir; e->size = r.size; e->mtime = r.mtime;
    }
    return s_snap.n;
}

bool lfs_stat(const char *path, lfs_entry *out) {
    Target t;
    if (resolve(path, &t) < 0) return false;
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "%s", lfs_path_leaf(path));
    const uint32_t epoch = t.port >= 0 ? port_epoch(t.port) : 0;
    Guard io(LOCK_IO);
    switch (t.kind) {
    case T_HDD_ROOT: out->is_dir = true; return true;
    case T_NATIVE: {
        bool d = false;
        if (!P.fs_stat || P.fs_stat(t.native, &d, &out->size, &out->mtime) != 0) return false;
        out->is_dir = d;
        return true;
    }
    case T_NTFS: {
        if (!target_alive(t, epoch)) return false;
        NtfsInfo i;
        if (statNtfs(&s_ntfs[t.port], t.rest, &i) != 0) return false;
        out->is_dir = i.isDir != 0; out->size = i.isDir ? 0 : i.size; out->mtime = i.mtime;
        return true;
    }
    case T_EXFAT: {
        if (!target_alive(t, epoch)) return false;
        ExfatInfo i;
        if (statExfat(&s_exfat[t.port], t.rest, &i) != 0) return false;
        out->is_dir = i.isDir != 0; out->size = i.isDir ? 0 : i.size; out->mtime = i.mtime;
        return true;
    }
    default: return false;
    }
}

// ---- files ----------------------------------------------------------------------

struct Handle {
    bool       used;
    TargetKind kind;
    int        port;
    uint32_t   epoch;
    int        fd;
    NtfsFile   nf;
    ExfatFile  ef;
    uint64_t   size;
    uint64_t   pos;          // where the reader's next byte is (NTFS/exFAT keep their own cursor)
};

static Handle s_h[MAX_HANDLES];

int lfs_open(const char *path) {
    Target t;
    int rc = resolve(path, &t);
    if (rc < 0) return rc;
    if (t.kind == T_HDD_ROOT) return LFS_E_BADPATH;
    const uint32_t epoch = t.port >= 0 ? port_epoch(t.port) : 0;
    Guard io(LOCK_IO);
    int slot = -1;
    for (int i = 0; i < MAX_HANDLES; i++) if (!s_h[i].used) { slot = i; break; }
    if (slot < 0) return LFS_E_TOOMANY;
    Handle &h = s_h[slot];
    memset(&h, 0, sizeof h);
    h.kind = t.kind; h.port = t.port; h.epoch = epoch; h.fd = -1;
    switch (t.kind) {
    case T_NATIVE: {
        bool d = false; uint64_t sz = 0, mt = 0;
        if (!P.fs_stat || P.fs_stat(t.native, &d, &sz, &mt) != 0) return LFS_E_NOTFOUND;
        if (d) return LFS_E_BADPATH;
        h.fd = P.fs_open ? P.fs_open(t.native) : -1;
        if (h.fd < 0) return target_alive(t, epoch) ? LFS_E_IO : LFS_E_REMOVED;
        h.size = sz;
        break;
    }
    case T_NTFS:
        if (!target_alive(t, epoch)) return LFS_E_REMOVED;
        if (openNtfs(&h.nf, &s_ntfs[t.port], t.rest) != 0) return why_failed(t, epoch);   // (compressed or encrypted data also lands here)
        h.size = h.nf.size;
        break;
    case T_EXFAT:
        if (!target_alive(t, epoch)) return LFS_E_REMOVED;
        if (openExfat(&h.ef, &s_exfat[t.port], t.rest) != 0) return why_failed(t, epoch);
        h.size = h.ef.size;
        break;
    default: return LFS_E_BADPATH;
    }
    h.used = true;
    return slot;
}

uint64_t lfs_size(int hnd) {
    Guard io(LOCK_IO);
    return (hnd >= 0 && hnd < MAX_HANDLES && s_h[hnd].used) ? s_h[hnd].size : 0;
}

void lfs_close(int hnd) {
    Guard io(LOCK_IO);
    if (hnd < 0 || hnd >= MAX_HANDLES || !s_h[hnd].used) return;
    Handle &h = s_h[hnd];
    if (h.kind == T_NATIVE && h.fd >= 0 && P.fs_close) P.fs_close(h.fd);
    if (h.kind == T_NTFS)  closeNtfs(&h.nf);
    if (h.kind == T_EXFAT) closeExfat(&h.ef);
    memset(&h, 0, sizeof h);
}

static void bench_note(int port, uint64_t bytes, uint64_t us) {
    Guard g(LOCK_TABLE);
    Slot &s = s_slot[port];
    if (s.bench_logged) return;
    s.rd_bytes += bytes; s.rd_us += us;
    if (s.rd_bytes >= BENCH_BYTES) {
        s.bench_logged = true;
        const uint64_t ms = s.rd_us / 1000ULL;
        log_line("lfs: usb%d %s read %llu MB in %llu ms (%llu MB/s)", port, lfs_kind_name(s.kind),
                 (unsigned long long)(s.rd_bytes >> 20), (unsigned long long)ms,
                 (unsigned long long)(ms ? (s.rd_bytes >> 20) * 1000ULL / ms : 0));
    }
}

// One slice under the io lock.  Returns bytes, 0 at the end, or a LFS_E_ code.
static int read_slice(int hnd, uint64_t off, uint8_t *buf, uint32_t n) {
    Guard io(LOCK_IO);
    if (hnd < 0 || hnd >= MAX_HANDLES || !s_h[hnd].used) return LFS_E_IO;
    Handle &h = s_h[hnd];
    if (off >= h.size) return 0;
    if (off + n > h.size) n = (uint32_t)(h.size - off);
    if (h.kind == T_NATIVE) {
        if (h.port >= 0) {                               // a FAT32 stick: still the drive it was opened on?
            Guard g(LOCK_TABLE);
            const Slot &s = s_slot[h.port];
            if (s.state != SLOT_MOUNTED || s.epoch != h.epoch) return LFS_E_REMOVED;
        }
        const int r = P.fs_read ? P.fs_read(h.fd, off, buf, n) : -1;
        if (r >= 0) return r;
        return (h.port >= 0 && P.usb_present && !P.usb_present(h.port)) ? LFS_E_REMOVED : LFS_E_IO;
    }
    {
        Guard g(LOCK_TABLE);
        const Slot &s = s_slot[h.port];
        if (s.state != SLOT_MOUNTED || s.epoch != h.epoch) return LFS_E_REMOVED;
    }
    const uint64_t t0 = P.now_us ? P.now_us() : 0;
    int got = 0;
    if (h.kind == T_NTFS) {
        if (h.pos != off) { seekNtfs(&h.nf, off); h.pos = off; }
        got = readNtfs(&h.nf, buf, (int)n);
    } else {
        if (h.pos != off) { seekExfat(&h.ef, off); h.pos = off; }
        got = readExfat(&h.ef, buf, (int)n);
    }
    if (got < 0) {
        h.pos = ~0ULL;                                   // the cursor is unknown: seek next time
        bool present = P.usb_present ? P.usb_present(h.port) : true;
        return present ? LFS_E_IO : LFS_E_REMOVED;
    }
    h.pos = off + (uint64_t)got;
    bench_note(h.port, (uint64_t)got, P.now_us ? P.now_us() - t0 : 0);
    return got;
}

int lfs_read(int hnd, uint64_t off, void *buf, uint32_t n) {
    uint8_t *p = (uint8_t *)buf;
    uint32_t done = 0;
    while (done < n) {
        const uint32_t want = (n - done) < READ_SLICE ? (n - done) : READ_SLICE;
        const int r = read_slice(hnd, off + done, p + done, want);
        if (r < 0) return done ? (int)done : r;          // what was read stands; the next call reports the fault
        if (r == 0) break;
        done += (uint32_t)r;
    }
    return (int)done;
}
