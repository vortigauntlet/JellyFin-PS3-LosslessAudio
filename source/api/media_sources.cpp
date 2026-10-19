// PlaybackInfo.MediaSources parser.  This file deliberately has no PS3-only
// dependencies so the exact parser shipped in the PKG can be host-tested.

#include "version_summary.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>    // strncasecmp

#include "jellyfin_api.h"

#include "json_lite.h"

// Defined HERE, in the file that sets it, rather than beside the other API
// globals: this parser is deliberately free of PS3-only dependencies so it
// can be host-tested standalone (tests/test_media_sources.cpp), and putting
// the definition in api_auth.cpp broke that link.
int g_source_fps_milli = 0;

// Audio language (see jellyfin_api.h).  Here for the same reason: host-tested.
char g_audio_lang[4] = "eng";

// One language's codes: ISO 639-2/B, 639-2/T, 639-1, and its English name
// (what a DisplayTitle starts with when the stream has no Language tag).
typedef struct { const char *codes[4]; const char *name; } LangCodes;
static const LangCodes kLangs[] = {
    {{"eng", "en"},         "English"},
    {{"jpn", "ja"},         "Japanese"},
    {{"fre", "fra", "fr"},  "French"},
    {{"spa", "es"},         "Spanish"},
    {{"ger", "deu", "de"},  "German"},
    {{"ita", "it"},         "Italian"},
    {{"dut", "nld", "nl"},  "Dutch"},
    {{"por", "pt"},         "Portuguese"},
    {{"rus", "ru"},         "Russian"},
    {{"kor", "ko"},         "Korean"},
    {{"chi", "zho", "zh"},  "Chinese"},
    {{"fin", "fi"},         "Finnish"},
    {{"swe", "sv"},         "Swedish"},
    {{"dan", "da"},         "Danish"},
    {{"nor", "nob", "no"},  "Norwegian"},
    {{"pol", "pl"},         "Polish"},
    {{"tur", "tr"},         "Turkish"},
};
#define N_LANGS (int)(sizeof(kLangs) / sizeof(kLangs[0]))

static bool ci_eq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return false;
    }
    return *a == *b;
}

static const LangCodes *lang_find(const char *code) {
    if (!code || !code[0]) return NULL;
    for (int i = 0; i < N_LANGS; i++) {
        for (int k = 0; k < 4 && kLangs[i].codes[k]; k++)
            if (ci_eq(code, kLangs[i].codes[k])) return &kLangs[i];
        if (ci_eq(code, kLangs[i].name)) return &kLangs[i];   // "Language":"Japanese"
    }
    return NULL;
}

void jf_audio_lang_from_system(int sysutil_lang) {
    // CELL_SYSUTIL_LANG_*: both English, both Portuguese and both Chinese
    // variants share one audio language.
    static const char *kSys[] = {
        "jpn", "eng", "fre", "spa", "ger", "ita", "dut", "por", "rus", "kor",
        "chi", "chi", "fin", "swe", "dan", "nor", "pol", "por", "eng", "tur",
    };
    const char *c = (sysutil_lang >= 0 && sysutil_lang < (int)(sizeof(kSys) / sizeof(kSys[0])))
                        ? kSys[sysutil_lang] : "eng";
    snprintf(g_audio_lang, sizeof g_audio_lang, "%s", c);
}

bool jf_lang_matches(const char *track_lang, const char *label, const char *pref) {
    const LangCodes *want = lang_find(pref);
    if (!want) return false;
    if (track_lang && track_lang[0]) return lang_find(track_lang) == want;
    if (!label || !label[0]) return false;
    // Untagged: the label's first word.
    const size_t n = strlen(want->name);
    return strncasecmp(label, want->name, n) == 0 &&
           (label[n] == '\0' || label[n] == ' ' || label[n] == '-' || label[n] == '/');
}

static bool is_commentary(const char *label) {
    for (const char *p = label; *p; p++)
        if (strncasecmp(p, "comment", 7) == 0) return true;
    return false;
}

int jf_audio_slot(const JFTracks *t, const char *lang, const char *label) {
    if (t->n_audio < JF_MAX_AUDIO) return t->n_audio;
    if (!jf_lang_matches(lang, label, g_audio_lang)) return -1;
    for (int i = 0; i < t->n_audio; i++)
        if (jf_lang_matches(t->audio[i].lang, t->audio[i].label, g_audio_lang)) return -1;
    return JF_MAX_AUDIO - 1;
}

void jf_tracks_pick_start(JFTracks *t, int file_default) {
    if (file_default < 0 || file_default >= t->n_audio) file_default = 0;
    t->default_audio = file_default;
    if (t->n_audio <= 0) { t->default_audio = 0; return; }
    const JFStream *d = &t->audio[file_default];
    if (jf_lang_matches(d->lang, d->label, g_audio_lang)) return;
    int first = -1;
    for (int i = 0; i < t->n_audio; i++) {
        if (!jf_lang_matches(t->audio[i].lang, t->audio[i].label, g_audio_lang)) continue;
        if (!is_commentary(t->audio[i].label)) { t->default_audio = i; return; }
        if (first < 0) first = i;
    }
    if (first >= 0) t->default_audio = first;
}

static void parse_tracks(const char *source, const char *source_end,
                         JFTracks *out, char *first_video, int video_cap) {
    memset(out, 0, sizeof(*out));
    if (first_video && video_cap > 0) first_video[0] = '\0';
    g_source_fps_milli = 0;   // re-learned from this source's Video stream
    const char *arr = find_top_value(source, source_end, "MediaStreams");
    if (!arr || arr >= source_end || *arr != '[') return;
    const char *p = arr + 1;
    int file_default = 0, slot = -1;
    while (p < source_end) {
        p = skip_ws(p, source_end);
        if (p >= source_end || *p == ']') break;
        if (*p != '{') { p++; continue; }
        const char *oe = object_end(p, source_end);
        if (!oe) break;

        char type[16] = "", display[128] = "", language[32] = "";
        const char *v = find_top_value(p, oe, "Type");
        if (v) read_json_string(v, oe, type, sizeof(type));
        v = find_top_value(p, oe, "DisplayTitle");
        if (v) read_json_string(v, oe, display, sizeof(display));
        v = find_top_value(p, oe, "Language");
        if (v) read_json_string(v, oe, language, sizeof(language));
        v = find_top_value(p, oe, "Index");
        int index = v ? (int)read_json_int(v, oe, -1) : -1;

        if (strcmp(type, "Video") == 0) {
            if (first_video && !first_video[0] && display[0])
                snprintf(first_video, video_cap, "%s", display);
            // Frame rate, for the player timing when VDEC reports no
            // frame-rate code -- which is exactly what a stream copy does.
            // Kept as milli-fps so it stays integer: 23.976025 -> 23976.
            if (!g_source_fps_milli) {
                const char *fr = find_top_value(p, oe, "RealFrameRate");
                if (!fr) fr = find_top_value(p, oe, "AverageFrameRate");
                if (fr) {
                    char numbuf[32]; int k = 0;
                    const char *q = fr;
                    while (q < oe && k < (int)sizeof(numbuf) - 1 &&
                           ((*q >= '0' && *q <= '9') || *q == '.'))
                        numbuf[k++] = *q++;
                    numbuf[k] = '\0';
                    if (numbuf[0]) {
                        double f = atof(numbuf);
                        if (f > 1.0 && f < 1000.0)
                            g_source_fps_milli = (int)(f * 1000.0 + 0.5);
                    }
                }
            }
        } else if (index >= 0 && strcmp(type, "Audio") == 0 &&
                   (slot = jf_audio_slot(out, language, display)) >= 0) {
            const int pos = slot;
            if (pos == out->n_audio) out->n_audio++;
            else if (file_default == pos) file_default = 0;   // its track was replaced
            memset(&out->audio[pos], 0, sizeof out->audio[pos]);
            out->audio[pos].index = index;
            snprintf(out->audio[pos].label, sizeof(out->audio[pos].label), "%s",
                     display[0] ? display : (language[0] ? language : "Audio"));
            snprintf(out->audio[pos].lang, sizeof(out->audio[pos].lang), "%s", language);
            v = find_top_value(p, oe, "IsDefault");
            if (v && read_json_bool(v, oe)) file_default = pos;
            v = find_top_value(p, oe, "BitRate");
            if (v) {
                const long long br = read_json_int(v, oe, 0);
                out->audio[pos].bitrate = br > 0 && br < 100000000 ? (unsigned)br : 0;
            }
        } else if (index >= 0 && strcmp(type, "Subtitle") == 0 &&
                   out->n_subs < JF_MAX_STREAMS) {
            int pos = out->n_subs++;
            out->subs[pos].index = index;
            snprintf(out->subs[pos].label, sizeof(out->subs[pos].label), "%s",
                     display[0] ? display : (language[0] ? language : "Subtitle"));
            out->subs[pos].codec[0] = '\0';
            v = find_top_value(p, oe, "Codec");
            if (v) read_json_string(v, oe, out->subs[pos].codec,
                                    sizeof(out->subs[pos].codec));
        }
        p = oe;
    }
    jf_tracks_pick_start(out, file_default);
}

// Case-insensitive substring search (the labels come from whatever named the
// file, so "1080P" and "1080p" both turn up).
static bool contains_ci(const char *hay, const char *needle) {
    if (!hay || !needle || !needle[0]) return false;
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b) {
            char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
            char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b - 'A' + 'a') : *b;
            if (ca != cb) break;
            a++; b++;
        }
        if (!*b) return true;
    }
    return false;
}

// Append `tag` to a label unless it is already in there.
static void label_append(char *label, int cap, const char *tag) {
    if (contains_ci(label, tag)) return;
    int len = (int)strlen(label);
    if (len + 4 + (int)strlen(tag) < cap)
        snprintf(label + len, (size_t)(cap - len), " \xB7 %s", tag);
}

// Make the resolution and video codec visible in a version's label.
//
// A source plugin names its entries after the release ("Show.S01E01.WEB-DL
// .DDP5.1.H.264-GROUP") and most such names carry the resolution, but not all
// do — and with a long list the choice that matters is which resolution and
// which codec, because both decide how much work the SERVER has to do.  The
// PS3 itself is unaffected: video is always transcoded to H.264 for it
// (AllowVideoStreamCopy=false), so a 4K HEVC source plays — it just makes the
// server re-encode 4K HEVC in real time, which is where "stream connection
// failed" after a long wait comes from.  Picking a 1080p H.264 source is
// dramatically cheaper, so it is worth being able to see which is which.
//
// Never invents: anything the video stream does not report is not added.
static void source_tag_video(char *label, int cap, const char *video) {
    if (!label || !video || !video[0]) return;

    static const char *kRes[] = { "2160p", "4K", "1440p", "1080p", "720p",
                                  "576p", "480p", "360p" };
    for (unsigned i = 0; i < sizeof(kRes) / sizeof(kRes[0]); i++) {
        if (!contains_ci(video, kRes[i])) continue;
        // "4K" and "2160p" mean the same thing to a reader; do not add one
        // when the other is already present.
        if (i <= 1 && (contains_ci(label, "2160p") || contains_ci(label, "4K")))
            break;
        label_append(label, cap, kRes[i]);
        break;
    }

    // Codec.  The PS3's own decoder handles H.264/AVC (the two names are the
    // same codec) and nothing newer — no HEVC, no AV1.  That is not what
    // decides whether a source plays here, because the server always
    // transcodes video to H.264 for this client, but it decides how HARD the
    // server has to work: re-encoding 4K HEVC or AV1 in real time is what
    // produces stalls and the two-minute "connection failed".  So name the
    // codec when the release name does not.
    //
    // H.264 sources are named "x264", "H.264" or "AVC" interchangeably; any
    // of those counts as already said.
    if (contains_ci(video, "AV1")) {
        if (!contains_ci(label, "AV1")) label_append(label, cap, "AV1");
    } else if (contains_ci(video, "HEVC") || contains_ci(video, "H265") ||
               contains_ci(video, "H.265") || contains_ci(video, "X265")) {
        if (!contains_ci(label, "265") && !contains_ci(label, "HEVC"))
            label_append(label, cap, "HEVC");
    } else if (contains_ci(video, "H264") || contains_ci(video, "H.264") ||
               contains_ci(video, "AVC") || contains_ci(video, "X264")) {
        if (!contains_ci(label, "264") && !contains_ci(label, "AVC"))
            label_append(label, cap, "H.264");
    }
}

static bool parse_source(const char *obj, const char *end, JFMediaSource *out) {
    memset(out, 0, sizeof(*out));
    const char *v = find_top_value(obj, end, "Id");
    if (v) read_json_string(v, end, out->id, sizeof(out->id));
    v = find_top_value(obj, end, "LiveStreamId");
    if (v) read_json_string(v, end, out->live_stream_id,
                            sizeof(out->live_stream_id));
    v = find_top_value(obj, end, "Name");
    if (v) read_json_string(v, end, out->label, sizeof(out->label));
    v = find_top_value(obj, end, "RunTimeTicks");
    long long ticks = v ? read_json_int(v, end, 0) : 0;
    if (ticks > 0) out->runtime_secs = (unsigned)(ticks / 10000000LL);
    // The file's size in bytes, as the server reports it (0 = not reported).
    v = find_top_value(obj, end, "Size");
    long long size = v ? read_json_int(v, end, 0) : 0;
    if (size > 0) out->size_bytes = (unsigned long long)size;
    v = find_top_value(obj, end, "Path");
    if (v) {
        char path[48] = "";
        read_json_string(v, end, path, sizeof(path));
        out->placeholder = strncmp(path, "gelato://stub", 13) == 0 ||
                           strcmp(path, "/stub") == 0;
    }

    char video[64] = "";
    parse_tracks(obj, end, &out->tracks, video, sizeof(video));
    if (!out->label[0])
        snprintf(out->label, sizeof(out->label), "%s",
                 video[0] ? video : "Version");
    else
        source_tag_video(out->label, sizeof(out->label), video);
    // From the RAW name, emoji and all: the globe marks the language.
    version_summary(out->label, out->summary, sizeof(out->summary));
    return out->id[0] != '\0';
}

static const char *media_sources_array(const char *json, const char **end) {
    if (!json) return NULL;
    const char *p = strstr(json, "\"MediaSources\"");
    if (!p) return NULL;
    p += sizeof("\"MediaSources\"") - 1;
    while (*p && *p != ':') p++;
    if (*p != ':') return NULL;
    p = skip_ws(p + 1, p + strlen(p));
    if (*p != '[') return NULL;
    *end = json + strlen(json);
    return p + 1;
}

int jellyfin_parse_media_sources(const char *json, JFMediaSources *out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    const char *limit = NULL;
    const char *p = media_sources_array(json, &limit);
    if (!p) return 0;
    while (p < limit && out->n_sources < JF_MAX_SOURCES) {
        p = skip_ws(p, limit);
        if (p >= limit || *p == ']') break;
        if (*p != '{') { p++; continue; }
        const char *oe = object_end(p, limit);
        if (!oe) break;
        if (parse_source(p, oe, &out->source[out->n_sources]) &&
            out->source[out->n_sources].placeholder) {
            // Not something to choose: see JFMediaSource.placeholder.
            out->n_placeholder++;
        } else if (out->source[out->n_sources].id[0]) {
            // Gelato re-lists the same stream once per sync, so an episode
            // that has been played several times carries the same version
            // dozens of times over (345 entries, 44 distinct, on one Better
            // Call Saul episode).  Keep the first of each: same name, same
            // size, same runtime is the same version as far as anyone
            // choosing between them can tell.
            const JFMediaSource *c = &out->source[out->n_sources];
            bool dup = false;
            for (int i = 0; i < out->n_sources && !dup; i++) {
                const JFMediaSource *o = &out->source[i];
                dup = o->size_bytes == c->size_bytes &&
                      o->runtime_secs == c->runtime_secs &&
                      strcmp(o->label, c->label) == 0;
            }
            if (!dup) out->n_sources++;
        }
        p = oe;
    }

    // Duplicate names are common for local versions.  Keep the server's name
    // where it is useful, but make identical entries distinguishable.
    bool duplicate[JF_MAX_SOURCES] = { false };
    int ordinal[JF_MAX_SOURCES] = { 0 };
    for (int i = 0; i < out->n_sources; i++) {
        ordinal[i] = 1;
        for (int j = 0; j < out->n_sources; j++) {
            if (strcmp(out->source[i].label, out->source[j].label) == 0) {
                if (j != i) duplicate[i] = true;
                if (j < i) ordinal[i]++;
            }
        }
    }
    for (int i = 0; i < out->n_sources; i++) {
        if (duplicate[i]) {
            char base[112];
            snprintf(base, sizeof(base), "%s", out->source[i].label);
            snprintf(out->source[i].label, sizeof(out->source[i].label),
                     "%s (%d)", base, ordinal[i]);
        }
    }
    return out->n_sources;
}

bool jellyfin_parse_selected_media_source(const char *json,
                                           const char *requested_id,
                                           JFMediaSource *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    const char *limit = NULL;
    const char *p = media_sources_array(json, &limit);
    if (!p) return false;
    JFMediaSource first;
    bool have_first = false;
    while (p < limit) {
        p = skip_ws(p, limit);
        if (p >= limit || *p == ']') break;
        if (*p != '{') { p++; continue; }
        const char *oe = object_end(p, limit);
        if (!oe) break;
        JFMediaSource cur;
        if (parse_source(p, oe, &cur)) {
            if (!have_first) { first = cur; have_first = true; }
            if (requested_id && requested_id[0] &&
                strcmp(cur.id, requested_id) == 0) {
                *out = cur;
                return true;
            }
        }
        p = oe;
    }
    if (have_first) { *out = first; return true; }
    return false;
}

// Text subtitle formats can be fetched as SubRip and drawn on the console;
// bitmap ones cannot. Everything not recognised is treated as a bitmap, so an
// unknown format falls back to the burn-in that has always worked rather than
// to an empty overlay.
static bool ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return false;
    }
    return *a == *b;
}

bool jf_sub_is_text(const char *codec)
{
    if (!codec || !codec[0]) return false;
    static const char *kText[] = { "subrip", "srt", "ass", "ssa", "vtt",
                                   "webvtt", "text", "mov_text", "sami" };
    for (unsigned i = 0; i < sizeof(kText)/sizeof(kText[0]); i++)
        if (ieq(codec, kText[i])) return true;
    return false;
}

bool jf_sub_is_pgs(const char *codec)
{
    if (!codec || !codec[0]) return false;
    return ieq(codec, "pgssub") || ieq(codec, "hdmv_pgs_subtitle");
}
