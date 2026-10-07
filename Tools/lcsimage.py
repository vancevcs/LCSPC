#!/usr/bin/env python3
"""Relocate the decrypted LCS EBOOT into a flat RAM image, and search it.

The dumped EBOOT (PPSSPP DumpFileTypes=1) is a relocatable PRX (e_type 0xffa0) with section
headers. This applies its SHT_PSPREL relocations exactly the way PPSSPP's
ElfReader::LoadRelocations does, so the image matches what the game sees in PSP RAM (apart from
import stubs, which PPSSPP rewrites into syscalls at load).

  python Tools/lcsimage.py build                      writes <eboot>.ram.bin next to the EBOOT
  python Tools/lcsimage.py find "02 00 84 2C ? ? ? ?"  every aligned match, as absolute addresses
  python Tools/lcsimage.py words 0x08ab3378 8          raw words
  python Tools/lcsimage.py disasm 0x08957a90 40        capstone listing (pip install capstone)
  python Tools/lcsimage.py callers 0x089d58b0          every jal to an address
  python Tools/lcsimage.py xref 0x08b5e144 [end]       code that forms an address with lui + imm16

The EBOOT defaults to ../eboot/ULUS10041_EBOOT.BIN beside this repo (Dev/gtalcspc/eboot); set
LCS_EBOOT to use another. The base is 0x08804000, where PPSSPP loads the module (LCSAddresses.h).
This is the module as loaded, before any code has run: no heap and no runtime state. For those,
read a savestate or use the live debugger.
"""
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
WORKSPACE = os.path.dirname(os.path.dirname(HERE))
EBOOT = os.environ.get('LCS_EBOOT', os.path.join(WORKSPACE, 'eboot', 'ULUS10041_EBOOT.BIN'))
IMAGE = os.path.splitext(EBOOT)[0] + '.ram.bin'
BASE = 0x08804000

SHT_PSPREL = 0x700000A0
R_MIPS_16, R_MIPS_32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16 = 1, 2, 4, 5, 6


def build(path=EBOOT, base=BASE):
    d = open(path, 'rb').read()
    (e_phoff, e_shoff) = struct.unpack_from('<II', d, 0x1C)
    (e_phnum,) = struct.unpack_from('<H', d, 0x2C)
    (e_shnum, e_shstrndx) = struct.unpack_from('<HH', d, 0x30)
    segs = [struct.unpack_from('<8I', d, e_phoff + i * 32) for i in range(e_phnum)]
    size = max(s[2] + s[5] for s in segs)
    img = bytearray(size)
    seg_vaddr = []
    for (_t, off, va, _pa, fsz, _msz, _fl, _al) in segs:
        img[va:va + fsz] = d[off:off + fsz]
        seg_vaddr.append(base + va)

    def rd(a):
        return struct.unpack_from('<I', img, a - base)[0]

    def wr(a, v):
        struct.pack_into('<I', img, a - base, v & 0xFFFFFFFF)

    secs = [struct.unpack_from('<10I', d, e_shoff + i * 40) for i in range(e_shnum)]
    nrel = 0
    for s in secs:
        if s[1] != SHT_PSPREL:
            continue
        rels = [struct.unpack_from('<II', d, s[4] + i * 8) for i in range(s[5] // 8)]
        ops = []
        for (off, info) in rels:
            a = off + seg_vaddr[(info >> 8) & 0xFF]
            ops.append(rd(a) if base <= a < base + size - 3 else 0)
        for r, (off, info) in enumerate(rels):
            typ = info & 0xF
            a = off + seg_vaddr[(info >> 8) & 0xFF]
            rel = (info >> 16) & 0xFF
            to = seg_vaddr[rel] if rel < len(seg_vaddr) else 0
            op = ops[r]
            if typ == R_MIPS_32:
                op += to
            elif typ == R_MIPS_26:
                op = (op & 0xFC000000) | (((op & 0x03FFFFFF) + (to >> 2)) & 0x03FFFFFF)
            elif typ == R_MIPS_HI16:
                cur = (op & 0xFFFF) << 16
                hi = 0
                for t in range(r + 1, len(rels)):
                    tt = rels[t][1] & 0xF
                    if tt == R_MIPS_HI16:
                        continue
                    if tt not in (R_MIPS_LO16, R_MIPS_16):
                        continue
                    lo = ops[t] & 0xFFFF
                    lo = lo - 0x10000 if lo & 0x8000 else lo
                    cur = (cur + lo + to) & 0xFFFFFFFF
                    hi = ((cur >> 16) + ((cur >> 15) & 1)) & 0xFFFF
                    break
                op = (op & 0xFFFF0000) | hi
            elif typ == R_MIPS_LO16:
                op = (op & 0xFFFF0000) | (((op & 0xFFFF) + to) & 0xFFFF)
            elif typ == R_MIPS_16:
                op = (op & 0xFFFF0000) | (((op & 0xFFFF) + to) & 0xFFFF)
            else:
                continue
            wr(a, op)
            nrel += 1
    return bytes(img), nrel


def load():
    if not os.path.exists(IMAGE) or os.path.getmtime(IMAGE) < os.path.getmtime(EBOOT):
        img, _ = build()
        open(IMAGE, 'wb').write(img)
    return open(IMAGE, 'rb').read()


def pattern_regex(pat):
    out = b''
    for tok in pat.split():
        out += b'.' if tok in ('?', '??') else re.escape(bytes([int(tok, 16)]))
    return re.compile(out, re.S)


def find(img, pat, base=BASE, aligned=True):
    rx = pattern_regex(pat)
    hits = []
    pos = 0
    while True:
        m = rx.search(img, pos)
        if not m:
            break
        if not aligned or m.start() % 4 == 0:
            hits.append(base + m.start())
        pos = m.start() + 1
    return hits


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'build'
    if cmd == 'build':
        img, n = build()
        open(IMAGE, 'wb').write(img)
        print(f'{IMAGE}: {len(img):#x} bytes from {BASE:#010x}, {n} relocations applied')
    elif cmd == 'find':
        img = load()
        for h in find(img, sys.argv[2]):
            print(f'{h:#010x}')
    elif cmd == 'words':
        img = load()
        a = int(sys.argv[2], 16)
        n = int(sys.argv[3]) if len(sys.argv) > 3 else 8
        for i in range(n):
            print(f'{a + 4 * i:#010x}: {struct.unpack_from("<I", img, a + 4 * i - BASE)[0]:08x}')
    elif cmd == 'disasm':
        img = load()
        a = int(sys.argv[2], 16)
        n = int(sys.argv[3]) if len(sys.argv) > 3 else 32
        for line in disasm(img, a, n):
            print(line)
    elif cmd == 'callers':
        img = load()
        for c in callers(img, int(sys.argv[2], 16)):
            print(f'{c:#010x}')
    elif cmd == 'xref':
        img = load()
        lo = int(sys.argv[2], 16)
        hi = int(sys.argv[3], 16) if len(sys.argv) > 3 else lo + 1
        for (a, kind, ea) in xrefs(img, lo, hi):
            print(f'{a:#010x}  {kind:6s} {ea:#010x}  {disasm(img, a, 1)[0][20:]}')


_md = None


def disasm(img, addr, count):
    """Capstone listing. Allegrex VFPU opcodes come out as '?vfpu?' (capstone can't decode them)."""
    global _md
    if _md is None:
        from capstone import Cs, CS_ARCH_MIPS, CS_MODE_MIPS32, CS_MODE_LITTLE_ENDIAN
        _md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS32 | CS_MODE_LITTLE_ENDIAN)
    out = []
    for i in range(count):
        a = addr + 4 * i
        w = struct.unpack_from('<I', img, a - BASE)[0]
        got = list(_md.disasm(struct.pack('<I', w), a))
        if got:
            out.append(f'{a:08x}  {w:08x}  {got[0].mnemonic:<8s} {got[0].op_str}')
        else:
            out.append(f'{a:08x}  {w:08x}  ?vfpu?')
    return out


_MEM_OPS = {0x20: 'lb', 0x21: 'lh', 0x23: 'lw', 0x24: 'lbu', 0x25: 'lhu', 0x28: 'sb', 0x29: 'sh',
            0x2B: 'sw', 0x31: 'lwc1', 0x39: 'swc1', 0x09: 'addiu', 0x0D: 'ori'}


def xrefs(img, lo, hi, text_end=0x08B0B5D4):
    """Code that forms an address in [lo, hi) with `lui` + a 16-bit immediate. Looks back a few
    instructions for the lui, so it misses addresses built far from their use and anything gp-relative."""
    out = []
    n = (min(text_end, BASE + len(img)) - BASE) // 4
    words = struct.unpack_from(f'<{n}I', img, 0)
    lui_hi = {}
    for i, w in enumerate(words):
        op = w >> 26
        rs, rt = (w >> 21) & 31, (w >> 16) & 31
        if op == 0x0F:
            lui_hi[rt] = (i, (w & 0xFFFF) << 16)
            continue
        if op in _MEM_OPS and rs in lui_hi and i - lui_hi[rs][0] <= 24:
            imm = w & 0xFFFF
            if op != 0x0D and imm & 0x8000:
                imm -= 0x10000
            ea = (lui_hi[rs][1] + imm) & 0xFFFFFFFF
            if lo <= ea < hi:
                out.append((BASE + 4 * i, _MEM_OPS[op], ea))
        # A write to a register invalidates any lui it held (rd for R-type, rt for I-type).
        if op == 0:
            lui_hi.pop((w >> 11) & 31, None)
        elif op not in (0x04, 0x05, 0x01, 0x28, 0x29, 0x2B, 0x39, 0x3D, 0x02, 0x03):
            lui_hi.pop(rt, None)
    return out


def callers(img, target):
    """Every aligned `jal target`."""
    word = struct.pack('<I', (3 << 26) | ((target >> 2) & 0x03FFFFFF))
    out, start = [], 0
    while True:
        i = img.find(word, start)
        if i < 0:
            return out
        if i % 4 == 0:
            out.append(BASE + i)
        start = i + 1


if __name__ == '__main__':
    main()
