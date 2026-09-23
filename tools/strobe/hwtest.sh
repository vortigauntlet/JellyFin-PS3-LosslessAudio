#!/usr/bin/env bash
# hwtest.sh -- one controlled hardware test of the JellyWave strobe, the same
#              way every time, recorded in the investigation's test table.
#
#   tools/strobe/hwtest.sh TEST_ID "ONE-LINE CHANGE" [options]
#
#   --eboot PATH       deploy exactly this artifact (e.g. the untouched Run-9
#                      EBOOT).  It is copied, never modified; no build.
#   (no --eboot)       build the current tree: make clean; make pkg, with
#                      SOURCE_DATE_EPOCH pinned to HEAD's commit time so the
#                      __DATE__ in ui_settings.cpp does not vary by day.
#   --expect-sha PFX   refuse to deploy unless the EBOOT's SHA-256 starts PFX
#   --launches N       cold launches for this build (default 3)
#   --ip ADDR          console (default $PS3_IP or 192.168.0.202)
#   --archive DIR      default $JW_ARCHIVE or ~/jw-strobe-archive
#   --no-clean         skip `make clean` (NOT for bisect steps)
#   --dry-run DIR      no console: DIR stands in for it (DIR/dev_hdd0/...);
#                      used to test this script
#
# Per test it archives, in <archive>/<TEST_ID>/:
#   EBOOT.BIN (read-only), the ELFs + map when built, git rev/status/diff,
#   toolchain identity, build log, the console's render-affecting config files,
#   EBOOT.console-before/after.bin, one player_log per launch + its stats.
# and appends to <archive>/TESTS.tsv, regenerating <archive>/TESTS.md.
#
# The protocol (what you are asked to do per launch) is the investigation's:
#   cold launch -> Home ~30 s -> Settings briefly -> Search briefly -> Home.

set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
PY=${PYTHON:-python3}

usage() { sed -n '2,32p' "$0"; exit 2; }
[ $# -ge 2 ] || usage
TEST_ID=$1; CHANGE=$2; shift 2
case "$TEST_ID" in *[!A-Za-z0-9._-]*|"") echo "TEST_ID: letters, digits, . _ - only" >&2; exit 2 ;; esac

EBOOT=""; EXPECT=""; LAUNCHES=3; IP=${PS3_IP:-192.168.0.202}
ARCH=${JW_ARCHIVE:-$HOME/jw-strobe-archive}; NOCLEAN=0; DRY=""
while [ $# -gt 0 ]; do
    case "$1" in
        --eboot) EBOOT=$2; shift 2 ;;
        --expect-sha) EXPECT=$2; shift 2 ;;
        --launches) LAUNCHES=$2; shift 2 ;;
        --ip) IP=$2; shift 2 ;;
        --archive) ARCH=$2; shift 2 ;;
        --no-clean) NOCLEAN=1; shift ;;
        --dry-run) DRY=$2; shift 2 ;;
        *) usage ;;
    esac
done

REMOTE_EBOOT=/dev_hdd0/game/JFPS30000/USRDIR/EBOOT.BIN
REMOTE_LOG=/dev_hdd0/tmp/player_log.txt
# Console files that change what the XMB draws or how.  A difference between
# two tests in ANY of these is an uncontrolled variable and is reported.
# (jellyfin_config.txt / device id are deliberately NOT copied: credentials.)
CFG_FILES="jellyfin_gpuwave.txt jellyfin_jwspeed.txt jellyfin_jwrebuild.txt
jellyfin_bgdither.txt jellyfin_gpucards.txt jellyfin_gputext.txt
jellyfin_theme.txt jellyfin_uiscale.txt jellyfin_overscan.txt jellyfin_1080p.txt
jellyfin_wavereact.txt jellyfin_months.ini jellyfin_settings.txt"

D="$ARCH/$TEST_ID"
if [ -e "$D" ]; then echo "$D exists -- pick a new TEST_ID (results are never overwritten)" >&2; exit 2; fi
mkdir -p "$D/console_cfg"
TSV="$ARCH/TESTS.tsv"
[ -f "$TSV" ] || printf "date\ttest\tlaunch\teboot_sha\tloadid\tcfg\tchange\tstrobe\tflicker\tsync_ms\tframe_ms\tvsync_ms\tjw_repaired\tresult\tnotes\n" > "$TSV"

log() { echo "[hwtest] $*" | tee -a "$D/hwtest.log"; }

ftp_get() {   # remote local
    if [ -n "$DRY" ]; then cp "$DRY$1" "$2" 2>/dev/null; return; fi
    curl -sS --fail --ftp-pasv --connect-timeout 10 --max-time 180 \
         -u anonymous:anonymous@ "ftp://$IP$1" -o "$2"
}
ftp_put() {   # local remote
    if [ -n "$DRY" ]; then mkdir -p "$(dirname "$DRY$2")"; cp "$1" "$DRY$2"; return; fi
    curl -sS --fail --ftp-pasv --connect-timeout 10 --max-time 600 \
         -u anonymous:anonymous@ -T "$1" "ftp://$IP$2"
}
sha() { sha256sum "$1" | cut -d' ' -f1; }
# Answers come from the terminal; without one (piped/scripted runs) from stdin.
if { exec 3</dev/tty; } 2>/dev/null; then :; else exec 3<&0; fi
ask() {       # prompt -> y/n on stdout ('?' if input ends)
    local a=""
    while :; do
        if ! read -r -u 3 -p "$1 " a; then echo "?"; return; fi
        case "$a" in y|Y|n|N) echo "$a"; return ;; esac
    done
}

# --------------------------------------------------------------------------
# 1. the artifact under test
# --------------------------------------------------------------------------
if [ -n "$EBOOT" ]; then
    [ -f "$EBOOT" ] || { echo "no such EBOOT: $EBOOT" >&2; exit 2; }
    cp -p "$EBOOT" "$D/EBOOT.BIN"
    [ "$(sha "$EBOOT")" = "$(sha "$D/EBOOT.BIN")" ] || { echo "copy mismatch" >&2; exit 1; }
    echo "$(readlink -f "$EBOOT")" > "$D/source_artifact.txt"
    log "artifact: supplied EBOOT $EBOOT (no build)"
else
    [ -n "${PSL1GHT:-}" ] && [ -n "${PS3DEV:-}" ] || { echo "export PSL1GHT and PS3DEV first" >&2; exit 2; }
    [ -f Makefile ] && git rev-parse --git-dir >/dev/null 2>&1 || { echo "run from the repo root" >&2; exit 2; }
    TARGET=$(basename "$PWD")
    git rev-parse HEAD > "$D/git_rev.txt"
    git describe --always --dirty --tags >> "$D/git_rev.txt" 2>/dev/null || true
    git status --porcelain=v1 > "$D/git_status.txt"
    git diff HEAD > "$D/source.diff"
    DIRTY=$(wc -l < "$D/git_status.txt")
    [ "$DIRTY" -gt 0 ] && log "WARNING: tree is dirty ($DIRTY paths) -- source.diff records exactly what was built"
    export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-$(git log -1 --format=%ct)}
    {
        echo "SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH"
        command -v ppu-gcc && ppu-gcc --version | head -1
        command -v ppu-ld && ppu-ld --version | head -1
        for t in make_self_npdrm sprxlinker; do p=$(command -v $t || true); [ -n "$p" ] && echo "$t $(sha "$p") $p"; done
        for l in librsx.a libgcm_sys.a librt.a liblv2.a; do
            [ -f "$PSL1GHT/ppu/lib/$l" ] && echo "$l $(sha "$PSL1GHT/ppu/lib/$l")"
        done
        git -C "$PSL1GHT" rev-parse HEAD 2>/dev/null | sed 's/^/PSL1GHT git /' || true
    } > "$D/toolchain.txt" 2>&1
    log "build: $(head -1 "$D/git_rev.txt") dirty=$DIRTY SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH"
    if [ "$NOCLEAN" = 0 ] && ! make clean > "$D/build.log" 2>&1; then
        log "make clean FAILED -- see $D/build.log (an unclean tree is not a controlled build)"; exit 1
    fi
    if ! make pkg >> "$D/build.log" 2>&1; then log "BUILD FAILED -- see $D/build.log"; exit 1; fi
    grep -c "warning:" "$D/build.log" > "$D/warnings.count" || true
    { ls obj/*.o 2>/dev/null || true; } | wc -l > "$D/objects.count"
    cp -p obj/pkg/USRDIR/EBOOT.BIN "$D/EBOOT.BIN"
    for f in "obj/$TARGET.elf" "$TARGET.elf" "obj/$TARGET.elf.map" "$TARGET.elf.map"; do
        [ -f "$f" ] && cp -p "$f" "$D/$(echo "$f" | tr '/' '_')"
    done
    log "build ok: $(cat "$D/objects.count") objects, $(cat "$D/warnings.count") warnings"
fi
chmod a-w "$D/EBOOT.BIN"

EBOOT_SHA=$(sha "$D/EBOOT.BIN")
LOADID=$("$PY" "$HERE/loadid.py" "$D/EBOOT.BIN" 2>/dev/null | awk '{print $1}' || true)
[ -n "$LOADID" ] && [ "$LOADID" != "n/a" ] || { LOADID=n/a; log "WARNING: no LOADID (export PSL1GHT so loadid.py finds oddkeys.h)"; }
log "EBOOT sha256 $EBOOT_SHA"
log "LOADID       $LOADID  (program identity; compare THIS across rebuilds, not the sha)"
if [ -z "$EBOOT" ] && [ -f "$D/obj_$(basename "$PWD").elf" ]; then
    L2=$("$PY" "$HERE/loadid.py" "$D/obj_$(basename "$PWD").elf" | awk '{print $1}' || true)
    [ "$L2" = "$LOADID" ] && log "check: EBOOT carries exactly obj/*.elf (LOADID match)" \
                          || log "WARNING: EBOOT LOADID != obj/*.elf LOADID ($L2)"
fi
if [ -n "$EXPECT" ]; then
    case "$EBOOT_SHA" in "$EXPECT"*) log "expected sha prefix $EXPECT: OK" ;;
        *) log "REFUSING: sha $EBOOT_SHA does not start with $EXPECT"; exit 1 ;; esac
fi

# --------------------------------------------------------------------------
# 2. console configuration snapshot -- an unnoticed gate-file change is a
#    different experiment
# --------------------------------------------------------------------------
for f in $CFG_FILES; do ftp_get "/dev_hdd0/tmp/$f" "$D/console_cfg/$f" 2>/dev/null || rm -f "$D/console_cfg/$f"; done
CFG_FP=$(cd "$D/console_cfg" && { ls -1 | while read -r f; do printf "%s %s\n" "$f" "$(sha "$f")"; done; } | sha256sum | cut -c1-12)
log "console cfg fingerprint $CFG_FP ($(ls "$D/console_cfg" | tr '\n' ' '))"
if [ -f "$ARCH/.last_cfg" ] && [ "$(cat "$ARCH/.last_cfg")" != "$CFG_FP" ]; then
    log "WARNING: console config CHANGED since the previous test ($(cat "$ARCH/.last_cfg") -> $CFG_FP)"
    [ -d "$ARCH/.last_cfg_dir" ] && diff -r "$ARCH/.last_cfg_dir" "$D/console_cfg" | tee -a "$D/hwtest.log" || true
fi
echo "$CFG_FP" > "$ARCH/.last_cfg"; rm -rf "$ARCH/.last_cfg_dir"; cp -r "$D/console_cfg" "$ARCH/.last_cfg_dir"
[ -f "$D/console_cfg/jellyfin_gpuwave.txt" ] && log "jellyfin_gpuwave.txt = $(tr -d '\r\n' < "$D/console_cfg/jellyfin_gpuwave.txt") (3 = JellyWave)"
[ -f "$D/console_cfg/jellyfin_settings.txt" ] || log "WARNING: no jellyfin_settings.txt -- is player logging (plog) enabled?"

# --------------------------------------------------------------------------
# 3. deploy, and prove it landed
# --------------------------------------------------------------------------
ftp_get "$REMOTE_EBOOT" "$D/EBOOT.console-before.bin" && log "console had $(sha "$D/EBOOT.console-before.bin")"
ftp_put "$D/EBOOT.BIN" "$REMOTE_EBOOT" || { log "UPLOAD FAILED (is webMAN FTP up at $IP?)"; exit 1; }
ftp_get "$REMOTE_EBOOT" "$D/EBOOT.console-after.bin" || { log "READ-BACK FAILED -- deploy unverified. Stop."; exit 1; }
if [ "$(sha "$D/EBOOT.console-after.bin")" != "$EBOOT_SHA" ]; then
    log "DEPLOY VERIFY: MISMATCH -- the console does not hold the tested EBOOT. Stop."; exit 1
fi
log "deploy verify: MATCH ($EBOOT_SHA)"

# --------------------------------------------------------------------------
# 4. launches
# --------------------------------------------------------------------------
n_strobe=0; n_done=0; n_unknown=0; prev_log_sha=""
for k in $(seq 1 "$LAUNCHES"); do
    echo
    echo "=== $TEST_ID launch $k/$LAUNCHES ============================================"
    echo "  1. Quit Jellyfin completely (PS button -> Quit Game) if it is running."
    echo "  2. COLD LAUNCH it from the XMB."
    echo "  3. Stay on Home ~30 s.  4. Settings briefly.  5. Search briefly.  6. Back to Home."
    echo "  Watch for: BIG STROBE (the failure) and the tiny TOP-EDGE flicker (separate bug)."
    read -r -u 3 -p "  Press Enter when back on Home after step 6 ... " _ || true
    L="$D/player_log.L$k.txt"
    if ! ftp_get "$REMOTE_LOG" "$L"; then log "launch $k: could not fetch $REMOTE_LOG"; continue; fi
    ls_sha=$(sha "$L")
    if [ "$ls_sha" = "$prev_log_sha" ]; then
        log "WARNING launch $k: player_log.txt is identical to the previous launch's -- did the app really relaunch?"
    fi
    prev_log_sha=$ls_sha
    "$PY" "$HERE/logstats.py" "$L" --json "$D/logstats.L$k.json" | tee "$D/logstats.L$k.txt" || true
    tl=$(grep '^TABLE' "$D/logstats.L$k.txt" || true)
    sync_ms=$(echo "$tl" | sed -n 's/.*[[:space:]]sync=\([0-9.]*\)ms.*/\1/p'); sync_ms=${sync_ms:-?}
    frame_ms=$(echo "$tl" | sed -n 's/.*frame=\([0-9.]*\)ms.*/\1/p'); frame_ms=${frame_ms:-?}
    vsync_ms=$(echo "$tl" | sed -n 's/.*vsync=\([0-9.]*\)ms.*/\1/p'); vsync_ms=${vsync_ms:-?}
    rep=$(sed -n 's/.*repaired(max)=\([0-9.]*\).*/\1/p' "$D/logstats.L$k.txt" | head -1); rep=${rep:-?}
    st=$(ask "  BIG STROBE this launch? [y/n]")
    fl=$(ask "  TOP-EDGE flicker this launch? [y/n]")
    read -r -u 3 -p "  notes (optional): " notes || notes=""
    case "$st" in y|Y) st=YES; n_strobe=$((n_strobe + 1)) ;; n|N) st=no ;; *) st="?"; n_unknown=$((n_unknown + 1)) ;; esac
    case "$fl" in y|Y) fl=YES ;; n|N) fl=no ;; *) fl="?" ;; esac
    n_done=$((n_done + 1))
    printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" \
        "$(date -Iseconds)" "$TEST_ID" "$k" "${EBOOT_SHA:0:12}" "${LOADID:0:12}" "$CFG_FP" \
        "$CHANGE" "$st" "$fl" "$sync_ms" "$frame_ms" "$vsync_ms" "$rep" "-" "${notes//$'\t'/ }" >> "$TSV"
    log "launch $k: strobe=$st flicker=$fl sync=${sync_ms}ms frame=${frame_ms}ms"
done

if [ "$n_done" -eq 0 ]; then result="NO DATA"
elif [ "$n_unknown" -gt 0 ]; then result="INCOMPLETE ($n_unknown/$n_done launches unanswered)"
elif [ "$n_strobe" -eq 0 ]; then result="CLEAN ($n_done/$n_done)"
elif [ "$n_strobe" -eq "$n_done" ]; then result="STROBE ($n_done/$n_done)"
else result="MIXED ($n_strobe/$n_done strobe) -- NOT deterministic: new evidence, investigate before continuing"
fi
printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" \
    "$(date -Iseconds)" "$TEST_ID" "ALL" "${EBOOT_SHA:0:12}" "${LOADID:0:12}" "$CFG_FP" \
    "$CHANGE" "$n_strobe/$n_done" "-" "-" "-" "-" "-" "$result" "-" >> "$TSV"
log "RESULT $TEST_ID: $result"

# --------------------------------------------------------------------------
# 5. the table, regenerated from the TSV every time
# --------------------------------------------------------------------------
{
    echo "# JellyWave strobe -- hardware test table"
    echo
    echo "Generated by tools/strobe/hwtest.sh from TESTS.tsv. SYNC/FRAME are the medians of the"
    echo "Home window (logstats.py). EBOOT SHA identifies the artifact; LOADID identifies the"
    echo "program (identical LOADID = identical loaded bytes, whatever the SHA)."
    echo
    echo "| TEST | LAUNCH | EBOOT SHA | LOADID | CFG | CHANGE | STROBE | FLICKER | SYNC ms | FRAME ms | RESULT | NOTES |"
    echo "|---|---|---|---|---|---|---|---|---|---|---|---|"
    tail -n +2 "$TSV" | while IFS=$'\t' read -r dt t l es li cf ch s f sy fr vs rp r no; do
        echo "| $t | $l | \`$es\` | \`$li\` | $cf | $ch | $s | $f | $sy | $fr | $r | $no |"
    done
} > "$ARCH/TESTS.md"
echo
echo "table: $ARCH/TESTS.md"
