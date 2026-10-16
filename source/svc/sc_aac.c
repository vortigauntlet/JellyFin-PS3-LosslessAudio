#include "sc_aac.h"
#include <stdlib.h>
#include <string.h>
#include <neaacdec.h>

struct ScAac {
    NeAACDecHandle h;
    uint8_t asc[32];
    int   asc_len;
    bool  src_mono;            // the config says one channel (libfaad returns it as two identical ones)
    int   rate;
};

// A fresh libfaad handle from the stored config.
static bool open_handle(ScAac *a) {
    a->h = NeAACDecOpen();
    if (!a->h) return false;
    NeAACDecConfigurationPtr cfg = NeAACDecGetCurrentConfiguration(a->h);
    cfg->outputFormat = FAAD_FMT_FLOAT;
    cfg->downMatrix = 0;
    cfg->dontUpSampleImplicitSBR = 0;                          // SBR output at its own (double) rate
    unsigned long rate = 0;
    unsigned char ch = 0;
    if (!NeAACDecSetConfiguration(a->h, cfg) ||
        NeAACDecInit2(a->h, a->asc, (unsigned long)a->asc_len, &rate, &ch) < 0) {
        NeAACDecClose(a->h);
        a->h = NULL;
        return false;
    }
    return true;
}

ScAac *scaac_open(const uint8_t *asc, int asc_len) {
    if (!asc || asc_len < 2 || asc_len > (int)sizeof(((ScAac *)0)->asc)) return NULL;
    ScAac *a = (ScAac *)calloc(1, sizeof(*a));
    if (!a) return NULL;
    memcpy(a->asc, asc, (size_t)asc_len);
    a->asc_len = asc_len;
    if (!open_handle(a)) { free(a); return NULL; }
    a->src_mono = ((asc[1] >> 3) & 15) == 1;                   // channelConfiguration, 2-byte config with a plain 4-bit rate index
    a->rate = 0;
    return a;
}

void scaac_close(ScAac *a) {
    if (!a) return;
    NeAACDecClose(a->h);
    free(a);
}

// A new handle rather than NeAACDecPostSeekReset: the decoder then starts from exactly the state a fresh
// stream does, so a seek that decodes a frame of lead-in produces the same samples as playing through
// (tests/svc/test_dec.c checks that bit for bit).  Keeps the old handle if a new one cannot be made.
void scaac_reset(ScAac *a) {
    if (!a) return;
    NeAACDecHandle old = a->h;
    a->h = NULL;
    if (open_handle(a)) NeAACDecClose(old);
    else a->h = old;
}

int scaac_rate(const ScAac *a) { return a ? a->rate : 0; }

int scaac_decode(ScAac *a, const uint8_t *frame, int len, float *out, int cap) {
    if (!a || len <= 0) return -1;
    NeAACDecFrameInfo info;
    memset(&info, 0, sizeof(info));
    const float *pcm = (const float *)NeAACDecDecode(a->h, &info, (unsigned char *)(uintptr_t)frame, (unsigned long)len);
    if (info.error || !pcm) return -1;
    const int ch = info.channels;
    if (ch <= 0) return -1;
    int frames = (int)(info.samples / (unsigned long)ch);
    if (frames <= 0) return 0;
    if (frames > cap) frames = cap;
    a->rate = (int)info.samplerate;
    if (ch == 1) {
        for (int i = 0; i < frames; i++) { out[2 * i] = pcm[i]; out[2 * i + 1] = pcm[i]; }
    } else if (a->src_mono && ch == 2 && !info.ps) {          // libfaad fills the second channel for parametric stereo only
        for (int i = 0; i < frames; i++) { out[2 * i] = pcm[2 * i]; out[2 * i + 1] = pcm[2 * i]; }
    } else {
        // The first two channels are the front pair for every layout SoundCloud serves (it serves stereo or mono).
        for (int i = 0; i < frames; i++) { out[2 * i] = pcm[(size_t)i * (size_t)ch]; out[2 * i + 1] = pcm[(size_t)i * (size_t)ch + 1]; }
    }
    return frames;
}
