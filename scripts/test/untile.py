"""untile.py <data file> <out.png> <width> <height> [--offset N] [--order argb|rgba|bgra]

Turns raw Xbox 360 Tiled2D 32bpp texture data (as the game wrote it, e.g. a save's
scrnshot.bin) into a PNG, to check what the CPU got from the GPU. <width> is the stored
width (DAO's 436-wide thumbnail is stored 448 wide: 32-pixel aligned). Bytes are in
memory order; DAO's 8888 thumbnails are argb. Needs Pillow."""
import sys

from PIL import Image


def tiled_offset(x, y, pitch, bpp_log2):
    # XGAddress2DTiledOffset, as Xenia's texture_address::Tiled2D.
    aligned_width = (pitch + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (aligned_width >> 5)) << (bpp_log2 + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << bpp_log2
    offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return (((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
            (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F))


def option(name, default):
    if name in sys.argv:
        return sys.argv[sys.argv.index(name) + 1]
    return default


def main():
    args = sys.argv[1:]
    for name in ("--offset", "--order"):
        if name in args:
            i = args.index(name)
            del args[i:i + 2]
    if len(args) != 4:
        sys.exit(__doc__)
    src, dst, width, height = args[0], args[1], int(args[2]), int(args[3])
    offset = int(option("--offset", "0"), 0)
    order = option("--order", "argb")
    if sorted(order) != sorted("argb"):
        sys.exit("--order: a permutation of argb")
    r, g, b = order.index("r"), order.index("g"), order.index("b")
    data = open(src, "rb").read()[offset:]
    image = Image.new("RGB", (width, height))
    pixels = image.load()
    for y in range(height):
        for x in range(width):
            o = tiled_offset(x, y, width, 2)
            if o + 4 > len(data):
                sys.exit(f"Data too short for {width}x{height} (pixel {x},{y})")
            px = data[o:o + 4]
            pixels[x, y] = (px[r], px[g], px[b])
    image.save(dst)
    print(f"{width}x{height} -> {dst}")


if __name__ == "__main__":
    main()
