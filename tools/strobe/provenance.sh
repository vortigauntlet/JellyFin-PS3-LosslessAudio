#!/usr/bin/env bash
# provenance.sh -- PHASE 1 of the JellyWave strobe forensics:
#                  "which source state and which build produced the Run-9 EBOOT?"
#
# Read-only.  Touches nothing it finds: every git command is a query, every
# artifact is hashed in place.  Writes only into the report directory.
#
#   tools/strobe/provenance.sh [--repo DIR]... [--search DIR]... [--want PREFIX]...
#                              [--keys oddkeys.h] [--out DIR]
#
# Defaults:
#   --repo    ~/JellyFin-PS3-jw-fix plus every git repo directly under ~ named JellyFin-PS3*
#   --search  ~  and, under WSL, /mnt/c/Users/*/{Desktop,Downloads,Documents}
#             (tools/ps3push/ftp.ps1 leaves EBOOT.console-before/after.bin next to itself)
#   --want    75ae4602 (Run 9, GOOD)  8fd2b538 (gradient-last + 24 KB pad, BAD)
#   --keys    $PSL1GHT/tools/geohot/include/oddkeys.h (to see inside EBOOTs)
#   --out     ~/jw-strobe-archive/provenance-<timestamp>
#
# What it establishes, and how:
#   * every EBOOT/.self/.elf/.pkg on disk: SHA-256 (artifact identity), mtime,
#     and LOADID (program identity, see loadid.py).  The Run-9 EBOOT is found
#     by SHA prefix; every ELF with the SAME LOADID is the same program, and
#     the build tree it sits in is the tree Run 9 was built from.
#   * for each repo and each of its worktrees: all refs, the full reflog with
#     dates, stashes, dangling commits (git fsck), and the UNCOMMITTED diff --
#     Run 9 may well have been built from a dirty tree, which no commit
#     records.  The dirty diff is saved verbatim.
#   * a timeline: reflog entries and object-file mtimes around each wanted
#     EBOOT's mtime, so "what was HEAD when this was built" has an answer.

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
PY=${PYTHON:-python3}

REPOS=(); SEARCH=(); WANT=(); KEYS=""; OUT=""
while [ $# -gt 0 ]; do
    case "$1" in
        --repo)   REPOS+=("$2"); shift 2 ;;
        --search) SEARCH+=("$2"); shift 2 ;;
        --want)   WANT+=("$2"); shift 2 ;;
        --keys)   KEYS=$2; shift 2 ;;
        --out)    OUT=$2; shift 2 ;;
        -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

[ ${#WANT[@]} -eq 0 ] && WANT=(75ae4602 8fd2b538)
if [ ${#REPOS[@]} -eq 0 ]; then
    [ -d "$HOME/JellyFin-PS3-jw-fix/.git" ] && REPOS+=("$HOME/JellyFin-PS3-jw-fix")
    for d in "$HOME"/JellyFin-PS3*; do
        [ -e "$d/.git" ] && [ "$d" != "$HOME/JellyFin-PS3-jw-fix" ] && REPOS+=("$d")
    done
fi
if [ ${#SEARCH[@]} -eq 0 ]; then
    SEARCH=("$HOME")
    for d in /mnt/c/Users/*/Desktop /mnt/c/Users/*/Downloads /mnt/c/Users/*/Documents; do
        [ -d "$d" ] && SEARCH+=("$d")
    done
fi
if [ -z "$KEYS" ] && [ -n "${PSL1GHT:-}" ] && [ -f "$PSL1GHT/tools/geohot/include/oddkeys.h" ]; then
    KEYS="$PSL1GHT/tools/geohot/include/oddkeys.h"
fi
[ -z "$OUT" ] && OUT="$HOME/jw-strobe-archive/provenance-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT/repos"
SUM="$OUT/SUMMARY.txt"
: > "$SUM"
say() { echo "$*" | tee -a "$SUM"; }

say "provenance report  $(date -Iseconds)"
say "repos : ${REPOS[*]:-(none found)}"
say "search: ${SEARCH[*]}"
say "want  : ${WANT[*]}"
say "keys  : ${KEYS:-(none -- EBOOT contents cannot be identified; export PSL1GHT)}"
say ""

# --------------------------------------------------------------------------
# 1. every binary artifact on disk
# --------------------------------------------------------------------------
ART="$OUT/artifacts.tsv"
printf "sha256\tloadid\tkind\tsize\tmtime\tpath\n" > "$ART"
KEYARG=(); [ -n "$KEYS" ] && KEYARG=(--keys "$KEYS")
find "${SEARCH[@]}" -xdev \( -name node_modules -o -name .cache -o -name .git -o -name proc \) -prune -o \
     -type f \( -iname 'EBOOT*.BIN' -o -iname 'EBOOT*.bin' -o -name '*.self' -o -name '*.elf' \
               -o -name '*.pkg' -o -name '*.elf.map' \) -size +1k -print0 2>/dev/null |
while IFS= read -r -d '' f; do
    sha=$(sha256sum "$f" | cut -d' ' -f1)
    mt=$(stat -c '%y' "$f" 2>/dev/null)
    sz=$(stat -c '%s' "$f" 2>/dev/null)
    case "$f" in
        *.pkg|*.map) lid="-"; kind="${f##*.}" ;;
        *) read -r lid kind _ < <("$PY" "$HERE/loadid.py" "${KEYARG[@]}" "$f" 2>/dev/null || echo "n/a err -") ;;
    esac
    printf "%s\t%s\t%s\t%s\t%s\t%s\n" "$sha" "$lid" "$kind" "$sz" "$mt" "$f" >> "$ART"
done
say "artifacts: $(($(wc -l < "$ART") - 1)) found -> $ART"

# --------------------------------------------------------------------------
# 2. git state of every repo and worktree -- including what was NOT committed
# --------------------------------------------------------------------------
for R in "${REPOS[@]}"; do
    name=$(basename "$R")
    D="$OUT/repos/$name"; mkdir -p "$D"
    git -C "$R" for-each-ref --format='%(objectname) %(committerdate:iso-strict) %(refname)' > "$D/refs.txt" 2>&1
    git -C "$R" log --all --date=iso-strict --format='%H %cd %ad %an |%d %s' > "$D/log_all.txt" 2>&1
    git -C "$R" log -g --all --date=iso-strict --format='%gd %H %gs' > "$D/reflog_all.txt" 2>&1
    git -C "$R" log -g --date=iso-strict --format='%gd %H %gs' HEAD > "$D/reflog_HEAD.txt" 2>&1
    git -C "$R" stash list --date=iso-strict > "$D/stash.txt" 2>&1
    git -C "$R" fsck --unreachable --no-reflogs 2>/dev/null | awk '$2=="commit"{print $3}' |
        while read -r c; do git -C "$R" show -s --date=iso-strict --format='%H %cd %s' "$c"; done |
        sort -k2 > "$D/dangling_commits.txt"
    git -C "$R" worktree list --porcelain > "$D/worktrees.txt" 2>&1
    wi=0
    awk '/^worktree /{print $2}' "$D/worktrees.txt" | while read -r W; do
        wi=$((wi + 1)); wn="$wi.$(basename "$W")"
        echo "$W" > "$D/wt${wn}.path"
        git -C "$W" rev-parse HEAD > "$D/wt${wn}.HEAD" 2>&1
        git -C "$W" status --porcelain=v1 > "$D/wt${wn}.status" 2>&1
        git -C "$W" diff HEAD > "$D/wt${wn}.dirty.diff" 2>&1
        # untracked files are part of a dirty build too
        git -C "$W" ls-files --others --exclude-standard -z | xargs -0 -r -I{} sh -c \
            'printf "%s\t%s\n" "$(stat -c %y "$1" 2>/dev/null)" "$1"' _ "$W/{}" > "$D/wt${wn}.untracked" 2>/dev/null
        # build products and their times: the newest object is when the tree was last built
        {
            ls -l --time-style=full-iso "$W"/*.elf "$W"/obj/*.elf "$W"/obj/pkg/USRDIR/EBOOT.BIN "$W"/*.pkg 2>/dev/null
            echo "newest objects:"; ls -lt --time-style=full-iso "$W"/obj/*.o 2>/dev/null | head -5
        } > "$D/wt${wn}.build_products" 2>&1
    done
    say "repo $R: $(wc -l < "$D/log_all.txt") commits, $(wc -l < "$D/reflog_all.txt") reflog entries, $(wc -l < "$D/dangling_commits.txt") dangling commits, $(grep -c '^worktree ' "$D/worktrees.txt") worktree(s)"
done
say ""

# --------------------------------------------------------------------------
# 3. the wanted EBOOTs: where, when, which program, which tree
# --------------------------------------------------------------------------
for w in "${WANT[@]}"; do
    say "=== WANT $w"
    hits=$(awk -F'\t' -v p="$w" 'NR>1 && index($1,p)==1' "$ART")
    if [ -z "$hits" ]; then
        say "  no artifact on disk has a SHA-256 starting $w"
        say "  (a rebuild will NOT recreate it: make_self_npdrm encrypts with random keys --"
        say "   look for the file itself, e.g. the copy you redeployed, or EBOOT.console-*.bin)"
        continue
    fi
    echo "$hits" | while IFS=$'\t' read -r sha lid kind sz mt path; do
        say "  artifact : $path"
        say "    sha256 : $sha   size $sz   mtime $mt"
        say "    loadid : $lid ($kind)"
        same=$(awk -F'\t' -v l="$lid" -v p="$path" 'NR>1 && $2==l && $6!=p {print "      " $5 "  " $6}' "$ART")
        if [ "$lid" != "n/a" ] && [ -n "$same" ]; then
            say "    SAME PROGRAM (identical LOADID) elsewhere on disk:"
            say "$same"
        else
            say "    no other file carries this LOADID (the build tree's obj/<dir>.elf may have been rebuilt since)"
        fi
        # timeline: reflog moves in the 6 h before the artifact's mtime
        ts=$(date -d "$mt" +%s 2>/dev/null || echo 0)
        for R in "${REPOS[@]}"; do
            say "    $R reflog in the 6 h before the artifact (newest last):"
            # With --date=unix, %gd is "<ref>@{<unix time of the reflog entry>}".
            git -C "$R" log -g --all --date=unix --format='%gd %H %gs' 2>/dev/null |
                while read -r ref h rest; do
                    rt=$(echo "$ref" | sed -n 's/.*@{\([0-9]*\)}.*/\1/p')
                    [ -z "$rt" ] && continue
                    if [ "$rt" -le "$ts" ] && [ "$rt" -ge $((ts - 21600)) ]; then
                        echo "      $(date -d @"$rt" -Iseconds)  ${ref%%@*}  $h  $rest"
                    fi
                done | sort | tail -15 | tee -a "$SUM"
        done
    done
    say ""
done

say "NEXT: for the tree that matches Run 9, rebuild it and compare PROGRAMS, not EBOOT hashes:"
say "  tools/strobe/loadid.py <Run-9 EBOOT> obj/<dir>.elf"
say "  tools/strobe/self2elf.py <Run-9 EBOOT> -o run9 && tools/strobe/elfcmp.py run9.elf obj/<dir>.elf"
echo
echo "report: $OUT"
