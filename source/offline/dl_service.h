#pragma once
#include <stdint.h>
#include "jellyfin_api.h"   // JFItem, JFMediaSource, XMBItemDetail

// -------------------------------------------------------------------------
//  Offline downloads -- the service lifecycle
// -------------------------------------------------------------------------
//  Two layers:
//
//  dl_svc_*  (dl_service.cpp, portable, host-tested)
//      start once, restore on the worker thread, gate on the session, stop.
//
//  dl_service_* / dl_download_item  (dl_ps3.cpp, console glue)
//      the same, fed from the app's globals (roots, g_token, device id,
//      live settings).  These are what main.cpp and the UI call.
//
//  Lifecycle, as wired in main.cpp:
//
//    load_config()          -> dl_service_start()
//                              worker restores every item from the HDD --
//                              no network, never on the UI thread; completed
//                              items are usable whatever the server's state.
//                              Nothing transfers yet: there is no session.
//    login succeeded        -> dl_service_refresh_auth()   (queue runs)
//    logout / token revoked -> dl_service_refresh_auth()   (queue holds)
//    app exit               -> dl_service_stop()           (active item back
//                              to QUEUED with its data; resumes next launch)
//
//  Playback gating is the player's (dl_playback_begin/end, dl_manager.h) and
//  is independent of all of this.
// -------------------------------------------------------------------------

// ---- portable core ----------------------------------------------------------

// Starts the worker (dl_plat_thread_start) at most once.  roots are tried in
// order on the worker thread; the first that proves writable is used.  The
// strings must outlive the service.  False only if the thread cannot start.
bool dl_svc_start(const char *const *roots, int n_roots);
// One worker iteration: restore on the first call, then at most one transfer
// attempt.  Returns how long to sleep before the next (0 = go again).
uint32_t dl_svc_tick(void);
void dl_svc_stop(void);
bool dl_svc_started(void);
// "" before restore has succeeded.
const char *dl_svc_root(void);
// Session for transfers.  An empty token (logged out) holds the queue.
void dl_svc_set_session(const char *token, const char *device_id);
// Builds the header dl_svc_set_session() installs -- the same MediaBrowser
// identity http.cpp and stream.cpp send.  Returns false (out = "") for an
// empty token.
bool dl_svc_auth_header(char *out, int cap, const char *token,
                        const char *device_id);

// ---- console glue (dl_ps3.cpp) -----------------------------------------------

bool dl_service_start(void);
void dl_service_stop(void);
void dl_service_refresh_auth(void);     // from g_token + jf_device_id()
const char *dl_service_root(void);

// DOWNLOAD FOR OFFLINE: the item page's action.  source is the version the
// page has selected (its versions list), detail may be NULL.  Only disk
// writes, no network: returns at once, the worker does the rest.
int dl_download_item(const JFItem *item, const XMBItemDetail *detail,
                     const JFMediaSource *source);   // DlResult
