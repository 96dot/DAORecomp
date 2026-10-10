"""Diffs the draw lists in a userdata/flicker/NNN_draws.txt dump.

For each frame, prints the draw count and the draws that are missing from or
extra in it compared to the previous frame (with their positions in the list),
so a vanished object shows up as the draws missing in the flicker frame.

usage: python scripts/diff_draws.py userdata/flicker/005_draws.txt
"""
import collections, sys

def load(path):
    frames = []
    for line in open(path):
        line = line.split()
        if line[0] == "swap":
            frames.append((int(line[1]), len(line) > 2, []))
        else:
            frames[-1][2].append(tuple(line))
    return frames

def main():
    frames = load(sys.argv[1])
    prev = None
    for swap, flicker, draws in frames:
        print(f"swap {swap}{' (flicker)' if flicker else ''}: {len(draws)} draws")
        if prev is not None:
            a, b = collections.Counter(prev), collections.Counter(draws)
            missing, extra = a - b, b - a
            for name, diff, source in (("missing", missing, prev), ("extra", extra, draws)):
                if not diff:
                    continue
                positions = [i for i, d in enumerate(source) if d in diff]
                print(f"   {name} {sum(diff.values())} at positions {positions[:20]}")
                for d, n in list(diff.items())[:12]:
                    print(f"      ib {d[0]} vf {d[1]} count {d[2]} prim {d[3]}" + (f" x{n}" if n > 1 else ""))
        prev = draws

main()
