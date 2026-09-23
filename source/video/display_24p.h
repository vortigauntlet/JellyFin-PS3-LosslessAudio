#pragma once
#include <ppu-types.h>

// Physical 1080p23.976 / 1080p24 HDMI output for 24fps film.
//
// Mechanism: cellVideoOutConfigure2 (cellSysutilAvconfExt) with a refresh
// value in the config -- the one game-facing call VSH honours a rate from.
// See display_mode.h for the firmware evidence and docs/24P_OUTPUT.md for the
// whole investigation and the hardware test procedure.
//
// Off unless /dev_hdd0/tmp/jellyfin_24p.txt contains 1.  Every switch is
// verified by TIMING the vblank handler against the timebase -- which is also
// how 23.976 is told from 24.000 -- and any doubt reverts to the mode the
// session started in.

// CellVideoOutConfiguration2, layout as read by VSH 4.93 (vsh.self 0x128000,
// 0x127f4c): the v1 struct with two of its reserved fields given meaning.
typedef struct {
	u8  resolution;     // +0  VIDEO_RESOLUTION_*
	u8  format;         // +1  VIDEO_BUFFER_FORMAT_*
	u8  aspect;         // +2  VIDEO_ASPECT_*
	u8  scanMode2;      // +3  VIDEO_SCANMODE2_* (0 auto, 1 i, 2 p)
	u8  reserved[6];    // +4
	u16 refreshRates;   // +A  ONE of 0, 0x01, 0x02, 0x10, 0x20, 0x40 -- VSH
	                    //     rejects anything else with 0x8002b226
	u32 pitch;          // +C
} videoConfiguration2;

#ifdef __cplusplus
extern "C" {
#endif
// avconf_stub.S.  Only valid while cellSysutilAvconfExt is loaded.
s32 videoConfigure2(u32 videoOut, videoConfiguration2 *cfg, void *option, u32 waitForEvent);
#ifdef __cplusplus
}
#endif

// Call once at startup, after plog is up.  If the previous run died while the
// output was in 24p (marker file still present), 24p is switched OFF so the
// console cannot come back into the same state -- and the log says so.
void d24_boot_check(void);

// Callbacks the player supplies, so this module never draws or reads the pad
// itself.  draw_prompt must draw AND flip AND wait for the flip; it is only
// ever called while the ORIGINAL mode is up.  poll_answer returns 1 for
// "yes, I can see it", -1 for "no", 0 for nothing yet.
typedef struct {
	void (*draw_prompt)(const char *line1, const char *line2);
	int  (*poll_answer)(void);
} d24_ui;

// Call after the prefill (frame rate known) and timing_register_vblank(),
// before any playback thread starts.  Returns true if the output is now at a
// MEASURED rate equal to the content rate; timing has then been re-initialised
// for it.  On false the output is exactly what it was before the call.
bool d24_session_begin(const d24_ui *ui);

// Call after every playback thread has been joined.  Restores the original
// mode if d24_session_begin switched it.  Safe to call unconditionally.
void d24_session_end(void);

// For the "24p:" diagnostic record.
bool d24_enabled(void);
