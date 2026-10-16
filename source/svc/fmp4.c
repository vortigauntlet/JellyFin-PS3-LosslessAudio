#include "fmp4.h"
#include <stdlib.h>
#include <string.h>

#define FOURCC(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

static uint32_t u32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint16_t u16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint64_t u64(const uint8_t *p) { return ((uint64_t)u32(p) << 32) | u32(p + 4); }

typedef struct {
    uint32_t type;
    size_t   pos;            // the box header
    size_t   start;          // first payload byte
    size_t   end;            // one past the box
} box;

// The box at pos inside [pos, end).  False when it is truncated or has an impossible size.
static bool box_at(const uint8_t *b, size_t pos, size_t end, box *o) {
    if (end - pos < 8) return false;
    uint64_t size = u32(b + pos);
    size_t hdr = 8;
    if (size == 1) {
        if (end - pos < 16) return false;
        size = u64(b + pos + 8);
        hdr = 16;
    } else if (size == 0) {
        size = end - pos;
    }
    if (size < hdr || size > end - pos) return false;
    o->type = u32(b + pos + 4);
    o->pos = pos;
    o->start = pos + hdr;
    o->end = pos + (size_t)size;
    return true;
}

static bool find_child(const uint8_t *b, size_t start, size_t end, uint32_t type, box *o) {
    size_t pos = start;
    box c;
    while (pos < end && box_at(b, pos, end, &c)) {
        if (c.type == type) { *o = c; return true; }
        pos = c.end;
    }
    return false;
}

// ---- esds ----------------------------------------------------------------------------------------

// One descriptor header: tag, then the 1-4 byte expandable length.  False when it runs off the end.
static bool desc_at(const uint8_t *b, size_t *pos, size_t end, int *tag, size_t *len) {
    if (*pos >= end) return false;
    *tag = b[(*pos)++];
    size_t l = 0;
    for (int i = 0; i < 4; i++) {
        if (*pos >= end) return false;
        const uint8_t c = b[(*pos)++];
        l = (l << 7) | (c & 0x7F);
        if (!(c & 0x80)) break;
    }
    *len = l;
    return l <= end - *pos;
}

static bool esds_asc(const uint8_t *b, size_t start, size_t end, fmp4_init *out) {
    size_t pos = start + 4;                                  // version + flags
    int tag; size_t len;
    if (!desc_at(b, &pos, end, &tag, &len) || tag != 0x03) return false;
    size_t e = pos + len;
    if (e - pos < 3) return false;
    const uint8_t flags = b[pos + 2];
    pos += 3;
    if (flags & 0x80) pos += 2;
    if (flags & 0x40) { if (pos >= e) return false; pos += 1 + b[pos]; }
    if (flags & 0x20) pos += 2;
    if (pos > e) return false;
    if (!desc_at(b, &pos, e, &tag, &len) || tag != 0x04) return false;
    size_t e2 = pos + len;
    if (e2 - pos < 13) return false;
    pos += 13;                                               // object type, stream type, buffer size, max and avg bit rate
    if (!desc_at(b, &pos, e2, &tag, &len) || tag != 0x05) return false;
    if (len == 0 || len > FMP4_MAX_ASC) return false;
    memcpy(out->asc, b + pos, len);
    out->asc_len = (int)len;
    out->aot = out->asc[0] >> 3;
    if (out->aot == 31 && len > 1) out->aot = 32 + (((out->asc[0] & 7) << 3) | (out->asc[1] >> 5));
    return true;
}

// ---- init segment ----------------------------------------------------------------------------------

int fmp4_parse_init(const uint8_t *b, size_t len, fmp4_init *out) {
    memset(out, 0, sizeof(*out));
    out->aot = -1;
    box moov;
    if (!find_child(b, 0, len, FOURCC('m', 'o', 'o', 'v'), &moov)) return FMP4_E_NOMOOV;

    box mvex, trex;
    if (find_child(b, moov.start, moov.end, FOURCC('m', 'v', 'e', 'x'), &mvex) &&
        find_child(b, mvex.start, mvex.end, FOURCC('t', 'r', 'e', 'x'), &trex) && trex.end - trex.start >= 24) {
        out->trex_duration = u32(b + trex.start + 12);
        out->trex_size = u32(b + trex.start + 16);
        out->trex_flags = u32(b + trex.start + 20);
    }

    size_t pos = moov.start;
    box trak;
    bool found = false;
    while (pos < moov.end && box_at(b, pos, moov.end, &trak)) {
        pos = trak.end;
        if (trak.type != FOURCC('t', 'r', 'a', 'k')) continue;
        box mdia, hdlr, mdhd, minf, stbl, stsd;
        if (!find_child(b, trak.start, trak.end, FOURCC('m', 'd', 'i', 'a'), &mdia)) continue;
        if (!find_child(b, mdia.start, mdia.end, FOURCC('h', 'd', 'l', 'r'), &hdlr) || hdlr.end - hdlr.start < 12) continue;
        if (u32(b + hdlr.start + 8) != FOURCC('s', 'o', 'u', 'n')) continue;
        if (!find_child(b, mdia.start, mdia.end, FOURCC('m', 'd', 'h', 'd'), &mdhd) || mdhd.end - mdhd.start < 24) continue;
        out->timescale = b[mdhd.start] == 1 ? u32(b + mdhd.start + 20) : u32(b + mdhd.start + 12);
        if (!out->timescale) continue;
        if (!find_child(b, mdia.start, mdia.end, FOURCC('m', 'i', 'n', 'f'), &minf) ||
            !find_child(b, minf.start, minf.end, FOURCC('s', 't', 'b', 'l'), &stbl) ||
            !find_child(b, stbl.start, stbl.end, FOURCC('s', 't', 's', 'd'), &stsd) || stsd.end - stsd.start < 8) continue;
        box ent;
        if (!box_at(b, stsd.start + 8, stsd.end, &ent) || ent.type != FOURCC('m', 'p', '4', 'a') || ent.end - ent.start < 28) continue;
        const int ver = u16(b + ent.start + 8);
        out->channels = u16(b + ent.start + 16);
        out->rate = (int)(u32(b + ent.start + 24) >> 16);
        const size_t kids = ent.start + (ver == 1 ? 44 : ver == 2 ? 64 : 28);
        if (kids > ent.end) continue;
        box esds;
        if (!find_child(b, kids, ent.end, FOURCC('e', 's', 'd', 's'), &esds)) return FMP4_E_NOESDS;
        if (!esds_asc(b, esds.start, esds.end, out)) return FMP4_E_NOESDS;
        found = true;
        break;
    }
    return found ? 0 : FMP4_E_NOTRACK;
}

// ---- media segment ---------------------------------------------------------------------------------

void fmp4_seg_free(fmp4_seg *s) {
    free(s->s);
    memset(s, 0, sizeof(*s));
}

static int add_sample(fmp4_seg *o, int *cap, uint32_t off, uint32_t size, uint32_t dur) {
    if (o->n >= FMP4_MAX_SAMPLES) return FMP4_E_RANGE;
    if (o->n == *cap) {
        const int nc = *cap ? *cap * 2 : 512;
        fmp4_sample *g = (fmp4_sample *)realloc(o->s, sizeof(*g) * (size_t)nc);
        if (!g) return FMP4_E_MEM;
        o->s = g; *cap = nc;
    }
    o->s[o->n].off = off; o->s[o->n].size = size; o->s[o->n].dur = dur;
    o->n++;
    o->total_ticks += dur;
    return 0;
}

// `limit` is how far a sample may reach: the buffer's length for a whole segment, the end of the address space for a head.
static int parse_impl(const uint8_t *b, size_t len, uint64_t limit, const fmp4_init *init, fmp4_seg *out) {
    memset(out, 0, sizeof(*out));
    box moof;
    if (!find_child(b, 0, len, FOURCC('m', 'o', 'o', 'f'), &moof)) return FMP4_E_NOMOOF;
    const size_t moof_pos = moof.pos;
    int cap = 0, rc = 0;
    size_t pos = moof.start;
    box traf;
    bool got_traf = false;
    while (pos < moof.end && box_at(b, pos, moof.end, &traf)) {
        pos = traf.end;
        if (traf.type != FOURCC('t', 'r', 'a', 'f')) continue;
        got_traf = true;

        box tfhd;
        if (!find_child(b, traf.start, traf.end, FOURCC('t', 'f', 'h', 'd'), &tfhd) || tfhd.end - tfhd.start < 8) { rc = FMP4_E_SHORT; break; }
        const uint32_t tfflags = u32(b + tfhd.start) & 0xFFFFFF;
        size_t p = tfhd.start + 8;                            // version/flags + track id
        uint64_t base = moof_pos;
        if (tfflags & 0x01) { if (tfhd.end - p < 8) { rc = FMP4_E_SHORT; break; } base = u64(b + p); p += 8; }
        if (tfflags & 0x02) p += 4;
        uint32_t def_dur = init->trex_duration, def_size = init->trex_size;
        if (tfflags & 0x08) { if (tfhd.end - p < 4) { rc = FMP4_E_SHORT; break; } def_dur = u32(b + p); p += 4; }
        if (tfflags & 0x10) { if (tfhd.end - p < 4) { rc = FMP4_E_SHORT; break; } def_size = u32(b + p); p += 4; }

        box tfdt;
        if (find_child(b, traf.start, traf.end, FOURCC('t', 'f', 'd', 't'), &tfdt) && tfdt.end - tfdt.start >= 8) {
            if (out->n == 0)
                out->base_time = b[tfdt.start] == 1 && tfdt.end - tfdt.start >= 12 ? u64(b + tfdt.start + 4) : u32(b + tfdt.start + 4);
        }

        uint64_t run = (uint64_t)moof.end + 8;                // where the data goes when a trun has no data_offset
        size_t tp = traf.start;
        box trun;
        while (tp < traf.end && box_at(b, tp, traf.end, &trun)) {
            tp = trun.end;
            if (trun.type != FOURCC('t', 'r', 'u', 'n')) continue;
            if (trun.end - trun.start < 8) { rc = FMP4_E_SHORT; break; }
            const uint32_t tf = u32(b + trun.start) & 0xFFFFFF;
            size_t q = trun.start + 4;
            const uint32_t count = u32(b + q); q += 4;
            uint64_t data = run;
            if (tf & 0x01) {
                if (trun.end - q < 4) { rc = FMP4_E_SHORT; break; }
                data = base + (uint64_t)(int64_t)(int32_t)u32(b + q); q += 4;
            }
            if (tf & 0x04) q += 4;                            // first_sample_flags: audio samples are all sync samples
            size_t per = 0;
            if (tf & 0x100) per += 4;
            if (tf & 0x200) per += 4;
            if (tf & 0x400) per += 4;
            if (tf & 0x800) per += 4;
            if (q > trun.end || (per && (trun.end - q) / per < count)) { rc = FMP4_E_SHORT; break; }
            for (uint32_t i = 0; i < count; i++) {
                uint32_t dur = def_dur, size = def_size;
                if (tf & 0x100) { dur = u32(b + q); q += 4; }
                if (tf & 0x200) { size = u32(b + q); q += 4; }
                if (tf & 0x400) q += 4;
                if (tf & 0x800) q += 4;
                if (data > limit || size > limit - data) { rc = FMP4_E_RANGE; break; }
                rc = add_sample(out, &cap, (uint32_t)data, size, dur);
                if (rc) break;
                data += size;
            }
            if (rc) break;
            run = data;
        }
        if (rc) break;
    }
    if (rc == 0 && !got_traf) rc = FMP4_E_SHORT;
    if (rc == 0 && out->n == 0) rc = FMP4_E_NOSAMPLES;
    if (rc) { fmp4_seg_free(out); return rc; }
    return out->n;
}

int fmp4_parse_segment(const uint8_t *b, size_t len, const fmp4_init *init, fmp4_seg *out) {
    return parse_impl(b, len, (uint64_t)len, init, out);
}

int fmp4_parse_head(const uint8_t *b, size_t avail, const fmp4_init *init, fmp4_seg *out, size_t *need) {
    *need = 0;
    memset(out, 0, sizeof(*out));
    size_t pos = 0;
    for (int guard = 0; guard < 16; guard++) {                // styp, sidx and the like come before the moof
        if (avail - pos < 8) { *need = pos + 8; return FMP4_E_SHORT; }
        uint64_t size = u32(b + pos);
        size_t hdr = 8;
        if (size == 1) {
            if (avail - pos < 16) { *need = pos + 16; return FMP4_E_SHORT; }
            size = u64(b + pos + 8);
            hdr = 16;
        }
        if (size < hdr) return FMP4_E_SHORT;                   // malformed (a box that runs "to the end" cannot precede the moof)
        if (u32(b + pos + 4) == FOURCC('m', 'o', 'o', 'f')) {
            if (size > avail - pos) {
                *need = size > 0xFFFFFFu ? 0xFFFFFFu : pos + (size_t)size;      // an absurd moof asks for an absurd head: the caller refuses
                return FMP4_E_SHORT;
            }
            return parse_impl(b, avail, 0xFFFFFFFFULL, init, out);
        }
        if (size > 0xFFFFFFu) return FMP4_E_NOMOOF;
        pos += (size_t)size;
        if (pos > avail) { *need = pos + 8; return FMP4_E_SHORT; }
    }
    return FMP4_E_NOMOOF;
}
