"""Shared helpers for poking at the dumped DAO image."""
import re
import struct
import capstone

BASE = 0x82000000
IMG = open(__file__.rsplit('scripts', 1)[0] + 'analysis/image.bin', 'rb').read()
TEXT = (0x821F0000, 0x821F0000 + 0xF3AB84)
BINK = (0x8312AC00, 0x8312AC00 + 0xFC08)
_cs = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)


def u32(a):
    return struct.unpack_from('>I', IMG, a - BASE)[0]


def imports():
    out = {}
    info = open(__file__.rsplit('scripts', 1)[0] + 'analysis/info.txt').read()
    for m in re.finditer(r'thunk (0x[0-9A-F]+) __imp__(\w+)', info):
        out[m.group(2)] = int(m.group(1), 16)
    return out


def code_words():
    for lo, hi in (TEXT, BINK):
        for a in range(lo, hi, 4):
            yield a, u32(a)


def branch_target(a, w):
    """Target of an I-form b/bl at address a, or None."""
    if w >> 26 != 18:
        return None
    li = w & 0x03FFFFFC
    if li & 0x02000000:
        li -= 0x04000000
    return (li if w & 2 else a + li) & 0xFFFFFFFF


def callers(target):
    return [a for a, w in code_words() if (w & 3) == 1 and branch_target(a, w) == target]


def dis(a, n=16):
    code = IMG[a - BASE:a - BASE + 4 * n]
    lines = []
    for i in range(n):
        insn = next(_cs.disasm(code[4 * i:4 * i + 4], a + 4 * i), None)
        word = code[4 * i:4 * i + 4].hex()
        lines.append(f"{a + 4*i:08X}  {word}  " + (f"{insn.mnemonic} {insn.op_str}" if insn else '???'))
    return '\n'.join(lines)


def pdata():
    """[(start, length_bytes)] from .pdata (start, packed: prolog:8 len:22 flags:2)."""
    lo, size = 0x82194C00, 0x4D098
    out = []
    for o in range(lo, lo + size, 8):
        start, packed = struct.unpack_from('>II', IMG, o - BASE)
        if start == 0:
            continue
        out.append((start, ((packed >> 8) & 0x3FFFFF) * 4))
    return out


def func_of(addr, pd=None):
    for s, l in (pd or pdata()):
        if s <= addr < s + l:
            return s, l
    return None
