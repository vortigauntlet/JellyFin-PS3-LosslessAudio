// The Spotify app's screens.  The console is a Spotify Connect speaker: the phone (or any Spotify app) chooses what plays, so this
// is not a browser of the catalogue but two things, in the look of the web player's dark surfaces and bright green, with covers as
// the main picture the way the console's own Spotify app made them:
//
//   * Home: what the receiver is doing (waiting for the phone, signing in, ready), with the three steps to connect, or the track
//     that is playing or paused as a card.  A new song starting opens Now Playing by itself.
//   * Now Playing: the music engine's own screen (cover, title, progress, the visualisers, volume), following the phone;
//     skip and pause on the pad go through Spotify, so the phone follows.  Leaving it pauses, so the phone shows paused.
//   * Settings (Triangle): audio quality (96, 160 or 320 kbps) and Sign out.
//
// Colours come from the theme (ui/theme.cpp, "Spotify"): surfaces #000 / #141414 / #1f1f1f, text #fff, accent #1ed760 (from the
// web player's stylesheet).  Text is the app's own faces; the mark is an original equaliser, not Spotify's logo.
#include "sp_ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../build_config.h"
#include "meminfo.h"
#include "music_screen.h"
#include "plog.h"
#include "sp_art.h"
#include "sp_svc.h"
#include "thumbnail_cache.h"
#include "timing.h"
#include "ui.h"
#include "ui_internal.h"
#include "ui_scale.h"
#include "ui_sfx.h"
#include "ui_wave.h"

extern u32 running;

namespace {

// ---- drawing helpers --------------------------------------------------------------------------------------------

int W(void) { return (int)display_width; }
int H(void) { return (int)display_height; }

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

// The mark: five rounded bars, an original equaliser.  Animated while music plays.
void equaliser(int x, int y, int s, bool alive) {
  static const float base[5] = {0.46f, 0.78f, 1.0f, 0.64f, 0.36f};
  const float t = (float)(timing_get_us() % 4000000ULL) / 4000000.0f * 6.2831853f;
  const int bw = s / 8 > 2 ? s / 8 : 2, gap = (s - 5 * bw) / 4 > 1 ? (s - 5 * bw) / 4 : 1;
  for (int i = 0; i < 5; i++) {
    float f = base[i];
    if (alive) f *= 0.72f + 0.28f * sinf(t * (2.0f + (float)i * 0.45f) + (float)i * 1.3f);
    int bh = (int)((float)s * f);
    if (bh < bw) bh = bw;
    rrect(x + i * (bw + gap), y + s - bh, bw, bh, bw / 2, XMB_ACCENT);
  }
}

void fmt_time(char *out, size_t n, unsigned ms) {
  const unsigned s = ms / 1000;
  snprintf(out, n, "%u:%02u", s / 60, s % 60);
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
  uint64_t no_open_until_us = 0;     // after leaving Now Playing: the state needs a moment to say paused
};

bool pressed_any(void) { return btn_cur.cross || btn_cur.circle || btn_cur.square || btn_cur.triangle; }

u32 state_colour(SpState st) {
  switch (st) {
  case SP_READY: case SP_PLAYING: case SP_PAUSED: return XMB_ACCENT;
  case SP_ERROR: return 0x00E5484DU;
  default: return 0x00F5A623U;      // waiting or signing in: amber
  }
}

const char *state_title(SpState st) {
  switch (st) {
  case SP_PAIRING:    return "Connect from Spotify";
  case SP_SIGNING_IN: return "Signing in";
  case SP_READY:      return "Ready to play";
  case SP_PLAYING:    return "Playing";
  case SP_PAUSED:     return "Paused";
  case SP_ERROR:      return "Not connected";
  default:            return "Starting";
  }
}

// ---- drawing ---------------------------------------------------------------------------------------------------------------------------

void draw_top(SpState st) {
  const int pad = (int)XMB_ITEM_PAD;
  const int y = (int)XMB_OY + UIS_H(26), s = UIS_H(34);
  equaliser(pad, y, s, st == SP_PLAYING);
  const float px = UIS_TF(30.0f);
  drawTTF((u32)(pad + s + UIS_W(14)), (u32)(y + (s - (int)px) / 2 - UIS_H(2)), "Spotify", px, XMB_TEXT, true);

  // the device chip: a dot for the state, and the name the phone shows
  const float cpx = UIS_TF(16.0f);
  const int ch = UIS_H(34), cw = ttf_text_width("PS3", cpx, true) + UIS_W(58);
  const int cx = W() - pad - cw;
  rrect(cx, y, cw, ch, ch / 2, XMB_PANEL_HI);
  const int d = UIS_H(12);
  rrect(cx + UIS_W(18), y + (ch - d) / 2, d, d, d / 2, state_colour(st));
  drawTTF((u32)(cx + UIS_W(18) + d + UIS_W(12)), (u32)(y + (ch - (int)cpx) / 2 - UIS_H(1)), "PS3", cpx, XMB_TEXT, true);
}

void draw_step(int x, int y, int w, int h, int n, const char *text, const char *sub) {
  rrect(x, y, w, h, UIS_H(8), XMB_PANEL);
  const int c = h - UIS_H(26);
  rrect(x + UIS_W(18), y + (h - c) / 2, c, c, c / 2, XMB_ACCENT);
  char d[4];
  snprintf(d, sizeof d, "%d", n);
  const float px = UIS_TF(20.0f);
  drawTTF((u32)(x + UIS_W(18) + (c - ttf_text_width(d, px, true)) / 2), (u32)(y + (h - (int)px) / 2 - UIS_H(1)), d, px, 0x00000000U, true);
  const int tx = x + UIS_W(18) + c + UIS_W(22);
  clip_text(tx, y + UIS_H(10), text, UIS_TF(21.0f), XMB_TEXT, w - (tx - x) - UIS_W(20), true);
  if (sub && sub[0]) clip_text(tx, y + UIS_H(40), sub, UIS_TF(15.0f), XMB_TEXT_DIM, w - (tx - x) - UIS_W(20), false);
}

void draw_card(int x, int y, int w, int h, const SpTrack &t, SpState st, bool focused) {
  rrect(x, y, w, h, UIS_H(8), XMB_PANEL);
  if (focused) {                                           // the one focusable thing: an accent outline
    const int b = UIS_H(2);
    drawRect((u32)x, (u32)y, (u32)w, (u32)b, XMB_ACCENT);
    drawRect((u32)x, (u32)(y + h - b), (u32)w, (u32)b, XMB_ACCENT);
    drawRect((u32)x, (u32)y, (u32)b, (u32)h, XMB_ACCENT);
    drawRect((u32)(x + w - b), (u32)y, (u32)b, (u32)h, XMB_ACCENT);
  }
  const int pad = UIS_H(20), s = h - 2 * pad;
  char key[80];
  bool drawn = false;
  if (sp_art_key(t.image, key, sizeof key)) drawn = xmb_cpu_blit_thumb(key, x + pad, y + pad, s, s);
  if (!drawn) {
    drawRect((u32)(x + pad), (u32)(y + pad), (u32)s, (u32)s, XMB_PANEL_HI);
    drawIcon((u32)(x + pad + (s - UIS_H(40)) / 2), (u32)(y + pad + (s - UIS_H(40)) / 2), ICON_MUSIC, UIS_TF(40.0f), XMB_TEXT_FAINT);
  }
  const int tx = x + pad + s + UIS_W(26), tw = x + w - tx - UIS_W(24);
  clip_text(tx, y + pad, t.title, UIS_TF(28.0f), XMB_WHITE, tw, true);
  clip_text(tx, y + pad + UIS_H(40), t.artist, UIS_TF(19.0f), XMB_TEXT, tw, false);
  clip_text(tx, y + pad + UIS_H(70), t.album, UIS_TF(15.0f), XMB_TEXT_DIM, tw, false);

  // progress: the position the phone reports against the track's length
  const int by = y + h - pad - UIS_H(8);
  const unsigned pos = sp_svc_position_ms(), dur = t.duration_ms ? t.duration_ms : 1;
  int fill = (int)((uint64_t)tw * (pos > dur ? dur : pos) / dur);
  rrect(tx, by, tw, UIS_H(5), UIS_H(3), XMB_TRACK);
  if (fill > 0) rrect(tx, by, fill, UIS_H(5), UIS_H(3), st == SP_PLAYING ? XMB_ACCENT : XMB_TEXT_DIM);
  char a[16], b[16], line[48];
  fmt_time(a, sizeof a, pos);
  fmt_time(b, sizeof b, t.duration_ms);
  snprintf(line, sizeof line, "%s / %s", a, b);
  drawTTF((u32)tx, (u32)(by - UIS_H(24)), line, UIS_TF(14.0f), XMB_TEXT_DIM);
  if (st == SP_PAUSED) {
    const char *p = "PAUSED";
    drawTTF((u32)(tx + tw - ttf_text_width(p, UIS_TF(13.0f), true)), (u32)(by - UIS_H(23)), p, UIS_TF(13.0f), XMB_ACCENT, true);
  }
}

void draw_home(Ui &ui, SpState st) {
  draw_top(st);
  const int cw = (W() - 2 * (int)XMB_ITEM_PAD) < UIS_W(860) ? (W() - 2 * (int)XMB_ITEM_PAD) : UIS_W(860);
  const int cx = (W() - cw) / 2, mid = W() / 2;
  SpTrack t;
  const bool have = sp_svc_track(&t) && st != SP_PAIRING && st != SP_SIGNING_IN && st != SP_ERROR;

  const int ty = H() * (have ? 20 : 25) / 100;
  centred(mid, ty, state_title(st), UIS_TF(40.0f), XMB_WHITE, cw, true);
  // a few dots that walk while something is being waited for
  const char *status = sp_svc_status_text();
  centred(mid, ty + UIS_H(60), status, UIS_TF(18.0f), XMB_TEXT_DIM, cw, false);
  if (st == SP_PAIRING || st == SP_SIGNING_IN) {
    const int n = (int)((timing_get_us() / 350000ULL) % 4), d = UIS_H(9), gap = UIS_W(16);
    const int sx = mid - (3 * gap + d) / 2, sy = ty + UIS_H(102);
    for (int i = 0; i < 4; i++) rrect(sx + i * gap, sy, d, d, d / 2, i == n ? XMB_ACCENT : XMB_PANEL_HI);
  }

  if (have) {
    const int ch = UIS_H(200);
    const int cy = H() * 38 / 100;
    draw_card(cx, cy, cw, ch, t, st, true);
    centred(mid, cy + ch + UIS_H(26), "In the Spotify app, tap the device icon to move the music to another device",
            UIS_TF(15.0f), XMB_TEXT_FAINT, cw, false);
  } else {
    const int rh = UIS_H(72), gap = UIS_H(14), y0 = H() * 43 / 100;
    draw_step(cx, y0, cw, rh, 1, "Open Spotify on your phone or computer", "Use the same account you want to listen with");
    draw_step(cx, y0 + rh + gap, cw, rh, 2, "Tap the device icon", "It is called Connect to a device");
    draw_step(cx, y0 + 2 * (rh + gap), cw, rh, 3, "Choose PS3", "Your music starts playing on the console right away");
  }

  Hint h[4];
  int n = 0;
  if (have) { h[n].glyph = 'X'; h[n].label = "Now Playing"; n++; }
  h[n].glyph = 'T'; h[n].label = "Settings"; n++;
  h[n].glyph = 'S'; h[n].label = "Visualizer"; n++;
  h[n].glyph = 'C'; h[n].label = "Quit"; n++;
  draw_hints_bar(h, n);
  (void)ui;
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
  draw_top(st);
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

void draw_quit(SpState st) {
  draw_top(st);
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
  ui.no_open_until_us = timing_get_us() + 2500000ULL;
  SpTrack t;
  ui.auto_serial = sp_svc_track(&t) ? sp_svc_track_serial() : 0;
  ui.prev_state = SP_PAUSED;
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
  Ui ui;
  bool first = true;
  while (running) {
    waitflip();
    clearScreen(XMB_BG);
    wave_draw();
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
        // a song started (or resumed) on the phone: show it
        const bool started = st == SP_PLAYING && (ui.prev_state != SP_PLAYING || sp_svc_track_serial() != ui.auto_serial);
        if (have && started && timing_get_us() >= ui.no_open_until_us) { open_now_playing(ui); continue; }
        if (BTN_PRESSED(cross) && have) { ui_sfx_play(SFX_DECIDE); open_now_playing(ui); continue; }
        if (BTN_PRESSED(triangle)) { ui_sfx_play(SFX_DECIDE); ui.mode = M_SETTINGS; ui.sel = 0; ui.confirm_signout = false; }
        else if (BTN_PRESSED(square)) { wave_vis_cycle(); ui_sfx_play(SFX_OPTION); }
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
    else draw_quit(st);
    flip();
  }
  sp_svc_stop();
}
