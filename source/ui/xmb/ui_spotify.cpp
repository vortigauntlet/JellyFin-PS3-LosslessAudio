// The Spotify tab: this console as a Spotify Connect speaker, with no Jellyfin server and no developer account.
//
//   * Entering the tab starts the receiver (sp_svc): the console shows up in the Spotify app's "Connect to a device" list, the
//     phone picks it, and what the phone plays comes out of the console through the music engine.  The first time, the phone
//     sends the account's login to the console (a few seconds); after that the console signs in by itself.
//   * The tab is a short list: what the receiver is doing, Now Playing, and Sign out.  A new track starting opens Now Playing by
//     itself, the screen the other music uses, so the visualisers, volume and the pad's skip and pause all work; skip and pause go
//     through Spotify, so the phone follows.  Leaving Now Playing pauses (the phone shows paused rather than playing into silence).
//   * The receiver is unofficial and may stop working whenever Spotify changes its protocol: the status line says what failed.
#include "../../build_config.h"
#if APP_SPOTIFY

#include <stdio.h>
#include <string.h>

#include "ui_internal.h"
#include "ui_scale.h"
#include "ui_sfx.h"
#include "ui_wave.h"
#include "music_screen.h"
#include "sp_svc.h"

namespace {

int list_x(void)     { return XMB_ITEM_PAD; }
int row_h(void)      { return UIS_H(62); }
int row_pitch(void)  { return row_h() + UIS_H(8); }
int tab_top(void)    { return XMB_CONTENT_Y + UIS_H(10); }

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

void draw_row_frame(int x, int y, int w, int h, bool sel) {
    drawRect((u32)x, (u32)y, (u32)w, (u32)h, sel ? XMB_PANEL_HI : XMB_PANEL);
    if (sel) drawRect((u32)(x - UIS_W(4)), (u32)y, UIS_W(3), (u32)h, XMB_ACCENT);
}

enum Row { R_STATUS, R_NOW, R_SIGNOUT, R_COUNT };

int s_sel = 0;
unsigned s_auto_serial = 0;          // the track serial Now Playing was last opened for by itself
bool s_confirm_signout = false;
uint64_t s_signed_out_at = 0;        // when the sign-in was forgotten (the line says so for a while)

bool playing(void) {
    const SpState st = sp_svc_state();
    return st == SP_PLAYING || st == SP_PAUSED;
}

void row_text(int r, char *title, int tcap, char *sub, int scap, int *icon) {
    *icon = ICON_MUSIC;
    switch (r) {
    case R_STATUS: {
        const SpState st = sp_svc_state();
        const char *t = TR("Spotify");
        switch (st) {
        case SP_PAIRING:    t = TR("Waiting for Spotify");   break;
        case SP_SIGNING_IN: t = TR("Signing in");            break;
        case SP_READY:      t = TR("Ready to play");         break;
        case SP_PLAYING:    t = TR("Playing");               break;
        case SP_PAUSED:     t = TR("Paused");                break;
        case SP_ERROR:      t = TR("Not connected");         break;
        default:            t = TR("Starting");              break;
        }
        snprintf(title, (size_t)tcap, "%s", t);
        snprintf(sub, (size_t)scap, "%s", sp_svc_status_text());
        break;
    }
    case R_NOW: {
        SpTrack t;
        snprintf(title, (size_t)tcap, "%s", TR("Now Playing"));
        if (playing() && sp_svc_track(&t)) snprintf(sub, (size_t)scap, "%s - %s", t.title, t.artist);
        else snprintf(sub, (size_t)scap, "%s", TR("Nothing playing yet: start a song on the phone and pick this console"));
        break;
    }
    case R_SIGNOUT:
        snprintf(title, (size_t)tcap, "%s", TR("Sign out"));
        if (s_confirm_signout) snprintf(sub, (size_t)scap, "%s", TR("Press X again to forget this Spotify account on the console"));
        else if (s_signed_out_at) snprintf(sub, (size_t)scap, "%s", TR("Forgotten: restart the app, then pick this console in Spotify to sign in again"));
        else snprintf(sub, (size_t)scap, "%s", TR("Forget the saved Spotify account; you pair again from the phone"));
        break;
    default:
        title[0] = sub[0] = '\0';
    }
}

void open_row(int r) {
    switch (r) {
    case R_NOW:
        if (playing()) music_screen_open_spotify();
        break;
    case R_SIGNOUT:
        if (!s_confirm_signout) { s_confirm_signout = true; return; }
        s_confirm_signout = false;
        sp_svc_forget();
        s_signed_out_at = 1;
        break;
    default:
        break;
    }
}

}   // namespace

void xmb_sp_on_enter(void) {
    sp_svc_start("PS3");                 // starts once; a second call does nothing
    s_confirm_signout = false;
    if (s_sel >= R_COUNT) s_sel = 0;
}

bool xmb_sp_at_top(void) { return s_sel <= 0; }

bool xmb_input_sp(void) {
    if (BTN_PRESSED(l1)) { xmb_switch_tab(xmb_next_enabled(g_active_tab, -1)); return false; }
    if (BTN_PRESSED(r1)) { xmb_switch_tab(xmb_next_enabled(g_active_tab, +1)); return false; }
    const int was = s_sel;
    if (BTN_REPEAT(up)   && s_sel > 0)           s_sel--;
    if (BTN_REPEAT(down) && s_sel < R_COUNT - 1) s_sel++;
    if (s_sel != was) { ui_sfx_play(SFX_CURSOR); s_confirm_signout = false; }
    if (BTN_PRESSED(cross)) { ui_sfx_play(SFX_DECIDE); open_row(s_sel); }
    // A song started on the phone: show it, once per track.
    if (sp_svc_state() == SP_PLAYING && sp_svc_track_serial() != s_auto_serial) {
        s_auto_serial = sp_svc_track_serial();
        music_screen_open_spotify();
    }
    return false;
}

void xmb_sp_hints(void) {
    Hint h[2]; int n = 0;
    h[n].glyph = 'X'; h[n].label = TRN("Open"); n++;
    h[n].glyph = 'S'; h[n].label = wave_vis_next_label(); n++;
    draw_hints_bar(h, n);
}

void xmb_cpu_draw_sp(void) {
    const int top = tab_top(), x = (int)(((int)display_width - XMB_LIST_W) / 2);
    for (int r = 0; r < R_COUNT; r++) draw_row_frame(x, top + r * row_pitch(), XMB_LIST_W, row_h(), r == s_sel);
}

void xmb_draw_sp(void) {
    const int top = tab_top(), x = (int)(((int)display_width - XMB_LIST_W) / 2), w = XMB_LIST_W;
    for (int r = 0; r < R_COUNT; r++) {
        const int y = top + r * row_pitch();
        const bool s = (r == s_sel);
        char title[64], sub[200];
        int icon;
        row_text(r, title, sizeof title, sub, sizeof sub, &icon);
        drawIcon((u32)(x + UIS_W(18)), (u32)(y + (row_h() - UIS_H(24)) / 2), icon, UIS_TF(24.0f), s ? XMB_ACCENT_ALT : XMB_TEXT_DIM);
        const int tx = x + UIS_W(68);
        clip_text(tx, y + UIS_H(9), title, UIS_TF(19), s ? XMB_WHITE : XMB_TEXT, w - UIS_W(90), s);
        clip_text(tx, y + UIS_H(37), sub, UIS_TF(13), XMB_TEXT_DIM, w - UIS_W(90), false);
    }
}

#endif   // APP_SPOTIFY
