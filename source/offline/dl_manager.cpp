// Offline downloads -- the download manager.  See dl_manager.h.

#include "dl_manager.h"
#include "dl_http.h"
#include "dl_platform.h"
#include "dl_store.h"

#include <stdio.h>
#include <string.h>

// One transfer buffer for the process.  128 KB: large enough that a 25 Mbps
// link needs ~25 reads a second, small enough to be irrelevant next to the
// player's reservations.  The file never passes through RAM beyond this.
#define DL_XFER_BUF (128 * 1024)

enum { CTL_NONE = 0, CTL_PAUSE, CTL_CANCEL, CTL_REMOVE };

typedef struct {
    bool     used;
    bool     active;         // owned by the worker; rec is the worker's to write
    bool     removing;       // remove requested while active: hidden already
    volatile int ctl;        // posted to the worker for the active item
    uint64_t retry_at_ms;    // not persisted: backoff restarts after a reboot
    DlRecord rec;
} Slot;

static Slot     s_slots[DL_MAX_ITEMS];
static bool     s_ready = false;
static DlConfig s_cfg;
static char     s_auth[512] = "";
static volatile bool s_suspended = false;
static uint32_t s_next_seq = 1;
static uint8_t  s_buf[DL_XFER_BUF];

#define LOCK()   dl_plat_lock()
#define UNLOCK() dl_plat_unlock()

static void logf_id(const char *id, const char *what) {
    char b[128];
    snprintf(b, sizeof(b), "dl: %.8s %s", id, what);
    dl_plat_log(b);
}

void dl_config_defaults(DlConfig *c) {
    // 1 GB of headroom: the console needs free HDD for its own caches and
    // for game data, and a download that fills the disk to the last byte
    // breaks things well outside this app.
    c->reserve_bytes     = 1024ull * 1024 * 1024;
    // Same header deadline as the player's stream_open(): a transcode that
    // extracts a subtitle or re-encodes 4K can take over a minute to start.
    c->head_timeout_ms   = 120000;
    c->idle_timeout_ms   = 30000;
    c->checkpoint_ms     = 2000;
    c->checkpoint_bytes  = 8ull * 1024 * 1024;
    c->space_check_bytes = 64ull * 1024 * 1024;
}

bool dl_manager_ready(void) { return s_ready; }

void dl_set_auth_header(const char *line) {
    LOCK();
    snprintf(s_auth, sizeof(s_auth), "%s", line ? line : "");
    UNLOCK();
}

void dl_set_suspended(bool suspended) { s_suspended = suspended; }

// -------------------------------------------------------------------------
// Slot helpers (callers hold the lock)
// -------------------------------------------------------------------------

static Slot *find_slot(const char *id) {
    for (int i = 0; i < DL_MAX_ITEMS; i++)
        if (s_slots[i].used && !s_slots[i].removing &&
            strcmp(s_slots[i].rec.id, id) == 0)
            return &s_slots[i];
    return NULL;
}

static Slot *free_slot(void) {
    for (int i = 0; i < DL_MAX_ITEMS; i++)
        if (!s_slots[i].used) return &s_slots[i];
    return NULL;
}

// Applies ev if legal.  False (record untouched) otherwise.
static bool apply(DlRecord *r, DlEvent ev) {
    DlState n;
    if (!dl_next_state(r->state, ev, &n)) return false;
    r->state = n;
    return true;
}

static bool space_ok(uint64_t need) {
    uint64_t freeb = dl_plat_free_bytes(dl_store_root());
    if (freeb == DL_FREE_UNKNOWN) return true;
    return freeb >= s_cfg.reserve_bytes && freeb - s_cfg.reserve_bytes >= need;
}

// -------------------------------------------------------------------------
// Restore
// -------------------------------------------------------------------------

static void restore_one(const char *id) {
    Slot *s = free_slot();
    if (!s) { logf_id(id, "restore: table full, skipped"); return; }

    char media[DL_PATH_MAX], part[DL_PATH_MAX];
    dl_store_item_file(media, sizeof(media), id, DL_FILE_MEDIA);
    dl_store_item_file(part,  sizeof(part),  id, DL_FILE_PART);
    const int64_t media_sz = dl_plat_file_size(media);
    const int64_t part_sz  = dl_plat_file_size(part);

    static DlMeta m;   // init runs single-threaded
    const bool have_meta = dl_store_load_meta(id, &m);

    DlRecord r;
    if (dl_store_load_record(id, &r)) {
        if (r.state == DL_DOWNLOADING) apply(&r, DL_EV_INTERRUPTED);
        if (r.state != DL_COMPLETED && media_sz > 0 &&
            (r.bytes_total == 0 || (uint64_t)media_sz == r.bytes_total)) {
            // Stopped between renaming the finished file and saving the
            // record that says so.  The file is the truth.
            r.state = DL_COMPLETED;
            r.error = DL_ERR_NONE;
        }
        if (r.state == DL_COMPLETED) {
            if (media_sz <= 0 ||
                (r.bytes_total > 0 && (uint64_t)media_sz != r.bytes_total)) {
                r.state = DL_FAILED;
                r.error = DL_ERR_CORRUPT;
                r.bytes_done = part_sz > 0 ? (uint64_t)part_sz : 0;
                logf_id(id, "restore: media missing or wrong size");
            } else {
                r.bytes_done = r.bytes_total = (uint64_t)media_sz;
            }
        } else {
            // What is on disk beats what the record last checkpointed: the
            // record can lag the file by up to one checkpoint.
            r.bytes_done = part_sz > 0 ? (uint64_t)part_sz : 0;
            if (r.bytes_total > 0 && r.bytes_done > r.bytes_total) {
                dl_plat_truncate(part);
                r.bytes_done = 0;
            }
        }
    } else {
        // No readable record.  Rebuild one from the files so the item is
        // either playable again or at least visible (and removable).
        dl_record_init(&r);
        snprintf(r.id, sizeof(r.id), "%s", id);
        snprintf(r.title, sizeof(r.title), "%s", have_meta ? m.title : id);
        if (have_meta && media_sz > 0) {
            r.state = DL_COMPLETED;
            r.bytes_done = r.bytes_total = (uint64_t)media_sz;
            logf_id(id, "restore: record rebuilt from media");
        } else {
            r.state = DL_FAILED;
            r.error = DL_ERR_CORRUPT;
            r.bytes_done = part_sz > 0 ? (uint64_t)part_sz : 0;
            logf_id(id, "restore: record damaged");
        }
    }
    memset(s, 0, sizeof(*s));
    s->used = true;
    s->rec  = r;
    if (r.seq >= s_next_seq) s_next_seq = r.seq + 1;
}

bool dl_manager_init(const char *root, const DlConfig *cfg) {
    s_ready = false;
    if (cfg) s_cfg = *cfg; else dl_config_defaults(&s_cfg);
    if (!dl_store_init(root)) return false;

    LOCK();
    memset(s_slots, 0, sizeof(s_slots));
    s_next_seq = 1;
    static char ids[DL_MAX_ITEMS][DL_ID_MAX];
    int n = dl_store_list_ids(ids, DL_MAX_ITEMS);
    for (int i = 0; i < n; i++) restore_one(ids[i]);
    // Rebuilt records have no queue position yet: put them after the rest,
    // then persist whatever restore changed.
    for (int i = 0; i < DL_MAX_ITEMS; i++) {
        Slot *s = &s_slots[i];
        if (!s->used) continue;
        if (s->rec.seq == 0) s->rec.seq = s_next_seq++;
        dl_store_save_record(&s->rec);
    }
    s_ready = true;
    UNLOCK();
    {
        char b[64];
        snprintf(b, sizeof(b), "dl: restored %d item(s) from %s", n, root);
        dl_plat_log(b);
    }
    return true;
}

void dl_manager_shutdown(void) {
    LOCK();
    s_ready = false;
    UNLOCK();
}

// -------------------------------------------------------------------------
// Control API
// -------------------------------------------------------------------------

DlResult dl_enqueue(const DlMeta *meta, const char *url, uint64_t size_hint) {
    if (!s_ready) return DL_E_NOT_READY;
    if (!meta || !url || !dl_id_valid(meta->id) || !meta->title[0])
        return DL_E_INVALID;
    DlUrl u;
    if (strlen(url) >= DL_URL_MAX || !dl_url_parse(url, &u)) return DL_E_INVALID;

    DlResult res = DL_OK;
    LOCK();
    Slot *s = find_slot(meta->id);
    bool requeue = false;
    if (s) {
        if (s->rec.state != DL_FAILED && s->rec.state != DL_CANCELLED) {
            UNLOCK();
            return DL_E_EXISTS;
        }
        requeue = true;
    } else if (!(s = free_slot())) {
        UNLOCK();
        return DL_E_FULL;
    }
    if (requeue && s->rec.error == DL_ERR_CORRUPT) {
        // Its record was lost, so nothing vouches for the partial's bytes
        // (which file, which quality, which offset): start clean.
        dl_store_remove_partial(meta->id);
        s->rec.bytes_done = 0;
        s->rec.bytes_total = 0;
    }
    uint64_t have = requeue ? s->rec.bytes_done : 0;
    uint64_t need = size_hint > have ? size_hint - have : 0;
    if (!space_ok(need)) { UNLOCK(); return DL_E_NO_SPACE; }

    if (!dl_store_make_item_dir(meta->id) || !dl_store_save_meta(meta)) {
        if (!requeue) dl_store_remove_item(meta->id);
        UNLOCK();
        return DL_E_IO;
    }
    DlRecord r;
    if (requeue) {
        r = s->rec;
        apply(&r, DL_EV_RETRY);
    } else {
        dl_record_init(&r);
        snprintf(r.id, sizeof(r.id), "%s", meta->id);
        r.seq = s_next_seq++;
    }
    snprintf(r.title, sizeof(r.title), "%s", meta->title);
    snprintf(r.url, sizeof(r.url), "%s", url);
    r.state = DL_QUEUED;
    r.error = DL_ERR_NONE;
    r.http_status = 0;
    r.attempts = 0;
    if (!requeue) r.bytes_total = size_hint;   // refined by the response
    if (!dl_store_save_record(&r)) {
        if (!requeue) dl_store_remove_item(meta->id);
        res = DL_E_IO;
    } else {
        if (!requeue) memset(s, 0, sizeof(*s));
        s->used = true;
        s->rec = r;
        s->retry_at_ms = 0;
        logf_id(r.id, requeue ? "re-queued" : "queued");
    }
    UNLOCK();
    return res;
}

// Shared body of pause/resume/cancel/retry.  On an idle item it is a plain
// transition; on the active one, ctl_if_active is posted to the worker.
static DlResult transition(const char *id, DlEvent ev, int ctl_if_active) {
    if (!s_ready) return DL_E_NOT_READY;
    LOCK();
    Slot *s = find_slot(id);
    if (!s) { UNLOCK(); return DL_E_NOT_FOUND; }
    if (s->active) {
        DlResult r = DL_E_STATE;
        if (ctl_if_active) {
            // Posted requests only ever escalate (pause < cancel < remove):
            // a pause pressed after a cancel must not rescue the download.
            if (ctl_if_active > s->ctl) s->ctl = ctl_if_active;
            r = DL_OK;
        } else if (ev == DL_EV_RESUME && s->ctl == CTL_PAUSE) {
            s->ctl = CTL_NONE;   // resumed before the pause landed
            r = DL_OK;
        }
        UNLOCK();
        return r;
    }
    DlRecord r = s->rec;
    if (!apply(&r, ev)) { UNLOCK(); return DL_E_STATE; }
    if (ev == DL_EV_RETRY && !r.url[0]) {
        // A record rebuilt from damaged files has no request to repeat;
        // it has to be downloaded again from the item page.
        UNLOCK();
        return DL_E_STATE;
    }
    if (ev == DL_EV_RESUME || ev == DL_EV_RETRY) {
        r.error = DL_ERR_NONE;
        r.attempts = 0;
        s->retry_at_ms = 0;
    }
    if (ev == DL_EV_CANCEL) {
        dl_store_remove_partial(id);
        r.bytes_done = 0;
        r.error = DL_ERR_NONE;
    }
    s->rec = r;
    dl_store_save_record(&r);
    UNLOCK();
    return DL_OK;
}

DlResult dl_pause(const char *id)  { return transition(id, DL_EV_PAUSE,  CTL_PAUSE); }
DlResult dl_resume(const char *id) { return transition(id, DL_EV_RESUME, CTL_NONE); }
DlResult dl_cancel(const char *id) { return transition(id, DL_EV_CANCEL, CTL_CANCEL); }
DlResult dl_retry(const char *id)  { return transition(id, DL_EV_RETRY,  CTL_NONE); }

DlResult dl_remove(const char *id) {
    if (!s_ready) return DL_E_NOT_READY;
    LOCK();
    Slot *s = find_slot(id);
    if (!s) { UNLOCK(); return DL_E_NOT_FOUND; }
    if (s->active) {
        // The worker deletes it when the transfer stops; hide it now.
        s->ctl = CTL_REMOVE;
        s->removing = true;
        UNLOCK();
        return DL_OK;
    }
    bool gone = dl_store_remove_item(id);
    s->used = false;
    UNLOCK();
    logf_id(id, gone ? "removed" : "removed (directory left behind)");
    return DL_OK;
}

// -------------------------------------------------------------------------
// Queries
// -------------------------------------------------------------------------

static void fill_status(const Slot *s, uint64_t now, DlStatus *out) {
    out->rec = s->rec;
    out->active = s->active;
    out->retry_in_ms = (s->rec.state == DL_QUEUED && s->retry_at_ms > now)
                           ? (uint32_t)(s->retry_at_ms - now) : 0;
}

int dl_list(DlStatus *out, int max, bool completed_only) {
    if (!s_ready) return 0;
    const uint64_t now = dl_plat_now_ms();
    LOCK();
    // Selection by increasing seq; 64 items makes the quadratic walk free.
    int n = 0;
    uint32_t last = 0;
    while (n < max) {
        const Slot *best = NULL;
        for (int i = 0; i < DL_MAX_ITEMS; i++) {
            const Slot *s = &s_slots[i];
            if (!s->used || s->removing || s->rec.seq <= last) continue;
            if (completed_only && s->rec.state != DL_COMPLETED) continue;
            if (!best || s->rec.seq < best->rec.seq) best = s;
        }
        if (!best) break;
        fill_status(best, now, &out[n++]);
        last = best->rec.seq;
    }
    UNLOCK();
    return n;
}

bool dl_find(const char *id, DlStatus *out) {
    if (!s_ready) return false;
    const uint64_t now = dl_plat_now_ms();
    LOCK();
    Slot *s = find_slot(id);
    if (s) fill_status(s, now, out);
    UNLOCK();
    return s != NULL;
}

bool dl_load_meta(const char *id, DlMeta *out) {
    if (!s_ready) return false;
    LOCK();
    bool ok = find_slot(id) && dl_store_load_meta(id, out);
    UNLOCK();
    return ok;
}

bool dl_media_path(const char *id, char *out, int cap) {
    if (!s_ready) return false;
    LOCK();
    Slot *s = find_slot(id);
    bool ok = s && s->rec.state == DL_COMPLETED &&
              dl_store_item_file(out, cap, id, DL_FILE_MEDIA);
    UNLOCK();
    return ok;
}

uint32_t dl_next_wake_ms(void) {
    uint32_t wake = 1000;
    if (!s_ready) return wake;
    const uint64_t now = dl_plat_now_ms();
    LOCK();
    for (int i = 0; i < DL_MAX_ITEMS; i++) {
        const Slot *s = &s_slots[i];
        if (!s->used || s->removing || s->rec.state != DL_QUEUED) continue;
        if (s->retry_at_ms <= now) { wake = 0; break; }
        uint64_t d = s->retry_at_ms - now;
        if (d < wake) wake = (uint32_t)d;
    }
    UNLOCK();
    return wake;
}

// -------------------------------------------------------------------------
// The transfer
// -------------------------------------------------------------------------

typedef enum { OUT_DONE, OUT_CTL, OUT_SUSPENDED, OUT_FAIL } OutKind;

typedef struct {
    OutKind  kind;
    DlError  err;
    int      http_status;
    bool     progressed;   // bytes moved this attempt
} Outcome;

static Outcome fail(DlError e, int status = 0) {
    Outcome o = { OUT_FAIL, e, status, false };
    return o;
}

// Posted control, or a suspension, that should stop the attempt now.
static bool stop_requested(const Slot *s, Outcome *o) {
    if (s->ctl != CTL_NONE) { o->kind = OUT_CTL; return true; }
    if (s_suspended)        { o->kind = OUT_SUSPENDED; return true; }
    return false;
}

// Publish progress to the table (always) and to disk (when asked).
static void publish(Slot *s, const DlRecord *r, bool persist) {
    LOCK();
    s->rec.bytes_done  = r->bytes_done;
    s->rec.bytes_total = r->bytes_total;
    s->rec.resumable   = r->resumable;
    s->rec.http_status = r->http_status;
    if (persist) dl_store_save_record(&s->rec);
    UNLOCK();
}

static Outcome attempt(Slot *s, DlRecord *r) {
    char part[DL_PATH_MAX], media[DL_PATH_MAX];
    dl_store_item_file(part,  sizeof(part),  r->id, DL_FILE_PART);
    dl_store_item_file(media, sizeof(media), r->id, DL_FILE_MEDIA);

    // The partial on disk is the resume point, whatever the record says.
    int64_t psz = dl_plat_file_size(part);
    uint64_t have = psz > 0 ? (uint64_t)psz : 0;
    if (r->bytes_total > 0 && have > r->bytes_total) {
        dl_plat_truncate(part);
        have = 0;
    }
    const uint64_t have_at_start = have;
    r->bytes_done = have;

    if (!space_ok(r->bytes_total > have ? r->bytes_total - have : 0))
        return fail(DL_ERR_NO_SPACE);

    DlUrl u;
    if (!dl_url_parse(r->url, &u)) return fail(DL_ERR_UNSUPPORTED);

    char auth[sizeof(s_auth)];
    LOCK();
    snprintf(auth, sizeof(auth), "%s", s_auth);
    UNLOCK();

    char req[DL_URL_MAX + 1024];
    int rlen = dl_http_build_get(req, sizeof(req), &u, have,
                                 auth[0] ? auth : NULL);
    if (rlen < 0) return fail(DL_ERR_UNSUPPORTED);

    int h = dl_plat_connect(u.host, u.port);
    if (h < 0) return fail(DL_ERR_UNREACHABLE);
    if (dl_plat_send(h, req, rlen) != rlen) {
        dl_plat_close(h);
        return fail(DL_ERR_NETWORK);
    }

    // ---- response head ----
    Outcome o = { OUT_FAIL, DL_ERR_NONE, 0, false };
    static DlHeadReader hr;   // 8 KB; only the worker thread gets here
    dl_head_reader_init(&hr);
    int body_off = 0, body_n = 0;
    uint64_t t0 = dl_plat_now_ms();
    while (!hr.done) {
        if (stop_requested(s, &o)) { dl_plat_close(h); return o; }
        int n = dl_plat_recv(h, s_buf, DL_XFER_BUF);
        if (n > 0) {
            int used = dl_head_reader_feed(&hr, s_buf, n);
            if (used < 0) { dl_plat_close(h); return fail(DL_ERR_BAD_RESPONSE); }
            body_off = used;
            body_n = n - used;
            continue;
        }
        if (n == DL_RECV_TIMEOUT) {
            if (dl_plat_now_ms() - t0 >= s_cfg.head_timeout_ms) {
                dl_plat_close(h);
                return fail(DL_ERR_TIMEOUT);
            }
            continue;
        }
        dl_plat_close(h);   // closed or reset before a complete head
        return fail(n == DL_RECV_CLOSED && hr.n == 0 ? DL_ERR_NETWORK
                                                     : DL_ERR_BAD_RESPONSE);
    }
    DlHttpHead hd;
    if (!dl_http_parse_head(hr.buf, hr.n, &hd)) {
        dl_plat_close(h);
        return fail(DL_ERR_BAD_RESPONSE);
    }
    r->http_status = hd.status;

    // ---- what the server did with the range ----
    bool restart = false;          // discard the partial and write from 0
    if (hd.status == 416) {
        dl_plat_close(h);
        if (hd.range_unsatisfied && hd.range_total >= 0 && have > 0 &&
            have == (uint64_t)hd.range_total) {
            r->bytes_total = have;   // nothing left to fetch
            goto verify;
        }
        dl_plat_truncate(part);
        r->bytes_done = 0;
        return fail(DL_ERR_BAD_RESPONSE, 416);
    }
    if (hd.status != 200 && hd.status != 206) {
        dl_plat_close(h);
        return fail(dl_error_for_http_status(hd.status), hd.status);
    }
    if (dl_http_content_type_is_error_page(hd.content_type)) {
        dl_plat_close(h);
        return fail(DL_ERR_BAD_RESPONSE, hd.status);
    }
    if (hd.status == 206) {
        const bool consistent =
            hd.has_range && !hd.range_unsatisfied && hd.range_start == have &&
            (r->bytes_total == 0 || hd.range_total < 0 ||
             (uint64_t)hd.range_total == r->bytes_total);
        if (!consistent) {
            // Not the bytes we asked for, or the file changed size under us:
            // appending would splice two different files together.
            dl_plat_close(h);
            dl_plat_truncate(part);
            r->bytes_done = 0;
            return fail(DL_ERR_BAD_RESPONSE, 206);
        }
        r->resumable = 1;
        if (hd.range_total >= 0) r->bytes_total = (uint64_t)hd.range_total;
        else if (hd.content_length >= 0)
            r->bytes_total = have + (uint64_t)hd.content_length;
    } else {
        // 200: a full body from byte 0, whatever we asked for.
        restart = have > 0;
        r->resumable = hd.accept_ranges ? 1 : 0;
        r->bytes_total = hd.content_length >= 0 ? (uint64_t)hd.content_length : 0;
    }
    if (restart) {
        logf_id(r->id, "server ignored Range; starting over");
        if (!dl_plat_truncate(part)) { dl_plat_close(h); return fail(DL_ERR_DISK); }
        have = 0;
        r->bytes_done = 0;
    }
    if (!space_ok(r->bytes_total > have ? r->bytes_total - have : 0)) {
        dl_plat_close(h);
        return fail(DL_ERR_NO_SPACE, hd.status);
    }

    {
        // ---- body ----
        int fh = dl_plat_file_open_append(part);
        if (fh < 0) { dl_plat_close(h); return fail(DL_ERR_DISK, hd.status); }

        DlChunked ch;
        dl_chunked_init(&ch);
        const int64_t cl = hd.content_length;   // -1 when chunked/unknown
        uint64_t body_got = 0;
        uint64_t last_ckpt_bytes = have, last_space_bytes = have;
        uint64_t last_ckpt_ms = dl_plat_now_ms(), last_rx_ms = last_ckpt_ms;
        bool finished = false;
        DlError err = DL_ERR_NONE;
        publish(s, r, true);

        uint8_t *data = s_buf + body_off;
        int n = body_n;
        for (;;) {
            if (n > 0) {
                last_rx_ms = dl_plat_now_ms();
                if (hd.chunked) {
                    n = dl_chunked_decode(&ch, data, n, data);
                    if (n < 0) { err = DL_ERR_BAD_RESPONSE; break; }
                } else if (cl >= 0 && body_got + (uint64_t)n > (uint64_t)cl) {
                    n = (int)((uint64_t)cl - body_got);   // ignore trailing junk
                }
                if (r->bytes_total > 0 && have + (uint64_t)n > r->bytes_total) {
                    err = DL_ERR_BAD_RESPONSE;             // more than declared
                    break;
                }
                if (n > 0 && dl_plat_file_write(fh, data, n) != n) {
                    err = space_ok(1) ? DL_ERR_DISK : DL_ERR_NO_SPACE;
                    break;
                }
                have     += (uint64_t)n;
                body_got += (uint64_t)n;
                r->bytes_done = have;

                const uint64_t now = dl_plat_now_ms();
                const bool ckpt = have - last_ckpt_bytes >= s_cfg.checkpoint_bytes ||
                                  now - last_ckpt_ms >= s_cfg.checkpoint_ms;
                if (ckpt) {
                    dl_plat_file_sync(fh);
                    last_ckpt_bytes = have;
                    last_ckpt_ms = now;
                }
                publish(s, r, ckpt);
                if (have - last_space_bytes >= s_cfg.space_check_bytes) {
                    last_space_bytes = have;
                    if (!space_ok(r->bytes_total > have ? r->bytes_total - have : 0)) {
                        err = DL_ERR_NO_SPACE;
                        break;
                    }
                }
            }
            if ((hd.chunked && ch.done) || (cl >= 0 && body_got >= (uint64_t)cl)) {
                finished = true;
                break;
            }
            if (stop_requested(s, &o)) break;

            n = dl_plat_recv(h, s_buf, DL_XFER_BUF);
            data = s_buf;
            if (n > 0) continue;
            if (n == DL_RECV_TIMEOUT) {
                n = 0;
                if (dl_plat_now_ms() - last_rx_ms >= s_cfg.idle_timeout_ms) {
                    err = DL_ERR_TIMEOUT;
                    break;
                }
                continue;
            }
            if (n == DL_RECV_CLOSED) {
                // Only a body with no declared length ends on close.
                if (!hd.chunked && cl < 0) finished = true;
                else err = DL_ERR_PARTIAL;
            } else {
                err = DL_ERR_NETWORK;
            }
            n = 0;
            break;
        }
        dl_plat_file_sync(fh);
        dl_plat_file_close(fh);
        dl_plat_close(h);
        r->bytes_done = have;
        o.progressed = have > have_at_start;
        o.http_status = hd.status;

        if (o.kind == OUT_CTL || o.kind == OUT_SUSPENDED) return o;
        if (!finished) {
            o.kind = OUT_FAIL;
            o.err = err;
            return o;
        }
        if (r->bytes_total == 0) r->bytes_total = have;
    }

verify:
    if (have == 0 || have != r->bytes_total) {
        Outcome f = fail(have == 0 ? DL_ERR_BAD_RESPONSE : DL_ERR_PARTIAL,
                         r->http_status);
        f.progressed = have > have_at_start;
        return f;
    }
    dl_plat_remove(media);
    if (!dl_plat_rename(part, media)) return fail(DL_ERR_DISK, r->http_status);
    r->bytes_done = have;
    Outcome done = { OUT_DONE, DL_ERR_NONE, r->http_status, have > have_at_start };
    return done;
}

static void finish(Slot *s, DlRecord *r, const Outcome *o) {
    const int ctl = s->ctl;
    s->active = false;
    s->ctl = CTL_NONE;

    if (ctl == CTL_REMOVE) {
        dl_store_remove_item(r->id);
        logf_id(r->id, "removed");
        s->used = false;
        s->removing = false;
        return;
    }
    char msg[96];
    if (o->kind == OUT_DONE) {
        // Completion is a fact; a pause or cancel racing it does not undo it.
        apply(r, DL_EV_COMPLETE);
        r->error = DL_ERR_NONE;
        r->attempts = 0;
        snprintf(msg, sizeof(msg), "completed, %llu bytes",
                 (unsigned long long)r->bytes_done);
    } else if (o->kind == OUT_CTL && ctl == CTL_CANCEL) {
        apply(r, DL_EV_CANCEL);
        dl_store_remove_partial(r->id);
        r->bytes_done = 0;
        r->error = DL_ERR_NONE;
        snprintf(msg, sizeof(msg), "cancelled");
    } else if (o->kind == OUT_CTL) {   // CTL_PAUSE
        apply(r, DL_EV_PAUSE);
        snprintf(msg, sizeof(msg), "paused at %llu",
                 (unsigned long long)r->bytes_done);
    } else if (o->kind == OUT_SUSPENDED) {
        apply(r, DL_EV_INTERRUPTED);
        s->retry_at_ms = 0;
        snprintf(msg, sizeof(msg), "suspended at %llu",
                 (unsigned long long)r->bytes_done);
    } else {
        r->error = o->err;
        if (o->http_status) r->http_status = o->http_status;
        if (dl_error_retryable(o->err)) {
            // Progress resets the count: only attempts that move nothing
            // add up to giving up.
            r->attempts = o->progressed ? 1 : r->attempts + 1;
        }
        if (dl_error_retryable(o->err) && r->attempts < DL_MAX_ATTEMPTS) {
            apply(r, DL_EV_FAIL_RETRY);
            uint32_t wait = dl_backoff_ms(r->attempts);
            s->retry_at_ms = dl_plat_now_ms() + wait;
            snprintf(msg, sizeof(msg), "%s (http %d), retry %u in %us",
                     dl_error_name(o->err), r->http_status,
                     (unsigned)r->attempts, (unsigned)(wait / 1000));
        } else {
            apply(r, DL_EV_FAIL);
            snprintf(msg, sizeof(msg), "failed: %s (http %d)",
                     dl_error_name(o->err), r->http_status);
        }
    }
    s->rec = *r;
    dl_store_save_record(r);
    logf_id(r->id, msg);
}

bool dl_manager_step(void) {
    if (!s_ready || s_suspended) return false;
    const uint64_t now = dl_plat_now_ms();
    LOCK();
    Slot *pick = NULL;
    for (int i = 0; i < DL_MAX_ITEMS; i++) {
        Slot *s = &s_slots[i];
        if (!s->used || s->removing || s->rec.state != DL_QUEUED) continue;
        if (s->retry_at_ms > now) continue;
        if (!pick || s->rec.seq < pick->rec.seq) pick = s;
    }
    if (!pick) { UNLOCK(); return false; }
    apply(&pick->rec, DL_EV_START);
    pick->active = true;
    pick->ctl = CTL_NONE;
    dl_store_save_record(&pick->rec);
    DlRecord r = pick->rec;
    UNLOCK();

    Outcome o = attempt(pick, &r);

    LOCK();
    finish(pick, &r, &o);
    UNLOCK();
    return true;
}
