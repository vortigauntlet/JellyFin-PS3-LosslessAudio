// Host test for the stream request decision (source/player/stream/
// stream_request.cpp): the one place a stream.ts URL is built, for playback
// and for downloads.
//
// The goldens are byte-exact URLs.  Parameter order is part of the contract:
// the server treats the query as a set, but a reordering is the first sign
// that a decision changed.  Each case's inputs are in stream_golden_cases.h,
// its expected URL in stream_golden_urls.inc.
//
//   make -f Makefile.host test_stream_request && ./test_stream_request

#include "stream_request.h"
#include "stream_golden_cases.h"

#include <stdio.h>
#include <string.h>

static int s_checks = 0, s_failed = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static const char *EXPECT[] = {
#include "stream_golden_urls.inc"
};

static const char *SERVER = "http://server:8096";
static const char *DEVICE = "DEVID123";

static void fill_tracks(JFTracks *t) {
    memset(t, 0, sizeof *t);
    struct { int idx; const char *label; unsigned br; } a[] = {
        {1, "English - Dolby Digital - 5.1 - Default", 640000},
        {2, "English - Dolby TrueHD - 7.1 - Atmos", 0},
        {3, "English - DTS-HD MA - 5.1", 1509000},
        {4, "English - Dolby Digital Plus - 5.1", 768000},
        {5, "English - AAC - Stereo", 192000},
    };
    for (int i = 0; i < 5; i++) {
        t->audio[i].index = a[i].idx;
        snprintf(t->audio[i].label, sizeof t->audio[i].label, "%s", a[i].label);
        t->audio[i].bitrate = a[i].br;
    }
    t->n_audio = 5;
    t->default_audio = 0;
    struct { int idx; const char *label; const char *codec; } s[] = {
        {6, "English - SRT", "subrip"},
        {7, "English - PGS", "pgssub"},
        {8, "English - VobSub", "dvdsub"},
    };
    for (int i = 0; i < 3; i++) {
        t->subs[i].index = s[i].idx;
        snprintf(t->subs[i].label, sizeof t->subs[i].label, "%s", s[i].label);
        snprintf(t->subs[i].codec, sizeof t->subs[i].codec, "%s", s[i].codec);
    }
    t->n_subs = 3;
}

struct Built { StreamPrefs p; StreamSelection sel; JFMediaSource src; JFTracks tracks; };

static void build_case(const GoldenCase *c, Built *b) {
    memset(b, 0, sizeof *b);
    b->p.quality     = (vquality_t)c->quality;
    b->p.hd1080      = c->hd != 0;
    b->p.surround    = c->surround != 0;
    b->p.surround_hd = c->surround_hd != 0;
    b->p.display_w   = c->disp_w;
    b->p.display_h   = c->disp_h;
    b->p.budget      = true;
    fill_tracks(&b->tracks);
    b->sel.item_id   = "ITEM0001";
    b->sel.tracks    = &b->tracks;
    b->sel.cur_audio = c->audio;
    b->sel.cur_sub   = c->sub;
    b->sel.sub_on_console = (c->sub == GS_TEXT || c->sub == GS_PGS);
    if (c->version != GV_NOSRC) {
        fill_tracks(&b->src.tracks);
        snprintf(b->src.id, sizeof b->src.id, "%s",
                 c->version == GV_ALT ? "VERSION0002" : "ITEM0001");
        if (c->version == GV_LIVE)
            snprintf(b->src.live_stream_id, sizeof b->src.live_stream_id, "live stream/1");
        b->sel.source = &b->src;
    }
}

static void golden(void) {
    printf("- golden URLs\n");
    CHECK(GOLDEN_N == (int)(sizeof EXPECT / sizeof EXPECT[0]));
    for (int i = 0; i < GOLDEN_N && i < (int)(sizeof EXPECT / sizeof EXPECT[0]); i++) {
        Built b; build_case(&GOLDEN[i], &b);
        StreamRequest rq;
        stream_request_resolve(&b.p, &b.sel, &rq);
        char url[1024];
        stream_url_build(url, sizeof url, &rq, SERVER, DEVICE,
                         GOLDEN[i].session ? "sess1234" : "", GOLDEN[i].start);
        if (strcmp(url, EXPECT[i]) != 0)
            printf("  %s:\n    got  %s\n    want %s\n", GOLDEN[i].name, url, EXPECT[i]);
        CHECK(strcmp(url, EXPECT[i]) == 0);
    }
}

// What the budget does, stated as properties rather than as URLs.
static void budget(void) {
    printf("- budget\n");
    for (int i = 0; i < GOLDEN_N; i++) {
        Built b; build_case(&GOLDEN[i], &b);
        StreamRequest rq;
        stream_request_resolve(&b.p, &b.sel, &rq);
        // Budgeted exactly when there is a step to take the audio out of.
        CHECK(rq.budgeted == (rq.vbitrate != 0));
        if (rq.budgeted) CHECK(rq.vreq <= rq.vbitrate && rq.vreq >= rq.vbitrate / 2);
        else             CHECK(rq.vreq == 0);

        // With the budget switched off the request carries the step as it is.
        b.p.budget = false;
        StreamRequest raw;
        stream_request_resolve(&b.p, &b.sel, &raw);
        CHECK(!raw.budgeted && raw.vreq == raw.vbitrate);
    }
}

static void subtitles(void) {
    printf("- subtitles\n");
    Built b; build_case(&GOLDEN[0], &b);
    StreamRequest rq;
    b.sel.cur_sub = 2; b.sel.sub_on_console = false;      // a bitmap track: burned in
    stream_request_resolve(&b.p, &b.sel, &rq);
    CHECK(rq.sub_idx == 8);
    b.sel.cur_sub = 0; b.sel.sub_on_console = true;       // drawn here: not requested
    stream_request_resolve(&b.p, &b.sel, &rq);
    CHECK(rq.sub_idx == -1);
    b.sel.cur_sub = 9; b.sel.sub_on_console = false;      // out of range: none
    stream_request_resolve(&b.p, &b.sel, &rq);
    CHECK(rq.sub_idx == -1);
}

static void initial_selection(void) {
    printf("- initial selection\n");
    JFTracks t; fill_tracks(&t);
    t.default_audio = 2;
    int a = -9, s = -9;
    stream_select_initial(&t, true, &a, &s);
    CHECK(a == 2 && s == -1);
    stream_select_initial(&t, false, &a, &s);
    CHECK(a == -1 && s == -1);
    JFTracks none; memset(&none, 0, sizeof none);
    stream_select_initial(&none, true, &a, &s);
    CHECK(a == -1 && s == -1);
}

int main(void) {
    golden();
    budget();
    subtitles();
    initial_selection();
    printf("%d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
