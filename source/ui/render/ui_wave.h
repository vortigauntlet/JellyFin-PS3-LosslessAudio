#pragma once
#include <ppu-types.h>

void wave_init(void);
void wave_draw(void);
void wave_reset(void);

// True when CPU framebuffer writes are cheap (emulator): the whole XMB
// background is composited on the CPU so the flip presents CPU-drawn content.
// False on real hardware, where the background stays on the GPU.  Set once by
// wave_init().  clearScreen() branches on this too.
bool ui_cpu_bg(void);

// True when the wave is running mode 2 -- vertex arrays with GPU alpha
// blending proven live.  Callers use it to choose a blended-quad path over a
// CPU read-modify-write one.
bool wave_gpu_blend_ready(void);

// The tab-bar hairline as a blended quad: six vertices, no VRAM reads.
// MUST be called in the GPU phase, BEFORE the frame's rsxSync() -- issuing it
// after the fence would put it in the FIFO behind CPU pixel writes that have
// already landed, and it would draw over them.  See ui_wave.cpp.
void wave_draw_divider_gpu(int y_px, u8 r, u8 g, u8 b, u8 peak_alpha);

// Focus glow for the XMB category strip.  This is a small blended fan emitted
// in the pre-fence GPU phase, so it never composites through the framebuffer.
void wave_draw_glow_gpu(int cx_px, int cy_px, int radius_px,
                        u8 r, u8 g, u8 b, u8 peak_alpha);

// Bind the wave's passthrough programs + standard alpha blend for immediate-
// mode geometry drawn elsewhere.  No vertex buffers, no fence.  GPU phase.
bool wave_imm_bind(void);

// --- spine panels (ui_wave_panels.cpp) -------------------------------------
// All immediate-mode on the programs above; GPU phase only, before the
// frame's rsxSync().

// Elliptical glow: peak alpha at the centre, falling linearly to zero at
// (rx, ry).  The three-radius form above is the round case.
void wave_draw_glow_gpu(int cx_px, int cy_px, int rx_px, int ry_px,
                        u8 r, u8 g, u8 b, u8 peak_alpha);

// Rounded rectangle (r = h/2 gives a pill), filled with a horizontal colour
// ramp from rgb_left to rgb_right at one alpha.
void wave_draw_rrect_gpu(int x, int y, int w, int h, int r,
                         u32 rgb_left, u32 rgb_right, u8 alpha);
// The same shape with a t-pixel border: the outer shape in the line colour,
// the inset one in the fill.
void wave_draw_rrect_outline_gpu(int x, int y, int w, int h, int r, int t,
                                 u32 line_rgb, u8 line_a,
                                 u32 fill_left, u32 fill_right, u8 fill_a);
// A one-colour alpha ramp over a rectangle -- a CSS linear-gradient of n
// stops at fractions pos[] (0..1 along x, or along y when vertical).
void wave_draw_ramp_gpu(int x, int y, int w, int h, u32 rgb, bool vertical,
                        int n, const float *pos, const u8 *alpha);

// Blended full-screen black quad at the given alpha, drawn on the GPU and
// fenced with rsxSync() so CPU pixel writes may follow immediately.  Used to
// dim the finished frame under a modal.
void wave_dim_screen(u8 alpha);
