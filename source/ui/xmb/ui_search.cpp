// XMB search tab — system keyboard input and Jellyfin search.

#include <stdio.h>
#include <string.h>

#include <unistd.h>      // usleep -- the retry pause below
#include <sys/thread.h>
#include <sys/memory.h>
#include <sysutil/sysutil.h>
#include <sysutil/osk.h>

#include "ui_internal.h"
#include "jellyfin_api.h"
#include "player.h"
#include "plog.h"
#include "timing.h"

// The term is typed on the PS3's own on-screen keyboard (cellOskDialog), not
// a keyboard drawn by the app.  X on the search field opens it pre-filled with
// the current term; confirming it sends exactly one query.  The dialog lives
// on the system layer and reports back through a sysutil callback, which the
// XMB loop's per-frame sysUtilCheckCallback() delivers, so the menu keeps
// drawing underneath while it is up.
//
// This server takes 2.5-8.4 s to emit the first byte of a search (see
// HANDOFF.md), and http.cpp allows up to 24 s for it -- as a blocking call on
// the render thread that is a 24-second dead console.  The request therefore
// runs on its own thread, like the thumbnail fetch and the update check, and
// the UI keeps running at 60 Hz with a "Searching..." line while it works.
//
// Two rules make the threading safe, and both matter:
//
//   1. The worker does NOT touch g_search_results.  It fills a private array,
//      and the render thread copies it across when it collects the result, so
//      the array the draw code walks is only ever written by the thread that
//      draws it.
//   2. The worker does NOT use the shared `responseBuffer`.  http_request()
//      holds a mutex while it fills the caller's buffer, but the CALLER parses
//      it after unlocking — so two callers sharing that global would happily
//      parse each other's response.  Search gets its own.
bool g_search_focus_results = false;
int  g_search_sel           = 0;
int  g_search_scroll        = 0;
char g_search_buf[64];
int  g_search_results_count = 0;
XMBItem g_search_results[XMB_ITEMS_MAX];

// --- the system keyboard --------------------------------------------------
//
// Slot 0 belongs to main.cpp's exit/overlay callback; every registered slot
// sees every sysutil event, so the two coexist.
#define SEARCH_OSK_SLOT  1
// UTF-16 units the dialog accepts.  A BMP character is at most 3 UTF-8 bytes
// and g_search_buf holds 63, so 21 always fits; the conversion below stops at
// a whole character anyway, so a longer limit would only ever truncate.
#define SEARCH_OSK_CHARS 40

static bool s_osk_active = false;          // LoadAsync .. UNLOADED
static bool s_osk_have   = false;          // a confirmed term waits in s_osk_text
static char s_osk_text[sizeof(g_search_buf)];
static u16  s_osk_msg[64];
static u16  s_osk_init[SEARCH_OSK_CHARS + 1];
static u16  s_osk_out[SEARCH_OSK_CHARS + 1];

bool xmb_search_osk_active(void) { return s_osk_active; }

// UTF-8 -> UTF-16 (BMP only; anything wider becomes '?').  Always terminates.
static void utf8_to_u16(const char *s, u16 *out, int cap) {
    int n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && n < cap - 1) {
        u32 c = *p++;
        if      (c >= 0xF0) { c = '?'; while ((*p & 0xC0) == 0x80) p++; }
        else if (c >= 0xE0 && p[0] && p[1]) { c = ((c & 0x0F) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F); p += 2; }
        else if (c >= 0xC0 && p[0]) { c = ((c & 0x1F) << 6) | (p[0] & 0x3F); p += 1; }
        else if (c >= 0x80) c = '?';
        out[n++] = (u16)c;
    }
    out[n] = 0;
}

// UTF-16 -> UTF-8, stopping before a character that would not fit whole.
// Surrogate pairs (emoji) are dropped -- nothing in a library title needs them
// and the fonts could not draw them.
static void u16_to_utf8(const u16 *s, char *out, int cap) {
    int n = 0;
    for (; *s; s++) {
        u32 c = *s;
        if (c >= 0xD800 && c <= 0xDFFF) continue;
        int need = c < 0x80 ? 1 : c < 0x800 ? 2 : 3;
        if (n + need > cap - 1) break;
        if (need == 1) out[n++] = (char)c;
        else if (need == 2) { out[n++] = (char)(0xC0 | (c >> 6)); out[n++] = (char)(0x80 | (c & 0x3F)); }
        else { out[n++] = (char)(0xE0 | (c >> 12)); out[n++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[n++] = (char)(0x80 | (c & 0x3F)); }
    }
    out[n] = '\0';
}

// Runs on the render thread, inside the XMB loop's sysUtilCheckCallback().
static void search_osk_cb(u64 status, u64 param, void *usrdata) {
    (void)param; (void)usrdata;
    if (status == SYSUTIL_OSK_DONE) {
        // Unloading is what takes the keyboard off screen and hands over the
        // text.  Cancel (O) and an empty field leave the term alone.
        oskCallbackReturnParam ret;
        memset(&ret, 0, sizeof(ret));
        s_osk_out[0] = 0;
        ret.res = OSK_OK;
        ret.len = SEARCH_OSK_CHARS;
        ret.str = s_osk_out;
        oskUnloadAsync(&ret);
        if (ret.res == OSK_OK) {
            u16_to_utf8(s_osk_out, s_osk_text, sizeof(s_osk_text));
            s_osk_have = true;
        } else if (ret.res == OSK_NO_TEXT) {   // confirmed an empty field: clear
            s_osk_text[0] = '\0';
            s_osk_have = true;
        }
        char b[96];
        snprintf(b, sizeof(b), "search: keyboard closed res=%d '%s'",
                 (int)ret.res, ret.res == OSK_OK ? s_osk_text : "");
        plog(b);
    } else if (status == SYSUTIL_OSK_UNLOADED) {
        sysUtilUnregisterCallback(SEARCH_OSK_SLOT);
        s_osk_active = false;
    }
}

static void search_osk_open(void) {
    if (s_osk_active) return;

    utf8_to_u16(TR("Search your library"), s_osk_msg,
                (int)(sizeof(s_osk_msg) / sizeof(s_osk_msg[0])));
    utf8_to_u16(g_search_buf, s_osk_init, SEARCH_OSK_CHARS + 1);

    oskInputFieldInfo info;
    info.message   = s_osk_msg;
    info.startText = s_osk_init;
    info.maxLength = SEARCH_OSK_CHARS;

    oskParam p;
    p.allowedPanels  = OSK_PANEL_TYPE_ALPHABET | OSK_PANEL_TYPE_NUMERAL |
                       OSK_PANEL_TYPE_ENGLISH;
    p.firstViewPanel = OSK_PANEL_TYPE_ALPHABET;
    p.controlPoint.x = 0.0f;
    p.controlPoint.y = 0.0f;
    p.prohibitFlags  = OSK_PROHIBIT_RETURN;   // one line: Enter confirms

    if (sysUtilRegisterCallback(SEARCH_OSK_SLOT, search_osk_cb, NULL) != 0) {
        plog("search: keyboard callback slot busy");
        return;
    }
    oskAddSupportLanguage(p.allowedPanels);
    oskSetKeyLayoutOption(OSK_10KEY_PANEL | OSK_FULLKEY_PANEL);

    s32 rc = oskLoadAsync((sys_mem_container_t)0xFFFFFFFFu, &p, &info);
    if (rc != 0) {
        char b[64];
        snprintf(b, sizeof(b), "search: oskLoadAsync FAILED 0x%08x", (u32)rc);
        plog(b);
        sysUtilUnregisterCallback(SEARCH_OSK_SLOT);
        return;
    }
    s_osk_active = true;
    s_osk_have   = false;
}

// --- the search worker ----------------------------------------------------
//
// Private response buffer: see rule 2 in the note at the top of this file.
// Sized to match RESPONSE_SIZE because search is the LARGEST response the app
// asks for -- a broad term like "the" measured 255 KB against 63 KB for a
// whole library tab -- so it is the one request that must not be the one that
// gets truncated.
static char     s_search_resp[RESPONSE_SIZE];

static sys_ppu_thread_t s_search_thr     = 0;
static bool             s_search_running = false;   // render thread only
static bool             s_search_pending = false;   // a term waits for the worker
static volatile bool    s_search_done    = false;   // worker -> render thread
static char             s_search_term[sizeof(g_search_buf)];  // term in flight
static XMBItem          s_search_out[XMB_ITEMS_MAX];
static int              s_search_out_n   = 0;
static int              s_search_status  = 0;

bool xmb_search_in_flight(void) { return s_search_running; }

static void search_thread_fn(void *arg) {
    (void)arg;

    char encoded[192];
    url_encode_query(s_search_term, encoded, sizeof(encoded));

    char url[512];
    snprintf(url, sizeof(url),
        "%s/Users/%s/Items?searchTerm=%s&Recursive=true"
        "&IncludeItemTypes=Movie,Series,Episode&Limit=%d"
        "&SortBy=SortName&SortOrder=Ascending"
        "&Fields=Genres,RunTimeTicks,ProductionYear,Container,Tags",
        g_server, g_userid, encoded, XMB_ITEMS_MAX);

    char dbg[512];
    snprintf(dbg, sizeof(dbg), "search url: %s", url);
    plog(dbg);

    // Retry a "no matches" answer, because on this server it is often a lie.
    //
    // THE PROBLEM IS SERVER-SIDE, and that is measured, not assumed: the exact
    // same URL, issued from a PC, ALSO comes back as a well-formed
    // `{"Items":[],"TotalRecordCount":0,"StartIndex":0}` -- 48 bytes, HTTP 200,
    // no timeout, no parse failure -- for some terms.  What the failures have
    // in common is how long they take:
    //
    //     DU  10.36s ->    48 B  count=0        DI   7.66s -> 112 KB count=76
    //     TI  10.38s ->    48 B  count=0        DIE 12.39s -> 114 KB count=60
    //     BAT 10.2 s ->    48 B  count=0        BA   3.0 s ->  79 KB count=40
    //
    // Everything that comes back empty sits at ~10.3 s, which looks like an
    // internal deadline in Jellyfin or in the debrid/AIOStreams backend behind
    // this library: when the query does not finish in time the server yields
    // zero rows instead of an error.  Nothing on this side can prevent that.
    //
    // What CAN be done is ask again, because the work the first attempt did is
    // cached: a repeat of the same term lands in 1.6-4 s and returns the full
    // set.  Measured on the console, 'BAT' did exactly this -- try 1 empty at
    // 10.2 s, try 2 with 41 results at 9.0 s.
    //
    // Three attempts, with the pause GROWING between them.  Two was not enough
    // for 'DI' (both attempts came back empty at ~10 s), and the reason to wait
    // longer rather than merely more often is that the backend appears to keep
    // working after it has answered -- the same term from a PC went 7.7 s cold
    // then 2.9 s warm.  Giving it a couple of seconds is what turns the next
    // attempt into the warm one.
    //
    // The cost is bounded and it is not a freeze: this is a worker thread, the
    // UI stays at 60 Hz with "Searching..." up, and a term that genuinely
    // matches nothing settles after three tries.
    static const u32 RETRY_PAUSE_US[] = { 500000, 2500000 };
    u64 t0 = timing_get_us();
    int status = 0, n = 0;

    for (int attempt = 0; attempt < 3; attempt++) {
        u64 ta = timing_get_us();
        status = http_request(0, url, NULL, g_token,
                              s_search_resp, sizeof(s_search_resp));
        n = (status == 200)
            ? parse_xmb_items(s_search_resp, s_search_out, XMB_ITEMS_MAX)
            : 0;

        snprintf(dbg, sizeof(dbg),
                 "search: term='%s' try=%d status=%d count=%d bytes=%d in %llums",
                 s_search_term, attempt + 1, status, n,
                 (int)strlen(s_search_resp),
                 (unsigned long long)((timing_get_us() - ta) / 1000));
        plog(dbg);

        if (status != 200) {
            char errbuf[320];
            snprintf(errbuf, sizeof(errbuf), "search_err: %.300s", s_search_resp);
            plog(errbuf);
            break;                       // a transport failure will not improve
        }
        if (n > 0) break;                // got what we asked for

        char respbuf[224];
        snprintf(respbuf, sizeof(respbuf),
                 "search_empty_resp: %.200s", s_search_resp);
        plog(respbuf);

        // Only worth retrying the case this exists for: the server said 200
        // and handed back an empty set.  Give it time to finish whatever it
        // was still doing when it answered, longer on each pass.
        if (attempt < (int)(sizeof(RETRY_PAUSE_US) / sizeof(RETRY_PAUSE_US[0])))
            usleep(RETRY_PAUSE_US[attempt]);
    }

    s_search_status = status;
    s_search_out_n  = n;

    snprintf(dbg, sizeof(dbg),
             "search: term='%s' FINAL status=%d count=%d in %llums",
             s_search_term, status, n,
             (unsigned long long)((timing_get_us() - t0) / 1000));
    plog(dbg);

    __sync_synchronize();   // results land before the done flag is visible
    s_search_done = true;
    sysThreadExit(0);
}

// Start a search for whatever is in g_search_buf.  No-op while one is already
// in flight -- the caller re-arms its debounce and tries again, so the term
// that actually goes out is the latest one the user settled on.
static void xmb_do_search(void) {
    if (s_search_running || !g_search_buf[0]) return;

    snprintf(s_search_term, sizeof(s_search_term), "%s", g_search_buf);
    s_search_done = false;

    s32 rc = sysThreadCreate(&s_search_thr, search_thread_fn, NULL,
                             1500, 65536, 0, "jf_search");
    if (rc != 0) {
        // Falling back to a blocking call here would reintroduce the freeze
        // this exists to remove, so don't: leave the previous results up and
        // say so.  Thread creation failing at all means the app is in trouble
        // for bigger reasons than search.
        char b[64];
        snprintf(b, sizeof(b), "search: sysThreadCreate FAILED 0x%08x", (u32)rc);
        plog(b);
        return;
    }
    s_search_running = true;
}

// Collect a finished search.  Called every frame from the input handler; does
// nothing until the worker signals.
static void xmb_search_collect(void) {
    if (!s_search_running || !s_search_done) return;

    u64 tret;
    sysThreadJoin(s_search_thr, &tret);
    s_search_thr     = 0;
    s_search_running = false;
    s_search_done    = false;

    // Publish only if the user has not moved on.  If they entered a new term
    // while this was in flight it no longer matches, and the pending query
    // goes out next frame -- showing the stale hits meanwhile would be worse
    // than showing nothing.
    if (strcmp(s_search_term, g_search_buf) != 0) return;

    if (s_search_status == 200) {
        for (int i = 0; i < s_search_out_n; i++)
            g_search_results[i] = s_search_out[i];
        g_search_results_count = s_search_out_n;
    } else {
        g_search_results_count = 0;
    }

    // Every answer is to a freshly entered term: land on its first hit, so X
    // opens it straight away.
    g_search_sel    = 0;
    g_search_scroll = 0;
    g_search_focus_results = g_search_results_count > 0;
}

// Join the worker before the XMB tears down.  Safe to call unconditionally.
void xmb_search_shutdown(void) {
    if (s_osk_active) oskAbort();   // the system unloads it; the slot goes with the app
    if (!s_search_running) return;
    u64 tret;
    sysThreadJoin(s_search_thr, &tret);
    s_search_thr     = 0;
    s_search_running = false;
    s_search_done    = false;
}

// X from a search hit's quick-peek: the same as Triangle did before it.
static void search_peek_open(void) {
    if (g_search_sel < 0 || g_search_sel >= g_search_results_count) return;
    const XMBItem *it = &g_search_results[g_search_sel];
    if (strcmp(it->type, "Series") == 0) {
        if (xmb_open_series(it)) {
            g_search_focus_results = false;
            init_btns();
        }
    } else {
        xmb_show_item_info(it);
    }
}

// Open the system keyboard on the current term.  Also the Search tab's entry
// from the tab bar (ui_spine.cpp), so X or Down there goes straight to typing.
void xmb_search_open_keyboard(void) {
    g_search_focus_results = false;
    search_osk_open();
}

// The search field and the results under it.  On the field, X opens the
// keyboard, and so does Down while there is nothing below to move into.
bool xmb_handle_input_search(void) {
    // Pick up a finished query first, so a result that landed since the last
    // frame is on screen before this frame's input is considered.
    xmb_search_collect();

    // While the keyboard is up it owns the pad (ui_xmb.cpp skips input).
    if (s_osk_active) return false;

    // A term confirmed on the keyboard: search for it at once.  The same term
    // again only re-runs if it found nothing, which is the retry this server
    // sometimes needs (see search_thread_fn).
    if (s_osk_have) {
        s_osk_have = false;
        init_btns();   // the press that closed the dialog is not ours
        if (strcmp(s_osk_text, g_search_buf) != 0 || g_search_results_count == 0) {
            snprintf(g_search_buf, sizeof(g_search_buf), "%s", s_osk_text);
            g_search_results_count = 0;
            g_search_focus_results = false;
            g_search_sel = g_search_scroll = 0;
            s_search_pending = g_search_buf[0] != '\0';
        }
        return false;
    }
    // Waits here while an older query is still on the worker.
    if (s_search_pending && !s_search_running) {
        s_search_pending = false;
        xmb_do_search();
    }

    if (BTN_PRESSED(l1)) { g_search_focus_results = false; xmb_switch_tab(xmb_next_enabled(g_active_tab, -1)); return false; }
    if (BTN_PRESSED(r1)) { g_search_focus_results = false; xmb_switch_tab(xmb_next_enabled(g_active_tab, +1)); return false; }
    if (BTN_PRESSED(circle) && !g_search_buf[0]) {
        xmb_switch_tab(xmb_next_enabled(g_active_tab, +1));
        return false;
    }
    if (BTN_PRESSED(circle)) {
        g_search_buf[0] = '\0';
        g_search_results_count = 0;
        g_search_focus_results = false;
        s_search_pending = false;
        return false;
    }

    if (!g_search_focus_results) {
        if (BTN_PRESSED(cross) ||
            (BTN_PRESSED(down) && g_search_results_count == 0)) {
            search_osk_open();
            return false;
        }
        if (BTN_REPEAT(down) && g_search_results_count > 0) {
            g_search_focus_results = true;
            g_search_sel    = 0;
            g_search_scroll = 0;
        }
        return false;
    }

    if (BTN_REPEAT(up)) {
        if (g_search_sel == 0) {
            g_search_focus_results = false;
        } else {
            g_search_sel--;
            if (g_search_sel < g_search_scroll) g_search_scroll = g_search_sel;
        }
    }
    if (BTN_REPEAT(down)) {
        if (g_search_sel < g_search_results_count - 1) {
            g_search_sel++;
            // Scroll against the rows that ACTUALLY fit (the renderer's count).
            int vis = xmb_search_vis_rows();
            if (vis < 1) vis = 1;
            if (g_search_sel >= g_search_scroll + vis)
                g_search_scroll = g_search_sel - vis + 1;
        }
    }
    // Triangle opens a search hit's details, as in the library grid.
    if (BTN_PRESSED(triangle) && g_search_sel < g_search_results_count &&
        timing_get_us() >= g_info_cooldown_until) {
        const XMBItem *it = &g_search_results[g_search_sel];
        if (g_spine_on) {
            peek_open_item(it, XMB_THUMB_W, XMB_THUMB_H, search_peek_open);
            return false;
        }
        if (strcmp(it->type, "Series") == 0) {
            // Same rule as everywhere else: browse a show rather than
            // offer it a version overlay it has no versions for.
            if (xmb_open_series(it)) {
                g_search_focus_results = false;
                init_btns();
            }
        } else {
            xmb_show_item_info(it);
        }
        return false;
    }
    if (BTN_PRESSED(cross) && g_search_sel < g_search_results_count) {
        const XMBItem *it = &g_search_results[g_search_sel];
        // A SERIES is a folder, not something to play: open the Seasons ->
        // Episodes browser the TV tab and the Home rows use.
        if (strcmp(it->type, "Series") == 0) {
            if (xmb_open_series(it)) {
                g_search_focus_results = false;
                init_btns();
            }
            return false;
        }
        // Films and episodes open their details page (Version, Quality,
        // Resume live there).  Songs still play at once.
        if (strcmp(it->type, "Audio") == 0) {
            xmb_play_item(it, 0);
        } else if (timing_get_us() >= g_info_cooldown_until) {
            xmb_show_item_info(it);
            init_btns();
        }
        return false;
    }

    return false;
}
