// Host test for source/video/mkv_demux.cpp.
//
//  1. Hand-built files (a small EBML writer below) exercise what a muxer rarely produces: all three
//     lacing modes, BlockGroups with and without ReferenceBlock, unknown-size Segment and Clusters,
//     Cues after the clusters found through the SeekHead, header stripping, an encrypted track, a
//     file that ends inside a block, damage between clusters, a block too big for the buffer.
//  2. Real files made by ffmpeg (fixtures/mkv/) are read and every track's packet count and first
//     timestamp compared with what ffprobe counted.
//
//   make -f Makefile.host test_mkv_demux && ./test_mkv_demux

#include "mkv_demux.h"

#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

static int s_checks = 0, s_failed = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

// ---- a small EBML writer ----------------------------------------------------------------------

typedef std::string Bytes;

static Bytes id_bytes(uint32_t id) {
    Bytes b;
    int n = id > 0xFFFFFF ? 4 : id > 0xFFFF ? 3 : id > 0xFF ? 2 : 1;
    for (int i = n - 1; i >= 0; i--) b.push_back((char)(id >> (8 * i)));
    return b;
}
static Bytes size_bytes(uint64_t size, int min_len = 1) {
    int n = min_len;
    while (n < 8 && size >= ((1ull << (7 * n)) - 1)) n++;
    Bytes b((size_t)n, '\0');
    uint64_t v = size;
    for (int i = n - 1; i >= 0; i--) { b[(size_t)i] = (char)(v & 0xFF); v >>= 8; }
    b[0] = (char)(b[0] | (0x80 >> (n - 1)));
    return b;
}
static Bytes unknown_size() { return Bytes("\x01\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 8); }
static Bytes elem(uint32_t id, const Bytes &payload) { return id_bytes(id) + size_bytes(payload.size()) + payload; }
static Bytes uint_el(uint32_t id, uint64_t v) {
    Bytes p;
    int n = 1;
    while (n < 8 && (v >> (8 * n))) n++;
    for (int i = n - 1; i >= 0; i--) p.push_back((char)(v >> (8 * i)));
    return elem(id, p);
}
static Bytes float_el(uint32_t id, double d) {
    uint64_t u; memcpy(&u, &d, 8);
    Bytes p;
    for (int i = 7; i >= 0; i--) p.push_back((char)(u >> (8 * i)));
    return elem(id, p);
}
static Bytes str_el(uint32_t id, const std::string &s) { return elem(id, s); }

enum { ID_EBML = 0x1A45DFA3, ID_DOCTYPE = 0x4282, ID_SEGMENT = 0x18538067, ID_SEEKHEAD = 0x114D9B74, ID_SEEK = 0x4DBB,
       ID_SEEKID = 0x53AB, ID_SEEKPOS = 0x53AC, ID_INFO = 0x1549A966, ID_TIMESCALE = 0x2AD7B1, ID_DURATION = 0x4489,
       ID_TRACKS = 0x1654AE6B, ID_TRACKENTRY = 0xAE, ID_TRACKNUMBER = 0xD7, ID_TRACKTYPE = 0x83, ID_CODECID = 0x86,
       ID_CODECPRIVATE = 0x63A2, ID_LANGUAGE = 0x22B59C, ID_NAME = 0x536E, ID_FLAGDEFAULT = 0x88, ID_FLAGFORCED = 0x55AA,
       ID_DEFAULTDURATION = 0x23E383, ID_VIDEO = 0xE0, ID_PIXELW = 0xB0, ID_PIXELH = 0xBA, ID_AUDIO = 0xE1,
       ID_SAMPLERATE = 0xB5, ID_CHANNELS = 0x9F, ID_CONTENTENCODINGS = 0x6D80, ID_CONTENTENCODING = 0x6240,
       ID_ENCTYPE = 0x5033, ID_ENCCOMP = 0x5034, ID_COMPALGO = 0x4254, ID_COMPSETTINGS = 0x4255, ID_ENCENC = 0x5035,
       ID_CUES = 0x1C53BB6B, ID_CUEPOINT = 0xBB, ID_CUETIME = 0xB3, ID_CUETRACKPOS = 0xB7, ID_CUETRACK = 0xF7,
       ID_CUECLUSTERPOS = 0xF1, ID_CLUSTER = 0x1F43B675, ID_TIMECODE = 0xE7, ID_SIMPLEBLOCK = 0xA3,
       ID_BLOCKGROUP = 0xA0, ID_BLOCK = 0xA1, ID_BLOCKDURATION = 0x9B, ID_REFERENCEBLOCK = 0xFB, ID_VOID = 0xEC,
       ID_TAGS = 0x1254C367 };

static Bytes vint(uint64_t v, int len) {
    Bytes b((size_t)len, '\0');
    for (int i = len - 1; i >= 0; i--) { b[(size_t)i] = (char)(v & 0xFF); v >>= 8; }
    b[0] = (char)(b[0] | (0x80 >> (len - 1)));
    return b;
}

// A Block's payload: track vint, 16-bit relative time, flags, [lacing header], frames.
static Bytes block_payload(int track, int rel, uint8_t flags, const std::vector<Bytes> &frames, int lacing) {
    Bytes b = vint((uint64_t)track, 1);
    b.push_back((char)((rel >> 8) & 0xFF)); b.push_back((char)(rel & 0xFF));
    b.push_back((char)(flags | (lacing << 1)));
    if (lacing) {
        b.push_back((char)(frames.size() - 1));
        if (lacing == 1) {                                  // Xiph
            for (size_t i = 0; i + 1 < frames.size(); i++) {
                size_t s = frames[i].size();
                while (s >= 255) { b.push_back((char)255); s -= 255; }
                b.push_back((char)s);
            }
        } else if (lacing == 3 && frames.size() >= 2) {     // EBML
            b += vint(frames[0].size(), 2);
            int64_t prev = (int64_t)frames[0].size();
            for (size_t i = 1; i + 1 < frames.size(); i++) {
                const int64_t d = (int64_t)frames[i].size() - prev;
                b += vint((uint64_t)(d + 8191), 2);          // 2-byte bias is 2^13 - 1
                prev = (int64_t)frames[i].size();
            }
        }                                                   // fixed: no sizes
    }
    for (auto &f : frames) b += f;
    return b;
}
static Bytes simple_block(int track, int rel, bool key, const std::vector<Bytes> &frames, int lacing = 0) {
    return elem(ID_SIMPLEBLOCK, block_payload(track, rel, key ? 0x80 : 0, frames, lacing));
}

static Bytes frame_of(int seed, size_t n) {
    Bytes b(n, '\0');
    for (size_t i = 0; i < n; i++) b[i] = (char)((seed * 31 + (int)i * 7) & 0xFF);
    return b;
}

static Bytes avcc() {
    static const uint8_t sps[] = { 0x67, 0x64, 0x00, 0x28, 0xAC, 0xD9, 0x40, 0x78, 0x02, 0x27, 0xE5, 0x84, 0x00, 0x00, 0x03, 0x00, 0x04, 0x00, 0x00, 0x03, 0x00, 0xF0, 0x3C, 0x60, 0xC6, 0x58 };
    static const uint8_t pps[] = { 0x68, 0xEB, 0xE3, 0xCB, 0x22, 0xC0 };
    Bytes b;
    b.push_back(1); b.push_back(0x64); b.push_back(0); b.push_back(0x28); b.push_back((char)0xFF);   // 4-byte lengths
    b.push_back((char)0xE1);
    b.push_back(0); b.push_back((char)sizeof sps); b.append((const char *)sps, sizeof sps);
    b.push_back(1);
    b.push_back(0); b.push_back((char)sizeof pps); b.append((const char *)pps, sizeof pps);
    return b;
}

static Bytes track_video(int num, const Bytes &cp) {
    return elem(ID_TRACKENTRY, uint_el(ID_TRACKNUMBER, (uint64_t)num) + uint_el(ID_TRACKTYPE, 1) +
        str_el(ID_CODECID, "V_MPEG4/ISO/AVC") + elem(ID_CODECPRIVATE, cp) + uint_el(ID_DEFAULTDURATION, 41708333) +
        elem(ID_VIDEO, uint_el(ID_PIXELW, 1280) + uint_el(ID_PIXELH, 720)));
}
static Bytes track_audio(int num, const char *codec, const char *lang, int ch, bool def = true, uint64_t dd = 0) {
    Bytes t = uint_el(ID_TRACKNUMBER, (uint64_t)num) + uint_el(ID_TRACKTYPE, 2) + str_el(ID_CODECID, codec) +
        str_el(ID_LANGUAGE, lang) + uint_el(ID_FLAGDEFAULT, def ? 1 : 0) +
        elem(ID_AUDIO, float_el(ID_SAMPLERATE, 48000.0) + uint_el(ID_CHANNELS, (uint64_t)ch));
    if (dd) t += uint_el(ID_DEFAULTDURATION, dd);
    return elem(ID_TRACKENTRY, t);
}
static Bytes track_sub(int num, const char *codec, const char *name, bool forced) {
    return elem(ID_TRACKENTRY, uint_el(ID_TRACKNUMBER, (uint64_t)num) + uint_el(ID_TRACKTYPE, 17) +
        str_el(ID_CODECID, codec) + str_el(ID_NAME, name) + uint_el(ID_FLAGFORCED, forced ? 1 : 0) + str_el(ID_LANGUAGE, "eng"));
}

struct Built {
    Bytes file;
    uint64_t seg_data;                 // where the Segment's data starts
    std::vector<uint64_t> cluster_pos; // each cluster, from the segment data start
};

struct Opts {
    bool unknown_segment = false, unknown_clusters = false, cues = true, cues_at_end = true, seekhead_cues = true;
};

static Built build(const Bytes &tracks, const std::vector<Bytes> &clusters_payload, const std::vector<uint64_t> &cluster_ticks,
                   const Opts &o, double duration_ticks = 10000.0) {
    Built out;
    Bytes head = elem(ID_EBML, str_el(ID_DOCTYPE, "matroska"));
    Bytes info = elem(ID_INFO, uint_el(ID_TIMESCALE, 1000000) + float_el(ID_DURATION, duration_ticks));
    Bytes trk = elem(ID_TRACKS, tracks);

    // clusters, with their positions relative to the segment data
    std::vector<Bytes> clusters;
    for (size_t i = 0; i < clusters_payload.size(); i++) {
        Bytes body = uint_el(ID_TIMECODE, cluster_ticks[i]) + clusters_payload[i];
        clusters.push_back(o.unknown_clusters ? id_bytes(ID_CLUSTER) + unknown_size() + body : elem(ID_CLUSTER, body));
    }
    // the SeekHead has a fixed size, so the layout can be computed in one pass
    auto seekhead = [&](uint64_t cues_pos) {
        Bytes pos;                                      // SeekPosition: a plain 4-byte unsigned integer
        for (int i = 3; i >= 0; i--) pos.push_back((char)(cues_pos >> (8 * i)));
        return elem(ID_SEEKHEAD, elem(ID_SEEK, elem(ID_SEEKID, id_bytes(ID_CUES)) + elem(ID_SEEKPOS, pos)));
    };
    const size_t sh_len = o.seekhead_cues && o.cues ? seekhead(0).size() : 0;
    uint64_t pos = sh_len + info.size() + trk.size();
    for (auto &c : clusters) { out.cluster_pos.push_back(pos); pos += c.size(); }
    Bytes cues;
    if (o.cues) {
        Bytes pts;
        for (size_t i = 0; i < clusters.size(); i++)
            pts += elem(ID_CUEPOINT, uint_el(ID_CUETIME, cluster_ticks[i]) +
                        elem(ID_CUETRACKPOS, uint_el(ID_CUETRACK, 1) + uint_el(ID_CUECLUSTERPOS, out.cluster_pos[i])));
        cues = elem(ID_CUES, pts);
    }
    Bytes seg;
    if (o.cues && o.seekhead_cues) seg += seekhead(pos);
    seg += info + trk;
    for (auto &c : clusters) seg += c;
    if (o.cues) seg += cues;
    out.file = head + id_bytes(ID_SEGMENT) + (o.unknown_segment ? unknown_size() : size_bytes(seg.size(), 8)) + seg;
    out.seg_data = head.size() + 4 + 8;                // the Segment's 4-byte id and 8-byte size
    return out;
}

// ---- reading from memory -------------------------------------------------------------------------

struct Mem { const Bytes *b; int fail_after; int reads; };
static int mem_read(void *ctx, uint64_t off, uint8_t *buf, int len) {
    Mem *m = (Mem *)ctx;
    m->reads++;
    if (m->fail_after >= 0 && m->reads > m->fail_after) return -1;
    if (off >= m->b->size()) return 0;
    const size_t n = std::min<size_t>((size_t)len, m->b->size() - (size_t)off);
    memcpy(buf, m->b->data() + off, n);
    return (int)n;
}

struct Fr { int track; int64_t pts; bool key; Bytes data; uint64_t dur; };
static std::vector<Fr> read_all(MkvFile *f, uint32_t cap = 1 << 20, MkvReader *keep = nullptr) {
    static std::vector<uint8_t> buf;
    buf.assign(cap, 0);
    MkvReader r;
    mkv_reader_init(&r, f, buf.data(), cap);
    std::vector<Fr> out;
    MkvFrame fr;
    int rc;
    while ((rc = mkv_next_frame(&r, &fr)) == 1) {
        Bytes d((const char *)fr.data, fr.size);
        if (fr.prefix_len) d = Bytes((const char *)fr.prefix, (size_t)fr.prefix_len) + d;
        out.push_back({ fr.track, fr.pts_ns, fr.key, d, fr.duration_ns });
    }
    if (keep) *keep = r;
    return out;
}

// ---- the tests -------------------------------------------------------------------------------------

static void tracks_and_frames() {
    printf("- tracks, frames, lacing\n");
    Bytes tracks = track_video(1, avcc()) + track_audio(2, "A_AC3", "eng", 6, true, 32000000) +
                   track_audio(3, "A_DTS", "fra", 6, false) + track_sub(4, "S_TEXT/UTF8", "Commentary", true);
    std::vector<Bytes> c(2);
    // cluster 0 (0 ms): a key frame, audio, a P frame in a BlockGroup with a ReferenceBlock, a B frame
    c[0] += simple_block(1, 0, true, { frame_of(1, 5000) });
    c[0] += simple_block(2, 0, true, { frame_of(2, 1000) });
    c[0] += elem(ID_BLOCKGROUP, elem(ID_BLOCK, block_payload(1, 42, 0, { frame_of(3, 700) }, 0)) +
                                uint_el(ID_BLOCKDURATION, 42) + elem(ID_REFERENCEBLOCK, Bytes("\xFF", 1)));
    c[0] += elem(ID_BLOCKGROUP, elem(ID_BLOCK, block_payload(1, 84, 0, { frame_of(4, 300) }, 0)) + uint_el(ID_BLOCKDURATION, 42));   // no reference: a key frame
    c[0] += simple_block(4, 100, true, { Bytes("1\n00:00:00,100 --> 00:00:01,000\nHi") });
    // cluster 1 (5000 ms): laced audio, every mode
    c[1] += simple_block(2, 0, true, { frame_of(10, 300), frame_of(11, 280), frame_of(12, 500) }, 1);         // Xiph, 3 frames
    c[1] += simple_block(2, 100, true, { frame_of(13, 300), frame_of(14, 305), frame_of(15, 290), frame_of(16, 40) }, 3);   // EBML, 4 frames
    c[1] += simple_block(2, 200, true, { frame_of(17, 160), frame_of(18, 160) }, 2);                           // fixed, 2 frames
    c[1] += simple_block(3, 0, true, { frame_of(19, 600), frame_of(20, 270) }, 1);                             // Xiph with a 255+ frame size boundary
    c[1] += simple_block(2, 300, true, { frame_of(21, 77) }, 3);                                               // EBML, one frame
    c[1] += simple_block(2, 310, true, { frame_of(22, 510), frame_of(23, 3) }, 1);                             // Xiph, a size of 255*2
    Opts o;
    Built b = build(tracks, c, { 0, 5000 }, o);
    Mem m = { &b.file, -1, 0 };
    MkvFile f;
    CHECK(mkv_open(&f, mem_read, &m, b.file.size()) == 0);
    CHECK(f.n_tracks == 4 && f.timescale_ns == 1000000);
    CHECK(fabs(f.duration_ns - 10000.0 * 1e6) < 1.0);
    CHECK(f.seg_data == b.seg_data);
    const MkvTrack *v = mkv_track(&f, 1), *a = mkv_track(&f, 2), *d = mkv_track(&f, 3), *s = mkv_track(&f, 4);
    CHECK(v && v->type == MKV_TRACK_VIDEO && v->width == 1280 && v->height == 720 && v->default_duration_ns == 41708333);
    CHECK(v && mkv_video_codec(v) == MKV_VC_AVC && v->cp_len == (int)avcc().size());
    CHECK(a && a->type == MKV_TRACK_AUDIO && !strcmp(a->language, "eng") && a->is_default && a->channels == 6 &&
          a->sample_rate == 48000.0 && mkv_audio_codec(a) == MKV_AC_AC3);
    CHECK(d && !strcmp(d->language, "fra") && !d->is_default && mkv_audio_codec(d) == MKV_AC_DTS);
    CHECK(s && s->is_forced && !strcmp(s->name, "Commentary") && mkv_sub_codec(s) == MKV_SC_SRT);
    CHECK(mkv_first_track(&f, MKV_TRACK_VIDEO) == v && mkv_first_track(&f, MKV_TRACK_AUDIO) == a);
    CHECK(mkv_track(&f, 9) == nullptr);
    CHECK(f.n_cues == 2 && f.cue_track == 1);

    auto fr = read_all(&f);
    CHECK(fr.size() == 19);                                           // 5 in cluster 0, 14 in cluster 1 (laced frames count one each)
    size_t i = 0;
    auto next = [&](int) -> Fr * { return i < fr.size() ? &fr[i++] : nullptr; };
    Fr *x;
    x = next(1); CHECK(x && x->track == 1 && x->pts == 0 && x->key && x->data == frame_of(1, 5000));
    x = next(2); CHECK(x && x->track == 2 && x->pts == 0 && x->key && x->data == frame_of(2, 1000));
    x = next(1); CHECK(x && x->track == 1 && x->pts == 42000000 && !x->key && x->data == frame_of(3, 700));    // ReferenceBlock: not a key frame
    x = next(1); CHECK(x && x->track == 1 && x->pts == 84000000 && x->key && x->data == frame_of(4, 300));     // none: a key frame
    x = next(4); CHECK(x && x->track == 4 && x->pts == 100000000 && x->data.find("Hi") != std::string::npos);
    // Xiph, 3 frames, 32 ms apart (the track's default duration)
    x = next(2); CHECK(x && x->pts == 5000000000ll && x->data == frame_of(10, 300) && x->dur == 32000000);
    x = next(2); CHECK(x && x->pts == 5032000000ll && x->data == frame_of(11, 280));
    x = next(2); CHECK(x && x->pts == 5064000000ll && x->data == frame_of(12, 500));
    // EBML, 4 frames
    x = next(2); CHECK(x && x->pts == 5100000000ll && x->data == frame_of(13, 300));
    x = next(2); CHECK(x && x->pts == 5132000000ll && x->data == frame_of(14, 305));
    x = next(2); CHECK(x && x->pts == 5164000000ll && x->data == frame_of(15, 290));
    x = next(2); CHECK(x && x->pts == 5196000000ll && x->data == frame_of(16, 40));
    // fixed, 2 frames
    x = next(2); CHECK(x && x->pts == 5200000000ll && x->data == frame_of(17, 160));
    x = next(2); CHECK(x && x->pts == 5232000000ll && x->data == frame_of(18, 160));
    // track 3 has no default duration and the block no duration: laced frames share its time
    x = next(3); CHECK(x && x->track == 3 && x->pts == 5000000000ll && x->data == frame_of(19, 600) && x->dur == 0);
    x = next(3); CHECK(x && x->track == 3 && x->data == frame_of(20, 270));
    x = next(2); CHECK(x && x->pts == 5300000000ll && x->data == frame_of(21, 77));
    x = next(2); CHECK(x && x->pts == 5310000000ll && x->data == frame_of(22, 510));
    x = next(2); CHECK(x && x->pts == 5342000000ll && x->data == frame_of(23, 3));
    CHECK(i == fr.size());
    mkv_close(&f);
}

static void odd_structure() {
    printf("- unknown sizes, cues after the clusters\n");
    Bytes tracks = track_video(1, avcc()) + track_audio(2, "A_AC3", "eng", 2);
    std::vector<Bytes> c(3);
    for (int k = 0; k < 3; k++) {
        c[k] += simple_block(1, 0, true, { frame_of(100 + k, 900) });
        c[k] += simple_block(2, 0, true, { frame_of(200 + k, 100) });
        c[k] += simple_block(1, 40, false, { frame_of(300 + k, 90) });
    }
    const std::vector<uint64_t> ticks = { 0, 2000, 4000 };
    for (int variant = 0; variant < 4; variant++) {
        Opts o;
        o.unknown_segment = variant & 1;
        o.unknown_clusters = variant & 2;
        Built b = build(tracks, c, ticks, o);
        Mem m = { &b.file, -1, 0 };
        MkvFile f;
        CHECK(mkv_open(&f, mem_read, &m, b.file.size()) == 0);
        CHECK(f.n_cues == 3);
        auto fr = read_all(&f);
        CHECK(fr.size() == 9);
        for (int k = 0; k < 3 && fr.size() == 9; k++) {
            CHECK(fr[k * 3].data == frame_of(100 + k, 900) && fr[k * 3].pts == (int64_t)ticks[k] * 1000000);
            CHECK(fr[k * 3 + 1].data == frame_of(200 + k, 100));
            CHECK(fr[k * 3 + 2].data == frame_of(300 + k, 90) && !fr[k * 3 + 2].key);
        }
        mkv_close(&f);
    }

    // no Cues at all, and no Duration: the duration is estimated from the last cluster
    Opts o; o.cues = false;
    Built b = build(tracks, c, ticks, o, 0.0);
    Mem m = { &b.file, -1, 0 };
    MkvFile f;
    CHECK(mkv_open(&f, mem_read, &m, b.file.size()) == 0);
    CHECK(f.n_cues == 0);
    CHECK(f.duration_ns >= 4000.0 * 1e6 - 1);
    mkv_close(&f);

    // Cues after the clusters are only found through the SeekHead: without one there are none, and
    // seeking falls back to the cluster search (see seeking())
    Opts o2; o2.seekhead_cues = false;
    b = build(tracks, c, ticks, o2);
    m = { &b.file, -1, 0 };
    CHECK(mkv_open(&f, mem_read, &m, b.file.size()) == 0);
    CHECK(f.n_cues == 0);
    mkv_close(&f);
}

static void seeking() {
    printf("- seeking\n");
    Bytes tracks = track_video(1, avcc()) + track_audio(2, "A_AC3", "eng", 2);
    std::vector<Bytes> c(8);
    std::vector<uint64_t> ticks;
    for (int k = 0; k < 8; k++) {
        ticks.push_back((uint64_t)k * 3000);
        c[k] += simple_block(1, 0, true, { frame_of(100 + k, 4000) });
        for (int j = 1; j < 6; j++) c[k] += simple_block(1, j * 40, false, { frame_of(500 + k * 10 + j, 300) });
        c[k] += simple_block(2, 0, true, { frame_of(900 + k, 200) });
    }
    for (int cues = 0; cues < 2; cues++) {
        Opts o; o.cues = cues != 0;
        Built b = build(tracks, c, ticks, o, 24000.0);
        Mem m = { &b.file, -1, 0 };
        MkvFile f;
        CHECK(mkv_open(&f, mem_read, &m, b.file.size()) == 0);
        std::vector<uint8_t> buf(1 << 20);
        MkvReader r;
        mkv_reader_init(&r, &f, buf.data(), (uint32_t)buf.size());
        struct { uint64_t target_ms; int cluster; } cases[] = {
            { 0, 0 }, { 1, 0 }, { 2999, 0 }, { 3000, 1 }, { 3001, 1 }, { 8999, 2 }, { 9000, 3 }, { 20999, 6 }, { 21000, 7 }, { 99999, 7 },
        };
        for (auto &t : cases) {
            uint64_t ct = 0;
            CHECK(mkv_seek(&f, &r, t.target_ms * 1000000ull, &ct));
            MkvFrame fr;
            CHECK(mkv_next_frame(&r, &fr) == 1);
            // the first frame is the cluster's key frame
            if (fr.track != 1 || !fr.key || fr.size != 4000 || fr.pts_ns != (int64_t)t.cluster * 3000 * 1000000ll) {
                printf("  cues=%d target=%llu: track %d key=%d size=%u pts=%lld (cluster %d expected)\n", cues,
                       (unsigned long long)t.target_ms, fr.track, fr.key, fr.size, (long long)fr.pts_ns, t.cluster);
            }
            CHECK(fr.track == 1 && fr.key && fr.size == 4000 && fr.pts_ns == (int64_t)t.cluster * 3000 * 1000000ll);
            CHECK(ct == (uint64_t)t.cluster * 3000 * 1000000ull);
            CHECK(memcmp(fr.data, frame_of(100 + t.cluster, 4000).data(), 4000) == 0);
        }
        mkv_close(&f);
    }
}

static void header_stripping_and_encryption() {
    printf("- header stripping, encrypted tracks\n");
    // a track whose frames are stored without their first two bytes
    Bytes comp = elem(ID_CONTENTENCODINGS, elem(ID_CONTENTENCODING, uint_el(ID_ENCTYPE, 0) +
                 elem(ID_ENCCOMP, uint_el(ID_COMPALGO, 3) + elem(ID_COMPSETTINGS, Bytes("\x0B\x77", 2)))));
    Bytes enc = elem(ID_CONTENTENCODINGS, elem(ID_CONTENTENCODING, uint_el(ID_ENCTYPE, 1) + elem(ID_ENCENC, Bytes("x"))));
    Bytes zlib = elem(ID_CONTENTENCODINGS, elem(ID_CONTENTENCODING, uint_el(ID_ENCTYPE, 0) +
                 elem(ID_ENCCOMP, uint_el(ID_COMPALGO, 0))));
    Bytes tracks = track_video(1, avcc()) +
        elem(ID_TRACKENTRY, uint_el(ID_TRACKNUMBER, 2) + uint_el(ID_TRACKTYPE, 2) + str_el(ID_CODECID, "A_AC3") + comp) +
        elem(ID_TRACKENTRY, uint_el(ID_TRACKNUMBER, 3) + uint_el(ID_TRACKTYPE, 2) + str_el(ID_CODECID, "A_AC3") + enc) +
        elem(ID_TRACKENTRY, uint_el(ID_TRACKNUMBER, 4) + uint_el(ID_TRACKTYPE, 2) + str_el(ID_CODECID, "A_AC3") + zlib) +
        track_audio(5, "A_AC3", "eng", 2);
    std::vector<Bytes> c(1);
    c[0] += simple_block(1, 0, true, { frame_of(1, 100) });
    c[0] += simple_block(2, 0, true, { Bytes("\x80\x01\x02") });
    Built b = build(tracks, c, { 0 }, Opts());
    Mem m = { &b.file, -1, 0 };
    MkvFile f;
    CHECK(mkv_open(&f, mem_read, &m, b.file.size()) == 0);
    CHECK(!mkv_track(&f, 2)->unsupported_encoding && mkv_track(&f, 2)->strip_len == 2);
    CHECK(mkv_track(&f, 3)->unsupported_encoding);                          // encrypted
    CHECK(mkv_track(&f, 4)->unsupported_encoding);                          // zlib
    CHECK(!mkv_track(&f, 5)->unsupported_encoding && mkv_track(&f, 5)->strip_len == 0);
    auto fr = read_all(&f);
    CHECK(fr.size() == 2 && fr[1].data == Bytes("\x0B\x77\x80\x01\x02", 5));   // the stripped header is put back
    mkv_close(&f);
}

static void damage() {
    printf("- truncated, damaged, oversize\n");
    Bytes tracks = track_video(1, avcc()) + track_audio(2, "A_AC3", "eng", 2);
    std::vector<Bytes> c(4);
    std::vector<uint64_t> ticks = { 0, 1000, 2000, 3000 };
    for (int k = 0; k < 4; k++) {
        c[k] += simple_block(1, 0, true, { frame_of(10 + k, 2000) });
        c[k] += simple_block(2, 0, true, { frame_of(20 + k, 100) });
    }
    Opts o; o.cues = false;
    Built b = build(tracks, c, ticks, o);

    // the file ends inside the third cluster: the frames before it are delivered, then the end
    {
        Bytes cut = b.file.substr(0, (size_t)(b.seg_data + b.cluster_pos[2] + 1500));
        Mem m = { &cut, -1, 0 };
        MkvFile f;
        CHECK(mkv_open(&f, mem_read, &m, cut.size()) == 0);
        auto fr = read_all(&f);
        CHECK(fr.size() == 4 && fr[0].data == frame_of(10, 2000) && fr[3].data == frame_of(21, 100));
        mkv_close(&f);
    }
    // damage where a cluster begins: that cluster is lost, the reader finds the next one and goes on
    {
        Bytes bad = b.file;
        const size_t at = (size_t)(b.seg_data + b.cluster_pos[2]);
        for (size_t i = 0; i < 12; i++) bad[at + i] = (char)0x00;
        Mem m = { &bad, -1, 0 };
        MkvFile f;
        CHECK(mkv_open(&f, mem_read, &m, bad.size()) == 0);
        MkvReader r;
        auto fr = read_all(&f, 1 << 20, &r);
        bool has[4] = { false, false, false, false };
        for (auto &x : fr)
            for (int k = 0; k < 4; k++) if (x.data == frame_of(10 + k, 2000)) has[k] = true;
        CHECK(has[0] && has[1] && !has[2] && has[3]);
        CHECK(r.damaged >= 1);
        mkv_close(&f);
    }
    // a frame bigger than the buffer is skipped and counted, the rest still arrive
    {
        Mem m = { &b.file, -1, 0 };
        MkvFile f;
        CHECK(mkv_open(&f, mem_read, &m, b.file.size()) == 0);
        MkvReader r;
        auto fr = read_all(&f, 1000, &r);
        CHECK(r.oversize == 4);                                             // the four 2000-byte frames
        CHECK(fr.size() == 4 && fr[0].track == 2);
        mkv_close(&f);
    }
    // a read error is reported, not mistaken for the end
    {
        Mem m = { &b.file, 12, 0 };
        MkvFile f;
        int rc = mkv_open(&f, mem_read, &m, b.file.size());
        if (rc == 0) {
            m.fail_after = m.reads + 2;
            std::vector<uint8_t> buf(1 << 20);
            MkvReader r;
            mkv_reader_init(&r, &f, buf.data(), (uint32_t)buf.size());
            MkvFrame fr;
            int last = 1;
            for (int i = 0; i < 20 && last == 1; i++) last = mkv_next_frame(&r, &fr);
            CHECK(last == -1 || last == 0);                                 // never loops, never crashes
            mkv_close(&f);
        } else {
            CHECK(rc == -1);
        }
    }
    // not Matroska
    {
        Bytes junk(4096, 'x');
        Mem m = { &junk, -1, 0 };
        MkvFile f;
        CHECK(mkv_open(&f, mem_read, &m, junk.size()) == -1 && f.err[0]);
        Bytes empty;
        Mem m2 = { &empty, -1, 0 };
        CHECK(mkv_open(&f, mem_read, &m2, 0) == -1);
        // a header with nothing after it
        Bytes head = elem(ID_EBML, str_el(ID_DOCTYPE, "matroska"));
        Mem m3 = { &head, -1, 0 };
        CHECK(mkv_open(&f, mem_read, &m3, head.size()) == -1);
        // a doctype that is not ours
        Bytes other = elem(ID_EBML, str_el(ID_DOCTYPE, "matroskb"));
        Mem m4 = { &other, -1, 0 };
        CHECK(mkv_open(&f, mem_read, &m4, other.size()) == -1);
    }
    // no tracks
    {
        Opts o2;
        Built nb = build(Bytes(), c, ticks, o2);
        Mem m = { &nb.file, -1, 0 };
        MkvFile f;
        CHECK(mkv_open(&f, mem_read, &m, nb.file.size()) == -1);
    }
}

static void avcc_parsing() {
    printf("- avcC\n");
    MkvAvcConfig a;
    const Bytes cp = avcc();
    CHECK(mkv_parse_avcc((const uint8_t *)cp.data(), (int)cp.size(), &a));
    CHECK(a.nal_length_size == 4 && a.n_sps == 1 && a.n_pps == 1 && a.sps_len[0] == 26 && a.pps_len[0] == 6);
    CHECK(a.sps[0][0] == 0x67 && a.pps[0][0] == 0x68);
    for (int n = 0; n < (int)cp.size(); n++) CHECK(!mkv_parse_avcc((const uint8_t *)cp.data(), n, &a));   // every cut is refused
    Bytes bad = cp; bad[0] = 2;
    CHECK(!mkv_parse_avcc((const uint8_t *)bad.data(), (int)bad.size(), &a));
    bad = cp; bad[4] = (char)0xFD;                                          // 2-byte lengths
    CHECK(mkv_parse_avcc((const uint8_t *)bad.data(), (int)bad.size(), &a) && a.nal_length_size == 2);
}

// ---- real files ---------------------------------------------------------------------------------------

static int file_read(void *ctx, uint64_t off, uint8_t *buf, int len) {
    FILE *f = (FILE *)ctx;
    if (fseeko(f, (off_t)off, SEEK_SET) != 0) return -1;
    return (int)fread(buf, 1, (size_t)len, f);
}

struct Expect { std::string file; int track; std::string type, codec; int packets; double first_pts; };

static void real_files() {
    printf("- ffmpeg-made files\n");
    FILE *ef = fopen("fixtures/mkv/expect.txt", "r");
    CHECK(ef != nullptr);
    if (!ef) return;
    std::vector<Expect> ex;
    char line[256];
    while (fgets(line, sizeof line, ef)) {
        char fn[64], type[32], codec[32], pts[32];
        int tr, pk;
        if (sscanf(line, "%63s %d %31s %31s %d %31s", fn, &tr, type, codec, &pk, pts) == 6)
            ex.push_back({ fn, tr, type, codec, pk, strcmp(pts, "N/A") ? atof(pts) : 0.0 });
    }
    fclose(ef);
    CHECK(ex.size() >= 9);

    for (const char *name : { "av.mkv", "live.mkv", "dts.mkv" }) {
        char path[128];
        snprintf(path, sizeof path, "fixtures/mkv/%s", name);
        FILE *fp = fopen(path, "rb");
        CHECK(fp != nullptr);
        if (!fp) continue;
        fseeko(fp, 0, SEEK_END);
        const uint64_t size = (uint64_t)ftello(fp);
        MkvFile f;
        const int rc = mkv_open(&f, file_read, fp, size);
        if (rc != 0) printf("  %s: %s\n", name, f.err);
        CHECK(rc == 0);
        if (rc != 0) { fclose(fp); continue; }
        const bool live = strcmp(name, "live.mkv") == 0;
        CHECK(live ? f.n_cues == 0 : f.n_cues >= 1);
        CHECK(f.duration_ns > 0);                                            // stated, or estimated for the live one

        auto fr = read_all(&f);
        int counts[MKV_MAX_TRACKS + 2] = { 0 };
        int64_t first[MKV_MAX_TRACKS + 2];
        for (auto &x : first) x = INT64_MAX;
        for (auto &x : fr) {
            if (x.track <= MKV_MAX_TRACKS) { counts[x.track]++; if (x.pts < first[x.track]) first[x.track] = x.pts; }
        }
        for (auto &e : ex) {
            if (e.file != name) continue;
            const MkvTrack *t = mkv_track(&f, e.track);
            CHECK(t != nullptr);
            if (!t) continue;
            CHECK((e.type == "video") == (t->type == MKV_TRACK_VIDEO) && (e.type == "audio") == (t->type == MKV_TRACK_AUDIO) &&
                  (e.type == "subtitle") == (t->type == MKV_TRACK_SUBTITLE));
            if (counts[e.track] != e.packets) printf("  %s track %d (%s): %d frames, ffprobe %d\n", name, e.track, e.codec.c_str(), counts[e.track], e.packets);
            CHECK(counts[e.track] == e.packets);
            // the first timestamp, to a millisecond (ffprobe prints start_time)
            if (e.type != "subtitle") {
                // ffprobe's start_time for audio is moved earlier by the encoder's priming (5 ms for AC-3, 23 ms
                // for MP3); the container's own time is 0.  Video must match to the millisecond.
                const double tol = e.type == "audio" ? 0.025 : 0.0011;
                const double got = (double)first[e.track] / 1e9;
                if (fabs(got - e.first_pts) >= tol)
                    printf("  %s track %d (%s): first %.6f s, ffprobe %.6f\n", name, e.track, e.codec.c_str(), got, e.first_pts);
                CHECK(fabs(got - e.first_pts) < tol);
            }
            if (e.codec == "h264") CHECK(mkv_video_codec(t) == MKV_VC_AVC && t->width == 64 && t->height == 64);
            if (e.codec == "ac3")  CHECK(mkv_audio_codec(t) == MKV_AC_AC3);
            if (e.codec == "mp3")  CHECK(mkv_audio_codec(t) == MKV_AC_MP3);
            if (e.codec == "dts")  CHECK(mkv_audio_codec(t) == MKV_AC_DTS);
            if (e.codec == "flac") CHECK(mkv_audio_codec(t) == MKV_AC_FLAC && t->cp_len >= 34);
            if (e.codec == "subrip") CHECK(mkv_sub_codec(t) == MKV_SC_SRT);
        }
        // every video frame carries a length-prefixed H.264 AU that adds up
        MkvAvcConfig avc;
        const MkvTrack *vt = mkv_first_track(&f, MKV_TRACK_VIDEO);
        CHECK(vt && mkv_parse_avcc(vt->codec_private, vt->cp_len, &avc) && avc.nal_length_size == 4);
        int keys = 0, bad_au = 0;
        for (auto &x : fr) {
            if (x.track != vt->number) continue;
            if (x.key) keys++;
            size_t p = 0;
            while (p + 4 <= x.data.size()) {
                const uint32_t n = ((uint8_t)x.data[p] << 24) | ((uint8_t)x.data[p + 1] << 16) | ((uint8_t)x.data[p + 2] << 8) | (uint8_t)x.data[p + 3];
                p += 4 + n;
            }
            if (p != x.data.size()) bad_au++;
        }
        CHECK(bad_au == 0 && keys >= 2);

        // seeking, where the file has a way to
        MkvReader r;
        std::vector<uint8_t> buf(1 << 20);
        mkv_reader_init(&r, &f, buf.data(), (uint32_t)buf.size());
        uint64_t ct = 0;
        CHECK(mkv_seek(&f, &r, 1500ull * 1000000, &ct));
        MkvFrame one;
        int rcn = 1;
        while (rcn == 1 && (rcn = mkv_next_frame(&r, &one)) == 1 && one.track != vt->number) {}
        CHECK(rcn == 1 && one.key && one.pts_ns <= 1500ll * 1000000 && one.pts_ns >= 0);
        mkv_close(&f);
        fclose(fp);
    }
}

int main() {
    tracks_and_frames();
    odd_structure();
    seeking();
    header_stripping_and_encryption();
    damage();
    avcc_parsing();
    real_files();
    printf("mkv demux: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
