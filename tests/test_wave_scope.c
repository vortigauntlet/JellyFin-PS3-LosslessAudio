// Host tests for JellyWave 2.0's scope (wave_scope.h) and shape deformation
// (wave_deform.h).  libm is fine here; the kernels may not use it.

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "wave_scope.h"
#include "wave_deform.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

#define RATE 48000
#define BLK  800                /* one 60 fps frame of audio */
#define DT   (1.0f / 60.0f)
#define N    72                 /* WF_SAMPLES */

static float buf[BLK * 2];
static long  t_abs = 0;

// kind: 0 mono sine, 1 right only, 2 left only, 3 anti-phase
static void gen(int kind, float amp, float hz)
{
    int i;
    for (i = 0; i < BLK; i++) {
        const float v = amp * sinf(2.0f * 3.14159265f * hz * (float)(t_abs + i) / RATE);
        float l = v, r = v;
        if (kind == 1) l = 0.0f;
        if (kind == 2) r = 0.0f;
        if (kind == 3) r = -v;
        buf[2 * i] = l; buf[2 * i + 1] = r;
    }
    t_abs += BLK;
}

static void run(wsc_state *s, wsc_out *o, int kind, float amp, float hz, int frames, float present)
{
    int f;
    for (f = 0; f < frames; f++) {
        gen(kind, amp, hz);
        wsc_push(s, buf, BLK);
        wsc_frame(s, DT, present, o);
    }
}

static float wave_peak(const wsc_out *o)
{
    float p = 0.0f; int i;
    for (i = 0; i < WSC_POINTS; i++) if (fabsf(o->wave[i]) > p) p = fabsf(o->wave[i]);
    return p;
}

static void test_scope(void)
{
    static wsc_state s;
    wsc_out o;
    float quiet, loud;
    int i;

    wsc_init(&s);
    run(&s, &o, 1, 0.5f, 220.0f, 60, 1.0f);
    printf("  scope: right only -> balance %+.2f\n", o.balance);
    CHECK(o.balance > 0.8f, "right-only audio does not lean right (%.2f)", o.balance);
    run(&s, &o, 2, 0.5f, 220.0f, 90, 1.0f);
    printf("  scope: left only  -> balance %+.2f\n", o.balance);
    CHECK(o.balance < -0.8f, "left-only audio does not lean left (%.2f)", o.balance);
    run(&s, &o, 0, 0.5f, 220.0f, 90, 1.0f);
    printf("  scope: mono       -> balance %+.2f width %.2f\n", o.balance, o.width);
    CHECK(fabsf(o.balance) < 0.1f && o.width < 0.05f, "mono is not centred and narrow");
    run(&s, &o, 3, 0.5f, 220.0f, 90, 1.0f);
    printf("  scope: anti-phase -> width %.2f\n", o.width);
    CHECK(o.width > 0.8f, "anti-phase audio does not read as wide (%.2f)", o.width);

    // the waveform: bounded, tapered to 0 at both ends, level-independent
    run(&s, &o, 0, 0.05f, 180.0f, 600, 1.0f);   /* the normaliser falls slowly by design */
    for (quiet = 0.0f, i = 0; i < 30; i++) {       /* peak over half a second: */
        run(&s, &o, 0, 0.05f, 180.0f, 1, 1.0f);   /* one frame's depends on   */
        if (wave_peak(&o) > quiet) quiet = wave_peak(&o);   /* the taper phase */
    }
    run(&s, &o, 0, 0.9f, 180.0f, 150, 1.0f);
    for (loud = 0.0f, i = 0; i < 30; i++) {
        run(&s, &o, 0, 0.9f, 180.0f, 1, 1.0f);
        if (wave_peak(&o) > loud) loud = wave_peak(&o);
    }
    printf("  scope: waveform peak quiet %.2f loud %.2f, ends %.3f %.3f\n",
           quiet, loud, o.wave[0], o.wave[WSC_POINTS - 1]);
    CHECK(loud > 0.2f && loud <= 1.0f, "waveform peak %.2f out of range", loud);
    CHECK(fabsf(quiet - loud) < 0.15f, "waveform size follows loudness (%.2f vs %.2f)", quiet, loud);
    CHECK(o.wave[0] == 0.0f && o.wave[WSC_POINTS - 1] == 0.0f, "waveform ends are not pinned");

    // silence (present 0): everything eases back to neutral
    run(&s, &o, 0, 0.0f, 180.0f, 120, 0.0f);
    printf("  scope: silence    -> balance %+.3f width %.3f wave %.3f\n", o.balance, o.width, wave_peak(&o));
    CHECK(fabsf(o.balance) < 0.01f && o.width < 0.01f && wave_peak(&o) < 0.01f,
          "silence does not return the scope to neutral");

    // hostile input stays finite
    for (i = 0; i < BLK * 2; i++) buf[i] = (i % 3) ? NAN : INFINITY;
    wsc_push(&s, buf, BLK);
    wsc_frame(&s, DT, 1.0f, &o);
    CHECK(o.balance == o.balance && o.width == o.width, "NaN/inf in -> NaN out");
    for (i = 0; i < WSC_POINTS; i++) CHECK(o.wave[i] == o.wave[i], "NaN waveform point %d", i);
}

static void in_rest(wdf_in *in, const wsc_out *sc)
{
    memset(in, 0, sizeof *in);
    in->scope = sc;
}

static void test_deform(void)
{
    static wsc_out sc;
    wdf_state st;
    wdf_look  d;
    wdf_in    in;
    float disp[N], ref[N];
    int i, l, f;

    // rest is rest: nothing live, the curve is untouched bit for bit
    memset(&st, 0, sizeof st); st.rim = 1.0f;
    memset(&sc, 0, sizeof sc);
    in_rest(&in, &sc);
    for (f = 0; f < 10; f++) wdf_map(&st, &in, DT, &d);
    for (i = 0; i < N; i++) disp[i] = ref[i] = 0.1f * sinf((float)i);
    for (l = 0; l < 3; l++) wdf_apply(&d, l, disp, N);
    CHECK(!d.live && d.rim == 1.0f, "rest is live (%d) or rim %.3f", d.live, d.rim);
    CHECK(memcmp(disp, ref, sizeof disp) == 0, "rest changes the curve");

    // everything at full: every sample inside the framing allowance
    {
        float lo = 1.0f, hi = -1.0f;
        for (i = 0; i < WSC_POINTS; i++) sc.wave[i] = (i & 1) ? 1.0f : -1.0f;
        sc.balance = 1.0f; sc.width = 1.0f;
        in_rest(&in, &sc);
        in.present = 1.0f; in.tempo_hz = 3.0f; in.tempo_conf = 1.0f;
        for (i = 0; i < 3; i++) { in.lvl[i] = 1.0f; in.punch[i] = 1.0f; }
        for (f = 0; f < 600; f++) {
            in.onset = (f % 20) == 0; in.onset_strength = 1.0f;
            wdf_map(&st, &in, DT, &d);
            for (l = 0; l < 3; l++) {
                memset(disp, 0, sizeof disp);
                wdf_apply(&d, l, disp, N);
                for (i = 0; i < N; i++) {
                    const float a = disp[i] / WDF_LAYER[l];
                    if (a > hi) hi = a;
                    if (a < lo) lo = a;
                }
            }
        }
        printf("  deform: full drive -> per-sample range %+.4f .. %+.4f (allowance %+.3f)\n",
               lo, hi, WDF_POS_MAX);
        CHECK(hi <= WDF_POS_MAX + 1e-6f && lo >= -WDF_NEG_MAX - 1e-6f, "deformation escapes its allowance");
        CHECK(d.rim > 1.3f && d.rim <= 1.0f + WDF_RIM_GAIN + 1e-4f, "treble rim gain %.2f", d.rim);
    }

    // the beat pulse swells and settles: not in one frame, gone within a second
    {
        float p[90], peak = 0.0f; int at = 0;
        memset(&st, 0, sizeof st); st.rim = 1.0f;
        memset(&sc, 0, sizeof sc);
        in_rest(&in, &sc);
        in.present = 1.0f;
        for (f = 0; f < 90; f++) {
            in.onset = (f == 5); in.onset_strength = 1.0f;
            wdf_map(&st, &in, DT, &d);
            p[f] = d.pulse;
            if (p[f] > peak) { peak = p[f]; at = f; }
        }
        printf("  deform: beat pulse first frame %.4f, peak %.4f at +%d frames, after 1 s %.4f\n",
               p[5], peak, at - 5, p[65]);
        CHECK(peak > 0.5f * WDF_PULSE_A, "beat pulse too weak (%.4f)", peak);
        CHECK(p[5] < 0.6f * peak, "beat pulse snaps instead of swelling");
        CHECK(p[65] < 0.1f * peak, "beat pulse lingers");
    }

    // stereo: right-heavy audio deforms the right half more than the left
    {
        float left = 0.0f, right = 0.0f;
        memset(&st, 0, sizeof st); st.rim = 1.0f;
        memset(&sc, 0, sizeof sc);
        sc.balance = 1.0f;
        in_rest(&in, &sc);
        in.present = 1.0f; in.lvl[1] = 0.8f;
        for (f = 0; f < 120; f++) wdf_map(&st, &in, DT, &d);
        memset(disp, 0, sizeof disp);
        wdf_apply(&d, 0, disp, N);
        for (i = 0; i < N; i++) { if (i < N / 2) left += disp[i]; else right += disp[i]; }
        printf("  deform: balance right -> mean left %+.4f right %+.4f\n", left / (N / 2), right / (N / 2));
        CHECK(right > left + 0.1f, "stereo does not lean the band");
    }

    // mids drive the ripple, and it travels
    {
        float a_lo, a_hi; int k0, k1, best; float bv;
        memset(&st, 0, sizeof st); st.rim = 1.0f;
        memset(&sc, 0, sizeof sc);
        in_rest(&in, &sc);
        in.present = 1.0f; in.lvl[1] = 0.0f;
        for (f = 0; f < 120; f++) wdf_map(&st, &in, DT, &d);
        a_lo = d.mid_a;
        in.lvl[1] = 1.0f; in.punch[1] = 1.0f;
        for (f = 0; f < 120; f++) wdf_map(&st, &in, DT, &d);
        a_hi = d.mid_a;
        memset(disp, 0, sizeof disp); wdf_apply(&d, 1, disp, N);
        for (best = 0, bv = -9.0f, i = 0; i < N; i++) if (disp[i] > bv) { bv = disp[i]; best = i; }
        k0 = best;
        for (f = 0; f < 30; f++) wdf_map(&st, &in, DT, &d);
        memset(disp, 0, sizeof disp); wdf_apply(&d, 1, disp, N);
        for (best = 0, bv = -9.0f, i = 0; i < N; i++) if (disp[i] > bv) { bv = disp[i]; best = i; }
        k1 = best;
        printf("  deform: ripple amp quiet mids %.4f loud mids %.4f; crest moved %d -> %d in 0.5 s\n",
               a_lo, a_hi, k0, k1);
        CHECK(a_hi > 2.0f * a_lo, "mids do not drive the ripple");
        CHECK(k0 != k1, "the ripple does not travel");
    }
}

static void report_cost(void)
{
    static wsc_state s;
    static wsc_out   o;
    wdf_state st; wdf_look d; wdf_in in;
    float disp[N];
    clock_t t0; int f, l;
    const int F = 20000;
    wsc_init(&s);
    memset(&st, 0, sizeof st); st.rim = 1.0f;
    memset(&in, 0, sizeof in);
    in.present = 1.0f; in.lvl[1] = 0.5f; in.scope = &o;
    t0 = clock();
    for (f = 0; f < F; f++) {
        gen(0, 0.5f, 220.0f);
        wsc_push(&s, buf, BLK);
        wsc_frame(&s, DT, 1.0f, &o);
        wdf_map(&st, &in, DT, &d);
        for (l = 0; l < 3; l++) { memset(disp, 0, sizeof disp); wdf_apply(&d, l, disp, N); }
    }
    printf("  cost: scope + map + apply(3 layers) = %.1f us/frame on this host "
           "(signal generation included)\n",
           1e6 * (double)(clock() - t0) / CLOCKS_PER_SEC / F);
}

int main(void)
{
    printf("-- scope --\n");   test_scope();
    printf("-- deform --\n");  test_deform();
    printf("-- cost --\n");    report_cost();
    if (failures) { printf("\nFAILED: %d check(s)\n", failures); return 1; }
    printf("\nOK\n");
    return 0;
}
