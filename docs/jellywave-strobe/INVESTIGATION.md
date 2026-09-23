# JellyWave big strobe: forensic record

The big strobe is the only subject of this record. The tiny top-edge flicker
is a **separate bug**. It is present on the golden control too, so it is
recorded in every row and investigated nowhere here.

Method: find the first real difference between the reproducibly good binary
and the reproducibly bad one by controlled comparison. No change counts as a
fix until it has been bisected, rebuilt from source, and shown clean across
multiple cold launches on the same console.

---

## Current control

| | artifact | hardware result | source |
|---|---|---|---|
| **GOOD** | Run-9 EBOOT, SHA-256 `75ae4602…` | big strobe **absent** on every clean relaunch; top flicker present | user hardware report, 2026-09-23 |
| **BAD**  | EBOOT SHA-256 `8fd2b538…` ("gradient last + 1024 vertex / 24 KB pad") | big strobe **present** | user hardware report |

Neither binary, nor the source trees they were built from, is in this
repository (see P1). Both exist only on the build machine.

---

## What is proven

These are proven from source, from the repository history, or from host
experiments that anyone can re-run with `tools/strobe/`. None of them
depends on a single hardware run.

**P1. Run 9 cannot be recovered from GitHub.**
There is no branch, tag, pull request, Actions artifact, commit message,
file content, or blob in any ref that mentions Run 9, `75ae4602`, or
`8fd2b538`. The newest JellyWave commit here is `428d54a` (2026-09-23 00:46).
Phase 1 therefore has to run on the build machine:
`tools/strobe/provenance.sh`.

**P2. An EBOOT's SHA-256 identifies a deployed file, never a build.**
`make pkg` runs PSL1GHT's `make_self_npdrm` (geohot, `tools/geohot/make_self.c`).
It seeds its RNG with `time(NULL)`, then deflates each ELF segment and
encrypts it with AES-128-CTR under freshly random keys. This was reproduced
here with the real tool, built from PSL1GHT source: one ELF packaged twice
gave EBOOT SHA-256 `4af59494…` and `ddbe34d5…`. The ELFs recovered from those
two files (`self2elf.py`, every segment HMAC-verified) are byte-identical to
each other and to the input in every loaded byte.

Consequences:
* Rebuilding the Run-9 source will **never** reproduce `75ae4602`, even with
  an identical toolchain.
* "Same build?" is answered by **LOADID**, a hash of the loaded bytes
  (`loadid.py`), or by `elfcmp.py`. It is never answered by the EBOOT hash.

**P3. `__DATE__` is compiled in** (`source/ui/render/ui_settings.cpp:274`).
A rebuild on a different day differs in 11 bytes of `.rodata`, with no
change in size or layout, unless `SOURCE_DATE_EPOCH` is pinned.
`hwtest.sh` pins it to HEAD's commit time.

**P4. VRAM layout, measured with PSL1GHT's own allocator**
(`ppu/librt/heap.c`, compiled unmodified; `tools/strobe/vram_map`):
* `rsxMemalign` carves each allocation from the **top** of the first free
  block that fits, so buffers run downward from the end of local memory in
  call order.
* Every allocation has a 16-byte heap header **in VRAM** directly below it.
  Every `rsxMemalign` or `rsxFree` is therefore a PPU read-modify-write of
  local memory next to live buffers.
* With the tip's init order (`init_screen`, `card_gpu`, `text_gpu`,
  `wave_init`, `thumb_cache`), `wave_vbuf1` lies directly **above
  `thumb_slot0`** (128-byte gap) and directly below `wave_vbuf0` (32-byte gap
  holding vbuf0's header). `wave_vbuf0` lies below `wave_fp`.
* Adding 24 KB to each vertex-buffer allocation (the `8fd2b538` pad, if it
  was applied to the allocation) moves `wave_vbuf0` down 0x6000 and
  `wave_vbuf1` plus every thumbnail slot down 0xC000. It changes **no**
  adjacency: thumb slot 0 still ends 128 bytes below `wave_vbuf1`.
* Absolute offsets depend on `gcmConfiguration.localSize` and on the
  fragment-program sizes. Log them rather than infer them.

**P5. Tiled and Z-cull regions are ruled out.** Nothing in `source/` calls
`gcmSetTileInfo`, `gcmBindTile` or any Z-cull setup, so no tile or
compression region can alias the vertex buffers.

**P6. The committed bad-state log is phase-locked to vblank.**
`player_log.txt` at `428d54a` comes from a build between `d8bff08` and
`428d54a` (main-memory staging, write-only upload, `repaired=0 dropped=0`,
8740 verts, 7 draws). Over the whole log (136 samples, each averaging 60
frames) `logstats.py` gives: `sync` median 13.6 ms with p10–p90 of
13 586–13 621 µs; `frame = 33.36 ms`, exactly two vblanks; `vsync = 9.55 ms`;
`gpu = 3.3 ms`.
In that 30 Hz timeline, the frame's `rsxSync()` returns ≈ 0.4 ms **after the
next vblank** (3.3 gpu + 0.2 other + 13.6 sync ≈ 17.1 ms, against a 16.68 ms
period). This is an **observation, not a conclusion**. It fits either ~10 ms
of extra RSX work or an RSX wait released by a vblank. Only measurement M1
below tells the two apart.

**P7. The "known good" reference in `9d2fec3` is not reproducible from Git.**
The JellyWave commit `9d2fec3` includes `bg_gradient.h` and `month_bg.h`, and
its `tests/Makefile.host` builds `test_bg_gradient`, `test_month_bg` and
`month_bg_table.h`. None of those files was ever committed. `bg_gradient.h`,
`month_bg.h` and `month_bg.cpp` were written afresh 36 minutes later
(`6abea97`, `0e73b40`, `ccaa592`), with `month_bg_current()` stubbed.
So the tree whose first hardware run is quoted in `9d2fec3`
(`sync` 3 375 µs, frame 21.3 ms) is **not** a Git state. The recreated
headers are pure and deterministic; that is noted here as a provenance gap
only.

### Latent defect found in passing: not causal until measured

`thumbnail_cache.cpp` admits a request by **pixel count**
(`w*h <= s_max_px`) but copies `VRAM_PITCH(w)*h` **bytes** into a slot of
`s_vram_bytes` (`thumb_gpu_texture`). If a `(w, h)` pair's 64-byte row padding
works out worse than both card geometries' padding, it writes past its slot.
Past slot 0 is `wave_vbuf1`'s header and first vertices (P4).

Nobody has measured whether such a request happens on Home. The tip rewrites
the whole vertex buffer on every call, so a one-off overrun cannot by itself
produce a constant 13.6 ms `sync`. This is **not** a candidate fix.

---

## Test table

The live table is written by `tools/strobe/hwtest.sh` to
`~/jw-strobe-archive/TESTS.md`, one row per cold launch plus a summary row.
Copy it here when committing results. Rows so far:

| TEST | BUILD / EBOOT SHA | LOADID | CHANGE | STROBE | TOP FLICKER | SYNC | FRAME | RESULT | evidence |
|---|---|---|---|---|---|---|---|---|---|
| R9 | `75ae4602…` | *not yet taken* | golden control, untouched | absent, multiple cold launches | present | *read its log* | *read its log* | **CLEAN** | user report |
| PAD | `8fd2b538…` | *not yet taken* | gradient last + 1024 vertex / 24 KB pad | present | ? | ~14 ms (user) | ? | **STROBE** | user report |
| LOG-0922 | unknown (build between `d8bff08` and `428d54a`) | – | staging + write-only upload + `rsxSync` per call | ? | ? | 13.6 ms | 33.36 ms | failure range | committed `player_log.txt` |
| JW-first | pre-`9d2fec3` tree, not in Git (P7) | – | first JellyWave hardware run | ? | ? | 3.375 ms | 21.3 ms | reference | `9d2fec3` message |

---

## Current experiment and next controlled steps

Each step answers one question, in order. Every step uses `hwtest.sh`, so each
result lands in the table with EBOOT SHA, LOADID, console-config fingerprint,
log and stats. Use **3 cold launches per build**. A MIXED result is new
evidence that relaunches are not deterministic, and it stops the bisect.

**E0: Where did Run 9 come from?** (Phase 1)
Run on the build machine, with `PSL1GHT` exported:
```
tools/strobe/provenance.sh
```
*Expected differentiator:* the `WANT 75ae4602` block lists the untouched EBOOT
file(s), their LOADID, every ELF on disk with the **same** LOADID (that
ELF's build tree is the Run-9 tree), and the reflog around the EBOOT's mtime.
Each worktree's `*.dirty.diff` holds any uncommitted source that went into it.
Freeze that exact state on a branch before doing anything else, e.g.
`forensic/run9-exact`, with the dirty diff committed.

**E1: Is the recovered source really Run 9?** (Phase 2)
```
git checkout forensic/run9-exact && make clean && SOURCE_DATE_EPOCH=<..> make pkg
tools/strobe/loadid.py <untouched Run-9 EBOOT> obj/<dir>.elf
tools/strobe/self2elf.py <untouched Run-9 EBOOT> -o run9
tools/strobe/elfcmp.py run9.elf obj/<dir>.elf --syms-b obj/<dir>.elf.map
```
*Expected differentiator:* LOADIDs equal means the source is proven to be Run 9.
If they differ, `elfcmp` names every differing section and symbol. A diff
confined to the `__DATE__` string is benign (P3). Anything else is a real
difference, and Phase 2's list applies: compiler, flags, link order, and
generated files. The toolchain identity is in each test's `toolchain.txt`.

**E2: Both controls on today's console config** (Phase 3)
```
tools/strobe/hwtest.sh R9-ctl  "golden Run-9 EBOOT, untouched" --eboot <run9 copy> --expect-sha 75ae4602
tools/strobe/hwtest.sh PAD-ctl "8fd2b538 bad control"          --eboot <pad copy>  --expect-sha 8fd2b538
tools/strobe/hwtest.sh R9-src  "Run-9 source rebuilt"          # from forensic/run9-exact
```
*Expected differentiator:* STROBE yes/no, plus SYNC (GOOD range vs ~13.6 ms)
from each launch's log. `R9-src` must match `R9-ctl`. If it does not, the
difference found in E1 is the lead, and bisecting source is premature.

**E3: The smallest change that flips CLEAN to STROBE** (Phase 4)
Order the logical changes between `forensic/run9-exact` and the bad tree
(`git diff --stat`, one change per file or hunk group, unrelated work kept
apart) and binary-search them. Run one `hwtest.sh` per step with a single
logical change applied. When one change flips CLEAN to STROBE, freeze it.
Then apply only the Phase-5 measurements (M1, layout log) to **both** sides
of that pair.

### Phase-5 measurements (apply identically to GOOD and BAD)

**Layout log.** At init, log every `rsxMemalign` result as offset and size,
plus the per-frame `vo`, first and count of each JellyWave draw for the first
few frames. Compare with `vram_map` and between the pair.

**M1: RSX work or RSX wait?** This answers P6. Write backend labels at the
draw boundaries: `rsxSetWriteBackendLabel(context, 250, seq)` after the
JellyWave draws, and 251 after the card phase. During the frame's fence,
poll labels 250, 251 and 255 (the existing `rsxSync` label) through
`gcmGetLabelAddress()`, recording `timing_get_us()` at each change and
`gcmGetVBlankCount()` at frame start and fence end.
* If the RSX is doing work, the JellyWave interval carries the ~10 ms and
  scales with the draws (strobe profiles 2, 3 and 5).
* If it is waiting, the completion times cluster just after a vblank,
  whatever the draw content.

Every API named above is declared in PSL1GHT:
`rsxSetWriteBackendLabel` in `ppu/include/rsx/commands_inc.h:248`, and
`gcmGetLabelAddress` / `gcmGetVBlankCount` in `ppu/include/rsx/gcm_sys.h`.
The label reads go to the same label area that `rsxSync()` already polls
every frame, so they add no new kind of PPU access. In particular they are
not reads of the vertex buffers, which is what the no-readback rule forbids.

---

## Protected: do not modify without bisect evidence

WaveVert layout; the PPU→RSX ownership model and the no-readback rule;
JellyWave topology; the body/rim architecture; the draw primitive; unrelated
XMB code; the audio-reactive branch; the spine UI branch; the playback UI;
cold-boot work. No padding, buffer-size, vertex-count, delay, sleep,
`rsxSync`, blend or clamp change unless a controlled comparison proves that
exact variable is causal.
