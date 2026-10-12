"""Tests the two Xbox 360 package readers, scripts/stfs.py (the installer's) and
DAORecomp/content/stfs.cpp (the launcher's), on packages made here: a valid one
(folders, files in order and chained, runs across hash tables) and damaged or
hostile ones. Both readers must extract the same files, or refuse with the same
reason.

    python -I scripts/test_stfs.py [build/DAORecomp/DAORecomp.exe]

Without the program only the Python reader is tested.
"""
import os
import random
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stfs  # noqa: E402

BLOCK = stfs.BLOCK
L0 = stfs.LEVELS[0]
HEADER_SIZE = 0x971A
BASE = 0xA000


def offset(block):
    """Where data block `block` is (the readers' layout)."""
    physical = block
    for level in stfs.LEVELS:
        physical += (block + level) // level
        if block < level:
            break
    return BASE + (physical << 12)


def hash_table(block):
    """Where the level-0 hash table of `block` is."""
    table = 0
    if block >= L0:
        table = (block // L0) * (L0 + 1) + block // stfs.LEVELS[1] + 1
        if block >= stfs.LEVELS[1]:
            table += 1
    return BASE + (table << 12)


class Package:
    """A package being made: entries, data blocks and their chains."""

    def __init__(self):
        self.entries = []
        self.blocks = {}
        self.next = {}
        self.free = 1  # block 0 is the file table

    def folder(self, name, parent=0xFFFF, raw_name=None):
        return self._entry(raw_name or name.encode(), parent, 0x80, 0, 0, 0)

    def file(self, name, data, parent=0xFFFF, contiguous=True, raw_name=None, start=None, size=None):
        count = max(1, -(-len(data) // BLOCK))
        blocks = list(range(self.free, self.free + count))
        self.free += count
        if not contiguous:
            random.Random(len(data)).shuffle(blocks)
        for i, block in enumerate(blocks):
            self.blocks[block] = data[i * BLOCK:(i + 1) * BLOCK]
            if i + 1 < len(blocks):
                self.next[block] = blocks[i + 1]
        return self._entry(raw_name or name.encode(), parent, 0x40 if contiguous else 0, count,
                           blocks[0] if start is None else start, len(data) if size is None else size)

    def _entry(self, name, parent, flags, count, start, size, length=None):
        e = bytearray(0x40)
        e[:len(name[:40])] = name[:40]
        e[40] = flags | (len(name) if length is None else length)
        e[41:44] = count.to_bytes(3, "little")
        e[44:47] = count.to_bytes(3, "little")
        e[47:50] = start.to_bytes(3, "little")
        struct.pack_into(">HI", e, 50, parent, size)
        self.entries.append(e)
        return len(self.entries) - 1

    def write(self, path, magic=b"LIVE", read_only=True, volume_type=0, cut=0, claimed_blocks=None):
        total = self.free
        h = bytearray(BASE)
        h[:4] = magic
        struct.pack_into(">II", h, 0x340, HEADER_SIZE, stfs.CONTENT_MARKETPLACE)
        struct.pack_into(">I", h, 0x354, 0x186A1115)
        struct.pack_into(">I", h, 0x360, 0x454108C0)
        h[0x32C:0x340] = bytes(range(20))
        vd = bytearray(0x24)
        vd[0] = 0x24
        vd[2] = 1 if read_only else 0
        vd[3:5] = (1).to_bytes(2, "little")
        vd[5:8] = (0).to_bytes(3, "little")
        struct.pack_into(">I", vd, 0x1C, claimed_blocks or total)
        h[0x379:0x379 + 0x24] = vd
        struct.pack_into(">I", h, 0x3A9, volume_type)
        name = "Test package".encode("utf-16-be")
        h[0x411:0x411 + len(name)] = name
        out = bytearray(offset(total - 1) + BLOCK)
        out[:BASE] = h
        table = b"".join(self.entries)
        out[offset(0):offset(0) + len(table)] = table
        for block, data in self.blocks.items():
            out[offset(block):offset(block) + len(data)] = data
        for block, nxt in self.next.items():
            at = hash_table(block) + (block % L0) * 0x18 + 0x15
            out[at:at + 3] = nxt.to_bytes(3, "big")
        with open(path, "wb") as f:
            f.write(out[:len(out) - cut] if cut else out)


def data(n, seed):
    return random.Random(seed).randbytes(n)


def valid():
    """Folders, a file in order across hash tables, a chained one, a 40-character name."""
    p = Package()
    a = p.folder("a")
    b = p.folder("b", parent=a)
    p.file("small.txt", b"hello")
    p.file("big.bin", data(200 * BLOCK + 123, 1), parent=a)          # crosses the table at 170
    p.file("chained.bin", data(30 * BLOCK + 7, 2), parent=b, contiguous=False)
    p.file("x" * 36 + ".bin", data(5000, 3))
    p.folder("empty")
    return p, {"small.txt": b"hello", "a/big.bin": data(200 * BLOCK + 123, 1),
               "a/b/chained.bin": data(30 * BLOCK + 7, 2), "x" * 36 + ".bin": data(5000, 3)}


def named(raw):
    p = Package()
    p.file("f", b"x", raw_name=raw)
    return p


def case_list():
    """(name, package, write options, expected: dict of files or the start of the refusal)."""
    package, files = valid()
    cases = [("valid", package, {}, files)]
    bad_name = "has a file name Windows can't hold"
    for raw in (b"..", b"a:b", b"x.", b"x ", b"COM1.txt", b"lpt9", b"LPT\xc2\xb9", b"com\xc2\xb3.dat", b"nul",
                b"a\x01b"):
        cases.append((f"name {raw!r}", named(raw), {}, bad_name))
    # A name length past the field (41-63) is read as the field's 40 bytes.
    p = Package()
    p._entry(b"y" * 40, 0xFFFF, 0, 0, 0, 0, length=50)
    p.blocks[1] = b""
    cases.append(("long name length", p, {}, {"y" * 40: b""}))

    p = Package()
    p.file("A.txt", b"1")
    p.file("a.TXT", b"2")
    cases.append(("listed twice", p, {}, "damaged (a file listed twice)"))

    p = Package()
    p.file("f", b"1", parent=5)
    cases.append(("entry before its folder", p, {}, "damaged (entry before its folder)"))

    p = Package()
    parent = 0xFFFF
    for i in range(33):
        parent = p.folder(f"d{i}", parent=parent)
    p.file("f", b"1", parent=parent)
    cases.append(("folders too deep", p, {}, "damaged (folders too deep)"))

    p = Package()
    p.file("one", data(3 * BLOCK, 4))
    for i in range(3):
        p.file(f"same{i}", b"", start=1, size=3 * BLOCK)    # all on the same blocks
    cases.append(("files larger than the package", p, {}, "damaged (bad file size)"))

    # One block chained to itself, in a package claiming far more blocks than it holds:
    # read in a loop, it would write the claimed size.
    p = Package()
    p.file("loop", data(BLOCK, 6), contiguous=False, size=50 * BLOCK)
    p.next[1] = 1
    cases.append(("a chain looping on itself", p, {"claimed_blocks": 100000}, "damaged (bad file size)"))

    p = Package()
    p.file("f", data(BLOCK, 5), start=900)
    cases.append(("block out of range", p, {}, "damaged (block out of range)"))

    p, _ = valid()
    cases.append(("truncated", p, {"cut": 3 * BLOCK}, "damaged (truncated)"))
    p, _ = valid()
    cases.append(("writable", p, {"read_only": False}, "a package in the writable format"))
    p, _ = valid()
    cases.append(("SVOD", p, {"volume_type": 1}, "not an STFS package"))
    p, _ = valid()
    cases.append(("not a package", p, {"magic": b"XEX2"}, "not an Xbox 360 package"))
    return cases


def run_python(package, into):
    try:
        with stfs.Package(package) as pkg:
            pkg.extract(into)
        return "ok"
    except stfs.Error as e:
        return str(e)


def run_program(program, game, package, into):
    env = dict(os.environ, DAO_GAME=game, DAO_EXTRACT_PACKAGE=f"{package}|{into}")
    out = subprocess.run([program], env=env, capture_output=True, text=True, encoding="utf-8",
                         errors="replace", timeout=60).stdout
    for line in out.splitlines():
        if "Package: " in line:
            return line.split("Package: ", 1)[1]
    return "no result: " + out[-300:]


def check(result, into, expected):
    if isinstance(expected, str):
        return result.startswith(expected), f"refused: {result}"
    if result != "ok":
        return False, f"refused: {result}"
    found = {}
    for folder, _, files in os.walk(into):
        for name in files:
            path = os.path.join(folder, name)
            with open(path, "rb") as f:
                found[os.path.relpath(path, into).replace(os.sep, "/")] = f.read()
    return found == expected, f"{len(found)} files" + ("" if found == expected else ", not the expected ones")


def main():
    program = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else None
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        game = os.path.join(tmp, "game")
        os.makedirs(game)
        open(os.path.join(game, "default.xex"), "wb").close()   # the program only checks it's there
        for i, (name, package, options, expected) in enumerate(case_list()):
            path = os.path.join(tmp, f"case{i}")
            package.write(path, **options)
            readers = [("python", lambda into: run_python(path, into))]
            if program:
                readers.append(("c++", lambda into: run_program(program, game, path, into)))
            for reader, run in readers:
                into = os.path.join(tmp, f"out{i}_{reader}")
                ok, detail = check(run(into), into, expected)
                print(f"{'ok  ' if ok else 'FAIL'} {reader:6} {name}: {detail}")
                failed += not ok
    print("all passed" if not failed else f"{failed} failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
