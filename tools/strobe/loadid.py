#!/usr/bin/env python3
"""loadid.py FILE... -- one hash per file that answers "is this the same PROGRAM?"

LOADID = SHA-256 over the entry point and every PT_LOAD segment's
(vaddr, memsz, filesz, bytes).  That is exactly what the console maps and
runs, and nothing else: no section names, no symbols, no SELF wrapper.

    ELF (stripped or not)  -> computed directly
    EBOOT.BIN / .self      -> computed over the ELF recovered by self2elf.py
                              (needs $PSL1GHT or --keys for geohot SELFs)

Which files are expected to agree:
    EBOOT.BIN  ==  obj/<dir>.elf   (the stripped, sprxlinker-patched ELF that
                                    `make pkg` hands to make_self_npdrm)
    ./<dir>.elf (unstripped) differs from both in the .lib.stub bytes that
    sprxlinker patches after the link -- elfcmp.py shows exactly which.
Two EBOOTs of one ELF have different SHA-256s and the SAME LOADID.

usage: loadid.py [--keys oddkeys.h] FILE...     prints "<loadid>  <kind>  <file>"
"""

import argparse
import hashlib
import os
import signal
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import self2elf  # noqa: E402


def loadid_of_elf(elf):
    eh = self2elf.elf_header(elf)
    # The first PT_LOAD usually maps the ELF header itself, and `strip`
    # rewrites its section-table fields (e_shoff, e_shentsize, e_shnum,
    # e_shstrndx).  Nothing at run time reads them, so they are masked --
    # otherwise stripping alone would change the "program" hash.
    elf = bytearray(elf)
    elf[0x28:0x30] = b"\0" * 8
    elf[0x3A:0x40] = b"\0" * 6
    h = hashlib.sha256()
    h.update(struct.pack(">Q", eh["entry"]))
    for p in self2elf.elf_phdrs(elf, eh):
        if p["type"] != 1:          # PT_LOAD only
            continue
        h.update(struct.pack(">QQQ", p["vaddr"], p["memsz"], p["filesz"]))
        h.update(elf[p["offset"]:p["offset"] + p["filesz"]])
    return h.hexdigest()


def loadid_of_file(path, keys=None):
    data = open(path, "rb").read()
    if data[:4] == b"\x7fELF":
        return loadid_of_elf(data), "elf"
    if data[:4] == b"SCE\0":
        elf, method = self2elf.recover_elf(data, keys)
        return loadid_of_elf(elf), "self:" + method.split(" (")[0].replace(" ", "_")
    raise ValueError("neither ELF nor SELF")


def main():
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--keys")
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()
    rc = 0
    for f in a.files:
        try:
            lid, kind = loadid_of_file(f, a.keys)
            print("%s  %-28s %s" % (lid, kind, f))
        except (SystemExit, Exception) as e:     # self2elf.die() raises SystemExit
            print("%-64s  %-28s %s" % ("n/a", "error:%s" % str(e)[:40], f))
            rc = 1
    sys.exit(rc)


if __name__ == "__main__":
    main()
