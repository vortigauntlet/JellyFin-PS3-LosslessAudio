// Host test for the Settings model (source/ui/settings_model.c): the rows,
// their sections and help text, and the layout and scroll rules.  Built twice,
// with the player statistics row compiled in and out:
//
//   make -f Makefile.host test_settings_model test_settings_model_nostats
#include <stdio.h>
#include <string.h>

#ifndef ENABLE_PLAYER_STATS
#define ENABLE_PLAYER_STATS 1
#endif
#include "settings_model.h"

static int s_checks = 0, s_failed = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void rows(void) {
    printf("- rows\n");
    const int n = settings_count();
    int seen[SET__COUNT] = { 0 };
    for (int i = 0; i < n; i++) {
        const setting_row *r = settings_row(i);
        CHECK(r != NULL);
        if (!r) continue;
        CHECK((int)r->id >= 0 && (int)r->id < SET__COUNT);
        seen[r->id]++;
        CHECK(settings_index_of(r->id) == i);
        // Labels fit the row.
        CHECK(strlen(r->label) >= 1 && strlen(r->label) <= 24);
        // Help is exactly two lines, each short enough for the panel.
        const char *nl = strchr(r->help, '\n');
        CHECK(nl != NULL && strchr(nl + 1, '\n') == NULL);
        if (nl) {
            CHECK((int)(nl - r->help) >= 1 && (int)(nl - r->help) <= 72);
            CHECK(strlen(nl + 1) >= 1 && strlen(nl + 1) <= 72);
            if ((int)(nl - r->help) > 72 || strlen(nl + 1) > 72)
                printf("  too long: %s\n", r->label);
        }
    }
    // Every id exactly once, except statistics when it is compiled out.
    for (int id = 0; id < SET__COUNT; id++) {
        const int want = (id == SET_STATS && !ENABLE_PLAYER_STATS) ? 0 : 1;
        CHECK(seen[id] == want);
        if (seen[id] != want) printf("  id %d seen %d times\n", id, seen[id]);
    }
    CHECK(n == SET__COUNT - (ENABLE_PLAYER_STATS ? 0 : 1));
    CHECK(settings_row(-1) == NULL && settings_row(n) == NULL);
    if (!ENABLE_PLAYER_STATS) CHECK(settings_index_of(SET_STATS) == -1);
}

static void sections(void) {
    printf("- sections\n");
    const int n = settings_count();
    // Grouped: a section never reappears after another has started.
    int closed[SEC__COUNT] = { 0 };
    int cur = -1;
    for (int i = 0; i < n; i++) {
        const int s = (int)settings_row(i)->section;
        if (s != cur) {
            CHECK(!closed[s]);
            if (cur >= 0) closed[cur] = 1;
            cur = s;
        }
    }
    // The first row of each non-empty section, and -1 for an empty one.
    for (int s = 0; s < SEC__COUNT; s++) {
        const int first = settings_section_first((setting_section)s);
        CHECK(first >= 0);                           // none is empty here
        if (first < 0) continue;
        CHECK((int)settings_row(first)->section == s);
        CHECK(first == 0 || (int)settings_row(first - 1)->section != s);
        CHECK(settings_section_label((setting_section)s)[0] != '\0');
    }
    CHECK(settings_section_first(SEC__COUNT) == -1);
    // Log Out sits last, in System: it is the most destructive action.
    CHECK(settings_index_of(SET_LOGOUT) == n - 1);
    // First focus is 1080p Playback, the first row of Playback.
    CHECK(settings_row(0)->id == SET_HD1080);

    // L2 / R2: the first row of the previous / next section.
    CHECK(settings_section_jump(0, -1) == -1);                 // already in the first
    CHECK(settings_section_jump(n - 1, +1) == -1);             // already in the last
    int at = 0, hops = 0;
    while ((at = settings_section_jump(at, +1)) >= 0) {
        hops++;
        CHECK(at == 0 || settings_row(at - 1)->section != settings_row(at)->section);
    }
    CHECK(hops == SEC__COUNT - 1);
    // From the middle of a section, back goes to the section before (not its own head).
    const int audio2 = settings_index_of(SET_DIALOGUE);
    CHECK(settings_section_jump(audio2, -1) == settings_section_first(SEC_PLAYBACK));
    CHECK(settings_section_jump(audio2, +1) == settings_section_first(SEC_SUBTITLES));
}

// A layout the way the screen builds it, at one of the sizes it runs at.
struct Geom { const char *name; int header_h, row_h, pitch, band_h; };

static int visible(const settings_item *it, int scroll, int band_h) {
    return it->y - scroll >= 0 && it->y + it->h - scroll <= band_h;
}

static void check_sel(const settings_item *items, int n, int sel, int band_h,
                      int scroll, int total_h, const char *where) {
    const int k = settings_item_of_row(items, n, sel);
    CHECK(k >= 0);
    if (k < 0) return;
    CHECK(visible(&items[k], scroll, band_h));
    if (!visible(&items[k], scroll, band_h))
        printf("  %s: row %d not visible at scroll %d\n", where, sel, scroll);
    // The first row of a section shows its header too.
    if (k > 0 && items[k - 1].is_header) {
        CHECK(visible(&items[k - 1], scroll, band_h));
        if (!visible(&items[k - 1], scroll, band_h))
            printf("  %s: header of row %d not visible at scroll %d\n", where, sel, scroll);
    }
    CHECK(scroll >= 0);
    CHECK(total_h <= band_h ? scroll == 0 : scroll <= total_h - band_h);
}

static void layout(void) {
    printf("- layout and scroll\n");
    // 720p, 1080p (the UI scale 1.5), a band barely taller than a header and a
    // row, and the smallest the screen could ever offer.
    static const struct Geom G[] = {
        { "720p",  34, 56, 66, 330 },
        { "1080p", 51, 84, 99, 600 },
        { "tight", 34, 56, 66, 34 + 56 },
        { "480p",  26, 42, 50, 200 },
    };
    for (unsigned g = 0; g < sizeof(G) / sizeof(G[0]); g++) {
        const struct Geom *q = &G[g];
        settings_item items[SETTINGS_MAX_ITEMS];
        int total = 0;
        const int n_items = settings_layout(items, SETTINGS_MAX_ITEMS, q->header_h,
                                            q->row_h, q->pitch, &total);
        const int n = settings_count();
        int headers = 0;
        for (int k = 0; k < n_items; k++) headers += items[k].is_header;
        CHECK(headers == SEC__COUNT && n_items == n + SEC__COUNT);
        // Content is laid out top to bottom without overlap.
        for (int k = 1; k < n_items; k++)
            CHECK(items[k].y >= items[k - 1].y + items[k - 1].h);
        CHECK(items[0].is_header && items[0].y == 0);
        CHECK(items[n_items - 1].y + items[n_items - 1].h == total);

        // Walk down, then up, one row at a time, as the d-pad does.
        int scroll = 0;
        for (int sel = 0; sel < n; sel++) {
            scroll = settings_scroll_for(items, n_items, sel, q->band_h, scroll, total);
            check_sel(items, n_items, sel, q->band_h, scroll, total, q->name);
        }
        for (int sel = n - 1; sel >= 0; sel--) {
            scroll = settings_scroll_for(items, n_items, sel, q->band_h, scroll, total);
            check_sel(items, n_items, sel, q->band_h, scroll, total, q->name);
        }
        // Jump straight to every row from every other scroll position, as L2 and
        // R2 do and as a long press does.
        for (int from = 0; from < n; from++) {
            int s0 = settings_scroll_for(items, n_items, from, q->band_h, 0, total);
            for (int sel = 0; sel < n; sel++) {
                const int s1 = settings_scroll_for(items, n_items, sel, q->band_h, s0, total);
                check_sel(items, n_items, sel, q->band_h, s1, total, q->name);
            }
        }
        // Minimal movement: a row already on screen does not move the list.
        scroll = settings_scroll_for(items, n_items, 1, q->band_h, 0, total);
        CHECK(settings_scroll_for(items, n_items, 1, q->band_h, scroll, total) == scroll);
    }
    // When everything fits, nothing scrolls.
    {
        settings_item items[SETTINGS_MAX_ITEMS];
        int total = 0;
        const int n_items = settings_layout(items, SETTINGS_MAX_ITEMS, 34, 56, 66, &total);
        CHECK(settings_scroll_for(items, n_items, settings_count() - 1, total + 100, 0, total) == 0);
    }
    // A row that is not in the list leaves the scroll where it was.
    {
        settings_item items[SETTINGS_MAX_ITEMS];
        int total = 0;
        const int n_items = settings_layout(items, SETTINGS_MAX_ITEMS, 34, 56, 66, &total);
        CHECK(settings_scroll_for(items, n_items, 999, 330, 40, total) == 40);
    }
}

int main(void) {
    rows();
    sections();
    layout();
    printf("settings model (stats %s): %d checks, %d failed\n",
           ENABLE_PLAYER_STATS ? "in" : "out", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
