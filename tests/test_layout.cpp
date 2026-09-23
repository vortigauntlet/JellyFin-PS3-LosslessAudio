// Host-side layout sanity for the XMB chrome and grid.
//
// Compiles the REAL ui_visuals.h macros against stubbed display metrics and
// evaluates the derived layout at every resolution this client runs at. It is
// not a pixel-perfect check -- it is a guard against the class of mistake that
// is invisible in source and obvious on a TV: a chrome band that grows into the
// content, a card height that goes negative, a hints bar that overlaps the grid.
//
// Added when Phase 2 rescaled the chrome band, which shifts the divider (and
// therefore everything below it) on any screen above 720p.  Extended when that
// rescale turned out to have been applied to the chrome and NOT to the content,
// leaving the UI at two scales at once -- see test_one_scale() below, which is
// the direct guard against that happening again.
//
// Build/run:  make -f Makefile.host test_layout && ./test_layout

#include <stdio.h>
#include <string.h>
#include <ppu-types.h>

// ---- stand-ins for the PS3 side ----
u32 display_width  = 1920;
u32 display_height = 1080;

static float s_overscan = 0.0f;
float overscan_frac(void) { return s_overscan; }
void  overscan_set_frac(float f) { s_overscan = f; }
int   overscan_x(void) { return (int)(s_overscan * (float)display_width  + 0.5f); }
int   overscan_y(void) { return (int)(s_overscan * (float)display_height + 0.5f); }

#include "../source/ui/theme.h"
Theme g_theme;                 // values are irrelevant here; layout only
QualityMode g_quality = QUALITY_FULL;

// Normally defined in ui/ui_scale.cpp, which pulls in the LV2 file paths.
// 0 = auto, which is the shipping default and what most of this file exercises.
int g_uis_pct = 0;

// Normally defined in ui/render/ui_spine.cpp.  false = the tab strip, which is
// what check_res() exercises; check_spine_res() turns it on.
bool g_spine_on = false;
// Also ui_spine.cpp's: the content glide offset, which is 0 at rest.  Layout
// is checked at rest, and SIZES must not depend on it anyway (they use
// XMB_CONTENT_Y_REST) -- see test_glide_sizes() below.
static int s_test_dy = 0;
int spine_content_dy(void) { return s_test_dy; }

// The macros under test come from the REAL header, so if ui_visuals.h changes
// this test changes with it.  That is the whole point: a hand-copied mirror
// would keep passing while the shipping layout drifted away from it.
#include "ui_visuals.h"

// Tab strip, from ui_widgets.cpp
#define TAB_ICON_Y   61
#define TAB_LABEL_Y  96
#define TAB_RULE_Y  110
#define TAB_RULE_H    2

static int g_fail = 0;

static void ck(const char *what, bool ok, const char *detail) {
    if (ok) return;
    printf("  FAIL %-46s %s\n", what, detail ? detail : "");
    g_fail++;
}

static void check_res(const char *name, u32 w, u32 h, float ovs) {
    display_width = w; display_height = h; overscan_set_frac(ovs);

    const int icon_y  = XMB_OY + UIS_H(TAB_ICON_Y);
    const int label_y = XMB_OY + UIS_H(TAB_LABEL_Y);
    const int rule_y  = XMB_OY + UIS_H(TAB_RULE_Y);
    const int rule_b  = rule_y + UIS_H(TAB_RULE_H);
    const int div_y   = XMB_DIVIDER_Y;
    const int card_h  = XMB_CARD_H_FIT;
    const int vis     = XMB_ITEMS_VIS;
    const int hint_y  = (int)display_height - XMB_OY - UIS_H(22);

    char buf[160];
    printf("%-22s %4ux%-4u ovs %.3f  icons %3d  label %3d  rule %3d  div %3d"
           "  card_h %3d  rows %d  hints %4d\n",
           name, w, h, (double)ovs, icon_y, label_y, rule_y, div_y,
           card_h, vis, hint_y);

    // The tab strip must fit entirely inside the chrome band.
    snprintf(buf, sizeof buf, "rule ends %d, divider %d", rule_b, div_y);
    ck("tab underline clears the divider", rule_b <= div_y, buf);
    ck("label sits below the icons", label_y > icon_y, NULL);
    ck("underline sits below the label", rule_y > label_y, NULL);

    // Content below the chrome must still have usable room.
    snprintf(buf, sizeof buf, "card_h %d", card_h);
    ck("cards have positive height", card_h > 40, buf);
    snprintf(buf, sizeof buf, "rows %d", vis);
    ck("list shows at least 3 rows", vis >= 3, buf);

    // The hints bar must not land inside the grid area.
    snprintf(buf, sizeof buf, "hints %d, grid bottom %d",
             hint_y, XMB_GRID_Y0 + XMB_GRID_AVAIL_H);
    ck("hints bar clears the grid", hint_y >= XMB_GRID_Y0 + XMB_GRID_AVAIL_H, buf);

    // And everything must stay on screen.
    ck("divider on screen", div_y > 0 && div_y < (int)display_height, NULL);
    ck("hints bar on screen", hint_y > 0 && hint_y < (int)display_height, NULL);

    // The jump bar's column has to leave room for itself to the left of the
    // grid.  The LETTER size inside it is fixed by measurement in
    // xmb_draw_jumpbar() -- it shrinks the font until "W" fits JBAR_W, so it
    // cannot overflow by construction -- but the column still has to exist.
    snprintf(buf, sizeof buf, "JBAR_W %d, gap %d", JBAR_W, JBAR_GAP);
    ck("jump bar column is at least 8px wide", JBAR_W >= 8, buf);
    ck("jump bar fits beside the content margin",
       JBAR_W + JBAR_GAP * 3 <= XMB_ITEM_PAD + JBAR_W, buf);
}

// ---------------------------------------------------------------------------
// The spine (jellyfin_spine.txt = 1, stage S1: every XMB screen at L2).
//
// test_spine.c proves the geometry on the 1280x720 authoring canvas.  This is
// the other half: the same numbers pushed through the REAL anchors at every
// resolution, overscan included, because the spine moves the divider from 144
// to 214 and so takes 70 authored px away from every screen below it.
// ---------------------------------------------------------------------------
static void check_spine_res(const char *name, u32 w, u32 h, float ovs) {
    display_width = w; display_height = h; overscan_set_frac(ovs);
    g_spine_on = true;

    const spine_level L = spine_eval((float)SPINE_L2);
    const int icon_top = XMB_OY + UIS_H((int)L.icon_y) - UIS_H((int)L.active_px) / 2;
    const int rule_b   = XMB_OY + UIS_H((int)spine_rule_top(&L)) + UIS_H((int)SPINE_RULE_H);
    const int div_y    = XMB_DIVIDER_Y;
    const int card_h   = XMB_CARD_H_FIT;
    const int vis      = XMB_ITEMS_VIS;
    const int hint_y   = (int)display_height - XMB_OY - UIS_H(22);
    // The slot left of the active one, the only left slot the design keeps.
    const int left_x   = XMB_OX + UIS_W((int)spine_slot_x(0, 1))
                       - UIS_H((int)SPINE_IDLE_PX) / 2;
    const int right_x  = XMB_OX + UIS_W((int)spine_slot_x(SPINE_FALLOFF_N - 1, 0))
                       + UIS_H((int)SPINE_IDLE_PX) / 2;

    char buf[160];
    printf("%-22s %4ux%-4u ovs %.3f  spine %3d..%3d  div %3d  card_h %3d"
           "  rows %d  left %4d  right %4d\n",
           name, w, h, (double)ovs, icon_top, rule_b, div_y, card_h, vis,
           left_x, right_x);

    snprintf(buf, sizeof buf, "underline ends %d, divider %d", rule_b, div_y);
    ck("spine: underline clears the divider", rule_b < div_y, buf);
    ck("spine: active icon below the top bar",
       icon_top >= XMB_OY + UIS_H(20) + UIS_H(24), NULL);

    snprintf(buf, sizeof buf, "card_h %d", card_h);
    ck("spine: cards have positive height", card_h > 40, buf);
    // The divider's move to 214 is paid for by every screen below it.  Two
    // list rows is the floor at which a list still navigates; below three is
    // recorded rather than failed, because it is the design's cost and not a
    // layout bug -- measured 2026-09-23 it happens only WITH overscan at 720p
    // and 480p, where the tab strip still managed three.
    snprintf(buf, sizeof buf, "rows %d", vis);
    ck("spine: list shows at least 2 rows", vis >= 2, buf);
    if (vis < 3)
        printf("   note: only %d list rows at %s (tab strip gives 3+)\n", vis, name);
    snprintf(buf, sizeof buf, "hints %d, grid bottom %d",
             hint_y, XMB_GRID_Y0 + XMB_GRID_AVAIL_H);
    ck("spine: hints bar clears the grid",
       hint_y >= XMB_GRID_Y0 + XMB_GRID_AVAIL_H, buf);

    // Icons are sized on the HEIGHT scale and placed on the WIDTH scale, so on
    // SD's non-square pixel grids the left neighbour's margin shrinks.  The
    // draw code SKIPS a slot that is not wholly on screen rather than
    // clipping it, so this is reported, not failed, where it does not fit:
    // the cost there is a missing neighbour icon, not a broken frame.
    if (left_x < 0)
        printf("   note: left neighbour skipped at %s (x %d)\n", name, left_x);
    // Without overscan the furthest slot the falloff draws must end on screen
    // -- that is the design's own framing.  Overscan shifts the row right
    // without narrowing it, and the row is DESIGNED to run off the right
    // edge, so there the last (0.14-alpha) slot is skipped by spine_draw()
    // and that is reported rather than failed.
    snprintf(buf, sizeof buf, "furthest drawn slot ends at %d", right_x);
    if (ovs == 0.0f)
        ck("spine: furthest drawn slot ends on screen", right_x <= (int)w, buf);
    else if (right_x > (int)w)
        printf("   note: furthest slot skipped at %s (ends %d)\n", name, right_x);

    g_spine_on = false;
}

// ---------------------------------------------------------------------------
// The content glide moves POSITIONS only.
//
// While a tab is entered, spine_content_dy() pushes the content down and eases
// it back.  Every SIZE derived from the content top must ignore that, or cards
// would resize on each frame of the glide and every thumbnail -- cached per
// size -- would be refetched mid-animation.
// ---------------------------------------------------------------------------
static void test_glide_sizes(void) {
    puts("-- content glide moves positions, not sizes --");
    display_width = 1920; display_height = 1080; overscan_set_frac(0.0f);
    g_spine_on = true;
    s_test_dy = 0;
    const int y0 = XMB_CONTENT_Y, g0 = XMB_GRID_Y0;
    const int h0 = XMB_CARD_H_FIT, v0 = XMB_ITEMS_VIS, a0 = XMB_GRID_AVAIL_H;
    s_test_dy = 72;                       // CONTENT_RISE_PX at 1080p
    char buf[160];
    snprintf(buf, sizeof buf, "content %d -> %d, grid %d -> %d",
             y0, XMB_CONTENT_Y, g0, XMB_GRID_Y0);
    ck("glide moves the content", XMB_CONTENT_Y == y0 + 72 && XMB_GRID_Y0 == g0 + 72, buf);
    snprintf(buf, sizeof buf, "card_h %d -> %d, rows %d -> %d, avail %d -> %d",
             h0, XMB_CARD_H_FIT, v0, XMB_ITEMS_VIS, a0, XMB_GRID_AVAIL_H);
    ck("glide leaves every size alone",
       XMB_CARD_H_FIT == h0 && XMB_ITEMS_VIS == v0 && XMB_GRID_AVAIL_H == a0, buf);
    s_test_dy = 0;
    g_spine_on = false;
}

// ---------------------------------------------------------------------------
// ONE scale.
//
// This is the regression guard for the bug that prompted the rewrite: the
// chrome was moved onto a scale that tracked the framebuffer while the content
// -- rows, cards, the jump bar, every literal font size -- stayed at the
// authored pixel count.  Everything still fitted, so the checks above passed;
// it just looked wrong, at two scales at once.
//
// Each entry is (authored value at 1280x720, the constant derived from it).
// A constant that was left raw shows up here immediately, because its value
// will not have moved with the screen.
// ---------------------------------------------------------------------------
struct Scaled { const char *name; int authored; int actual; char axis; };

static void test_one_scale(const char *label, u32 w, u32 h) {
    display_width = w; display_height = h; overscan_set_frac(0.0f);

    const Scaled tab[] = {
        // chrome
        { "XMB_TOPBAR_H",     64, XMB_TOPBAR_H,     'h' },
        { "XMB_TABBAR_H",     80, XMB_TABBAR_H,     'h' },
        { "XMB_BOTTOM_PAD",   70, XMB_BOTTOM_PAD,   'h' },
        // content -- the half that got left behind last time
        { "XMB_ITEM_H",       90, XMB_ITEM_H,       'h' },
        // XMB_THUMB_W is a WIDTH defined with the HEIGHT scale.  That predates
        // this test and is left as found: changing it would restretch every
        // list thumbnail on SD, where the pixels are not square, and nothing
        // has reported a problem with how they look.  Asserted as it is so the
        // oddity is recorded rather than silently normalised away.
        { "XMB_THUMB_W",      52, XMB_THUMB_W,      'h' },
        { "XMB_THUMB_H",      74, XMB_THUMB_H,      'h' },
        { "XMB_ROW_H",        88, XMB_ROW_H,        'h' },
        { "XMB_ROW_GAP",      16, XMB_ROW_GAP,      'h' },
        { "XMB_CARD_TEXT_H",  50, XMB_CARD_TEXT_H,  'h' },
        { "XMB_CARD_GAP_X",   24, XMB_CARD_GAP_X,   'w' },
        { "XMB_CARD_W_CAP",  300, XMB_CARD_W_CAP,   'w' },
        { "XMB_MUSIC_SUBTAB_H", 44, XMB_MUSIC_SUBTAB_H, 'h' },
        { "XMB_MUSIC_TEXT_H", 68, XMB_MUSIC_TEXT_H, 'h' },
        { "JBAR_W",           20, JBAR_W,           'w' },
        { "JBAR_GAP",          8, JBAR_GAP,         'w' },
        { "OSK_KEY_H",        44, OSK_KEY_H,        'h' },
        { "OSK_GAP",           8, OSK_GAP,          'w' },
        { "XMB_ROW_RADIUS",    8, XMB_ROW_RADIUS,   'w' },
    };

    printf("-- one scale at %ux%u --\n", w, h);
    for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
        const Scaled &s = tab[i];
        int want = s.axis == 'w'
                 ? s.authored * (int)display_width  / 1280
                 : s.authored * (int)display_height /  720;
        char buf[160];
        snprintf(buf, sizeof buf,
                 "%s (%s): authored %d -> %d, but the screen's scale gives %d",
                 label, s.name, s.authored, s.actual, want);
        ck(s.name, s.actual == want, buf);
    }

    // Type scales with geometry, not independently: one row's height and the
    // type that sits in it have to move together or the text drifts within it.
    float t = UIS_TF(12.0f);
    char buf[160];
    snprintf(buf, sizeof buf, "UIS_TF(12) = %.2f, XMB_ROW_H/88*12 = %.2f",
             (double)t, (double)((float)XMB_ROW_H / 88.0f * 12.0f));
    ck("type scales with the rows it sits in",
       t > (float)XMB_ROW_H / 88.0f * 12.0f - 1.0f &&
       t < (float)XMB_ROW_H / 88.0f * 12.0f + 1.0f, buf);
}

// ---------------------------------------------------------------------------
// The override.  jellyfin_uiscale.txt exists so that a scale that reads badly
// on a given TV can be corrected without a rebuild, so the two values that
// matter most are worth pinning: 100 must reproduce the authored numbers
// EXACTLY (that is the "put it back how it was" setting), and an explicit
// percentage must beat the automatic one.
// ---------------------------------------------------------------------------
static void test_override(void) {
    puts("-- the jellyfin_uiscale.txt override --");
    display_width = 1920; display_height = 1080; overscan_set_frac(0.0f);

    g_uis_pct = 100;
    char buf[160];
    snprintf(buf, sizeof buf, "topbar %d, rows %d, type %.1f",
             XMB_TOPBAR_H, XMB_ROW_H, (double)UIS_TF(12.0f));
    ck("uiscale=100 gives the authored pixel counts",
       XMB_TOPBAR_H == 64 && XMB_ROW_H == 88 && XMB_CARD_W_CAP == 300 &&
       JBAR_W == 20 && UIS_TF(12.0f) == 12.0f, buf);

    g_uis_pct = 150;
    snprintf(buf, sizeof buf, "topbar %d, rows %d", XMB_TOPBAR_H, XMB_ROW_H);
    ck("uiscale=150 matches auto at 1080p",
       XMB_TOPBAR_H == 96 && XMB_ROW_H == 132, buf);

    // An explicit percentage ignores the framebuffer, which is the point: the
    // same file has to mean the same thing on a 720p set and a 1080p one.
    display_height = 720; display_width = 1280;
    snprintf(buf, sizeof buf, "topbar %d at 720p", XMB_TOPBAR_H);
    ck("an explicit percentage does not track the screen",
       XMB_TOPBAR_H == 96, buf);

    g_uis_pct = 0;
}

int main(void) {
    printf("%-22s %-10s %-10s\n", "resolution", "", "derived layout");
    check_res("1080p",            1920, 1080, 0.00f);
    check_res("1080p + overscan", 1920, 1080, 0.05f);
    check_res("720p",             1280,  720, 0.00f);
    check_res("720p + overscan",  1280,  720, 0.08f);
    check_res("576i",              720,  576, 0.00f);
    check_res("480p",              720,  480, 0.00f);
    check_res("480p + overscan",   720,  480, 0.08f);

    putchar('\n');
    check_spine_res("1080p",            1920, 1080, 0.00f);
    check_spine_res("1080p + overscan", 1920, 1080, 0.05f);
    check_spine_res("720p",             1280,  720, 0.00f);
    check_spine_res("720p + overscan",  1280,  720, 0.08f);
    check_spine_res("576i",              720,  576, 0.00f);
    check_spine_res("480p",              720,  480, 0.00f);
    check_spine_res("480p + overscan",   720,  480, 0.08f);

    putchar('\n');
    test_glide_sizes();

    putchar('\n');
    test_one_scale("1080p", 1920, 1080);
    test_one_scale("720p",  1280,  720);
    test_one_scale("480p",   720,  480);
    putchar('\n');
    test_override();

    if (g_fail) { printf("\ntest_layout: %d FAILURES\n", g_fail); return 1; }
    printf("\ntest_layout: layout sane at every resolution, one scale throughout\n");
    return 0;
}
