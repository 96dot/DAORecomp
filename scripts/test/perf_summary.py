"""perf_summary.py <dao.log> [from s] [to s]

Summarises a run made with EXTRA="DAO_THREAD_STATS=2": swaps per second (fps) and the
busiest threads' CPU % over a time window (default: all of it), in two lines."""
import re
import sys
from collections import defaultdict


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    start = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0
    end = float(sys.argv[3]) if len(sys.argv) > 3 else float("inf")
    fps, threads = [], defaultdict(list)
    with open(sys.argv[1], encoding="utf-8", errors="replace") as log:
        for line in log:
            m = re.match(r"\[\s*([\d.]+)\].*Thread stats: ([\d.]+) swaps/s(.*)", line)
            if not m or not start <= float(m.group(1)) <= end:
                continue
            fps.append(float(m.group(2)))
            for name, pct in re.findall(r"([A-Za-z][\w:. ]*?) (\d+)%", m.group(3)):
                threads[name.strip()].append(int(pct))
    if not fps:
        sys.exit("No 'Thread stats' lines in that window (run with DAO_THREAD_STATS=2).")
    print(f"fps avg {sum(fps) / len(fps):.1f}, min {min(fps):.1f}, max {max(fps):.1f} "
          f"({len(fps)} samples)")
    busiest = sorted(threads.items(), key=lambda kv: -sum(kv[1]) / len(kv[1]))[:5]
    print("CPU: " + ", ".join(f"{n} {sum(v) / len(v):.0f}%" for n, v in busiest))


if __name__ == "__main__":
    main()
