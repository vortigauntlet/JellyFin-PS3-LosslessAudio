// Offline downloads, the screens (Stage 5): the Downloads list, the Offline
// library, and the confirm dialog both use.
//
// Same shape as the other full-screen overlays (xmb_resume_choice,
// xmb_show_item_info): a blocking loop over the wave background that owns
// input until Circle.  Everything they SAY and DECIDE comes from
// offline/dl_ui.h (host-tested); this file only lays it out and forwards
// the result to dl_manager / show_player_offline.
//
// Cost while open: the queue is re-read about four times a second, and only
// for the rows on screen (dl_find per visible row); the Offline list reads
// metadata only when the list or the scroll position changes.  Nothing here
// runs when neither screen is open, and nothing is allocated per frame.

#include <stdio.h>
#include <string.h>

#include <sysutil/sysutil.h>

#include "ui_internal.h"
#include "ui_wave.h"
#include "rsxutil.h"
#include "timing.h"
#include "plog.h"
#include "slog.h"
#include "player.h"
#include "dl_manager.h"
#include "dl_library.h"
#include "dl_ui.h"
#include "dl_service.h"

#define DL_WARN_CLR   0x00E8B64CUL   // the rating star's gold: attention, not error red
#define DL_TOAST_US   3000000ULL
#define DL_REFRESH_US 250000ULL

static DlUiContext ui_context(void) {
    DlUiContext cx;
    cx.playback_block = dl_playback_blocking();
    cx.auth_held      = dl_auth_held();
    cx.ready          = dl_manager_ready();
    return cx;
}

// Text clipped to max_w with an ellipsis.
static void clip_text(int x, int y, const char *text, float px, u32 color,
                      int max_w, bool bold) {
    char buf[160];
    snprintf(buf, sizeof(buf), "%s", text);
    int len = (int)strlen(buf);
    if (ttf_text_width(buf, px, bold) > max_w) {
        while (len > 3 && ttf_text_width(buf, px, bold) > max_w) {
            buf[--len] = '\0';
            if (len > 3) { buf[len - 1] = '.'; buf[len - 2] = '.'; buf[len - 3] = '.'; }
        }
    }
    drawTTF((u32)x, (u32)y, buf, px, color, bold);
}

static void frame_begin(void) {
    clearScreen(XMB_BG);
    wave_draw();
    rsxSync();
}

static void draw_title(const char *title, const char *banner, u32 banner_clr) {
    const int x = XMB_ITEM_PAD, y = XMB_OY + UIS_H(28);
    drawTTF((u32)x, (u32)y, title, 28, XMB_TEXT, true);
    if (banner && banner[0])
        drawTTF((u32)x, (u32)(y + UIS_H(42)), banner, 15, banner_clr);
}

static int list_top(void)    { return XMB_OY + UIS_H(112); }
static int list_bottom(void) { return (int)display_height - XMB_BOTTOM_PAD; }
static int row_h(void)       { return UIS_H(72); }
static int row_pitch(void)   { return row_h() + UIS_H(10); }
static int rows_visible(void) {
    int n = (list_bottom() - list_top()) / row_pitch();
    return n < 1 ? 1 : n > 8 ? 8 : n;
}

// ---------------------------------------------------------------------------
// Confirm
// ---------------------------------------------------------------------------

// Two options, the safe one first and selected.  True = the action.
bool xmb_dl_confirm(const char *title, const char *line, const char *safe,
                    const char *action) {
    rsxSync();
    flip();
    init_btns();
    int sel = 0;          // 0 = keep (safe default), 1 = the action
    bool armed = false;
    const char *opts[2] = { safe, action };
    while (running) {
        waitflip();
        sysUtilCheckCallback();
        poll_buttons();
        if (!armed) {
            if (!btn_cur.cross && !btn_cur.circle && !btn_cur.square) armed = true;
        } else {
            if (BTN_PRESSED(circle)) { init_btns(); return false; }
            if (BTN_PRESSED(up))     sel = 0;
            if (BTN_PRESSED(down))   sel = 1;
            if (BTN_PRESSED(cross))  { init_btns(); return sel == 1; }
        }
        frame_begin();
        int pw = UIS_W(600), ph = UIS_H(236);
        int px = ((int)display_width - pw) / 2, py = ((int)display_height - ph) / 2;
        drawRect((u32)px, (u32)py, (u32)pw, (u32)ph, XMB_PANEL);
        drawRect((u32)px, (u32)py, (u32)pw, 1, XMB_HAIRLINE);
        drawRect((u32)px, (u32)(py + ph - 1), (u32)pw, 1, XMB_HAIRLINE);
        drawRect((u32)px, (u32)py, 1, (u32)ph, XMB_HAIRLINE);
        drawRect((u32)(px + pw - 1), (u32)py, 1, (u32)ph, XMB_HAIRLINE);
        const int cx = px + 32, mw = pw - 64;
        int y = py + 28;
        clip_text(cx, y, title, 24, XMB_WHITE, mw, true);
        y += 38;
        clip_text(cx, y, line, 15, XMB_TEXT_DIM, mw, false);
        y += 36;
        const int oh = UIS_H(44);
        for (int i = 0; i < 2; i++) {
            int oy = y + i * (oh + 8);
            if (i == sel) {
                drawRect((u32)cx, (u32)oy, (u32)mw, (u32)oh, XMB_PANEL_HI);
                drawRect((u32)(cx - 4), (u32)oy, 3, (u32)oh, XMB_ACCENT);
            }
            drawTTF_vcentered((u32)(cx + 16), oy + oh / 2, opts[i], 19,
                              i == sel ? XMB_TEXT : XMB_TEXT_DIM);
        }
        { static const Hint h[] = {{'X', "Select"}, {'C', "Back"}};
          draw_hints_bar(h, 2); }
        flip();
    }
    init_btns();
    return false;
}

// ---------------------------------------------------------------------------
// Shared row drawing
// ---------------------------------------------------------------------------

static void draw_row_frame(int x, int y, int w, int h, bool sel) {
    drawRect((u32)x, (u32)y, (u32)w, (u32)h, sel ? XMB_PANEL_HI : XMB_PANEL);
    if (sel) drawRect((u32)(x - 4), (u32)y, 3, (u32)h, XMB_ACCENT);
}

static void draw_bar(int x, int y, int w, int permille) {
    drawRect((u32)x, (u32)y, (u32)w, 4, XMB_HAIRLINE);
    int fw = (int)((long long)w * (permille < 0 ? 0 : permille > 1000 ? 1000 : permille) / 1000);
    if (fw > 0) drawRect((u32)x, (u32)y, (u32)fw, 4, XMB_ACCENT);
}

static void draw_empty(const char *line1, const char *line2) {
    int y = list_top() + UIS_H(40);
    drawTTF((u32)XMB_ITEM_PAD, (u32)y, line1, 20, XMB_TEXT_DIM);
    drawTTF((u32)XMB_ITEM_PAD, (u32)(y + 32), line2, 15, XMB_TEXT_FAINT);
}

static void draw_toast(const char *msg, u64 until) {
    if (!msg[0] || timing_get_us() > until) return;
    drawTTF((u32)XMB_ITEM_PAD, (u32)(list_bottom() - UIS_H(4)), msg, 15, DL_WARN_CLR);
}

// ---------------------------------------------------------------------------
// Downloads
// ---------------------------------------------------------------------------

void xmb_show_downloads(void) {
    rsxSync();
    flip();
    init_btns();
    slog_state("DOWNLOADS_OPEN");

    static char ids[DL_MAX_ITEMS][DL_ID_MAX];
    static DlStatus vis[8];
    static bool     vis_ok[8];
    int  n = 0, sel = 0, top = 0;
    u64  next_refresh = 0, toast_until = 0;
    char toast[96] = "";
    bool armed = false;

    while (running) {
        waitflip();
        sysUtilCheckCallback();
        poll_buttons();

        const int visible = rows_visible();
        if (!armed) {
            if (!btn_cur.cross && !btn_cur.circle && !btn_cur.square) armed = true;
        } else {
            if (BTN_PRESSED(circle)) break;
            int moved = sel;
            if (BTN_REPEAT(up)   && sel > 0)     sel--;
            if (BTN_REPEAT(down) && sel < n - 1) sel++;
            if (moved != sel) {
                sel = dl_ui_clamp_selection(sel, n, visible, &top);
                next_refresh = 0;
            }
            const int vi = sel - top;
            // Not in a frame that scrolled: vis[] still holds the old window.
            const bool have = moved == sel && n > 0 && vi >= 0 && vi < visible && vis_ok[vi];
            DlUiAction act = DL_UI_NONE;
            if (have && BTN_PRESSED(cross))  act = dl_ui_row_primary(&vis[vi]);
            if (have && BTN_PRESSED(square)) act = dl_ui_row_secondary(&vis[vi]);
            if (act != DL_UI_NONE) {
                const DlStatus st = vis[vi];
                const char *id = st.rec.id;
                int r = DL_OK;
                if (act == DL_UI_PLAY_OFFLINE) {
                    if (!show_player_offline(id, 0))
                        snprintf(toast, sizeof(toast), "The offline copy is missing or damaged");
                    else
                        toast[0] = '\0';
                    toast_until = timing_get_us() + DL_TOAST_US;
                    init_btns();
                    armed = false;
                } else if (!dl_ui_action_needs_confirm(act, &st) ||
                           xmb_dl_confirm(st.rec.title,
                                          act == DL_UI_REMOVE ? "Delete the downloaded file from the HDD?"
                                                              : "Stop and throw away what has downloaded so far?",
                                          act == DL_UI_REMOVE ? "Keep it" : "Keep downloading",
                                          act == DL_UI_REMOVE ? "Delete" : "Cancel download")) {
                    switch (act) {
                    case DL_UI_PAUSE:  r = dl_pause(id);  break;
                    case DL_UI_RESUME: r = dl_resume(id); break;
                    case DL_UI_RETRY:  r = dl_retry(id);  break;
                    case DL_UI_CANCEL: r = dl_cancel(id); break;
                    case DL_UI_REMOVE: r = dl_remove(id); break;
                    default: break;
                    }
                    snprintf(toast, sizeof(toast), "%s", dl_ui_result_text(r));
                    toast_until = timing_get_us() + DL_TOAST_US;
                    armed = false;
                }
                next_refresh = 0;
            }
        }

        // ---- refresh (after input, so a scroll never draws stale rows) ----
        {
            const u64 now = timing_get_us();
            if (now >= next_refresh) {
                next_refresh = now + DL_REFRESH_US;
                n = dl_ids(ids, DL_MAX_ITEMS);
                sel = dl_ui_clamp_selection(sel, n, visible, &top);
                for (int i = 0; i < visible; i++)
                    vis_ok[i] = top + i < n && dl_find(ids[top + i], &vis[i]);
            }
        }

        // ---- draw ----
        const DlUiContext cx = ui_context();
        frame_begin();
        draw_title("Downloads", dl_ui_queue_banner(&cx), DL_WARN_CLR);
        const int x = XMB_ITEM_PAD, w = (int)display_width - 2 * XMB_ITEM_PAD;
        if (n == 0) {
            draw_empty("Nothing is downloading.",
                       "Open a film or an episode and choose Download.");
        }
        for (int i = 0; i < visible && top + i < n; i++) {
            if (!vis_ok[i]) continue;
            DlUiRow row;
            dl_ui_row(&vis[i], &cx, &row);
            const bool s = (top + i == sel);
            const int y = list_top() + i * row_pitch();
            draw_row_frame(x, y, w, row_h(), s);
            clip_text(x + 20, y + UIS_H(10), row.title, 19, s ? XMB_WHITE : XMB_TEXT,
                      w - 280, s);
            const u32 sc = row.warning ? DL_WARN_CLR : row.emphasis ? XMB_ACCENT : XMB_TEXT_DIM;
            clip_text(x + 20, y + UIS_H(38), row.status, 14, sc, w - 280, false);
            if (row.size[0]) {
                int sw = ttf_text_width(row.size, 14);
                drawTTF((u32)(x + w - 20 - sw), (u32)(y + UIS_H(38)), row.size, 14, XMB_TEXT_DIM);
            }
            if (row.permille >= 0)
                draw_bar(x + 20, y + row_h() - UIS_H(12), w - 40, row.permille);
        }
        if (n > visible) {
            char pos[24];
            snprintf(pos, sizeof(pos), "%d / %d", sel + 1, n);
            int pw = ttf_text_width(pos, 14);
            drawTTF((u32)(x + w - pw), (u32)(XMB_OY + UIS_H(40)), pos, 14, XMB_TEXT_FAINT);
        }
        draw_toast(toast, toast_until);
        {
            Hint h[3]; int nh = 0;
            h[nh].glyph = 'C'; h[nh].label = "Back"; nh++;
            const int vi = sel - top;
            if (n > 0 && vi >= 0 && vi < visible && vis_ok[vi]) {
                const char *p = dl_ui_action_label(dl_ui_row_primary(&vis[vi]));
                const char *q = dl_ui_action_label(dl_ui_row_secondary(&vis[vi]));
                if (q[0]) { h[nh].glyph = 'S'; h[nh].label = q; nh++; }
                if (p[0]) { h[nh].glyph = 'X'; h[nh].label = p; nh++; }
            }
            draw_hints_bar(h, nh);
        }
        flip();
    }
    slog_state("DOWNLOADS_CLOSE");
    init_btns();
}

// ---------------------------------------------------------------------------
// Offline library
// ---------------------------------------------------------------------------

void xmb_show_offline(void) {
    rsxSync();
    flip();
    init_btns();
    slog_state("OFFLINE_OPEN");

    static char ids[DL_MAX_ITEMS][DL_ID_MAX];
    static DlLibraryEntry vis[8];
    static bool vis_ok[8];
    int  n = 0, sel = 0, top = 0, loaded_top = -1;
    bool reload = true, armed = false;
    u64  toast_until = 0;
    char toast[96] = "";

    while (running) {
        waitflip();
        sysUtilCheckCallback();
        poll_buttons();

        const int visible = rows_visible();
        if (!armed) {
            if (!btn_cur.cross && !btn_cur.circle && !btn_cur.square) armed = true;
        } else {
            if (BTN_PRESSED(circle)) break;
            if (BTN_REPEAT(up)   && sel > 0)     sel--;
            if (BTN_REPEAT(down) && sel < n - 1) sel++;
            sel = dl_ui_clamp_selection(sel, n, visible, &top);
            const int vi = sel - top;
            const bool have = loaded_top == top && n > 0 && vi >= 0 && vi < visible &&
                              vis_ok[vi];
            if (have && BTN_PRESSED(cross)) {
                static char id[DL_ID_MAX];
                snprintf(id, sizeof(id), "%s", vis[vi].meta.id);
                if (!show_player_offline(id, 0)) {
                    snprintf(toast, sizeof(toast), "The offline copy is missing or damaged");
                    toast_until = timing_get_us() + DL_TOAST_US;
                }
                reload = true;            // the library may have changed under us
                init_btns();
                armed = false;
            } else if (have && BTN_PRESSED(square)) {
                char title[DL_TITLE_MAX], sub[160];
                dl_ui_offline_lines(&vis[vi].meta, vis[vi].meta_ok, vis[vi].bytes,
                                    title, sizeof(title), sub, sizeof(sub));
                if (xmb_dl_confirm(title, "Delete the downloaded file from the HDD?",
                                   "Keep it", "Delete")) {
                    int r = dl_remove(vis[vi].meta.id);
                    snprintf(toast, sizeof(toast), "%s",
                             r == DL_OK ? "Deleted" : dl_ui_result_text(r));
                    toast_until = timing_get_us() + DL_TOAST_US;
                    reload = true;
                }
                armed = false;
            }
        }

        // ---- refresh: metadata is read only when the list or window moves ----
        if (reload) {
            reload = false;
            n = dl_library_ids(ids, DL_MAX_ITEMS);
            loaded_top = -1;
        }
        sel = dl_ui_clamp_selection(sel, n, visible, &top);
        if (loaded_top != top) {
            loaded_top = top;
            for (int i = 0; i < visible; i++)
                vis_ok[i] = top + i < n && dl_library_get(ids[top + i], &vis[i]);
        }

        // ---- draw ----
        frame_begin();
        draw_title("Offline", n > 0 ? "Plays from the HDD -- no server needed" : "", XMB_TEXT_DIM);
        const int x = XMB_ITEM_PAD, w = (int)display_width - 2 * XMB_ITEM_PAD;
        if (n == 0)
            draw_empty("Nothing downloaded yet.",
                       dl_manager_ready() ? "Open a film or an episode and choose Download."
                                          : "Downloads are unavailable: no writable HDD folder.");
        for (int i = 0; i < visible && top + i < n; i++) {
            if (!vis_ok[i]) continue;
            char title[DL_TITLE_MAX], sub[160];
            dl_ui_offline_lines(&vis[i].meta, vis[i].meta_ok, vis[i].bytes,
                                title, sizeof(title), sub, sizeof(sub));
            const bool s = (top + i == sel);
            const int y = list_top() + i * row_pitch();
            draw_row_frame(x, y, w, row_h(), s);
            clip_text(x + 20, y + UIS_H(12), title, 19, s ? XMB_WHITE : XMB_TEXT, w - 40, s);
            clip_text(x + 20, y + UIS_H(40), sub, 14, XMB_TEXT_DIM, w - 40, false);
        }
        if (n > visible) {
            char pos[24];
            snprintf(pos, sizeof(pos), "%d / %d", sel + 1, n);
            int pw = ttf_text_width(pos, 14);
            drawTTF((u32)(x + w - pw), (u32)(XMB_OY + UIS_H(40)), pos, 14, XMB_TEXT_FAINT);
        }
        draw_toast(toast, toast_until);
        {
            Hint h[3]; int nh = 0;
            h[nh].glyph = 'C'; h[nh].label = "Back"; nh++;
            if (n > 0) {
                h[nh].glyph = 'S'; h[nh].label = "Delete"; nh++;
                h[nh].glyph = 'X'; h[nh].label = "Play"; nh++;
            }
            draw_hints_bar(h, nh);
        }
        flip();
    }
    slog_state("OFFLINE_CLOSE");
    init_btns();
}

bool xmb_offer_offline_after_login_failure(void) {
    // Only worth asking when there is something to play.
    if (!dl_service_wait_restored(1500)) return false;
    char one[1][DL_ID_MAX];
    if (dl_library_ids(one, 1) == 0) return false;
    if (!xmb_dl_confirm("Couldn't sign in",
                        "You have downloads on this console. Play one offline?",
                        "Try again", "Open Offline"))
        return false;
    xmb_show_offline();
    return true;
}
