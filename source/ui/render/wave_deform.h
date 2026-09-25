// JellyWave 2.0: deformation -- what each signal does to the curve's SHAPE.
//
// wave_render_map.h's wrm_distinct() already gives every layer its height
// (bass on the near layer, mids the middle, highs the far one), the beat
// punch and the sub-bass ripples.  Height alone is one number per layer; this
// file is the part that changes the shape along the band, each signal with
// its own job:
//
//   mids      curvature: a travelling ripple along each layer, plus a smaller
//             secondary ripple running the other way (the "second wave") --
//             stronger and quicker with the mids and the beat punch
//   waveform  fine deformation: the scope trace (wave_scope.h) laid along the
//             band at a small amplitude, strongest on the finest (far) layer
//   beat      a brief macro pulse: one broad swell of the whole band on an
//             onset, eased in and out, never a jolt
//   stereo    asymmetric motion: the band leans toward the louder side, and
//             the ripple is stronger on that side than the other
//   treble    the rim: a brightness gain for the rim pass only
//             (applied in ui_wave.cpp's emit, not here)
//
// WHERE IT RUNS.  wdf_map() on the UI thread, once per frame, into a small
// wdf_look value; the renderer copies that into the generation job beside the
// rest of the look, and wdf_apply() adds the deformation to the layer's copy
// of the curve on the worker, right after the accents -- the same seam the
// ripples use.  No new geometry, no new draw, nothing on the upload path.
//
// FRAMING.  Every sample's total deformation is clamped to
// [-WDF_NEG_MAX, +WDF_POS_MAX] x the layer weight, and test_distinct_framing
// measures the band with that whole positive allowance added on the crest,
// on top of the accents, at every layer's height cap.
//
// REST IS REST.  With no audio every amplitude eases to exactly 0 and
// wdf_look.live goes false, and wdf_apply() then touches nothing -- the build
// stays bit-identical to the resting wave.
//
// House rules: header-only, pure C, no libm, no PS3 headers, caller-owned
// state.  tests/test_wave_scope.c.

#ifndef WAVE_DEFORM_H
#define WAVE_DEFORM_H

#include "wave_scope.h"

// amplitudes, in the solver's displacement units (as the accents)
#define WDF_MID_A      0.030f
#define WDF_SEC        0.40f     // secondary ripple, of the main one
#define WDF_WAVE_A     0.016f
#define WDF_PULSE_A    0.022f
#define WDF_TILT_A     0.020f
#define WDF_POS_MAX    0.022f    // the framing allowance (per sample, x layer weight) -- measured
#define WDF_NEG_MAX    0.060f
#define WDF_RIM_GAIN   0.40f

static const float WDF_LAYER[3]  = { 1.00f, 0.85f, 0.70f };
static const float WDF_WAVE_W[3] = { 0.40f, 0.70f, 1.00f };
static const float WDF_K1[3]     = { 1.70f, 2.40f, 3.10f };   // ripple cycles along the band
static const float WDF_K2[3]     = { 3.30f, 4.20f, 5.20f };
static const float WDF_PH[3]     = { 0.00f, 0.31f, 0.62f };

typedef struct {
    float wave[WSC_POINTS];
    float wave_a;       // waveform amplitude
    float mid_a;        // ripple amplitude
    float mid_ph;       // main ripple phase, cycles (travels one way)
    float sec_ph;       // secondary ripple phase (travels the other)
    float tilt;         // stereo lean
    float side;         // stereo: ripple weighting toward the louder side, -1..1
    float pulse;        // beat macro pulse
    float rim;          // rim brightness gain, exactly 1 at rest
    int   live;         // 0 = add nothing
} wdf_look;

typedef struct {
    float lvl[3];       // wrm_db_state.lvl: shaped level per layer, 0..1
    float punch[3];     // wrm_db_state.punch: the beat per layer, 0..1
    float onset;        // 1 on the frame an onset fires
    float onset_strength;
    float tempo_hz, tempo_conf;
    float present;      // 0 at rest .. 1 with audio
    const wsc_out *scope;   // may be NULL
} wdf_in;

typedef struct {
    float mid_a, pulse_tgt, pulse, rim, mid_ph, sec_ph;
} wdf_state;

static inline float wdf_clamp(float v, float lo, float hi)
{
    if (v != v) return lo;
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float wdf_k(float dt, float tau) { return dt / (tau + dt); }

// sin(2 pi t) for any t, libm-free (Bhaskara on the folded phase, < 0.2%)
static inline float wdf_sin2pi(float t)
{
    float f = t - (float)(int)t, s = 1.0f, x;
    if (f < 0.0f) f += 1.0f;
    if (f >= 0.5f) { f -= 0.5f; s = -1.0f; }
    x = f * (0.5f - f);
    return s * 16.0f * x / (1.25f - 4.0f * x);
}

static inline float wdf_wrap(float p) { return p - (float)(int)p; }

static inline void wdf_rest(wdf_look *o)
{
    int i;
    if (!o) return;
    for (i = 0; i < WSC_POINTS; i++) o->wave[i] = 0.0f;
    o->wave_a = o->mid_a = o->mid_ph = o->sec_ph = 0.0f;
    o->tilt = o->side = o->pulse = 0.0f;
    o->rim = 1.0f;
    o->live = 0;
}

static inline void wdf_map(wdf_state *st, const wdf_in *in, float dt, wdf_look *o)
{
    int i;
    float present, mids, highs, fast;
    if (!st || !o) return;
    if (!in) { wdf_rest(o); return; }
    if (!(dt > 0.0f)) dt = 0.0f;
    present = wdf_clamp(in->present, 0.0f, 1.0f);
    mids  = wdf_clamp(0.5f * in->lvl[1] + 0.8f * in->punch[1], 0.0f, 1.0f);
    highs = wdf_clamp(0.55f * in->lvl[2] + 0.9f * in->punch[2], 0.0f, 1.0f);
    fast  = wdf_clamp((in->tempo_hz - 1.5f) / 1.5f, 0.0f, 1.0f) * wdf_clamp(in->tempo_conf, 0.0f, 1.0f);

    // mids -> the ripple: quick to swell, slower to settle
    {
        const float t = present * (0.30f + 0.70f * mids) * WDF_MID_A;
        st->mid_a += (t - st->mid_a) * wdf_k(dt, t > st->mid_a ? 0.05f : 0.35f);
    }
    // travelling: both phases always move (a still ripple reads as a bump);
    // a quicker tempo moves them faster
    st->mid_ph = wdf_wrap(st->mid_ph + dt * (0.16f + 0.22f * fast + 0.14f * mids));
    st->sec_ph = wdf_wrap(st->sec_ph + dt * (0.07f + 0.15f * fast));

    // beat -> macro pulse, eased: the target jumps on the onset and decays,
    // the pulse follows it with a short second pole so it swells, not snaps
    if (in->onset > 0.0f && present > 0.0f) {
        const float s = 0.45f + 0.55f * wdf_clamp(in->onset_strength, 0.0f, 1.0f);
        if (s > st->pulse_tgt) st->pulse_tgt = s;
    }
    st->pulse_tgt *= 1.0f / (1.0f + dt / 0.20f);
    st->pulse += (st->pulse_tgt * present - st->pulse) * wdf_k(dt, 0.04f);

    // treble -> the rim, fast in, quick out
    {
        const float t = 1.0f + WDF_RIM_GAIN * highs * present;
        st->rim += (t - st->rim) * wdf_k(dt, t > st->rim ? 0.02f : 0.16f);
    }

    if (present <= 0.0f) {
        // ease out, then snap to exact rest so the build is bit-identical
        if (st->mid_a < 1e-4f) st->mid_a = 0.0f;
        if (st->pulse < 1e-4f) { st->pulse = 0.0f; st->pulse_tgt = 0.0f; }
        if (st->rim - 1.0f < 1e-4f) st->rim = 1.0f;
    }

    o->mid_a  = st->mid_a;
    o->mid_ph = st->mid_ph;
    o->sec_ph = st->sec_ph;
    o->pulse  = st->pulse * WDF_PULSE_A;
    o->rim    = st->rim;
    o->wave_a = present * WDF_WAVE_A * (0.35f + 0.65f * highs);
    if (in->scope) {
        o->tilt = in->scope->balance * WDF_TILT_A * present;
        o->side = in->scope->balance * present;
        for (i = 0; i < WSC_POINTS; i++) o->wave[i] = in->scope->wave[i];
    } else {
        o->tilt = o->side = 0.0f;
        for (i = 0; i < WSC_POINTS; i++) o->wave[i] = 0.0f;
    }
    if (present <= 0.0f) { o->wave_a = 0.0f; o->tilt = 0.0f; o->side = 0.0f; }
    o->live = (o->mid_a > 0.0f || o->pulse > 0.0f || o->wave_a > 0.0f || o->tilt != 0.0f);
}

// Add the deformation to one layer's copy of the curve.  disp spans u in
// [0,1] over n samples, as wave_field.h's do.
static inline void wdf_apply(const wdf_look *d, int layer, float *disp, int n)
{
    int k;
    float du, wl, hi, lo;
    if (!d || !d->live || !disp || n < 2) return;
    if (layer < 0) layer = 0;
    if (layer > 2) layer = 2;
    wl = WDF_LAYER[layer];
    hi = WDF_POS_MAX * wl;
    lo = -WDF_NEG_MAX * wl;
    du = 1.0f / (float)(n - 1);
    for (k = 0; k < n; k++) {
        const float u  = (float)k * du;
        const float c  = 2.0f * u - 1.0f;                 // -1 left .. +1 right
        float side = 1.0f + 0.8f * d->side * c;
        float x;
        if (side < 0.2f) side = 0.2f;
        x  = d->mid_a * wl * side
           * (wdf_sin2pi(WDF_K1[layer] * u - d->mid_ph + WDF_PH[layer])
              + WDF_SEC * wdf_sin2pi(WDF_K2[layer] * u + d->sec_ph - WDF_PH[layer]));
        {
            const float s  = u * (float)(WSC_POINTS - 1);
            int         i  = (int)s;
            float       fr;
            if (i > WSC_POINTS - 2) i = WSC_POINTS - 2;
            fr = s - (float)i;
            x += d->wave_a * WDF_WAVE_W[layer]
               * (d->wave[i] + (d->wave[i + 1] - d->wave[i]) * fr);
        }
        x += d->pulse * wl * (1.0f - c * c);              // broad swell, 0 at the ends
        x += d->tilt * wl * c;                            // lean toward the louder side
        disp[k] += wdf_clamp(x, lo, hi);
    }
}

#endif // WAVE_DEFORM_H
