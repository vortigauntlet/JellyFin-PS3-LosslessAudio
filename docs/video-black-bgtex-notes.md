# Video black after the background texture (10-14)

## Report

Audio plays, the player HUD shows, back works -- no picture.  First seen on the
first build with the dithered background texture (f4eda6b, "bgtex: on, 2 x
8100 KB" in the log).  The last good playback (PS3 log 10:47, 5efc4c4) and the
two bad ones (11:07, 11:08) took the same path: 24p switch to 23.976, 1:1
presentation, frames decoded (slot_write non-zero), displayed (disp_seq,
first frame displayed, vb: ticks), 648 and 274 frames before the user stopped.
Nothing in the player changed between them.

## What bgtex changed for the player

Only the VRAM heap: 2 x 8,294,400 bytes plus a fragment-program copy stay
allocated across playback, so vid_gpu_init's six YUV planes land elsewhere.

librsx's rsxMemalign is heapAllocateAligned with exactly the alignment asked.
The planes asked for 64; an RSX texture offset needs 128.  A layout that put a
plane on an odd 64 would sample from the wrong place.  This is the suspected
cause, NOT proven: nothing logged the plane offsets.

## Fix (unverified on hardware)

- YUV planes rsxMemalign(128) and their offsets logged ("vid_gpu: Y/Cb/Cr off").
  If a future bad log shows all offsets 128-aligned, alignment is ruled out.
- wave_bgtex_release() before vid_gpu_init: rsxFinish, free both textures; the
  menu reallocates and rebuilds them (24 frames of gouraud fallback on return).
  Playback then gets the pre-bgtex layout back whatever the real mechanism.
- Fallback if it is still black: jellyfin_bgtex.txt = 0 over FTP.
