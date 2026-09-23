#!/usr/bin/env python3
"""self2elf.py -- recover the ELF that is inside a PSL1GHT-built EBOOT.BIN/.self.

WHY THIS EXISTS
    An EBOOT.BIN's SHA-256 identifies a DEPLOYED ARTIFACT, never a BUILD.
    PSL1GHT's `make pkg` runs geohot's make_self_npdrm, which seeds its RNG with
    time(NULL) and then compresses + AES-128-CTR-encrypts every ELF segment
    under fresh random keys (PSL1GHT tools/geohot/make_self.c, enumerate_segments
    and main).  Packaging the SAME ELF twice therefore gives two different
    EBOOT hashes.  The only honest way to ask "is this EBOOT the same program as
    that rebuild?" is to take the ELF back out and compare segment bytes.

WHAT IT HANDLES
    * geohot make_self_npdrm / make_self  (what `make pkg` produces).  The
      metadata is decrypted with npdrm_keypair_d / appold_keypair_d, which are
      read at run time from YOUR toolchain's
      $PSL1GHT/tools/geohot/include/oddkeys.h (nothing is embedded here).
      Every segment is checked against the HMAC-SHA1 stored in its own
      metadata, so a wrong key or a corrupt file fails loudly instead of
      producing a plausible-looking ELF.
    * fself / fself -n (fake SELF): the ELF is stored in the clear and is
      extracted byte-for-byte.

WHAT COMES OUT
    <out>.elf      the reconstructed ELF.  For geohot SELFs every LOADED byte
                   (all PT_LOAD/PT_TLS data), the ELF header, program headers
                   and section headers are exact; bytes that live in no segment
                   (typically .shstrtab/.comment of a stripped ELF) are not in
                   the SELF at all and are left zero, so compare segments or
                   sections, NOT the whole-file hash.  tools/strobe/elfcmp.py
                   does exactly that.
    <out>.json     per-segment and per-section SHA-256 of the recovered bytes.

usage: self2elf.py EBOOT.BIN [-o out_prefix] [--keys path/to/oddkeys.h]
"""

import argparse
import hashlib
import hmac
import json
import os
import re
import signal
import struct
import sys
import zlib

SCE_MAGIC = 0x53434500
ELF_MAGIC = b"\x7fELF"


def die(msg):
    sys.stderr.write("self2elf: " + msg + "\n")
    sys.exit(2)


def aes128_ctr(key, iv, data):
    """AES-128-CTR with a full 16-byte big-endian counter -- the semantics of
    OpenSSL's CRYPTO_ctr128_encrypt, which is what make_self used.

    Tries pycryptodome, then the `openssl` command line, then the
    `cryptography` module, so it runs on a stock WSL/Linux box with nothing
    installed.  BaseException, not ImportError: a broken `cryptography` wheel
    raises a pyo3 PanicException on import, which does not derive from
    Exception -- and it is tried last because it prints a panic when broken."""
    try:
        from Crypto.Cipher import AES
        from Crypto.Util import Counter
        ctr = Counter.new(128, initial_value=int.from_bytes(iv, "big"))
        return AES.new(key, AES.MODE_CTR, counter=ctr).decrypt(data)
    except BaseException:
        pass
    import subprocess
    try:
        r = subprocess.run(["openssl", "enc", "-d", "-aes-128-ctr", "-nopad",
                            "-K", key.hex(), "-iv", iv.hex()],
                           input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           check=True)
        if len(r.stdout) == len(data):
            return r.stdout
    except (OSError, subprocess.CalledProcessError):
        pass
    try:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        d = Cipher(algorithms.AES(key), modes.CTR(iv)).decryptor()
        return d.update(data) + d.finalize()
    except BaseException:
        pass
    die("no AES-128-CTR available: install the openssl CLI, or python3 "
        "'pycryptodome' / 'cryptography'")


def find_keys_file(explicit):
    if explicit:
        return explicit
    cands = []
    if os.environ.get("PSL1GHT"):
        cands.append(os.path.join(os.environ["PSL1GHT"], "tools/geohot/include/oddkeys.h"))
    if os.environ.get("PS3DEV"):
        cands.append(os.path.join(os.environ["PS3DEV"], "../PSL1GHT/tools/geohot/include/oddkeys.h"))
    for c in cands:
        if os.path.isfile(c):
            return c
    return None


def load_keys(path):
    txt = open(path, "r", errors="replace").read()
    out = {}
    for name in ("npdrm_keypair_d", "npdrm_keypair_e",
                 "appold_keypair_d", "appold_keypair_e"):
        m = re.search(r"u8\s+" + name + r"\s*\[\s*\]\s*=\s*\{([^}]*)\}", txt)
        if m:
            b = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))
            if len(b) == 0x40:
                out[name] = b
    return out


# --------------------------------------------------------------------------
# ELF64 big-endian helpers
# --------------------------------------------------------------------------
def elf_header(buf, off=0):
    if buf[off:off + 4] != ELF_MAGIC:
        die("no ELF header at 0x%x" % off)
    if buf[off + 4] != 2 or buf[off + 5] != 2:
        die("expected ELF64 big-endian (PPU)")
    (e_type, e_machine, e_version, e_entry, e_phoff, e_shoff, e_flags,
     e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum,
     e_shstrndx) = struct.unpack_from(">HHIQQQIHHHHHH", buf, off + 16)
    return dict(type=e_type, machine=e_machine, entry=e_entry, phoff=e_phoff,
                shoff=e_shoff, flags=e_flags, ehsize=e_ehsize,
                phentsize=e_phentsize, phnum=e_phnum, shentsize=e_shentsize,
                shnum=e_shnum, shstrndx=e_shstrndx)


def elf_phdrs(buf, eh, base=0):
    out = []
    for i in range(eh["phnum"]):
        (p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz,
         p_align) = struct.unpack_from(">IIQQQQQQ", buf, base + eh["phoff"] + i * eh["phentsize"])
        out.append(dict(index=i, type=p_type, flags=p_flags, offset=p_offset,
                        vaddr=p_vaddr, filesz=p_filesz, memsz=p_memsz, align=p_align))
    return out


def elf_shdrs(buf, eh, shoff=None):
    shoff = eh["shoff"] if shoff is None else shoff
    out = []
    for i in range(eh["shnum"]):
        (sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link,
         sh_info, sh_addralign, sh_entsize) = struct.unpack_from(
             ">IIQQQQIIQQ", buf, shoff + i * eh["shentsize"])
        out.append(dict(index=i, name_off=sh_name, type=sh_type, flags=sh_flags,
                        addr=sh_addr, offset=sh_offset, size=sh_size, link=sh_link,
                        info=sh_info, addralign=sh_addralign, entsize=sh_entsize))
    return out


def sha(b):
    return hashlib.sha256(b).hexdigest()


# --------------------------------------------------------------------------
# SELF parsing
# --------------------------------------------------------------------------
def parse_sce(data):
    if len(data) < 0x70:
        die("file too small to be a SELF")
    magic, hdrver, flags, hdrtype, esize, shsize, exsize = struct.unpack_from(">IIHHIQQ", data, 0)
    if magic != SCE_MAGIC:
        die("not a SELF (magic 0x%08x)" % magic)
    ext = struct.unpack_from(">10Q", data, 0x20)
    return dict(hdrver=hdrver, flags=flags, hdrtype=hdrtype, esize=esize,
                shsize=shsize, exsize=exsize,
                e_magic=ext[0], ihoff=ext[1], ehoff=ext[2], phoff=ext[3],
                shoff=ext[4], pmoff=ext[5], svoff=ext[6], cfoff=ext[7])


def extract_fself(data, h):
    """fself: the complete ELF sits in the clear at header_len."""
    # (Deliberately no looser fallback: a geohot SELF also carries a plaintext
    # ELF HEADER at e_ehoff, followed by encrypted data, and treating that as
    # a plain ELF would return garbage labelled "byte-exact".)
    start, length = h["shsize"], h["exsize"]
    if data[start:start + 4] == ELF_MAGIC and start + length == len(data):
        return bytes(data[start:start + length]), "fself (plain, byte-exact)"
    return None, None


def extract_geohot(data, h, keys):
    """geohot make_self(_npdrm): decrypt metadata, then every segment."""
    md_off = h["esize"] + 0x20
    md_file = bytes(data[md_off:md_off + 0x40])
    variant = None
    for tag in ("npdrm", "appold"):
        if keys.get(tag + "_keypair_e") == md_file:
            variant = tag
            break
    if variant is None:
        return None, None, "metadata key block matches neither npdrm_keypair_e nor " \
                           "appold_keypair_e -- not made by this toolchain's make_self"
    kd = keys[variant + "_keypair_d"]
    enc = bytes(data[md_off + 0x40:h["shsize"]])
    dec = aes128_ctr(kd[0x00:0x10], kd[0x20:0x30], enc)

    sig_off, version, seg_count, crypt_len, unk2, _pad = struct.unpack_from(">QIIIIQ", dec, 0)
    if version != 1 or not (0 < seg_count < 64) or crypt_len * 0x10 > len(dec):
        return None, None, "metadata decrypted to nonsense (version=%d segs=%d) -- wrong key?" \
                           % (version, seg_count)
    segs = []
    for i in range(seg_count):
        (s_off, s_size, s_type, s_num, s_hashed, s_sha1_idx, s_encf, s_erk_idx,
         s_riv_idx, s_compf) = struct.unpack_from(">QQIIIIIIII", dec, 0x20 + i * 0x30)
        segs.append(dict(offset=s_off, size=s_size, number=s_num, sha1_idx=s_sha1_idx,
                         enc=s_encf, erk_idx=s_erk_idx, riv_idx=s_riv_idx, comp=s_compf))
    crypt = dec[0x20 + seg_count * 0x30:0x20 + seg_count * 0x30 + crypt_len * 0x10]

    # ELF header + phdrs are stored in the clear at e_ehoff.
    eh = elf_header(data, h["ehoff"])
    phdrs = elf_phdrs(data, eh, base=h["ehoff"])
    out = bytearray(h["exsize"])
    head_len = eh["phoff"] + eh["phnum"] * eh["phentsize"]
    out[0:head_len] = data[h["ehoff"]:h["ehoff"] + head_len]

    checks = []
    for s in segs:
        blob = bytes(data[s["offset"]:s["offset"] + s["size"]])
        if s["enc"] == 3:
            erk = crypt[s["erk_idx"] * 0x10:s["erk_idx"] * 0x10 + 0x10]
            riv = crypt[s["riv_idx"] * 0x10:s["riv_idx"] * 0x10 + 0x10]
            blob = aes128_ctr(erk, riv, blob)
        # geohot hashes the (compressed) payload BEFORE encryption with an
        # HMAC-SHA1 keyed by the 0x40-byte hmac that follows the digest.
        base = s["sha1_idx"] * 0x10
        want = crypt[base:base + 0x14]
        hkey = crypt[base + 0x20:base + 0x60]
        got = hmac.new(hkey, blob, hashlib.sha1).digest()
        ok = hmac.compare_digest(got, want)
        checks.append(ok)
        if not ok:
            die("segment %d fails its HMAC-SHA1 check -- extraction is NOT trustworthy" % s["number"])
        if s["comp"] == 2:
            blob = zlib.decompress(blob)
        ph = phdrs[s["number"]]
        if len(blob) != ph["filesz"]:
            die("segment %d inflated to %d bytes, phdr says %d" % (s["number"], len(blob), ph["filesz"]))
        out[ph["offset"]:ph["offset"] + len(blob)] = blob

    # Section headers are stored in the clear at e_shoff (absent for SPRX).
    if h["shoff"] and eh["shnum"]:
        shlen = eh["shnum"] * eh["shentsize"]
        out[eh["shoff"]:eh["shoff"] + shlen] = data[h["shoff"]:h["shoff"] + shlen]

    note = "geohot make_self%s (decrypted, %d/%d segment HMACs verified)" % (
        "_npdrm" if variant == "npdrm" else "", sum(checks), len(checks))
    return bytes(out), note, None


def report(elf, method):
    eh = elf_header(elf)
    phdrs = elf_phdrs(elf, eh)
    shdrs = elf_shdrs(elf, eh) if eh["shoff"] and eh["shoff"] + eh["shnum"] * eh["shentsize"] <= len(elf) else []
    covered = [(p["offset"], p["offset"] + p["filesz"]) for p in phdrs if p["filesz"]]

    def in_segments(a, b):
        return any(lo <= a and b <= hi for lo, hi in covered)

    names = {}
    if shdrs and eh["shstrndx"] < len(shdrs):
        st = shdrs[eh["shstrndx"]]
        tab = elf[st["offset"]:st["offset"] + st["size"]]
        for s in shdrs:
            end = tab.find(b"\0", s["name_off"])
            n = tab[s["name_off"]:end].decode("ascii", "replace") if end > s["name_off"] else ""
            names[s["index"]] = n
    segs = []
    for p in phdrs:
        segs.append(dict(p, sha256=sha(elf[p["offset"]:p["offset"] + p["filesz"]])))
    secs = []
    for s in shdrs:
        e = dict(s, name=names.get(s["index"], ""))
        if s["type"] == 8:          # SHT_NOBITS (.bss/.tbss): size is the content
            e["sha256"] = None
            e["recoverable"] = True
        elif s["size"] and in_segments(s["offset"], s["offset"] + s["size"]):
            e["sha256"] = sha(elf[s["offset"]:s["offset"] + s["size"]])
            e["recoverable"] = True
        else:
            e["sha256"] = None
            e["recoverable"] = method.startswith("fself")
        secs.append(e)
    return dict(method=method, elf_size=len(elf), entry=eh["entry"],
                segments=segs, sections=secs)


def recover_elf(data, keys_path=None):
    """(elf_bytes, method) for the bytes of an EBOOT.BIN / .self."""
    h = parse_sce(data)
    elf, method = extract_fself(data, h)
    if elf is not None:
        return elf, method
    kf = find_keys_file(keys_path)
    if not kf:
        die("not a plain fself, and no oddkeys.h found: pass --keys "
            "$PSL1GHT/tools/geohot/include/oddkeys.h (or export PSL1GHT)")
    keys = load_keys(kf)
    if not keys:
        die("no keypairs parsed from " + kf)
    elf, method, err = extract_geohot(data, h, keys)
    if elf is None:
        die(err)
    return elf, method


def main():
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)   # quiet when piped to head
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("self_file")
    ap.add_argument("-o", "--out", help="output prefix (default: <input>.extracted)")
    ap.add_argument("--keys", help="path to PSL1GHT tools/geohot/include/oddkeys.h")
    a = ap.parse_args()

    data = open(a.self_file, "rb").read()
    elf, method = recover_elf(data, a.keys)

    prefix = a.out or (a.self_file + ".extracted")
    with open(prefix + ".elf", "wb") as f:
        f.write(elf)
    rep = report(elf, method)
    rep["self_file"] = os.path.abspath(a.self_file)
    rep["self_sha256"] = sha(data)
    with open(prefix + ".json", "w") as f:
        json.dump(rep, f, indent=1)

    print("SELF     %s" % a.self_file)
    print("  sha256 %s  (artifact identity only -- see header comment)" % rep["self_sha256"])
    print("method   %s" % method)
    print("ELF      %s.elf  (%d bytes)" % (prefix, len(elf)))
    for s in rep["segments"]:
        if s["filesz"]:
            print("  seg %-2d type=0x%08x vaddr=0x%08x filesz=0x%07x memsz=0x%07x sha256=%s"
                  % (s["index"], s["type"], s["vaddr"], s["filesz"], s["memsz"], s["sha256"][:16]))
    print("report   %s.json" % prefix)


if __name__ == "__main__":
    main()
