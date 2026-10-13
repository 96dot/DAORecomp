"""flicker.py <dir>... [--worst N]

Scores flicker in consecutive frames (a DAO_FRAME_HISTORY save, or captures taken every
frame), in each <dir>'s PNGs in frame order:
- flicker: how far each frame is from the average of its neighbours; ratio: that over
  the change between frames, so runs with different motion compare (lower is steadier;
  compare against the same walk with the effect off);
- the frames and blocks most like a one-frame flash (different from both neighbours,
  which agree with each other), to look at.
Needs numpy and Pillow."""
import re
import sys
from pathlib import Path

import numpy as np
from PIL import Image

SIZE = (480, 270)
BLOCK = 10


def frames(folder):
    files = sorted(Path(folder).glob("*.png"),
                   key=lambda p: [int(t) if t.isdigit() else t for t in re.split(r"(\d+)", p.name)])
    return files, [np.asarray(Image.open(f).convert("RGB").resize(SIZE), dtype=np.float32) for f in files]


def blocks(a, b):
    h, w = SIZE[1] // BLOCK * BLOCK, SIZE[0] // BLOCK * BLOCK
    return np.abs(a - b).mean(2)[:h, :w].reshape(h // BLOCK, BLOCK, w // BLOCK, BLOCK).mean((1, 3))


def main():
    args = sys.argv[1:]
    worst = 5
    if "--worst" in args:
        i = args.index("--worst")
        worst = int(args[i + 1])
        del args[i:i + 2]
    if not args:
        print(__doc__)
        return 2
    for folder in args:
        files, x = frames(folder)
        if len(x) < 3:
            print(f"{folder}: {len(x)} frames, need 3 or more")
            continue
        # Without the edges (UI).
        gray = [f.mean(2)[40:230, 60:420] for f in x]
        flicker = np.mean([np.abs(gray[t] - 0.5 * (gray[t - 1] + gray[t + 1])).mean() for t in range(1, len(x) - 1)])
        motion = np.mean([np.abs(gray[t] - gray[t - 1]).mean() for t in range(1, len(x))])
        print(f"{folder}: {len(x)} frames, flicker {flicker:.2f}, motion {motion:.2f}, "
              f"ratio {flicker / max(motion, 1e-6):.3f}")
        flashes = []
        for t in range(1, len(x) - 1):
            before, after, across = blocks(x[t], x[t - 1]), blocks(x[t], x[t + 1]), blocks(x[t - 1], x[t + 1])
            score = np.minimum(before, after) - 1.5 * across
            at = np.unravel_index(np.argmax(score), score.shape)
            flashes.append((float(score[at]), t, at, int((score > 8).sum())))
        for score, t, at, count in sorted(flashes, reverse=True)[:worst]:
            print(f"  flash {score:.1f} at {files[t].name}, block (row {at[0]}, column {at[1]}) "
                  f"of {BLOCK}x{BLOCK} at {SIZE[0]}x{SIZE[1]}; {count} blocks over 8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
