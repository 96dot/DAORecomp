"""Compute explicit function boundaries for functions that XenonRecomp's
analyzer cuts short (frameless leaf functions with no .pdata):

- functions containing a switch, whose cases lie past the point where the
  analyzer stopped;
- functions it split at a conditional return or branch (memset's `bdzlr`
  tail, a `bne` to the next instruction): the first part then falls off its
  end and returns instead of running the rest. A split shows up as an emitted
  function that nothing calls or points to, right after an instruction that
  can fall through.

Reads the switch TOML and the list of functions XenonRecomp emitted (hex
addresses, or ppc_func_mapping.cpp), flows through each affected function
(branches + switch labels) and rewrites the `functions = [...]` block in
DAO.toml between the AUTO-FUNCTIONS markers. Entries already in that block are
kept: once fixed, a split no longer shows up in the emitted list.

usage: fix_functions.py <DAO.toml> <switch.toml> <emitted functions>
"""
import bisect
import re
import struct
import sys
sys.path.insert(0, __file__.rsplit('\\', 1)[0].rsplit('/', 1)[0])
from ppc import IMG, u32, pdata, branch_target, code_words

BLR, BCTR = 0x4E800020, 0x4E800420


def bc_target(a, w):
    bd = w & 0xFFFC
    if bd & 0x8000:
        bd -= 0x10000
    return (bd if w & 2 else a + bd) & 0xFFFFFFFF


def extent(start, limit, switches):
    """Highest address reachable by intra-function control flow, +4."""
    seen, work, hi = set(), [start], start
    while work:
        a = work.pop()
        while start <= a < limit and a not in seen:
            seen.add(a)
            hi = max(hi, a)
            w = u32(a)
            op = w >> 26
            if w == BLR:
                break
            if w == BCTR:
                if a in switches:
                    work.extend(l for l in switches[a] if start <= l < limit)
                break
            if op == 18 and not (w & 1):
                t = branch_target(a, w)
                if start <= t < limit:
                    work.append(t)
                break
            if op == 16:
                t = bc_target(a, w)
                if start <= t < limit:
                    work.append(t)
            if op == 19 and ((w >> 1) & 0x3FF) == 16 and ((w >> 21) & 0x14) == 0x14:
                break  # unconditional bclr variants
            a += 4
    return hi + 4


def falls_through(w):
    """False for instructions that never continue to the next one."""
    op = w >> 26
    if w in (BLR, BCTR):
        return False
    if op == 18 and not (w & 1):
        return False  # b
    if op == 19 and ((w >> 1) & 0x3FF) in (16, 528) and ((w >> 21) & 0x14) == 0x14 and not (w & 1):
        return False  # unconditional bclr/bcctr
    return True


def split_functions(emitted, pd_starts):
    """Emitted functions whose start is really the continuation of the previous one."""
    referenced = {branch_target(a, w) for a, w in code_words() if w >> 26 == 18}
    referenced |= set(struct.unpack(f'>{len(IMG) // 4}I', IMG[:len(IMG) // 4 * 4]))  # function pointers
    pd = set(pd_starts)
    out = []
    for prev, start in zip(emitted, emitted[1:]):
        if start in pd or start in referenced:
            continue
        w = u32(start - 4)
        if w and falls_through(w):
            out.append(prev)
    return out


def main():
    toml_path, sw_path, funcs_path = sys.argv[1:4]
    sw_text = open(sw_path).read()
    switches = {}
    for m in re.finditer(r'base = 0x([0-9A-F]+)\nr = \d+\ndefault = 0x[0-9A-F]+\nlabels = \[\n(.*?)\]', sw_text, re.S):
        switches[int(m.group(1), 16)] = [int(x, 16) for x in re.findall(r'0x([0-9A-F]+)', m.group(2))]

    emitted = sorted({int(x, 16) for x in re.findall(r'\b(?:0x|sub_)?([0-9A-Fa-f]{8})\b', open(funcs_path).read())})
    pd = sorted(pdata())
    pd_starts = [s for s, _ in pd]

    def in_pdata(a):
        i = bisect.bisect_right(pd_starts, a) - 1
        return i >= 0 and pd[i][0] <= a < pd[i][0] + pd[i][1]

    text = open(toml_path).read()
    m = re.search(r'# AUTO-FUNCTIONS.*?# END AUTO-FUNCTIONS', text, re.S)
    out = {}
    if m:
        out = {int(a, 16): int(n, 16) for a, n in re.findall(r'address = 0x([0-9A-F]+), size = 0x([0-9A-F]+)', m.group(0))}

    def fix(start):
        j = bisect.bisect_right(pd_starts, start)
        limit = pd_starts[j] if j < len(pd_starts) else start + 0x10000
        out[start] = max(out.get(start, 0), extent(start, limit, switches) - start)

    for start in split_functions(emitted, pd_starts):
        if not in_pdata(start):
            fix(start)
            print(f'split function {start:08X}: size 0x{out[start]:X}')

    done = set()
    for base, labels in switches.items():
        if in_pdata(base):
            continue
        start = emitted[bisect.bisect_right(emitted, base) - 1]
        if start not in done:
            done.add(start)
            fix(start)

    # Drop entries inside another one (the later pieces of a split function).
    for start in sorted(out):
        if any(a < start < a + n for a, n in out.items()):
            del out[start]

    # Keep anything hand-written outside the markers.
    body = ''.join(f'    {{ address = 0x{a:X}, size = 0x{s:X} }},\n' for a, s in sorted(out.items()))
    block = f'# AUTO-FUNCTIONS (scripts/fix_functions.py)\nfunctions = [\n{body}]\n# END AUTO-FUNCTIONS\n'
    if '# AUTO-FUNCTIONS' in text:
        text = re.sub(r'# AUTO-FUNCTIONS.*?# END AUTO-FUNCTIONS\n', lambda _: block, text, flags=re.S)
    else:
        text = text.replace('\n# Found by byte pattern', '\n' + block + '\n# Found by byte pattern', 1)
    open(toml_path, 'w').write(text)
    print(f'{len(out)} explicit function boundaries written')


if __name__ == '__main__':
    main()
