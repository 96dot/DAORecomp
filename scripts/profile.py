"""Summarizes a DAO_PROFILE sample file: self and inclusive time per function.

usage: python scripts/profile.py userdata/profile_5.txt [top N] [map]
"""
import bisect, collections, re, sys

def load_map(path):
    syms = []
    for line in open(path, errors="replace"):
        m = re.match(r"\s*0001:[0-9a-f]+\s+(\S+)\s+([0-9a-f]{16})\s", line)
        if m:
            syms.append((int(m.group(2), 16) - 0x140000000, m.group(1)))
    syms.sort()
    return [a for a, _ in syms], [n for _, n in syms]

def demangle(name):
    m = re.match(r"\?(\w+)@", name)
    return m.group(1) if m else name

def main():
    path = sys.argv[1]
    top = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    addrs, names = load_map(sys.argv[3] if len(sys.argv) > 3 else "build/DAORecomp/DAORecomp.map")

    def sym(v):
        if v >= 0x100000000:  # outside the exe: a system DLL
            return "<system>"
        i = bisect.bisect_right(addrs, v) - 1
        return demangle(names[i]) if i >= 0 else hex(v)

    self_counts, incl_counts, callers = collections.Counter(), collections.Counter(), collections.Counter()
    total = 0
    for line in open(path, errors="replace"):
        if "host stack:" not in line:
            continue
        frames = [sym(int(o, 16)) for o in line.split("host stack:")[1].split() if o != "?"]
        if not frames:
            continue
        total += 1
        # Attribute system-DLL leaves to the first exe frame (the import that called it).
        leaf = next((f for f in frames if f != "<system>"), "<system>")
        self_counts[leaf] += 1
        for f in set(frames):
            incl_counts[f] += 1
        first_exe = [f for f in frames if f != "<system>"]
        if len(first_exe) > 1:
            callers[(first_exe[0], first_exe[1])] += 1

    print(f"{total} samples\n\n-- self (leaf, system DLL time charged to its caller) --")
    for name, n in self_counts.most_common(top):
        print(f"{100 * n / total:6.1f}%  {name}")
    print("\n-- inclusive --")
    for name, n in incl_counts.most_common(top):
        print(f"{100 * n / total:6.1f}%  {name}")
    print("\n-- hottest leaf <- caller --")
    for (a, b), n in callers.most_common(top // 2):
        print(f"{100 * n / total:6.1f}%  {a} <- {b}")

main()
