// The playback buffering screen.  See ui_buffering.h and buffer_anim.h.
//
// LAYERS, back to front (all GPU, in one GPU phase):
//   1. the item's artwork, opaque, full screen: the detail page's backdrop,
//      else its poster cropped to a wide band (LINEAR upscaling softens it,
//      which is what a background wants); with neither, the accent's deep
//      shade as a soft pool of light;
//   2. the veil: a vertical black ramp, darkest at the bottom where the title
//      sits.  It also carries the fade: at ui_a = 0 the veil is opaque black;
//   3. a soft glow behind the logo, in the artwork's glow shade, breathing;
//   4. the ring's faint full-circle track, then the arc in Jellyfin's own
//      colours: #AA5CC3 purple at the (transparent) tail -> #00A4DC blue at
//      the head.  Closed, it is the same gradient round both halves, so the
//      full circle has no seam;
//   5. the legacy JellyFin-PS3 logo -- MontyMcK's original app icon
//      (ICON0.PNG, gfx/jf_legacy_icon_png.h): the PlayStation mark in
//      Jellyfin's gradient.  Decoded ONCE per run into its own VRAM slot
//      (GPU_TEX_BRAND, ~56 KB) and blended by its own alpha -- the HUD
//      overlay's blend.  If that ever fails, the vector Jellyfin mark
//      (jf_logo_geom.h) stands in;
//   6. the fade: one near-black quad over all of the above at 1 - ui_a.  A
//      texture blended by its own alpha cannot also take a per-frame
//      opacity, so everything fades together under one cover instead.
// Then rsxSync and the text: BUFFERING / 47% under the ring, the title at the
// lower left, and the O hint.

#include <stdio.h>
#include <string.h>

#include <sysutil/sysutil.h>

#include "ui_buffering.h"
#include "buffer_anim.h"
#include "ui_art.h"
#include "ui_card_gpu.h"
#include "ui_wave.h"
#include "ui_text_gpu.h"
#include "ui.h"
#include "ui_visuals.h"
#include "rsxutil.h"
#include "timing.h"
#include "stb_image.h"          // implementation compiled in thumbnail_cache.cpp
#include "jf_legacy_icon_png.h"
#include <stdlib.h>

static buf_anim    s_anim;
static bool        s_on = false;
static bool        s_flip_pending = false;
static u64         s_last_draw = 0;
static char        s_title[128];
static char        s_label[48] = "BUFFERING";
static bool        s_show_pct = false;
static const char *s_hint = NULL;
static int         s_art = -1;           // GPU_TEX_* slot, or -1
static art_palette s_pal;

// Jellyfin's brand colours: the ring is drawn in them whatever the artwork.
#define JF_PURPLE 0x00AA5CC3
#define JF_BLUE   0x0000A4DC

// The legacy logo in VRAM: 0 = not tried, 1 = ready, -1 = failed (vector mark).
static int s_logo_state = 0;

static void logo_load_once(void) {
    if (s_logo_state) return;
    s_logo_state = -1;
    int w = 0, h = 0, comp = 0;
    unsigned char *img = stbi_load_from_memory(jf_legacy_icon_png,
                                               (int)sizeof jf_legacy_icon_png,
                                               &w, &h, &comp, 4);
    if (!img) return;
    Bitmap bm;
    memset(&bm, 0, sizeof bm);
    bm.width = (u32)w; bm.height = (u32)h;
    bm.pixels = (u32 *)malloc((size_t)w * h * 4);
    if (bm.pixels) {
        // RGBA bytes -> A8R8G8B8, straight alpha (what the texture samples).
        for (int i = 0; i < w * h; i++)
            bm.pixels[i] = ((u32)img[i*4+3] << 24) | ((u32)img[i*4] << 16) |
                           ((u32)img[i*4+1] << 8)  |  (u32)img[i*4+2];
        if (ui_gpu_tex_upload(GPU_TEX_BRAND, &bm)) s_logo_state = 1;
        free(bm.pixels);
    }
    stbi_image_free(img);
}

static int BX(int x) { return XMB_OX + UIS_W(x); }
static int BY(int y) { return XMB_OY + UIS_H(y); }

static inline u8 a8(float a) {
    a = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
    return (u8)(a * 255.0f + 0.5f);
}

void buffering_begin(const char *item_id, const char *title) {
    buf_anim_start(&s_anim, timing_get_us());
    s_on = true;
    s_flip_pending = false;
    s_last_draw = 0;
    snprintf(s_title, sizeof s_title, "%s", title ? title : "");
    snprintf(s_label, sizeof s_label, "PREPARING");
    s_show_pct = false;
    s_hint = NULL;
    // Only artwork that is known to be THIS item's.  After an auto-advance to
    // the next episode the slots still hold the previous one's: better the
    // plain background than the wrong picture.
    s_art = -1;
    if (item_id && item_id[0] && ui_card_gpu_ready()) {
        if (strcmp(ui_gpu_tex_tag(GPU_TEX_BACKDROP), item_id) == 0)    s_art = GPU_TEX_BACKDROP;
        else if (strcmp(ui_gpu_tex_tag(GPU_TEX_POSTER), item_id) == 0) s_art = GPU_TEX_POSTER;
    }
    ui_art_peek(item_id, &s_pal);
    // Before vdec_open reshuffles the heap: the decode's ~110 KB of scratch is
    // freed again at once, and the texture itself lives in VRAM.
    logo_load_once();
}

void buffering_step(const char *label, bool show_pct, const char *circle_hint) {
    snprintf(s_label, sizeof s_label, "%s", label ? label : "");
    s_show_pct = show_pct;
    s_hint = circle_hint;
}

void buffering_progress(float p) { buf_anim_progress(&s_anim, p); }

bool buffering_active(void) { return s_on; }

static void draw_gpu(const buf_frame &f) {
    const int W = (int)display_width, H = (int)display_height;
    const float ua = f.ui_a;

    // 1. artwork
    bool art = false;
    if (s_art == GPU_TEX_BACKDROP) {
        art = ui_gpu_tex_draw(GPU_TEX_BACKDROP, 0, 0, W, H);
    } else if (s_art == GPU_TEX_POSTER) {
        // A 2:3 poster filling the width shows H / (W * 1.5) of its height;
        // take that band from the middle.
        float band = (float)H / ((float)W * 1.5f);
        if (band > 1.0f) band = 1.0f;
        art = ui_gpu_tex_draw_crop(GPU_TEX_POSTER, 0, 0, W, H,
                                   0.5f - band * 0.5f, 0.5f + band * 0.5f);
    }
    ui_card_gpu_end();

    // 2. the veil.  Without artwork the clear colour is already near black
    //    and the accent's deep shade pools behind the logo instead.
    if (art) {
        static const float vp[4] = { 0.0f, 0.40f, 0.70f, 1.0f };
        static const u8    va[4] = { 204, 179, 199, 240 };   // .80 .70 .78 .94
        wave_draw_ramp_gpu(0, 0, W, H, 0x00030408, true, 4, vp, va);
    } else {
        wave_draw_glow_gpu(BX(640), BY(330), UIS_W(620), UIS_H(420),
                           (u8)(s_pal.deep >> 16), (u8)(s_pal.deep >> 8), (u8)s_pal.deep,
                           a8(0.9f));
    }

    // 3-4. glow, ring
    const int cx = BX(640), cy = BY(318);
    const float R = (float)UIS_H(64), T = (float)UIS_H(3) < 2.0f ? 2.0f : (float)UIS_H(3);
    const u32 g = s_pal.glow;
    wave_draw_glow_gpu(cx, cy, UIS_H(150), UIS_H(150),
                       (u8)(g >> 16), (u8)(g >> 8), (u8)g, a8(f.glow_a));
    if (f.track_a > 0.0f)
        wave_draw_ring_arc_gpu(cx, cy, R, T * 0.66f, 0.0f, 1.0f, 72,
                               0x00FFFFFF, a8(f.track_a * 0.6f),
                               0x00FFFFFF, a8(f.track_a * 0.6f));
    {
        const float arc = f.arc > 1.0f ? 1.0f : f.arc;
        const u8 head = a8(f.ring_a);
        if (arc >= 0.999f) {
            // Closed: purple -> blue -> purple round the two halves.
            const float a0 = f.ring_angle - 1.0f;
            wave_draw_ring_arc_gpu(cx, cy, R, T, a0, 0.5f, 48,
                                   JF_PURPLE, head, JF_BLUE, head);
            wave_draw_ring_arc_gpu(cx, cy, R, T, a0 + 0.5f, 0.5f, 48,
                                   JF_BLUE, head, JF_PURPLE, head);
        } else {
            int segs = (int)(96.0f * arc);
            if (segs < 8) segs = 8;
            wave_draw_ring_arc_gpu(cx, cy, R, T, f.ring_angle - arc, arc, segs,
                                   JF_PURPLE, 0, JF_BLUE, head);
        }
    }

    // 5. the logo (breath and final pulse are its scale)
    if (s_logo_state == 1) {
        const float lh = (float)UIS_H(76) * f.mark_scale;
        const float lw = lh * (float)JF_LEGACY_ICON_W / (float)JF_LEGACY_ICON_H;
        ui_gpu_tex_draw_alpha(GPU_TEX_BRAND, cx - (int)(lw * 0.5f), cy - (int)(lh * 0.5f),
                              (int)lw, (int)lh);
        ui_card_gpu_end();
    } else {
        wave_draw_jf_logo_gpu(cx, cy, (float)UIS_H(34) * f.mark_scale,
                              JF_PURPLE, JF_BLUE, a8(f.mark_a));
    }

    // 6. the fade
    if (ua < 0.999f) {
        static const float fp[2] = { 0.0f, 1.0f };
        const u8 c = a8(1.0f - ua);
        const u8 fa[2] = { c, c };
        wave_draw_ramp_gpu(0, 0, W, H, 0x00030408, true, 2, fp, fa);
    }
}

static void draw_text(const buf_frame &f) {
    // Quantised fades (depth_mix_q's rule): a fading label reuses cached runs.
    const float q = (float)(int)(f.ui_a * 16.0f + 0.5f) / 16.0f;
    const u32 bg = 0x00030408;
    const int W = (int)display_width;

    if (s_label[0]) {
        const int ew = xmb_eyebrow_width(s_label);
        xmb_draw_eyebrow((W - ew) / 2, BY(408), s_label, art_mix(bg, XMB_TEXT_DIM, q));
    }
    if (s_show_pct) {
        char pct[8];
        snprintf(pct, sizeof pct, "%d%%", f.pct);
        const float px = UIS_TF(20.0f);
        const int pw = ttf_text_width_face(pct, px, UI_FACE_DISPLAY);
        drawTTF_face((u32)((W - pw) / 2), (u32)BY(430), pct, px,
                     art_mix(bg, XMB_TEXT, q), UI_FACE_DISPLAY);
    }
    if (s_title[0]) {
        const float px = UIS_TF(22.0f);
        char t[128];
        snprintf(t, sizeof t, "%s", s_title);
        int len = (int)strlen(t);
        const int maxw = BX(1223) - BX(57);
        while (len > 1 && ttf_text_width_face(t, px, UI_FACE_DISPLAY) > maxw) t[--len] = 0;
        drawTTF_face((u32)BX(57), (u32)BY(606), t, px, art_mix(bg, XMB_TEXT, q * 0.92f),
                     UI_FACE_DISPLAY);
    }
    if (s_hint && f.ui_a > 0.5f && s_anim.phase == BUF_LOADING) {
        Hint h[1];
        h[0].glyph = 'C'; h[0].label = s_hint;
        draw_hints_bar(h, 1);
    }
}

void buffering_frame(void) {
    if (!s_on) return;
    if (s_flip_pending) waitflip();
    const u64 now = timing_get_us();
    s_last_draw = now;
    buf_frame f;
    buf_anim_eval(&s_anim, now, &f);

    clearScreen(0x00030408);
    draw_gpu(f);
    rsxSync();
    ui_text_gpu_begin();
    draw_text(f);
    ui_text_gpu_flush();
    flip();
    s_flip_pending = true;
#if BUILD_FOR_RPCS3
    // Same rule as player_startup_flip(): the emulator must not have a frame
    // in flight across the long heap reshuffles that follow these screens.
    rsxSync();
#endif
}

void buffering_frame_paced(u64 min_us) {
    if (!s_on) return;
    if (s_last_draw && timing_get_us() - s_last_draw < min_us) return;
    buffering_frame();
}

void buffering_finish(bool ready) {
    if (!s_on) return;
    const u64 now = timing_get_us();
    if (ready) buf_anim_ready(&s_anim, now);
    else       buf_anim_cancel(&s_anim, now);
    // The outro runs at the display's own pace (one frame per flip); it is
    // bounded by buffer_anim.h (at most BUF_MIN_SHOW_US + ~0.45 s).
    while (running && !buf_anim_done(&s_anim, timing_get_us())) {
        sysUtilCheckCallback();
        buffering_frame();
    }
    // One last black frame so nothing of the screen lingers under the
    // player's first video frame.
    if (running) {
        if (s_flip_pending) waitflip();
        clearScreen(0x00000000);
        flip();
        s_flip_pending = true;
    }
    s_on = false;
}
