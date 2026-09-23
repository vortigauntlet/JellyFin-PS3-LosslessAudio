# tools/strobe: forensic kit for the JellyWave big strobe

The investigation record, with its controls, proven facts, test table and
next steps, is in `docs/jellywave-strobe/INVESTIGATION.md`.

Everything here runs on the build machine (WSL/Linux) with `python3`, and
uses `openssl` or a Python AES module for EBOOTs. Export `PSL1GHT` so the
tools can find `tools/geohot/include/oddkeys.h`, the keys your own
`make_self_npdrm` used. No key is embedded in this repository.

| tool | question it answers |
|---|---|
| `provenance.sh` | Phase 1: which file is the Run-9 EBOOT, which build tree produced the same program, and what was HEAD (and the dirty diff) when it was built? Read-only. |
| `loadid.py` | "Is this the same program?" A hash over the loaded bytes that agrees between an EBOOT, its `obj/<dir>.elf`, and a reproducible rebuild. |
| `self2elf.py` | Extracts the ELF from an EBOOT/.self (geohot `make_self_npdrm`, or `fself`). Every segment is HMAC-verified. |
| `elfcmp.py` | Compares GOOD and BAD by segment, section and symbol (with `obj/<dir>.elf.map`); a watch-list puts `s_wave_*`, `s_jw*`, `rsx*`, … first. |
| `hwtest.sh` | One controlled hardware test: build (or take an EBOOT) → hash → archive → snapshot console config → FTP deploy with read-back verify → guided cold launches → pull `player_log.txt` → stats → row in `TESTS.md`. |
| `logstats.py` | Turns one launch's `player_log.txt` into sync / frame / vsync / gpu medians (Home, all, tail, per strobe profile). |
| `vram_map/` | Replays `rsxMemalign` through PSL1GHT's own `heap.c` to show where every RSX-local buffer lands and what sits next to it. |

## Why EBOOT hashes are not build identities

`make_self_npdrm` encrypts every segment with keys drawn from `time(NULL)`.
One ELF packaged twice gives two SHA-256s. Keep the untouched Run-9 EBOOT as
the artifact control, and compare builds by LOADID or `elfcmp`.

## Typical session

```sh
export PSL1GHT=... PS3DEV=...
tools/strobe/provenance.sh                                   # once: find Run 9
tools/strobe/hwtest.sh R9-ctl "golden control" --eboot ~/keep/EBOOT.run9.BIN --expect-sha 75ae4602
tools/strobe/hwtest.sh B03 "apply <one logical change>"      # each bisect step, from the repo root
```

Each test directory under `~/jw-strobe-archive/<TEST_ID>/` is write-once.
`hwtest.sh` refuses to reuse an ID.
