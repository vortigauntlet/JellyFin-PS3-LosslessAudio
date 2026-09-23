#pragma once
#include <ppu-types.h>
#include "display_mode.h"

// Write the per-playback 24p decision record to player_log.txt:
//
//   24p: DISPLAY  what the output is doing and what the TV advertises
//   24p: CONTENT  what the stream is and how sure we are
//   24p: DECISION would 24Hz output help, and is it supported
//   24p: RESULT   what was attempted (always nothing -- see display_mode.h)
//
// Called once per stream, at the moment the frame rate is settled and
// timing_init() has run.  Read-only: queries the firmware, changes nothing.
void display_diag_session(u32 fps_num, u32 fps_den, dm_fps_source src,
                          u32 width, u32 height);
