# Changelog

## 3.1.1

A bug-fix release on top of 3.1. It adds no new features, apart from the
interface languages and 50 Hz output for 25 fps video.

### Playback
- **Fixed rubber-banding motion.** With streams that use B-frame pyramids (most
  x264 transcodes), the PS3 decoder returns pictures in decode order, so the
  picture stepped back and forth while the sound played normally. Pictures are
  now shown in timestamp order.
- **Fixed 24 fps film playing at about 20 fps** on 60 Hz output when frames
  weren't blended, followed by catch-up frame drops. The pulldown is a proper
  3:2 again.
- Whole frames on 60 Hz and interlaced outputs (no frame blending), so a TV's
  film mode no longer sees a broken cadence. Put `1` in `jellyfin_blend.txt` to
  turn blending back on.
- A/V sync no longer chases small offsets every refresh (a hitch about once a
  second).
- The frame rate comes from the stream's timestamps when the decoder reports the
  wrong one (30 fps live TV labelled 23.976), and the fallback rate is
  consistent when nothing reports one.
- **25 fps video switches the TV to 1080p 50Hz** when the TV supports it, through
  the same one-time check as 24Hz Output.
- Playback keeps updating while paused with the PS3's system overlay open.
- A server reply announced as chunked but not chunk-framed is no longer cut off.

### Languages
- English, Japanese, Brazilian Portuguese, German, French and Spanish.
  **Settings → Language**: Auto follows the PS3's own language setting.

## 3.1

**The new interface is now what everyone gets.** In 3.0 (and the beta before it),
the layered interface, JellyWave, and the GPU-drawn posters and text were all
switched off unless a setting file existed on the console, and nothing created
those files. A fresh install showed the old tab strip, a flat three-band wave,
and slower CPU-drawn menus. They are all on by default now. If you installed 3.0,
installing 3.1 over it is all you need to do.

### New
- **Settings → Software Update.** Shows the result of the launch check (up to
  date, a newer version available, or couldn't check). `X` checks again, or
  reopens the update popup when there is a new version.
- The update popup now says where to get the new `.pkg`.

### Controls
- **`O` stops playback**, the same as `Start`, whether or not the player bar is
  showing. If a track menu or the volume slider is open, `O` closes that first.
- **`□` hides the player bar** without waiting for it to time out. The player
  stats overlay is still on the remote's `DISPLAY` key and in Settings.
- **Remotes without `L1`/`R1`:** the Blu-ray remote's `|<<` `>>|` and `<<` `>>`
  keys now switch tabs in the menus, so a Bluetooth remote can reach every tab.
  Unrecognised remote keys are written to the debug log so they can be mapped.

### Fixes
- **Home posters on the selected row.** The poster cache has a fixed number of
  slots, and when Home wanted more than fit, the row you were looking at could
  lose out: some of its posters stayed blank until you moved off it. The
  selected row now loads first.
- JellyWave moves at the speed it was tuned to on a TV (a little calmer than 3.0's
  default).

### Turning things back off
Each of these still has a file in `/dev_hdd0/tmp/`; put `0` in it to switch that
feature off: `jellyfin_spine.txt` (layered interface), `jellyfin_gpuwave.txt`
(JellyWave), `jellyfin_gpucards.txt` and `jellyfin_gputext.txt` (GPU posters and
text). `jellyfin_jwspeed.txt` sets the JellyWave speed (default 14).
