// The spine, drawn and driven -- SPINE-PLAN.md S1 plus the base layer of S4.
//
// README 2.9 of the 2026-09-21 export replaces the tab strip with a spine: one
// left-biased row of category icons, the active slot fixed at x=230, the rest
// trailing off to the right and fading by distance.  All of its geometry is in
// spine.h and host-tested there; nothing in this file decides a number the
// design gives.  What IS decided here is marked as such.
//
// TWO LEVELS.  Navigation is the PS3 XMB's, and 1etu/XMP's (MIT; its
// xmb-plugin/nav.ts is the clearest statement of it): a root row the user can
// always get back to, with each category entered downward.
//
//   base  Left/Right (and L1/R1) walk the tabs; the active tab's first items
//         hang in a column under its icon, with the focused one's title
//         beside it -- the design's L1, "names the thing".  Down or X enters.
//   tab   The tab's own screen, untouched, under the depth-1 spine.  Up from
//         its top row, or O at its root, goes back to base.  Sub-screens (a
//         series' seasons, a collection, the jump bar, the OSK) keep their
//         own Up and O; only the tab's ROOT hands them to the spine.
//
// MOTION is the XMB's own approach (spine.h; gains from XMP's verified
// table): the row glides between tabs, the depth drifts between levels, and
// nothing about the spine or the column snaps.  The tab's own screen takes
// over at the halfway point of a level move and glides up the rest of the way
// (spine_content_dy); the focus ring travels between cards and the grid eases
// between rows (the end of this file).  The divider fades in only once the
// label stack has cleared it (spine_eval()).
//
// COST.  Nothing here reads video memory: icons are cached runs with their
// opacity in the run colour, the label is one short tracked string, the
// underline is an opaque rect, the glows are three 14-vertex fans, and the
// column is at most two card quads plus one blended dimming rect.

#include <stdio.h>
#include <string.h>

#include "ui.h"
#include "ui_visuals.h"
#include "ui_internal.h"
#include "ui_render_internal.h"
#include "ui_card_gpu.h"
#include "ui_wave.h"
#include "ui_spine.h"
#include "spine.h"
#include "icons.h"
#include "jf_paths.h"
#include "timing.h"
#include "plog.h"
#include "music_screen.h"

extern void crash_log(const char *msg);   // main.cpp; survives a GPU wedge

bool g_spine_on = false;

#define SPINE_FILE "jellyfin_spine.txt"

static int            s_level = SPINE_L1;  // where the user is going
static spine_approach s_dep;               // how far they have got (0..1)
static spine_approach s_cat;               // the row's position, in slots
static unsigned long long s_last_us = 0;
static bool           s_cat_fresh = true;  // snap the row on the first frame

void spine_load(void) {
    int v = 0;
    FILE *f = fopen(jf_data_path(SPINE_FILE), "r");
    if (f) {
        if (fscanf(f, "%d", &v) != 1) v = 0;
        fclose(f);
    }
    g_spine_on = (v == 1);
    s_level    = SPINE_L1;
    spine_approach_init(&s_dep, (float)SPINE_L1, SPINE_LEVEL_MS);
    spine_approach_init(&s_cat, 0.0f, SPINE_CATEGORY_MS);
    s_cat_fresh = true;
    s_last_us   = 0;
    plog(g_spine_on ? "spine: ON (jellyfin_spine.txt = 1) -- base layer + tabs, approach motion"
                    : "spine: off -- tab strip");
    crash_log(g_spine_on ? "spine on" : "spine off");
}

static int spine_row(int *order, int *active);

// Both animated values are stepped ONCE per frame, here, by the real elapsed
// time -- so the GPU and CPU phases of one frame agree on where everything
// is, and the motion keeps its speed when the frame rate does not.
static unsigned           s_frame_id = 1;
static unsigned long long s_frame_dt = 0;

unsigned           spine_frame_id(void)    { return s_frame_id; }
unsigned long long spine_frame_dt_us(void) { return s_frame_dt; }

void spine_frame_begin(void) {
    const unsigned long long now = timing_get_us();
    const unsigned long long dt  = s_last_us ? now - s_last_us : 0;
    s_last_us  = now;
    s_frame_dt = dt;
    s_frame_id++;

    int order[XMB_TAB_COUNT], a;
    if (spine_row(order, &a) > 0 && a >= 0) {
        if (s_cat_fresh) { s_cat.value = (float)a; s_cat_fresh = false; }
        spine_approach_set(&s_cat, (float)a);
    }
    spine_approach_step(&s_dep, dt);
    spine_approach_step(&s_cat, dt);
}

float spine_depth(void) {
    if (!g_spine_on) return (float)SPINE_L2;
    return s_dep.value;
}

bool spine_at_base(void) {
    return g_spine_on && spine_depth() < 0.5f;
}

float spine_divider_alpha(void) {
    if (!g_spine_on) return 1.0f;
    return spine_eval(spine_depth()).divider_a;
}

static void spine_go(int level) {
    if (level == s_level) return;
    s_level = level;
    spine_approach_set(&s_dep, (float)level);
}

int spine_target_level(void) { return g_spine_on ? s_level : SPINE_L2; }

void spine_set_level(int level) {
    if (!g_spine_on) return;
    if (level < SPINE_L1) level = SPINE_L1;
    if (level > SPINE_L3) level = SPINE_L3;
    spine_go(level);
}

// --- the row ------------------------------------------------------------------

// Same glyph per tab KIND as the tab strip -- ui_widgets.cpp's tab_icon() is
// file-static there, and duplicating eight cases is cheaper than widening its
// linkage for a path that may yet replace it outright.
static int spine_icon(int tab) {
    switch (xmb_kind(tab)) {
    case TABKIND_SEARCH:    return ICON_SEARCH;
    case TABKIND_HOME:      return ICON_HOME;
    case TABKIND_MOVIES:    return ICON_MOVIE;
    case TABKIND_TV:        return ICON_TV;
    case TABKIND_MUSIC:     return ICON_MUSIC;
    case TABKIND_PLAYLISTS: return ICON_MUSIC;
    case TABKIND_BOXSETS:   return ICON_COLLECTIONS;
    case TABKIND_SETTINGS:  return ICON_SETTINGS;
    default:                return ICON_PHOTO;
    }
}

// Idle icons are "sunk to #39406a" (README 2.9).  That is not a theme token,
// and hardcoding it would leave a blue row under Golden Age.  It IS, to within
// 3 per channel, 25% of the way from `hairline` (2C3258) to `icon_idle`
// (646C96) -- 3A4067 against 39406A -- so it is derived from those two, and
// follows whichever theme is live.
static u32 spine_idle_colour(void) {
    const u32 a = XMB_HAIRLINE, b = XMB_ICON_IDLE;
    u32 out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        int ca = (int)((a >> sh) & 0xFF), cb = (int)((b >> sh) & 0xFF);
        out |= (u32)(ca + (cb - ca) / 4) << sh;
    }
    return out;
}

// The row in display order and where the active tab sits in it.  -1 when the
// active tab is somehow not in the order (it should always be).
static int spine_row(int *order, int *active) {
    int n = xmb_tab_order(order);
    *active = -1;
    for (int i = 0; i < n; i++)
        if (order[i] == g_active_tab) { *active = i; break; }
    return n;
}

static int sx(float authored_x) { return XMB_OX + UIS_W((int)authored_x); }
static int sy(float authored_y) { return XMB_OY + UIS_H((int)authored_y); }

// --- input ----------------------------------------------------------------------

// Left/Right at base.  CLAMPED at the ends, not wrapped: the row is a line
// with a start and an end (XMP's sweep() clamps too), and wrapping would
// fling the whole row across the screen in one frame.
static void spine_step_tab(int dir) {
    int order[XMB_TAB_COUNT], a;
    const int n = spine_row(order, &a);
    if (a < 0) return;
    const int to = a + dir;
    if (to < 0 || to >= n) return;
    xmb_switch_tab(order[to]);
}

// X at base OPENS the item the column is showing (the canvas's "X always
// opens the item -- playback only ever starts from detail"); Down enters the
// tab.  Where there is no item to open (Search, Settings, a tab still
// loading) X enters too.  A series opens its seasons, which live inside the
// tab, so it enters first; an album opens the music screen.
static void spine_open_column_item(int tab) {
    const int k = xmb_kind(tab);
    if (k == TABKIND_HOME) {
        if (!xmb_home_open_focused()) spine_go(SPINE_L2);
        return;
    }
    if (k == TABKIND_SEARCH || k == TABKIND_SETTINGS ||
        !g_items_loaded[tab] || g_item_count[tab] <= 0) {
        spine_go(SPINE_L2);
        return;
    }
    const XMBItem *it = &g_items[tab][0];
    if (strcmp(it->type, "Series") == 0) {
        spine_go(SPINE_L2);
        if (xmb_open_series(it)) init_btns();
    } else if (strcmp(it->type, "MusicAlbum") == 0) {
        music_screen_open_album(it, g_tabs[tab].label);
        init_btns();
    } else if (strcmp(it->type, "BoxSet") == 0 || strcmp(it->type, "Folder") == 0 ||
               strcmp(it->type, "Playlist") == 0 || strcmp(it->type, "MusicArtist") == 0 ||
               strcmp(it->type, "MusicGenre") == 0) {
        spine_go(SPINE_L2);         // containers open inside their tab
    } else {
        xmb_show_item_info(it);
        init_btns();
    }
}

bool spine_input_base(void) {
    if (BTN_REPEAT(left)  || BTN_PRESSED(l1)) { spine_step_tab(-1); return false; }
    if (BTN_REPEAT(right) || BTN_PRESSED(r1)) { spine_step_tab(+1); return false; }
    if (BTN_PRESSED(down))  { spine_go(SPINE_L2); return false; }
    if (BTN_PRESSED(cross)) spine_open_column_item(g_active_tab);
    return false;
}

bool spine_try_back(int tab) {
    if (!g_spine_on || s_level != SPINE_L2) return false;
    const bool up = BTN_PRESSED(up), back = BTN_PRESSED(circle);
    if (!up && !back) return false;

    // A sub-screen or a modal owns its own Up / O.  Only a tab's root hands
    // them over.  PRESSED, not REPEAT, on purpose: holding Up to scroll a long
    // grid must stop at its top row, not carry on out of the tab.
    if (g_tv_depth > 0 || g_col_depth > 0 || g_music_depth > 0) return false;
    if (g_jumpbar_active) return false;

    bool leave = false;
    switch (xmb_kind(tab)) {
    case TABKIND_SEARCH:
        // O already means "clear" on the keyboard, so only Up leaves, and
        // only from the keyboard's top row.
        leave = up && !g_search_focus_results && g_osk_row == 0;
        break;
    case TABKIND_SETTINGS:
        if (g_settings_confirm || g_overscan_calib) return false;
        leave = back || (up && g_settings_sel == 0);
        break;
    case TABKIND_HOME:
        leave = back || (up && xmb_home_at_top());
        break;
    case TABKIND_MUSIC:
        // The grid's top row already moves Up onto the Albums/Artists/...
        // header, so the header is this tab's top row.
        leave = back || (up && g_music_header);
        break;
    default: {
        GridGeom gg;
        xmb_grid_geom(tab, &gg);
        leave = back || (up && g_sel < gg.cols && g_tab_start[tab] == 0);
        break;
    }
    }
    if (!leave) return false;
    spine_go(SPINE_L1);
    return true;
}

// --- the spine itself -------------------------------------------------------------
//
// EVERYTHING HERE IS A FUNCTION OF TWO APPROACHED VALUES: the row's position
// (s_cat, in slots) and the depth (s_dep).  Nothing snaps.  A slot at
// fractional distance d from the focus gets its x, size, colour and opacity
// from d, so as the row glides under the fixed focus every icon swells or
// recedes continuously, the outgoing label fades while the incoming one
// fades up, and the underline grows under whichever slot is arriving.

// Colour mix, t in 0..1.  QUANTISED to sixteenths: every distinct colour is a
// separate cached run, so a continuous mix would upload a new run for every
// label and icon on every frame of a move.  Sixteen steps is below what a
// fade on a TV resolves, and repeated moves hit the cache.
static u32 mix_q(u32 a, u32 b, float t) {
    int k = (int)(t * 16.0f + 0.5f);
    if (k < 0) k = 0;
    if (k > 16) k = 16;
    u32 out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        int ca = (int)((a >> sh) & 0xFF), cb = (int)((b >> sh) & 0xFF);
        out |= (u32)(ca + (cb - ca) * k / 16) << sh;
    }
    return out;
}

void spine_draw_gpu(void) {
    if (!g_spine_on || !wave_gpu_blend_ready()) return;

    const spine_level L = spine_eval(spine_depth());
    const int cx = sx(SPINE_ACTIVE_X);        // the focus never moves
    const int cy = sy(L.icon_y);
    const u32 acc = XMB_ACCENT;
    const u8  r = (u8)((acc >> 16) & 0xFF), g = (u8)((acc >> 8) & 0xFF),
              b = (u8)(acc & 0xFF);

    // The glow band: dirD's ellipse (28% x 50% of a 1280x112 band centred on
    // the focus), transparent at 70% of its radius.  Peak alpha is the
    // level's: 0.12 at base, 0.09 inside a tab.
    wave_draw_glow_gpu(cx, cy,
                       UIS_W((int)(SPINE_GLOW_RX * SPINE_GLOW_FADE)),
                       UIS_H((int)(SPINE_GLOW_RY * SPINE_GLOW_FADE)),
                       r, g, b, (u8)(L.glow_a * 255.0f + 0.5f));

    // The "two-stage bloom" on the active icon.  NOT MEASURED: the README
    // names it and gives no numbers, and neither mockup draws one.  A wide
    // soft stage and a tight bright one.  It sits at the focus, so while a new
    // icon slides in it lights up as it arrives.
    const int ap = UIS_H((int)L.active_px);
    wave_draw_glow_gpu(cx, cy, ap, ap, r, g, b, 36);                  // ~0.14
    wave_draw_glow_gpu(cx, cy, ap * 5 / 8, ap * 5 / 8, r, g, b, 64);  // ~0.25
}

void spine_draw(void) {
    int order[XMB_TAB_COUNT], a;
    const int n = spine_row(order, &a);
    if (n <= 0 || a < 0) return;

    const spine_level L = spine_eval(spine_depth());
    const float f       = s_cat.value;
    const int   cy      = sy(L.icon_y);
    const int   idle_px = UIS_H((int)SPINE_IDLE_PX);
    const int   act_px  = UIS_H((int)L.active_px);
    const u32   idle    = spine_idle_colour();
    const int   W       = (int)display_width;

    for (int i = 0; i < n; i++) {
        const float d  = (float)i - f;
        const float al = spine_slot_alpha_f(d);
        if (al <= 0.0f) continue;
        const float near = spine_near(d);
        const int   px   = idle_px + (int)((float)(act_px - idle_px) * near + 0.5f);
        const int   cx   = sx(spine_slot_x_f(d));
        // The row is cropped at the left edge by design.  The blitter takes
        // unsigned coordinates and cannot draw a part-icon, so a slot not
        // wholly on screen is skipped; its falloff has it nearly transparent
        // by then anyway.
        if (cx - px / 2 < 0 || cx + px / 2 > W) continue;
        drawIconA((u32)(cx - px / 2), (u32)(cy - px / 2),
                  spine_icon(order[i]), (float)px,
                  mix_q(idle, XMB_ACCENT, near),
                  (u32)(al * 255.0f + 0.5f));
    }

    // Labels: every slot within one of the focus has one, at that slot's
    // nearness, so a move cross-fades the outgoing name into the incoming.
    const float lpx   = UIS_TF(L.label_px);
    const float track = lpx * SPINE_LABEL_TRACK_EM;
    const int   ly    = sy(spine_label_top(&L));
    const int   ry    = sy(spine_rule_top(&L));
    for (int i = 0; i < n; i++) {
        const float near = spine_near((float)i - f);
        if (near <= 0.04f) continue;
        const int t  = order[i];
        const int cx = sx(spine_slot_x_f((float)i - f));
        char label[sizeof(g_tabs[t].label)];
        snprintf(label, sizeof label, "%s", g_tabs[t].label);
        ui_upper_ascii(label);

        int lw = ttf_text_width_tracked(label, lpx, UI_FACE_TAB, track);
        int lx = cx - lw / 2;
        const int lo = XMB_ITEM_PAD, hi = W - XMB_ITEM_PAD - lw;
        if (lx < lo) lx = lo;
        if (hi >= lo && lx > hi) lx = hi;
        drawTTF_tracked((u32)lx, (u32)ly, label, lpx,
                        mix_q(XMB_BG, XMB_TEXT, near), UI_FACE_TAB, track);

        // 26x2 accent underline, centred on the slot (dirD), growing in with
        // the slot's arrival.
        const int rw = (int)((float)UIS_W((int)SPINE_RULE_W) * near + 0.5f);
        if (rw >= 2)
            drawRect((u32)(cx - rw / 2), (u32)ry,
                     (u32)rw, (u32)UIS_H((int)SPINE_RULE_H), XMB_ACCENT);
    }
}

// --- the base layer's column ---------------------------------------------------
//
// dirE's L1: the focused item's artwork on the intersection under the active
// icon, the next item below it, the focused item's title block beside it.
//
// It MOVES WITH ITS TAB.  The column hangs off its category, so when the row
// glides the column glides with it (XMP's columnX), and fades by the same
// nearness -- a new tab's items drift in from the side rather than cutting.
// Entering the tab lifts the column and fades it out over the first half of
// the depth move; the tab's own screen takes over at the halfway point.
//
// The images are the tab's OWN thumbnails.  The column asks the cache for
// exactly the size the tab's grid (or Home's row) caches them at, and the GPU
// scales them up with linear filtering.  That makes the two levels share one
// cache slot per item, so a poster seen in either level shows in the other
// with no fetch.  (The first cut asked for its own, larger size; on the
// console those never appeared while the same items loaded fine in the tab.
// Sharing the tab's size makes the column load exactly when the tab does.)
//
// DECIDED HERE, not by the design:
//   * The column top is y=318, not dirE's 300: under the README's 64 px icon
//     the L1 underline ends at y~303 (test_spine).
//   * Two items, not a column sinking into the wave: the wave's first draw is
//     an opaque full-screen gradient, so anything drawn before it is covered.
//   * Fades are a blended background-coloured rect over the artwork, since a
//     card quad is opaque; text fades by colour.
#define COL_TOP        318
#define COL_GAP         14
#define COL_NEXT_K      0.72f   // the next item's size relative to the focused
#define COL_NEXT_DIM    0.55f   // and how far it sinks toward the background
#define COL_TEXT_GAP    36      // artwork edge to the title block
#define COL_LIFT        36      // how far the column rises as a tab is entered

enum { SHAPE_POSTER = 0, SHAPE_WIDE = 1, SHAPE_SQUARE = 2 };

static bool spine_music_type(const char *ty) {
    return strcmp(ty, "MusicAlbum") == 0 || strcmp(ty, "Audio") == 0 ||
           strcmp(ty, "MusicArtist") == 0 || strcmp(ty, "MusicGenre") == 0 ||
           strcmp(ty, "Playlist") == 0;
}

typedef struct {
    const XMBItem *items;
    int            count;
    const char    *eyebrow;
    int            src_w, src_h;   // the size the tab caches thumbnails at
    int            shape;
} ColSrc;

static void column_source(int tab, ColSrc *c) {
    memset(c, 0, sizeof *c);
    c->eyebrow = g_tabs[tab].label;
    switch (xmb_kind(tab)) {
    case TABKIND_SEARCH:
    case TABKIND_SETTINGS:
        return;
    case TABKIND_HOME:
        c->count = xmb_home_preview(&c->items, &c->eyebrow,
                                    &c->src_w, &c->src_h, &c->shape);
        return;
    default: {
        if (!g_items_loaded[tab]) return;
        GridGeom gg;
        xmb_grid_geom(tab, &gg);
        c->items = g_items[tab];
        c->count = g_item_count[tab];
        c->src_w = gg.card_w;
        c->src_h = gg.card_h;
        const int k = xmb_kind(tab);
        c->shape = (k == TABKIND_MUSIC || k == TABKIND_PLAYLISTS) ? SHAPE_SQUARE
                 : gg.portrait ? SHAPE_POSTER : SHAPE_WIDE;
        return;
    }
    }
}

// Same rule the grid and Home use for which image a card shows.
static ThumbImg column_img(const ColSrc *c, const XMBItem *it) {
    return (c->shape == SHAPE_WIDE && !spine_music_type(it->type) && it->has_thumb)
         ? THUMB_IMG_THUMB : THUMB_IMG_PRIMARY;
}

typedef struct { int x, y, w, h; } ColBox;

typedef struct {
    ColSrc src;
    ColBox box[2];
    int    n;
    float  vis;     // 0..1: slide-in nearness x the level fade
    int    cx;      // the column's centre x, following its category
} ColView;

void spine_column_anchor(int tab, int *cx, float *near) {
    (void)tab;
    int order[XMB_TAB_COUNT], a;
    spine_row(order, &a);
    const float d = (a >= 0) ? (float)a - s_cat.value : 0.0f;
    *cx   = sx(spine_slot_x_f(d));
    *near = spine_near(d);
}

static void column_view(int tab, ColView *v) {
    column_source(tab, &v->src);

    int order[XMB_TAB_COUNT], a;
    spine_row(order, &a);
    const float d  = (a >= 0) ? (float)a - s_cat.value : 0.0f;
    const float lf = spine_clampf(1.0f - spine_depth() * 2.0f, 0.0f, 1.0f);
    v->vis = spine_near(d) * lf;
    v->cx  = sx(spine_slot_x_f(d));
    const int lift = UIS_H((int)((1.0f - lf) * (float)COL_LIFT));

    const spine_level L1 = SPINE_LEVEL[SPINE_L1];
    float w = L1.art_w, h = L1.art_h;                     // 128 x 192
    if (v->src.shape == SHAPE_WIDE)   { w = 224.0f; h = 126.0f; }
    if (v->src.shape == SHAPE_SQUARE) { w = 160.0f; h = 160.0f; }

    v->n = 0;
    if (v->src.count <= 0 || v->src.src_w <= 0 || v->src.src_h <= 0) return;
    int top = sy(COL_TOP) - lift;
    for (int i = 0; i < 2 && i < v->src.count; i++) {
        const float k = i ? COL_NEXT_K : 1.0f;
        ColBox *b = &v->box[i];
        b->w = UIS_W((int)(w * k));
        b->h = UIS_H((int)(h * k));
        b->x = v->cx - b->w / 2;
        b->y = top;
        if (b->y + b->h > (int)display_height - XMB_BOTTOM_PAD) break;
        if (b->x < 0 || b->x + b->w > (int)display_width) break;
        top += b->h + UIS_H(COL_GAP);
        v->n++;
    }
}

void spine_column_gpu(int tab) {
    ColView v;
    column_view(tab, &v);
    for (int i = 0; i < v.n; i++) {
        const XMBItem *it = &v.src.items[i];
        const ColBox  *b  = &v.box[i];
        xmb_card_gpu_src(it->id, v.src.src_w, v.src.src_h,
                         b->x, b->y, b->w, b->h, column_img(&v.src, it));
        // Fade toward the background: the focused item by the column's
        // visibility, the next one further, since it sits deeper.
        const float keep = v.vis * (i ? 1.0f - COL_NEXT_DIM : 1.0f);
        const u8    a    = (u8)((1.0f - keep) * 255.0f + 0.5f);
        if (a) ui_rect_gpu_draw(b->x, b->y, b->w, b->h, XMB_BG, a);
    }
    // The focus ring only once the column has arrived: a card quad's ring
    // cannot fade, and one flashing in mid-slide reads as a glitch.
    if (v.n && v.vis > 0.9f)
        ui_card_gpu_selection(v.box[0].x, v.box[0].y, v.box[0].w, v.box[0].h);
}

void spine_column_cpu(int tab) {
    ColView v;
    column_view(tab, &v);
    for (int i = 0; i < v.n; i++) {
        const XMBItem *it = &v.src.items[i];
        const ColBox  *b  = &v.box[i];
        xmb_draw_card_src(it->id, v.src.src_w, v.src.src_h,
                          b->x, b->y, b->w, b->h,
                          it->progress_pct, i == 0 && v.vis > 0.9f,
                          spine_music_type(it->type) ? it->name : NULL,
                          column_img(&v.src, it));
    }
}

// Copy `s` into `out`, cut with "..." to fit `max_w` at (px, face).  The cut
// walks back to a UTF-8 lead byte so it never splits a character.
static void fit_text(char *out, size_t cap, const char *s, float px, int face,
                     int max_w) {
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

void spine_column_text(int tab) {
    ColView v;
    column_view(tab, &v);
    if (v.vis <= 0.04f) return;

    const spine_level L1 = SPINE_LEVEL[SPINE_L1];
    // The title block sits beside the focused artwork, or where it would be
    // when there is none (Search, Settings, a library still loading).
    const int lift = v.n ? sy(COL_TOP) - v.box[0].y : 0;
    const int art_right = v.n ? v.box[0].x + v.box[0].w
                              : v.cx + UIS_W((int)L1.art_w) / 2;
    const int tx = art_right + UIS_W(COL_TEXT_GAP);
    const int max_w = (int)display_width - XMB_ITEM_PAD - tx;
    int ty = sy(COL_TOP) + UIS_H(22) - lift;
    if (max_w <= 0) return;

    char line[160];
    int adv = xmb_draw_eyebrow(tx, ty, v.src.eyebrow,
                               mix_q(XMB_BG, XMB_ACCENT_ALT, v.vis));
    if (v.src.count > 0) {
        snprintf(line, sizeof line, "1/%d", v.src.count);
        drawTTF_face((u32)(tx + adv + UIS_W(12)), (u32)ty, line, UIS_TF(11.0f),
                     mix_q(XMB_BG, XMB_TEXT_FAINT, v.vis), UI_FACE_TAB_REG);
    }
    ty += UIS_H(24);

    const float title_px = UIS_TF(L1.title_px);
    const char *title;
    switch (xmb_kind(tab)) {
    case TABKIND_SEARCH:   title = "Search";   break;
    case TABKIND_SETTINGS: title = "Settings"; break;
    default:
        title = v.src.count > 0 ? v.src.items[0].name
              : (xmb_kind(tab) == TABKIND_HOME || !g_items_loaded[tab])
                    ? "Loading..." : "Nothing here yet";
        break;
    }
    fit_text(line, sizeof line, title, title_px, UI_FACE_DISPLAY, max_w);
    drawTTF_face((u32)tx, (u32)ty, line, title_px,
                 mix_q(XMB_BG, XMB_TEXT, v.vis), UI_FACE_DISPLAY);
    ty += (int)(title_px * 1.35f);

    // The meta line draws in its own fixed colours, so it waits for the
    // column to arrive rather than popping in at full strength mid-slide.
    if (v.src.count > 0 && v.vis > 0.95f)
        xmb_draw_meta((u32)tx, (u32)ty, &v.src.items[0], UIS_TF(14.0f));
}

// --- motion shared with the screens under the spine ---------------------------
//
// The spine made the row and the levels drift; everything under it still
// snapped, and on the TV that is what read as static.  These three are the
// rest of README 4.1's list, on the same approach as the spine:
//
//   * the tab's content glides up into place as a tab is entered;
//   * the focus ring travels between cards instead of teleporting;
//   * the grid scrolls by easing rows up or down instead of jumping a row.
//
// All three are inert with the gate off (values snap), so jellyfin_spine.txt=0
// still reproduces the old frame exactly.

// How far below rest the content starts as a tab is entered.  Decided here:
// enough to read as the content rising out of the base layer, small enough
// that the bottom row never reaches past the hints bar.
#define CONTENT_RISE_PX 48

int spine_content_dy(void) {
    if (!g_spine_on) return 0;
    const float d = spine_depth();
    if (d <= 0.5f || d >= 1.0f) return 0;
    const float t = (d - 0.5f) * 2.0f;             // 0 at the cut, 1 at rest
    return UIS_H((int)((1.0f - t) * (float)CONTENT_RISE_PX + 0.5f));
}

// A screen identity for the ring and the grid: which tab, and which sub-screen
// of it.  A change means "new screen", and the motion snaps rather than
// sweeping from somewhere on the previous one.
static int motion_ctx(void) {
    return g_active_tab * 4096 + g_tv_depth * 256 + g_col_depth * 64 +
           g_music_depth * 16 + g_music_subtab;
}

static spine_approach s_ring[4];
static int            s_ring_ctx   = -1;
static unsigned       s_ring_frame = 0;    // frame it was last drawn

void spine_focus_ring_gpu(int x, int y, int w, int h) {
    if (!g_spine_on) { ui_card_gpu_selection(x, y, w, h); return; }

    const float tgt[4] = { (float)x, (float)y, (float)w, (float)h };
    const int   ctx    = motion_ctx();
    const bool  fresh  = ctx != s_ring_ctx || s_ring_frame + 1 < s_frame_id;
    for (int i = 0; i < 4; i++) {
        if (fresh) spine_approach_init(&s_ring[i], tgt[i], SPINE_CATEGORY_MS);
        spine_approach_set(&s_ring[i], tgt[i]);
        // Step once per frame even if a screen draws two rings.
        if (!fresh && s_ring_frame != s_frame_id)
            spine_approach_step(&s_ring[i], s_frame_dt);
    }
    s_ring_ctx   = ctx;
    s_ring_frame = s_frame_id;
    ui_card_gpu_selection((int)(s_ring[0].value + 0.5f), (int)(s_ring[1].value + 0.5f),
                          (int)(s_ring[2].value + 0.5f), (int)(s_ring[3].value + 0.5f));
}

static spine_approach s_grid;
static int            s_grid_ctx   = -1;
static unsigned       s_grid_frame = 0;

void spine_grid_motion(const GridGeom *gg, int scroll,
                       int *row0, int *rows, int *dy) {
    *row0 = 0; *rows = XMB_GRID_ROWS; *dy = 0;
    if (!g_spine_on || gg->cols <= 0) return;

    const float target = (float)(scroll / gg->cols);
    const int   ctx    = motion_ctx();
    if (s_grid_frame != s_frame_id) {
        // New screen, a missed frame, or a jump bigger than a screenful (the
        // loaded window sliding a whole page re-bases every index): snap.
        float gap = s_grid.value - target;
        if (gap < 0.0f) gap = -gap;
        if (ctx != s_grid_ctx || s_grid_frame + 1 < s_frame_id ||
            gap > (float)(XMB_GRID_ROWS + 1))
            spine_approach_init(&s_grid, target, SPINE_CATEGORY_MS);
        spine_approach_set(&s_grid, target);
        spine_approach_step(&s_grid, s_frame_dt);
        s_grid_ctx   = ctx;
        s_grid_frame = s_frame_id;
    }

    const float drawn = s_grid.value;
    if (drawn == target) return;
    int first = (int)drawn;                       // floor (drawn >= 0)
    if ((float)first > drawn) first--;
    *row0 = first - (int)target;
    *rows = XMB_GRID_ROWS + ((float)first != drawn ? 1 : 0);
    const float off = (target - drawn) * (float)gg->stride;   // round to nearest
    *dy   = (int)(off + (off >= 0.0f ? 0.5f : -0.5f));
}
