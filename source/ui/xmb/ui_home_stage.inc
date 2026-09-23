// Home under the spine: one stage, drawn at every depth.   (#included by
// ui_home.cpp, after the model, so it reads the rows directly.)
//
// The current canvas (design-import-v3, "00 · Depth model") draws Home as
// ONE category at a time, and says the L1 column and the L2 queue are "the
// same items on the same invisible surface, seen from a different angle --
// it swings from vertical to horizontal rather than cross-fading".  So this
// is not two screens with a cut between them.  Every item has a box at L1
// (hanging under the Home icon) and a box at L2 (the queue), and is drawn
// at the depth-weighted blend of the two:
//
//   L1  the focused item 128x192 under the icon, the next two below it at
//       106x159 and 86x129 under colder veils, sinking off the bottom.
//   L2  the focused item forward at 200x300 (x=64) with a 46 px reflection;
//       the rest bottom-aligned on y~552 and receding right -- 104x156 @.62,
//       92x138 @.48, 80x120 @.36, 70x105 @.26 -- each with a short
//       reflection; the facts column floating between, 520 wide at x=304.
//
// Left/Right slides the queue (the item leaving the focus goes off to the
// left, the next one flies forward), Up/Down changes category with a
// vertical glide, and nothing here snaps: the queue position, the category
// and the depth are all spine.h approaches, stepped once per frame.
//
// DECIDED HERE, not by the canvas:
//   * Posters, not stills, in every row: an episode shows its SERIES poster
//     (Continue Watching and Next Up were 16:9 stills on the old shelves).
//     Music albums are square, 1.2x the portrait width.
//   * The focus halo (the canvas's glow + three accent bands) follows the
//     item nearest the focus slot, so it travels with the poster.
//   * The facts fade out while the queue moves and back in as it settles --
//     text under a poster flying 770 px reads as noise.
//   * Items before the focus leave to the left and are not drawn at rest.
//   * The L1 column is drawn AFTER the wave, not before it: the wave's first
//     draw is an opaque gradient (ui_spine.cpp says the same).  True opacity
//     (ui_card_gpu_draw_ex) keeps the wave visible through the receding items.
//   * Coming back from detail (depth 2 -> 1) the queue settles down 40 px
//     into place as it fades up, under the descending spine.

#include <math.h>
#include "thumbnail_cache.h"
#include "ui_wave.h"
#include "api_facts.h"

// --- geometry (authored 1280x720 units) -----------------------------------------

// A slot: where an item at queue distance k sits.  k runs -1 (just left the
// focus) .. 5 (about to enter at the right); k = 0 is the focus.
typedef struct { float x, top, w, h, op, veil, refl; } QBox;

#define Q_KMIN   (-1)
#define Q_KMAX     5
#define Q_NK     (Q_KMAX - Q_KMIN + 1)
#define Q_GAP     24.0f   // between receding items
#define Q_ROW_DY 110.0f   // how far a category travels as it changes
#define Q_VEIL   0x00121E3Cu   // rgba(18,30,60): the canvas's "colder veil"

// L2, portrait: x, bottom, w, h, opacity (canvas "L2 · Category").
static const float Q2_W[Q_NK]   = { 200, 200, 104,  92,  80,  70,  62 };
static const float Q2_H[Q_NK]   = { 300, 300, 156, 138, 120, 105,  93 };
static const float Q2_BOT[Q_NK] = { 566, 566, 552, 550, 548, 545, 543 };
static const float Q2_OP[Q_NK]  = { 0.0f, 1.0f, 0.62f, 0.48f, 0.36f, 0.26f, 0.0f };
static const float Q2_VEIL[Q_NK]= { 0.0f, 0.0f, 0.13f, 0.18f, 0.22f, 0.26f, 0.26f };
// L1, portrait, under the icon (canvas "L1 · Home"): w, h, veil.  Tops chain
// down from the column top with a 14 px gap.
static const float Q1_W[Q_NK]   = { 128, 128, 106,  86,  70,  60,  50 };
static const float Q1_H[Q_NK]   = { 192, 192, 159, 129, 105,  90,  75 };
static const float Q1_OP[Q_NK]  = { 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f };
static const float Q1_VEIL[Q_NK]= { 0.0f, 0.0f, 0.34f, 0.58f, 0.7f, 0.7f, 0.7f };
#define Q1_TOP   318.0f   // ui_spine.cpp's COL_TOP: clears the L1 underline
#define Q1_GAP    14.0f

static float q_kx(void) { return g_uis_pct ? (float)g_uis_pct / 100.0f : (float)display_width  / 1280.0f; }
static float q_ky(void) { return g_uis_pct ? (float)g_uis_pct / 100.0f : (float)display_height / 720.0f; }
static int   q_sx(float ax) { return XMB_OX + (int)floorf(ax * q_kx() + 0.5f); }
static int   q_sy(float ay) { return XMB_OY + (int)floorf(ay * q_ky() + 0.5f); }

// Square rows are 1.2x the portrait width, and as tall as they are wide.
static float q_shape_w(float w, bool sq) { return sq ? w * 1.2f : w; }

// The queue's box for an INTEGER slot k at L2.
static QBox q2_slot(int k, bool sq) {
    const int i = k - Q_KMIN;
    QBox b;
    b.w = q_shape_w(Q2_W[i], sq);
    b.h = sq ? b.w : Q2_H[i];
    b.top = Q2_BOT[i] - b.h;
    b.op = Q2_OP[i];
    b.veil = Q2_VEIL[i];
    b.refl = k <= 0 ? 46.0f : 0.18f * b.h;
    // x: the focus at 64; k=-1 one width and a gap further left; the queue
    // from 836, each item one width and a gap after the last.  A square row's
    // wider focus pushes the facts column right, and the queue with it, so
    // the 520 px column never runs under the first item.
    if (k == 0)      b.x = 64.0f;
    else if (k < 0)  b.x = 64.0f - b.w - 40.0f;
    else {
        float x = 836.0f + (q_shape_w(200.0f, sq) - 200.0f);
        for (int j = 1; j < k; j++) x += q_shape_w(Q2_W[j - Q_KMIN], sq) + Q_GAP;
        b.x = x;
    }
    return b;
}

// The column's box for an INTEGER slot k at L1, centred on the anchor x.
static QBox q1_slot(int k, bool sq, float anchor_x) {
    const int i = k - Q_KMIN;
    QBox b;
    b.w = q_shape_w(Q1_W[i], sq);
    b.h = sq ? b.w : Q1_H[i];
    b.op = Q1_OP[i];
    b.veil = Q1_VEIL[i];
    b.refl = 0.0f;
    b.x = anchor_x - b.w / 2.0f;
    float top = Q1_TOP;
    if (k > 0)
        for (int j = 0; j < k; j++) {
            const float wj = q_shape_w(Q1_W[j - Q_KMIN], sq);
            top += (sq ? wj : Q1_H[j - Q_KMIN]) + Q1_GAP;
        }
    b.top = top;
    return b;
}

static QBox q_lerp_box(const QBox &a, const QBox &b, float t) {
    QBox o;
    o.x    = spine_lerp(a.x, b.x, t);
    o.top  = spine_lerp(a.top, b.top, t);
    o.w    = spine_lerp(a.w, b.w, t);
    o.h    = spine_lerp(a.h, b.h, t);
    o.op   = spine_lerp(a.op, b.op, t);
    o.veil = spine_lerp(a.veil, b.veil, t);
    o.refl = spine_lerp(a.refl, b.refl, t);
    return o;
}

// An item at fractional queue distance k, at swing e (0 = L1, 1 = L2).
static bool q_box(float k, float e, bool sq, float anchor_x, QBox *out) {
    if (k <= (float)Q_KMIN || k >= (float)Q_KMAX) return false;
    int k0 = (int)floorf(k);
    const float f = k - (float)k0;
    const QBox a = q_lerp_box(q1_slot(k0, sq, anchor_x), q2_slot(k0, sq), e);
    const QBox b = q_lerp_box(q1_slot(k0 + 1, sq, anchor_x), q2_slot(k0 + 1, sq), e);
    *out = q_lerp_box(a, b, f);
    return out->op > 0.004f;
}

// --- per-frame state ----------------------------------------------------------------

static spine_approach s_qrow;                  // category, in rows
static spine_approach s_qpos[HOME_ROWS_N];     // each row's queue position
static int            s_row_col[HOME_ROWS_N];  // each row's focused item
static unsigned       s_q_frame = 0;
static bool           s_q_fresh = true;

// A row worth landing on: loading (it may fill) or not empty.
static bool q_visitable(int r) {
    return s_rows[r].kind != HROW_STUB && (!s_rows[r].loaded || s_rows[r].count > 0);
}

static void q_clamp_cols(void) {
    for (int r = 0; r < HOME_ROWS_N; r++) {
        const int n = s_rows[r].count;
        if (s_row_col[r] >= n) s_row_col[r] = n > 0 ? n - 1 : 0;
        if (s_row_col[r] < 0)  s_row_col[r] = 0;
    }
    s_focus_col = s_row_col[s_focus_row];
}

static void q_tick(void) {
    const unsigned id = spine_frame_id();
    if (id == s_q_frame) return;
    const bool snap = s_q_fresh || s_q_frame + 1 < id;
    const unsigned long long dt = spine_frame_dt_us();

    // Landed on a category that turned out empty: move to the nearest one
    // that is not, downward first.
    if (!q_visitable(s_focus_row)) {
        for (int d = 1; d < HOME_ROWS_N; d++) {
            if (s_focus_row + d < HOME_ROWS_N && q_visitable(s_focus_row + d)) { s_focus_row += d; break; }
            if (s_focus_row - d >= 0 && q_visitable(s_focus_row - d))          { s_focus_row -= d; break; }
        }
    }
    q_clamp_cols();

    if (snap) spine_approach_init(&s_qrow, (float)s_focus_row, SPINE_CATEGORY_MS);
    spine_approach_set(&s_qrow, (float)s_focus_row);
    if (!snap) spine_approach_step(&s_qrow, dt);
    for (int r = 0; r < HOME_ROWS_N; r++) {
        if (snap) spine_approach_init(&s_qpos[r], (float)s_row_col[r], SPINE_CATEGORY_MS);
        spine_approach_set(&s_qpos[r], (float)s_row_col[r]);
        if (!snap) spine_approach_step(&s_qpos[r], dt);
    }
    s_q_frame = id;
    s_q_fresh = false;
}

// Everything the three phases agree on for one frame.
typedef struct {
    float e;          // swing: 0 = L1 column, 1 = L2 queue
    float anchor_x;   // authored x of the column (follows the spine row)
    float near;       // the column's nearness to the spine focus (base layer)
    float settle_dy;  // authored px the stage sits low (coming back from detail)
    float settle_a;   // and its fade
    int   clip_top;   // screen px
} QFrame;

static void q_frame(QFrame *q) {
    const float d = spine_depth();
    q->e = spine_ease(spine_clampf(d, 0.0f, 1.0f));
    int cx = 0;
    float near = 1.0f;
    spine_column_anchor(g_active_tab, &cx, &near);
    q->anchor_x = ((float)(cx - XMB_OX)) / q_kx();
    q->near     = spine_lerp(near, 1.0f, q->e);
    const float over = spine_clampf(d - 1.0f, 0.0f, 1.0f);   // detail above
    q->settle_dy = over * 40.0f;
    q->settle_a  = 1.0f - over;
    // At L2 a category gliding up must not cross the divider (y=214).  At
    // L1 there is no divider and the focus halo may reach up toward the
    // label, so the clip opens as the stage swings down.
    q->clip_top = (int)((float)q_sy(216.0f) * q->e);
}

static bool q_square(int r) { return s_rows[r].kind == HROW_SQUARE; }

// Which image stands for an item in the queue: an episode's series poster.
static const char *q_img_id(int r, int c) {
    const XMBItem *it = &s_rows[r].items[c];
    if (strcmp(it->type, "Episode") == 0 && s_extra[r][c].series_id[0])
        return s_extra[r][c].series_id;
    return it->id;
}

static void q_src(int r, int *w, int *h) {
    xmb_queue_src(w, h);
    if (q_square(r)) {
        const int s = (int)sqrtf((float)(*w) * (float)(*h));
        *w = *h = s;
    }
}

// Rows on screen this frame, with their weight (1 at rest) and offset.
typedef struct { int r; float w, dy; } QRow;

static int q_rows(QRow out[2]) {
    const float f = s_qrow.value;
    int r0 = (int)floorf(f);
    int n = 0;
    for (int r = r0; r <= r0 + 1; r++) {
        if (r < 0 || r >= HOME_ROWS_N) continue;
        const float w = 1.0f - fabsf((float)r - f);
        if (w <= 0.01f) continue;
        out[n].r  = r;
        out[n].w  = w;
        out[n].dy = ((float)r - f) * Q_ROW_DY;
        n++;
    }
    return n;
}

// Screen rect of an authored box, offset by dy.
static void q_rect(const QBox &b, float dy, int *x, int *y, int *w, int *h) {
    *x = q_sx(b.x);
    *y = q_sy(b.top + dy);
    *w = (int)floorf(b.w * q_kx() + 0.5f);
    *h = (int)floorf(b.h * q_ky() + 0.5f);
}

static u8 q_a8(float a) {
    a = spine_clampf(a, 0.0f, 1.0f);
    return (u8)(a * 255.0f + 0.5f);
}

// Colour mix, t in 0..1, quantised to sixteenths so a fade reuses cached text
// runs (ui_spine.cpp's mix_q, for the same reason).
static u32 q_mix(u32 a, u32 b, float t) {
    int k = (int)(spine_clampf(t, 0.0f, 1.0f) * 16.0f + 0.5f);
    u32 out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        int ca = (int)((a >> sh) & 0xFF), cb = (int)((b >> sh) & 0xFF);
        out |= (u32)(ca + (cb - ca) * k / 16) << sh;
    }
    return out;
}

// --- GPU phase ------------------------------------------------------------------------

// The canvas's focus treatment: a soft accent glow behind (drawn first), then
// three accent bands fading outward, a faint white line and the 2 px white
// frame.  `a` scales all of it (0..1).
static void q_halo_glow(int x, int y, int w, int h, float a) {
    if (!wave_gpu_blend_ready() || a <= 0.02f) return;
    const u32 c = XMB_ACCENT;
    wave_draw_glow_gpu(x + w / 2, y + h / 2,
                       (int)(0.7f * (w / 2 + UIS_W(100))), (int)(0.7f * (h / 2 + UIS_H(80))),
                       (u8)((c >> 16) & 0xFF), (u8)((c >> 8) & 0xFF), (u8)(c & 0xFF),
                       q_a8(0.2f * a));
}

static void q_ring(int x, int y, int w, int h, int out, int t, u32 c, float a) {
    const u8 al = q_a8(a);
    if (!al || t <= 0) return;
    const int X = x - out, Y = y - out, W = w + 2 * out, H = h + 2 * out;
    ui_rect_gpu_draw(X, Y, W, t, c, al);
    ui_rect_gpu_draw(X, Y + H - t, W, t, c, al);
    ui_rect_gpu_draw(X, Y + t, t, H - 2 * t, c, al);
    ui_rect_gpu_draw(X + W - t, Y + t, t, H - 2 * t, c, al);
}

static void q_halo_bands(int x, int y, int w, int h, float a) {
    if (a <= 0.02f) return;
    const u32 acc = XMB_ACCENT;
    q_ring(x, y, w, h, UIS_W(24), UIS_W(8), acc, 0.07f * a);
    q_ring(x, y, w, h, UIS_W(16), UIS_W(6), acc, 0.14f * a);
    q_ring(x, y, w, h, UIS_W(9),  UIS_W(6), acc, 0.24f * a);
    q_ring(x, y, w, h, UIS_W(3),  1,        XMB_WHITE, 0.27f * a);
    q_ring(x, y, w, h, 0,         UIS_W(2) > 2 ? UIS_W(2) : 2, XMB_WHITE, a);
}

// One item: the image at its opacity, the veil, the reflection.
static void q_card_gpu(int r, int c, const QBox &b, float dy, float alpha) {
    int x, y, w, h;
    q_rect(b, dy, &x, &y, &w, &h);
    if (w <= 0 || h <= 0 || x >= (int)display_width || x + w <= 0) return;
    const float op = b.op * alpha;
    int sw, sh;
    q_src(r, &sw, &sh);
    u32 off = 0, pitch = 0;
    const bool have = thumb_gpu_texture(q_img_id(r, c), sw, sh, &off, &pitch,
                                        THUMB_IMG_PRIMARY);
    if (have)
        ui_card_gpu_draw_ex(off, (u32)sw, (u32)sh, pitch, x, y, w, h, 0.0f, 1.0f, q_a8(op));
    else
        ui_rect_gpu_draw(x, y, w, h, XMB_THUMB_DIM, q_a8(op));
    if (b.veil > 0.01f) ui_rect_gpu_draw(x, y, w, h, Q_VEIL, q_a8(b.veil * alpha));

    // The reflection: the image's bottom rows, mirrored, fading out in six
    // strips at 0.34 x the item's opacity (the canvas's 46 px gradient).
    const int rh = (int)floorf(b.refl * q_ky() + 0.5f);
    if (have && rh >= 4) {
        const int N = 6, gap = UIS_H(4) > 0 ? UIS_H(4) : 1;
        const float span = (float)rh / (float)h;       // texture rows covered
        for (int i = 0; i < N; i++) {
            const int y0 = y + h + gap + rh * i / N, y1 = y + h + gap + rh * (i + 1) / N;
            if (y1 <= y0) continue;
            const float v0 = 1.0f - span * (float)i / N, v1 = 1.0f - span * (float)(i + 1) / N;
            const float ra = 0.34f * op * (1.0f - ((float)i + 0.5f) / N);
            ui_card_gpu_draw_ex(off, (u32)sw, (u32)sh, pitch, x, y0, w, y1 - y0, v0, v1, q_a8(ra));
        }
    }
}

// The facts column's layout: shared by the GPU (chip, progress) and the text
// phase so the two cannot drift apart.
typedef struct {
    int   x, top;
    bool  tech;          // this kind of item has streams to describe
    int   y_eye, y_title, y_meta, y_lab, y_val, y_alab, y_chip, chip_h, y_prog, y_watch;
    float a;             // overall opacity
} QFacts;

static void q_facts_layout(int r, float dy, float alpha, float e, QFacts *f) {
    const bool sq = q_square(r);
    const float drift = (1.0f - spine_clampf((e - 0.5f) * 2.0f, 0.0f, 1.0f)) * 20.0f;
    f->x   = q_sx(64.0f + q_shape_w(200.0f, sq) + 40.0f + drift);
    f->top = 266;
    const int c = s_row_col[r];
    f->tech = s_rows[r].count > 0 && facts_wanted(s_rows[r].items[c].type);
    const float T = 266.0f + dy;
    f->y_eye   = q_sy(T);
    f->y_title = q_sy(T + 29);
    f->y_meta  = q_sy(T + 80);
    f->y_lab   = q_sy(T + 124);
    f->y_val   = q_sy(T + 142);
    f->y_alab  = q_sy(T + 174);
    f->y_chip  = q_sy(T + 189);
    f->chip_h  = UIS_H(25);
    f->y_prog  = q_sy(T + (f->tech ? 236 : 120));
    f->y_watch = q_sy(T + (f->tech ? 249 : 133));
    // Out while the queue moves or the stage is still swinging; in as it
    // settles.
    const float settle = spine_clampf(1.0f - fabsf(s_qpos[r].value - (float)c) * 3.0f, 0.0f, 1.0f);
    const float lvl    = spine_clampf((spine_depth() - 0.55f) / 0.45f, 0.0f, 1.0f);
    f->a = alpha * settle * lvl;
}

// Watched fraction and runtime for the focused item, or false.
static bool q_progress(const XMBItem *it, const ItemFacts *fx, bool hfx,
                       float *frac, u32 *runtime) {
    if (it->resume_secs < 10) return false;
    u32 rt = hfx ? fx->runtime_secs : 0;
    if (!rt && it->progress_pct > 0) rt = it->resume_secs * 100u / it->progress_pct;
    if (!rt || rt <= it->resume_secs) return false;
    *frac = (float)it->resume_secs / (float)rt;
    *runtime = rt;
    return true;
}

static void q_facts_gpu(int r, float dy, float alpha, float e) {
    if (s_rows[r].count <= 0) return;
    QFacts f;
    q_facts_layout(r, dy, alpha, e, &f);
    if (f.a <= 0.02f) return;
    const XMBItem *it = &s_rows[r].items[s_row_col[r]];
    ItemFacts fx;
    unsigned long long age = 0;
    const bool hfx = facts_get(it->id, &fx, &age);

    // The audio chip: outlined in the accent for lossless, hairline for not.
    if (f.tech && hfx && fx.audio[0]) {
        const float ta = f.a * spine_clampf((float)age / 250000.0f, 0.0f, 1.0f);
        const float px = UIS_TF(11.0f);
        const int cw = ttf_text_width_face(fx.audio, px, UI_FACE_SPEC) + UIS_W(20);
        wave_draw_rrect_outline_gpu(f.x, f.y_chip, cw, f.chip_h, f.chip_h / 2, 1,
                                    fx.lossless ? XMB_ACCENT_ALT : XMB_HAIRLINE, q_a8(ta),
                                    XMB_BG, XMB_BG, q_a8(ta));
    }
    // Progress: 320x4, the file's blue (the canvas's L1 and L2 frames).
    float frac;
    u32 rt;
    if (q_progress(it, &fx, hfx, &frac, &rt)) {
        const int pw = UIS_W(320), ph = UIS_H(4) > 2 ? UIS_H(4) : 2;
        ui_rect_gpu_draw(f.x, f.y_prog, pw, ph, XMB_TRACK, q_a8(f.a));
        const int fw = (int)((float)pw * frac + 0.5f);
        if (fw > 0) ui_rect_gpu_draw(f.x, f.y_prog, fw, ph, XMB_ACCENT_ALT, q_a8(f.a));
    }
}

void xmb_home_stage_gpu(void) {
    if (!ui_card_gpu_ready()) return;
    home_init_once();
    q_tick();
    QFrame q;
    q_frame(&q);
    ui_card_gpu_clip(q.clip_top, 0);

    QRow rows[2];
    const int nr = q_rows(rows);
    for (int ri = 0; ri < nr; ri++) {
        const int r = rows[ri].r;
        const HomeRow *row = &s_rows[r];
        if (row->count <= 0) continue;
        const bool sq = q_square(r);
        const float dy = rows[ri].dy * q.e + q.settle_dy;
        const float alpha = rows[ri].w * q.near * q.settle_a;
        const float pos = s_qpos[r].value;
        const int c0 = (int)floorf(pos) + Q_KMIN, c1 = (int)floorf(pos) + Q_KMAX;

        // Halo glows first: they sit behind every card.
        for (int c = c0; c <= c1; c++) {
            if (c < 0 || c >= row->count) continue;
            const float k = (float)c - pos, hs = 1.0f - fabsf(k) * 1.6f;
            QBox b;
            if (hs <= 0.0f || !q_box(k, q.e, sq, q.anchor_x, &b)) continue;
            int x, y, w, h;
            q_rect(b, dy, &x, &y, &w, &h);
            q_halo_glow(x, y, w, h, hs * alpha);
        }
        // Cards far to near, so the focus lands on top of anything it passes.
        for (int c = c1; c >= c0; c--) {
            if (c < 0 || c >= row->count) continue;
            QBox b;
            if (!q_box((float)c - pos, q.e, sq, q.anchor_x, &b)) continue;
            q_card_gpu(r, c, b, dy, alpha);
        }
        // Bands and the frame over the focus, plus the L1 progress strip,
        // which hands over to the facts column's bar as the stage swings.
        for (int c = c0; c <= c1; c++) {
            if (c < 0 || c >= row->count) continue;
            const float k = (float)c - pos, hs = 1.0f - fabsf(k) * 1.6f;
            QBox b;
            if (hs <= 0.0f || !q_box(k, q.e, sq, q.anchor_x, &b)) continue;
            int x, y, w, h;
            q_rect(b, dy, &x, &y, &w, &h);
            q_halo_bands(x, y, w, h, hs * alpha);
            const XMBItem *it = &row->items[c];
            if (it->progress_pct > 0 && q.e < 0.99f) {
                const int sh = UIS_H(4) > 2 ? UIS_H(4) : 2;
                const float sa = hs * alpha * (1.0f - q.e);
                ui_rect_gpu_draw(x, y + h - sh, w, sh, XMB_TRACK, q_a8(sa));
                ui_rect_gpu_draw(x, y + h - sh, w * it->progress_pct / 100, sh,
                                 XMB_ACCENT_ALT, q_a8(sa));
            }
        }
        q_facts_gpu(r, dy, alpha, q.e);
    }
    ui_card_gpu_clip(0, 0);
}

// --- CPU phase ------------------------------------------------------------------------
//
// Thumbnail requests (the cache is driven by what is asked for each frame)
// and, only when the RSX card path is off, a plain opaque fallback.

void xmb_home_stage_cpu(void) {
    home_init_once();
    home_step_load();
    q_tick();
    if (g_play_gen != s_seen_gen) home_mark_dynamic_stale();

    // Requests: the focused row from one behind to two past what is drawn,
    // and the neighbouring rows' first few, so Up/Down lands on posters.
    for (int r = 0; r < HOME_ROWS_N; r++) {
        const HomeRow *row = &s_rows[r];
        if (row->count <= 0) continue;
        const int col = s_row_col[r];
        const bool focus = r == s_focus_row;
        if (!focus && (r < s_focus_row - 1 || r > s_focus_row + 1)) continue;
        const int lo = col - 1, hi = col + (focus ? Q_KMAX + 1 : 2);
        int sw, sh;
        q_src(r, &sw, &sh);
        for (int c = lo; c <= hi; c++)
            if (c >= 0 && c < row->count)
                thumb_request(q_img_id(r, c), sw, sh, THUMB_IMG_PRIMARY);
        if (focus && facts_wanted(row->items[col].type) &&
            fabsf(s_qpos[r].value - (float)col) < 0.35f)
            facts_request(row->items[col].id);
    }

    if (ui_card_gpu_ready()) return;

    // The fallback: opaque cards where they are wholly on screen.
    QFrame q;
    q_frame(&q);
    g_cpu_clip_top = q.clip_top;
    QRow rows[2];
    const int nr = q_rows(rows);
    for (int ri = 0; ri < nr; ri++) {
        const int r = rows[ri].r;
        const HomeRow *row = &s_rows[r];
        const bool sq = q_square(r);
        const float dy = rows[ri].dy * q.e + q.settle_dy;
        const float pos = s_qpos[r].value;
        int sw, sh;
        q_src(r, &sw, &sh);
        for (int c = (int)floorf(pos) + Q_KMAX; c >= (int)floorf(pos) + Q_KMIN; c--) {
            if (c < 0 || c >= row->count) continue;
            QBox b;
            if (!q_box((float)c - pos, q.e, sq, q.anchor_x, &b) || b.op < 0.3f) continue;
            int x, y, w, h;
            q_rect(b, dy, &x, &y, &w, &h);
            if (x < 0 || y < 0 || x + w > (int)display_width || y + h > (int)display_height) continue;
            xmb_draw_card_src(q_img_id(r, c), sw, sh, x, y, w, h, 0,
                              c == s_row_col[r] && r == s_focus_row, NULL, THUMB_IMG_PRIMARY);
        }
    }
    g_cpu_clip_top = 0;
}

// --- text -------------------------------------------------------------------------------

// Cut `s` to fit max_w at (px, face), with "..." -- walking back to a UTF-8
// lead byte so a character is never split.
static void q_fit(char *out, size_t cap, const char *s, float px, int face, int max_w) {
    snprintf(out, cap, "%s", s);
    if (ttf_text_width_face(out, px, face) <= max_w) return;
    size_t len = strlen(out);
    while (len > 0) {
        do { len--; } while (len > 0 && ((unsigned char)out[len] & 0xC0) == 0x80);
        if (len + 4 > cap) continue;
        memcpy(out + len, "...", 4);
        if (ttf_text_width_face(out, px, face) <= max_w) return;
    }
}

// What the item is called on this screen: an episode goes by its series.
static const char *q_title(int r, int c) {
    const XMBItem *it = &s_rows[r].items[c];
    if (strcmp(it->type, "Episode") == 0 && s_extra[r][c].series_name[0])
        return s_extra[r][c].series_name;
    return it->name;
}

// The meta row, "a · b · c", separators at half strength.  Returns nothing;
// clipped to max_w by dropping trailing parts.
static void q_meta(int x, int y, int r, int c, float px, u32 col, u32 sep_col, int max_w) {
    const XMBItem *it = &s_rows[r].items[c];
    const HomeExtra *x2 = &s_extra[r][c];
    const char *part[6];
    char se[24] = "";
    int n = 0;
    if (strcmp(it->type, "Episode") == 0) {
        if (x2->season || x2->episode) {
            snprintf(se, sizeof se, "S%d E%d", x2->season, x2->episode);
            part[n++] = se;
        }
        if (x2->series_name[0]) part[n++] = it->name;   // the episode's own name
        if (it->duration_str[0]) part[n++] = it->duration_str;
    } else if (strcmp(it->type, "MusicAlbum") == 0) {
        if (it->artist[0])       part[n++] = it->artist;
        if (it->year_str[0])     part[n++] = it->year_str;
        if (it->duration_str[0]) part[n++] = it->duration_str;
    } else {
        if (it->year_str[0])     part[n++] = it->year_str;
        if (it->duration_str[0]) part[n++] = it->duration_str;
        if (x2->rating[0])       part[n++] = x2->rating;
        if (it->genre[0])        part[n++] = it->genre;
    }
    const int gap = UIS_W(10);
    const int sw = ttf_text_width("\xC2\xB7", px);
    int cx = x;
    for (int i = 0; i < n; i++) {
        char buf[132];
        snprintf(buf, sizeof buf, "%s", part[i]);
        int w = ttf_text_width(buf, px);
        if (cx + w > x + max_w) {
            if (i == 0) {   // the first part always shows, cut to fit
                q_fit(buf, sizeof buf, part[i], px, UI_FACE_REGULAR, max_w);
                drawTTF((u32)cx, (u32)y, buf, px, col);
            }
            break;
        }
        if (i) {
            drawTTF((u32)cx, (u32)y, "\xC2\xB7", px, sep_col);
            cx += sw + gap;
        }
        drawTTF((u32)cx, (u32)y, buf, px, col);
        cx += w + gap;
    }
}

static void q_hms(char *out, size_t cap, u32 s) {
    snprintf(out, cap, "%u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
}

static void q_facts_text(int r, float dy, float alpha, float e) {
    QFacts f;
    q_facts_layout(r, dy, alpha, e, &f);
    const float lvl = spine_clampf((spine_depth() - 0.55f) / 0.45f, 0.0f, 1.0f);
    const HomeRow *row = &s_rows[r];
    const int max_w = UIS_W(520);
    char line[160];

    // The eyebrow is the category and stays up through a queue move; the
    // rest is the focused item's and waits for it to settle.
    const float ea = alpha * lvl;
    if (ea > 0.02f) {
        const int adv = xmb_draw_eyebrow(f.x, f.y_eye, row->title,
                                         q_mix(XMB_BG, XMB_ACCENT_ALT, ea));
        if (row->count > 0) {
            snprintf(line, sizeof line, "%d of %d", s_row_col[r] + 1, row->count);
            drawTTF_face((u32)(f.x + adv + UIS_W(12)), (u32)f.y_eye, line, UIS_TF(11.0f),
                         q_mix(XMB_BG, XMB_TEXT_FAINT, ea), UI_FACE_SPEC);
        }
    }
    if (row->count <= 0) {
        if (ea > 0.02f)
            drawTTF_face((u32)f.x, (u32)f.y_title, row->loaded ? "Nothing here yet" : "Loading...",
                         UIS_TF(36.0f), q_mix(XMB_BG, XMB_TEXT_DIM, ea), UI_FACE_DISPLAY);
        return;
    }
    if (f.a <= 0.02f) return;

    const int c = s_row_col[r];
    const XMBItem *it = &row->items[c];
    const float tpx = UIS_TF(36.0f);
    q_fit(line, sizeof line, q_title(r, c), tpx, UI_FACE_DISPLAY, max_w);
    drawTTF_face((u32)f.x, (u32)f.y_title, line, tpx, q_mix(XMB_BG, XMB_TEXT, f.a), UI_FACE_DISPLAY);
    q_meta(f.x, f.y_meta, r, c, UIS_TF(13.5f), q_mix(XMB_BG, XMB_TEXT_DIM, f.a),
           q_mix(XMB_BG, XMB_TEXT_DIM, f.a * 0.5f), max_w);

    ItemFacts fx;
    unsigned long long age = 0;
    const bool hfx = facts_get(it->id, &fx, &age);
    if (f.tech && hfx) {
        const float ta = f.a * spine_clampf((float)age / 250000.0f, 0.0f, 1.0f);
        const float lpx = UIS_TF(9.0f), vpx = UIS_TF(11.0f), trk = lpx * 0.1f;
        const u32 lc = q_mix(XMB_BG, XMB_TEXT_FAINT, ta), vc = q_mix(XMB_BG, XMB_TEXT_DIM, ta);
        const char *lab[3] = { "CONTAINER", "VIDEO", "SUBTITLES" };
        const char *val[3] = { fx.container[0] ? fx.container : "-",
                               fx.video[0] ? fx.video : "-", fx.subs };
        int x = f.x;
        for (int i = 0; i < 3; i++) {
            drawTTF_tracked((u32)x, (u32)f.y_lab, lab[i], lpx, lc, UI_FACE_SPEC, trk);
            drawTTF_face((u32)x, (u32)f.y_val, val[i], vpx, vc, UI_FACE_SPEC);
            const int lw = ttf_text_width_tracked(lab[i], lpx, UI_FACE_SPEC, trk);
            const int vw = ttf_text_width_face(val[i], vpx, UI_FACE_SPEC);
            x += (lw > vw ? lw : vw) + UIS_W(40);
        }
        if (fx.audio[0]) {
            drawTTF_tracked((u32)f.x, (u32)f.y_alab, "AUDIO", lpx, lc, UI_FACE_SPEC, trk);
            const int ty = f.y_chip + (f.chip_h - (int)vpx) / 2 - UIS_H(1);
            drawTTF_face((u32)(f.x + UIS_W(10)), (u32)ty, fx.audio, vpx,
                         q_mix(XMB_BG, fx.lossless ? XMB_ACCENT_ALT : XMB_TEXT_DIM, ta),
                         UI_FACE_SPEC);
            if (fx.audio_more[0]) {
                const int cw = ttf_text_width_face(fx.audio, vpx, UI_FACE_SPEC) + UIS_W(20);
                drawTTF_face((u32)(f.x + cw + UIS_W(12)), (u32)ty, fx.audio_more, vpx,
                             q_mix(XMB_BG, XMB_TEXT_FAINT, ta), UI_FACE_SPEC);
            }
        }
    }

    float frac;
    u32 rt;
    if (q_progress(it, &fx, hfx, &frac, &rt)) {
        char el[16];
        q_hms(el, sizeof el, it->resume_secs);
        const u32 left = rt - it->resume_secs;
        if (left >= 3600) snprintf(line, sizeof line, "%s WATCHED \xC2\xB7 %uH %02uM LEFT",
                                   el, left / 3600, (left / 60) % 60);
        else              snprintf(line, sizeof line, "%s WATCHED \xC2\xB7 %uM LEFT",
                                   el, left / 60 > 0 ? left / 60 : 1);
        drawTTF_face((u32)f.x, (u32)f.y_watch, line, UIS_TF(11.0f),
                     q_mix(XMB_BG, XMB_TEXT_FAINT, f.a), UI_FACE_SPEC);
    }
}

// L1's title block, beside the column (ui_spine.cpp's spine_column_text,
// which this replaces for Home): eyebrow and position, a 30 px title, one
// meta line.  Fades out over the first half of the swing.
static void q_column_text(const QFrame &q) {
    const float a = q.near * spine_clampf(1.0f - spine_depth() * 2.2f, 0.0f, 1.0f);
    if (a <= 0.02f) return;
    const int r = s_focus_row;
    const HomeRow *row = &s_rows[r];
    const bool sq = q_square(r);
    const float art_right = q.anchor_x + q_shape_w(128.0f, sq) / 2.0f;
    const int tx = q_sx(art_right + 36.0f);
    const int max_w = (int)display_width - XMB_ITEM_PAD - tx;
    int ty = q_sy(Q1_TOP + 22.0f);
    if (max_w <= 0) return;

    char line[160];
    const int adv = xmb_draw_eyebrow(tx, ty, row->count > 0 ? row->title : "Home",
                                     q_mix(XMB_BG, XMB_ACCENT_ALT, a));
    if (row->count > 0) {
        snprintf(line, sizeof line, "%d/%d", s_row_col[r] + 1, row->count);
        drawTTF_face((u32)(tx + adv + UIS_W(12)), (u32)ty, line, UIS_TF(11.0f),
                     q_mix(XMB_BG, XMB_TEXT_FAINT, a), UI_FACE_SPEC);
    }
    ty += UIS_H(24);
    const float tpx = UIS_TF(30.0f);
    const char *title = row->count > 0 ? q_title(r, s_row_col[r])
                      : row->loaded ? "Nothing here yet" : "Loading...";
    q_fit(line, sizeof line, title, tpx, UI_FACE_DISPLAY, max_w);
    drawTTF_face((u32)tx, (u32)ty, line, tpx, q_mix(XMB_BG, XMB_TEXT, a), UI_FACE_DISPLAY);
    ty += (int)(tpx * 1.35f);
    if (row->count > 0)
        q_meta(tx, ty, r, s_row_col[r], UIS_TF(14.0f), q_mix(XMB_BG, XMB_TEXT_DIM, a),
               q_mix(XMB_BG, XMB_TEXT_DIM, a * 0.5f), max_w);
}

void xmb_home_stage_text(void) {
    home_init_once();
    q_tick();
    QFrame q;
    q_frame(&q);
    g_cpu_clip_top = q.clip_top;
    q_column_text(q);
    QRow rows[2];
    const int nr = q_rows(rows);
    for (int ri = 0; ri < nr; ri++) {
        const float dy = rows[ri].dy * q.e + q.settle_dy;
        q_facts_text(rows[ri].r, dy, rows[ri].w * q.settle_a, q.e);
    }
    g_cpu_clip_top = 0;
}

// --- input ------------------------------------------------------------------------------

bool xmb_home_at_top(void) {
    if (!g_spine_on) return s_focus_row == 0;
    for (int r = s_focus_row - 1; r >= 0; r--)
        if (q_visitable(r)) return false;
    return true;
}

bool xmb_home_open_focused(void) {
    home_init_once();
    const HomeRow *row = &s_rows[s_focus_row];
    const int c = g_spine_on ? s_row_col[s_focus_row] : s_focus_col;
    if (row->count <= 0 || c < 0 || c >= row->count) return false;
    const XMBItem *it = &row->items[c];
    if (strcmp(it->type, "Series") == 0) {
        spine_set_level(SPINE_L2);          // its seasons live inside the TV tab
        if (xmb_open_series(it)) init_btns();
        return true;
    }
    if (strcmp(it->type, "MusicAlbum") == 0) {
        music_screen_open_album(it, "Home");
        init_btns();
        return true;
    }
    XMBItem copy = *it;                     // the rows may refetch under detail
    xmb_show_item_info(&copy);
    init_btns();
    return true;
}

static bool home_queue_input(void) {
    if (BTN_PRESSED(l1)) { xmb_switch_tab(xmb_next_enabled(g_active_tab, -1)); return false; }
    if (BTN_PRESSED(r1)) { xmb_switch_tab(xmb_next_enabled(g_active_tab, +1)); return false; }

    if (BTN_REPEAT(up)) {
        for (int r = s_focus_row - 1; r >= 0; r--)
            if (q_visitable(r)) { s_focus_row = r; break; }
    }
    if (BTN_REPEAT(down)) {
        for (int r = s_focus_row + 1; r < HOME_ROWS_N; r++)
            if (q_visitable(r)) { s_focus_row = r; break; }
    }
    const int n = s_rows[s_focus_row].count;
    int *col = &s_row_col[s_focus_row];
    if (BTN_REPEAT(right) && *col + 1 < n) (*col)++;
    if (BTN_REPEAT(left)  && *col > 0)     (*col)--;
    s_focus_col = *col;

    // X opens; so does Triangle, which used to mean "info" -- detail IS the
    // info now.
    if (BTN_PRESSED(cross) || BTN_PRESSED(triangle)) xmb_home_open_focused();
    return false;
}
