"""Finds functions and data of the disc's executable in another build of it
(Title Update 6): a function by its code with branch offsets and address
immediates masked out, data through an instruction that addresses it in a
function found that way.

usage: match_build.py <other image.bin> <v1 address>...
    A function address prints its match; a data address (not a function start)
    is looked up through the functions that address it.
"""
import functools
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ppc  # the disc's image (DAO_VARIANT unset)

BASE = ppc.BASE
V1 = ppc.IMG
OTHER = open(sys.argv[1], 'rb').read()
# Instructions whose low 16 bits are an immediate that moves with the layout:
# addi, addis, ori, and loads/stores (lwz..stfdu, lbz.., ld/std).
IMM16 = {14, 15, 24, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55,
         58, 62}


def words(img, address, count):
    return list(struct.unpack_from(f'>{count}I', img, address - BASE))


def mask(word):
    op = word >> 26
    if op == 18:            # b/bl
        return word & 0xFC000003
    if op == 16:            # bc
        return word & 0xFFFF0003
    if op in IMM16:
        return word & 0xFFFF0000
    return word


@functools.cache
def pdata():
    return ppc.pdata()


@functools.cache
def other_masked():
    return [mask(w) for w in struct.unpack_from(f'>{len(OTHER) // 4}I', OTHER)]


def function_size(address):
    for s, length in pdata():
        if s == address:
            return length
    later = [s for s, _ in pdata() if s > address]
    return (min(later) - address) if later else 0x40


def find(address, length=None):
    count = (length or function_size(address)) // 4
    pattern = [mask(w) for w in words(V1, address, count)]
    other = other_masked()
    first = pattern[0]
    hits = []
    for i, w in enumerate(other):
        if w == first and other[i:i + count] == pattern:
            hits.append(BASE + 4 * i)
    return hits


def referencing(target):
    """(function, index of the lis, index of the low part) of v1 code that forms target."""
    hi = (target + 0x8000) >> 16 & 0xFFFF
    lo = target & 0xFFFF
    out = []
    pd = pdata()
    for a, w in ppc.code_words():
        if w >> 26 == 15 and (w >> 16) & 0x1F == 0 and w & 0xFFFF == hi:   # lis rD, hi
            rd = (w >> 21) & 0x1F
            for k in range(1, 12):
                w2 = ppc.u32(a + 4 * k)
                if w2 >> 26 in IMM16 and (w2 >> 16) & 0x1F == rd and w2 & 0xFFFF == lo:
                    f = ppc.func_of(a, pd)
                    if f:
                        out.append((f[0], f[1], (a - f[0]) // 4, (a + 4 * k - f[0]) // 4))
                    break
    return out


def main():
    starts = {s for s, _ in pdata()}
    for arg in sys.argv[2:]:
        address = int(arg, 16)
        refs = [] if address in starts else referencing(address)
        if not refs:
            hits = find(address)
            # (Many hits: not a function, or one too short to tell apart, e.g. zeros.)
            shown = ", ".join(f"{h:08X}" for h in hits[:8]) + (f" and {len(hits) - 8} more" if len(hits) > 8 else "")
            print(f'{address:08X} function -> {shown or "no match"}')
            continue
        found = set()
        for f, length, lis, low in refs:
            for h in find(f, length):
                w1 = struct.unpack_from('>I', OTHER, h + 4 * lis - BASE)[0]
                w2 = struct.unpack_from('>I', OTHER, h + 4 * low - BASE)[0]
                hi = w1 & 0xFFFF
                lo = w2 & 0xFFFF
                found.add(((hi << 16) + (lo - 0x10000 if lo & 0x8000 else lo)) & 0xFFFFFFFF)
        print(f'{address:08X} data -> {", ".join(f"{x:08X}" for x in sorted(found)) or "no match"}')


if __name__ == '__main__':
    main()
