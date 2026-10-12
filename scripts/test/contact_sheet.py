"""contact_sheet.py <dir> [first last] [--columns N]

Puts a test run's captures (capture_N.png) side by side in <dir>/sheet.png, so a whole
run is checked by looking at one image. Needs Pillow."""
import re
import sys
from pathlib import Path

from PIL import Image, ImageDraw

THUMB = (400, 225)


def main():
    args = sys.argv[1:]
    columns = 4
    if "--columns" in args:
        i = args.index("--columns")
        columns = int(args[i + 1])
        del args[i:i + 2]
    if not args:
        sys.exit(__doc__)
    folder = Path(args[0])
    first, last = (int(args[1]), int(args[2])) if len(args) >= 3 else (0, 1 << 30)
    shots = []
    for p in folder.glob("capture_*.png"):
        m = re.fullmatch(r"capture_(\d+)\.png", p.name)
        if m and first <= int(m.group(1)) <= last:
            shots.append((int(m.group(1)), p))
    if not shots:
        sys.exit(f"No captures in {folder}")
    shots.sort()
    rows = (len(shots) + columns - 1) // columns
    sheet = Image.new("RGB", (THUMB[0] * columns, THUMB[1] * rows))
    draw = ImageDraw.Draw(sheet)
    for i, (n, p) in enumerate(shots):
        x, y = (i % columns) * THUMB[0], (i // columns) * THUMB[1]
        with Image.open(p) as im:
            sheet.paste(im.convert("RGB").resize(THUMB), (x, y))
        draw.text((x + 4, y + 4), str(n), fill=(255, 255, 0))
    out = folder / "sheet.png"
    sheet.save(out)
    print(f"{len(shots)} captures -> {out}")


if __name__ == "__main__":
    main()
