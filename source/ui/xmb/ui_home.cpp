// Home shelf screen — a vertically-scrolling stack of horizontally-scrolling
// rows (Continue Watching, Next Up, Recently Added Movies/Shows/Music),
// mirroring the Jellyfin web home.  Each row carries its
// own card aspect ratio.  Card images / selection are drawn in the CPU phase
// (xmb_draw_card, shared with the grid); titles / chevrons in the text phase.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/thread.h>

#include "ui_internal.h"
#include "ui_render_internal.h"
#include "ui_card_gpu.h"
#include "ui_spine.h"       // frame clock + gliding focus ring
#include "jellyfin_api.h"
#include "music_screen.h"
#include "plog.h"
#include "timing.h"
#include "player.h"       // show_player_offline
#include "dl_manager.h"
#include "dl_library.h"

// -------------------------------------------------------
// Model
// -------------------------------------------------------

#define HOME_ROW_MAX 25

typedef enum { HROW_LANDSCAPE, HROW_PORTRAIT, HROW_SQUARE, HROW_STUB } HomeRowKind;

typedef struct {
    const char *title;
    HomeRowKind kind;
    XMBItem     items[HOME_ROW_MAX];
    int         count;
    bool        loaded;
} HomeRow;

// Favourites and Downloaded appear only when they have something to show.
enum { HR_CONTINUE, HR_NEXTUP, HR_FAVOURITES, HR_DOWNLOADED, HR_MOVIES, HR_SHOWS,
       HR_MUSIC, HOME_ROWS_N };

// Rows that go stale when something is played or starred: refetched.
static bool row_dynamic(int r)  { return r == HR_CONTINUE || r == HR_NEXTUP || r == HR_FAVOURITES; }
// Rows that are not shown while empty.
static bool row_optional(int r) { return r == HR_FAVOURITES || r == HR_DOWNLOADED; }
// Rows read from the offline library on this console, not asked of the server.
static bool row_local(int r)    { return r == HR_DOWNLOADED; }

static HomeRow s_rows[HOME_ROWS_N];

// What XMBItem has no room for, per row item, read from the same JSON
// (parse_xmb_items_each).  The spine's queue shows an episode by its series
// -- poster, name, "S1 E4" -- and a film's rating in the meta row.
typedef struct {
    char series_id[40];
    char series_name[64];
    char rating[12];      // OfficialRating, "PG-13"
    u8   season, episode;
} HomeExtra;
static HomeExtra s_extra[HOME_ROWS_N][HOME_ROW_MAX];

// g_play_gen when Continue Watching / Next Up were last fetched.
static unsigned s_seen_gen = 0;

static int     s_focus_row = 0;
static bool    s_inited    = false;

static void home_init_once(void) {
    if (s_inited) return;
    s_inited = true;
    memset(s_rows, 0, sizeof(s_rows));
    s_rows[HR_CONTINUE].title = TRN("Continue Watching");        s_rows[HR_CONTINUE].kind = HROW_LANDSCAPE;
    s_rows[HR_NEXTUP].title   = TRN("Next Up");                  s_rows[HR_NEXTUP].kind   = HROW_LANDSCAPE;
    s_rows[HR_FAVOURITES].title = TRN("Favourites");             s_rows[HR_FAVOURITES].kind = HROW_PORTRAIT;
    s_rows[HR_DOWNLOADED].title = TRN("Downloaded");             s_rows[HR_DOWNLOADED].kind = HROW_PORTRAIT;
    s_rows[HR_MOVIES].title   = TRN("Recently Added in Movies"); s_rows[HR_MOVIES].kind   = HROW_PORTRAIT;
    s_rows[HR_SHOWS].title    = TRN("Recently Added in Shows");  s_rows[HR_SHOWS].kind    = HROW_PORTRAIT;
    s_rows[HR_MUSIC].title    = TRN("Recently Added in Music"); s_rows[HR_MUSIC].kind    = HROW_SQUARE;
}

static void home_each_extra(int i, const char *o, int n, void *ctx) {
    HomeExtra *x = &((HomeExtra *)ctx)[i];
    xmb_json_str_range(o, n, "SeriesId",       x->series_id,   sizeof x->series_id);
    xmb_json_str_range(o, n, "SeriesName",     x->series_name, sizeof x->series_name);
    xmb_json_str_range(o, n, "OfficialRating", x->rating,      sizeof x->rating);
    decode_unicode_escapes(x->series_name);
    const int se = xmb_json_int_range(o, n, "ParentIndexNumber", 0);
    const int ep = xmb_json_int_range(o, n, "IndexNumber", 0);
    x->season  = (u8)(se > 0 && se < 256 ? se : 0);
    x->episode = (u8)(ep > 0 && ep < 256 ? ep : 0);
}

// Continue Watching and Next Up change with every playback: refetch them,
// and put their focus back on the front, where what was just watched lands.
static void home_stage_dynamic_reset(void);
static void home_mark_dynamic_stale(void) {
    s_rows[HR_CONTINUE].loaded = false;
    s_rows[HR_NEXTUP].loaded   = false;
    s_rows[HR_FAVOURITES].loaded = false;     // keeps its place: a star toggled
    s_seen_gen = g_play_gen;
    home_stage_dynamic_reset();
}

// The request for row r.  False when the row has nothing to ask for (a stub,
// or no library of that kind): it is then marked loaded and empty here.
static bool home_row_url(int r, char *url, size_t cap) {
    HomeRow *row = &s_rows[r];
    const char *fields = "Genres,RunTimeTicks,ProductionYear,Container,Tags";   // Tags: see is_gelato_stream()

    if (r == HR_CONTINUE) {
        snprintf(url, cap,
            "%s/Users/%s/Items/Resume?Limit=%d&Recursive=true&MediaTypes=Video&Fields=%s",
            g_server, g_userid, HOME_ROW_MAX, fields);
    } else if (r == HR_NEXTUP) {
        snprintf(url, cap,
            "%s/Shows/NextUp?userId=%s&Limit=%d&Fields=%s",
            g_server, g_userid, HOME_ROW_MAX, fields);
    } else if (r == HR_FAVOURITES) {
        snprintf(url, cap,
            "%s/Users/%s/Items?Filters=IsFavorite&Recursive=true"
            "&IncludeItemTypes=Movie,Series,Episode&SortBy=SortName&Limit=%d"
            "&Fields=%s,PrimaryImageAspectRatio",
            g_server, g_userid, HOME_ROW_MAX, fields);
    } else if (r == HR_DOWNLOADED) {
        return false;        // read from the offline library (home_step_local)
    } else if (r == HR_MUSIC) {
        // Home samples the FIRST music library; the others are still reachable
        // as their own tabs.
        int mt = xmb_tab_of_kind(TABKIND_MUSIC);
        if (mt < 0) { row->count = 0; row->loaded = true; return false; }
        const char *lib = g_tabs[mt].library_id;
        if (!lib[0]) { row->count = 0; row->loaded = true; return false; }
        snprintf(url, cap,
            "%s/Users/%s/Items?ParentId=%s&IncludeItemTypes=MusicAlbum"
            "&Recursive=true&SortBy=DateCreated&SortOrder=Descending"
            "&Limit=%d&Fields=%s",
            g_server, g_userid, lib, HOME_ROW_MAX, fields);
    } else if (r == HR_MOVIES || r == HR_SHOWS) {
        int tab = xmb_tab_of_kind((r == HR_MOVIES) ? TABKIND_MOVIES : TABKIND_TV);
        if (tab < 0) { row->count = 0; row->loaded = true; return false; }
        const char *lib  = g_tabs[tab].library_id;
        const char *type = (r == HR_MOVIES) ? "Movie" : "Series";
        if (!lib[0]) { row->count = 0; row->loaded = true; return false; }
        snprintf(url, cap,
            "%s/Users/%s/Items?ParentId=%s&IncludeItemTypes=%s&Recursive=true"
            "&SortBy=DateCreated&SortOrder=Descending&Limit=%d&Fields=%s",
            g_server, g_userid, lib, type, HOME_ROW_MAX, fields);
    } else {
        row->loaded = true;   // stub
        return false;
    }
    return true;
}

// Synchronous: the boot worker's prefetch (nothing else is drawing then).
static void home_fetch_row(int r) {
    HomeRow *row = &s_rows[r];
    char url[512];
    if (!home_row_url(r, url, sizeof url)) return;
    int status = http_request(0, url, NULL, g_token, responseBuffer, RESPONSE_SIZE);
    memset(s_extra[r], 0, sizeof s_extra[r]);
    row->count = (status == 200)
               ? parse_xmb_items_each(responseBuffer, row->items, HOME_ROW_MAX,
                                      home_each_extra, s_extra[r]) : 0;
    row->loaded = true;
}

// --- rows off the render thread -------------------------------------------
//
// Measured 2026-09-24: back from a film, Home's first frame took 556 ms, and
// 1,136 ms the second time (`xmb: frame=... other=1114320`).  That was
// home_step_load() -- a blocking HTTP request on the RENDER thread, one row a
// frame, for Continue Watching and Next Up (every playback makes them stale).
// Worse, http_request() holds one mutex for every caller, so that frame also
// waited behind whatever the thumbnail thread was fetching at the time.
//
// So the rows are fetched by a worker of their own, one at a time, into its
// own buffer, and the render thread only copies a finished row in (a few KB).
// Until the new row lands the old one stays on screen.  A fetch that started
// before a playback bumped g_play_gen is thrown away for the dynamic rows and
// asked for again, so a stale Continue Watching can never be published.
#define HOME_WORK_BUF (256 * 1024)     // a 25-item row is ~40-80 KB of JSON
static char            *s_hw_buf = NULL;
static XMBItem          s_hw_items[HOME_ROW_MAX];
static HomeExtra        s_hw_extra[HOME_ROW_MAX];
static char             s_hw_url[512];
static int              s_hw_count = 0;
static int              s_hw_status = 0;
static int              s_hw_row = -1;          // the row in flight
static unsigned         s_hw_gen = 0;           // g_play_gen when it was asked for
static volatile int     s_hw_state = 0;         // 0 idle, 1 queued, 2 done
static volatile bool    s_hw_run = false;
static int              s_hw_ok = -1;           // -1 not tried, 0 failed, 1 up
static sys_ppu_thread_t s_hw_tid;

static void home_worker_fn(void *arg) {
    (void)arg;
    while (s_hw_run) {
        if (s_hw_state != 1) { usleep(4000); continue; }
        __sync_synchronize();
        memset(s_hw_extra, 0, sizeof s_hw_extra);
        const int status = http_request(0, s_hw_url, NULL, g_token, s_hw_buf, HOME_WORK_BUF);
        s_hw_status = status;
        s_hw_count  = (status == 200)
                    ? parse_xmb_items_each(s_hw_buf, s_hw_items, HOME_ROW_MAX,
                                           home_each_extra, s_hw_extra) : 0;
        __sync_synchronize();
        s_hw_state = 2;
    }
    sysThreadExit(0);
}

static bool home_worker_up(void) {
    if (s_hw_ok >= 0) return s_hw_ok == 1;
    s_hw_ok = 0;
    s_hw_buf = (char *)malloc(HOME_WORK_BUF);
    if (!s_hw_buf) { plog("home: row worker buffer FAILED -- rows load inline"); return false; }
    s_hw_run = true;
    static char name[] = "jf_homerow";
    if (sysThreadCreate(&s_hw_tid, home_worker_fn, NULL, 1300, 64 * 1024,
                        THREAD_JOINABLE, name) != 0) {
        s_hw_run = false;
        free(s_hw_buf); s_hw_buf = NULL;
        plog("home: row worker FAILED -- rows load inline");
        return false;
    }
    s_hw_ok = 1;
    return true;
}

// Render thread: publish a finished row.
static void home_worker_collect(void) {
    if (s_hw_state != 2) return;
    __sync_synchronize();
    const int r = s_hw_row;
    const bool dynamic = r >= 0 && r < HOME_ROWS_N && row_dynamic(r);
    if (r >= 0 && r < HOME_ROWS_N && !(dynamic && s_hw_gen != g_play_gen)) {
        HomeRow *row = &s_rows[r];
        const int n = s_hw_count < 0 ? 0 : (s_hw_count > HOME_ROW_MAX ? HOME_ROW_MAX : s_hw_count);
        // A failed request keeps what the row had rather than emptying it.
        if (s_hw_status == 200 || row->count == 0) {
            memcpy(row->items, s_hw_items, (size_t)n * sizeof(XMBItem));
            memcpy(s_extra[r], s_hw_extra, sizeof s_extra[r]);
            row->count = n;
        }
        row->loaded = true;
    }
    s_hw_row = -1;
    __sync_synchronize();
    s_hw_state = 0;
}

// --- the Downloaded row: the offline library, read from this console's HDD ----------
//
// Not asked of the server, so it works with the server gone.  It is rebuilt
// when the set of finished downloads changes, a few entries a frame (each is
// a small file read), and published whole so the row never shows half a list.
// Its cards are the downloads' own posters under the "offp:" key; an item's
// id carries that prefix and X plays it offline.
static char      s_dl_ids[DL_MAX_ITEMS][DL_ID_MAX];
static int       s_dl_n = 0, s_dl_at = 0;
static bool      s_dl_building = false;
static XMBItem   s_dl_items[HOME_ROW_MAX];
static HomeExtra s_dl_extra[HOME_ROW_MAX];
static int       s_dl_count = 0;
static unsigned  s_dl_sig = 0, s_dl_pending_sig = 0;
static u64       s_dl_next_poll = 0;

static unsigned dl_signature(char (*ids)[DL_ID_MAX], int n) {
    unsigned h = 2166136261u;
    for (int i = 0; i < n; i++)
        for (const char *p = ids[i]; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    return h ^ (unsigned)n;
}

static void home_step_local(void) {
    HomeRow *row = &s_rows[HR_DOWNLOADED];
    if (!s_dl_building) {
        const u64 now = timing_get_us();
        if (now < s_dl_next_poll) return;
        s_dl_next_poll = now + 500000ULL;
        static char ids[DL_MAX_ITEMS][DL_ID_MAX];
        const int n = dl_library_ids(ids, DL_MAX_ITEMS);
        const unsigned sig = dl_signature(ids, n);
        if (row->loaded && sig == s_dl_sig) return;
        // Newest first: the library lists oldest first.
        s_dl_n = 0;
        for (int i = n - 1; i >= 0 && s_dl_n < HOME_ROW_MAX; i--)
            snprintf(s_dl_ids[s_dl_n++], DL_ID_MAX, "%s", ids[i]);
        s_dl_at = 0;
        s_dl_count = 0;
        s_dl_pending_sig = sig;
        memset(s_dl_extra, 0, sizeof s_dl_extra);
        s_dl_building = true;
    }
    for (int k = 0; k < 4 && s_dl_at < s_dl_n; k++, s_dl_at++) {
        DlLibraryEntry e;
        if (strlen(s_dl_ids[s_dl_at]) > 56 || !dl_library_get(s_dl_ids[s_dl_at], &e)) continue;
        XMBItem *it = &s_dl_items[s_dl_count];
        HomeExtra *x = &s_dl_extra[s_dl_count];
        memset(it, 0, sizeof *it);
        snprintf(it->id, sizeof it->id, "offp:%s", e.meta.id);
        snprintf(it->name, sizeof it->name, "%s", e.meta.title);
        snprintf(it->type, sizeof it->type, "%s", e.meta.type[0] ? e.meta.type : "Movie");
        if (e.meta.year > 0) snprintf(it->year_str, sizeof it->year_str, "%d", e.meta.year);
        if (e.meta.runtime_secs >= 60) {
            const unsigned m = e.meta.runtime_secs / 60;
            if (m >= 60) snprintf(it->duration_str, sizeof it->duration_str, TR("%uh %02um"), m / 60, m % 60);
            else         snprintf(it->duration_str, sizeof it->duration_str, TR("%u min"), m);
        }
        if (e.meta.series[0]) snprintf(x->series_name, sizeof x->series_name, "%s", e.meta.series);
        x->season  = (u8)(e.meta.season  > 0 && e.meta.season  < 256 ? e.meta.season  : 0);
        x->episode = (u8)(e.meta.episode > 0 && e.meta.episode < 256 ? e.meta.episode : 0);
        s_dl_count++;
    }
    if (s_dl_at >= s_dl_n) {
        memcpy(row->items, s_dl_items, (size_t)s_dl_count * sizeof(XMBItem));
        memcpy(s_extra[HR_DOWNLOADED], s_dl_extra, sizeof s_extra[HR_DOWNLOADED]);
        row->count  = s_dl_count;
        row->loaded = true;
        s_dl_sig = s_dl_pending_sig;
        s_dl_building = false;
    }
}

// X on a Downloaded card plays it from the HDD.  False for any other item.
static bool home_play_downloaded(const XMBItem *it) {
    if (strncmp(it->id, "offp:", 5) != 0) return false;
    static char id[DL_ID_MAX];
    snprintf(id, sizeof id, "%s", it->id + 5);
    show_player_offline(id, 0);
    init_btns();
    return true;
}

// Once per frame (render thread): publish what has landed, then ask for the
// next unloaded row.  Never blocks.
static void home_step_load(void) {
    home_step_local();
    if (!home_worker_up()) {
        // No worker: the old way, one blocking row a frame.
        for (int r = 0; r < HOME_ROWS_N; r++) {
            if (s_rows[r].kind == HROW_STUB || row_local(r)) continue;
            if (!s_rows[r].loaded) { home_fetch_row(r); return; }
        }
        return;
    }
    home_worker_collect();
    if (s_hw_state != 0) return;              // one in flight
    for (int r = 0; r < HOME_ROWS_N; r++) {
        if (s_rows[r].kind == HROW_STUB || s_rows[r].loaded || row_local(r)) continue;
        if (!home_row_url(r, s_hw_url, sizeof s_hw_url)) continue;
        s_hw_row = r;
        s_hw_gen = g_play_gen;
        __sync_synchronize();
        s_hw_state = 1;
        return;
    }
}

// Every row now, not one a frame -- for the boot worker (xmb_prepare).  The
// per-frame loader below then finds nothing left to do.
void xmb_home_prefetch(void) {
    home_init_once();
    for (int r = 0; r < HOME_ROWS_N; r++)
        if (s_rows[r].kind != HROW_STUB && !row_local(r) && !s_rows[r].loaded)
            home_fetch_row(r);
}

void xmb_home_on_enter(void) {
    home_init_once();
    // The XMB remembers where it was in a category.  Coming back to Home
    // keeps the category and each row's position, and refetches the dynamic
    // rows only when something has played since -- walking the spine past
    // Home used to cost two blocking fetches every time.
    if (g_play_gen != s_seen_gen) home_mark_dynamic_stale();
}

#include "ui_home_stage.inc"

static void home_stage_dynamic_reset(void) {
    s_row_col[HR_CONTINUE] = 0;
    s_row_col[HR_NEXTUP]   = 0;
    s_q_fresh = true;               // snap: nothing should slide back to 0
}

void xmb_queue_src(int *w, int *h) {
    // The focused poster's own size, capped at 120,000 px (~15 MB for the
    // cache's 32 slots): at 720p that is the full 200x300, at 1080p it
    // fetches 283x424 for a 300x450 box -- a 6% stretch nobody will see.
    float fw = (float)UIS_W(200), fh = (float)UIS_H(300);
    const float cap = 120000.0f;
    if (fw * fh > cap) {
        const float k = sqrtf(cap / (fw * fh));
        fw *= k; fh *= k;
    }
    *w = (int)fw;
    *h = (int)fh;
}

bool xmb_handle_input_home(void) {
    home_init_once();
    return home_queue_input();
}
