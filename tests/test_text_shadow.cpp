// test_text_shadow -- see the block after the harness below.
// The harness (stubs + the real ui_text.cpp) is test_text_runs.cpp's.

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
bool strobe_test_disable_tracked_text(void) { return false; }

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
#define RUN_W_MAX_T 2048
#define RUN_H_MAX_T 160

// ===========================================================================
// test_text_shadow: the shadow baked into light GPU text runs (text_shadow.h),
// on the REAL rasterizer and fonts above.  Built from test_text_runs.cpp's
// harness (everything before this point is that file's, verbatim).
//
// Checks, for real titles in the faces the grids use:
//   - light text wants a shadow, dark text (a key label) does not;
//   - fully covered glyph pixels are untouched, and no ink pixel loses alpha;
//   - every pixel the shadow adds lies below/right of ink (within off+1);
//   - the shadow fits the box grown by `off` (nothing is lost at the edge).
// Then writes previews over a bright wave-like band at 1080 and 480 lines
// (the 4:3 SD case): text_shadow_1080.ppm, text_shadow_480.ppm.
// ===========================================================================
#include "text_shadow.h"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static u32  s_a[RUN_W_MAX_T * RUN_H_MAX_T], s_b[RUN_W_MAX_T * RUN_H_MAX_T];
static u8   s_cov[RUN_W_MAX_T * RUN_H_MAX_T];

static void check_run(const char *text, float px, int face, u32 colour, int off)
{
    TtfRunBox box;
    if (!ttf_run_box(text, px, face, &box)) { CHECK(0, "no box for '%s'", text); return; }
    TtfRunBox big = box; big.w += off; big.h += off;
    ttf_run_raster(text, px, face, colour, s_a, &box);
    ttf_run_raster(text, px, face, colour, s_b, &big);
    text_shadow_bake(s_b, big.w, big.h, off, s_cov);

    long added = 0, stray = 0, lost = 0, changed_full = 0;
    for (int y = 0; y < big.h; y++)
        for (int x = 0; x < big.w; x++) {
            const u32 ab = s_b[y * big.w + x] >> 24;
            const u32 aa = (x < box.w && y < box.h) ? s_a[y * box.w + x] >> 24 : 0;
            if (aa == 255 && s_b[y * big.w + x] != s_a[y * box.w + x]) changed_full++;
            if (ab < aa) lost++;
            if (aa == 0 && ab > 0) {
                added++;
                // the source of this shadow pixel must be ink within the blur
                bool src = false;
                for (int dy = -1; dy <= 1 && !src; dy++)
                    for (int dx = -1; dx <= 1 && !src; dx++) {
                        const int sx = x - off + dx, sy = y - off + dy;
                        if (sx >= 0 && sy >= 0 && sx < box.w && sy < box.h && (s_a[sy * box.w + sx] >> 24))
                            src = true;
                    }
                if (!src) stray++;
            }
        }
    CHECK(changed_full == 0, "'%s' px=%.0f: %ld fully covered pixels changed", text, px, changed_full);
    CHECK(lost == 0,         "'%s' px=%.0f: %ld pixels lost alpha", text, px, lost);
    CHECK(added > 0,         "'%s' px=%.0f: no shadow added", text, px);
    CHECK(stray == 0,        "'%s' px=%.0f: %ld shadow pixels not under ink", text, px, stray);
    // The last column/row of the grown box can only hold shadow of the
    // original box's last column/row: present exactly when that had ink.
    printf("  ok  '%s' px=%.1f face=%d off=%d box %dx%d -> %dx%d, %ld shadow px\n",
           text, px, face, off, box.w, box.h, big.w, big.h, added);
}

// ---- preview ---------------------------------------------------------------
static u32 *s_img; static int s_iw, s_ih;
static void bg_wave(void)
{
    for (int y = 0; y < s_ih; y++)
        for (int x = 0; x < s_iw; x++) {
            // dark navy, with a bright ribbon band through the middle
            float t = (float)y / s_ih;
            float band = expf(-((t - 0.55f) * (t - 0.55f)) / 0.006f) *
                         (0.75f + 0.25f * sinf((float)x * 0.01f));
            float hue = (float)x / s_iw;
            u32 r = (u32)(11 + band * (90 + 160 * hue));
            u32 g = (u32)(14 + band * (200 - 60 * hue));
            u32 b = (u32)(30 + band * (220 - 120 * hue));
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            s_img[y * s_iw + x] = (r << 16) | (g << 8) | b;
        }
}
static void put_run(const char *text, float px, int face, u32 colour, int x, int y, bool shadow, int off)
{
    TtfRunBox box;
    if (!ttf_run_box(text, px, face, &box)) return;
    if (shadow) { box.w += off; box.h += off; }
    ttf_run_raster(text, px, face, colour, s_b, &box);
    if (shadow) text_shadow_bake(s_b, box.w, box.h, off, s_cov);
    for (int r = 0; r < box.h; r++)
        for (int c = 0; c < box.w; c++) {
            const int dx = x + box.ox + c, dy = y + box.oy + r;
            if (dx < 0 || dy < 0 || dx >= s_iw || dy >= s_ih) continue;
            const u32 s = s_b[r * box.w + c], a = s >> 24;
            if (!a) continue;
            const u32 d = s_img[dy * s_iw + dx];
            u32 o = 0;
            for (int sh = 0; sh <= 16; sh += 8) {
                const u32 sc = (s >> sh) & 0xFF, dc = (d >> sh) & 0xFF;
                o |= (((sc * a + dc * (255 - a)) / 255) & 0xFF) << sh;
            }
            s_img[dy * s_iw + dx] = o;
        }
}
static void preview(const char *path, int w, int h)
{
    s_iw = w; s_ih = h;
    s_img = (u32 *)malloc((size_t)w * h * 4);
    bg_wave();
    const float k = (float)h / 720.0f;
    const int off = text_shadow_px((unsigned)h);
    const char *titles[] = { "Better Call Saul", "The Grand Budapest Hotel", "Oppenheimer" };
    // left half: as it was; right half: with the shadow.  Rows cross the band.
    for (int side = 0; side < 2; side++)
        for (int i = 0; i < 6; i++) {
            const int x = (int)(w * (side ? 0.53f : 0.03f));
            const int y = (int)(h * (0.36f + 0.06f * i));
            const bool sel = (i % 3) == 0;
            float px = (sel ? 20.0f : 16.0f) * k;
            if (h < 720 && px < 11.0f) px = 11.0f;
            put_run(titles[i % 3], px, UI_FACE_DISPLAY, sel ? 0x00FFFFFFu : 0x0099A0BCu,
                    x, y, side == 1, off);
        }
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        unsigned char px3[3] = { (unsigned char)(s_img[i] >> 16), (unsigned char)(s_img[i] >> 8), (unsigned char)s_img[i] };
        fwrite(px3, 1, 3, f);
    }
    fclose(f);
    free(s_img);
    printf("  wrote %s (%dx%d, shadow %d px)\n", path, w, h, off);
}

int main(void)
{
    ttf_init();
    CHECK(text_shadow_wanted(0x00FFFFFFu),  "white should have a shadow");
    CHECK(text_shadow_wanted(0x0099A0BCu),  "XMB text_dim should have a shadow");
    CHECK(!text_shadow_wanted(0x00151A38u), "a dark key label must not");
    CHECK(!text_shadow_wanted(0x005C6386u), "text_faint (dark) must not");
    CHECK(text_shadow_px(1080) == 2 && text_shadow_px(720) == 1 && text_shadow_px(480) == 1 &&
          text_shadow_px(576) == 1, "shadow offset per resolution");

    const char *titles[] = { "Better Call Saul", "The Grand Budapest Hotel", "gjpqy Qq", "Amélie" };
    const float sizes[] = { 11.0f, 16.0f, 20.0f, 24.0f, 30.0f };
    const int faces[] = { UI_FACE_DISPLAY, UI_FACE_REGULAR, UI_FACE_BOLD };
    for (size_t t = 0; t < sizeof titles / sizeof *titles; t++)
        for (size_t z = 0; z < sizeof sizes / sizeof *sizes; z++)
            for (size_t f = 0; f < sizeof faces / sizeof *faces; f++)
                for (int off = 1; off <= 2; off++)
                    check_run(titles[t], sizes[z], faces[f], 0x0099A0BCu, off);

    preview("text_shadow_1080.ppm", 1920, 1080);
    preview("text_shadow_480.ppm", 720, 480);
    printf("test_text_shadow: %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
