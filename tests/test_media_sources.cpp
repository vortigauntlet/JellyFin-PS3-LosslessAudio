#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jellyfin_api.h"

static const char kPlaybackInfo[] =
    "{\"MediaSources\":["
      "{\"Protocol\":\"File\",\"Id\":\"source-a\","
       "\"Name\":\"Same cut\",\"RunTimeTicks\":600000000,"
       "\"MediaStreams\":["
         "{\"DisplayTitle\":\"720p H264 SDR\",\"Type\":\"Video\",\"Index\":0},"
         "{\"DisplayTitle\":\"English - DTS - 5.1\",\"IsDefault\":true,\"BitRate\":1509000,"
          "\"Type\":\"Audio\",\"Index\":1},"
         "{\"DisplayTitle\":\"English - SRT\",\"Type\":\"Subtitle\",\"Index\":2}"
       "]},"
      "{\"Id\":\"source-b\",\"LiveStreamId\":\"live-b\","
       "\"Name\":\"Same cut\",\"RunTimeTicks\":720000000,"
       "\"MediaStreams\":["
         "{\"Name\":\"nested-name-must-not-win\","
          "\"DisplayTitle\":\"1080p H264 SDR\",\"Type\":\"Video\",\"Index\":0},"
         "{\"Language\":\"Japanese\",\"Type\":\"Audio\",\"Index\":4}"
       "]},"
      "{\"Id\":\"source-c\",\"Name\":\"\\u26a1 4K Web\","
       "\"MediaStreams\":[]}"
    "],\"PlaySessionId\":\"session\"}";

int main(int argc, char **argv) {
    JFMediaSources all;
    assert(jellyfin_parse_media_sources(kPlaybackInfo, &all) == 3);
    assert(all.n_sources == 3);
    assert(strcmp(all.source[0].id, "source-a") == 0);
    // Version labels carry resolution and codec (source_tag_video), which is
    // the whole point of the row: two cuts of the same title have to be
    // tellable apart before you pick one.  The bare "(1)"/"(2)" numbering is
    // only the fallback for when even those tags collide.
    assert(strstr(all.source[0].label, "Same cut") != NULL);
    assert(strstr(all.source[0].label, "720p")     != NULL);
    assert(strstr(all.source[0].label, "H.264")    != NULL);
    assert(strstr(all.source[1].label, "Same cut") != NULL);
    assert(strstr(all.source[1].label, "1080p")    != NULL);
    assert(strcmp(all.source[0].label, all.source[1].label) != 0);
    assert(all.source[0].runtime_secs == 60);
    assert(all.source[1].runtime_secs == 72);
    assert(all.source[0].tracks.n_audio == 1);
    assert(all.source[0].tracks.default_audio == 0);
    assert(all.source[0].tracks.audio[0].index == 1);
    // BitRate feeds the stream budget (player/core/stream_budget.h); a
    // track that does not report one reads as 0, never garbage.
    assert(all.source[0].tracks.audio[0].bitrate == 1509000u);
    assert(all.source[1].tracks.audio[0].bitrate == 0u);
    assert(all.source[0].tracks.n_subs == 1);
    assert(all.source[0].tracks.subs[0].index == 2);
    assert(strcmp(all.source[1].live_stream_id, "live-b") == 0);
    assert(strcmp(all.source[1].tracks.audio[0].label, "Japanese") == 0);
    assert(strstr(all.source[2].label, "4K Web") != NULL);

    JFMediaSource selected;
    assert(jellyfin_parse_selected_media_source(kPlaybackInfo, "source-b",
                                                 &selected));
    assert(strcmp(selected.id, "source-b") == 0);
    assert(strcmp(selected.live_stream_id, "live-b") == 0);
    assert(selected.tracks.audio[0].index == 4);

    assert(jf_sub_is_pgs("pgssub"));
    assert(jf_sub_is_pgs("PgsSub"));
    assert(jf_sub_is_pgs("hdmv_pgs_subtitle"));
    assert(!jf_sub_is_pgs("subrip"));
    assert(!jf_sub_is_pgs("dvdsub"));
    assert(!jf_sub_is_pgs(NULL));
    assert(!jf_sub_is_pgs(""));

    // ---- start track: the console's language (2026-10-17, Avatar) ----------
    // Russian remux: nine Russian tracks, the first flagged default, then the
    // English ones at 10 and 11 -- past the old 8-track cap.
    {
        static char js[8192];
        int n = snprintf(js, sizeof js,
            "{\"MediaSources\":[{\"Id\":\"ru\",\"Name\":\"Avatar\",\"MediaStreams\":["
            "{\"Type\":\"Video\",\"Index\":0,\"DisplayTitle\":\"1080p H264\"}");
        for (int i = 1; i <= 9; i++)
            n += snprintf(js + n, sizeof js - n,
                ",{\"Type\":\"Audio\",\"Index\":%d,\"Language\":\"rus\",%s"
                "\"DisplayTitle\":\"Russian dub %d\"}", i, i == 1 ? "\"IsDefault\":true," : "", i);
        n += snprintf(js + n, sizeof js - n,
            ",{\"Type\":\"Audio\",\"Index\":10,\"Language\":\"eng\",\"DisplayTitle\":\"English - TrueHD 7.1\"}"
            ",{\"Type\":\"Audio\",\"Index\":11,\"Language\":\"eng\",\"DisplayTitle\":\"English - DTS-HD MA 5.1\"}"
            "]}]}");
        JFMediaSources m;
        snprintf(g_audio_lang, sizeof g_audio_lang, "eng");
        assert(jellyfin_parse_media_sources(js, &m) == 1);
        const JFTracks *t = &m.source[0].tracks;
        assert(t->n_audio == 11);
        assert(t->audio[t->default_audio].index == 10);
        assert(strcmp(t->audio[9].lang, "eng") == 0);
        // A Russian console keeps the file's own default.
        jf_audio_lang_from_system(8);
        assert(strcmp(g_audio_lang, "rus") == 0);
        assert(jellyfin_parse_media_sources(js, &m) == 1);
        assert(m.source[0].tracks.audio[m.source[0].tracks.default_audio].index == 1);
        // Japanese: no Japanese track, so the file's default.
        jf_audio_lang_from_system(0);
        assert(jellyfin_parse_media_sources(js, &m) == 1);
        assert(m.source[0].tracks.audio[m.source[0].tracks.default_audio].index == 1);
        jf_audio_lang_from_system(1);
        assert(strcmp(g_audio_lang, "eng") == 0);
    }
    // Twenty Russian tracks then one English: the list is full, so the English
    // one takes the last slot rather than being dropped.
    {
        static char js[16384];
        int n = snprintf(js, sizeof js, "{\"MediaSources\":[{\"Id\":\"x\",\"MediaStreams\":[");
        for (int i = 1; i <= 20; i++)
            n += snprintf(js + n, sizeof js - n, "%s{\"Type\":\"Audio\",\"Index\":%d,\"Language\":\"rus\"%s}",
                          i > 1 ? "," : "", i, i == 16 ? ",\"IsDefault\":true" : "");
        n += snprintf(js + n, sizeof js - n,
            ",{\"Type\":\"Audio\",\"Index\":21,\"Language\":\"eng\"}"
            ",{\"Type\":\"Audio\",\"Index\":22,\"Language\":\"eng\"}]}]}");
        JFMediaSources m;
        assert(jellyfin_parse_media_sources(js, &m) == 1);
        const JFTracks *t = &m.source[0].tracks;
        assert(t->n_audio == JF_MAX_AUDIO);
        assert(t->audio[JF_MAX_AUDIO - 1].index == 21);   // first English kept, second dropped
        assert(t->audio[t->default_audio].index == 21);
    }
    // Commentary goes last; tags in other spellings; untagged labels.
    {
        static const char js[] =
            "{\"MediaSources\":[{\"Id\":\"c\",\"MediaStreams\":["
            "{\"Type\":\"Audio\",\"Index\":1,\"Language\":\"ger\",\"IsDefault\":true,\"DisplayTitle\":\"Deutsch\"},"
            "{\"Type\":\"Audio\",\"Index\":2,\"Language\":\"eng\",\"DisplayTitle\":\"English - Director's Commentary\"},"
            "{\"Type\":\"Audio\",\"Index\":3,\"Language\":\"en\",\"DisplayTitle\":\"English - AC3 5.1\"}"
            "]}]}";
        JFMediaSources m;
        assert(jellyfin_parse_media_sources(js, &m) == 1);
        assert(m.source[0].tracks.audio[m.source[0].tracks.default_audio].index == 3);
        // German console: the default is German already.
        jf_audio_lang_from_system(4);
        assert(jellyfin_parse_media_sources(js, &m) == 1);
        assert(m.source[0].tracks.audio[m.source[0].tracks.default_audio].index == 1);
        jf_audio_lang_from_system(1);
    }
    assert(jf_lang_matches("fra", NULL, "fre"));
    assert(jf_lang_matches("Japanese", NULL, "jpn"));
    assert(jf_lang_matches("", "English - DTS - 5.1", "eng"));
    assert(!jf_lang_matches("", "Englishman", "eng"));
    assert(!jf_lang_matches("und", "English", "eng"));      // a tag wins over the label
    assert(!jf_lang_matches("eng", "English", "xx"));
    jf_audio_lang_from_system(18);   // English (UK)
    assert(strcmp(g_audio_lang, "eng") == 0);
    jf_audio_lang_from_system(99);
    assert(strcmp(g_audio_lang, "eng") == 0);
    puts("media source parser: start track follows the console language ok");

    puts("media source parser: synthetic ok");

    // Optional real item-DTO/PlaybackInfo fixture (used by the WSL check).
    if (argc == 2) {
        FILE *f = fopen(argv[1], "rb");
        assert(f != NULL);
        assert(fseek(f, 0, SEEK_END) == 0);
        long size = ftell(f);
        assert(size > 0);
        rewind(f);
        char *json = (char *)malloc((size_t)size + 1);
        assert(json != NULL);
        assert(fread(json, 1, (size_t)size, f) == (size_t)size);
        fclose(f);
        json[size] = '\0';
        JFMediaSources real;
        int count = jellyfin_parse_media_sources(json, &real);
        assert(count >= 2);
        assert(real.source[0].id[0] && real.source[1].id[0]);
        assert(real.source[0].tracks.n_audio > 0);
        assert(real.source[1].tracks.n_audio > 0);
        printf("media source parser: real MediaSources JSON ok (%d sources)\n",
               count);
        free(json);
    }
    return 0;
}
