// The SoundCloud tab: music from SoundCloud with no Jellyfin server and no developer account.
//
//   * The tab is a short list: Likes, Playlists, Reposts, Tracks, Search, the profile it reads from, and the audio
//     quality.  There is no sign-in on the console (SoundCloud's login is a browser flow), so the user types the
//     name of a PUBLIC profile with the on-screen keyboard and gets that profile's public Likes, Playlists, Reposts
//     and uploads.  Private likes and the personal feed are out of scope.
//   * A list opens a screen of its own, like the Media tab's folder browser: tracks with their artwork, the rest of
//     the pages loading as the cursor nears the end.  X plays from the chosen track through the ordinary Now Playing
//     screen (music_screen_open_service), so the visualisers, skip, seek and pause all work.
//   * Tracks whose only streams are copy-protected (the encrypted HLS variants) or that are previews are not shown
//     and never fetched; the footer says how many were left out.
//
// The protocol is portable and host-tested (svc/sc_*.c); the network calls here run on the loading screen's worker
// thread, never the render thread.  The SoundCloud service is unofficial and may stop working whenever SoundCloud
// changes its web player: the errors say so rather than failing silently.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sysutil/sysutil.h>

#include "ui_internal.h"
#include "ui_spine.h"          // spine_frame_id
#include "ui_wave.h"
#include "ui_buffering.h"      // loading_run
#include "ui_sfx.h"
#include "rsxutil.h"
#include "plog.h"
#include "thumbnail_cache.h"
#include "local_names.h"       // local_format_duration
#include "music_screen.h"

#include "sc_api.h"
#include "sc_net.h"
#include "sc_svc.h"
#include "sx_plat.h"

namespace {

const char *MIDDOT = "\xC2\xB7";

// ---------------------------------------------------------------------------
//  Geometry and drawing: the same look as the Media tab's screens
// ---------------------------------------------------------------------------

int list_x(void)      { return XMB_ITEM_PAD; }
int list_w(void)      { return (int)display_width - 2 * XMB_ITEM_PAD; }
int row_h(void)       { return UIS_H(62); }
int row_pitch(void)   { return row_h() + UIS_H(8); }
int tab_top(void)     { return XMB_CONTENT_Y + UIS_H(10); }
int screen_top(void)  { return XMB_OY + UIS_H(112); }
int bottom(void)      { return (int)display_height - XMB_BOTTOM_PAD; }
int rows_in(int top)  {
    const int n = (bottom() - top + UIS_H(8)) / row_pitch();
    return n < 1 ? 1 : n;
}

void clip_text(int x, int y, const char *text, float px, u32 colour, int max_w, bool bold) {
    char buf[240];
    snprintf(buf, sizeof buf, "%s", text);
    int len = (int)strlen(buf);
    if (ttf_text_width(buf, px, bold) > max_w) {
        while (len > 3 && ttf_text_width(buf, px, bold) > max_w) {
            len--;
            while (len > 3 && ((unsigned char)buf[len] & 0xC0) == 0x80) len--;     // not inside a character
            buf[len] = '\0';
        }
        if (len > 3) { buf[len - 1] = '.'; buf[len - 2] = '.'; buf[len - 3] = '.'; }
    }
    drawTTF((u32)x, (u32)y, buf, px, colour, bold);
}

void frame_begin(void) {
    clearScreen(XMB_BG);
    wave_draw();
    rsxSync();
}

void draw_row_frame(int x, int y, int w, int h, bool sel) {
    drawRect((u32)x, (u32)y, (u32)w, (u32)h, sel ? XMB_PANEL_HI : XMB_PANEL);
    if (sel) drawRect((u32)(x - UIS_W(4)), (u32)y, UIS_W(3), (u32)h, XMB_ACCENT);
}

void draw_screen_title(const char *title, const char *line) {
    const int x = XMB_ITEM_PAD, y = XMB_OY + UIS_H(28);
    clip_text(x, y, title, UIS_TF(28), XMB_TEXT, list_w() - UIS_W(160), true);
    if (line && line[0]) clip_text(x, y + UIS_H(42), line, UIS_TF(15), XMB_TEXT_DIM, list_w() - UIS_W(160), false);
}

void centre_message(const char *m, int y) {
    const int cx = (int)display_width / 2;
    drawTTF((u32)(cx - ttf_text_width(m, UIS_TF(18)) / 2), (u32)y, m, UIS_TF(18), XMB_TEXT_DIM);
}

// A screen waits for the buttons that opened it to be let go before it takes any press.
struct Arm {
    bool armed = false;
    bool ready(void) {
        if (!armed && !btn_cur.cross && !btn_cur.circle && !btn_cur.square && !btn_cur.triangle) armed = true;
        return armed;
    }
};

void screen_begin(void) { rsxSync(); flip(); init_btns(); }

// ---------------------------------------------------------------------------
//  What the network said, in words
// ---------------------------------------------------------------------------

void error_text(const sc_net &n, int rc, char *out, int cap) {
    if (rc == SC_NET_E_CID)
        snprintf(out, (size_t)cap, "%s", TR("Could not connect to SoundCloud. Its web player may have changed; this tab may need an update."));
    else if (rc == SC_NET_E_HTTP && n.last_status == 404)
        snprintf(out, (size_t)cap, "%s", TR("SoundCloud has nothing at that address."));
    else if (rc == SC_NET_E_HTTP && (n.last_status == 429 || n.last_status == 503))
        snprintf(out, (size_t)cap, "%s", TR("SoundCloud is busy. Wait a moment and try again."));
    else if (rc == SC_NET_E_HTTP)
        snprintf(out, (size_t)cap, TR("SoundCloud answered with an error (%d)."), n.last_status);
    else if (n.last_err == SX_E_CERT)
        snprintf(out, (size_t)cap, "%s", TR("A secure connection was refused. Check the console's date and time."));
    else
        snprintf(out, (size_t)cap, "%s", TR("Could not reach SoundCloud. Check the console's network connection."));
}

// ---------------------------------------------------------------------------
//  The profile the tab reads from
// ---------------------------------------------------------------------------

int64_t s_uid = 0;                     // the profile's SoundCloud id, once resolved
char    s_uid_for[96];                 // the profile name it was resolved for

struct ProfileJob {
    sc_net  net;
    char    name[96];
    char    url[SC_URL_MAX];
    int64_t uid;
    char    permalink[96];
    char    display[80];
    int     rc;
};

void profile_work(void *arg) {
    ProfileJob *j = (ProfileJob *)arg;
    sc_svc_net_begin(&j->net);
    char purl[200];
    char *body = NULL;
    size_t len = 0;
    j->uid = 0;
    if (!sc_profile_url(j->name, purl, sizeof purl) || !sc_url_resolve(purl, SC_CID_PLACEHOLDER, j->url, sizeof j->url)) {
        j->rc = SC_NET_E_ARG;
    } else {
        j->rc = sc_net_api(&j->net, j->url, 256 * 1024, &body, &len);
        sc_user u;
        if (j->rc == SC_NET_OK) {
            if (sc_parse_user(body, len, &u)) {
                j->uid = u.id;
                snprintf(j->permalink, sizeof j->permalink, "%s", u.permalink);
                snprintf(j->display, sizeof j->display, "%s", u.name);
            } else {
                j->rc = SC_NET_E_HTTP;                         // a track or playlist link, not a profile
                j->net.last_status = 404;
            }
        }
        free(body);
    }
    memset(j->url, 0, sizeof j->url);
    sc_svc_net_end(&j->net);
}

// Asks for the profile name and checks that it exists. True when it was set.
bool ask_profile(void) {
    char text[96];
    snprintf(text, sizeof text, "%s", sc_svc_profile());
    const int r = get_input(text, sizeof text, TR("SoundCloud profile name (like forss) or its link"), false, true);
    init_btns();
    if (r != 1 || !text[0]) return false;
    static ProfileJob job;
    memset(&job, 0, sizeof job);
    snprintf(job.name, sizeof job.name, "%s", text);
    char purl[200];
    if (!sc_profile_url(text, purl, sizeof purl)) {
        xmb_dl_notice(TR("Not a SoundCloud profile"), TR("Type the name from the profile's address, like forss."));
        return false;
    }
    loading_run(profile_work, &job, TR("Loading"), false);
    screen_begin();
    if (job.rc != SC_NET_OK || job.uid <= 0) {
        char why[200];
        if (job.rc == SC_NET_E_HTTP && job.net.last_status == 404) snprintf(why, sizeof why, "%s", TR("SoundCloud has no profile with that name."));
        else error_text(job.net, job.rc, why, sizeof why);
        xmb_dl_notice(TR("Profile not found"), why);
        return false;
    }
    sc_svc_set_profile(job.permalink);
    s_uid = job.uid;
    snprintf(s_uid_for, sizeof s_uid_for, "%s", job.permalink);
    return true;
}

// ---------------------------------------------------------------------------
//  A list of tracks (or playlists), loaded a page at a time
// ---------------------------------------------------------------------------

enum Kind { K_LIKES, K_REPOSTS, K_UPLOADS, K_SEARCH, K_PLAYLIST, K_PLAYLISTS };

struct Listing {
    Kind      kind;
    int64_t   id;                    // a playlist's id
    char      query[96];
    char      title[128];
    sc_track *tr;  int ntr, cap_tr;
    sc_playlist *pl; int npl, cap_pl;
    int       hidden[SC_HID_COUNT];
    char      next[320];
    bool      done;                  // no more pages
    int       status;                // 0, or SC_NET_E_* of the last failure
    char      error[200];
};

struct PageJob {
    sc_net   net;
    Listing *ls;
    bool     first;
    char     url[SC_URL_MAX];
    int      rc;
    bool     failed_fatal;
    int      quality;
    char     profile[96];
};

bool grow_tracks(Listing *ls, int need) {
    if (ls->ntr + need <= ls->cap_tr) return true;
    int nc = ls->cap_tr ? ls->cap_tr * 2 : 64;
    while (nc < ls->ntr + need) nc *= 2;
    sc_track *g = (sc_track *)realloc(ls->tr, sizeof(sc_track) * (size_t)nc);
    if (!g) return false;
    ls->tr = g; ls->cap_tr = nc;
    return true;
}

bool grow_playlists(Listing *ls, int need) {
    if (ls->npl + need <= ls->cap_pl) return true;
    int nc = ls->cap_pl ? ls->cap_pl * 2 : 32;
    while (nc < ls->npl + need) nc *= 2;
    sc_playlist *g = (sc_playlist *)realloc(ls->pl, sizeof(sc_playlist) * (size_t)nc);
    if (!g) return false;
    ls->pl = g; ls->cap_pl = nc;
    return true;
}

void listing_free(Listing *ls) {
    free(ls->tr); free(ls->pl);
    memset(ls, 0, sizeof *ls);
}

// Takes the playable tracks (and the playlists) of a parsed page into the listing.
void absorb(Listing *ls, sc_page *p) {
    for (int i = 0; i < SC_HID_COUNT; i++) ls->hidden[i] += p->hidden[i];
    if (grow_tracks(ls, p->ntr)) {
        for (int i = 0; i < p->ntr; i++) {
            if (p->tr[i].hidden != SC_HID_NONE) continue;
            // a track appears once however many times a page repeats it
            bool dup = false;
            for (int k = 0; k < ls->ntr && !dup; k++) dup = ls->tr[k].id == p->tr[i].id;
            if (!dup) ls->tr[ls->ntr++] = p->tr[i];
        }
    }
    if (grow_playlists(ls, p->npl)) {
        for (int i = 0; i < p->npl; i++) ls->pl[ls->npl++] = p->pl[i];
    }
}

bool resolve_uid(PageJob *j) {
    if (s_uid > 0 && !strcmp(s_uid_for, j->profile)) return true;
    char purl[200], url[SC_URL_MAX];
    char *body = NULL;
    size_t len = 0;
    if (!sc_profile_url(j->profile, purl, sizeof purl) || !sc_url_resolve(purl, SC_CID_PLACEHOLDER, url, sizeof url)) { j->rc = SC_NET_E_ARG; return false; }
    j->rc = sc_net_api(&j->net, url, 256 * 1024, &body, &len);
    sc_user u;
    const bool ok = j->rc == SC_NET_OK && sc_parse_user(body, len, &u);
    free(body);
    if (!ok) { if (j->rc == SC_NET_OK) { j->rc = SC_NET_E_HTTP; j->net.last_status = 404; } return false; }
    s_uid = u.id;
    snprintf(s_uid_for, sizeof s_uid_for, "%s", j->profile);
    return true;
}

// The first page URL of a listing, or the next_href of the one before.
bool first_url(PageJob *j) {
    Listing *ls = j->ls;
    switch (ls->kind) {
    case K_LIKES:     return sc_url_user_likes(s_uid, 50, SC_CID_PLACEHOLDER, j->url, sizeof j->url);
    case K_REPOSTS:   return sc_url_user_reposts(s_uid, 50, SC_CID_PLACEHOLDER, j->url, sizeof j->url);
    case K_UPLOADS:   return sc_url_user_tracks(s_uid, 50, SC_CID_PLACEHOLDER, j->url, sizeof j->url);
    case K_PLAYLISTS: return sc_url_user_playlists(s_uid, 50, SC_CID_PLACEHOLDER, j->url, sizeof j->url);
    case K_SEARCH:    return sc_url_search_tracks(ls->query, 50, SC_CID_PLACEHOLDER, j->url, sizeof j->url);
    default:          return false;
    }
}

void page_work(void *arg) {
    PageJob *j = (PageJob *)arg;
    Listing *ls = j->ls;
    sc_svc_net_begin(&j->net);
    ls->status = 0;
    bool ok = true;
    if (j->first) {
        if (ls->kind != K_SEARCH) ok = resolve_uid(j);
        if (ok) ok = first_url(j);
        if (!ok && j->rc == SC_NET_OK) j->rc = SC_NET_E_ARG;
    } else {
        ok = sc_url_with_cid(ls->next, SC_CID_PLACEHOLDER, j->url, sizeof j->url);
        if (!ok) j->rc = SC_NET_E_ARG;
    }
    if (ok) {
        char *body = NULL;
        size_t len = 0;
        j->rc = sc_net_api(&j->net, j->url, 3 * 1024 * 1024, &body, &len);
        if (j->rc == SC_NET_OK) {
            sc_page page;
            sc_page_init(&page);
            const int n = sc_parse_page(body, len, j->quality, &page);
            if (n < 0) j->rc = SC_NET_E_HTTP;
            else {
                absorb(ls, &page);
                snprintf(ls->next, sizeof ls->next, "%s", page.next_href);
                if (n == 0 || !ls->next[0]) ls->done = true;
            }
            sc_page_free(&page);
        }
        free(body);
    }
    ls->status = j->rc;
    if (j->rc != SC_NET_OK) error_text(j->net, j->rc, ls->error, sizeof ls->error);
    memset(j->url, 0, sizeof j->url);
    sc_svc_net_end(&j->net);
}

// One page. False (with ls->error set) when it failed.
bool load_page(Listing *ls, bool first) {
    static PageJob job;
    memset(&job, 0, sizeof job);
    job.ls = ls;
    job.first = first;
    job.quality = sc_svc_quality();
    snprintf(job.profile, sizeof job.profile, "%s", sc_svc_profile());
    loading_run(page_work, &job, TR("Loading"), false);
    screen_begin();
    if (ls->status != SC_NET_OK) {
        char b[120];
        snprintf(b, sizeof b, "sc: a page failed (rc %d, http %d)", ls->status, job.net.last_status);
        plog(b);
    }
    return ls->status == SC_NET_OK;
}

// ---- a playlist's tracks: the playlist document has them in full or as bare ids ----

struct PlaylistJob {
    sc_net   net;
    Listing *ls;
    char     url[SC_URL_MAX];
    int      rc;
    int      quality;
};

void playlist_work(void *arg) {
    PlaylistJob *j = (PlaylistJob *)arg;
    Listing *ls = j->ls;
    sc_svc_net_begin(&j->net);
    char *body = NULL;
    size_t len = 0;
    ls->status = 0;
    sc_playlist meta;
    sc_page page;
    sc_page_init(&page);
    sc_url_playlist(ls->id, SC_CID_PLACEHOLDER, j->url, sizeof j->url);
    j->rc = sc_net_api(&j->net, j->url, 4 * 1024 * 1024, &body, &len);
    if (j->rc == SC_NET_OK && !sc_parse_playlist(body, len, j->quality, &meta, &page)) j->rc = SC_NET_E_HTTP;
    free(body);
    if (j->rc == SC_NET_OK) {
        // The first 200 tracks; the ones the document only names are fetched 30 ids at a time.
        if (page.ntr > 200) page.ntr = 200;
        int64_t ids[200];
        int nid = 0;
        for (int i = 0; i < page.ntr; i++) if (page.tr[i].hidden == SC_HID_STUB) ids[nid++] = page.tr[i].id;
        for (int b = 0; b < nid && j->rc == SC_NET_OK; b += 30) {
            const int n = nid - b < 30 ? nid - b : 30;
            sc_url_tracks_ids(ids + b, n, SC_CID_PLACEHOLDER, j->url, sizeof j->url);
            body = NULL;
            j->rc = sc_net_api(&j->net, j->url, 2 * 1024 * 1024, &body, &len);
            if (j->rc == SC_NET_OK) {
                sc_page full;
                sc_page_init(&full);
                sc_parse_tracks_array(body, len, j->quality, &full);
                sc_fill_stubs(&page, &full);
                sc_page_free(&full);
            }
            free(body);
        }
        // Whatever is still a bare id (deleted or private) is not shown.
        for (int i = 0; i < page.ntr; i++) {
            if (page.tr[i].hidden != SC_HID_STUB) continue;
            page.tr[i].hidden = SC_HID_UNSUPPORTED;
            page.hidden[SC_HID_STUB]--;
            page.hidden[SC_HID_UNSUPPORTED]++;
        }
        absorb(ls, &page);
        snprintf(ls->title, sizeof ls->title, "%s", meta.title);
        ls->done = true;
    }
    sc_page_free(&page);
    ls->status = j->rc;
    if (j->rc != SC_NET_OK) error_text(j->net, j->rc, ls->error, sizeof ls->error);
    memset(j->url, 0, sizeof j->url);
    sc_svc_net_end(&j->net);
}

bool load_playlist(Listing *ls) {
    static PlaylistJob job;
    memset(&job, 0, sizeof job);
    job.ls = ls;
    job.quality = sc_svc_quality();
    loading_run(playlist_work, &job, TR("Loading"), false);
    screen_begin();
    return ls->status == SC_NET_OK;
}

// ---------------------------------------------------------------------------
//  Playing
// ---------------------------------------------------------------------------

// The queue from the listing, a window of at most MUSIC_QUEUE_MAX tracks around the chosen one.
void play_from(const Listing *ls, int sel, const char *title) {
    static MusicTrack q[MUSIC_QUEUE_MAX];
    int first = sel - 10;
    if (first + MUSIC_QUEUE_MAX > ls->ntr) first = ls->ntr - MUSIC_QUEUE_MAX;
    if (first < 0) first = 0;
    int n = ls->ntr - first;
    if (n > MUSIC_QUEUE_MAX) n = MUSIC_QUEUE_MAX;
    for (int i = 0; i < n; i++) {
        const sc_track &t = ls->tr[first + i];
        MusicTrack &m = q[i];
        memset(&m, 0, sizeof m);
        snprintf(m.id, sizeof m.id, "sc:%lld:%d", (long long)t.id, t.tc_kbps);
        snprintf(m.name, sizeof m.name, "%s", t.title);
        snprintf(m.artist, sizeof m.artist, "%s", t.artist);
        if (t.art[0]) sc_art_key(t.art, m.art_id, sizeof m.art_id);
        m.duration_secs = (t.dur_ms + 500) / 1000;
        m.track_num = first + i + 1;
        snprintf(m.path, sizeof m.path, "sc:%s", t.tc_url);
    }
    char msg[120];
    snprintf(msg, sizeof msg, "sc: playing %d queued tracks from %d", n, sel - first);
    plog(msg);
    music_screen_open_service(q, n, sel - first, "SoundCloud", title);
    init_btns();
}

// ---------------------------------------------------------------------------
//  The track list screen
// ---------------------------------------------------------------------------

void hidden_note(const Listing *ls, char *out, int cap) {
    const int h = ls->hidden[SC_HID_ENCRYPTED] + ls->hidden[SC_HID_PREVIEW] + ls->hidden[SC_HID_UNSUPPORTED] +
                  ls->hidden[SC_HID_NOT_STREAMABLE];
    out[0] = '\0';
    if (h > 0) snprintf(out, (size_t)cap, h == 1 ? TR("%d track left out (protected or unavailable)") : TR("%d tracks left out (protected or unavailable)"), h);
}

void draw_art(const char *art_url, int x, int y, int s, bool sel) {
    char key[72];
    bool drawn = false;
    if (art_url[0] && sc_art_key(art_url, key, sizeof key)) drawn = xmb_cpu_blit_thumb(key, x, y, s, s);
    if (!drawn) {
        drawRect((u32)x, (u32)y, (u32)s, (u32)s, XMB_PANEL_HI);
        drawIcon((u32)(x + (s - UIS_H(22)) / 2), (u32)(y + (s - UIS_H(22)) / 2), ICON_MUSIC, UIS_TF(22.0f), sel ? XMB_ACCENT_ALT : XMB_TEXT_FAINT);
    }
}

// Returns when the user backs out.
void show_tracks(Listing *ls, const char *crumb) {
    int sel = 0, top = 0;
    Arm arm;
    bool leave = false;
    if (ls->status != SC_NET_OK && ls->ntr == 0 && ls->error[0]) {
        xmb_dl_notice("SoundCloud", ls->error);
        return;
    }
    while (running && !leave) {
        waitflip();
        sysUtilCheckCallback();
        poll_buttons();
        thumb_cache_tick();
        const int vis = rows_in(screen_top());
        if (arm.ready()) {
            const int was = sel;
            if (BTN_REPEAT(up)   && sel > 0)            sel--;
            if (BTN_REPEAT(down) && sel < ls->ntr - 1)  sel++;
            if (BTN_PRESSED(l2)) sel = sel - vis < 0 ? 0 : sel - vis;
            if (BTN_PRESSED(r2)) sel = sel + vis > ls->ntr - 1 ? (ls->ntr > 0 ? ls->ntr - 1 : 0) : sel + vis;
            if (sel != was) ui_sfx_play(SFX_CURSOR);
            if (sel < top) top = sel;
            if (sel >= top + vis) top = sel - vis + 1;
            if (top > ls->ntr - vis) top = ls->ntr - vis;
            if (top < 0) top = 0;

            if (BTN_PRESSED(circle)) { ui_sfx_play(SFX_CANCEL); break; }
            if (BTN_PRESSED(cross) && ls->ntr > 0 && sel < ls->ntr) {
                ui_sfx_play(SFX_DECIDE);
                char title[160];
                snprintf(title, sizeof title, "%s", ls->title[0] ? ls->title : crumb);
                play_from(ls, sel, title);
                arm.armed = false;
                continue;
            }
            // The next page, a screen before the end.
            if (!ls->done && ls->kind != K_PLAYLIST && sel >= ls->ntr - 8) {
                if (!load_page(ls, false)) {
                    ls->done = true;                           // stop asking; what is loaded stays
                    xmb_dl_notice("SoundCloud", ls->error);
                }
                arm.armed = false;
                continue;
            }
        }

        frame_begin();
        draw_screen_title(crumb, ls->query[0] && ls->kind == K_SEARCH ? ls->query : "");
        const int x = list_x(), w = list_w();
        if (ls->ntr == 0) {
            char note[100];
            hidden_note(ls, note, sizeof note);
            centre_message(ls->kind == K_SEARCH ? TR("No playable tracks found") : TR("No playable tracks here"), screen_top() + UIS_H(60));
            if (note[0]) centre_message(note, screen_top() + UIS_H(96));
        }
        const int art = row_h() - UIS_H(8);
        for (int r = 0; r < vis && top + r < ls->ntr; r++) {
            const int i = top + r;
            const sc_track &t = ls->tr[i];
            const int y = screen_top() + r * row_pitch();
            const bool s = (i == sel);
            draw_row_frame(x, y, w, row_h(), s);
            draw_art(t.art, x + UIS_W(8), y + UIS_H(4), art, s);
            const int tx = x + UIS_W(8) + art + UIS_W(16);
            const int right = x + w - UIS_W(20);
            char du[32], line[200];
            local_format_duration((t.dur_ms + 500) / 1000, du, sizeof du);
            snprintf(line, sizeof line, "%s  %s  %s", t.artist, MIDDOT, du);
            clip_text(tx, y + UIS_H(9), t.title, UIS_TF(19), s ? XMB_WHITE : XMB_TEXT, right - tx, s);
            clip_text(tx, y + UIS_H(37), line, UIS_TF(13), XMB_TEXT_DIM, right - tx, false);
        }
        if (ls->ntr > vis) {
            char pos[48];
            snprintf(pos, sizeof pos, ls->done ? "%d / %d" : "%d / %d+", sel + 1, ls->ntr);
            const int pw = ttf_text_width(pos, UIS_TF(14));
            drawTTF((u32)(x + w - pw), (u32)(XMB_OY + UIS_H(70)), pos, UIS_TF(14), XMB_TEXT_FAINT);
        }
        {
            char note[100];
            hidden_note(ls, note, sizeof note);
            if (note[0] && ls->ntr > 0) {
                const int nw = ttf_text_width(note, UIS_TF(13));
                drawTTF((u32)(x + w - nw), (u32)(XMB_OY + UIS_H(90)), note, UIS_TF(13), XMB_TEXT_FAINT);
            }
        }
        { Hint h[5]; int nh = 0;
          if (ls->ntr > 0) { h[nh].glyph = 'X'; h[nh].label = TRN("Play"); nh++; }
          if (ls->ntr > vis) { h[nh].glyph = 'L'; h[nh].label = ""; nh++; h[nh].glyph = 'R'; h[nh].label = TRN("Page"); nh++; }
          h[nh].glyph = 'C'; h[nh].label = TRN("Back"); nh++;
          draw_hints_bar(h, nh); }
        flip();
    }
    init_btns();
}

// ---------------------------------------------------------------------------
//  The playlists screen
// ---------------------------------------------------------------------------

void show_playlists(Listing *ls, const char *crumb) {
    int sel = 0, top = 0;
    Arm arm;
    bool leave = false;
    if (ls->status != SC_NET_OK && ls->npl == 0 && ls->error[0]) {
        xmb_dl_notice("SoundCloud", ls->error);
        return;
    }
    while (running && !leave) {
        waitflip();
        sysUtilCheckCallback();
        poll_buttons();
        thumb_cache_tick();
        const int vis = rows_in(screen_top());
        if (arm.ready()) {
            const int was = sel;
            if (BTN_REPEAT(up)   && sel > 0)            sel--;
            if (BTN_REPEAT(down) && sel < ls->npl - 1)  sel++;
            if (BTN_PRESSED(l2)) sel = sel - vis < 0 ? 0 : sel - vis;
            if (BTN_PRESSED(r2)) sel = sel + vis > ls->npl - 1 ? (ls->npl > 0 ? ls->npl - 1 : 0) : sel + vis;
            if (sel != was) ui_sfx_play(SFX_CURSOR);
            if (sel < top) top = sel;
            if (sel >= top + vis) top = sel - vis + 1;
            if (top > ls->npl - vis) top = ls->npl - vis;
            if (top < 0) top = 0;

            if (BTN_PRESSED(circle)) { ui_sfx_play(SFX_CANCEL); break; }
            if (BTN_PRESSED(cross) && ls->npl > 0 && sel < ls->npl) {
                ui_sfx_play(SFX_DECIDE);
                static Listing inner;
                memset(&inner, 0, sizeof inner);
                inner.kind = K_PLAYLIST;
                inner.id = ls->pl[sel].id;
                snprintf(inner.title, sizeof inner.title, "%s", ls->pl[sel].title);
                load_playlist(&inner);
                show_tracks(&inner, ls->pl[sel].title);
                listing_free(&inner);
                arm.armed = false;
                continue;
            }
            if (!ls->done && sel >= ls->npl - 6) {
                if (!load_page(ls, false)) { ls->done = true; xmb_dl_notice("SoundCloud", ls->error); }
                arm.armed = false;
                continue;
            }
        }

        frame_begin();
        draw_screen_title(crumb, "");
        const int x = list_x(), w = list_w();
        if (ls->npl == 0) centre_message(TR("No playlists here"), screen_top() + UIS_H(60));
        const int art = row_h() - UIS_H(8);
        for (int r = 0; r < vis && top + r < ls->npl; r++) {
            const int i = top + r;
            const sc_playlist &p = ls->pl[i];
            const int y = screen_top() + r * row_pitch();
            const bool s = (i == sel);
            draw_row_frame(x, y, w, row_h(), s);
            draw_art(p.art, x + UIS_W(8), y + UIS_H(4), art, s);
            const int tx = x + UIS_W(8) + art + UIS_W(16);
            const int right = x + w - UIS_W(20);
            char line[160];
            snprintf(line, sizeof line, p.track_count == 1 ? TR("%d track") : TR("%d tracks"), p.track_count);
            if (p.owner[0]) { size_t n = strlen(line); snprintf(line + n, sizeof line - n, "  %s  %s", MIDDOT, p.owner); }
            clip_text(tx, y + UIS_H(9), p.title, UIS_TF(19), s ? XMB_WHITE : XMB_TEXT, right - tx, s);
            clip_text(tx, y + UIS_H(37), line, UIS_TF(13), XMB_TEXT_DIM, right - tx, false);
        }
        { Hint h[3]; int nh = 0;
          if (ls->npl > 0) { h[nh].glyph = 'X'; h[nh].label = TRN("Open"); nh++; }
          h[nh].glyph = 'C'; h[nh].label = TRN("Back"); nh++;
          draw_hints_bar(h, nh); }
        flip();
    }
    init_btns();
}

// ---------------------------------------------------------------------------
//  The tab's list
// ---------------------------------------------------------------------------

enum Row { R_LIKES, R_PLAYLISTS, R_REPOSTS, R_TRACKS, R_SEARCH, R_PROFILE, R_QUALITY, R_COUNT };

int s_sel = 0;

bool have_profile(void) { return sc_svc_profile()[0] != 0; }

void row_text(int r, char *title, int tcap, char *sub, int scap, int *icon) {
    *icon = ICON_MUSIC;
    sub[0] = '\0';
    const bool needs = (r == R_LIKES || r == R_PLAYLISTS || r == R_REPOSTS || r == R_TRACKS);
    switch (r) {
    case R_LIKES:     snprintf(title, (size_t)tcap, "%s", TR("Likes"));     break;
    case R_PLAYLISTS: snprintf(title, (size_t)tcap, "%s", TR("Playlists")); *icon = ICON_COLLECTIONS; break;
    case R_REPOSTS:   snprintf(title, (size_t)tcap, "%s", TR("Reposts"));   break;
    case R_TRACKS:    snprintf(title, (size_t)tcap, "%s", TR("Tracks"));    break;
    case R_SEARCH:    snprintf(title, (size_t)tcap, "%s", TR("Search"));    *icon = ICON_SEARCH; break;
    case R_PROFILE:   snprintf(title, (size_t)tcap, "%s", TR("Profile"));   *icon = ICON_SETTINGS; break;
    default:          snprintf(title, (size_t)tcap, "%s", TR("Audio quality")); *icon = ICON_SETTINGS; break;
    }
    if (needs) {
        if (have_profile()) snprintf(sub, (size_t)scap, TR("From %s"), sc_svc_profile());
        else snprintf(sub, (size_t)scap, "%s", TR("Choose a profile first"));
    } else if (r == R_SEARCH) {
        snprintf(sub, (size_t)scap, "%s", TR("Find tracks on SoundCloud"));
    } else if (r == R_PROFILE) {
        if (have_profile()) snprintf(sub, (size_t)scap, TR("%s  -  X to change"), sc_svc_profile());
        else snprintf(sub, (size_t)scap, "%s", TR("Not set. X to enter the name of a public profile"));
    } else {
        snprintf(sub, (size_t)scap, TR("%d kbps  -  X to switch"), sc_svc_quality());
    }
}

void draw_root_panels(int top, int x, int w) {
    const int vis = rows_in(top);
    for (int r = 0; r < vis && r < R_COUNT; r++)
        draw_row_frame(x, top + r * row_pitch(), w, row_h(), r == s_sel);
}

void draw_root_text(int top, int x, int w) {
    const int vis = rows_in(top);
    for (int r = 0; r < vis && r < R_COUNT; r++) {
        const int y = top + r * row_pitch();
        const bool s = (r == s_sel);
        char title[64], sub[160];
        int icon;
        row_text(r, title, sizeof title, sub, sizeof sub, &icon);
        drawIcon((u32)(x + UIS_W(18)), (u32)(y + (row_h() - UIS_H(24)) / 2), icon, UIS_TF(24.0f),
                 s ? XMB_ACCENT_ALT : XMB_TEXT_DIM);
        const int tx = x + UIS_W(68);
        clip_text(tx, y + UIS_H(9), title, UIS_TF(19), s ? XMB_WHITE : XMB_TEXT, w - UIS_W(90), s);
        clip_text(tx, y + UIS_H(37), sub, UIS_TF(13), XMB_TEXT_DIM, w - UIS_W(90), false);
    }
}

void open_listing(Kind kind, const char *crumb) {
    static Listing ls;
    memset(&ls, 0, sizeof ls);
    ls.kind = kind;
    if (!load_page(&ls, true) && ls.ntr == 0 && ls.npl == 0) {
        xmb_dl_notice("SoundCloud", ls.error[0] ? ls.error : TR("Could not load this list."));
        listing_free(&ls);
        return;
    }
    if (kind == K_PLAYLISTS) show_playlists(&ls, crumb);
    else show_tracks(&ls, crumb);
    listing_free(&ls);
}

void open_search(void) {
    char q[96] = "";
    const int r = get_input(q, sizeof q, TR("Search SoundCloud"), false);
    init_btns();
    if (r != 1 || !q[0]) return;
    static Listing ls;
    memset(&ls, 0, sizeof ls);
    ls.kind = K_SEARCH;
    snprintf(ls.query, sizeof ls.query, "%s", q);
    if (!load_page(&ls, true) && ls.ntr == 0) {
        xmb_dl_notice("SoundCloud", ls.error[0] ? ls.error : TR("The search failed."));
        listing_free(&ls);
        return;
    }
    show_tracks(&ls, TR("Search"));
    listing_free(&ls);
}

void open_row(int r) {
    switch (r) {
    case R_LIKES: case R_PLAYLISTS: case R_REPOSTS: case R_TRACKS:
        if (!have_profile() && !ask_profile()) return;
        open_listing(r == R_LIKES ? K_LIKES : r == R_PLAYLISTS ? K_PLAYLISTS : r == R_REPOSTS ? K_REPOSTS : K_UPLOADS,
                     r == R_LIKES ? TR("Likes") : r == R_PLAYLISTS ? TR("Playlists") : r == R_REPOSTS ? TR("Reposts") : TR("Tracks"));
        break;
    case R_SEARCH:  open_search(); break;
    case R_PROFILE: ask_profile(); break;
    case R_QUALITY: sc_svc_set_quality(sc_svc_quality() == 160 ? 96 : 160); break;
    }
}

void root_move(void) {
    const int was = s_sel;
    if (BTN_REPEAT(up)   && s_sel > 0)           s_sel--;
    if (BTN_REPEAT(down) && s_sel < R_COUNT - 1) s_sel++;
    if (s_sel != was) ui_sfx_play(SFX_CURSOR);
}

}   // namespace

// ---------------------------------------------------------------------------
//  The tab, as the XMB drives it
// ---------------------------------------------------------------------------

void xmb_sc_on_enter(void) {
    if (s_sel >= R_COUNT) s_sel = 0;
}

bool xmb_sc_at_top(void) { return s_sel <= 0; }

bool xmb_input_sc(void) {
    if (BTN_PRESSED(l1)) { xmb_switch_tab(xmb_next_enabled(g_active_tab, -1)); return false; }
    if (BTN_PRESSED(r1)) { xmb_switch_tab(xmb_next_enabled(g_active_tab, +1)); return false; }
    root_move();
    if (BTN_PRESSED(cross)) { ui_sfx_play(SFX_DECIDE); open_row(s_sel); }
    return false;
}

void xmb_sc_hints(void) {
    Hint h[2]; int n = 0;
    h[n].glyph = 'X'; h[n].label = TRN("Open"); n++;
    h[n].glyph = 'S'; h[n].label = wave_vis_next_label(); n++;
    draw_hints_bar(h, n);
}

void xmb_cpu_draw_sc(void) {
    draw_root_panels(tab_top(), (int)(((int)display_width - XMB_LIST_W) / 2), XMB_LIST_W);
}

void xmb_draw_sc(void) {
    draw_root_text(tab_top(), (int)(((int)display_width - XMB_LIST_W) / 2), XMB_LIST_W);
}
