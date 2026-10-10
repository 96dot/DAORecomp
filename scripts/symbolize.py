"""Resolves 'host stack:' offsets in a DAORecomp log using build/DAORecomp/DAORecomp.map."""
import bisect, re, sys

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
    log = sys.argv[1]
    mp = sys.argv[2] if len(sys.argv) > 2 else "build/DAORecomp/DAORecomp.map"
    addrs, names = load_map(mp)
    for line in open(log, errors="replace"):
        line = line.rstrip("\n")
        if "host stack:" not in line:
            print(line)
            continue
        head, offs = line.split("host stack:")
        out = []
        for o in offs.split():
            if o == "?":
                continue
            v = int(o, 16)
            i = bisect.bisect_right(addrs, v) - 1
            out.append(f"{demangle(names[i])}+{v - addrs[i]:X}" if i >= 0 else o)
        print(head + "host stack: " + " <- ".join(out))

main()
