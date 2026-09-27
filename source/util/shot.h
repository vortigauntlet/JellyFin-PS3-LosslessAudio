#pragma once
#include <ppu-types.h>

// Screenshots of the app's own frames, for docs and bug reports.
//
// The console's own screenshot feature does not reach homebrew, so the app
// writes the frame it is about to show to /dev_hdd0/tmp/jfshot/<name>.bmp.
// Two triggers:
//   * R3 on a pad (numbered names), from poll_buttons();
//   * the file /dev_hdd0/tmp/jf_shot_req.txt, e.g. dropped over FTP; its first
//     word names the shot.  Checked every 30 flips.
// flip() asks shot_due() and, when true, waits for the GPU and calls
// shot_write() on the finished frame.

void shot_request(void);
bool shot_due(void);
void shot_write(const u32 *fb, int w, int h);
