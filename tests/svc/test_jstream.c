// Host tests for the streaming JSON reader: every fixture read in chunks of every size must deliver exactly the elements
// and members a whole-document read does, and malformed or oversized input must fail or skip, never crash.
//
//   test_jstream <fixtures/sc>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sc_jstream.h"
#include "sx_json.h"

static int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_MSG(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char *slurp(const char *dir, const char *name, size_t *len) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
    b[n] = 0;
    fclose(f);
    *len = (size_t)n;
    return b;
}

// A reader that hands out `step` bytes at a time.
typedef struct { const char *p; size_t n, pos, step, fail_at; } Rd;
static int rd_read(void *c, void *buf, int n) {
    Rd *r = (Rd *)c;
    if (r->fail_at && r->pos >= r->fail_at) return -1;
    size_t k = r->n - r->pos;
    if (k > r->step) k = r->step;
    if (k > (size_t)n) k = (size_t)n;
    memcpy(buf, r->p + r->pos, k);
    r->pos += k;
    return (int)k;
}

typedef struct {
    int ne, nm;
    int64_t ids[256];
    char keys[16][32];
    char next[400];
    char title[128];
    int arrays_seen;
} Got;

static bool on_elem(void *ud, const char *key, const char *json, size_t len) {
    Got *g = (Got *)ud;
    sx_jv v = sx_j_root(json, len);
    CHECK(sx_j_valid(json, len));
    if (g->ne < 256) {
        sx_jv inner;
        int64_t id = sx_j_get_int(v, "id", 0);
        if (!id && sx_j_get(v, "track", &inner)) id = sx_j_get_int(inner, "id", 0);
        if (!id && sx_j_get(v, "playlist", &inner)) id = sx_j_get_int(inner, "id", 0);
        g->ids[g->ne] = id;
    }
    g->ne++;
    (void)key;
    return true;
}
static void on_member(void *ud, const char *key, const char *json, size_t len) {
    Got *g = (Got *)ud;
    sx_jv v = sx_j_root(json, len);
    if (!strcmp(key, "next_href")) sx_j_str(v, g->next, sizeof(g->next));
    else if (!strcmp(key, "title")) sx_j_str(v, g->title, sizeof(g->title));
    g->nm++;
}

// What the whole-document parser says about the same file.
static void dom_expect(const char *doc, size_t len, const char *arr_key, Got *want) {
    memset(want, 0, sizeof(*want));
    sx_jv root = sx_j_root(doc, len), arr;
    if (sx_j_type(root) == SXJ_ARR) arr = root;
    else if (!sx_j_get(root, arr_key, &arr)) arr.p = NULL;
    if (arr.p) {
        sx_jit it = sx_j_iter(arr);
        sx_jv e;
        while (sx_j_next(&it, NULL, &e)) {
            sx_jv inner;
            int64_t id = sx_j_get_int(e, "id", 0);
            if (!id && sx_j_get(e, "track", &inner)) id = sx_j_get_int(inner, "id", 0);
            if (!id && sx_j_get(e, "playlist", &inner)) id = sx_j_get_int(inner, "id", 0);
            if (want->ne < 256) want->ids[want->ne] = id;
            want->ne++;
        }
    }
    sx_j_get_str(root, "next_href", want->next, sizeof(want->next));
    sx_j_get_str(root, "title", want->title, sizeof(want->title));
}

static void check_file(const char *dir, const char *name, const char *arr_key) {
    size_t len;
    char *doc = slurp(dir, name, &len);
    Got want;
    dom_expect(doc, len, arr_key, &want);
    static const size_t steps[] = { 1, 2, 3, 7, 64, 511, 2048, 100000 };
    for (size_t s = 0; s < sizeof(steps) / sizeof(steps[0]); s++) {
        Got g;
        memset(&g, 0, sizeof(g));
        Rd r = { doc, len, 0, steps[s], 0 };
        sc_jcb cb = { on_elem, on_member, &g };
        sc_jstats st;
        const int rc = sc_jstream_run(rd_read, &r, &cb, 64 * 1024, &st);
        CHECK_MSG(rc == SC_J_OK, "%s step %zu rc %d", name, steps[s], rc);
        CHECK_MSG(g.ne == want.ne, "%s step %zu: %d elements, DOM %d", name, steps[s], g.ne, want.ne);
        for (int i = 0; i < g.ne && i < 256; i++) CHECK_MSG(g.ids[i] == want.ids[i], "%s element %d id", name, i);
        CHECK_MSG(!strcmp(g.next, want.next), "%s next_href", name);
        CHECK_MSG(!strcmp(g.title, want.title), "%s title", name);
        CHECK(st.skipped == 0 && st.elements == want.ne);
    }
    // Every truncation point: an error or a clean prefix, never a crash and never a callback with a broken element.
    for (size_t cut = 0; cut < len; cut += (len / 97) + 1) {
        Got g;
        memset(&g, 0, sizeof(g));
        Rd r = { doc, cut, 0, 50, 0 };
        sc_jcb cb = { on_elem, on_member, &g };
        const int rc = sc_jstream_run(rd_read, &r, &cb, 64 * 1024, NULL);
        CHECK(rc != SC_J_OK);
        CHECK(g.ne <= want.ne);
    }
    // A reader that fails.
    {
        Got g;
        memset(&g, 0, sizeof(g));
        Rd r = { doc, len, 0, 100, len / 2 };
        sc_jcb cb = { on_elem, on_member, &g };
        CHECK(sc_jstream_run(rd_read, &r, &cb, 64 * 1024, NULL) == SC_J_E_READ);
    }
    // An element larger than the work buffer is skipped and counted, and the rest still arrive.
    {
        Got g;
        memset(&g, 0, sizeof(g));
        Rd r = { doc, len, 0, 4096, 0 };
        sc_jcb cb = { on_elem, on_member, &g };
        sc_jstats st;
        const int rc = sc_jstream_run(rd_read, &r, &cb, 600, &st);
        CHECK(rc == SC_J_OK);
        CHECK_MSG(st.skipped + g.ne >= want.ne - 0 || want.ne == 0, "%s small buffer: %d skipped + %d", name, st.skipped, g.ne);
    }
    free(doc);
}

static bool stop_after_two(void *ud, const char *key, const char *json, size_t len) {
    (void)key; (void)json; (void)len;
    return ++((Got *)ud)->ne < 2;
}

static const char *run_str(const char *doc, Got *g, size_t step, int *rc) {
    memset(g, 0, sizeof(*g));
    Rd r = { doc, strlen(doc), 0, step, 0 };
    sc_jcb cb = { on_elem, on_member, g };
    *rc = sc_jstream_run(rd_read, &r, &cb, 4096, NULL);
    return doc;
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: test_jstream <fixtures>\n"); return 2; }
    check_file(argv[1], "user_likes.json", "collection");
    check_file(argv[1], "user_tracks.json", "collection");
    check_file(argv[1], "search_tracks.json", "collection");
    check_file(argv[1], "user_playlists.json", "collection");
    check_file(argv[1], "playlist.json", "tracks");
    check_file(argv[1], "tracks_by_ids.json", "");

    Got g;
    int rc;
    // Awkward text: braces and quotes inside strings, escapes, nesting, empty arrays, scalars and literals at the end.
    run_str("{\"a\":1,\"collection\":[{\"id\":1,\"t\":\"}{][,\\\"\"},{\"id\":2,\"x\":[1,[2,{\"y\":3}]]}],\"next_href\":\"u\\\"v\",\"n\":null,\"b\":true,\"o\":{\"p\":[1]}}", &g, 1, &rc);
    CHECK(rc == SC_J_OK && g.ne == 2 && g.ids[0] == 1 && g.ids[1] == 2 && g.nm == 5);
    CHECK(!strcmp(g.next, "u\"v") || !strcmp(g.next, "u\\\"v"));
    run_str("{\"collection\":[],\"next_href\":null}", &g, 3, &rc);
    CHECK(rc == SC_J_OK && g.ne == 0);
    run_str("[ {\"id\":5} , {\"id\":6}\n]", &g, 2, &rc);
    CHECK(rc == SC_J_OK && g.ne == 2 && g.ids[1] == 6);
    run_str("[]", &g, 1, &rc);
    CHECK(rc == SC_J_OK && g.ne == 0);
    run_str("[1,2.5,\"s\",null,true]", &g, 1, &rc);
    CHECK(rc == SC_J_OK && g.ne == 5);
    run_str("{\"a\":[{\"id\":1}],\"b\":[{\"id\":2}]}", &g, 5, &rc);
    CHECK(rc == SC_J_OK && g.ne == 2);
    // Malformed.
    static const char *bad[] = { "", "x", "{\"a\" 1}", "{\"a\":1 \"b\":2}", "{1:2}", "[1 2]", "{\"a\":[1,2}", "{" , "{\"a\":" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        run_str(bad[i], &g, 2, &rc);
        CHECK_MSG(rc != SC_J_OK, "bad case %zu accepted", i);
    }
    // Stop early.
    {
        struct S { int n; } s = { 0 };
        (void)s;
        Rd r = { "{\"c\":[{\"id\":1},{\"id\":2},{\"id\":3}]}", 0, 0, 4, 0 };
        r.n = strlen(r.p);
        Got gg;
        memset(&gg, 0, sizeof gg);
        sc_jcb cb = { NULL, NULL, &gg };
        cb.elem = stop_after_two;
        sc_jstats st;
        CHECK(sc_jstream_run(rd_read, &r, &cb, 256, &st) == SC_J_OK && st.stopped && gg.ne == 2);
    }
    printf("test_jstream: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
