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
    ws_ctl c = { 0.5f, 0.5f, 0.0f, 0.5f };
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
