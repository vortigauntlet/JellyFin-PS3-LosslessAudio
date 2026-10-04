#pragma once

// -------------------------------------------------------------------------
//  AAC channels -> the PS3's: what libfaad hands over, in CellAudio order
// -------------------------------------------------------------------------
//  libfaad returns interleaved floats in the file's own channel order and says where each
//  channel belongs (NeAACDecFrameInfo.channel_position).  The port's frame is
//  0=FL 1=FR 2=FC 3=LFE 4=SL 5=SR (six wide, a 5.1 program) or FL FR (two wide).
//
//  Pure (no PS3 headers): tests/test_aac.cpp compiles this file.
// -------------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

// libfaad's position codes (neaacdec.h), repeated so this file needs no libfaad.
enum {
    AAC_POS_UNKNOWN = 0, AAC_POS_FRONT_CENTER = 1, AAC_POS_FRONT_LEFT = 2, AAC_POS_FRONT_RIGHT = 3,
    AAC_POS_SIDE_LEFT = 4, AAC_POS_SIDE_RIGHT = 5, AAC_POS_BACK_LEFT = 6, AAC_POS_BACK_RIGHT = 7,
    AAC_POS_BACK_CENTER = 8, AAC_POS_LFE = 9
};

// Mixes `frames` interleaved frames of `ch` channels (positions pos[0..ch)) into out_ch-wide frames
// (6 or 2).  Six wide: each channel goes to its own slot; side and back channels share the surround
// slots (a 7.1 program's second pair is folded in at -3 dB), a back centre goes to both surrounds at
// -3 dB, mono goes to the centre.  Two wide: the front pair, plus the centre and the surrounds at -3 dB,
// all scaled by 1/(1+0.707+0.707) as the AC-3 downmix is so the sum cannot clip (a plain stereo or mono
// file is left alone, mono to both sides), and no LFE.  A channel of unknown position is left out; if
// every position is unknown the channels are taken in the usual order (L R C LFE SL SR).
void aac_map_frames(const float *in, int frames, int ch, const unsigned char *pos, float *out, int out_ch);

#ifdef __cplusplus
}
#endif
