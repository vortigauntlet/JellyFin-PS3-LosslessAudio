# Offline Downloads — Design & Implementation

Status: **Stages 1–2 implemented** (storage model, queue, state machine,
persistence, resumable network transfer), host-tested, and compiled into the
PS3 build. **Not yet user-visible**: nothing starts the service or enqueues
anything until Stage 3 wires in the item page. With this build installed, the
app behaves exactly as before and writes nothing new to the HDD.

## 1. Goal

From an item's page, **DOWNLOAD FOR OFFLINE** fetches a PS3-playable copy to
the HDD, survives interruptions and reboots, and plays through the
**existing** player from an Offline section that works with the server gone.

## 2. Architecture

```
 item page / Downloads / Offline        (Stage 5 UI -- screens only)
                 |
                 v
       dl_manager.h  -- the one service: queue, control, queries
        |       |        (enqueue/pause/resume/cancel/retry/remove,
        |       |         dl_list, dl_find, dl_media_path)
        |       +--> dl_model.h   state machine, records, error policy (pure)
        |       +--> dl_http.h    URL, Range request, head parser,
        |       |                 streaming chunked decoder (pure)
        |       +--> dl_store.h   on-disk layout, atomic record writes
        v
   dl_platform.h  -- the seam: ~20 calls that differ between PS3 and host
     |                         |
 dl_ps3.cpp (console)      tests/dl_fake.cpp (host tests)
 libnet via http.cpp,      temp dir + scripted fake server
 lv2 fs, lv2 mutex,
 worker thread (dl_service.h)
```

* **One service, not a screen.** The item page, the Downloads list and the
  Offline library all call `dl_manager.h`; none of them owns a transfer.
* **The same code runs on the host.** Everything above the seam is plain C++
  with no PSL1GHT headers. `tests/test_offline.cpp` compiles the *same files*
  the PS3 build does and drives them against a real temporary directory and a
  scripted HTTP server. The console-only file (`dl_ps3.cpp`) only makes direct
  system calls; it contains no logic of its own.
* **No new networking stack.** Resolution and connect come from `http.cpp`
  (the new `http_open_socket()`, under the lock `http_request()` already uses,
  because the PS3 resolver is not reentrant). The download reads the socket
  itself because neither existing reader fits:
  `http_request()` buffers the whole body (384 KB cap), and `stream.cpp`'s
  reader keeps its state in statics the player owns.

## 3. States

```
            START              COMPLETE
  QUEUED ----------> DOWNLOADING ------> COMPLETED   (terminal; only removed)
    ^  ^               |  |  |
    |  +- FAIL_RETRY --+  |  +-- FAIL --> FAILED --RETRY--> QUEUED
    |  +- INTERRUPTED -+  |
    |                     +-- PAUSE ---> PAUSED --RESUME--> QUEUED
  CANCEL (from QUEUED/DOWNLOADING/PAUSED/FAILED) --> CANCELLED --RETRY--> QUEUED
```

| State | Meaning | Partial data |
|---|---|---|
| queued | waiting its turn, or waiting out a retry backoff | kept |
| downloading | the worker is transferring it | growing |
| paused | the user paused it | kept |
| completed | `media.ts` complete and verified | — |
| failed | gave up; the user can retry | kept |
| cancelled | the user cancelled; entry kept so it can be retried | **deleted** |

`dl_next_state()` is the only thing that moves an item between states, and
any transition not in the table is refused as a no-op. A stale button press
therefore cannot corrupt a record. **Remove** is not a transition: the item
and all its files cease to exist.

Control on the **active** item is posted to the worker and takes effect
within about a second (the socket's receive timeout). Requests only escalate
(pause < cancel < remove), so a pause pressed after a cancel cannot rescue the
download. A resume pressed before a pending pause lands withdraws the pause.

**Suspension** (`dl_set_suspended`) is for playback. It sends the active item
back to QUEUED with its data, recorded as an interruption rather than an error
or a pause, and nothing starts until suspension lifts.

## 4. Storage

```
<root>/
  layout.txt              "jfdl-layout 1"
  items/<item id>/
    state.txt             DlRecord  -- queue + transfer bookkeeping
    meta.txt              DlMeta    -- identify, show and play it offline
    media.ts.part         while downloading
    media.ts              once complete and length-verified
    poster.jpg            artwork (optional)
    backdrop.jpg
```

**Root.** The first root that can be created *and written* wins. The write
probe is the layout marker; mkdir alone can succeed on a read-only mount.

| Target | Tried first | Fallback |
|---|---|---|
| Hardware | `/dev_hdd0/jellyfin_offline` | `/dev_hdd0/tmp/jellyfin_offline` |
| RPCS3 | `/dev_hdd0/game/JFPS30000/USRDIR/jellyfin_offline` | same `tmp` path |

Not `/dev_hdd0/tmp` by choice, since that is scratch space. Not the game
USRDIR on hardware, because `jf_paths.cpp` records that it is not reliably
writable while the title runs. The `tmp` fallback exists only because it is
the one place this app has proven writable on hardware. **To verify on
hardware:** the root in use is logged as `dl: root ... free=...MB`.

**Why per item.** An item is the unit everything happens to (enqueue, cancel,
delete, restore). With its files together, deleting it means removing a fixed
list of known names and then the directory. The store never recursively
deletes whatever it finds, so a wrong root cannot eat the HDD. Unknown files a
user drops into an item directory are left alone.

**Records** are `key=value` text, with enums written as *names*. The settings
files in this app learned that a persisted digit pins an enum's numbering
forever. Values escape `\`, newline and CR, so a title cannot forge a key.
Each file has a magic/version first line and an `end` last line, so a torn
write is detected rather than half-loaded. Unknown keys are ignored, so a newer
build's files still load in an older one.

Writes are **atomic**: they go to `<file>.tmp`, remove the old file, then
rename. lv2's rename does not promise to replace an existing file. A crash at
any point leaves either the old or the new version readable, and load falls
back to `.tmp`.

**No token is ever written to disk.** The URL is stored without credentials.
The auth header lives only in memory and is rebuilt from the session
(`dl_service_refresh_auth`).

## 5. Transfer

A plain HTTP/1.1 GET, streamed to `media.ts.part` through **one fixed 128 KB
buffer**, so the file never passes through RAM. When bytes are already on disk
the request carries `Range: bytes=N-`, where N is the partial's actual size on
disk rather than the record's, because the record can lag by one checkpoint.

| Server answer | Action |
|---|---|
| `206` from N, same total | append; a real resume |
| `200` | the Range was ignored (a live transcode does this): truncate, write from 0 |
| `206` from another offset, or a different total | discard and retry; appending would splice two files |
| `416`, `*/N` with N bytes already here | already complete, so verify and finish |
| `416` otherwise | discard, retry from 0 |
| `text/*` / JSON with 200 | refused: an error page must not be saved as a film |
| `401`/`403` | failed: `auth` |
| `404`/`410` | failed: `not_found` |
| other `4xx` | failed: `http` |
| `5xx`, `408`, `429` | retried: `server` |

Other failures:

| Condition | Error | Retried? |
|---|---|---|
| resolve/connect fails (server down, no network) | `unreachable` | yes |
| no head within 120 s (same as the player's transcode wait) / no body bytes for 30 s | `timeout` | yes |
| reset mid-body | `network` | yes |
| closed before Content-Length / before the last chunk | `partial` | yes |
| malformed head, bad chunk framing, more body than declared | `bad_response` | yes |
| `https://` (no TLS on this path, same as streaming) | `unsupported` | no |
| free space below need + reserve (1 GB), checked before, after the head, and every 64 MB | `no_space` | no |
| write fails | `disk`, or `no_space` if the disk is now full | no |

**Retry policy.** Backoff is 2 s, 4 s, 8 s … capped at 5 minutes. After 8
consecutive attempts that moved no bytes, the item goes to FAILED. **Progress
resets the count**, so a flaky link that keeps advancing never runs out of
retries. Backoff is not persisted: after a reboot a queued item is tried at
once.

**Checkpoints.** The record is saved every 8 MB or 2 s, and the file is fsynced
at the same time. In-memory progress, which the UI will read, updates on every
buffer.

**Completion.** The partial is renamed to `media.ts` only once its length
matches Content-Length/Content-Range, or the terminating chunk arrived. A
close-delimited body with no length is the one case where a close means done.

## 6. Restore (offline startup)

`dl_manager_init` reads only the HDD and makes no network call. For each item
directory whose name is a valid id:

| Found | Result |
|---|---|
| record says `downloading` | back to `queued`; resumes from the partial |
| record says `completed`, media present at the recorded size | completed |
| record says `completed`, media missing or wrong size | `failed`/`corrupt`; retry re-downloads |
| record not `completed` but `media.ts` present at the right size | completed (stopped between rename and record save) |
| record damaged, meta + media present | record **rebuilt**, completed |
| record damaged, no media | `failed`/`corrupt`, visible with its title (or id) so it can be removed; re-enqueueing starts clean |
| record filed under another item's directory | treated as damaged |
| directory name not a valid id | ignored |

Damaged items are never silently dropped, because they still hold HDD space.

## 7. Compatibility (Stage 3 design)

The download must be something the existing player can play, and the player
consumes MPEG-TS (`video/ts_demux.cpp`) requested by `build_stream_url()` in
`player/core/player_session.cpp`. That function is the capability engine: it
decides from the quality ladder (`vquality`), the 1080p toggle and the surround
mode whether video is copied or transcoded (`AllowVideoStreamCopy` with a
bitrate ceiling) and whether HD audio is stream-copied. **Stage 3 reuses it and
builds no second engine.** The plan is to split its query-string construction
into a function the downloader can call with `StartTimeTicks=0` and no
`PlaySessionId`. The server then makes the same original/remux/transcode
decision it makes for playback, and the file on disk is exactly what the
player would have streamed.

Consequence for resume: a stream-copied or transcoded `stream.ts` is produced
live, so Jellyfin serves it as a `200` with no Range support, and an
interrupted download of one restarts from zero. The transfer layer already
handles that correctly (§5, tested). Byte-exact resume applies when the server
honours Range, which is the case for static file responses. Stage 3 will
evaluate time-offset resume (`StartTimeTicks` plus TS splicing) or HLS
segments for transcodes.

## 8. Stages

| Stage | Scope | Status |
|---|---|---|
| 1 | persistent storage model, states, queue, bookkeeping, tests | **done** |
| 2 | network transfer, progress, retry, resume, cancellation | **done** (service built, not started) |
| 3 | Jellyfin integration: shared request builder, metadata + artwork capture, start the service after login, suspend around playback | next |
| 4 | offline library, offline startup path, playback of `media.ts` through the existing player (`stream_open` on a local file) | |
| 5 | UI: item-page action, Downloads list with progress, Offline section | |

## 9. Tests

```
cd tests && make -f Makefile.host test_offline && ./test_offline   # -v logs
```

The suite covers the state machine (every legal and illegal transition),
record and metadata round-trips, and malformed input of every kind: torn,
traversal, overflow, bad numbers. It also covers URL, request, head and chunked
parsing across every split point, and against the fake server:
complete, queue order and limits, progress/checkpoints, drop plus Range
resume, reset, idle and head timeouts, Range ignored, inconsistent 206, 416,
chunked transcode, close-delimited bodies, pause/resume, cancel (including
racing a pause), retry/backoff/exhaustion, progress resetting retries,
permanent HTTP errors, invalid responses, unreachable server, playback
suspension, deletion (including the active item and unknown files), storage
limits (up front, from the server's size, disk full mid-write, space draining
mid-transfer), crash/restart recovery, damaged state, and offline startup.
The suite runs clean under ASan and UBSan.
