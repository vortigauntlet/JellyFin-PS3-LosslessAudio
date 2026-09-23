# The spine — implementation plan (2026-09-23)

Plan for README §2.9 "Depth model — the spine" from the 2026-09-21 export
(`design-import-v2/`, copied from `C:\NewDownloads\Jellyfin PS3 UI Revamp\`).
`UI-BRIEF.md`'s rules all apply. `UI-HANDOFF.md` (09-20) predates this export
and is stale; do not plan from it.

**Status (2026-09-23 evening):** S0 through spine6 are built, and spine7 — the
reusable depth engine every category now uses — is committed on
`feature/xmb-spine` in the WSL repo (worktree `~/jf-spine`). Nothing since
spine2 has been seen on a TV. Read "spine7" at the end first. Section 1 lists
the original decisions; each stage says which ones it waited on.

---

## 0. What the design actually says

Sources, in authority order: README §2.9 (it calls itself "source of truth"),
then the two mockups that carry the spine, `uploads/dirE.html` (L1 Home) and
`uploads/dirD.html` (L2). None of the five `.dc.html` canvases draw the spine —
they are unchanged from v1 apart from a stripped runtime script.

Three levels, one composition. Down descends, Up / ○ ascends, Left / Right
moves within the level. L1 names, L2 describes, L3 states every fact.

| constant | L1 Home | L2 Category | L3 Item |
| --- | --- | --- | --- |
| spine icon centre y | 232 | 141 | 74 |
| active icon | 64 | 56 | 48 |
| active label | 15 | 13.5 | 13 |
| spine glow alpha | 0.12 | 0.09 | 0.06 |
| header divider | not drawn | y=214 | y=144 |
| focused artwork | 128×192 | 200×300 | 216×324 + backdrop |
| title size | 30 | 36 | 25 |
| stride · active x | 200 · 230 | same | same |
| idle falloff | 0.62/0.46/0.33/0.22/0.14 | same | same |

- Idle icons 44×44, 1.4px stroke, `#39406a`. Active in accent purple, 1.16×
  scale, two-stage bloom, uppercase Satoshi label at 0.04em, 26×2 accent
  underline, over an elliptical glow band (dirE: `ellipse 28% 50%` of a 112px
  band centred on the active slot).
- The geometry never re-lays-out; it slides up and shrinks. One float animates
  0→1→2, ~280 ms slow-in/slow-out, no spring or overshoot.
- The item column swings from vertical (L1) to horizontal (L2). At L1 the
  column's lower half is drawn before the wave, so the ribbons pass over it
  ("sinking into the horizon"). Items tip further back as they descend.
- ✕ opens detail and never plays. Playback starts only from detail. △ plays
  the trailer. There is no separate △ overlay and no fourth level.
- L3 is the existing item detail screen wearing the depth-2 spine.

### Measured from the mockups (script, not eyeballed)

| | dirE (L1) | dirD (L2) | README §2.9 |
| --- | --- | --- | --- |
| icon centre y | 232 | 141 | 232 / 141 |
| stride | 128 | 128 | **200** |
| active x | 152 | 152 | **230** |
| idle icon | 26 | 26 | **44** |
| active icon | 36 | 32 | **64 / 56** |
| falloff | 0.62/0.46/0.33/0.22/0.14 | same | same |
| focused art | 128×192 at (88, 300) | — | 128×192 |

**The y-positions and falloff agree everywhere; the spine's SIZE does not.** The
README is 1.5–1.8× the mockups, and not uniformly. That is decision D1.

---

## 1. Decisions needed

| # | question | my default | blocks |
| --- | --- | --- | --- |
| D1 | Spine size: README (200/230/44/64) or mockups (128/152/26/36)? | README. It is the later, self-declared authority, and "too small on the TV" is this UI's recurring complaint. Both are one table in `spine.h`, so switching later is one edit. | S1 |
| D2 | L2 content: keep today's screens (Home shelves, library grid, sub-screens) under the depth-1 spine, or adopt dirD's focused item + facts + receding queue? | Keep today's screens for now; dirD's L2 becomes a later stage. It keeps README §8.1 ("no functional behaviour changes") true for as long as possible. | S1, S5 |
| D3 | ✕ opens / never plays, and △ = trailer. This IS a behaviour change, against §8.1. The client has no trailer support; Jellyfin's remote trailers are YouTube URLs the PS3 cannot play, so only local trailers could work. | Adopt ✕-opens. △ = local trailer when one exists, otherwise hidden from the hint bar (never shown disabled, matching the HUD's stats-hint rule). | S3 |
| D4 | The wave. The §2.9 table says bottom 190 / 228 / one third; the paragraph below it says "the wave does not move between levels". | Doesn't move — the paragraph explains *why* (it is the reference everything moves through). Treat the table row as a mistake. | S4 |
| D5 | Where do TV seasons → episodes, collection contents and music drill-down live, given "no fourth level"? | L2, as today, with the breadcrumb. The spine stays at depth-1 geometry. | S1 |
| D6 | Search and Settings at L1: what is their column? | No column; Down / ✕ goes straight to the L2 screen. | S4 |
| D7 | L1 needs a column for every library before it is visited. | Lazy: fetch on first focus at L1, the same as the tab fetch today, with the column drawn as skeletons until it lands. Prefetching all libraries at boot costs one request per library on a slow first launch. | S4 |
| D8 | "Tips further back as they descend": a perspective tilt. A trapezoid drawn as two textured triangles warps affinely (visible seam), and §4.2 bans 3D tilt on focus. | Approximate with scale + alpha falloff down the column. Revisit only if it reads flat on a TV. | S4 |

README §3.1 and §8.3 Phase 2 still specify the tab strip (stride 110, icon 26,
label active-only). §2.9 supersedes them but was not applied to them. The
upstream README should be fixed in Claude Design so the next export does not
reopen this.

---

## 2. Architecture

**`source/ui/render/spine.h` — header-only pure C, host-tested.** The same
pattern as `wave_gel.h`: all geometry, no drawing.

- `spine_level_t` holds the per-level triples above as a table, in 1280×720
  authoring units. It is the ONE place D1 is answered.
- `spine_ease(t)` is slow-in/slow-out. Use smoothstep or cubic in-out; no
  overshoot, so the output is monotonic and clamped to [0, 1].
- `spine_eval(depth_f, &out)` lerps every triple at a float depth in [0, 2].
  The "not drawn" divider at L1 becomes alpha 0, not a special case.
- `spine_slot_x(i, active_i)` = `active_x + (i - active_i) * stride`. Slots left
  of the active one can go negative (the row is cropped at the left edge by
  design).
- `spine_idle_alpha(dist)` = the 5-step falloff; 0 beyond distance 5.

**`source/ui/render/ui_spine.cpp` — the draw code.** It replaces
`xmb_draw_tabs()` when the gate is on.

- **GPU phase (before `rsxSync`):** the glow band and the two-stage bloom, as
  blended fans. Reuse `wave_draw_glow_gpu()` from the 09-22 WIP (saved in
  `outputs/wip-2026-09-22-full.patch`), widened to separate `rx`/`ry` for the
  ellipse. 14 vertices per fan, so three fans cost nothing.
- **Text phase:** icons via `drawIcon()` and the active label via
  `drawTTF_tracked()`. Both are cached runs, so a settled spine uploads nothing.
- The underline is an opaque `drawRect` (write-only, fine).
- Everything goes through `UIS_W` / `UIS_H` / `UIS_TF` plus `XMB_OX` / `XMB_OY`.
  `test_layout`'s `test_one_scale()` must cover the new constants.

**Navigation state:** one `int g_depth` target (0..2) and one `float g_depth_f`
that eases toward it. Every consumer reads `g_depth_f`, never a per-screen
flag. `xmb_nav_depth()` from the WIP is the seed of this, but it conflates
"inside a TV series" with "at L2". Under D5 those are different things.

**Chrome anchors become depth-driven.** `XMB_DIVIDER_Y` / `XMB_CONTENT_Y` are
used in 7 files (21 uses). Make them read the evaluated spine rather than
constants. `test_layout` must then assert the fit at each level, not once.
**Consequence to flag early:** at L2 the divider moves from 144 to 214, which
takes 70px (at 720) from the 2-row library grid, so cards shrink by ~20%. If
that reads badly, it is an argument for dirD's single-row L2 (D2).

**Gate:** `/dev_hdd0/tmp/jellyfin_spine.txt`, read in `main.cpp` after
`plog_load_setting()`. 0 or missing = the current tab strip, 1 = the spine. Pair
the plog with a `crash_log()`. The spine binds no new GPU state (fans on the
existing wave program), so a bad value costs a relaunch, not a power cycle.
It is still gated because it changes every screen.

---

## 3. Stages

Each stage ships alone, gated, and is measured against the `xmb:` line
(`bpx` must not move; `tm`/`tkb` must settle to 0).

**S0 — DONE 2026-09-23.** `source/ui/render/spine.h` + `tests/test_spine.c`
(in `Makefile.host`'s `all`; on this PC run it with `mingw32-make -B -f
Makefile.host test_spine`, then `./test_spine.exe`). Passes with `-Wextra`,
compiles as C++, and four planted faults (a table typo, a removed clearance
factor, an overshooting ease, a swapped falloff) each fail it. It found two
things:

- **The divider cut through the label on L1→L2.** It fades in at y=214 while
  the underline is still sliding up past that line. Its alpha is now scaled by
  the underline's clearance, so it first shows at depth 0.913 and settled
  screens are unchanged.
- **The L1 poster moves down in S4.** With the README-size spine, the L1
  underline ends at y≈303, and dirE's poster top is y=300 (drawn for its
  smaller spine). Put the L1 art at ≥ ~311.

Label and underline offsets and the glow ellipse came from dirD by script,
because the README omits them.

**S0 — geometry only, host (original brief).** Write `spine.h` + `tests/test_spine.c`:

- triples at exactly 0/1/2 equal the table;
- easing is monotonic with no overshoot;
- the falloff table is right;
- all 7 slots are on-screen at 720p and 1080p for D1's numbers.

No console. *Waits on D1; can start with the README numbers.*

**S1 — BUILT + DEPLOYED 2026-09-23, awaiting a TV check.**

- **Files:** `render/ui_spine.{h,cpp}`. `drawIconA()` in `ui_text.cpp` gives
  real icon opacity through the run colour's top byte (0 = unchanged for every
  existing caller). `wave_draw_glow_gpu()` now takes separate rx/ry.
  `XMB_DIVIDER_Y` is gated in `ui_visuals.h`.
- **Idle colour:** derived as 25% `hairline` → `icon_idle`, not hard-coded.
- **Hints:** unchanged. ✕ still does what it did, and there is no options
  cross yet, so §2.9's `✕ Open · ▫ Options` would describe buttons that do
  not exist.
- **The bloom:** not measured, because the design gives no numbers. It is two
  round fans (radius 1.0 and 0.625 × the active icon, alpha 36 and 64);
  judge it on the TV.
- **test_layout:** gained a spine pass. It reports the design's cost as notes:
  2 list rows (not 3) with overscan at 720p/480p, the farthest slot skipped
  under overscan, and the left neighbour skipped at 576i.
- **Build:** 109 objects, 48 warnings (none new).
- **Deployed:** `outputs/EBOOT.BIN.spine1` (sha 7be16ba3…), with
  `jellyfin_spine.txt=1`. Rollback: the gate to 0, or
  `outputs/EBOOT.BIN.pre-spine1`.
- **Not committed:** the glow function is interleaved in `ui_wave.cpp` with
  the untested JellyWave safety work, and that deploys with it.

**S1 on hardware (2026-09-23):** the spine renders. The user's verdict: "looks
better, but it is still not multiple layers". That moved the base layer forward.

**Base layer (S4, first cut) — BUILT + DEPLOYED 2026-09-23 as
`outputs/EBOOT.BIN.spine2`, awaiting a TV check.** The navigation follows
1etu/XMP (MIT, `packages/xmb-plugin/src/nav.ts`):

- **At base:** Left/Right and L1/R1 walk the tabs (clamped, not wrapped).
  Down or ✕ enters a tab.
- **Inside a tab:** Up from its top row, or ○ at its root, returns to base.
  Search leaves on Up from the keyboard's top row only, because ○ already
  means "clear" there. Sub-screens keep their own Up and ○.
- **Drawing at base:** the spine at y=232, no divider, and a column under the
  active icon. The focused poster is 128×192 at y=318, not dirE's 300. The
  next item is shown at 0.72× and dimmed by a GPU rect. The title block
  (eyebrow, 30 px display title, meta) sits beside the poster.
- **Motion:** the depth is latched once per frame. The spine moves over
  280 ms, content cuts at depth 0.5, and the divider fades in once the label
  has cleared it.
- **Not done:** the column sinking behind the wave (the wave's first draw is
  an opaque full-screen gradient), scrolling the column at base, and the
  L2→L3 item level.

**spine2 on the TV:** the user reported base-layer posters never loading
(they load in L2), motion "way too static", the A–Z rail tiny, and the whole
thing not elegant.

**spine3 — BUILT, NOT DEPLOYED (the user said to hold).**
`outputs/EBOOT.BIN.spine3`, sha ab94eb52….

- **Motion:** the XMB's approach curve (`spine_approach_*` in spine.h, gains
  from XMP's verified table, frame-rate independent, host-tested at 60, 36 and
  20 fps). The row position glides in slots; icon size, colour, falloff,
  labels and underline are continuous in the fractional distance. The depth
  approaches over 320 ms. The column slides and fades with its category and
  lifts out on entering a tab.
- **Posters:** the column now requests the tab's OWN grid or Home-row card
  size, and draws it scaled with a new LINEAR-filtered
  `ui_card_gpu_draw_scaled`. The two levels share cache slots, so L1 loads
  exactly when L2 does. The root cause of spine2's misses is not proven: the
  size budget (~67k px), the VRAM slot size and the fetch buffer all check
  out, and the spine2 log was overwritten.
- **A–Z rail:** fixed pitch (13 px type on a 17 px pitch, README 3.3 plus one
  step) instead of two grid rows. Colours follow README 3.3 (idle
  `text_faint`, current section `text`, jump cursor `accent_alt`); every
  letter used to be `hairline`.
- **Still static:** L2 content cuts in at depth 0.5, and focus movement
  inside the grid and Home shelves snaps (README 4.1 asks for 120 ms).

**spine4 — BUILT, NOT DEPLOYED.** `outputs/EBOOT.BIN.spine4`, sha
c66907d1…, 48 warnings. It fixes the two "still static" items above. All of it
is inert with the gate off.

- **Content glide:** `spine_content_dy()`. The tab's content rises the last
  48 px into place over the second half of a level move. `XMB_CONTENT_Y` is
  the drawn position; `XMB_CONTENT_Y_REST` / `XMB_GRID_Y0_REST` feed every
  SIZE (card height, visible rows, grid space, Home card height, the A–Z
  pitch), so nothing resizes and no thumbnail is refetched mid-glide.
  `test_layout test_glide_sizes()` guards that.
- **Focus ring:** `spine_focus_ring_gpu()` replaces `ui_card_gpu_selection`
  in the grid and on Home. It approaches the focused card's drawn rect (200
  ms, the XMB value), and snaps when the screen changes or a frame was
  missed.
- **Grid scroll:** `spine_grid_motion()` plus a draw window in `ui_lists.cpp`
  (`xmb_grid_motion`). Rows ease up and down under a clip to the grid band.
  The scrollbar keeps the logical position. A page slide (a jump bigger
  than a screen) snaps.
- **Home scroll:** eased copies of `s_vscroll` and each row's `scroll`. A
  card sliding off an edge keeps its GPU image, while its CPU rects and text
  drop the moment it is partly off screen, because they take unsigned x.

**spine5 — BUILT, NOT DEPLOYED.** `outputs/EBOOT.BIN.spine5`, sha
7c49ebf5…, 48 warnings. It works from the CURRENT canvas: `design-import-v3/`,
measured in `design-import-v3/MEASURED-detail-hud.md`. Everything is behind
the spine gate; with the gate off, the old detail page and HUD run unchanged.

- **A–Z rail:** with the spine on, it is a 15-letter window centred on the
  current letter, at 17/24 px authored. The window glides and fades toward
  its ends. This is a deliberate departure from the static 27-row rail,
  because under the spine the static rail is capped at ~19 px letters.
- **Item detail (L3):** `xmb_show_item_info_v3` in `ui_info.cpp`.
  - The backdrop and poster are uploaded to VRAM once
    (`ui_gpu_tex_upload`, card-gpu slots 0 and 1) and drawn scaled, LINEAR.
    The scrims are GPU ramps (`wave_draw_ramp_gpu`); chips, buttons and
    selectors are rounded fans (`wave_draw_rrect*_gpu`).
  - All text goes through the run cache, and the focus ring glides between
    controls.
  - Controls: Resume / Play from start / Mark as watched (a new POST to
    `/Users/{u}/PlayedItems/{id}`), Version, Quality.
  - The spine rises to L3 on entry and returns to L2 on exit.
  - Dropped relative to the old page: More Like This, the tagline and the
    facts rows. The canvas has none of them.
- **Player HUD:** `hud_body_v3` in `player/hud/hud_v3.inc`, on the same
  compose-on-change overlay. It has the scrim, the 30 px title, audio / vol /
  subs chips (which are also the old focus slots), the full-width progress
  bar with times, the audio-path pill at top right, and the hints.
  - New buttons with the gate on: △ Tracks, ○ Stop (new `HUD_ACTION_STOP`),
    □ Stats (toggles `statsovl`).
- **Not done:** Home's L2 "queue recedes" layout (00 · Depth model, L2
  frame), and the L1 column's tipped items drawn before the wave. (Home's L2
  queue is done in spine6.)

**spine6 — BUILT, NOT DEPLOYED.** `outputs/EBOOT.BIN.spine6`, sha
91f830e8…, 111 objects, 48 warnings (a clean rebuild), header 0480. This is
spine5 plus Home's L2 queue. Host tests pass: test_facts (new), test_spine,
test_layout and test_media_sources.

- **Home stage** (`xmb/ui_home_stage.inc`, included by `ui_home.cpp`). With
  the gate on, Home is ONE stage drawn at every depth. Each item's box is
  the depth-weighted blend of its L1 column slot and its L2 queue slot, so
  the column swings into the queue (the canvas: "swings from vertical to
  horizontal rather than cross-fading").
  - Canvas numbers: the focus 200×300 at x=64 with a 46 px reflection; the
    queue at 104/92/80/70 wide at .62/.48/.36/.26 on y≈552; the facts column
    520 wide at x=304 (eyebrow + "3 of 12", 36 px title, meta, CONTAINER /
    VIDEO / SUBTITLES, AUDIO chip + "+ other track", progress, "WATCHED ·
    LEFT").
  - Motion: Left/Right slides the queue, and Up/Down changes category with a
    vertical glide. Each row remembers its position. Empty rows are skipped.
- **Decided here:**
  - Posters in every row, with episodes shown by their SERIES poster and
    name ("S1 E4 · name").
  - The halo follows the item nearest the focus.
  - The facts fade while the queue moves.
  - Square rows are 1.2× wide, and the queue shifts right to clear the
    facts.
  - Coming back from detail, the queue settles 40 px into place.
- **True translucency:** `ui_card_gpu_draw_ex`, with constant-alpha blending
  via `rsxSetBlendColor` (NEW RSX STATE, unproven on hardware), plus UV
  flips for reflections, drawn as 6 strips.
- **Facts worker** (`api/api_facts.cpp` + `api/facts_parse.cpp`, host-tested):
  - fetches `Items?Ids=…&Fields=MediaStreams` on its own thread and its own
    buffer, 12-entry cache;
  - words: "DTS-HD MA 5.1", "TRUEHD ATMOS 7.1", "4K HEVC HDR10", "PGS ENG";
  - also feeds the detail page's audio / video chips, whose blue outline is
    now lossless-only.
- **Thumbnail slots** grow to fit the queue's source size
  (`xmb_queue_src`), capped at 120 k px (about +6% over the ~113 k px a 1080p slot holds today).
- **X is detail's only door (the canvas's Resolved box):**
  - X on the base layer opens the column's item (Down still enters);
  - X / △ in the queue open detail;
  - detail hands back the level it was opened from.
- **Home no longer refetches** Continue Watching / Next Up every time the
  spine passes over it. It refetches only when `g_play_gen` changes (a
  playback start or Mark as watched).

**S1 — the spine at L2, static (original brief).** Every existing screen draws the depth-1 spine
instead of the tab strip:

- divider at 214 and content anchors moved;
- glow band + bloom;
- hints per §2.9 (`✚ Nav · ✕ Open · ▫ Options · ○ Back`), but ✕ keeps its
  current meaning until S3, so the ✕ label stays "Select" until then.

This is the first thing to see on a TV. *Waits on D1, D2, D5.*

**S2 — L3 in item detail, cut.** `xmb_show_item_info()` runs its OWN frame loop
(own `waitflip`/`flip`), so it must draw the depth-2 spine and the divider at
144 itself, in its own GPU and text phases. Transitions into and out of it cut
for now. Check that the detail layout (poster top 172) clears a spine at y=74.

**S3 — ✕ opens, never plays.**

- Every grid/shelf ✕ that calls `xmb_play_*` (`ui_home.cpp:401/403`,
  `ui_nav.cpp` TV episodes ~278, collections ~348) opens detail instead.
- △ becomes trailer-or-nothing.
- Detail's own ✕ keeps playing.

This is a behaviour change: its own commit, done after S2 so detail already
looks right. *Waits on D3.*

**S4 — L1, the new screen.**

- The spine at y=232 as content, no divider.
- The vertical column under the active slot, with the focused art 128×192 at
  the intersection, fixed. The column slides underneath it.
- The label block beside it: eyebrow + title 30 + meta.
- Left/Right changes category, Down → L2, ✕ → detail.

Draw order changes: the column's lower half must be issued BEFORE `wave_draw()`
so the ribbons pass over it. Today `wave_draw()` runs straight after
`clearScreen()`, ahead of the whole GPU phase. That is a real reordering of a
load-bearing sequence; do it behind the gate and check that `sync` does not
move. *Waits on D4, D6, D7, D8.*

**S5 — transitions.**

- `g_depth_f` eases over 280 ms.
- The spine slides and shrinks.
- L1→L2 swings each card's rect from its column slot to its grid/shelf slot, as
  a lerp of card quads on the GPU.

Risk: animating type sizes makes every in-between frame a text-cache miss
(runs are keyed on size). Measure `tm`/`tkb` during a transition. If uploads
spike, draw text at the endpoint size and cross-fade instead of scaling it.
L2↔L3 stays a cut until detail is folded into the main loop, which is a separate
refactor and not part of this plan.

**Later, not planned here:** dirD's L2 layout (if D2 says so), and folding
detail into the main loop so L2↔L3 can animate.

---

## 4. Budget

The spine itself is cheap:

| item | cost |
| --- | --- |
| icons + label | 8 cached runs |
| glows | 3 fans × 14 verts |
| underline | 1 opaque rect |

It adds nothing meaningful to a frame. The real frame problem is elsewhere:
JellyWave mode 3 measured 19.7–22.5 ms on hardware (7.4 ms of PPU geometry per
rebuild, amortised by `jellyfin_jwrebuild.txt`). Measure spine stages against
**mode 2**, so their cost is not buried in the wave's.

The only spine stages with real risk are:

- **S4:** the draw reordering;
- **S5:** text-cache churn during a transition.

---

## 5. Housekeeping before S1

- The 09-22 working tree mixes two things. The JellyWave safety/upload work
  (`ui_wave.cpp`, `wave_gel.h`, `test_wave_gel.c`) is consistent with the new
  ribbon's "alpha blend only" and should be committed on its own once built. The
  tab-strip WIP (`ui_widgets.cpp`, `ui_xmb.cpp`, `ui_visuals.h`) matches neither
  the old strip nor the spine and should be restored to HEAD. Its glow function
  lives in `ui_wave.cpp` and survives. Full backup:
  `outputs/wip-2026-09-22-full.patch`.
- Rewrite or retire `UI-HANDOFF.md` so the next session does not plan from it.

---

## spine7 — the depth engine (2026-09-23), BUILT, NOT DEPLOYED

**Where it lives now.** Everything above spine1 was only ever an uncommitted
working tree in the OneDrive fork (branch `feature/spine6-strobe-integrated`),
built on 3e86481 without the 50 JellyWave strobe commits. It is now committed
on **`feature/xmb-spine`** in the WSL repo, worktree **`~/jf-spine`**, based on
428d54a (the strobe line, same base as `feature/cold-boot-animation`):

| commit | what |
| --- | --- |
| 7f7b903 | S0 (`spine.h`, `test_spine.c`), cherry-picked |
| 3cbab08 | spine1–6 ported unchanged in behaviour |
| f95e618 | the depth engine (this section) |

The OneDrive working tree was not modified. **The JellyWave renderer is not
touched**: spine6's own 09-22 JellyWave safety edits (`ui_wave.cpp`
`jw_upload`/`s_jw_stage`, `wave_gel.h` sanitize, `test_wave_gel.c`) were left
out, and the strobe line's renderer stands. The spine's immediate-mode helpers
(elliptical glow, rounded rect, alpha ramp) moved to a new
`render/ui_wave_panels.cpp`; `ui_wave.cpp` only gained `wave_imm_bind()`
appended at the end of the file — the program + blend preamble its own divider
and glow already use. No vertex buffer, upload, fence, draw count, primitive or
blend-state change to the wave.

### Audit of spine6 (what was real, what was placeholder)

| piece | state found |
| --- | --- |
| `spine.h` level table, eval, approach motion, falloff | real, host-tested |
| spine row, glow band, bloom, labels, underline | real |
| level motion, content glide, focus-ring glide, grid scroll easing | real |
| Home stage (column → queue swing, facts, reflections, translucency) | real, but its geometry and state were Home-only file statics in `ui_home_stage.inc`, untested |
| library L1 column | **placeholder**: two opaque posters, always the library's FIRST item, a dimming rect instead of opacity, cut to the grid at depth 0.5 |
| per-category focus | Home rows only; `xmb_switch_tab()` zeroed every library's selection |
| L2 → L3 | the level move started before detail's blocking fetch, so its first frame carried the whole fetch as dt and the spine landed in one step |

### What the engine is

**`render/depth.h`** — pure C, host-tested, no display/theme/RSX/allocation.
Every item is placed by its distance k from the category's focus; every layout
is `k -> depth_box` (position, size, opacity, veil, reflection):

- `depth_column_slot` — L1 column (128×192, 106×159, 86×129, colder veils).
- `depth_queue_slot` — Home's L2 queue (spine6's numbers, unchanged).
- `depth_grid_cell` — a library's L2 grid cell, exactly as `grid_cell_pos()`.
- the swing: `depth_column_queue_box`, `depth_column_grid_box`, `depth_swing`.
- `depth_rows` — the category glide; `depth_step_visitable` /
  `depth_nearest_visitable` / `depth_at_top` — empty categories skipped.
- `depth_focus_mem` — one fixed slot per category, clamped on restore.
- fades: `depth_l1_label_alpha`, `depth_l2_facts_alpha`, `depth_over_l2`,
  `depth_queue_settle`, `depth_halo`; layouts: `depth_l1_label_layout`,
  `depth_facts_layout`; `depth_mix_q` (16-step colour, run-cache friendly);
  `depth_xform` (matches UIS_W/UIS_H + overscan); `depth_refl_clamp`.

**`render/ui_depth.cpp`** — one stage renderer. A category hands it an index
range and a callback (index → `DepthCard`); it draws halo glow behind, cards
far to near with veil + reflection, bands + progress over, and notes the focus
rect. Only existing primitives: `ui_card_gpu_draw_ex` (constant alpha),
`ui_rect_gpu_draw`, the immediate-mode fans. No VRAM reads, no allocation (the
card list is a fixed stack array), no vertex buffers.

### What uses it

- **Home** (`xmb/ui_home_stage.inc`): now resolves its cards through the
  engine. `test_depth` carries spine6's `q1_slot`/`q2_slot`/`q_box` verbatim
  and asserts the engine returns the identical box for every slot, shape and
  swing — the move changed no geometry.
- **Movies / Shows / Music / Collections / Playlists**
  (`xmb/ui_depth_lib.cpp`, new): Home's L1 column (remembered focus + two
  receding items, translucent, halo, label block) and a **continuous swing
  into their real grid cells**; the rest of the page unfolds out of the column.
  At `DEPTH_HANDOFF_E` (0.999: every card within ~1 px of its cell at 1080p)
  the grid takes the screen back and draws exactly as before — titles,
  scrollbar, A–Z, paging, focus ring. Going up, the stage takes over on the
  first frame the swing leaves 1. The content glide is off for these tabs
  (`depth_stage_tab`): the swing is their motion. Sub-screens (seasons,
  collection contents, music drill-down) swing the same way.
- **Decided: libraries keep the grid at L2** (the canvas's "02 · Library tab"),
  not Home's queue — the grid carries paging, A–Z and sort for large libraries.
  The engine makes this one callback, so a queue L2 for libraries later is a
  layout swap, not a new screen.
- **Focus memory**: `xmb_switch_tab()` calls `spine_focus_leave/enter`; each
  library remembers `g_sel`/`g_scroll_top` (forgotten when its paged window or
  filter is dropped). X on the base layer opens that item (was item 1).
- **Thumbnails at L1**: the column's three always; the rest of the page only
  once the row has settled on the category or the swing has started — walking
  the spine costs three fetches per library, not a page.
- **L2 → L3**: detail starts the level move after its first fetch
  (`spine_clock_reset`), flies its poster out of the card it was opened from
  (`depth_last_focus_rect`, noted by the stage or the focus ring) and fades its
  backdrop in (`ui_gpu_tex_draw_a`). Closing hands back the level it came from;
  Home's queue settles 40 px, a library returns to its grid with its focus.

**Found and fixed by the new test_layout depth pass:** under 8% overscan at
720p and 480p, spine6's queue-focus reflection ran up to 30 px into the hints
bar (the stage is pushed down by the inset while the hints bar is pushed up).
Reflections now end at the hints bar; nothing changes without overscan.

### Tests and build

- `test_depth` (new): 6,360 checks — spine6 equivalence, swing endpoints, grid
  hand-over ≤ 1.5 px at 1080p, fit at 720p/1080p, rows, skip-empty, focus
  memory, fades, colour quantisation, transform, reflection floor.
- `test_layout`: new depth pass through the real `ui_visuals.h` anchors at 7
  resolutions/overscans.
- Host suite: **32/32 pass** after `make -B`. `make check` itself cannot
  succeed on this branch **or its base**: `test_bg_gradient.c`,
  `test_month_bg.c` and `month_bg_table.h` are named by `Makefile.host` but
  were never committed anywhere. Use `~/run_host_spine.sh` (runs each test on
  its own). `check` now also runs `test_spine`, `test_depth`, `test_facts`.
- PS3 clean serial build: **115 objects, 48 warnings** — the pre-existing
  census, none in new or edited files; new files also clean under `-Wextra`
  with the real PSL1GHT headers. NPDRM header `0480`. Not deployed.

### Needs a TV (nothing from spine3 on has been seen on hardware)

1. **Constant-alpha card blending** (`rsxSetBlendColor`, spine6): every
   translucent poster, veil-free fade and reflection depends on it. New RSX
   state, unproven.
2. The library **L1 column and swing**, and the **hand-over**: titles,
   scrollbar and A–Z appear at the hand-over rather than fading — judge it.
3. **Frame cost** of the swing (up to ~16 translucent quads + halo) against the
   `xmb:` line — `bpx` must not move, `tm`/`tkb` settle to 0. Measure against
   JellyWave mode 2.
4. **Focus memory** across Left/Right at L1 and L1/R1 at L2.
5. The **detail arrival**: poster flight timing, backdrop fade.
6. Overscan: the shortened reflection at 720p/480p.
7. Everything spine3–6 already listed (approach feel, A–Z window, v3 detail,
   v3 HUD, facts worker).

### Still open

- Tipped L1 items (11° / 19°) — scale + veil stand in (D8).
- The column drawn BEFORE the wave — blocked while JellyWave's strobe work is
  open (reordering its first draw is off limits).
- Grid chrome (titles, scrollbar, A–Z) fading in at the hand-over instead of
  appearing.
- Detail still runs its own frame loop; folding it into the main loop is what
  would let L3's text arrive with its poster.
- Search and Settings have no column (D6); the ▫ Options cross does not exist.
- Library L2 as a queue, if the grid reads wrong under the spine (engine-ready).
- Merging: `feature/cold-boot-animation` hooks `ui_run_xmb` and
  `xmb_draw_topbar`; this branch changed the three XMB phase functions and made
  `xmb_grid_view()` public. The strobe session's uncommitted `ui_wave.cpp`
  overlaps only at end-of-file (`wave_imm_bind`).
