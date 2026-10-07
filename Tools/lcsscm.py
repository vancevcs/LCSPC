#!/usr/bin/env python3
"""Map every LCS script opcode to the code that runs it.

LCS dispatches script commands the GTA III way, not the VCS way: CRunningScript::ProcessOneCommand
(0x08957a90) reads the opcode, strips the NOT bit (0x8000, stored at script+0x216) and walks a
ladder of `slti` range checks, calling one ProcessCommandsXToY function per range. Each of those is
a compiler switch: `addiu rX, a1, -BASE; sltiu rY, rX, N; ... lw at, TABLE(at); jr at`.

This reads the ladder and every jump table out of the relocated EBOOT image (tools/lcsimage.py)
and prints `opcode  case-address  range-fn  name`. A case address equal to the switch's default
label means the opcode is not implemented in this build.

    python Tools/lcsscm.py                 full table
    python Tools/lcsscm.py 022A 02D3       just these opcodes (hex)
    python Tools/lcsscm.py --grep health   names containing "health"

Names come from Sanny Builder's LCS mode: LCSSCM.INI (1,279 rows), overridden by lcs.json where it
has one. Set SANNY_LCS to the folder holding them; the default is the VCS workspace's copy. They
are community names in LCS's own numbering; check a handler's CollectParameters count before
trusting one.
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lcsimage as L  # noqa: E402

PROCESS_ONE_COMMAND = 0x08957A90
SANNY_LCS = os.environ.get('SANNY_LCS', os.path.join(L.WORKSPACE, '..', 'gtavcspc', 'sanny builder', 'data', 'lcs'))
LCS_JSON = os.path.join(SANNY_LCS, 'lcs.json')
LCS_INI = os.path.join(SANNY_LCS, 'LCSSCM.INI')


def u32(img, a):
    return struct.unpack_from('<I', img, a - L.BASE)[0]


def s16(x):
    x &= 0xFFFF
    return x - 0x10000 if x & 0x8000 else x


def ladder(img):
    """[(lo, hi, fn)] from ProcessOneCommand's slti/jal chain."""
    out, lo, a = [], 0, PROCESS_ONE_COMMAND
    limit = None
    for i in range(200):
        w = u32(img, a + 4 * i)
        op = w >> 26
        if op == 0x0A and (w >> 21) & 31 == 5:          # slti rt, a1, imm
            limit = s16(w)
        elif op == 0x03 and limit is not None:           # jal
            fn = (w & 0x03FFFFFF) << 2 | ((a + 4 * i) & 0xF0000000)
            out.append((lo, limit, fn))
            lo, limit = limit, None
        elif w == 0x03E00008 and out:                    # jr ra
            break
    return out


def switch(img, fn):
    """(base, count, table, default) for the jump-table switch at the top of fn."""
    base, count, hi, table, default = 0, None, None, None, None
    for i in range(80):
        a = fn + 4 * i
        w = u32(img, a)
        op = w >> 26
        rs, rt = (w >> 21) & 31, (w >> 16) & 31
        if op == 0x09 and rs in (4, 5) and s16(w) < 0 and count is None:   # addiu rX, a0/a1, -BASE
            base = -s16(w)
        elif op == 0x0B and count is None:                                  # sltiu
            count = w & 0xFFFF
        elif op == 0x04 and rs != 0 and rt == 0 and count is not None and default is None:  # beqz
            default = a + 4 + 4 * s16(w)
        elif op == 0x0F and rt == 1:                                        # lui at
            hi = (w & 0xFFFF) << 16
        elif op == 0x23 and rs == 1 and rt == 1 and hi is not None:         # lw at, lo(at)
            table = (hi + s16(w)) & 0xFFFFFFFF
        elif w == 0x00200008:                                               # jr at
            return base, count, table, default
    raise ValueError(f'no switch found at {fn:#010x}')


def handlers(img):
    rows = []
    for lo, hi, fn in ladder(img):
        base, count, table, default = switch(img, fn)
        for op in range(lo, hi):
            idx = op - base
            if 0 <= idx < count:
                case = u32(img, table + 4 * idx)
            else:
                case = None                    # handled outside the jump table (if-chain or default)
            rows.append((op, case, fn, case is not None and case == default))
    return rows


def names():
    """lcs.json names (28 of them) over LCSSCM.INI rows (1,279)."""
    out = {}
    try:
        for line in open(LCS_INI, encoding='latin1'):
            line = line.strip()
            if len(line) > 5 and line[4] == '=' and all(c in '0123456789ABCDEFabcdef' for c in line[:4]):
                np, _, text = line[5:].partition(',')
                out[int(line[:4], 16)] = (text.strip(), int(np) if np.lstrip('-').isdigit() else None)
    except OSError:
        pass
    try:
        j = json.load(open(LCS_JSON, encoding='utf-8'))
        for ext in j.get('extensions', []):
            for c in ext.get('commands', []):
                out[int(c['id'], 16)] = (c.get('name', ''), c.get('num_params'))
    except OSError:
        pass
    return out


def main():
    img = L.load()
    nm = names()
    rows = handlers(img)
    args = sys.argv[1:]
    want, grep = None, None
    if args[:1] == ['--grep']:
        grep = args[1].upper()
    elif args:
        want = {int(x, 16) for x in args}
    for op, case, fn, dflt in rows:
        name, np = nm.get(op, ('', None))
        if want is not None and op not in want:
            continue
        if grep is not None and grep not in name.upper():
            continue
        where = f'{case:#010x}' if case is not None else ' (outside)'
        tag = '  [unimplemented]' if dflt else ''
        print(f'{op:04X}  {where}  {fn:#010x}  {np if np is not None else "?":>2}  {name}{tag}')


if __name__ == '__main__':
    main()
