"""pair_data.py <image A> <image B> <A func:B func>... -- <A address>...

Finds data addresses in build B (a title update) for addresses in build A, for porting
patches and hooks between builds. Functions matched between the builds (hex addresses)
form the same addresses with lis + an immediate at the same instruction positions; each
<A address> is looked up near such a pair. Images are the loaded XEX images (base
0x82000000), as the recompiler's analysis writes them."""
import struct
import sys

B = 0x82000000
IMM16 = {14, 15, 24, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55,
         58, 62}
v1 = open(sys.argv[1], 'rb').read()
t6 = open(sys.argv[2], 'rb').read()
args = sys.argv[3:]
cut = args.index('--')
pairs = [tuple(int(x, 16) for x in a.split(':')) for a in args[:cut]]
targets = [int(x, 16) for x in args[cut + 1:]]


def w(img, a):
    return struct.unpack_from('>I', img, a - B)[0]


def formed(img, f, n=2000):
    out = {}
    for i in range(n):
        x = w(img, f + 4 * i)
        if x >> 26 == 15 and (x >> 16) & 0x1F == 0:
            rd = (x >> 21) & 0x1F
            hi = x & 0xFFFF
            for k in range(1, 16):
                y = w(img, f + 4 * (i + k))
                if y >> 26 in IMM16 and (y >> 16) & 0x1F == rd:
                    lo = y & 0xFFFF
                    out[(i, k)] = ((hi << 16) + (lo - 0x10000 if lo & 0x8000 else lo)) & 0xFFFFFFFF
                if y >> 26 in (14, 15) and (y >> 21) & 0x1F == rd and (y >> 16) & 0x1F != rd:
                    break
    return out


mapping = {}
for a, b in pairs:
    fa, fb = formed(v1, a), formed(t6, b)
    for key, va in fa.items():
        if key in fb:
            mapping.setdefault(va, set()).add(fb[key])
for t in targets:
    best = [(t - a, a) for a in mapping if 0 <= t - a < 0x800]
    best.sort()
    if not best:
        print(f'{t:08X}: none')
        continue
    d, a = best[0]
    print(f'{t:08X}: via {a:08X}+{d:#x} -> ' + ', '.join(f'{x + d:08X}' for x in sorted(mapping[a])))
