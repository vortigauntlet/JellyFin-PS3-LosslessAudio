// Is the cached, binary-searched ttf_clip() the cut the old per-character trim made?
// Same harness as test_text_runs: the REAL ui_text.cpp, compiled on the host.
// Build:  make -f Makefile.host test_text_clip && ./test_text_clip

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <ppu-types.h>
#include <rsx/rsx.h>

// ---- the framebuffer ui_text.cpp draws into --------------------------------
#define FB_W 1280
#define FB_H 256
static u32 s_fb[FB_W * FB_H];

// ---- everything ui_text.cpp expects the rest of the app to provide ---------
// Declared in ui.h / rsxutil.h / ui_visuals.h; defined here so the real source
// compiles unmodified.  None of it is stubbed behaviour the test relies on:
// the pieces that matter (cpu_draw_row and friends) are the genuine contract,
// pointed at s_fb instead of video memory.
gcmContextData *context = NULL;
u32  display_width  = FB_W;
u32  display_height = FB_H;
u32  display_par_num = 1, display_par_den = 1;
u32  curr_fb = 0;
u32  color_pitch = FB_W * 4;
u32  color_offset[2] = { 0, 0 };
u32 *color_buffer[2] = { s_fb, s_fb };
u32  depth_pitch = 0, depth_offset = 0;

int g_cpu_clip_top = 0;
int g_cpu_clip_bot = 0;

bool  cpu_rt_on(void)        { return false; }
u32   cpu_draw_w(void)       { return FB_W; }
u32   cpu_draw_h(void)       { return FB_H; }
u32  *cpu_draw_row(u32 y)    { return s_fb + (size_t)y * FB_W; }
bool  cpu_row_clipped(int sy) {
    if (sy < g_cpu_clip_top || sy >= (int)FB_H) return true;
    if (g_cpu_clip_bot && sy >= g_cpu_clip_bot) return true;
    return false;
}

void plog(const char *) {}
int  overscan_x(void) { return 0; }
int  overscan_y(void) { return 0; }

#include "bitmap.h"
void bitmapSetXpm(Bitmap *bm, const char *[]) { memset(bm, 0, sizeof(*bm)); }
void bitmapDestroy(Bitmap *bm)         { memset(bm, 0, sizeof(*bm)); }

// The collecting window is never open here: this test is comparing the CPU
// path against the run path directly, so drawTTF must take the CPU path.
#include "ui_text_gpu.h"
bool ui_text_gpu_run(u32, u32, const char *, float, u32, int) { return false; }

// Likewise drawIcon(): ui_text.cpp calls it unconditionally, so the link needs
// it whether or not a test case draws an icon.  Returning false keeps drawIcon
// on its own per-glyph blit -- which is the path this file compares against,
// and which is NOT the gamma-corrected one the run path uses.
bool ui_text_gpu_icon(u32, u32, int, float, u32) { return false; }

// drawIconA()'s CPU fallback mixes toward XMB_BG, which reads the theme.
// Colours do not affect metrics, so a zeroed theme is enough.
#include "theme.h"
Theme g_theme;

// The real thing, compiled as-is.
#include "../source/ui/render/ui_text.cpp"

// ---------------------------------------------------------------------------
// ttf_clip: the cached cut must be what trimming one character at a time gave
// (on character boundaries), and a second call must return the same thing from
// the cache.

static void reference_clip(const char *text, float px, int face, int max_w, char *out, int cap) {
    snprintf(out, (size_t)cap, "%s", text);
    if (ttf_text_width_face(out, px, face) <= max_w) return;
    char buf[250];
    snprintf(buf, sizeof buf, "%s", text);
    int len = (int)strlen(buf);
    while (len > 1) {
        len--;
        while (len > 1 && ((unsigned char)buf[len] & 0xC0) == 0x80) len--;
        if (((unsigned char)text[len] & 0xC0) == 0x80) continue;   // the old trim's one bug: half a character
        buf[len] = '\0';
        char trial[256];
        snprintf(trial, sizeof trial, "%s..", buf);
        if (ttf_text_width_face(trial, px, face) <= max_w) { snprintf(out, (size_t)cap, "%s", trial); return; }
    }
    out[0] = '\0';
}

static int g_fail = 0, g_n = 0;
#define CHECK(c, ...) do { g_n++; if (!(c)) { g_fail++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    ttf_init();
    static const char *kText[] = {
        "Avatar", "Mad Max: Fury Road", "The Lord of the Rings: The Return of the King",
        "Pok\xC3\xA9mon: Mewtwo Strikes Back \xE2\x80\x94 Evolution",
        "\xE5\x8D\x83\xE3\x81\xA8\xE5\x8D\x83\xE5\xB0\x8B\xE3\x81\xAE\xE7\xA5\x9E\xE9\x9A\xA0\xE3\x81\x97",
        "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWW", "iiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiii", "A",
    };
    static const int   kFace[]  = { UI_FACE_REGULAR, UI_FACE_BOLD };
    static const float kPx[]    = { 13.f, 20.f, 32.f };
    static const int   kW[]     = { 4, 30, 90, 160, 300, 2000 };
    for (const char *t : kText)
        for (int f : kFace)
            for (float px : kPx)
                for (int w : kW) {
                    char want[256], got[256], again[256];
                    reference_clip(t, px, f, w, want, sizeof want);
                    ttf_clip(t, px, f, w, got, sizeof got);
                    ttf_clip(t, px, f, w, again, sizeof again);
                    CHECK(strcmp(want, got) == 0, "'%s' px=%.0f face=%d w=%d: want '%s' got '%s'",
                          t, px, f, w, want, got);
                    CHECK(strcmp(got, again) == 0, "cache returned '%s' then '%s'", got, again);
                    CHECK(!got[0] || ttf_text_width_face(got, px, f) <= w, "'%s' overflows %d", got, w);
                }
    // Same text, another width: never the other width's cut.
    char a[64], b[64];
    ttf_clip("Mad Max: Fury Road", 20.f, UI_FACE_REGULAR, 60, a, sizeof a);
    ttf_clip("Mad Max: Fury Road", 20.f, UI_FACE_REGULAR, 2000, b, sizeof b);
    CHECK(strcmp(b, "Mad Max: Fury Road") == 0 && strcmp(a, b) != 0, "width is part of the key");
    ttf_clip("", 20.f, UI_FACE_REGULAR, 100, a, sizeof a);
    CHECK(a[0] == '\0', "empty text");
    printf("text clip: %d checks, %d failed\n", g_n, g_fail);
    return g_fail ? 1 : 0;
}
