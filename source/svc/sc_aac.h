// Raw AAC frames (as an MP4 track carries them, no ADTS) to interleaved stereo floats, on libfaad.
//
// The decoder is opened from the AudioSpecificConfig of the init segment (NeAACDecInit2), so it needs no
// header on each frame.  Output is always two channels, +-1.0: a mono stream is duplicated.
//
// libfaad is float in the portlibs build (2.7) and in the host's (2.11); both scale to +-1.0.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ScAac ScAac;

// NULL when libfaad rejects the config or memory runs out.
ScAac *scaac_open(const uint8_t *asc, int asc_len);
void   scaac_close(ScAac *a);

// Forget the overlap of the frame before: call after a seek, before decoding the first frame there.
void   scaac_reset(ScAac *a);

// Decodes one frame.  Returns the frames (L,R pairs) written to out, 0 when the frame made none, or -1 on a
// decode error.  cap_frames should be at least 2048 (a frame with SBR).
int    scaac_decode(ScAac *a, const uint8_t *frame, int len, float *out_lr, int cap_frames);

// The sample rate of the last decoded frame (SBR doubles the core rate); 0 before the first.
int    scaac_rate(const ScAac *a);

#ifdef __cplusplus
}
#endif
