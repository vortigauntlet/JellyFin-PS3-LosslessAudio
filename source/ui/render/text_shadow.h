#pragma once
// A soft dark shadow under light text, baked into a GPU text run when it is
// rasterized (ui_text_gpu.cpp).  Pure C, no RSX, so tests/test_text_shadow.cpp
// checks the exact code the console runs.
//
// Why: titles drawn over the XMB wave -- the dim grey under a poster crossing a
// bright ribbon -- had nothing to separate them from it (2026-10-14).  The
// PS3's own XMB shadows its text for the same reason.  Light colours only: a
// dark label (the one on a white key) would only smear.
//
// The shadow is the glyph coverage moved `off` px down and right, box-blurred
// 3x3, at TEXT_SHADOW_A, composited UNDER the text.  The caller grows the run's
// box by `off` on the right and bottom so it fits; the ink's own position (the
// box's ox/oy) does not move, so nothing is laid out differently.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define TEXT_SHADOW_A 150u                 /* 0..255 at full coverage */

/* 1 px on SD and 720p, 2 px on 1080 lines. */
static inline int text_shadow_px(unsigned display_h) { return display_h >= 1080 ? 2 : 1; }

/* Light enough to need one (0x00RRGGBB). */
static inline bool text_shadow_wanted(uint32_t colour)
{
    const uint32_t r = (colour >> 16) & 0xFF, g = (colour >> 8) & 0xFF, b = colour & 0xFF;
    return (r * 299u + g * 587u + b * 114u) / 1000u >= 120u;
}

/* buf: w*h pixels, A = coverage (gamma-baked), RGB = the text colour.  cov:
 * w*h bytes of scratch.  Composites the shadow under the text in place. */
static inline void text_shadow_bake(uint32_t *buf, int w, int h, int off, uint8_t *cov)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t sum = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    const int sx = x - off + dx, sy = y - off + dy;
                    if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;
                    sum += buf[(size_t)sy * (size_t)w + (size_t)sx] >> 24;
                }
            cov[(size_t)y * (size_t)w + (size_t)x] = (uint8_t)((sum / 9u) * TEXT_SHADOW_A / 255u);
        }
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
        const uint32_t ta = buf[i] >> 24, sa = cov[i];
        if (!sa || ta == 255u) continue;
        /* text OVER a black shadow: the alpha grows, the colour keeps the
         * text's share of it. */
        const uint32_t oa = ta + sa * (255u - ta) / 255u;
        const uint32_t k  = ta * 255u / oa;
        const uint32_t rgb = buf[i] & 0x00FFFFFFu;
        const uint32_t r = ((rgb >> 16) & 0xFF) * k / 255u;
        const uint32_t g = ((rgb >> 8) & 0xFF) * k / 255u;
        const uint32_t b = (rgb & 0xFF) * k / 255u;
        buf[i] = (oa << 24) | (r << 16) | (g << 8) | b;
    }
}
