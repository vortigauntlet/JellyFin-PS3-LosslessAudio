// HUD rendering — paused title, dim strip, transport controls, seek bar,
// time labels, AUDIO/CC buttons, and the track-selection popup menu.
//
// The HUD is NOT drawn into the framebuffer.  It is composed with the CPU
// into a main-RAM ARGB staging buffer only when its content changes (the
// time string ticks once a second; everything else is input-driven),
// uploaded to an RSX texture, and drawn every frame as a single
// alpha-blended GPU quad after the video (rsx_draw_hud_overlay).  Per-frame
// cost is a few FIFO words — no rsxSync stall, no CPU writes into VRAM —
// so a visible seek bar can no longer push the display loop past one
// vblank (the "HUD splits the FPS with the video" lag).

#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <math.h>

#include <ppu-types.h>
#include <rsx/rsx.h>

#include "player_hud.h"
#include "player_hud_internal.h"
#include "player_rsx.h"
#include "ui.h"
#include "ui_visuals.h"
#include "trickplay.h"
#include "rsxutil.h"
#include "plog.h"
#include "timing.h"   // timing_get_us(): the channel banner expires
#include "audio.h"   // audio_get_volume() for the volume slider
#include "audio_bitstream.h"   // audio_passthrough_active(): the receiver owns the volume
#include "../core/player_picture.h"   // picture_get(): the picture-shape chip

// -------------------------------------------------------
// Overlay buffers + compose-on-change state
// -------------------------------------------------------

static u32 *s_ovl_stage   = NULL;   // main-RAM compose target (straight alpha)
static u32 *s_ovl_tex     = NULL;   // RSX-local texture the GPU samples
static u32  s_ovl_tex_off = 0;
static u32  s_ovl_w = 0, s_ovl_h = 0;
static int  s_ovl_prev_y0 = 0, s_ovl_prev_y1 = 0;  // rows drawn by last compose
static int  s_ovl_y0 = 0, s_ovl_y1 = 0;            // rows drawn by this compose

// Everything the overlay's pixels depend on.  Compared memberwise per frame;
// a change triggers one recompose + upload.  Zero the struct before filling
// so padding compares clean.
struct OvlKey {
    u32  elapsed_secs;
    u32  total_secs;
    u32  menu_epoch;
    s32  focus, incr_idx;
    s32  menu_sel, menu_cur, menu_n;
    s32  vol_pct;
    s32  aspect;                       // Fit / Fill / Stretch (picture_size.h)
    s32  live_permille;
    u32  live_epoch;
    u8   paused, cc, menu_vis, title_on, vol_active, scrubbing, live, banner_on, solo;
    char audio[64];
};
static OvlKey s_ovl_key;
static bool   s_ovl_key_valid = false;

// The overlay buffers are CACHED across player sessions.  They're sized to
// the DISPLAY (fixed at boot), not the movie, so there's never a reason to
// re-allocate them — and doing so is what used to lose the seek bar: the
// per-video RSX textures change size with each movie's resolution, and
// after a session at a different resolution the freed holes no longer fit
// an 8MB display-sized re-alloc, hud_draw saw NULL buffers and drew nothing.
void hud_overlay_alloc(void) {
    u32 bytes = display_width * display_height * 4;
    if (!s_ovl_stage) s_ovl_stage = (u32*)memalign(128, bytes);
    if (!s_ovl_tex) {
        s_ovl_tex = (u32*)rsxMemalign(64, bytes);
        if (s_ovl_tex) rsxAddressToOffset(s_ovl_tex, &s_ovl_tex_off);
    }
    s_ovl_w = display_width;
    s_ovl_h = display_height;
    if (s_ovl_stage) memset(s_ovl_stage, 0, bytes);
    if (s_ovl_tex)   memset((void*)s_ovl_tex, 0, bytes);
    if (!s_ovl_stage || !s_ovl_tex) plog("hud_ovl: alloc FAILED");
    s_ovl_prev_y0 = s_ovl_prev_y1 = 0;
    s_ovl_key_valid = false;
}

void hud_overlay_free(void) {
    // Keep the buffers (see hud_overlay_alloc) — just drop stale state so
    // the next session starts with a clean compose.
    s_ovl_prev_y0 = s_ovl_prev_y1 = 0;
    s_ovl_key_valid = false;
}

// Extend the dirty row span of the current compose.
static void ovl_span(int y0, int y1) {
    if (y0 < 0) y0 = 0;
    if (y1 > (int)s_ovl_h) y1 = (int)s_ovl_h;
    if (y1 <= y0) return;
    if (y0 < s_ovl_y0) s_ovl_y0 = y0;
    if (y1 > s_ovl_y1) s_ovl_y1 = y1;
}

// Translucent black backdrop written straight into the staging buffer —
// the bottom layer of each HUD region, so no compositing needed.
static void ovl_dim(u32 rx, u32 ry, u32 rw, u32 rh, u8 alpha) {
    if (rx >= s_ovl_w || ry >= s_ovl_h) return;
    u32 x2 = (rx + rw > s_ovl_w) ? s_ovl_w : rx + rw;
    u32 y2 = (ry + rh > s_ovl_h) ? s_ovl_h : ry + rh;
    u32 px = (u32)alpha << 24;
    for (u32 y = ry; y < y2; y++) {
        u32 *row = s_ovl_stage + y * s_ovl_w;
        for (u32 x = rx; x < x2; x++) row[x] = px;
    }
    ovl_span((int)ry, (int)y2);
}

// -------------------------------------------------------
// CPU-drawn primitives (render into the compose target)
// -------------------------------------------------------

static void draw_circle(int cx, int cy, int r, u32 color) {
    int r2 = r * r;
    u32 tw_ = cpu_draw_w(), th_ = cpu_draw_h();
    if (cpu_rt_on()) color |= 0xFF000000u;
    for (int dy = -r; dy <= r; dy++) {
        int sy = cy + dy;
        if (sy < 0 || (u32)sy >= th_) continue;
        u32 *rowp = cpu_draw_row((u32)sy);
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy > r2) continue;
            int sx = cx + dx;
            if (sx < 0 || (u32)sx >= tw_) continue;
            rowp[sx] = color;
        }
    }
}

static void fmt_time(char *buf, int sz, u32 secs) {
    u32 h = secs / 3600;
    u32 m = (secs % 3600) / 60;
    u32 s = secs % 60;
    if (h > 0) snprintf(buf, sz, "%u:%02u:%02u", h, m, s);
    else        snprintf(buf, sz, "%u:%02u",      m, s);
}

// -------------------------------------------------------
// Popup menu (track selection), bottom-right above the strip
// -------------------------------------------------------

static void draw_menu(int dw, int dh) {
    int max_w = ttf_text_width(g_hud.menu_title, MENU_TITLE_PX);
    for (int i = 0; i < g_hud.menu_n; i++) {
        int tw = MENU_DOT_COL + ttf_text_width(g_hud.menu_items[i], ROW_TEXT_PX);
        if (tw > max_w) max_w = tw;
    }
    int ox  = overscan_x();
    int mw = max_w + 2 * MENU_PAD;
    if (mw > dw - 2 * (RIGHT_PAD + ox)) mw = dw - 2 * (RIGHT_PAD + ox);
    int mx1 = dw - RIGHT_PAD - ox;
    int mx0 = mx1 - mw;
    int my1 = dh - HUD_STRIP_H - 6;
    int max_rows = (my1 - 6 - MENU_TITLE_H - MENU_PAD) / MENU_ROW_H;
    if (max_rows < 1) max_rows = 1;
    int shown = g_hud.menu_n < max_rows ? g_hud.menu_n : max_rows;
    int first = 0;
    if (g_hud.menu_n > shown) {
        first = g_hud.menu_sel - shown / 2;
        if (first < 0) first = 0;
        if (first > g_hud.menu_n - shown) first = g_hud.menu_n - shown;
    }
    int mh  = MENU_TITLE_H + shown * MENU_ROW_H + MENU_PAD;
    int my0 = my1 - mh;
    if (my0 < 6) my0 = 6;

    ovl_dim((u32)mx0, (u32)my0, (u32)mw, (u32)(my1 - my0), 225);

    drawTTF((u32)(mx0 + MENU_PAD),
            (u32)(my0 + (MENU_TITLE_H - (int)MENU_TITLE_PX) / 2),
            g_hud.menu_title, MENU_TITLE_PX, HUD_ACCENT, /*bold=*/true);

    if (g_hud.menu_n > shown) {
        char page[20];
        snprintf(page, sizeof(page), "%d/%d", g_hud.menu_sel + 1, g_hud.menu_n);
        int pw = ttf_text_width(page, ROW_TEXT_PX);
        drawTTF((u32)(mx1 - MENU_PAD - pw),
                (u32)(my0 + (MENU_TITLE_H - (int)ROW_TEXT_PX) / 2),
                page, ROW_TEXT_PX, HUD_DIMMED);
    }

    for (int row = 0; row < shown; row++) {
        int i = first + row;
        int row_y0 = my0 + MENU_TITLE_H + row * MENU_ROW_H;
        int row_cy = row_y0 + MENU_ROW_H / 2;
        if (i == g_hud.menu_sel)
            drawRect((u32)(mx0 + 4), (u32)row_y0,
                     (u32)(mw - 8), MENU_ROW_H, HUD_ACCENT_DIM);
        if (i == g_hud.menu_cur)
            draw_circle(mx0 + MENU_PAD + 5, row_cy, 4, HUD_ACCENT);
        drawTTF((u32)(mx0 + MENU_PAD + MENU_DOT_COL),
                (u32)(row_cy - (int)(ROW_TEXT_PX * 0.5f)),
                g_hud.menu_items[i], ROW_TEXT_PX,
                (i == g_hud.menu_sel) ? HUD_FOCUSED : XMB_TEXT_DIM);
    }
}

// -------------------------------------------------------
// hud_compose — render the full overlay into the staging buffer
// -------------------------------------------------------

#include "hud_v3.inc"

static void hud_compose(u64 elapsed_us, bool paused, bool scrubbing) {
    // Wipe the rows the previous compose drew, then track this one's span.
    if (s_ovl_prev_y1 > s_ovl_prev_y0)
        memset(s_ovl_stage + (u32)s_ovl_prev_y0 * s_ovl_w, 0,
               (u32)(s_ovl_prev_y1 - s_ovl_prev_y0) * s_ovl_w * 4);
    s_ovl_y0 = (int)s_ovl_h;
    s_ovl_y1 = 0;

    cpu_rt_begin(s_ovl_stage, s_ovl_w, s_ovl_h);

    if (g_hud.banner_solo) v3_draw_banner();        // the bar itself stays away
    else                   hud_body_v3(elapsed_us, paused, scrubbing);

    cpu_rt_end();

    // Upload the union of the previous and current spans so rows the HUD no
    // longer covers (e.g. a closed menu) are cleared on the texture too.
    int up_y0 = (s_ovl_prev_y0 < s_ovl_y0) ? s_ovl_prev_y0 : s_ovl_y0;
    int up_y1 = (s_ovl_prev_y1 > s_ovl_y1) ? s_ovl_prev_y1 : s_ovl_y1;
    if (up_y1 > up_y0)
        memcpy((void*)(s_ovl_tex + (u32)up_y0 * s_ovl_w),
               s_ovl_stage + (u32)up_y0 * s_ovl_w,
               (u32)(up_y1 - up_y0) * s_ovl_w * 4);
    s_ovl_prev_y0 = (s_ovl_y1 > s_ovl_y0) ? s_ovl_y0 : 0;
    s_ovl_prev_y1 = (s_ovl_y1 > s_ovl_y0) ? s_ovl_y1 : 0;

    {
        static int s_cl = 0;
        if (s_cl < 12) {
            s_cl++;
            char buf[80];
            snprintf(buf, sizeof(buf), "hud_ovl: compose#%d rows=%d..%d up=%d..%d",
                     s_cl, s_ovl_y0, s_ovl_y1, up_y0, up_y1);
            plog(buf);
        }
    }
}

// -------------------------------------------------------
// hud_draw — recompose the overlay if its content changed, then queue the
// GPU quad.  Called every frame the HUD is visible; the compose branch runs
// about once a second (when the time string ticks) or on input.
// -------------------------------------------------------

void hud_draw(u64 elapsed_us, bool paused, bool scrubbing) {
    if (!g_hud.visible && !g_hud.banner_solo) return;
    if (!s_ovl_stage || !s_ovl_tex) return;   // alloc failed: no HUD

    OvlKey key;
    memset(&key, 0, sizeof(key));
    key.elapsed_secs = (u32)(elapsed_us / 1000000ULL);
    key.total_secs   = g_hud.total_secs;
    key.menu_epoch   = g_hud.menu_epoch;
    key.focus        = g_hud.focus;
    key.incr_idx     = g_hud.incr_idx;
    key.menu_sel     = g_hud.menu_sel;
    key.menu_cur     = g_hud.menu_cur;
    key.menu_n       = g_hud.menu_n;
    key.paused       = paused ? 1 : 0;
    key.cc           = g_hud.cc_active ? 1 : 0;
    key.menu_vis     = g_hud.menu_visible ? 1 : 0;
    key.title_on     = (paused && g_hud.title[0]) ? 1 : 0;
    key.vol_active   = g_hud.vol_active ? 1 : 0;
    key.vol_pct      = audio_get_volume();
    key.aspect       = picture_get().aspect;
    key.scrubbing    = scrubbing ? 1 : 0;
    key.live         = g_hud.live ? 1 : 0;
    key.live_permille = g_hud.live_permille;
    key.live_epoch   = g_hud.live_epoch;
    key.solo         = g_hud.banner_solo ? 1 : 0;
    key.banner_on    = (g_hud.banner[0] && timing_get_us() < g_hud.banner_until_us) ? 1 : 0;
    snprintf(key.audio, sizeof(key.audio), "%s", g_hud.audio_label);

    if (!s_ovl_key_valid || memcmp(&key, &s_ovl_key, sizeof(key)) != 0) {
        s_ovl_key       = key;
        s_ovl_key_valid = true;
        hud_compose(elapsed_us, paused, scrubbing);
    }

    rsx_draw_hud_overlay(s_ovl_tex_off, s_ovl_w, s_ovl_h);
}
