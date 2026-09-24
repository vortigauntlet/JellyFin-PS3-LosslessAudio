// Pad input — merged multi-pad polling, edge detection, nav auto-repeat.

#include <string.h>

#include "ui.h"
#include "timing.h"
#include "ui_sfx.h"

ButtonState btn_cur  = {0};
ButtonState btn_prev = {0};

void update_buttons(padData *pad) {
    btn_prev         = btn_cur;
    btn_cur.up       = pad->BTN_UP;
    btn_cur.down     = pad->BTN_DOWN;
    btn_cur.left     = pad->BTN_LEFT;
    btn_cur.right    = pad->BTN_RIGHT;
    btn_cur.cross    = pad->BTN_CROSS;
    btn_cur.circle   = pad->BTN_CIRCLE;
    btn_cur.square   = pad->BTN_SQUARE;
    btn_cur.triangle = pad->BTN_TRIANGLE;
    btn_cur.start    = pad->BTN_START;
    btn_cur.select   = pad->BTN_SELECT;
    btn_cur.l1       = pad->BTN_L1;
    btn_cur.r1       = pad->BTN_R1;
    btn_cur.l2       = pad->BTN_L2;
    btn_cur.r2       = pad->BTN_R2;
}

// The left stick as a d-pad.  One direction at a time -- the dominant axis --
// so a diagonal push does not move a grid both ways at once.  Hysteresis: a
// direction engages past ENGAGE and holds until the stick drops back inside
// RELEASE, so a stick resting near the threshold cannot chatter.  The result
// is ORed into BTN_UP..BTN_RIGHT, so the menus' auto-repeat, the player's
// seeking and everything else that reads the d-pad take it with no changes.
#define STICK_ENGAGE  80   // of 127
#define STICK_RELEASE 50
enum { STK_NONE, STK_UP, STK_DOWN, STK_LEFT, STK_RIGHT };

static int stick_dir(int prev, int dx, int dy) {
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    // How far the stick still leans the way it was already going.
    int keep = prev == STK_UP ? -dy : prev == STK_DOWN ? dy
             : prev == STK_LEFT ? -dx : prev == STK_RIGHT ? dx : 0;
    int major = ax > ay ? ax : ay;
    int cand = major <= STICK_ENGAGE ? STK_NONE
             : ax > ay ? (dx < 0 ? STK_LEFT : STK_RIGHT)
                       : (dy < 0 ? STK_UP : STK_DOWN);
    // Still leaning the old way and nothing new past ENGAGE: keep it.
    if (prev != STK_NONE && keep > STICK_RELEASE && (cand == STK_NONE || cand == prev))
        return prev;
    return cand;
}

bool poll_buttons(void) {
    static u8 s_stick[MAX_PADS];
    padInfo pi;
    ioPadGetInfo(&pi);
    padData merged; memset(&merged, 0, sizeof(merged));
    bool any = false;
    for (int i = 0; i < MAX_PADS; i++) {
        if (!pi.status[i]) continue;
        padData pd;
        ioPadGetData(i, &pd);
        if (!pd.len) continue;
        merged.BTN_UP       |= pd.BTN_UP;
        merged.BTN_DOWN     |= pd.BTN_DOWN;
        merged.BTN_LEFT     |= pd.BTN_LEFT;
        merged.BTN_RIGHT    |= pd.BTN_RIGHT;
        merged.BTN_CROSS    |= pd.BTN_CROSS;
        merged.BTN_CIRCLE   |= pd.BTN_CIRCLE;
        merged.BTN_SQUARE   |= pd.BTN_SQUARE;
        merged.BTN_TRIANGLE |= pd.BTN_TRIANGLE;
        merged.BTN_START    |= pd.BTN_START;
        merged.BTN_SELECT   |= pd.BTN_SELECT;
        merged.BTN_L1       |= pd.BTN_L1;
        merged.BTN_R1       |= pd.BTN_R1;
        merged.BTN_L2       |= pd.BTN_L2;
        merged.BTN_R2       |= pd.BTN_R2;
        // Sticks are words 4..7; a pad that sends fewer has none.
        if (pd.len >= 8) {
            s_stick[i] = (u8)stick_dir(s_stick[i], (int)pd.ANA_L_H - 128,
                                       (int)pd.ANA_L_V - 128);
            merged.BTN_UP    |= s_stick[i] == STK_UP;
            merged.BTN_DOWN  |= s_stick[i] == STK_DOWN;
            merged.BTN_LEFT  |= s_stick[i] == STK_LEFT;
            merged.BTN_RIGHT |= s_stick[i] == STK_RIGHT;
        }
        any = true;
    }
    // If no pad delivered a fresh packet this frame, keep the last known button
    // state (the controller simply hasn't sent new data yet) — exactly like a game
    // does, so a held trigger reads as continuously held instead of flickering.
    // Still advance btn_prev so the BTN_PRESSED edge doesn't re-fire every idle
    // frame (which would spam taps/pause for any held button).
    if (!any) { btn_prev = btn_cur; return false; }
    merged.len = 1;
    update_buttons(&merged);
    // XMB sounds on the button edges.  The cursor sound for the d-pad is in
    // btn_nav_repeat() instead, so it ticks with each step of a held scroll.
    // During video playback the effects port is suspended and these no-op.
    if (BTN_PRESSED(cross))                       ui_sfx_play(SFX_DECIDE);
    else if (BTN_PRESSED(circle))                 ui_sfx_play(SFX_CANCEL);
    else if (BTN_PRESSED(triangle))               ui_sfx_play(SFX_OPTION);
    else if (BTN_PRESSED(l1) || BTN_PRESSED(r1))  ui_sfx_play(SFX_CURSOR);
    return any;
}

void init_btns(void) {
    poll_buttons();
    btn_prev = btn_cur;
}

// Auto-repeat for held navigation buttons.  Returns true on the initial press,
// then every NAV_REPEAT_US after an initial NAV_DELAY_US for as long as the button
// is held — giving menus a steady, controllable scroll instead of one-per-tap.
bool btn_nav_repeat(bool held, int slot) {
    static u64  next_us[NAV_REPEAT_SLOTS] = { 0 };
    static bool active[NAV_REPEAT_SLOTS]  = { false };
    if (slot < 0 || slot >= NAV_REPEAT_SLOTS) return false;
    if (!held) { active[slot] = false; return false; }
    u64 now = timing_get_us();
    if (!active[slot]) {                       // first press
        active[slot]  = true;
        next_us[slot] = now + NAV_DELAY_US;
        if (slot <= NAV_right) ui_sfx_play(SFX_CURSOR);
        return true;
    }
    if (now >= next_us[slot]) {                // repeat tick
        next_us[slot] = now + NAV_REPEAT_US;
        if (slot <= NAV_right) ui_sfx_play(SFX_CURSOR);
        return true;
    }
    return false;
}
