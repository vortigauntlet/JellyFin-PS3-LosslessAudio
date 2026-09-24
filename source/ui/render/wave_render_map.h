// The calibration seam: stage B's unitless parameters -> the three arguments
// ui_wave.cpp already passes wf_step(), plus a height multiplier per ribbon
// and a colour multiplier, which are the two things the renderer can take
// without a geometry or GPU-state change.
//
// JellyWave 2.0 adds two more, both still CPU-side numbers handed to code
// that already takes them: a body SWELL from the low-mids (a multiplier on
// the layer's scale, i.e. its half-width and half-thickness), and stage B's
// travelling pulses as an ACCENT -- a smooth bump added to the solver's
// displacement curve before the loft reads it.  Neither touches a vertex
// format, a buffer, a draw call or GPU state.
//
// STILL NOT CARRIED, and why: detail[] and phase[] have no input on the spring
// chain (it makes its own fine detail from perturb and its own phase from
// integration); lift moves the whole band, which would shift its framing; hue
// would have to re-tint wave_gel.h's lit colour per vertex.  wave_layers.h
// consumes all of them and is not wired in.
//
//   wave_audio.h       PCM -> features
//   wave_motion.h      features -> slew-limited parameters
//   wave_render_map.h  parameters -> this renderer's numbers   (this file)
//
// WHY THIS IS A SEPARATE FILE AND NOT THREE LINES IN THE GLUE
//
// wave_motion.h's ranges are chosen for the MODEL: drive runs 0.35 at rest to
// 1.10 at full, because that is where the spring chain behaves.  ui_wave.cpp's
// ranges are chosen for the SCREEN: it divides wf_disp's output by
// WF_NOMINAL_PEAK (0.653, measured at drive 1.0) to land the ribbons back on
// their authored WAVE_AMP pixel heights, so the pixel amplitude is directly
// proportional to whatever drive it is handed, and drive 1.0 is "today's
// look".
//
// Those two ranges do not line up, and feeding one straight into the other
// gets both ends wrong:
//
//   AT REST.  Stage B idles at drive 0.35.  Through ui_wave.cpp's fixed
//   normalisation that is 35% of today's ribbon height -- 10 px, 8 px and 5 px
//   for the three layers.  That does not read as "calm", it reads as broken,
//   and the XMB with no music playing is the state this client spends most of
//   its life in.
//
//   AT FULL.  Stage B's ceiling is 1.10 because that is the largest drive that
//   keeps the chain clear of WK_KNEE (0.80) -- see WL_BASE_MAX.  Any mapping
//   that scales ABOVE 1.10 to get a bigger swing puts the solver inside its own
//   soft clip, where it stops responding to level at all.
//
// So the mapping is a compressed lerp between two endpoints that were each
// picked for a reason, and both reasons are testable.  It is header-only and
// PS3-header-free so test_wave_layers.c can assert them against the real
// kernel (.clinerules rule 7).

#ifndef WAVE_RENDER_MAP_H
#define WAVE_RENDER_MAP_H

#include "wave_motion.h"

// --- drive ---------------------------------------------------------------
// 0.62 at rest rather than 1.0: silence SHOULD be calmer than music, and 62%
// of the authored amplitude is visibly quieter while still reading as a wave
// (19 / 14 / 9 px against today's 30 / 22 / 15).  Going to 1.0 here would make
// the idle state identical to today and leave only a 10% swing for music,
// which is not worth building any of this for.
#define WRM_DRIVE_IDLE  0.62f

// 1.10 at full, straight from WM_MAX_DRIVE: this is the knee limit, not a look
// choice, and test_wave_layers.c re-measures that the chain stays under
// WK_KNEE when driven here through wf_step's own per-layer WF_DRIVE scaling.
#define WRM_DRIVE_MAX   WM_MAX_DRIVE

// Derived, not typed, so the endpoints stay exact if WM_MAX_DRIVE moves again.
#define WRM_DRIVE_GAIN  ((WRM_DRIVE_MAX - WRM_DRIVE_IDLE) / \
                         (WM_MAX_DRIVE  - WM_IDLE_DRIVE))

// --- timescale -----------------------------------------------------------
// This one is normalised so REST IS EXACTLY TODAY.  ui_wave.cpp's
// WAVE_FIELD_DT of 1.25 was calibrated against the drift the old sine had
// (WK_W1 * 1.25 = 0.0081 rad/frame against WAVE_DPHASE[0] of 0.008), and there
// is no reason for the idle XMB to drift at a different rate than it does
// now -- the audio system existing should not make anything worse when there
// is no audio.  Music then speeds it up to 1.44x at the fastest tempo the beat
// estimator will report.
#define WRM_TS_IDLE     1.00f
#define WRM_TS_GAIN     0.55f
#define WRM_TS_MIN      0.90f
#define WRM_TS_MAX      1.50f

// Stage B's perturbation passes through UNCHANGED, and that is deliberate:
// WM_IDLE_PERTURB is 0.02, which is exactly the literal ui_wave.cpp already
// passes, so the idle texture is bit-identical to today's and only the audio
// can raise it.  No mapping needed and none wanted.

// --- per-ribbon amplitude ------------------------------------------------
// drive moves all three chains together.  This is what makes them move
// DIFFERENTLY: stage B's amp[] is already split by band through
// WM_LAYER_MIX, and this carries it to the renderer as a multiplier on each
// ribbon's authored height.
//
// Keyed by SOLVER layer, not by screen depth, because the two renderers
// disagree about depth -- JellyWave draws solver layer 0 nearest, the legacy
// ribbons draw it furthest back.  What both agree on is that layer 0 is the
// widest, slowest chain and layer 2 the narrowest, fastest one (WF_DRIVE
// 1.00 / 0.85 / 0.70, WF_RATE 1.00 / 0.78 / 1.34), so:
//
//   solver 0  <- Swell                 bass       the big slow swing
//   solver 1  <- mean(Body, Filament)  low-mid / mid
//   solver 2  <- Sheen                 high / air  the quick, fine one
//
// Centred on WM_IDLE_AMP so REST IS EXACTLY 1.0 and the no-music look does not
// change: stage B idles every layer at 0.34, and dividing by that instead
// (the obvious mapping) would have multiplied a full band by 2.9.
//
// WRM_AMP_MAX IS MEASURED.  The real solver at WRM_DRIVE_MAX, WM_MAX_PERTURB
// and WRM_TS_MAX, lofted through wave_gel.h with disp_gain scaled up, first
// breaks test_wave_gel.c's framing box at 1.75x -- the near layer's top edge
// reaches the horizontal midline.  At 1.30x that edge peaks at -0.099 in clip
// space, a tenth of the screen's half-height below the line; on the legacy
// ribbons it is 30 px * 1.10 * 1.30 = 43 px against a crest at 78% of the
// height.  test_wave_layers.c re-runs that measurement against this constant.
#define WRM_AMP_MAX     1.30f
#define WRM_AMP_GAIN    ((WRM_AMP_MAX - 1.0f) / (1.0f - WM_IDLE_AMP))
// Derived: where amp = 0 lands, 0.845.  A band that is silent while the
// others play makes its ribbon calmer than rest, which is the point.
#define WRM_AMP_MIN     (1.0f - WRM_AMP_GAIN * WM_IDLE_AMP)

// --- luminance ------------------------------------------------------------
// One multiplier on the ribbon's colour: louder is brighter, and an onset adds
// a short sheen on top.  Also centred so rest is exactly 1.0.
//
// KEPT SMALL ON PURPOSE.  wave_motion.h's own rule is that a beat which
// brightens the screen is a strobe, and this client has had a real strobe on
// this exact ribbon.  So glow is a garnish, not a flash:
//
//   bright  0.35 (quiet music) .. 1.00 (loud)  ->  0.94 .. 1.135
//   glow    0 .. 1                             ->  +0 .. +0.08
//
// and the step between frames is bounded by stage B's slews rather than by
// anything here: at 60 fps the worst case is WM_SLEW_BRIGHT * 0.30 +
// WM_SLEW_GLOW_UP * 0.08 = 0.017 a frame, so the full rise takes a fifth of a
// second.  test_wave_layers.c asserts that bound.
#define WRM_LUM_BRIGHT  0.30f
#define WRM_LUM_GLOW    0.08f
#define WRM_LUM_MIN     0.85f
#define WRM_LUM_MAX     1.20f

// --- body swell (JellyWave 2.0) ---------------------------------------
// Low-mids thicken the body: stage B's Body layer (0.46 bass, 0.40 low-mid,
// 0.14 mid) scales every layer's half-width and half-thickness.  Centred on
// WM_IDLE_AMP like the heights, so rest is exactly 1.0.
//
// 1.04 IS MEASURED, together with WRM_ACC_H below: with everything at its
// loudest at once -- WRM_DRIVE_MAX, WRM_AMP_MAX, this, and the accent sitting
// on the crest along the whole band -- the near layer's top edge measured
// -0.053 in clip space, just inside the same 0.05 margin under the midline
// WRM_AMP_MAX keeps.  test_wave_layers.c re-runs that worst case.
#define WRM_THICK_MAX   1.04f
#define WRM_THICK_GAIN  ((WRM_THICK_MAX - 1.0f) / (1.0f - WM_IDLE_AMP))
#define WRM_THICK_MIN   (1.0f - WRM_THICK_GAIN * WM_IDLE_AMP)

// --- the accent (JellyWave 2.0) ----------------------------------------
// A stage B pulse is an object: it spawns on an onset, travels the band over
// two beats and fades on a C1 envelope.  Here it becomes a smooth compact
// bump, (1 - q^2)^2, added to the solver's displacement -- a swell that runs
// along the ribbon, which is the "brief controlled accent" rather than a
// flash of the whole screen.
//
//   WRM_ACC_H      bump height in the solver's own units at pulse amp 1.
//                  0.10 is about 15% of the chain's nominal peak (0.653): an
//                  accent, not a second wave.  Measured -- see WRM_THICK_MAX.
//   WRM_ACC_WIDEN  the ribbon's bump is twice stage B's pulse width, 0.15 to
//                  0.30 in u.  JellyWave re-samples its geometry every third
//                  frame, and a pulse at 120 BPM moves 0.057 u between
//                  samples; against a bump this broad that reads as travel,
//                  against stage B's raw width it read as stepping.
//   WRM_ACC_LAYER  full on the near/widest layer, less on the others, so the
//                  accent reads as one gesture through the band rather than
//                  three.
//
// Overlapping pulses are summed and then clamped to 1, so the accent can never
// exceed WRM_ACC_H however many are live.
#define WRM_ACC_H       0.10f
#define WRM_ACC_WIDEN   2.0f

static const float WRM_ACC_LAYER[3] = { 1.00f, 0.80f, 0.60f };

typedef struct {
    float x[WM_PULSES];     // pulse centre, u along the solver's [0,1]
    float inv[WM_PULSES];   // 1 / bump half-width in u
    float a[WM_PULSES];     // amplitude 0..1, 0 when not live
} wrm_accent_set;

typedef struct {
    float dt_scale;     // multiplies WAVE_FIELD_DT
    float perturb;      // straight to wf_step
    float drive;        // straight to wf_step
    float amp[3];       // per solver layer, multiplies the ribbon's height
    float lum;          // multiplies the ribbon's colour
    float thick;        // multiplies the layer's scale (width and thickness)
    wrm_accent_set acc; // travelling accents, see wrm_accent()
} wrm_out;

static inline float wrm_clamp(float v, float lo, float hi)
{
    if (v != v) return lo;              // NaN in, defined out
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// The whole mapping.  p may be NULL, which yields the rest values -- so a
// caller whose analyser is disabled or failed to start gets the idle look from
// this one call and needs no second code path.
// RESPONSE GAIN.  wrm_map_gain() multiplies every response's DEVIATION FROM
// REST by `gain` before the same clamps apply -- so a higher gain reaches the
// caps at lower music levels (a steeper, more visible response) but can never
// pass them.  Every ceiling above was measured against the framing box, and
// none of them moves; rest is still exactly rest at any gain, because the
// deviation there is zero.  gain 1 is wrm_map exactly.
//
// Added after the first hardware look: "barely noticed".  The mapping was
// tuned to a brief that asked for "restrained"; which of the two is right is a
// question for a TV, so it is a runtime knob (ui_wave_audio.cpp's gate file)
// instead of a rebuild.
#define WRM_GAIN_MIN    0.25f
#define WRM_GAIN_MAX    4.00f

static inline void wrm_map_gain(const wm_params *p, float gain, wrm_out *out);

static inline void wrm_map(const wm_params *p, wrm_out *out)
{
    wrm_map_gain(p, 1.0f, out);
}

static inline void wrm_map_gain(const wm_params *p, float g, wrm_out *out)
{
    if (!out) return;
    g = (g == g) ? wrm_clamp(g, WRM_GAIN_MIN, WRM_GAIN_MAX) : 1.0f;
    if (!p) {
        out->dt_scale = WRM_TS_IDLE;
        out->perturb  = WM_IDLE_PERTURB;
        out->drive    = WRM_DRIVE_IDLE;
        out->amp[0]   = out->amp[1] = out->amp[2] = 1.0f;
        out->lum      = 1.0f;
        out->thick    = 1.0f;
        memset(&out->acc, 0, sizeof out->acc);
        return;
    }
    out->dt_scale = wrm_clamp(
        WRM_TS_IDLE + (p->timescale - WM_IDLE_TIME) * WRM_TS_GAIN * g,
        WRM_TS_MIN, WRM_TS_MAX);
    out->perturb  = wrm_clamp(
        WM_IDLE_PERTURB + (p->perturb - WM_IDLE_PERTURB) * g,
        0.0f, WM_MAX_PERTURB);
    out->drive    = wrm_clamp(
        WRM_DRIVE_IDLE + (p->drive - WM_IDLE_DRIVE) * WRM_DRIVE_GAIN * g,
        WRM_DRIVE_IDLE, WRM_DRIVE_MAX);

    // The clamp comes AFTER the sum so one NaN anywhere lands on the floor
    // rather than escaping through an arithmetic that happens to absorb it.
    {
        const float a[3] = {
            p->amp[0],
            0.5f * (p->amp[1] + p->amp[2]),
            p->amp[3]
        };
        int i;
        for (i = 0; i < 3; i++)
            out->amp[i] = wrm_clamp(1.0f + (a[i] - WM_IDLE_AMP) * WRM_AMP_GAIN * g,
                                    WRM_AMP_MIN, WRM_AMP_MAX);
    }
    out->lum = wrm_clamp(1.0f + ((p->bright - WM_IDLE_BRIGHT) * WRM_LUM_BRIGHT
                              + p->glow * WRM_LUM_GLOW) * g,
                         WRM_LUM_MIN, WRM_LUM_MAX);
    out->thick = wrm_clamp(1.0f + (p->amp[1] - WM_IDLE_AMP) * WRM_THICK_GAIN * g,
                           WRM_THICK_MIN, WRM_THICK_MAX);
    {
        int j;
        for (j = 0; j < WM_PULSES; j++) {
            const wm_pulse *q = &p->pulse[j];
            float w = WRM_ACC_WIDEN * wrm_clamp(q->width, 0.02f, 0.5f);
            out->acc.x[j]   = wrm_clamp(q->x, -0.5f, 1.5f);
            out->acc.inv[j] = 1.0f / w;
            out->acc.a[j]   = q->live ? wrm_clamp(q->amp * g, 0.0f, 1.0f) : 0.0f;
        }
    }
}

// The accent at one point u of the solver's [0,1], in the solver's units, for
// one layer (0 near .. 2 far).  EXACTLY 0.0f when no pulse is live, so adding
// it leaves a displacement bit-identical -- the idle look depends on that.
static inline float wrm_accent_at(const wrm_accent_set *a, int layer, float u)
{
    float acc = 0.0f;
    int   j;
    if (!a) return 0.0f;
    if (layer < 0) layer = 0;
    if (layer > 2) layer = 2;
    for (j = 0; j < WM_PULSES; j++) {
        float q = (u - a->x[j]) * a->inv[j];
        float t = 1.0f - q * q;
        t    = (t > 0.0f) ? t : 0.0f;
        acc += a->a[j] * t * t;
    }
    acc = (acc < 1.0f) ? acc : 1.0f;
    return WRM_ACC_H * WRM_ACC_LAYER[layer] * acc;
}

// out[k] = in[k] + the accent at sample k, with the samples spanning u in
// [0,1] as wave_field.h's do.  in and out may not alias; n < 2 copies nothing.
static inline void wrm_accent(const wrm_accent_set *a, int layer,
                              const float *in, float *out, int n)
{
    float du;
    int   k;
    if (!in || !out || n < 2) return;
    du = 1.0f / (float)(n - 1);
    for (k = 0; k < n; k++)
        out[k] = in[k] + wrm_accent_at(a, layer, (float)k * du);
}

// --- distinct bands (2026-09-24) -------------------------------------------
//
// Hardware verdict on the mapping above: "it all moves at a similar intensity;
// you can't tell the lows, mids and highs apart".  The log said why: during
// music `drive` went 0.62 -> 1.02 (every layer ~65% taller, together), `ts`
// sat at 1.2-1.3 (the whole wave faster, together), while the per-band
// heights only separated by +-15% (amp 0.88..1.16) -- shared terms swamping
// the per-band ones, and the per-band ones built from MIXED bands.
//
// This is applied on top of wrm_map_gain()'s result by the glue:
//
//   * The shared terms are held near rest: tempo no longer speeds the wave
//     (dt_scale 1.0), loudness keeps only a fifth of its drive swing (capped
//     at WRM_DB_DRIVE_MAX), and the broadband ripple and brightness keep a
//     fraction.  The base motion stays slow and calm under any music.
//   * Each solver layer then follows ONE part of the spectrum, taken straight
//     from stage A's bands, with its own timing -- so the three read as three
//     different instruments:
//
//       layer 0 (near, widest)   sub + bass       slow heavy swell   att 60 ms / rel 420 ms
//       layer 1 (middle)         low-mid + mid    body               att 45 ms / rel 260 ms
//       layer 2 (far, finest)    high + air       quick flicker      att 15 ms / rel 120 ms
//
//     height from WRM_DB_AMP_QUIET (a band that is quiet sinks BELOW rest,
//     which is what makes the loud one stand out) to WRM_DB_AMP_MAX, and a
//     per-layer brightness, strongest on the highs.
//   * At rest (no audio, silence latched) every output is exactly the rest
//     value, as before.
//
// FRAMING.  Height is linear in drive and in disp_gain, so the budget
// test_look_framing measured (drive 1.10 x amp 1.30) is spent differently,
// not raised: with drive held at WRM_DB_DRIVE_MAX the per-layer caps can go
// much higher.  test_distinct_framing re-runs the same measurement -- real
// solver, real loft, the fullest swell, the accent on the crest -- with THESE
// caps, and must stay inside the same box with the same 0.05 margin.
// Per layer: the band level (self-referenced) that reads as 0, and a response
// multiplier.  2026-09-24 hardware: "the bass could be a bit more sensitive"
// -- the lows start lower and climb faster; the caps (and so the framing) do
// not move.
static const float WRM_DB_FLOOR[3] = { 0.18f, 0.28f, 0.28f };
static const float WRM_DB_RESP[3]  = { 1.40f, 1.00f, 1.00f };
#define WRM_DB_AMP_QUIET    0.78f
#define WRM_DB_DRIVE_MAX    0.70f
#define WRM_DB_DRIVE_KEEP   0.20f
#define WRM_DB_PERTURB_KEEP 0.30f
#define WRM_DB_LUM_KEEP     0.30f
#define WRM_DB_ACC_KEEP     0.60f

static const float WRM_DB_ATT[3]     = { 0.060f, 0.045f, 0.015f };
static const float WRM_DB_REL[3]     = { 0.420f, 0.260f, 0.120f };
static const float WRM_DB_AMP_MAX[3] = { 1.75f, 1.65f, 2.10f };   // measured: test_distinct_framing
static const float WRM_DB_LUM_MAX[3] = { 1.10f, 1.18f, 1.35f };

typedef struct {
    float env[3];
    float lvl[3];      // this frame's shaped level per layer, 0..1 (for the snow)
    // the sub-bass shock (2026-09-25: "the subbass needs to be more
    // distinctive -- when it bumps, send extra shocks through the whole wave")
    float sub_slow;    // the sub band's recent level
    float shock;       // 0..1, decays
    float shock_x;     // where the travelling shock is along the band, u
    float refr;        // s until the next hit may fire
    float kick;        // non-zero on the frame a hit lands (read by the snow)
} wrm_db_state;

// A hit is a fast rise of the sub band above its own recent level.  On a hit
// every layer is pushed toward its OWN height cap (never past it) and lifted
// in brightness, and a broad bump runs along the band -- a shock through the
// whole wave, not just the bass layer.  The accent total is then capped at the
// measured bound (see the end of wrm_distinct), so the framing still holds.
#define WRM_SUB_TAU        0.35f
#define WRM_SUB_THRESH     0.14f
#define WRM_SUB_REFR       0.22f
#define WRM_SHOCK_TAU      0.28f
#define WRM_SHOCK_CROSS    0.55f      // s to run the length of the band
#define WRM_SHOCK_PUSH     0.60f      // fraction of the way to each layer's cap
#define WRM_SHOCK_LUM      0.14f
#define WRM_SHOCK_ACC      0.60f
#define WRM_SHOCK_WIDTH    0.16f
#define WRM_ACC_TOTAL_MAX  WRM_DB_ACC_KEEP   // test_distinct_framing measured this

// src[3]: 0..1 band levels for layers 0..2.  present: 0 at rest .. 1 with
// audio.  resp: the intensity level's response (1 = default).  Rewrites the
// shared terms of *o and its amp[], and fills lum3[] (per-layer brightness,
// exactly 1.0 at rest).
static inline void wrm_distinct(wrm_db_state *st, const float src[3],
                                float sub_fast, float present, float resp,
                                float dt, wrm_out *o, float lum3[3])
{
    int i;
    if (!st || !o || !lum3) return;
    present = wrm_clamp(present, 0.0f, 1.0f);
    resp    = wrm_clamp(resp, 0.25f, 2.0f);
    if (!(dt > 0.0f)) dt = 0.0f;

    o->dt_scale = WRM_TS_IDLE;
    o->drive    = wrm_clamp(WRM_DRIVE_IDLE + (o->drive - WRM_DRIVE_IDLE) * WRM_DB_DRIVE_KEEP,
                            WRM_DRIVE_IDLE, WRM_DB_DRIVE_MAX);
    o->perturb  = WM_IDLE_PERTURB + (o->perturb - WM_IDLE_PERTURB) * WRM_DB_PERTURB_KEEP;
    o->lum      = 1.0f + (o->lum - 1.0f) * WRM_DB_LUM_KEEP;
    for (i = 0; i < WM_PULSES; i++) o->acc.a[i] *= WRM_DB_ACC_KEEP;

    for (i = 0; i < 3; i++) {
        const float x   = wrm_clamp(src ? src[i] : 0.0f, 0.0f, 1.0f);
        const float tau = (x > st->env[i]) ? WRM_DB_ATT[i] : WRM_DB_REL[i];
        const float k   = dt / (tau + dt);
        float s, amp;
        st->env[i] += (x - st->env[i]) * k;
        s   = wrm_clamp((st->env[i] - WRM_DB_FLOOR[i]) / (1.0f - WRM_DB_FLOOR[i])
                        * resp * WRM_DB_RESP[i], 0.0f, 1.0f);
        amp = WRM_DB_AMP_QUIET + (WRM_DB_AMP_MAX[i] - WRM_DB_AMP_QUIET) * s;
        st->lvl[i] = s;
        if (present <= 0.0f) {
            o->amp[i] = 1.0f;                   // rest is exactly rest
            lum3[i]   = 1.0f;
        } else {
            o->amp[i] = 1.0f + present * (amp - 1.0f);
            lum3[i]   = 1.0f + present * (WRM_DB_LUM_MAX[i] - 1.0f) * s;
        }
    }

    // --- the sub-bass shock ---
    {
        const float sf = wrm_clamp(sub_fast, 0.0f, 1.0f);
        const float ks = dt / (WRM_SUB_TAU + dt);
        const float excess = sf - st->sub_slow;
        st->sub_slow += (sf - st->sub_slow) * ks;
        st->kick  = 0.0f;
        st->refr -= dt;
        st->shock *= 1.0f / (1.0f + dt / WRM_SHOCK_TAU);
        st->shock_x += dt / WRM_SHOCK_CROSS;
        if (present > 0.0f && st->refr <= 0.0f && excess > WRM_SUB_THRESH) {
            const float m = wrm_clamp(0.35f + (excess - WRM_SUB_THRESH) * 3.0f, 0.0f, 1.0f);
            if (m > st->shock) st->shock = m;
            st->shock_x = -0.15f;
            st->refr    = WRM_SUB_REFR;
            st->kick    = m * present;
        }
        if (present <= 0.0f) st->shock = 0.0f;
        if (st->shock > 0.001f) {
            const float sh = st->shock * present;
            for (i = 0; i < 3; i++) {
                o->amp[i] += (WRM_DB_AMP_MAX[i] - o->amp[i]) * WRM_SHOCK_PUSH * sh;
                if (o->amp[i] > WRM_DB_AMP_MAX[i]) o->amp[i] = WRM_DB_AMP_MAX[i];
                lum3[i] += WRM_SHOCK_LUM * sh;
                if (lum3[i] > WRM_DB_LUM_MAX[2] + WRM_SHOCK_LUM) lum3[i] = WRM_DB_LUM_MAX[2] + WRM_SHOCK_LUM;
            }
            if (st->shock_x < 1.3f) {
                int j, free_j = -1;
                for (j = 0; j < WM_PULSES; j++) if (o->acc.a[j] <= 0.0f) { free_j = j; break; }
                if (free_j < 0) free_j = WM_PULSES - 1;
                o->acc.x[free_j]   = st->shock_x;
                o->acc.inv[free_j] = 1.0f / WRM_SHOCK_WIDTH;
                o->acc.a[free_j]   = WRM_SHOCK_ACC * sh;
            }
        }
    }

    // The accents may add up to the measured bound and no further.
    {
        float sum = 0.0f;
        for (i = 0; i < WM_PULSES; i++) sum += o->acc.a[i];
        if (sum > WRM_ACC_TOTAL_MAX) {
            const float k = WRM_ACC_TOTAL_MAX / sum;
            for (i = 0; i < WM_PULSES; i++) o->acc.a[i] *= k;
        }
    }
}

#endif // WAVE_RENDER_MAP_H
