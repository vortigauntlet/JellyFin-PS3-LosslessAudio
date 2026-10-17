// The Spotify app's screen: what the receiver is waiting for or playing, and the pad controls.  A plain screen on purpose: the
// app's own UI is a separate piece of work, and this one is what proves pairing and playback work.
//
// The audio itself goes through the music engine (music_start_spotify), so it plays, resampled to 48 kHz, through the
// console's audio output with the same visualisers and volume as the other music.
#include "sp_ui.h"

#include <stdio.h>
#include <string.h>

#include "../build_config.h"
#include "meminfo.h"
#include "music_player.h"
#include "plog.h"
#include "sp_svc.h"
#include "timing.h"
#include "ui.h"
#include "ui_internal.h"
#include "ui_scale.h"
#include "ui_wave.h"

extern u32 running;

namespace {

void fmt_time(char* out, size_t n, unsigned ms) {
  const unsigned s = ms / 1000;
  snprintf(out, n, "%u:%02u", s / 60, s % 60);
}

}  // namespace

void sp_ui_run(void) {
  sp_svc_start("PS3");
  music_start_spotify();                 // plays what the receiver decodes; the screen only shows it
  bool first = true;
  while (running) {
    waitflip();
    clearScreen(XMB_BG);
    wave_draw();
    rsxSync();
    poll_buttons();
    if (first) { plog("spotify: screen up"); first = false; }

    if (BTN_PRESSED(circle)) break;
    if (BTN_PRESSED(cross)) sp_svc_set_pause(sp_svc_state() != SP_PAUSED);
    if (BTN_PRESSED(r1)) sp_svc_next();
    if (BTN_PRESSED(l1)) sp_svc_prev();

    const int x = (int)XMB_ITEM_PAD;
    int y = UIS_H(60);
    drawTTF(x, y, "Spotify", UIS_TF(26), XMB_TEXT, true);
    y += UIS_H(46);
    drawTTF(x, y, sp_svc_status_text(), UIS_TF(18), XMB_TEXT_DIM);
    y += UIS_H(40);

    SpTrack t;
    if (sp_svc_track(&t)) {
      drawTTF(x, y, t.title, UIS_TF(24), XMB_TEXT, true);
      y += UIS_H(34);
      drawTTF(x, y, t.artist, UIS_TF(18), XMB_TEXT);
      y += UIS_H(28);
      drawTTF(x, y, t.album, UIS_TF(16), XMB_TEXT_DIM);
      y += UIS_H(34);
      char a[16], b[16], line[48];
      fmt_time(a, sizeof a, sp_svc_position_ms());
      fmt_time(b, sizeof b, t.duration_ms);
      snprintf(line, sizeof line, "%s / %s%s", a, b, sp_svc_state() == SP_PAUSED ? "   (paused)" : "");
      drawTTF(x, y, line, UIS_TF(18), XMB_TEXT);
      y += UIS_H(40);
    }

    unsigned frames_in = 0, tracks = 0;
    sp_svc_counters(&frames_in, &tracks);
    char dbg[96];
    snprintf(dbg, sizeof dbg, "audio in: %u s   tracks: %u   free: %u KB", frames_in / 44100, tracks, meminfo_avail_kb());
    drawTTF(x, UIS_H(640), dbg, UIS_TF(14), XMB_TEXT_DIM);
    drawTTF(x, UIS_H(664), "X pause   L1 / R1 previous / next   O exit", UIS_TF(14), XMB_TEXT_DIM);
    flip();
  }
  music_stop();
  sp_svc_stop();
}
