// The Spotify app's screens.  The console is a Spotify Connect speaker: the phone (or any Spotify app) chooses what plays, so this
// is not a browser of the catalogue but two things, in the look of the web player's dark surfaces and bright green, with covers as
// the main picture the way the console's own Spotify app made them:
//
//   * Home: what the receiver is doing (waiting for the phone, signing in, ready), with the three steps to connect, or the track
//     that is playing or paused as a card.  A new song starting opens Now Playing by itself.
//   * Now Playing: a Spotify-styled screen with cover, title, progress, and transport controls, following the phone;
//     skip and pause on the pad go through Spotify, so the phone follows.  Leaving it returns Home while audio keeps playing.
//   * Settings (Triangle): audio quality (96, 160 or 320 kbps) and Sign out.
//
// Recreates the 2015 PS3 Spotify receiver layout: compact Spotify mark, horizontal tabs, a focused song tile, and a persistent mini-player.
#include "sp_ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <sysutil/sysutil.h>

#include "../build_config.h"
#include "meminfo.h"
#include "music_player.h"
#include "music_screen.h"
#include "plog.h"
#include "sp_art.h"
#include "sp_svc.h"
#include "thumbnail_cache.h"
#include "timing.h"
#include "ui.h"
#include "ui_card_gpu.h"
#include "ui_internal.h"
#include "ui_scale.h"
#include "ui_sfx.h"
#include "ui_wave.h"

extern u32 running;

namespace {

// ---- drawing helpers --------------------------------------------------------------------------------------------

int W(void) { return (int)display_width; }
int H(void) { return (int)display_height; }

constexpr u32 SPOTIFY_GREEN = 0x001ED760U;

// A filled rounded rectangle: one rectangle for the middle, one per row in the corners.
void rrect(int x, int y, int w, int h, int r, u32 col) {
  if (w <= 0 || h <= 0) return;
  if (r * 2 > h) r = h / 2;
  if (r * 2 > w) r = w / 2;
  if (r <= 0) { drawRect((u32)x, (u32)y, (u32)w, (u32)h, col); return; }
  for (int dy = 0; dy < r; dy++) {                        // top and bottom corner rows
    const float d = (float)(r - dy) - 0.5f;
    const int inset = r - (int)(sqrtf((float)r * (float)r - d * d) + 0.5f);
    drawRect((u32)(x + inset), (u32)(y + dy), (u32)(w - 2 * inset), 1, col);
    drawRect((u32)(x + inset), (u32)(y + h - 1 - dy), (u32)(w - 2 * inset), 1, col);
  }
  drawRect((u32)x, (u32)(y + r), (u32)w, (u32)(h - 2 * r), col);
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

void centred(int cx, int y, const char *text, float px, u32 colour, int max_w, bool bold) {
  int w = ttf_text_width(text, px, bold);
  if (w > max_w) w = max_w;
  clip_text(cx - w / 2, y, text, px, colour, max_w, bold);
}

// 4x supersampled coverage keeps the brand mark smooth on both 480p and 1080p.
void spotify_mark(int x, int y, int s) {
  if (s < 4 || s > 64) return;
  static u32 pixels[64 * 64];
  const int samples = 4;
  const float mid = (float)s * 0.5f, radius = (float)s * 0.48f;
  for (int py = 0; py < s; py++) {
    for (int px = 0; px < s; px++) {
      int covered = 0;
      for (int sy = 0; sy < samples; sy++) for (int sx = 0; sx < samples; sx++) {
        const float dx = (float)px + ((float)sx + 0.5f) / samples - mid;
        const float dy = (float)py + ((float)sy + 0.5f) / samples - mid;
        if (dx * dx + dy * dy <= radius * radius) covered++;
      }
      pixels[py * s + px] = (u32)(covered * 255 / (samples * samples)) << 24 | (SPOTIFY_GREEN & 0x00FFFFFFU);
    }
  }
  drawBitmapRectAlpha(pixels, (u32)s, 0, 0, (u32)s, (u32)s, (u32)x, (u32)y);

  const float halfw[3] = {0.34f, 0.28f, 0.22f};
  const float basey[3] = {0.36f, 0.51f, 0.65f};
  const float bend[3] = {0.075f, 0.061f, 0.047f};
  const float thick[3] = {0.048f, 0.042f, 0.036f};
  for (int py = 0; py < s; py++) for (int px = 0; px < s; px++) {
    int covered = 0;
    for (int sy = 0; sy < samples; sy++) for (int sx = 0; sx < samples; sx++) {
      const float dx = (float)px + ((float)sx + 0.5f) / samples - mid;
      const float dy = (float)py + ((float)sy + 0.5f) / samples - mid;
      if (dx * dx + dy * dy > radius * radius) continue;
      for (int arc = 0; arc < 3; arc++) {
        const float span = (float)s * halfw[arc];
        if (fabsf(dx) > span) continue;
        const float n = dx / span;
        const float line_y = (float)s * (basey[arc] + bend[arc] * n * n);
        if (fabsf(dy - line_y) <= (float)s * thick[arc] * 0.5f) { covered++; break; }
      }
    }
    pixels[py * s + px] = (u32)(covered * 255 / (samples * samples)) << 24 | 0x00121212U;
  }
  drawBitmapRectAlpha(pixels, (u32)s, 0, 0, (u32)s, (u32)s, (u32)x, (u32)y);
}

// ---- state --------------------------------------------------------------------------------------------------------------------------

enum Mode { M_HOME, M_SETTINGS, M_QUIT };
enum { S_QUALITY, S_SIGNOUT, S_BACK, S_COUNT };

struct Ui {
  Mode mode = M_HOME;
  int sel = 0;                       // settings row
  bool confirm_signout = false;
  bool signed_out = false;
  bool armed = false;                // buttons that opened a screen must be let go before presses count
  SpState prev_state = SP_OFF;
  unsigned auto_serial = 0;
  int tab = 1;                       // Browse (0) or Your Music (1)
  int queue_sel = 0;
  int queue_scroll = 0;
  int library_sel = 0;
  int library_scroll = 0;
};

bool pressed_any(void) { return btn_cur.cross || btn_cur.circle || btn_cur.square || btn_cur.triangle; }

u32 state_colour(SpState st) {
  switch (st) {
  case SP_READY: case SP_PLAYING: case SP_PAUSED: return SPOTIFY_GREEN;
  case SP_ERROR: return 0x00E5484DU;
  default: return 0x00F5A623U;      // waiting or signing in: amber
  }
}

// ---- drawing ---------------------------------------------------------------------------------------------------------------------------

void draw_spotify_backdrop(void) {
  // The original PS3 app used a restrained, nearly black surface. A single
  // fill is both more faithful and free of the 32 visible gradient bands.
  ui_rect_gpu_draw(0, 0, W(), H(), 0x00121212U, 255);
}

void draw_top(SpState st, int active_tab) {
  const int pad = (int)XMB_ITEM_PAD;
  const int y = (int)XMB_OY + UIS_H(26), mark = UIS_H(34);
  spotify_mark(pad, y, mark);
  const float tp = UIS_TF(17.0f);
  const int browse_x = pad + mark + UIS_W(28);
  const int browse_w = ttf_text_width("Browse", tp, true);
  const int music_x = browse_x + browse_w + UIS_W(32);
  const int tab_y = y + (mark - (int)tp) / 2;
  drawTTF((u32)browse_x, (u32)tab_y, "Browse", tp, active_tab == 0 ? XMB_WHITE : XMB_TEXT_DIM, true);
  drawTTF((u32)music_x, (u32)tab_y, "Your Music", tp, active_tab == 1 ? XMB_WHITE : XMB_TEXT_DIM, true);
  const int tx = active_tab == 0 ? browse_x : music_x;
  const int tw = ttf_text_width(active_tab == 0 ? "Browse" : "Your Music", tp, true);
  drawRect((u32)tx, (u32)(y + mark + UIS_H(6)), (u32)tw, UIS_H(3), SPOTIFY_GREEN);
  const float cpx = UIS_TF(15.0f);
  const int label_w = ttf_text_width("PS3", cpx, true);
  const int rx = W() - pad - label_w - UIS_W(24);
  const int d = UIS_H(9);
  rrect(rx, y + (mark - d) / 2, d, d, d / 2, state_colour(st));
  drawTTF((u32)(rx + d + UIS_W(10)), (u32)(y + (mark - (int)cpx) / 2), "PS3", cpx, XMB_TEXT_DIM, true);
}

void draw_home(Ui &ui, SpState st) {
  draw_top(st, ui.tab);
  const int pad = (int)XMB_ITEM_PAD;
  const int cw = W() - 2 * pad;
  SpTrack t;
  const bool have = sp_svc_track(&t) && st != SP_PAIRING && st != SP_SIGNING_IN && st != SP_ERROR;
  const bool browse = ui.tab == 0;
  const int heading_y = (int)XMB_OY + UIS_H(112);
  drawTTF((u32)pad, (u32)heading_y, browse ? "BROWSE" : "SONGS",
          UIS_TF(19.0f), XMB_WHITE, true);

  if (browse) {
    drawTTF((u32)pad, (u32)(heading_y + UIS_H(62)), "Spotify queue",
            UIS_TF(28.0f), XMB_WHITE, true);
    const unsigned queue_count = sp_svc_queue_count();
    const int current = sp_svc_queue_current();
    const int row_y = heading_y + UIS_H(112), row_h = UIS_H(58);
    const int visible = 6;
    if (!queue_count) {
      clip_text(pad, row_y, "Play a playlist or album on your phone, then connect to PS3.",
                UIS_TF(17.0f), XMB_TEXT_DIM, cw, false);
    } else {
      if (ui.queue_sel < 0) ui.queue_sel = 0;
      if ((unsigned)ui.queue_sel >= queue_count) ui.queue_sel = (int)queue_count - 1;
      if (ui.queue_scroll > ui.queue_sel) ui.queue_scroll = ui.queue_sel;
      if (ui.queue_sel >= ui.queue_scroll + visible) ui.queue_scroll = ui.queue_sel - visible + 1;
      for (int i = 0; i < visible && (unsigned)(ui.queue_scroll + i) < queue_count; i++) {
        const int index = ui.queue_scroll + i, ry = row_y + i * row_h;
        const bool selected = index == ui.queue_sel;
        if (selected) ui_rect_gpu_draw(pad - UIS_W(10), ry - UIS_H(4), cw + UIS_W(20), row_h - UIS_H(3), 0x002E3338U, 230);
        SpTrack row;
        const bool named = sp_svc_queue_track((unsigned)index, &row);
        char fallback[40];
        snprintf(fallback, sizeof fallback, "Queued song %02d", index + 1);
        clip_text(pad, ry + UIS_H(5), named ? row.title : fallback, UIS_TF(17.0f),
                  selected ? XMB_WHITE : XMB_TEXT, cw - UIS_W(170), true);
        if (named && row.artist[0])
          clip_text(pad, ry + UIS_H(31), row.artist, UIS_TF(13.0f), XMB_TEXT_DIM, cw - UIS_W(170), false);
        if (index == current) drawTTF((u32)(W() - pad - UIS_W(112)), (u32)(ry + UIS_H(12)), "NOW PLAYING",
                                      UIS_TF(11.0f), SPOTIFY_GREEN, true);
        else if (selected) drawTTF((u32)(W() - pad - UIS_W(90)), (u32)(ry + UIS_H(12)), "PLAY",
                                   UIS_TF(12.0f), SPOTIFY_GREEN, true);
      }
      clip_text(pad, row_y + visible * row_h + UIS_H(5),
                "This is the queue sent by Spotify Connect. Pick a row to play it on PS3.",
                UIS_TF(13.0f), XMB_TEXT_DIM, cw, false);
    }
  } else {
    sp_svc_library_request((unsigned)(ui.library_sel < 0 ? 0 : ui.library_sel));
    const unsigned total = sp_svc_library_total();
    const unsigned page_offset = sp_svc_library_offset();
    const int row_y = heading_y + UIS_H(88), row_h = UIS_H(58), visible = 6;
    drawTTF((u32)pad, (u32)(heading_y + UIS_H(42)), "Liked Songs",
            UIS_TF(28.0f), XMB_WHITE, true);
    if (sp_svc_library_loading()) {
      clip_text(pad, row_y, "Loading your saved songs...", UIS_TF(17.0f), XMB_TEXT_DIM, cw, false);
    } else if (!total) {
      const char* status = sp_svc_library_status_text();
      clip_text(pad, row_y, status[0] ? status : "No liked songs found",
                UIS_TF(17.0f), XMB_TEXT_DIM, cw, false);
    } else {
      char count_text[64];
      snprintf(count_text, sizeof count_text, "%u songs saved", total);
      drawTTF((u32)pad, (u32)(heading_y + UIS_H(76)), count_text, UIS_TF(14.0f), XMB_TEXT_DIM, false);
      if (ui.library_sel < 0) ui.library_sel = 0;
      if ((unsigned)ui.library_sel >= total) ui.library_sel = (int)total - 1;
      if (ui.library_scroll > ui.library_sel) ui.library_scroll = ui.library_sel;
      if (ui.library_sel >= ui.library_scroll + visible) ui.library_scroll = ui.library_sel - visible + 1;
      if ((unsigned)ui.library_scroll < page_offset) ui.library_scroll = (int)page_offset;
      for (int i = 0; i < visible && (unsigned)(ui.library_scroll + i) < total; i++) {
        const int index = ui.library_scroll + i, ry = row_y + i * row_h;
        const bool selected = index == ui.library_sel;
        if (selected) ui_rect_gpu_draw(pad - UIS_W(10), ry - UIS_H(4), cw + UIS_W(20), row_h - UIS_H(3), 0x002E3338U, 230);
        SpTrack row;
        if (sp_svc_library_track((unsigned)index, &row)) {
          clip_text(pad, ry + UIS_H(5), row.title, UIS_TF(17.0f),
                    selected ? XMB_WHITE : XMB_TEXT, cw - UIS_W(170), true);
          clip_text(pad, ry + UIS_H(31), row.artist, UIS_TF(13.0f), XMB_TEXT_DIM, cw - UIS_W(170), false);
        } else {
          clip_text(pad, ry + UIS_H(12), "Loading...", UIS_TF(16.0f), XMB_TEXT_DIM, cw - UIS_W(170), false);
        }
        if (selected) drawTTF((u32)(W() - pad - UIS_W(90)), (u32)(ry + UIS_H(12)), "PLAY",
                             UIS_TF(12.0f), SPOTIFY_GREEN, true);
      }
      clip_text(pad, row_y + visible * row_h + UIS_H(4), sp_svc_library_status_text(),
                UIS_TF(13.0f), XMB_TEXT_DIM, cw, false);
    }
  }

  // Browse and Your Music own this content area. Connection guidance stays in the
  // empty queue/library messages and the persistent player rail below.

  // Persistent Now Playing rail, matching the original app's compact player.
  const int by = H() - UIS_H(132), bh = UIS_H(82);
  ui_rect_gpu_draw(pad - UIS_W(12), by - UIS_H(7), cw + UIS_W(24), bh + UIS_H(8), 0x00252A30U, 222);
  ui_rect_gpu_draw(pad - UIS_W(12), by - UIS_H(7), cw + UIS_W(24), UIS_H(1), 0x0096A0A9U, 112);
  if (have) {
    const int art = UIS_H(48), ay = by + (bh - art) / 2;
    char key[80];
    if (!sp_art_key(t.image, key, sizeof key) || !xmb_cpu_blit_thumb(key, pad, ay, art, art)) {
      drawRect((u32)pad, (u32)ay, (u32)art, (u32)art, XMB_PANEL_HI);
      drawIcon((u32)(pad + UIS_H(12)), (u32)(ay + UIS_H(12)), ICON_MUSIC, UIS_TF(24.0f), XMB_TEXT_FAINT);
    }
    const int tx = pad + art + UIS_W(14);
    clip_text(tx, by + UIS_H(12), t.title, UIS_TF(15.0f), XMB_WHITE, UIS_W(360), true);
    clip_text(tx, by + UIS_H(38), t.artist, UIS_TF(12.0f), XMB_TEXT_DIM, UIS_W(360), false);
    drawIcon((u32)(W() - pad - UIS_W(30)), (u32)(by + UIS_H(22)),
             st == SP_PLAYING ? ICON_PAUSE : ICON_PLAY, UIS_TF(24.0f), XMB_WHITE);
    const unsigned dur = t.duration_ms ? t.duration_ms : 1, pos = sp_svc_position_ms();
    const int pw = W() - 2 * pad, py = by + bh - UIS_H(3);
    drawRect((u32)pad, (u32)py, (u32)pw, UIS_H(2), XMB_TRACK);
    drawRect((u32)pad, (u32)py, (u32)((uint64_t)pw * (pos > dur ? dur : pos) / dur), UIS_H(2), XMB_ACCENT);
  } else {
    // Nothing on this console yet: say what the receiver is doing (signing in, ready, playing elsewhere).
    const int d = UIS_H(9);
    rrect(pad, by + UIS_H(30), d, d, d / 2, state_colour(st));
    clip_text(pad + d + UIS_W(12), by + UIS_H(24), sp_svc_status_text(), UIS_TF(15.0f), XMB_TEXT_DIM,
              cw - d - UIS_W(12), false);
  }

  const bool can_pick = (ui.tab == 0 && sp_svc_queue_count()) || (ui.tab == 1 && sp_svc_library_total());
  Hint h[7];
  int n = 0;
  if (can_pick) { h[n].glyph = 'X'; h[n].label = "Play"; n++; }
  else if (have) { h[n].glyph = 'X'; h[n].label = "Now Playing"; n++; }
  if (can_pick && have) { h[n].glyph = 'A'; h[n].label = "Now Playing"; n++; }
  h[n].glyph = 'D'; h[n].label = can_pick ? "Choose song / Tabs" : "Tabs"; n++;
  if (have) { h[n].glyph = 'l'; h[n].label = ""; n++; h[n].glyph = 'r'; h[n].label = "Skip track"; n++; }
  h[n].glyph = 'T'; h[n].label = "Settings"; n++;
  h[n].glyph = 'C'; h[n].label = "Quit"; n++;
  draw_hints_bar(h, n);
}

void settings_text(int r, char *title, int tc, char *value, int vc, char *sub, int sc, const Ui &ui) {
  value[0] = '\0';
  switch (r) {
  case S_QUALITY: {
    const int q = sp_svc_quality();
    snprintf(title, (size_t)tc, "Audio quality");
    snprintf(value, (size_t)vc, "%s", q == 96 ? "Normal  96 kbps" : q == 320 ? "Very high  320 kbps" : "High  160 kbps");
    snprintf(sub, (size_t)sc, "Left and right change it, from the next song. Very high needs Spotify Premium.");
    break;
  }
  case S_SIGNOUT:
    snprintf(title, (size_t)tc, "Sign out");
    if (ui.confirm_signout) snprintf(sub, (size_t)sc, "Press X again to forget the Spotify account on this console.");
    else if (ui.signed_out) snprintf(sub, (size_t)sc, "Forgotten. Restart the app, then choose PS3 in Spotify to sign in again.");
    else snprintf(sub, (size_t)sc, "Forget the saved account; you connect again from the phone.");
    break;
  default:
    snprintf(title, (size_t)tc, "Back");
    snprintf(sub, (size_t)sc, "Return to the main screen.");
  }
}

void draw_settings(const Ui &ui, SpState st) {
  draw_top(st, ui.tab);
  const int cw = (W() - 2 * (int)XMB_ITEM_PAD) < UIS_W(860) ? (W() - 2 * (int)XMB_ITEM_PAD) : UIS_W(860);
  const int cx = (W() - cw) / 2;
  centred(W() / 2, H() * 21 / 100, "Settings", UIS_TF(36.0f), XMB_WHITE, cw, true);
  const int rh = UIS_H(78), gap = UIS_H(14), y0 = H() * 34 / 100;
  for (int r = 0; r < S_COUNT; r++) {
    const int y = y0 + r * (rh + gap);
    const bool s = r == ui.sel;
    rrect(cx, y, cw, rh, UIS_H(8), s ? XMB_PANEL_HI : XMB_PANEL);
    if (s) rrect(cx, y + UIS_H(14), UIS_W(4), rh - UIS_H(28), UIS_W(2), XMB_ACCENT);
    char title[48], value[48], sub[160];
    settings_text(r, title, sizeof title, value, sizeof value, sub, sizeof sub, ui);
    const int tx = cx + UIS_W(28);
    clip_text(tx, y + UIS_H(12), title, UIS_TF(22.0f), s ? XMB_WHITE : XMB_TEXT, cw - UIS_W(300), true);
    clip_text(tx, y + UIS_H(46), sub, UIS_TF(14.0f), XMB_TEXT_DIM, cw - UIS_W(60), false);
    if (value[0]) {
      const float vp = UIS_TF(19.0f);
      const int vw = ttf_text_width(value, vp, true);
      drawTTF((u32)(cx + cw - UIS_W(28) - vw), (u32)(y + UIS_H(14)), value, vp, XMB_ACCENT, true);
    }
  }
  Hint h[3];
  int n = 0;
  h[n].glyph = 'X'; h[n].label = ui.sel == S_QUALITY ? "Change" : "Select"; n++;
  h[n].glyph = 'D'; h[n].label = "Move"; n++;
  h[n].glyph = 'C'; h[n].label = "Back"; n++;
  draw_hints_bar(h, n);
}

void draw_quit(SpState st, int tab) {
  draw_top(st, tab);
  const int bw = UIS_W(620), bh = UIS_H(220), bx = (W() - bw) / 2, by = (H() - bh) / 2;
  rrect(bx, by, bw, bh, UIS_H(10), XMB_PANEL);
  centred(W() / 2, by + UIS_H(40), "Quit Spotify?", UIS_TF(30.0f), XMB_WHITE, bw - UIS_W(40), true);
  centred(W() / 2, by + UIS_H(96), "Music stops, and PS3 leaves the device list in Spotify.", UIS_TF(16.0f), XMB_TEXT_DIM, bw - UIS_W(40), false);
  rrect(bx + UIS_W(60), by + bh - UIS_H(74), bw - UIS_W(120), UIS_H(46), UIS_H(23), XMB_ACCENT);
  centred(W() / 2, by + bh - UIS_H(74) + UIS_H(11), "Press X to quit, O to stay", UIS_TF(19.0f), 0x00000000U, bw - UIS_W(160), true);
}

// ---- actions ---------------------------------------------------------------------------------------------------------------------------

void open_now_playing(Ui &ui) {
  plog("spotify: opening Now Playing");
  flip();                                  // finish the frame in flight (the screen expects the caller to have flipped)
  init_btns();
  music_screen_open_spotify();
  ui.armed = false;
  ui.auto_serial = sp_svc_track_serial();      // coming back must not open it again for the same song
  ui.prev_state = sp_svc_state();
}

void change_quality(int dir) {
  static const int steps[3] = {96, 160, 320};
  int i = 1;
  for (int k = 0; k < 3; k++) if (steps[k] == sp_svc_quality()) i = k;
  i += dir;
  if (i < 0) i = 0;
  if (i > 2) i = 2;
  if (steps[i] != sp_svc_quality()) { sp_svc_set_quality(steps[i]); ui_sfx_play(SFX_CURSOR); }
}

}  // namespace

void sp_ui_run(void) {
  sp_svc_start("PS3");
  music_start_spotify();                 // the speaker: runs until the app ends, whatever screen is up
  Ui ui;
  bool first = true;
  while (running) {
    waitflip();
    sysUtilCheckCallback();
    if (!running) break;                 // honour XMB's Quit Game before rendering another frame
    clearScreen(XMB_BG);
    draw_spotify_backdrop();
    rsxSync();
    thumb_cache_tick();
    poll_buttons();
    if (first) {
      char b[160];
      snprintf(b, sizeof b, "spotify: screen up, display %dx%d pad=%d ox=%d oy=%d W100=%d H100=%d TF100=%d", W(), H(), (int)XMB_ITEM_PAD,
               (int)XMB_OX, (int)XMB_OY, UIS_W(100), UIS_H(100), (int)UIS_TF(100.0f));
      plog(b);
      first = false;
    }
    if (!ui.armed && !pressed_any()) ui.armed = true;
    const SpState st = sp_svc_state();

    if (ui.armed) {
      if (ui.mode == M_HOME) {
        SpTrack t;
        const bool have = sp_svc_track(&t) && st != SP_PAIRING && st != SP_SIGNING_IN && st != SP_ERROR;
        // Keep the library view in place as playback changes; users choose Now Playing with X.
        if (ui.tab == 0 && sp_svc_queue_count()) {
          const int qn = (int)sp_svc_queue_count();
          if (BTN_REPEAT(up) && ui.queue_sel > 0) { ui.queue_sel--; ui_sfx_play(SFX_CURSOR); }
          if (BTN_REPEAT(down) && ui.queue_sel < qn - 1) { ui.queue_sel++; ui_sfx_play(SFX_CURSOR); }
        } else if (ui.tab == 1) {
          sp_svc_library_request((unsigned)(ui.library_sel < 0 ? 0 : ui.library_sel));
          const unsigned total = sp_svc_library_total();
          if (BTN_REPEAT(up) && ui.library_sel > 0) { ui.library_sel--; ui_sfx_play(SFX_CURSOR); }
          if (BTN_REPEAT(down) && total && (unsigned)(ui.library_sel + 1) < total) { ui.library_sel++; ui_sfx_play(SFX_CURSOR); }
          if (ui.library_sel < ui.library_scroll) ui.library_scroll = ui.library_sel;
          if (ui.library_sel >= ui.library_scroll + 6) ui.library_scroll = ui.library_sel - 5;
          if (ui.library_sel >= 0) sp_svc_library_request((unsigned)ui.library_sel);
        }
        if (BTN_PRESSED(left) || BTN_PRESSED(right)) {
          ui.tab = 1 - ui.tab;
          if (ui.tab == 1) sp_svc_library_request((unsigned)(ui.library_sel < 0 ? 0 : ui.library_sel));
          ui_sfx_play(SFX_CURSOR);
        }
        if (BTN_PRESSED(cross) && ui.tab == 1 && sp_svc_library_total()) {
          ui_sfx_play(SFX_DECIDE);
          if (!sp_svc_library_play((unsigned)ui.library_sel))
            sp_svc_library_request((unsigned)ui.library_sel);
        } else if (BTN_PRESSED(cross) && ui.tab == 0 && sp_svc_queue_count()) {
          ui_sfx_play(SFX_DECIDE);
          if (sp_svc_play_queue_track((unsigned)ui.queue_sel)) ui.auto_serial = sp_svc_track_serial();
        } else if (BTN_PRESSED(cross) && have) { ui_sfx_play(SFX_DECIDE); open_now_playing(ui); continue; }
        if (BTN_PRESSED(start) && have) { ui_sfx_play(SFX_DECIDE); open_now_playing(ui); continue; }
        if (BTN_PRESSED(l1) && have) { ui_sfx_play(SFX_CURSOR); sp_svc_prev(); }
        if (BTN_PRESSED(r1) && have) { ui_sfx_play(SFX_CURSOR); sp_svc_next(); }
        if (BTN_PRESSED(triangle)) { ui_sfx_play(SFX_DECIDE); ui.mode = M_SETTINGS; ui.sel = 0; ui.confirm_signout = false; }
        else if (BTN_PRESSED(circle)) { ui_sfx_play(SFX_CANCEL); ui.mode = M_QUIT; }
      } else if (ui.mode == M_SETTINGS) {
        const int was = ui.sel;
        if (BTN_REPEAT(up) && ui.sel > 0) ui.sel--;
        if (BTN_REPEAT(down) && ui.sel < S_COUNT - 1) ui.sel++;
        if (ui.sel != was) { ui_sfx_play(SFX_CURSOR); ui.confirm_signout = false; }
        if (ui.sel == S_QUALITY) {
          if (BTN_PRESSED(left)) change_quality(-1);
          if (BTN_PRESSED(right) || BTN_PRESSED(cross)) change_quality(+1);
        } else if (BTN_PRESSED(cross)) {
          ui_sfx_play(SFX_DECIDE);
          if (ui.sel == S_SIGNOUT) {
            if (!ui.confirm_signout) ui.confirm_signout = true;
            else { ui.confirm_signout = false; sp_svc_forget(); ui.signed_out = true; }
          } else {
            ui.mode = M_HOME;
          }
        }
        if (BTN_PRESSED(circle) || BTN_PRESSED(triangle)) { ui_sfx_play(SFX_CANCEL); ui.mode = M_HOME; }
      } else {
        if (BTN_PRESSED(cross)) { ui_sfx_play(SFX_DECIDE); flip(); break; }
        if (BTN_PRESSED(circle)) { ui_sfx_play(SFX_CANCEL); ui.mode = M_HOME; }
      }
    }
    ui.prev_state = st;

    if (ui.mode == M_HOME) draw_home(ui, st);
    else if (ui.mode == M_SETTINGS) draw_settings(ui, st);
    else draw_quit(st, ui.tab);
    flip();
  }
  music_stop();
  music_join_stale();                    // the feed thread, left finishing by music_stop()
  sp_svc_stop();
}
