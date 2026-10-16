// sx_json.c: value views, iteration, string decoding, and robustness against
// malformed and truncated documents (including mutated copies of the real
// fixtures).  Run from tests/ so fixtures/stremio resolves.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sx_json.h"

static int g_fail, g_run;
#define CHECK(c) do { g_run++; if (!(c)) { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_STR(a, b) do { g_run++; const char *_a = (a), *_b = (b); if (strcmp(_a, _b) != 0) { g_fail++; printf("FAIL %s:%d  \"%s\" != \"%s\"\n", __FILE__, __LINE__, _a, _b); } } while (0)

static uint32_t rs = 987654321u;
static uint32_t rnd(void) { rs = rs * 1664525u + 1013904223u; return rs >> 8; }

static sx_jv root(const char *s) { return sx_j_root(s, strlen(s)); }

static void test_types(void) {
    CHECK(sx_j_type(root("{}")) == SXJ_OBJ);
    CHECK(sx_j_type(root("[]")) == SXJ_ARR);
    CHECK(sx_j_type(root("\"x\"")) == SXJ_STR);
    CHECK(sx_j_type(root("-12.5e3")) == SXJ_NUM);
    CHECK(sx_j_type(root("true")) == SXJ_BOOL);
    CHECK(sx_j_type(root("null")) == SXJ_NULL);
    CHECK(sx_j_type(root("")) == SXJ_NONE);
    CHECK(sx_j_type(root("   ")) == SXJ_NONE);
    CHECK(sx_j_type(root("?")) == SXJ_NONE);
    CHECK(!sx_j_present(root("")));
    CHECK(sx_j_type(root("\xEF\xBB\xBF{\"a\":1}")) == SXJ_OBJ);      // BOM
}

static void test_lookup(void) {
    const char *doc = "{ \"a\": 1, \"b\" : [1, 2, {\"c\": \"deep\"}], \"d\":{\"a\":99}, \"e\":\"s\\\"q\", \"k\\u0065y\":7, \"n\":null, \"t\":true }";
    sx_jv r = root(doc), v;
    CHECK(sx_j_valid(doc, strlen(doc)));
    CHECK(sx_j_get(r, "a", &v) && sx_j_get_int(r, "a", -1) == 1);       // the top-level a, not d.a
    CHECK(sx_j_get(r, "b", &v) && sx_j_type(v) == SXJ_ARR && sx_j_len(v) == 3);
    sx_jv el;
    CHECK(sx_j_idx(v, 2, &el) && sx_j_type(el) == SXJ_OBJ);
    char buf[16];
    CHECK(sx_j_get_str(el, "c", buf, sizeof(buf))); CHECK_STR(buf, "deep");
    CHECK(!sx_j_idx(v, 3, &el)); CHECK(!sx_j_idx(v, -1, &el));
    CHECK(sx_j_get_str(r, "e", buf, sizeof(buf))); CHECK_STR(buf, "s\"q");
    CHECK(sx_j_get_int(r, "key", 0) == 7);                               // an escaped member name matches its plain form
    CHECK(sx_j_get_int(r, "missing", 42) == 42);
    CHECK(sx_j_get_int(r, "n", 5) == 5);                                 // null is not an int
    CHECK(sx_j_get_bool(r, "t", false));
    CHECK(sx_j_get_bool(r, "zz", true));
    CHECK(sx_j_len(r) == 7);
    CHECK(sx_j_len(root("42")) == 0);
    CHECK(!sx_j_get(root("[1]"), "a", NULL));

    // Iteration order and keys.
    sx_jit it = sx_j_iter(root("{\"x\":1,\"y\":\"two\",\"z\":[3]}"));
    sx_jv k, val; int n = 0; char kb[8];
    const char *want[] = { "x", "y", "z" };
    while (sx_j_next(&it, &k, &val)) { CHECK(sx_j_str(k, kb, sizeof(kb))); CHECK_STR(kb, want[n]); n++; }
    CHECK(n == 3);
    it = sx_j_iter(root("[10, 20, 30]"));
    int sum = 0; int64_t x;
    while (sx_j_next(&it, NULL, &val)) { CHECK(sx_j_int(val, &x)); sum += (int)x; }
    CHECK(sum == 60);
    it = sx_j_iter(root("[]")); CHECK(!sx_j_next(&it, NULL, &val));
    it = sx_j_iter(root("{}")); CHECK(!sx_j_next(&it, NULL, &val));
}

static void test_strings(void) {
    char b[64];
    CHECK(sx_j_str(root("\"plain\""), b, sizeof(b))); CHECK_STR(b, "plain");
    CHECK(sx_j_str(root("\"a\\nb\\tc\\\\d\\/e\\\"f\""), b, sizeof(b))); CHECK_STR(b, "a\nb\tc\\d/e\"f");
    CHECK(sx_j_str(root("\"caf\\u00e9\""), b, sizeof(b))); CHECK_STR(b, "caf\xC3\xA9");
    CHECK(sx_j_str(root("\"\\u0027s\""), b, sizeof(b))); CHECK_STR(b, "'s");
    CHECK(sx_j_str(root("\"\\u65e5\\u672c\""), b, sizeof(b))); CHECK_STR(b, "\xE6\x97\xA5\xE6\x9C\xAC");
    CHECK(sx_j_str(root("\"\\ud83d\\ude00\""), b, sizeof(b))); CHECK_STR(b, "\xF0\x9F\x98\x80");   // surrogate pair
    CHECK(sx_j_str(root("\"\\ud83dx\""), b, sizeof(b))); CHECK_STR(b, "\xEF\xBF\xBDx");            // lone high surrogate
    CHECK(sx_j_str(root("\"\\ude00\""), b, sizeof(b))); CHECK_STR(b, "\xEF\xBF\xBD");               // lone low surrogate
    CHECK(sx_j_str(root("\"\\uZZZZ!\""), b, sizeof(b)));                                           // bad escape: no crash
    CHECK(sx_j_str(root("\"\xC3\xA9tai\xC3\xA9\""), b, sizeof(b))); CHECK_STR(b, "\xC3\xA9tai\xC3\xA9");   // raw UTF-8 passes through
    CHECK(sx_j_str(root("\"\""), b, sizeof(b))); CHECK_STR(b, "");
    CHECK(!sx_j_str(root("12"), b, sizeof(b))); CHECK_STR(b, "");
    CHECK(!sx_j_str(root("\"unterminated"), b, sizeof(b)));

    // Truncation never splits a UTF-8 sequence and always terminates.
    char s3[4];
    CHECK(sx_j_str(root("\"\xC3\xA9\xC3\xA9\""), s3, sizeof(s3))); CHECK_STR(s3, "\xC3\xA9");
    CHECK(strlen(s3) == 2);
    char s1[1];
    CHECK(sx_j_str(root("\"abc\""), s1, sizeof(s1))); CHECK_STR(s1, "");
    char s2[2];
    CHECK(sx_j_str(root("\"\\u00e9\""), s2, sizeof(s2))); CHECK_STR(s2, "");     // 2-byte char does not fit in 1+NUL
    CHECK(sx_j_str(root("\"\\ud83d\\ude00\""), s3, sizeof(s3))); CHECK_STR(s3, "");   // 4-byte char does not fit
    CHECK(!sx_j_str(root("\"x\""), NULL, 4));
    CHECK(!sx_j_str(root("\"x\""), b, 0));
}

static void test_numbers(void) {
    int64_t i; double d; bool bo;
    CHECK(sx_j_int(root("12"), &i) && i == 12);
    CHECK(sx_j_int(root("-7"), &i) && i == -7);
    CHECK(sx_j_int(root("\"34\""), &i) && i == 34);                      // a numeric string
    CHECK(sx_j_int(root("12.0"), &i) && i == 12);
    CHECK(sx_j_int(root("1e3"), &i) && i == 1000);
    CHECK(!sx_j_int(root("12.5"), &i));
    CHECK(!sx_j_int(root("\"abc\""), &i));
    CHECK(!sx_j_int(root("null"), &i));
    CHECK(sx_j_int(root("1700000000000"), &i) && i == 1700000000000LL);    // a ms timestamp
    CHECK(sx_j_dbl(root("7.5"), &d) && d == 7.5);
    CHECK(sx_j_dbl(root("\"8.1\""), &d) && d > 8.09 && d < 8.11);          // imdbRating is a string
    CHECK(!sx_j_dbl(root("[]"), &d));
    CHECK(sx_j_bool(root("false"), &bo) && !bo);
    CHECK(sx_j_bool(root("true"), &bo) && bo);
    CHECK(!sx_j_bool(root("1"), &bo));
}

static void test_malformed(void) {
    const char *bad[] = {
        "{", "[", "{\"a\"", "{\"a\":", "{\"a\":1", "[1,", "[1 2]", "{\"a\" 1}", "{a:1}", "[1,]x", "tru", "nul", "\"abc",
        "{\"a\":1,}", "[}", "{]", "{\"a\":[}", "]", "}", ",", "{\"a\":1}}", "[1]]", "12 13",
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(!sx_j_valid(bad[i], strlen(bad[i])));
        // None of the readers may crash on it.
        sx_jv r = root(bad[i]), v; char b[16]; int64_t x;
        (void)sx_j_end(r);
        (void)sx_j_get(r, "a", &v); (void)sx_j_idx(r, 1, &v); (void)sx_j_len(r);
        (void)sx_j_str(r, b, sizeof(b)); (void)sx_j_int(r, &x);
        sx_jit it = sx_j_iter(r);
        while (sx_j_next(&it, NULL, &v)) { }
    }
    CHECK(sx_j_valid("  {\"a\":[1,2,{\"b\":null}]}  \n", 27));
    CHECK(sx_j_valid("[]", 2));
    CHECK(sx_j_valid("0", 1));

    // Nesting is capped, not recursed into without limit.
    char *deep = (char *)malloc(5000);
    memset(deep, '[', 2000); memset(deep + 2000, ']', 2000);
    CHECK(!sx_j_valid(deep, 4000));
    CHECK(sx_j_end(sx_j_root(deep, 4000)) == NULL);
    free(deep);
    char shallow[200];
    int n = 0;
    for (int i = 0; i < 40; i++) shallow[n++] = '[';
    for (int i = 0; i < 40; i++) shallow[n++] = ']';
    CHECK(sx_j_valid(shallow, (size_t)n));
}

static void test_escape(void) {
    char out[64];
    CHECK(sx_json_escape("plain", out, sizeof(out))); CHECK_STR(out, "plain");
    CHECK(sx_json_escape("a\"b\\c\nd\te\x01", out, sizeof(out))); CHECK_STR(out, "a\\\"b\\\\c\\nd\\te\\u0001");
    CHECK(sx_json_escape("caf\xC3\xA9", out, sizeof(out))); CHECK_STR(out, "caf\xC3\xA9");
    char tiny[4];
    CHECK(!sx_json_escape("abcdef", tiny, sizeof(tiny)));
    CHECK(!sx_json_escape("\"\"\"", tiny, sizeof(tiny)));
    // What it writes decodes back to the input.
    char in[40] = "x\"y\\z\n\xE6\x97\xA5";
    char esc[128], q[140], back[64];
    CHECK(sx_json_escape(in, esc, sizeof(esc)));
    snprintf(q, sizeof(q), "\"%s\"", esc);
    CHECK(sx_j_str(root(q), back, sizeof(back))); CHECK_STR(back, in);
}

static void walk(sx_jv v, int depth) {
    if (depth > 60) return;
    char b[64]; int64_t x; double d; bool bo;
    switch (sx_j_type(v)) {
    case SXJ_OBJ: case SXJ_ARR: {
        sx_jit it = sx_j_iter(v); sx_jv k, e;
        while (sx_j_next(&it, &k, &e)) { if (k.p) (void)sx_j_str(k, b, sizeof(b)); walk(e, depth + 1); }
        break;
    }
    case SXJ_STR: (void)sx_j_str(v, b, sizeof(b)); (void)sx_j_int(v, &x); (void)sx_j_dbl(v, &d); break;
    case SXJ_NUM: (void)sx_j_int(v, &x); (void)sx_j_dbl(v, &d); break;
    case SXJ_BOOL: (void)sx_j_bool(v, &bo); break;
    default: break;
    }
}

static char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)sz + 1);
    if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); return NULL; }
    fclose(f);
    b[sz] = 0;
    *n = (size_t)sz;
    return b;
}

static void test_fixtures(void) {
    const char *files[] = {
        "fixtures/stremio/cinemeta_manifest.json", "fixtures/stremio/cinemeta_catalog_movie_top.json",
        "fixtures/stremio/cinemeta_meta_movie.json", "fixtures/stremio/cinemeta_meta_series_simpsons.json",
        "fixtures/stremio/account_addon_collection.json", "fixtures/stremio/account_library.json",
        "fixtures/stremio/aio_streams_tt0133093.json",
    };
    for (unsigned f = 0; f < sizeof(files) / sizeof(files[0]); f++) {
        size_t n = 0;
        char *b = slurp(files[f], &n);
        if (!b) { printf("SKIP %s (not found)\n", files[f]); continue; }
        CHECK(sx_j_valid(b, n));
        walk(sx_j_root(b, n), 0);

        // Truncations: every prefix must be survivable (sampled for the big ones).
        const size_t step = n > 20000 ? n / 400 + 1 : 1;
        for (size_t cut = 0; cut < n; cut += step) walk(sx_j_root(b, cut), 0);

        // Mutations: flip bytes and walk the result.
        char *m = (char *)malloc(n + 1);
        for (int it = 0; it < 150; it++) {
            memcpy(m, b, n);
            const int flips = 1 + (int)(rnd() % 8);
            for (int k = 0; k < flips; k++) m[rnd() % n] = (char)rnd();
            walk(sx_j_root(m, n), 0);
            (void)sx_j_valid(m, n);
        }
        free(m);
        free(b);
    }
}

static void test_real_shapes(void) {
    size_t n = 0;
    char *b = slurp("fixtures/stremio/cinemeta_catalog_movie_top.json", &n);
    if (!b) { printf("SKIP real shapes\n"); return; }
    sx_jv r = sx_j_root(b, n), metas, m0;
    CHECK(sx_j_get(r, "metas", &metas) && sx_j_type(metas) == SXJ_ARR);
    CHECK(sx_j_len(metas) == 49);      // counted independently with python json
    CHECK(sx_j_idx(metas, 0, &m0));
    char id[32], name[64];
    CHECK(sx_j_get_str(m0, "imdb_id", id, sizeof(id)) && strncmp(id, "tt", 2) == 0);
    CHECK(sx_j_get_str(m0, "name", name, sizeof(name)) && name[0]);
    free(b);
}

int main(void) {
    test_types();
    test_lookup();
    test_strings();
    test_numbers();
    test_malformed();
    test_escape();
    test_real_shapes();
    test_fixtures();
    printf("test_sx_json: %d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
