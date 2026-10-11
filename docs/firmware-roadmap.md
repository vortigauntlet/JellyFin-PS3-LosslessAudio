# Firmware roadmap: passthrough, Sony decoders, more video codecs, system integration

Status: **research, nothing implemented.** It works out what the four items below
would take, from public sources, and lists the experiments that settle each
one on hardware. It is meant to be read before anyone writes code for them.

## Where the evidence comes from

| Source | What it is | What it gives us |
|---|---|---|
| RPCS3 (`rpcs3/Emu/Cell/Modules`, `lv2/`, commit `7b4de2f`) | GPL reimplementation of the firmware libraries and lv2 syscalls | enum values, struct layouts, module IDs, permission checks |
| Linux `arch/powerpc/include/asm/ps3av.h`, `drivers/ps3/ps3av_cmd.c`, `sound/ppc/snd_ps3.c` | GPL driver for the same AV controller, used by OtherOS Linux | the AV controller's audio-mode packet, and a working compressed passthrough |
| This repo's own probes | `audio_out_probe.cpp`, `audio_bitstream.cpp`, `avconf_capture.cpp` | what this console and this receiver actually report |

**Not used, and blocked here:** decrypting the 4.9x PUP and disassembling
`bdp_BDMV.self` / `libtrhddec.sprx` (the method used in
[24P_OUTPUT.md](24P_OUTPUT.md)). The cloud session that wrote this doc could
not reach Sony's update server, so every "the firmware does X" claim below
comes from RPCS3 or Linux. Each one is marked **(RPCS3)** or **(Linux)**, and
none of them has been checked against Sony's code. The next step for each item
is that disassembly, or a probe on the console.

Licensing stays the same as for the 24p work: we learn layouts and call the
firmware at runtime. No decompiled Sony code goes into this GPLv3 tree, and no
keys or decrypted firmware get committed.

---

## 1. HD bitstream passthrough (TrueHD/Atmos, DTS-HD MA/DTS:X, DD+)

### What changed since "impossible"

`dolby-truehd.md` and `dts-hd.md` reject passthrough because "nothing in
PSL1GHT can hand a receiver an encoded stream". `audio_bitstream.cpp` has since
shown that this was only a missing binding for the *mode* half. It tried
`cellAudioOutConfigure` with AC-3, DTS and `0xff`. The *data* half is still
open: anything written to a cellAudio port goes through the system mixer as
float samples, which is not a reliable carrier for bit-exact IEC 61937 bursts.

The public sources split the problem into two layers. Each layer has a known
entry point.

**Data layer: `sys_rsxaudio`, lv2 syscalls 650–657 (RPCS3).**
- These syscalls are not root-gated. RPCS3's syscall table marks `sys_uart_*`
  as ROOT and these as unmarked (`lv2.cpp`).
- The model they expose is the hardware:
  - a "serial" ring of 4 stereo streams (8 channels) feeding HDMI and AV-multi;
  - two S/PDIF rings;
  - 16- or 32-bit samples;
  - clock bases of 384 kHz and 352.8 kHz with a divider (`sys_rsxaudio.h`).
- 4 × 2 channels at 192 kHz is exactly the IEC 61937 high-bit-rate (HBR)
  layout that TrueHD and DTS-HD MA travel in. 2 channels at 48 kHz is the
  layout for AC-3/DTS, and 2 channels at 192 kHz is the layout for DD+.
- Each per-port `hdmi_param_t` carries a 5-byte channel status (`chstat`) and
  an audio `info_frame`. The non-audio flag and the coding-type field live in
  those.
- As far as RPCS3's design shows, the real `libaudio.sprx` is a client of these
  syscalls. That would mean a process can drive the rings itself, bit-exact, if
  it does not also start cellAudio.

**Mode layer: the AV controller "audio mode" packet, `PS3AV_CID_AUDIO_MODE` `0x02000002` (Linux, RPCS3).**
- `struct ps3av_pkt_audio_mode` (`ps3av.h:588`) takes these fields:
  - `audio_num_of_ch`, `audio_fs`, `audio_word_bits`;
  - **`audio_format` = `PS3AV_CMD_AUDIO_FORMAT_BITSTREAM` (`0xff`)**;
  - `audio_source` (serial or S/PDIF);
  - `audio_cs_info[8]`, the IEC channel status.
- Linux's `snd_ps3.c` does compressed S/PDIF/HDMI passthrough this way. It sets
  the non-audio channel-status bit (`cs_info[0] & 0x02`) and mutes the analog
  outputs while that bit is set.
- In GameOS, this packet travels over `sys_uart_send` (syscall 369), which
  **requires root permission (RPCS3)**. That check is
  `ctrl_flags1 & (0xc << 28)`, which comes from the SELF's control flags. A
  fake-signed homebrew SELF sets those flags when it is signed, so on
  CFW/HEN this looks reachable. It has not been confirmed.
- The VSH probably owns this channel already. Writing to it underneath the VSH
  risks fighting the system's own AV settings.

**The game-facing path to try first.** The firmware's cellAudioOut coding-type
enum (RPCS3 `cellAudioOut.h`) already has these values:

| Value | Name |
|---|---|
| 8 | TrueHD |
| 9 | DD+ |
| 10 | DTS-HD HRA |
| 11 | DTS-HD MA |

RPCS3 calls the names for 8, 10 and 11 "speculative", and `audio_out.h` here
says "name unconfirmed". The console cannot *encode* any of those formats, so a
coding type that names one only makes sense as "the app supplies the
bitstream". That is the strongest public hint that the BD player feeds a
compressed stream into a normal output path. `audio_out_probe.cpp` already asks
`audioOutGetSoundAvailability` about 8, 9 and 11, so **the startup log on a
console wired to an HD-capable receiver may already answer whether the
firmware offers them.**

### Experiments, in order (each one is cheap, reversible, and needs the console)

1. **Read the existing `aout: avail` lines** on an HDMI chain whose receiver
   advertises TrueHD/DTS-HD.
   - If types 8, 9 or 11 report channels > 0, the firmware is willing.
   - Also note any `mode[i] type=8..11` lines in the device-info dump.
2. **Configure 8–11 the way `audio_bitstream.cpp` configures AC-3.** Use
   `channel=8` for 8 and 11, and `channel=2` for 9.
   - Read the result back with `audioOutGetState` (not GetConfiguration).
   - Feed silence, never decoded audio. A receiver that locks onto non-audio
     garbage mutes rather than blasting noise, but silence is safer still.
   - This needs new values in `jellyfin_bitstream.txt`, and the existing
     journal/revert handles the cleanup.
3. **If (2) sticks:** find out where the BD player writes its bursts.
   - Capture `bdp_BDMV.self`'s syscall usage the way `avconf_capture.cpp`
     captured avconf: dump its import table and look for direct `sc` to
     650–657.
   - Its import table has no audio-output PRX (`audio_bitstream.cpp` notes
     this), which already points at direct `sys_rsxaudio`.
4. **Write a minimal `sys_rsxaudio` client.** It plays a pre-built IEC 61937
   AC-3 file at 2 ch/48 kHz/16-bit with the non-audio bit set. AC-3 first,
   because the receiver's "Dolby Digital" light confirms success without any
   ambiguity.
   - Then DD+ at 2 ch/192 kHz.
   - Then TrueHD at 8 ch/192 kHz (MAT framing).
   - Each step is a bigger change to the same writer, so stop at the first one
     that fails.
5. **Only if (2) fails:** the root path through `sys_uart` with
   `PS3AV_CID_AUDIO_MODE`, using a SELF signed with root control flags. This
   is CFW/HEN-only and contends with the VSH. Treat it as the last resort.

### Known limits

- **Hardware:** HBR (TrueHD/DTS-HD MA bitstream) is a Slim/Super Slim feature
  of the BD player. Launch "fat" models bitstream only AC-3/DTS. Expect the
  same limit here, and gate on `GetSoundAvailability`, never on the model
  name.
- **Demux:** passthrough means the server stream-copies the HD track, which
  the HD path already asks for. The decoders in `source/audio/` stay as the
  fallback for LPCM-only chains.

---

## 2. Sony's own audio decoders (cellAdec on the SPUs)

**What exists (RPCS3 `cellAdec.h`, `cellSysmodule.cpp`):**

| Codec | `CELL_ADEC_TYPE_*` | Module | Load path |
|---|---|---|---|
| AC-3 | `AC3` | `libac3dec.sprx` | public |
| **E-AC-3 / DD+** | `EAC3` | `libddpdec.sprx` (`0xf026`) | internal (`cellSysmoduleLoadModuleInternal`) |
| **TrueHD** | `TRUEHD` | `internal/libtrhddec.sprx` (`0xf01f`) | internal |
| **DTS-HD** (core + HD since FW 4.00) | `DTSHD` | `internal/libdtshddec.sprx` (`0xf037`) | internal |
| DTS Express (LBR) | `DTSLBR` | `libdtslbrdec.sprx` (`0xf056`) | internal |
| AAC (MPEG-4) | `M4AAC` | `libm4aacdec.sprx` | public |
| WMA | `WMA` | `libwmadec.sprx` (`0xf024`) | internal, **PARAM.SFO-gated** (see §3) |
| LPCM (Blu-ray / DVD) | `LPCM_BLURAY`, `LPCM_DVD` | `libadec` | public |

**Why it matters:**
- **DD+ is the gap that matters most.** `surround-5.1.md` and
  `dolby-truehd.md` rejected E-AC-3 only because no small GPL decoder exists.
  With Sony's decoder, DD+ (and the DD+ Atmos bed) plays without the
  server's AC-3 transcode.
- **TrueHD/DTS-HD would move off the PPU.** The MLP decoder and `dcahd`
  currently run on the main CPU, so moving them frees PPU time for higher
  video bitrates. Keep our own decoders anyway. They are bit-exact and tested
  (`tests/test_dts_hd.c` and friends), and Sony's output has to match them
  before it replaces them.
- **AAC would be a cheap win.** Jellyfin direct-plays AAC 5.1 sources that
  currently get transcoded.

**What is unknown:**
- Whether `cellSysmoduleLoadModuleInternal` is callable from a homebrew title
  for `0xf01f`/`0xf037`/`0xf026`.
- Whether those three carry a PARAM.SFO gate like `0xf020`/`0xf024`/`0xf03e`.
  RPCS3 only shows the gate for those three IDs.
- PSL1GHT binds neither `cellSysmoduleLoadModuleInternal` nor any `cellAdec*`
  function. Both would need FNID stubs, the way `audio_out_stub.S` does it.
- The `CellAdecParam*` structs for EAC3/TRUEHD/DTSHD. RPCS3 has
  `CellAdecParamAc3` and the core ops, but the codec-specific params need the
  disassembly.

**Experiment:**
- A file-triggered probe, `jellyfin_adecprobe.txt`, in the style of
  `avconf_capture.cpp`. It would:
  1. call `cellSysmoduleLoadModuleInternal` for each ID above and log the
     return code;
  2. call `cellAdecQueryAttr` for each type and log the memory and SPU needs;
  3. dump the loaded segments of `libtrhddec`/`libddpdec` for
     `fwcap_analyze.py`.
- No decoding happens, so there is nothing to break.

---

## 3. More hardware video codecs (MPEG-2, VC-1, MVC)

`vdec.cpp` opens only `SYSMODULE_VDEC_H264`. The firmware has more (RPCS3
`cellVdec.h`, `cellSysmodule.cpp`):

| Codec | `CELL_VDEC_CODEC_TYPE_*` | Module | Load path |
|---|---|---|---|
| MPEG-2 | `MPEG2` (0) | `libsmvd2.sprx` | **public** (`CELL_SYSMODULE_VDEC_MPEG2`) |
| VC-1 | `VC1` (3) | `libsvc1d.sprx` (`0xf020`) | internal, **PARAM.SFO-gated** |
| MPEG-4 Part 2 / DivX | `MPEG4`, `DIVX` | `libsmvd4`, `libdivxdec` | public |
| MVC (3D Blu-ray) | `MVC` (11) | `libmvcdec.sprx` (`0xf049`) | internal |

**MPEG-2 is the easy one.** It is a public module behind the same `cellVdec`
API the H.264 path already uses, so it needs:
- `ts_demux` to pass stream type `0x02`;
- `vdec_open` to choose the codec type;
- the Jellyfin device profile to list `mpeg2video` for direct play.

That covers DVD rips and early Blu-rays without a server transcode.
`vdec.cpp:433` already has a table of "MPEG-2/H264 frame rate codes", so the
picture-info side was anticipated.

**VC-1 needs the gate opened.** `cellSysmoduleLoadModuleInternal` refuses
`0xf020` (VC-1), `0xf024` (WMA) and `0xf03e` unless bit 0 of the qword at
`0x18` or at `0x30` of the `_sys_process_get_paramsfo` (syscall 30) buffer is
set (RPCS3 `cellSysmodule.cpp:1149`). That buffer is built from the title's
PARAM.SFO. RPCS3 does not know which SFO field feeds those offsets; it fills
in only the title ID at offset 1.

**Experiment:**
1. Log the 0x40-byte syscall-30 buffer for this app.
2. Compare it against `sfo.xml`'s `ATTRIBUTE` and the other fields.
3. Set the candidate bit in this app's `sfo.xml`.
4. Retry `LoadModuleInternal(0xf020)`.

PARAM.SFO is ours to write, so if the gate is an SFO attribute, VC-1 (and WMA)
come free. VC-1 matters only for older Warner/Paramount/Universal Blu-ray
remuxes, so do this after MPEG-2.

**MVC 3D is a curiosity.** It needs frame-packed 3D output from
`cellVideoOutConfigure2`, the same call the 24p work decoded, and a 3D TV.
Park it.

---

## 4. System integration

The BD remote is already handled: `ui_input.cpp` reads the key code in
`button[25]` for `device_type` 4. The gaps are elsewhere.

| Feature | Mechanism | Status |
|---|---|---|
| **Pause when the PS button opens the in-game XMB** | sysutil callback events `0x0131` SYSTEM_MENU_OPEN / `0x0132` CLOSE (RPCS3 `cellSysutil.h`) | **Public API, implementable now.** `main.cpp:73` handles only EXIT_GAME. |
| **Throttle the GPU UI while the system draws over us** | `0x0121` DRAWING_BEGIN / `0x0122` DRAWING_END | Public. JellyWave and GPU cards are heavy, so this keeps the system overlay smooth. |
| **The user's XMB background music ducking or mixing with ours** | `cellSysutilDisableBgmPlayback` / `GetBgmPlaybackStatus`, events `0x0141`/`0x0142` | Public. Disable it during Music and video playback. |
| **TV remote via HDMI-CEC** | AV controller `PS3AV_CID_AV_CEC_MESSAGE` `0x000A0001` (RPCS3 `sys_uart.h`) | **Needs research.** The BD player answers TV-remote keys over CEC. Find out first whether a game sees them as pad input (log every `device_type` while pressing TV-remote keys). If not, the only route is root `sys_uart`, which belongs to the VSH. |
| **Console auto-off during long films** | none found in the public headers | Check first whether it happens at all: leave a 3-hour film running with no input. |

Items 1–3 need no reverse engineering. They are the only part of this document
that can ship without a console experiment first, and even they should be
tested on the console, because the callback runs on the thread that calls
`sysUtilCheckCallback()`.

---

## Suggested order

1. §4 menu/drawing/BGM events: small, public API, immediately useful.
2. §1 experiments 1–2: free information from logs and one more value in
   `jellyfin_bitstream.txt`.
3. §3 MPEG-2: public module, existing vdec path.
4. §2 module-load probe, which also yields the dumps that §1 step 3 needs.
5. §1 steps 3–4, depending on what 1–2 and the probe show.
6. §3 VC-1 gate.
