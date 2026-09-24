// Host test for render/wave_snow.h.
#include <stdio.h>
#include <string.h>
#include "wave_snow.h"

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

static ws_state  st;
static ws_sprite sp[WS_MAX];

int main(void)
{
    static float wv[WS_WV];
    ws_ctl c = { 0.5f, 0.5f, 0.0f, 0.5f, 0, -0.3f };
    int i, f, near = 0, vis;
    ws_init(&st, WS_COUNT_DEF, 1234u);
    for (i = 0; i < st.n; i++) near += st.z[i] < 0.22f;
    printf("  snow: %d particles, %d near (bokeh)\n", st.n, near);
    CHECK(near > st.n / 20 && near < st.n / 4, "bokeh share %d/%d", near, st.n);

    // ten minutes, with a kick every second: everything stays finite and on the field
    for (f = 0; f < 60 * 600; f++) {
        c.kick = (f % 60) == 0 ? 1.0f : 0.0f;
        ws_step(&st, &c, 1.0f / 60.0f);
    }
    for (i = 0; i < st.n; i++) {
        CHECK(st.x[i] == st.x[i] && st.y[i] == st.y[i], "NaN particle %d", i);
        CHECK(st.x[i] >= -WS_X_EDGE - 0.01f && st.x[i] <= WS_X_EDGE + 0.01f, "x out %d %f", i, st.x[i]);
        CHECK(st.y[i] >= -WS_Y_EDGE - 0.2f && st.y[i] <= WS_Y_EDGE + 0.25f, "y out %d %f", i, st.y[i]);
    }

    // a kick moves nothing: the sub-bass may glint the field, never push it
    {
        float vy0 = 0.0f, vy1 = 0.0f;
        c.kick = 0.0f;
        for (f = 0; f < 120; f++) ws_step(&st, &c, 1.0f / 60.0f);
        for (i = 0; i < st.n; i++) vy0 += st.vy[i];
        c.kick = 1.0f; ws_step(&st, &c, 1.0f / 60.0f);
        for (i = 0; i < st.n; i++) vy1 += st.vy[i];
        printf("  snow: mean vy %.4f -> %.4f on a kick\n", vy0 / st.n, vy1 / st.n);
        CHECK(vy1 / st.n < vy0 / st.n + 0.01f, "the kick lifts the field");
    }

    // the field fills the screen: every quarter of it holds particles
    {
        int q[4] = { 0, 0, 0, 0 };
        for (i = 0; i < st.n; i++)
            if (st.x[i] > -1.0f && st.x[i] < 1.0f && st.y[i] > -1.0f && st.y[i] < 1.0f)
                q[(st.x[i] >= 0.0f) + 2 * (st.y[i] >= 0.0f)]++;
        printf("  snow: quadrants %d %d %d %d\n", q[0], q[1], q[2], q[3]);
        for (i = 0; i < 4; i++) CHECK(q[i] > st.n / 10, "quadrant %d holds %d", i, q[i]);
    }

    // a rising wave lifts what floats near the band, and only there
    {
        float near_vy = 0.0f, far_vy = 0.0f; int nn = 0, nf = 0;
        for (i = 0; i < WS_WV; i++) wv[i] = 0.8f;
        c.wv = wv;
        for (i = 0; i < st.n; i++) st.vy[i] = 0.0f;
        ws_step(&st, &c, 1.0f / 60.0f);
        for (i = 0; i < st.n; i++) {
            const float d = st.y[i] - c.band_y;
            if (d > -0.2f && d < 0.2f) { near_vy += st.vy[i]; nn++; }
            if (d > 0.9f || d < -0.9f) { far_vy += st.vy[i]; nf++; }
        }
        near_vy /= nn ? nn : 1; far_vy /= nf ? nf : 1;
        printf("  snow: wave lift near %.4f far %.4f\n", near_vy, far_vy);
        CHECK(near_vy > far_vy + 0.005f, "the wave does not carry the particles near it");
        c.wv = 0;
    }

    // presence 0 draws nothing; presence 1 draws most of them
    c.kick = 0.0f;
    vis = ws_shade(&st, &c, 0.0f, sp, WS_MAX);
    CHECK(vis == 0, "%d particles drawn at zero presence", vis);
    vis = ws_shade(&st, &c, 1.0f, sp, WS_MAX);
    printf("  snow: %d of %d drawn\n", vis, st.n);
    CHECK(vis > st.n / 2, "only %d drawn", vis);

    printf(g_fail ? "FAILED: %d check(s)\n" : "OK\n", g_fail);
    return g_fail != 0;
}
