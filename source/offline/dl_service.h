#pragma once

// -------------------------------------------------------------------------
//  Offline downloads -- console service (dl_ps3.cpp)
// -------------------------------------------------------------------------
//  Owns the pieces of the download system that only exist on the console:
//  where the store lives on the HDD, the worker thread that runs transfers,
//  and the auth header built from the signed-in session.  Everything else --
//  the queue, the state machine, the transfer -- is dl_manager.h, which the
//  UI calls directly.
//
//  Stage 2 (this build): the service is complete but NOT started anywhere
//  yet.  Stage 3 starts it from main.cpp once login succeeds and suspends it
//  around playback (show_player), alongside the item-page request builder.
// -------------------------------------------------------------------------

// Picks the store root, restores every item from the HDD (no network
// needed), and starts the worker.  Safe to call more than once.  False if no
// writable root could be found -- downloads are then unavailable, and every
// dl_* call reports DL_E_NOT_READY rather than touching the disk.
bool dl_service_start(void);

// Stops the worker: an active transfer ends at its next read (at most about
// a second) and goes back to QUEUED with its data, to resume next launch.
void dl_service_stop(void);

// Rebuild the auth header from the current session (g_token + device id).
// Call after login and after a token change; logout passes an empty token.
void dl_service_refresh_auth(void);

// The root in use ("" before a successful start).
const char *dl_service_root(void);
