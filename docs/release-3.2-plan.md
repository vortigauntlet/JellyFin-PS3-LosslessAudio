# JellyFin-PS3 3.2 — implementation plan

Written 2026-10-03 for a Sonnet session to implement. Scope decided with the
user: **fixes + pending merges, Dolby Digital passthrough as a real menu option,
a reorganised Settings screen, and the ui_wave statics refactor.** Offline
downloads are **not** in 3.2 (that is 3.3's headline).

Base: `origin/main` = `32219a5` (3.1 + README). Work branch: `release/3.2` in
the WSL worktree `~/jf-r32` (created from `origin/main` by the planning
session; this file is committed there as `docs/release-3.2-plan.md`).

---

## 0. Ground rules (read before touching anything)

These come from incidents on this project. Each one has cost a day before.

1. **Build in WSL, from a clean export, never from a shared tree.**
   ```bash
   wsl -e bash -c 'cd ~/jf-r32 && rm -rf ~/jf-r32-build && mkdir ~/jf-r32-build && git archive HEAD | tar -x -C ~/jf-r32-build && cd ~/jf-r32-build && export PS3DEV=/root/ps3dev PSL1GHT=/root/ps3dev PATH=/root/ps3dev/bin:/root/ps3dev/ppu/bin:$PATH && make pkg 2>&1 | tail -20'
   ```
   Commit first, then build — the archive only contains committed work. Other
   sessions' uncommitted files have been compiled into builds before.
   Use `wsl -e bash -c '...'` from the Bash tool for one-liners; for anything
   with backslashes, pipe a script through stdin, and never put anything
   load-bearing on line 1 of that script (a BOM eats it).
2. **The only EBOOT that boots** from `/dev_hdd0/game` is
   `obj/pkg/USRDIR/EBOOT.BIN` (from `make pkg`). Not `*.self`, not
   `*.fake.self`. "An error occurred during the start operation" = wrong file.
3. **Host tests:** `make -B -f tests/Makefile.host all && make -f tests/Makefile.host check`.
   Without `-B` a stale binary can report green. Grep the log for
   `undefined|error:|\*\*\*` rather than trusting the exit code.
4. **Compile-check a single PS3 file** without a full build (catches what host
   stubs can't):
   ```bash
   ppu-g++ -c -O2 -Wall -mcpu=cell -I/root/ps3dev/ppu/include \
     $(for d in source source/ui source/ui/render source/gfx source/util; do echo -n " -I$d"; done) \
     -o /tmp/x.o source/ui/render/ui_wave.cpp
   ```
5. **Deploying** (only when the user asks to test on the TV):
   `powershell -File JellyFin-PS3\tools\ps3push\ftp.ps1 -Local <EBOOT.BIN>`
   (console `192.168.0.202`). The script prints a "before" sha — the console
   is shared with other sessions; if "before" isn't the build you last pushed,
   say so before overwriting. Run `ListAgents` first.
6. **Never commit `data/jf_spu_kernel.bin`** — every build rewrites it.
   `git checkout data/jf_spu_kernel.bin` before `git add`.
7. **Comments state rules, not history.** No dates, no "long shot", no
   "hardware, 2026-09-29", no quoted TV verdicts, no "used to". Incidents and
   measurements go in `docs/*-notes.md`. Never write "identical",
   "bit-exact" or "pixel-perfect" unless a test asserts it. Several commits
   being ported below violate this — clean them as you port.
8. **Line endings:** the repo is LF. Before every commit run
   `git diff --cached --stat` and check no file shows a whole-file rewrite.
9. **Don't push, tag or publish a GitHub release** without the user saying so.
10. Commit messages end with
    `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>` (or whichever
    model is implementing).

---

## 1. Work items at a glance

| # | Item | Source | Size | Risk | TV test? |
|---|------|--------|------|------|----------|
| A1 | Paused-player / PS-button overlay freeze | cherry-pick `aaf4382` | XS | low | yes (quick) |
| A2 | Chunked-body decode fix (empty details page + hung retry) | uncommitted in `~/jf-bits` | XS | low | no |
| A3 | Send Log to Server + logging on by default | `8ddb523` (CRLF-normalise) | S | low | yes |
| B1 | Dolby Digital passthrough as an Audio Output value | port from `feature/bitstream` minus experiments | M | med | **yes** |
| B2 | Bitstream bug fixes: reassert channel, false "LPCM intact" log | new | S | low | with B1 |
| C1 | Settings: table-driven model + host test | new | M | med | — |
| C2 | Settings: sections, headers, L2/R2 section jump, Left/Right value step | new | M | med | yes |
| D1 | ui_wave.cpp statics → state structs | new, separate branch | L (mechanical) | **high** | **yes, long** |
| R  | Release 3.2 (version, changelog, fresh-install test, pkg) | — | S | — | yes |

Order: A → B → C → R-prep. D1 runs on its own branch in parallel and only
merges if it passes a TV soak before release day. **3.2 ships without D1
rather than waiting for it** — it has no user-visible value.

---

## A. Pending fixes

### A1. Paused player freezes when the PS button is pressed

**Bug:** paused + idle, the player loop stops flipping. The system draws its
PS-button overlay only on the app's flips, so the overlay was invisible but
ate every button — the player looked frozen.

**Fix (exists):** `git cherry-pick aaf4382` (on `feature/bitstream`; the same
change as `9ad37d5` on `fix/paused-overlay` — take `aaf4382`, it's the clean
one). It adds `volatile u32 g_sys_overlay` in `main.cpp`, set on
`SYSUTIL_DRAW_BEGIN`, cleared on `SYSUTIL_DRAW_END`, and the paused-idle gate in
`player.cpp` skips while it's set.

**Clean while porting:** the comment in `player.cpp` says
"(hardware, 2026-09-29)" — rewrite as a rule: *"Keep flipping while the system
overlay is up: it is drawn on our flips, so idling here would hide it while it
still takes input."*

**Also check (5 min):** grep for other idle loops that stop flipping
(`usleep` inside loops that skip `flip()` / `waitflip`): music screen paused,
buffering screen, error screen. Apply the same `!g_sys_overlay` guard to any
that gate their flip. List what you checked in the commit message.

Do **not** take anything else from `fix/paused-overlay` — the reassert modes
(`d0e8b28`…`27f760e`) were a disproven theory.

### A2. Chunked body that isn't chunk-framed

Uncommitted in `~/jf-bits/source/net/http.cpp` (`dechunk()`): if a response
says `Transfer-Encoding: chunked` but the body doesn't start with a hex chunk
size, keep the body as-is instead of decoding it to 0 bytes. Seen on an item
reply: 85 KB discarded, the details page empty, then a retry that hung.

Port it to `release/3.2` with the comment rewritten (no date):
```c
// A body announced as chunked that does not start with a chunk-size line is
// not chunk-framed; decoding it would discard it. Keep it as it is.
if (len > 0 && !isxdigit((unsigned char)body[0])) return len;
```
Make sure `<ctype.h>` is included. If `tests/test_invariant_http.cpp` can reach
`dechunk` (it's `static` today), add a case; if it can't, don't widen the
linkage just for this.

**Discard** the other uncommitted change in `~/jf-bits` (`surround.cpp`,
withdrawing Bitstream from the menu) — superseded by B1. Don't modify
`~/jf-bits` itself; copy the hunk.

### A3. Send Log to Server + logging on by default

Commit `8ddb523` on `feature/tester-diagnostics` (worktree `~/jf-v32`).
Contents are good; the problem is that it rewrote `source/ui/xmb/ui_nav.cpp`
with CRLF line endings (1607-line diff, only 5 real lines).

Port:
```bash
git cherry-pick -n 8ddb523
git show 8ddb523 -- source/ui/xmb/ui_nav.cpp | ...   # or simply:
git checkout HEAD -- source/ui/xmb/ui_nav.cpp
# re-apply the 4-line change by hand (row 15 -> Send Log action, stats -> row 16)
git diff --cached --stat   # ui_nav.cpp must show ~5 lines, not 1600
```
Note that C1 replaces the row-index code anyway — if you're doing C1
straight after, it's fine to port only the non-UI parts here
(`log_upload.cpp/.h`, `http_post_text`, the plog default) and wire the row in C1.

**Checks:**
- `grep -rn "api_key\|ApiKey\|Token=" source/` and confirm no plog line can
  contain the access token (today the token goes in the auth header and the
  stream URL has none — keep it that way; the uploaded log ends up on GitHub
  issues).
- The upload thread is `THREAD_JOINABLE` and joined before restarting — good.
  (On this SDK flag 0 means *detached*; don't "simplify" it to 0.)
- Help text currently uses `--`; use an en dash or reword.

---

## B. Dolby Digital passthrough

### Background (why this is a menu option)

The user's chain is PS3 → TV → soundbar over ARC/eARC. On **plain ARC** only
2-channel PCM or Dolby Digital gets through, so multichannel LPCM loses the
centre (a speaker-map test proved only L/R arrive). On eARC, LPCM 5.1 works.
Plain-ARC soundbars are very common, so **sending the film's Dolby Digital
track untouched** (IEC 61937 bursts over a 2-channel output) is the only way
those users get 5.1 with a centre. Proven on hardware: configure
`encoder=AUDIO_OUT_CODING_BITSTREAM (255)`, `channel=2`, rc=0, soundbar shows
DOLBY AUDIO, centre works — even with the DD/LPCM boxes unticked in the PS3's
sound settings. `audioOutGetState().soundMode.type` is **unreliable** (says 0
while the wire is DD) — never branch on it.

What doesn't work and must **not** ship: E-AC-3 passthrough (soundbar silent),
TrueHD bitstream (needs HBR, not reachable from cellAudio), DTS (soundbar has
no decoder; the configure is rejected).

### B1. Port

Source commits on `feature/bitstream` (base = main):

| commit | take? |
|---|---|
| `aaf4382` pause fix | already taken in A1 |
| `e93b815` AC-3 passthrough core (iec61937.c/h, adec_ac3 packing, audio.cpp no-gain path, file modes 6/7) | **yes** |
| `6a0a912` Bitstream surround mode, AC-3 track stream-copy, `track_label_is_ac3` | **yes**, minus the `hdprobe` part |
| `28858e7` hdprobe restores config | no (drop hdprobe entirely) |
| `c3d9cb3`, `a27b448` E-AC-3 | **no** |
| `bca8ae8` chantest | **no** |

Recommended mechanics: `git cherry-pick -n e93b815 6a0a912`, then strip, then
commit as one or two clean commits. Then `git diff origin/main` and check for
any surviving `eac3`, `chantest`, `hdprobe`, `jellyfin_eac3pt` — there must be
none. `tests/test_iec61937.c` + its Makefile.host entry come with `e93b815`;
keep them. Its "E-AC-3 header is rejected by `iec61937_pack_ac3`" case
stays — that's exactly the guard we want; drop only cases that exercise the
removed E-AC-3 *packing* path, if any.

**User-facing shape:**
- Setting file stays `jellyfin_surround.txt`, value `4` = Dolby Digital
  (`SURROUND_BITSTREAM` keeps its number, so nothing saved shifts).
- Audio Output cycle: `Stereo → 5.1 → [7.1] → Dolby Digital → Stereo`.
  Label **"Dolby Digital"** (not "Bitstream"). Update `surround.h` comments to
  match (the header still says "three states" and talks about the menu as
  "Off / AC-3 / HD" history — rewrite the header comment as current rules).
- `bitstream_mode()` maps `SURROUND_BITSTREAM` → `BITSTREAM_PASSTHROUGH_FORCE`
  (the proven semantics: send bursts even though GetState claims LPCM).
  `jellyfin_bitstream.txt` research values still override, as on main.
- Stream URL (`player_session.cpp` `build_stream_url`): DD tracks →
  `AudioCodec=ac3&AudioSampleRate=48000&MaxAudioChannels=6`, `copy_audio=true`,
  no `AudioBitrate` (a ceiling below the track rate demotes a copy to a
  transcode). Every other track → the existing AC-3 640 kbps transcode request.
  Never a TrueHD/DTS copy in this mode.
- `show_player`: passthrough opens a **2-channel** port.
- `adec_set_codec`: if passthrough is active and the codec isn't AC-3, end
  the bitstream config and decode to stereo LPCM (with one plog line).
- Volume: no gain on IEC bursts (`audio.cpp` skips `apply_volume`). **New:**
  the player's volume slider must not pretend to work — when
  `audio_passthrough_active()`, the slider either doesn't open or shows
  "Volume is set on your soundbar/receiver" (pick whichever is fewer lines in
  the HUD code; check both legacy and spine HUDs).
- Dialogue Boost has no effect in passthrough (nothing is decoded). In
  Settings, draw its value faint as `n/a` while Audio Output is Dolby Digital;
  its help text gains: "Not available with Dolby Digital passthrough."
- Music player is unaffected (stereo by design) — verify `music_screen` never
  calls `audio_passthrough_request(true)`.
- `audio_close()` calls `audio_passthrough_request(false)` and
  `audio_bitstream_end()` so the XMB/next app never inherits encoder 255.

**Help text** (two lines, ≤ ~70 chars each, matches the panel):
```
Stereo, 5.1 or 7.1 over HDMI. Dolby Digital sends the film's Dolby
track untouched: use it if your soundbar drops the centre channel.
```

### B2. Bitstream fixes

1. **Reassert uses the wrong channel count.** `audio_bitstream_reassert()`
   (called after the 24p switch and revert in `display_24p.cpp`) hard-codes
   `want.channel = 6`. In passthrough that must be `2` with encoder 255.
   Fix: `audio_bitstream_begin()` stores the `audioOutConfiguration` it
   applied (`s_applied_cfg`); `reassert()` re-applies exactly that. Log line
   keeps the rc but drops the GetState "wire type" claim (see 2).
2. **False log wording.** `audio_bitstream.cpp` logs
   `"routing as %s, wire stays type=%u (LPCM intact)"` and nearby comments
   claim the wire stays LPCM. That's false whenever Dolby Digital is ticked in
   the PS3's sound settings (the AC-3 request then becomes real DD), and
   GetState can't tell. Reword the log to
   `"bitstream: requested %s (GetState type=%u, not authoritative)"` and fix
   the comments at ~lines 109 and 319 on main to say the request *may* become
   real Dolby Digital depending on the console's sound settings.
3. **Do not change** the 5.1/7.1 default AC-3 "routing" request behaviour in
   3.2. It's how every current user gets a centre channel; whether it ends up
   PCM or DD depends on ARC vs eARC and console settings, and that isn't
   something to re-tune now.

### B tests

- Host: `test_iec61937` (burst header Pa/Pb/Pc/Pd, 6144-byte frame for
  48 kHz AC-3, zero padding, byte order); `test_track_codec` gains
  `track_label_is_ac3` cases ("Dolby Digital 5.1", "AC3 5.1", "E-AC-3" must be
  **false**, "Dolby Digital Plus" must be **false**, "TrueHD" false).
- TV (user): see §T.

---

## C. Settings reorganised

Today: 16–17 rows in one list, and the row index is a magic number in three
places (`SETTINGS_LABELS` / `SETTINGS_ICONS` / `SETTINGS_HELP` arrays and the
`if (i == N)` draw chain in `ui_settings.cpp`, the `if (g_settings_sel == N)`
chain in `ui_nav.cpp xmb_input_settings`). Every new row shifted every
number (the changelogs are full of "stats moved to row 15").

### C1. Table-driven model

New file `source/ui/settings_model.c` + `.h` — **pure C, no PS3/RSX
includes**, so it host-tests:

```c
typedef enum {            // stable ids; never reorder-dependent
    SET_LOGOUT, SET_DEBUG_LOG, SET_SCREEN_SIZE, SET_HD1080, SET_AUDIO_OUT,
    SET_DIALOGUE, SET_SUB_FONT, SET_SUB_COLOUR, SET_THEME, SET_PARTICLES,
    SET_DAYNIGHT, SET_WAVE_INT, SET_AUTOSKIP, SET_24HZ, SET_UPDATE,
    SET_SEND_LOG, SET_STATS, SET__COUNT
} setting_id;

typedef enum { SEC_PLAYBACK, SEC_AUDIO, SEC_SUBTITLES, SEC_DISPLAY,
               SEC_SYSTEM, SEC__COUNT } setting_section;

typedef struct {
    setting_id      id;
    setting_section section;
    const char     *label;
    const char     *help;   // exactly two lines, '\n'-separated
} setting_row;

int                settings_count(void);            // visible rows (stats row compiled out => absent)
const setting_row *settings_row(int i);             // i in [0, count)
int                settings_index_of(setting_id);   // -1 if absent
const char        *settings_section_label(setting_section);
int                settings_section_first(setting_section); // row index, -1 if empty
```

The UI side (`ui_settings.cpp`) keeps a parallel table keyed by `setting_id`
with the PS3-specific bits: icon, `value(char *buf, int n, bool *accent)`,
`activate()`, `step(int dir)`. `xmb_input_settings` and the draw loop become
loops over the table — no row numbers anywhere. `XMB_SETTINGS_COUNT` goes
away; its users (`ui_xmb.cpp slog_menu_tick`, `ui_nav.cpp`, `ui_spine.cpp`
"Up at row 0 leaves") call `settings_count()`. `g_settings_sel` stays a plain
row index, so the spine's "Up from the first row leaves the tab" keeps working
unchanged.

**No setting file names or values change.** This is presentation only.

### C2. Sections and controls

Order and grouping:

| Section | Rows |
|---|---|
| **Playback** | 1080p Playback (Alpha) · 24Hz Output · Auto Skip |
| **Audio** | Audio Output · Dialogue Boost |
| **Subtitles** | Subtitle Font · Subtitle Colour |
| **Display** | Screen Size · Theme · Day / Night Palette · Wave Intensity · Menu Particles |
| **System** | Software Update · Send Log to Server · Debug Logging · Player Stats Overlay · Log Out |

Log Out moves to the bottom (it was first only because it was the original
row; it's the most destructive action and needn't be the default focus).
First focus is therefore 1080p Playback.

**Layout** (in `ui_settings.cpp`):
- Section header: label in `UIS_TF(13)`, `XMB_TEXT_FAINT`, letter-spaced
  uppercase (e.g. `PLAYBACK`), at `list_x + UIS_W(20)`, with a 1 px
  `XMB_HAIRLINE` rule from the end of the text to the list's right edge.
  Header block height `UIS_H(34)`; rows keep `SET_ROW_H` / current pitch.
- Replace the uniform-pitch window (`settings_first_row`/`settings_row_y`)
  with a computed layout: one pass builds `y[]` for every header and row
  (content coordinates), then a scroll offset in pixels keeps the selected row
  fully inside `[rows_top, display_height - XMB_BOTTOM_PAD]` **and**, when the
  selected row is the first of its section, keeps that section's header
  visible too. Scroll eases (`~120 ms`, the same easing the grids use) rather
  than snapping, if a shared easing helper exists; otherwise snap.
- Draw only items that intersect the visible band (clip with the existing
  scissor helpers if present; otherwise skip partial items).
- `settings_open_help_peek()` takes its rect from the new layout.
- The legacy (non-spine) in-place help panel keeps working.
- Must look right at 720p and 1080p and with Screen Size overscan applied.

**Controls:**
- Up/Down: rows (headers are skipped — they're never selectable).
- **L2 / R2: jump to the first row of the previous/next section** (free in
  menus today — verify with `git grep "BTN_PRESSED(l2)" source/ui/xmb`).
- X: activate / next value (unchanged behaviour).
- **Left / Right: previous / next value** on value rows; ignored on action
  rows (Log Out, Screen Size, Software Update, Send Log). Toggles flip on
  either. Needs a direction-aware step in each module:
  `surround_step(int dir)`, `centermix_step`, `subfont_step`, `subcolor_step`,
  `theme_step`, Wave Intensity (`(lvl + dir + 4) % 4`). Implement as
  `xxx_step(int dir)` with `xxx_cycle()` = `xxx_step(+1)`, persisting once per
  step. Don't loop the forward cycle n-1 times (that writes the file n-1 times).
- Triangle help, O-closes-help, L1/R1 tabs: unchanged.
- Update the bottom hint bar for Settings: `Triangle About · L2/R2 Section`.

### C tests

`tests/test_settings_model.c` (add to `tests/Makefile.host` `all:`):
- every id in `[0, SET__COUNT)` appears exactly once (except `SET_STATS`
  when `ENABLE_PLAYER_STATS` is 0 — build the test both ways, `-DENABLE_PLAYER_STATS=0/1`);
- rows are grouped: a section never reappears after another starts;
- every help string has exactly one `'\n'` and each line ≤ 72 chars;
- every label ≤ 24 chars;
- `settings_section_first` returns the first row of each non-empty section.

Layout math is easiest to test if the scroll function is pure too: put
`settings_scroll_for(sel, item_y[], item_h[], n, band_top, band_bottom)` in
`settings_model.c` and assert the selected row is fully visible for every
`sel` at 720 and 1080 heights, and that the header is visible when `sel` is a
section's first row.

---

## D1. ui_wave.cpp statics → state structs (separate branch)

The open item from the 09-27 external review: `source/ui/render/ui_wave.cpp`
(2,717 lines) has ~109 file-scope mutable statics. Goal: group them into a
dozen named state structs, **no behaviour change**.

Branch `refactor/wave-state` off `release/3.2` once A–C are committed.

**Grouping** (line numbers are `origin/main`):

| struct (static instance) | members |
|---|---|
| `wave_audio_state s_au` | `s_amp[3] s_lum s_lum3[3] s_thick s_acc s_def s_band_lvl[3] s_band_kick` (104–113, 189–190) |
| `wave_nav_state s_nv` | `s_nav s_nav_init s_nav_fx s_nav_us` (144–147) |
| `jd_state s_jd` | `s_jd_loaded … s_jd_toggles` (177–186) |
| `vis_state s_vs` | `s_vis s_vis_off s_reveal_t0` (216–218) |
| `tint_state s_tn` | `s_art_rgb s_art_k s_art_on s_art_us s_album_rgb s_tint_rgb` (378–400, 929) |
| `wave_gpu s_gpu` | `s_bg s_bg_dim s_wave_fp_buf s_wave_fp_offset s_wave_vbuf[2] s_wave_vbuf_off[2] s_wave_vbuf_turn s_wave_varray s_wave_blend s_wave_jelly` (493–597) |
| `jw_render s_jr` (render thread only) | `s_jw[] s_jw_stage[] s_jw_disp[] s_jw_upload_us s_jw_repaired s_jw_dropped s_jw_off s_jw_cnt s_jw_have_geom s_jw_drawn s_jw_ex_stage s_jw_fresh` |
| `jw_stats s_js` | `s_jw_gen_us s_jw_frames s_jw_rebuilds s_jw_verts s_jw_draws s_jw_late s_jw_lerp_n s_jw_marker` |
| `jw_worker s_jk` (**shared with the worker thread**) | `s_jw_wdisp[] s_jw_wscratch[] s_jw_job_sy s_jw_job_aspect s_jw_job_look s_jw_back_* s_jw_job s_jw_worker_run s_jw_worker s_jw_tid` (465–466, 1287–1301) |
| `jw_interp s_ji` | `s_jw_interp_on … s_jw_t_frame` (1405–1415) |
| `wave_tune s_tu` | `s_jw_speed s_jw_rebuild_every s_jw_rebuild_phase s_jw_front_body s_bg_dither` |
| `snow_state s_sn` | `s_snow s_snow_spr[] s_mote_* s_obst_n` (674–698) |

`s_field` (already one object) and the `static const` tables stay as they are.

**Rules:**
- One struct per commit. After each: PS3 compile-check `ui_wave.cpp` (§0.4),
  `make -B` host tests, and a full `make pkg` every 3–4 commits.
- Pure renames: `s_jd_morph` → `s_jd.morph`. No reordering of statements, no
  "while I'm here" fixes, no changes to initial values (designated
  initialisers must reproduce every non-zero initial value — e.g.
  `s_nav_fx = {0,0,0,1}`, `s_lum = 1`, `s_amp = {1,1,1}`, `s_vis = -1`,
  `s_jw_speed = JW_SPEED_DEF/100`, `s_bg_dither = 1`, `s_mote_lvl = 1`,
  `s_jw_t_frame = 1`, `s_jw_job_aspect = 1`, `s_tint_rgb = {1,1,1}`).
  Write a checklist of every non-zero initialiser before starting and tick it off.
- Keep `volatile` on `s_jw_job` and `s_jw_worker_run` **as members**
  (`volatile int job;`), and keep any `__attribute__((aligned(N)))` on arrays
  (RSX/DMA-touched buffers need it — a misaligned store into RSX memory has
  wedged the GPU before). Add `_Static_assert(offsetof(...) % 16 == 0, ...)`
  for every array that is memcpy'd/uploaded to RSX.
- Do not touch the reuse-frame / blend branch logic around `s_jw_stage` —
  that's where the strobe bug was (a reuse frame fell into the legacy blend
  branch and overwrote `s_jw_stage[0..581]`); rename only.
- Size check per commit: `ppu-size` of `ui_wave.o` text/data/bss before and
  after — `bss+data` must match to within padding, `text` within ~1 %.
- Don't reflow the history comments; they were already moved to
  `docs/wave-renderer-notes.md`.

**Done when:** user TV soak (§T-D) passes. If it isn't TV-verified by release
day, 3.2 ships without it and the branch carries into 3.3.

---

## T. Hardware test script (for the user — one session, ~30 min)

Ask the user to run these; keep `player_log.txt` (now uploadable via
Settings → Send Log to Server, which is itself test T1).

**T1 Send Log:** Settings → System → Send Log to Server → "Sent".
Check Jellyfin Dashboard → Logs has `upload_PS3_*.log` with both runs.
**T2 Pause + PS button:** play anything, pause, wait 5 s, press PS → overlay
visible, navigable, "Return to game" resumes control.
**T3 Dolby Digital, DD track:** Audio Output = Dolby Digital; play a film with
a Dolby Digital 5.1 track → soundbar shows DOLBY AUDIO, centre speaks,
log has `copy_audio=true` and `AudioCodec=ac3`. Seek twice → still DD.
**T4 Dolby Digital, non-DD track** (TrueHD/DTS/AAC film) → still DOLBY AUDIO
(server transcode to AC-3), centre OK.
**T5 Dolby Digital + 24p** (24Hz Output = Auto, a 24 fps film) → after the
mode switch the log shows `re-asserted ... ch=2` (not 6) and audio is DD;
exit to menu → menu sounds normal (no encoder 255 leak).
**T6 Volume slider** in DD mode shows the soundbar message / doesn't open.
**T7 Back to 5.1:** set 5.1, play the same film → behaves exactly as 3.1.
**T8 Music** in DD mode plays stereo normally.
**T9 Settings:** sections render at 1080p and 720p, with Screen Size at a
non-zero overscan; L2/R2 jump sections; Left/Right step values; Triangle help
on a few rows; Up from the first row returns to the tab spine; values persist
after a restart.
**T-D (D1 build only):** JellyWave in menus 10 min; JellyDrop + snow with
music 10 min; Square cycles visualisers incl. Canyon; Off → JellyWave reveal;
a 24p film start/stop; watch for strobing, black frames, or a stuck wave.

---

## R. Release 3.2

1. `source/net/update_check.h`: `APP_VERSION "3.2"`.
2. `CHANGELOG.md`: new `## 3.2` section (draft below), user-facing, no
   internals.
3. `README.md`: controls table (Settings L2/R2, Left/Right) and the Audio
   Output description (Dolby Digital).
4. **Fresh-install test (mandatory — 3.0 shipped broken for new users).** On
   the console, rename every `/dev_hdd0/tmp/jellyfin_*.txt` except
   `jellyfin_config.txt` and `jellyfin_device_id.txt` to `.txt.devbak`,
   launch, and confirm: new interface, JellyWave, GPU posters/text, logging on,
   Audio Output = Stereo, Settings opens on 1080p Playback. **Restore the
   `.devbak` files afterwards.**
5. Clean build from `git archive` (§0.1); copy
   `obj/pkg/USRDIR/EBOOT.BIN`-containing `.pkg` to `release/JellyFin-PS3.pkg`
   and `outputs/JellyFin-PS3v3.2.pkg` (overwrite, don't add `-v2` files).
6. Merge `release/3.2` → `main` (fast-forward), and `feature/xmb-spine` ff to
   match, as 3.1 did. **Ask the user before pushing**, and before creating the
   GitHub Release `v3.2` (the in-app update check reads the latest release tag,
   so publishing it notifies every user).

### CHANGELOG draft

```markdown
## 3.2

### New
- **Dolby Digital output.** Settings → Audio → Audio Output has a new
  *Dolby Digital* choice. It sends the film's Dolby Digital track to your
  soundbar or receiver untouched (other tracks are converted to Dolby Digital
  by your server). Use it if your soundbar is connected through the TV's
  ARC port and the centre channel — where the dialogue is — goes missing in 5.1.
  Volume is then set on the soundbar.
- **Send Log to Server.** Settings → System → Send Log to Server uploads the
  diagnostic log to your Jellyfin server (Dashboard → Logs), so you can attach
  it to a bug report without FTP. Diagnostic logging is now on by default.
- **Settings has sections** — Playback, Audio, Subtitles, Display and System.
  `L2`/`R2` jump between sections, and `←`/`→` change a value in either
  direction (`X` still steps forward).

### Fixes
- Pressing the PS button while a video was paused could leave the player
  looking frozen; the system menu now appears.
- Some items opened to an empty details page and then hung on retry when the
  server's reply was framed unusually.
```

---

## Things deliberately left out of 3.2

- Offline downloads (`origin/feature/offline-downloads`, 403 commits behind
  main) — 3.3.
- E-AC-3 / TrueHD / DTS bitstream — not reachable or not decodable on the
  test chain.
- Changing how the 5.1/7.1 AC-3 routing request behaves.
- The `jellyfin_bitstream.txt` research modes on main stay as they are
  (file-only, undocumented).
- psl1ght-extras upstream patches, Moonlight work, Android TV port.
