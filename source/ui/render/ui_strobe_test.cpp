#include <stdio.h>

#include "ui_strobe_test.h"
#include "plog.h"
#include "timing.h"

// Run 7.  Profiles advance on WALL-CLOCK time so a person watching the TV can
// tell which one is on screen with a stopwatch.
//
// Model under test: the ~16 KB of vertex data right AFTER the gradient quad
// draws wrong; nothing else does.
enum {
    STROBE_PROFILE_LAST = 2,
    STROBE_PROFILE_US   = 12 * 1000000
};

static bool s_active;
static int  s_profile = 2;
static int  s_frames;
static u64  s_t0;

static const char *const s_names[] = {
    "grad4096-geom4100",    // 0   0-12 s  the failed fix (control, bad)
    "grad0-geom1024",       // 1  12-24 s  run 4's good layout, no labels
    "geom0-grad-at-end",    // 2  24-36 s  gradient moved after the geometry
    "body-off",             // 3  36-48 s  control
    "grad0-geom1024-2"      // 4  48 s on  candidate fix again
};

int strobe_test_profile(void)
{
    return s_profile;
}

const char *strobe_test_profile_name(void)
{
    return s_names[s_profile];
}

bool strobe_test_disable_inactive_labels(void)   { return false; }
bool strobe_test_disable_category_labels(void)   { return false; }
bool strobe_test_disable_tracked_text(void)      { return false; }
bool strobe_test_disable_cpu_divider(void)       { return false; }
bool strobe_test_disable_card_cpu_fallback(void) { return false; }
bool strobe_test_disable_gpu_glow(void)          { return false; }
bool strobe_test_disable_jellywave(void)         { return false; }
bool strobe_test_disable_jellywave_rim(void)     { return false; }
bool strobe_test_disable_jellywave_body(void)    { return s_profile == 3; }

// Where n starts on a rebuild: the gradient's index, or (grad_end) the
// geometry's.  upfrom is the first index uploaded; it equals base here.
int strobe_test_jw_base(void)
{
    return (s_profile == 0 || s_profile == 3) ? 4096 : 0;
}

int strobe_test_jw_upfrom(void) { return strobe_test_jw_base(); }

int strobe_test_jw_geom0(void)
{
    return (s_profile == 1 || s_profile == 4) ? 1024 : 0;
}

bool strobe_test_jw_grad_end(void) { return s_profile == 2; }
int  strobe_test_jw_trail(void)    { return 0; }
bool strobe_test_jw_reverse(void)  { return false; }

static void strobe_test_log(const char *event)
{
    char line[224];
    snprintf(line, sizeof(line),
             "strobe_test: %s profile=%d name=%s frames=%d t=%llus "
             "base=%d geom0=%d grad_end=%d body=%s",
             event, s_profile, strobe_test_profile_name(), s_frames,
             (unsigned long long)((timing_get_us() - s_t0) / 1000000ULL),
             strobe_test_jw_base(), strobe_test_jw_geom0(),
             (int)strobe_test_jw_grad_end(),
             strobe_test_disable_jellywave_body() ? "off" : "on");
    plog(line);
}

void strobe_test_enter_xmb(void)
{
    s_active = true;
    s_profile = 2;
    s_frames = 0;
    s_t0 = timing_get_us();
    strobe_test_log("start");
}

void strobe_test_leave_xmb(void)
{
    if (!s_active) return;
    strobe_test_log("stop");
    s_active = false;
}

void strobe_test_tick(void)
{
    if (!s_active || s_profile == STROBE_PROFILE_LAST) return;
    ++s_frames;
    if (timing_get_us() - s_t0 < (u64)(s_profile + 1) * STROBE_PROFILE_US)
        return;

    strobe_test_log("complete");
    s_profile++;
    s_frames = 0;
    strobe_test_log("start");
}
