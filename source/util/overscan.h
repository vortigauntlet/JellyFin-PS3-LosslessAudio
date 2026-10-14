#pragma once
#include <ppu-types.h>

// User-calibrated CRT overscan inset.
//
// A 576i / 480i CRT (and many via component/composite) hides a border of the
// framebuffer behind the bezel, so UI drawn to the panel edges is clipped off
// screen (issue #21, Sony Wega).  There is no reliable way to detect how much
// a given set overscans, so the user calibrates it once in
// Settings > Screen Size and the value is persisted.  Both the browsing UI and
// the video-player HUD push their content inward by this inset.
//
// The value is a fraction of EACH axis (0 .. OVERSCAN_MAX_FRAC); overscan_x()/
// overscan_y() convert it to a pixel inset per edge at the current resolution.

#define OVERSCAN_MAX_FRAC   0.08f    // clamp: up to 8% inset per edge
#define OVERSCAN_STEP_FRAC  0.005f   // d-pad calibration step (0.5%)
// Before the user has calibrated anything: about the safe area the XMB and games
// keep, so a TV that crops its edges (most do outside a "PC" / "Just Scan" mode)
// still shows the logo, the clock and the hints.  A saved 0 ("Off") stays 0.
#define OVERSCAN_DEFAULT_FRAC 0.035f

void  overscan_load(void);           // read the persisted value (once, at startup)
void  overscan_save(void);           // persist the current value
float overscan_frac(void);           // current inset fraction
void  overscan_set_frac(float f);    // clamp to [0, OVERSCAN_MAX_FRAC] and set
int   overscan_x(void);              // px inset per left/right edge  (frac * width)
int   overscan_y(void);              // px inset per top/bottom edge  (frac * height)
