#!/usr/bin/env python3
"""elfcmp.py -- compare two PPU ELFs the way the strobe investigation needs.

    elfcmp.py GOOD.elf BAD.elf [--syms-a X] [--syms-b Y] [--watch REGEX]

GOOD/BAD may be real build outputs (unstripped ./<dir>.elf, stripped
obj/<dir>.elf) or ELFs recovered from an EBOOT with self2elf.py.  A recovered
ELF has no section NAMES (they live in .shstrtab, which no segment carries);
names are borrowed from the other side when the section tables line up.

--syms-a / --syms-b take an unstripped ELF (.symtab) or a GNU ld map file
(obj/<dir>.elf.map, which the Makefile always writes).  The map is the better
source here: the build uses -ffunction-sections -fdata-sections, so every
function AND every file-static variable is its own input section with an
address and size -- including the statics (s_wave_vbuf, s_jw_stage, ...)
that .symtab only carries as local symbols.

Reported, in order:
  1. identity   -- is every loaded byte identical?  (the only question that
                   decides "same build" -- see self2elf.py on EBOOT hashes)
  2. segments   -- vaddr/filesz/memsz and content per program header
  3. sections   -- addr/size/type and content per section, with the byte
                   ranges that differ, symbolised
  4. symbols    -- moved / resized / added / removed, watch-list first
"""

import argparse
import hashlib
import re
import signal
import struct
import sys

WATCH_DEFAULT = (r"^(s_wave_|s_jw|jw_|wave_|s_field|context$|color_buffer|depth_buffer|"
                 r"s_atlas|s_fp_buf|s_rect_fp_buf|s_slots|s_vram|rsx|gcm|flip$|waitflip|"
                 r"init_screen|setRenderTarget|ui_text_gpu|ui_card_gpu|thumb_|s_bg$|s_div)")


def u(fmt, buf, off):
    return struct.unpack_from(fmt, buf, off)


class Elf:
    def __init__(self, path):
        self.path = path
        b = open(path, "rb").read()
        self.b = b
        if b[:4] != b"\x7fELF" or b[4] != 2 or b[5] != 2:
            sys.exit("elfcmp: %s is not an ELF64 big-endian file" % path)
        (self.type, self.machine, _v, self.entry, phoff, shoff, _f, _eh, phentsize,
         phnum, shentsize, shnum, shstrndx) = u(">HHIQQQIHHHHHH", b, 16)
        self.ph = []
        for i in range(phnum):
            t, fl, off, va, _pa, fsz, msz, al = u(">IIQQQQQQ", b, phoff + i * phentsize)
            self.ph.append(dict(i=i, type=t, flags=fl, off=off, vaddr=va, filesz=fsz, memsz=msz, align=al))
        self.sh = []
        if shoff and shoff + shnum * shentsize <= len(b):
            for i in range(shnum):
                (nm, t, fl, addr, off, size, link, info, al, es) = u(">IIQQQQIIQQ", b, shoff + i * shentsize)
                self.sh.append(dict(i=i, name_off=nm, type=t, flags=fl, addr=addr, off=off,
                                    size=size, link=link, info=info, name=""))
        self.names_ok = False
        if self.sh and shstrndx < len(self.sh):
            st = self.sh[shstrndx]
            tab = b[st["off"]:st["off"] + st["size"]]
            if tab[:1] == b"\0" and any(tab[1:]):
                for s in self.sh:
                    e = tab.find(b"\0", s["name_off"])
                    s["name"] = tab[s["name_off"]:e].decode("ascii", "replace") if e >= 0 else ""
                self.names_ok = True

    def seg_bytes(self, p):
        return self.b[p["off"]:p["off"] + p["filesz"]]

    def sec_bytes(self, s):
        if s["type"] == 8:           # NOBITS
            return b""
        return self.b[s["off"]:s["off"] + s["size"]]

    def symtab(self):
        out = []
        for s in self.sh:
            if s["type"] != 2:           # SHT_SYMTAB
                continue
            strs = self.sh[s["link"]]
            stab = self.b[strs["off"]:strs["off"] + strs["size"]]
            for k in range(s["size"] // 24):
                nm, info, other, shndx, val, size = u(">IBBHQQ", self.b, s["off"] + k * 24)
                typ = info & 0xF
                if typ not in (1, 2, 6) or not nm or shndx == 0:   # OBJECT, FUNC, TLS
                    continue
                e = stab.find(b"\0", nm)
                name = stab[nm:e].decode("ascii", "replace")
                # PPC64 ELFv1: FUNC symbols point at the .opd descriptor; keep them,
                # the dot-symbol (".name") carries the code address when present.
                out.append((val, size, name, "F" if typ == 2 else "O"))
        return out


def load_map(path):
    """GNU ld map -> [(addr, size, name, kind)] from -ffunction/-fdata input sections."""
    out = []
    pend = None
    rx_full = re.compile(r"^ (\.[\w.$@-]+)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S+)")
    rx_name = re.compile(r"^ (\.[\w.$@-]+)\s*$")
    rx_cont = re.compile(r"^\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S+)")
    for line in open(path, "r", errors="replace"):
        m = rx_full.match(line)
        if m:
            pend = None
            sec, addr, size, obj = m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4)
        else:
            m = rx_name.match(line)
            if m:
                pend = m.group(1)
                continue
            m = rx_cont.match(line)
            if not (m and pend):
                pend = None
                continue
            sec, addr, size, obj = pend, int(m.group(1), 16), int(m.group(2), 16), m.group(3)
            pend = None
        if not size or not addr:
            continue
        name = sec
        for pfx in (".text.", ".data.rel.ro.", ".data.rel.", ".data.", ".rodata.", ".bss.",
                    ".sbss.", ".sdata.", ".tbss.", ".tdata.", ".opd.", ".toc."):
            if sec.startswith(pfx):
                name = sec[len(pfx):]
                break
        obj = obj.split("/")[-1]
        out.append((addr, size, "%s  [%s %s]" % (name, sec.split(".")[1] if sec.count(".") > 1 else sec, obj), "M"))
    return out


def load_syms(path):
    if not path:
        return []
    head = open(path, "rb").read(4)
    if head == b"\x7fELF":
        return Elf(path).symtab()
    return load_map(path)


class Symboliser:
    def __init__(self, syms):
        self.s = sorted(syms)

    def at(self, addr):
        # linear is fine: called only for the handful of differing ranges
        best = None
        for a, sz, n, k in self.s:
            if a <= addr < a + max(sz, 1):
                if best is None or sz < best[1]:
                    best = (a, sz, n)
        if best:
            return "%s+0x%x" % (best[2], addr - best[0])
        return None


def diff_ranges(a, b, base, limit=12):
    """Byte ranges where a and b differ (same length assumed), as vaddrs."""
    n = min(len(a), len(b))
    out, i, total = [], 0, 0
    while i < n:
        if a[i] != b[i]:
            j = i
            while j < n and a[j] != b[j]:
                j += 1
            total += j - i
            if len(out) < limit:
                out.append((base + i, j - i))
            i = j
        else:
            i += 1
    return out, total


def sha(b):
    return hashlib.sha256(b).hexdigest()[:16]


def main():
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)   # quiet when piped to head
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a", help="GOOD (control) ELF")
    ap.add_argument("b", help="BAD / candidate ELF")
    ap.add_argument("--syms-a", help="unstripped ELF or .map for A")
    ap.add_argument("--syms-b", help="unstripped ELF or .map for B")
    ap.add_argument("--watch", default=WATCH_DEFAULT, help="regex of symbols to list first")
    ap.add_argument("--all-symbols", action="store_true", help="list every symbol change")
    a = ap.parse_args()

    A, B = Elf(a.a), Elf(a.b)
    SA = Symboliser(load_syms(a.syms_a) or (A.symtab() if A.sh else []))
    SB = Symboliser(load_syms(a.syms_b) or (B.symtab() if B.sh else []))

    # borrow section names for a recovered (nameless) ELF
    if A.names_ok and not B.names_ok and len(A.sh) == len(B.sh):
        for x, y in zip(A.sh, B.sh):
            y["name"] = x["name"] + "?"
    if B.names_ok and not A.names_ok and len(A.sh) == len(B.sh):
        for x, y in zip(B.sh, A.sh):
            y["name"] = x["name"] + "?"

    print("A (GOOD) %s" % a.a)
    print("B (BAD)  %s" % a.b)
    print()

    # ---- 1. identity -------------------------------------------------------
    loadA = [(p["vaddr"], p["memsz"], A.seg_bytes(p)) for p in A.ph if p["type"] == 1]
    loadB = [(p["vaddr"], p["memsz"], B.seg_bytes(p)) for p in B.ph if p["type"] == 1]
    same = loadA == loadB and A.entry == B.entry
    print("1. IDENTITY: %s" % ("IDENTICAL -- every loaded byte, vaddr and memsz match (same program)"
                               if same else "DIFFERENT PROGRAMS -- see below"))
    print()

    # ---- 2. segments --------------------------------------------------------
    print("2. SEGMENTS")
    for i in range(max(len(A.ph), len(B.ph))):
        pa = A.ph[i] if i < len(A.ph) else None
        pb = B.ph[i] if i < len(B.ph) else None
        if not pa or not pb:
            print("  [%d] only in %s" % (i, "A" if pa else "B"))
            continue
        ca, cb = A.seg_bytes(pa), B.seg_bytes(pb)
        tag = "same" if (ca == cb and pa["vaddr"] == pb["vaddr"] and pa["memsz"] == pb["memsz"]) else "DIFF"
        print("  [%d] type=0x%08x  A vaddr=0x%08x filesz=0x%07x memsz=0x%07x %s | "
              "B vaddr=0x%08x filesz=0x%07x memsz=0x%07x %s  %s"
              % (i, pa["type"], pa["vaddr"], pa["filesz"], pa["memsz"], sha(ca),
                 pb["vaddr"], pb["filesz"], pb["memsz"], sha(cb), tag))
    print()

    # ---- 3. sections --------------------------------------------------------
    print("3. SECTIONS")
    if not A.sh or not B.sh:
        print("  (no section table on one side)")
    else:
        byname = {}
        if all(s["name"] for s in A.sh[1:]) and all(s["name"] for s in B.sh[1:]):
            bmap = {s["name"].rstrip("?"): s for s in B.sh}
            pairs = [(s, bmap.get(s["name"].rstrip("?"))) for s in A.sh[1:]]
            pairs += [(None, s) for s in B.sh[1:] if s["name"].rstrip("?") not in
                      {x["name"].rstrip("?") for x in A.sh}]
        else:
            pairs = [(A.sh[i] if i < len(A.sh) else None, B.sh[i] if i < len(B.sh) else None)
                     for i in range(1, max(len(A.sh), len(B.sh)))]
        for sa, sb in pairs:
            if sa is None or sb is None:
                s = sa or sb
                print("  %-22s only in %s  size=0x%x" % (s["name"] or "#%d" % s["i"], "A" if sa else "B", s["size"]))
                continue
            if not (sa["flags"] & 2):        # SHF_ALLOC only: what the PPU runs
                continue
            name = sa["name"] or sb["name"] or "#%d" % sa["i"]
            ca, cb = A.sec_bytes(sa), B.sec_bytes(sb)
            moved = sa["addr"] != sb["addr"]
            resized = sa["size"] != sb["size"]
            content = ca != cb
            if not (moved or resized or content):
                print("  %-22s same   addr=0x%08x size=0x%07x" % (name, sa["addr"], sa["size"]))
                continue
            print("  %-22s A addr=0x%08x size=0x%07x | B addr=0x%08x size=0x%07x  %s%s%s"
                  % (name, sa["addr"], sa["size"], sb["addr"], sb["size"],
                     "MOVED(%+d) " % (sb["addr"] - sa["addr"]) if moved else "",
                     "RESIZED(%+d) " % (sb["size"] - sa["size"]) if resized else "",
                     "CONTENT" if content else ""))
            if content and sa["type"] != 8 and not moved and not resized:
                rngs, total = diff_ranges(ca, cb, sa["addr"])
                print("      %d byte(s) differ; first ranges:" % total)
                for va, ln in rngs:
                    sy = SA.at(va) or SB.at(va) or ""
                    print("        0x%08x +%-5d %s" % (va, ln, sy))
            elif content and sa["type"] != 8:
                # layout moved: find the first differing offset, that is where it starts
                n = min(len(ca), len(cb))
                k = next((i for i in range(n) if ca[i] != cb[i]), n)
                va = sa["addr"] + k
                print("      first difference at A 0x%08x %s" % (va, SA.at(va) or ""))
    print()

    # ---- 4. symbols ----------------------------------------------------------
    print("4. SYMBOLS")
    if not SA.s or not SB.s:
        print("  (need --syms-a/--syms-b: an unstripped ELF or obj/<dir>.elf.map for each side)")
        return
    def key(n):
        return n.split("  [")[0]
    ma, mb = {}, {}
    for addr, sz, n, k in SA.s:
        ma.setdefault(key(n), (addr, sz, n))
    for addr, sz, n, k in SB.s:
        mb.setdefault(key(n), (addr, sz, n))
    rx = re.compile(a.watch)
    rows = []
    for n in sorted(set(ma) | set(mb)):
        x, y = ma.get(n), mb.get(n)
        if x and y:
            if x[0] == y[0] and x[1] == y[1]:
                continue
            what = []
            if x[1] != y[1]:
                what.append("size %d -> %d (%+d)" % (x[1], y[1], y[1] - x[1]))
            if x[0] != y[0]:
                what.append("addr 0x%08x -> 0x%08x (%+d)" % (x[0], y[0], y[0] - x[0]))
            rows.append((0 if rx.search(n) else 1, n, ", ".join(what)))
        elif x:
            rows.append((0 if rx.search(n) else 1, n, "REMOVED (was 0x%08x size %d)" % (x[0], x[1])))
        else:
            rows.append((0 if rx.search(n) else 1, n, "ADDED at 0x%08x size %d" % (y[0], y[1])))
    rows.sort()
    watch = [r for r in rows if r[0] == 0]
    rest = [r for r in rows if r[0] == 1]
    print("  watch-list (%s):" % a.watch)
    for _, n, w in watch:
        print("    %-40s %s" % (n, w))
    if not watch:
        print("    (no watch-list symbol changed)")
    resized = [r for r in rest if "size" in r[2] or "ADDED" in r[2] or "REMOVED" in r[2]]
    print("  other symbols: %d changed (%d resized/added/removed, %d only moved)"
          % (len(rest), len(resized), len(rest) - len(resized)))
    for _, n, w in (rest if a.all_symbols else resized[:60]):
        print("    %-40s %s" % (n, w))


if __name__ == "__main__":
    main()
