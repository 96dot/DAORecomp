"""Minimal XDVDFS (Xbox 360 ISO) reader: list files or extract one/all.

The image is the player's file, so its directory tables are read with limits and its
names are checked before they become paths."""
import hashlib
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Known game-partition offsets: plain XDVDFS, XGD1, XGD2, XGD3
PARTITION_OFFSETS = [0x0, 0x18300000, 0xFD90000, 0x2080000]
# Far larger than any real directory table (the game's biggest is a few KB), as
# DAORecomp/content/xiso.cpp has it.
MAX_TABLE_SIZE = 16 << 20
MAX_DEPTH = 32
# Names Windows can't hold, as scripts/stfs.py refuses them.
BAD_CHARS = set('<>:"/\\|?*') | {chr(c) for c in range(32)}
RESERVED = {"CON", "PRN", "AUX", "NUL"} | {f"{d}{n}" for d in ("COM", "LPT") for n in "0123456789¹²³"}


def find_partition(f):
    for off in PARTITION_OFFSETS:
        f.seek(off + 32 * SECTOR)
        if f.read(20) == MAGIC:
            return off
    raise ValueError("not an Xbox 360 game disc image (no XDVDFS header found)")


def _check_name(name):
    if (not name or name in (".", "..") or set(name) & BAD_CHARS or name[-1] in ". "
            or name.split(".")[0].upper() in RESERVED):
        raise ValueError(f"the disc image has a file name Windows can't hold ({name!r})")


def walk(f, base, sector, size, prefix="", depth=0, tables=None):
    """Iterate the binary-tree directory table; yields (path, is_dir, sector, size)."""
    # Each table once (a folder pointing back at its parent would loop).
    tables = set() if tables is None else tables
    if depth > MAX_DEPTH or size > MAX_TABLE_SIZE or sector in tables:
        raise ValueError("the disc image's folders are damaged")
    tables.add(sector)
    f.seek(base + sector * SECTOR)
    table = f.read(size)
    seen = set()
    stack = [0]
    while stack:
        off = stack.pop()
        if off in seen or off + 14 > len(table):
            continue
        seen.add(off)
        left, right, sec, sz, attr, nlen = struct.unpack_from("<HHIIBB", table, off)
        if left == 0xFFFF and right == 0xFFFF and sec == 0xFFFFFFFF:
            continue
        name = table[off + 14: off + 14 + nlen].decode("latin-1")
        _check_name(name)
        path = prefix + name
        is_dir = bool(attr & 0x10)
        yield path, is_dir, sec, sz
        if is_dir and sz:
            yield from walk(f, base, sec, sz, path + "/", depth + 1, tables)
        if left and left != 0xFFFF:
            stack.append(left * 4)
        if right and right != 0xFFFF:
            stack.append(right * 4)


def read_entries(f):
    """All entries of the ISO open as f: (base offset, [(path, is_dir, sector, size)])."""
    base = find_partition(f)
    f.seek(base + 32 * SECTOR + 20)
    root_sec, root_sz = struct.unpack("<II", f.read(8))
    return base, list(walk(f, base, root_sec, root_sz))


def hash_file(f, base, sector, size, algorithm="sha1"):
    """The hash of a file in the ISO open as f, read in chunks."""
    h = hashlib.new(algorithm)
    f.seek(base + sector * SECTOR)
    left = size
    while left:
        chunk = f.read(min(left, 1 << 24))
        if not chunk:
            raise ValueError("the disc image is truncated")
        h.update(chunk)
        left -= len(chunk)
    return h.hexdigest()


def extract(iso, out, wanted=None, progress=None):
    """Extracts the files of iso (or just the paths in wanted) under out. Files already
    there at their full size are kept (an interrupted extraction resumes).

    progress(done_bytes, total_bytes, path) is called after each file."""
    root = os.path.abspath(out)
    with open(iso, "rb") as f:
        base, entries = read_entries(f)
        files = [e for e in entries if not e[1] and (not wanted or e[0] in wanted)]
        total = sum(z for _, _, _, z in files)
        done = 0
        for p, d, s, z in files:
            dst = os.path.abspath(os.path.join(root, *p.split("/")))
            # (Names are checked in walk; this is the last word on where it goes.)
            if os.path.commonpath([root, dst]) != root:
                raise ValueError(f"the disc image has a file outside its folders ({p!r})")
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if not (os.path.isfile(dst) and os.path.getsize(dst) == z):
                f.seek(base + s * SECTOR)
                with open(dst, "wb") as o:
                    left = z
                    while left:
                        chunk = f.read(min(left, 1 << 24))
                        if not chunk:
                            raise IOError(f"{iso}: truncated at {p}")
                        o.write(chunk)
                        left -= len(chunk)
            done += z
            if progress:
                progress(done, total, p)
        return len(files)


def main():
    iso, cmd = sys.argv[1], sys.argv[2]
    if cmd == "list":
        with open(iso, "rb") as f:
            base, entries = read_entries(f)
        print(f"partition offset 0x{base:X}, {len(entries)} entries")
        for p, d, s, z in sorted(entries):
            print(f"{'D' if d else 'F'} {z:12d} {p}")
    elif cmd == "extract":
        extract(iso, sys.argv[3], set(sys.argv[4:]),
                lambda done, total, p: print("extracted", p))


if __name__ == "__main__":
    main()
