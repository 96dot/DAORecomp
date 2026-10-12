"""Draws DAORecomp's icon (DAORecomp/res/daorecomp.ico): a sword over a shield in
the launcher's colours. Original art (nothing from the game, which can't be in the
repository). Needs Pillow; the .ico is committed, so builds don't run this.

Usage: python scripts/make_icon.py [out.ico] [preview.png]
"""
import pathlib
import sys

from PIL import Image, ImageDraw

REPO = pathlib.Path(__file__).resolve().parent.parent
OUT = REPO / "DAORecomp" / "res" / "daorecomp.ico"
SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)
S = 1024  # drawn at this size, then scaled down
# The tile on the canvas: its rectangle and corner radius.
TILE = [16, 16, S - 17, S - 17]
TILE_RADIUS = 190

BACKGROUND_TOP = (46, 30, 22)
BACKGROUND_BOTTOM = (14, 10, 9)
BORDER = (150, 108, 58)
SHIELD = (128, 22, 20)
SHIELD_DARK = (70, 10, 10)
SHIELD_RIM = (176, 128, 64)
BLADE = (214, 212, 206)
BLADE_EDGE = (140, 138, 134)
GOLD = (196, 146, 64)
GOLD_DARK = (120, 82, 34)
GRIP = (62, 36, 24)


def shield_outline(cx, top, width, height, steps=48):
    """A heater shield: straight top, sides curving to a point."""
    half = width / 2
    points = [(cx - half, top), (cx + half, top)]
    straight = height * 0.38
    for i in range(steps + 1):
        t = i / steps
        # From the right shoulder down to the point, easing into it.
        x = cx + half * (1 - t ** 1.6)
        y = top + straight + (height - straight) * (1 - (1 - t) ** 2) ** 0.85
        points.append((x, y))
    for x, y in reversed(points[2:-1]):
        points.append((2 * cx - x, y))
    return points


def draw():
    image = Image.new("RGBA", (S, S), (0, 0, 0, 0))

    # The tile: a rounded square, darker towards the bottom, with a rim.
    gradient = Image.new("RGBA", (S, S))
    gd = ImageDraw.Draw(gradient)
    for y in range(S):
        t = y / (S - 1)
        gd.line([(0, y), (S, y)], fill=tuple(round(a + (b - a) * t) for a, b in
                                              zip(BACKGROUND_TOP, BACKGROUND_BOTTOM)) + (255,))
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle(TILE, radius=TILE_RADIUS, fill=255)
    image.paste(gradient, (0, 0), mask)
    d = ImageDraw.Draw(image)
    d.rounded_rectangle(TILE, radius=TILE_RADIUS, outline=BORDER + (255,), width=22)

    # On the 1024 canvas: the shield spans y 250-850, the sword (pommel to point)
    # y 92-900 down the middle.
    # The shield.
    cx = S / 2
    outline = shield_outline(cx, 250, 560, 600)
    d.polygon(outline, fill=SHIELD_RIM)
    inner = shield_outline(cx, 282, 500, 548)
    d.polygon(inner, fill=SHIELD)
    # Its left half in shade.
    shade = Image.new("L", (S, S), 0)
    sd = ImageDraw.Draw(shade)
    sd.polygon(inner, fill=110)
    sd.rectangle([cx, 0, S, S], fill=0)
    image.paste(Image.new("RGBA", (S, S), SHIELD_DARK + (255,)), (0, 0), shade)

    # The sword, point down, over the shield.
    blade_top, blade_bottom, blade_half = 300, 900, 34
    d.polygon([(cx - blade_half, blade_top), (cx + blade_half, blade_top),
               (cx + blade_half, blade_bottom - 90), (cx, blade_bottom),
               (cx - blade_half, blade_bottom - 90)], fill=BLADE)
    d.polygon([(cx - blade_half, blade_top), (cx - 6, blade_top), (cx - 6, blade_bottom - 20),
               (cx - blade_half, blade_bottom - 90)], fill=BLADE_EDGE)
    # Grip, crossguard and pommel.
    d.rectangle([cx - 24, 150, cx + 24, 300], fill=GRIP)
    for y in range(170, 290, 30):
        d.line([(cx - 24, y), (cx + 24, y + 14)], fill=GOLD_DARK, width=8)
    d.rounded_rectangle([cx - 190, 280, cx + 190, 330], radius=24, fill=GOLD)
    d.rounded_rectangle([cx - 190, 310, cx + 190, 330], radius=12, fill=GOLD_DARK)
    d.ellipse([cx - 52, 92, cx + 52, 196], fill=GOLD)
    d.ellipse([cx - 22, 122, cx + 22, 166], fill=GOLD_DARK)
    return image


def main():
    out = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else OUT
    image = draw()
    out.parent.mkdir(parents=True, exist_ok=True)
    big = image.resize((max(SIZES), max(SIZES)), Image.Resampling.LANCZOS)
    big.save(out, format="ICO", sizes=[(s, s) for s in SIZES])
    if len(sys.argv) > 2:
        preview = Image.new("RGBA", (sum(SIZES) + 10 * len(SIZES), 256), (40, 40, 40, 255))
        x = 0
        for s in SIZES:
            frame = big.resize((s, s), Image.Resampling.LANCZOS)
            preview.paste(frame, (x, 0), frame)
            x += s + 10
        preview.save(sys.argv[2])
    print(out)


if __name__ == "__main__":
    main()
