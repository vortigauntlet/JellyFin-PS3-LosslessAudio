#pragma once
#include <stdint.h>
#include "dl_model.h"

// -------------------------------------------------------------------------
//  Offline downloads -- the download manager
// -------------------------------------------------------------------------
//  One service, not a screen: the item page's DOWNLOAD FOR OFFLINE, the
//  Downloads list and the Offline library all talk to this, and none of them
//  owns a transfer.  It keeps the queue, runs one transfer at a time on the
//  caller's worker thread (dl_ps3.cpp on the console, the test itself on the
//  host), and persists every state change so a power cut loses nothing but
//  the bytes since the last checkpoint.
//
//  Threading: every public call is safe from any thread.  dl_manager_step()
//  must only ever be called from ONE thread (the worker); the transfer itself
//  runs there without holding the lock, and control calls on the active item
//  (pause/cancel/remove) are posted to it and take effect within about a
//  second -- the socket's receive timeout.
//
//  Transfers: a plain HTTP GET streamed to <item>/media.ts.part through one
//  fixed 128 KB buffer -- never the file in RAM.  When bytes are already on
//  disk the request carries "Range: bytes=N-":
//    206 from N       -> appended; this is a real resume
//    200              -> the server ignored the range (a live transcode
//                        does); the partial is discarded and it starts over
//    206 not from N   -> inconsistent; discarded, retried from 0
//    416 */N with N bytes already here -> it was already complete
//  The file only becomes media.ts once the length is verified.
// -------------------------------------------------------------------------

typedef enum {
    DL_OK = 0,
    DL_E_NOT_READY,   // dl_manager_init() has not succeeded
    DL_E_INVALID,     // bad id / url / arguments
    DL_E_EXISTS,      // already downloaded or already in the queue
    DL_E_FULL,        // DL_MAX_ITEMS reached
    DL_E_NO_SPACE,    // the HDD cannot take it
    DL_E_IO,          // could not write the item's files
    DL_E_NOT_FOUND,   // no such item
    DL_E_STATE,       // not possible in the item's current state
} DlResult;

typedef struct {
    DlRecord rec;
    uint32_t retry_in_ms;   // > 0 while waiting out a backoff
    bool     active;        // the worker is transferring it right now
} DlStatus;

// Tunables.  The defaults are for the console; the host tests shrink them.
typedef struct {
    uint64_t reserve_bytes;      // HDD space never used by downloads
    uint32_t head_timeout_ms;    // wait for response headers
    uint32_t idle_timeout_ms;    // body silence that counts as a timeout
    uint32_t checkpoint_ms;      // persist progress at least this often
    uint64_t checkpoint_bytes;   // ...or at least every this many bytes
    uint64_t space_check_bytes;  // re-check free space this often
} DlConfig;

void dl_config_defaults(DlConfig *c);

// Opens (creating if needed) the store at root and restores every item:
// an item that was DOWNLOADING when the app stopped goes back to QUEUED with
// its partial intact; damaged records are recovered from what is on disk or
// surfaced as FAILED/corrupt -- never silently dropped, since they still
// hold HDD space.  Needs no network.  cfg may be NULL.
bool dl_manager_init(const char *root, const DlConfig *cfg);
void dl_manager_shutdown(void);
bool dl_manager_ready(void);

// The auth header sent with every transfer, as a complete header line
// without CRLF.  Kept in memory only: records never contain a token.
void dl_set_auth_header(const char *line);

// Queue an item.  meta is saved beside the media and must carry at least id
// and title; url is the media request (http://, no token).  size_hint is the
// expected size in bytes if known (0 if not) and is checked against free
// space now, so an item that cannot fit is refused up front rather than
// failing half way.  A FAILED or CANCELLED item is re-queued with the new
// url/meta, keeping any partial data.
DlResult dl_enqueue(const DlMeta *meta, const char *url, uint64_t size_hint);
DlResult dl_pause(const char *id);
DlResult dl_resume(const char *id);
DlResult dl_cancel(const char *id);    // deletes partial data, keeps the entry
DlResult dl_retry(const char *id);
DlResult dl_remove(const char *id);    // deletes everything for the item

// Snapshots, in queue order.  completed_only gives the Offline library.
int  dl_list(DlStatus *out, int max, bool completed_only);
bool dl_find(const char *id, DlStatus *out);
bool dl_load_meta(const char *id, DlMeta *out);
// Path of the finished media file; false unless the item is COMPLETED.
bool dl_media_path(const char *id, char *out, int cap);

// ---- worker side ----------------------------------------------------------
// While suspended nothing starts, and an active transfer stops at its next
// read and goes back to QUEUED with its data (not PAUSED: the user did not
// ask).  The player suspends downloads so they never compete with a stream.
void dl_set_suspended(bool suspended);
// Runs one transfer attempt on the next due item.  Blocks for the length of
// the attempt.  Returns false when nothing was due.
bool dl_manager_step(void);
// How long the worker may sleep before something becomes due (capped at
// 1000 ms so control changes are noticed).
uint32_t dl_next_wake_ms(void);
