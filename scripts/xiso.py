"""Minimal XDVDFS (Xbox 360 ISO) reader: list files or extract one/all."""
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Known game-partition offsets: plain XDVDFS, XGD1, XGD2, XGD3
PARTITION_OFFSETS = [0x0, 0x18300000, 0xFD90000, 0x2080000]


def find_partition(f):
    for off in PARTITION_OFFSETS:
        f.seek(off + 32 * SECTOR)
        if f.read(20) == MAGIC:
            return off
    raise ValueError("not an Xbox 360 game disc image (no XDVDFS header found)")


def walk(f, base, sector, size, prefix=""):
    """Iterate the binary-tree directory table; yields (path, is_dir, sector, size)."""
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
        path = prefix + name
        is_dir = bool(attr & 0x10)
        yield path, is_dir, sec, sz
        if is_dir and sz:
            yield from walk(f, base, sec, sz, path + "/")
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


def extract(iso, out, wanted=None, progress=None):
    """Extracts the files of iso (or just the paths in wanted) under out.

    progress(done_bytes, total_bytes, path) is called after each file."""
    with open(iso, "rb") as f:
        base, entries = read_entries(f)
        files = [e for e in entries if not e[1] and (not wanted or e[0] in wanted)]
        total = sum(z for _, _, _, z in files)
        done = 0
        for p, d, s, z in files:
            dst = os.path.join(out, *p.split("/"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
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
