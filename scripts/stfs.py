"""Reads Xbox 360 STFS packages in the read-only format Xbox Live delivers them in
(LIVE/PIRS: downloadable content and title updates), after Xenia's reader. The
packages are the player's files and treated as untrusted: every block, name and
size is checked before it's used.

DAORecomp/content/stfs.cpp is the same reader for the launcher (which has no
Python); keep the two in step.

    python -I stfs.py info <package>...        header and file list
    python -I stfs.py extract <package> <dir>  its files into dir (which must not exist)
"""
import os
import struct
import sys

BLOCK = 0x1000
# Data blocks covered by one hash table at each level. The blocks under one level-0
# table are next to each other in the file.
LEVELS = (170, 170 * 170, 170 * 170 * 170)
MAGICS = (b"CON ", b"LIVE", b"PIRS")
CONTENT_MARKETPLACE = 0x00000002   # downloadable content
CONTENT_INSTALLER = 0x000B0000     # title update
# Names Windows can't hold, or that would leave the folder.
BAD_CHARS = set('<>:"/\\|?*') | {chr(c) for c in range(32)}
# (COM and LPT also with a superscript 1-3.)
RESERVED = {"CON", "PRN", "AUX", "NUL"} | {f"{d}{n}" for d in ("COM", "LPT") for n in "0123456789\u00b9\u00b2\u00b3"}
MAX_TABLE_BLOCKS = 1024     # 65,536 entries
MAX_DEPTH = 32


class Error(Exception):
    pass


def _u24(b, at):
    return b[at] | (b[at + 1] << 8) | (b[at + 2] << 16)


def _text(raw):
    """A UTF-16 string from the header, without control characters (it's the package's to choose)."""
    text = raw.decode("utf-16-be", "replace").split("\0")[0]
    return "".join(c for c in text if c >= " " and c != "\x7f")


class Package:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        try:
            self._read_header()
        except BaseException:
            self.f.close()
            raise

    def close(self):
        self.f.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _read_header(self):
        # The fields are at the offsets Xenia's stfs_container_device.cc and Free60's
        # STFS page give.
        self.size = os.fstat(self.f.fileno()).st_size
        h = self.f.read(0x1800)
        if len(h) < 0x1800 or h[:4] not in MAGICS:
            raise Error("not an Xbox 360 package")
        self.magic = h[:4].decode("ascii")
        self.header_size, = struct.unpack_from(">I", h, 0x340)
        self.content_type, = struct.unpack_from(">I", h, 0x344)
        self.media_id, self.version, self.base_version, self.title_id = struct.unpack_from(">IIII", h, 0x354)
        vd = h[0x379:0x379 + 0x24]
        read_only = vd[2] & 1
        self.table_blocks = vd[3] | (vd[4] << 8)
        self.table_block = _u24(vd, 5)
        self.total_blocks, = struct.unpack_from(">I", vd, 0x1C)
        volume_type, = struct.unpack_from(">I", h, 0x3A9)
        self.display_name = _text(h[0x411:0x411 + 0x80])
        if volume_type != 0:
            raise Error("not an STFS package (an SVOD one, as games on demand are)")
        if not read_only:
            raise Error("a package in the writable format (as saves are), not downloadable content")
        self.base = (self.header_size + BLOCK - 1) // BLOCK * BLOCK
        if not 0x1000 <= self.header_size <= 0x10000 or self.base > self.size:
            raise Error("damaged (bad header size)")
        if not 0 < self.table_blocks <= MAX_TABLE_BLOCKS or self.total_blocks >= LEVELS[2]:
            raise Error("damaged (bad volume descriptor)")
        self.files = self._read_table()

    # Where data block `index` is: after the hash tables in front of it.
    def _offset(self, index):
        block = index
        for base in LEVELS:
            block += (index + base) // base
            if index < base:
                break
        return self.base + (block << 12)

    def _read_block(self, index, length=BLOCK, count=1):
        """`count` blocks from `index`, all under one level-0 hash table: `length` bytes."""
        if not (0 <= index and index + count <= self.total_blocks
                and index // LEVELS[0] == (index + count - 1) // LEVELS[0]):
            raise Error("damaged (block out of range)")
        offset = self._offset(index)
        if offset + length > self.size:
            raise Error("damaged (truncated)")
        self.f.seek(offset)
        return self.f.read(length)

    def _next_block(self, index):
        """The next block of a chain, from the level-0 hash table's entry."""
        if index < LEVELS[0]:
            table = 0
        else:
            table = (index // LEVELS[0]) * (LEVELS[0] + 1) + index // LEVELS[1] + 1
            if index >= LEVELS[1]:
                table += 1
        at = self.base + (table << 12) + (index % LEVELS[0]) * 0x18
        if at + 0x18 > self.size:
            raise Error("damaged (truncated hash table)")
        self.f.seek(at + 0x14)
        return struct.unpack(">I", self.f.read(4))[0] & 0xFFFFFF

    def _read_table(self):
        dirs = {0xFFFF: ("", 0)}
        files = []
        seen = set()
        # Files may share blocks: their sizes together are held to the package's.
        total = 0
        index = 0
        block = self.table_block
        for _ in range(self.table_blocks):
            data = self._read_block(block)
            for j in range(BLOCK // 0x40):
                e = data[j * 0x40:(j + 1) * 0x40]
                if e[0] == 0:
                    break
                flags = e[40]
                # (Decoded as the launcher's FromUtf8 does.)
                name = e[:min(flags & 0x3F, 40)].decode("utf-8", "replace")
                start = _u24(e, 47)
                parent, length = struct.unpack_from(">HI", e, 50)
                if (not name or name in (".", "..") or set(name) & BAD_CHARS or name[-1] in ". "
                        or name.split(".")[0].upper() in RESERVED):
                    raise Error(f"has a file name Windows can't hold ({name!r})")
                if parent not in dirs:
                    raise Error("damaged (entry before its folder)")
                folder, depth = dirs[parent]
                path = folder + name
                if depth >= MAX_DEPTH:
                    raise Error("damaged (folders too deep)")
                if path.lower() in seen:
                    raise Error("damaged (a file listed twice)")
                seen.add(path.lower())
                if flags & 0x80:
                    dirs[index] = (path + "/", depth + 1)
                else:
                    total += length
                    # (No more than the package holds: a chain looping on one block
                    # would otherwise be read over and over.)
                    if total > min(self.total_blocks << 12, self.size - self.base):
                        raise Error("damaged (bad file size)")
                    files.append((path, length, start, bool(flags & 0x40)))
                index += 1
            block = self._next_block(block)
        return files

    def read_file(self, start, length, contiguous, out):
        """Writes a file's data to out (a binary stream)."""
        block = start
        left = length
        while left > 0:
            # Up to a level-0 table's blocks at a time, where the file's blocks are in order.
            count = min(-(-left // BLOCK), LEVELS[0] - block % LEVELS[0]) if contiguous else 1
            n = min(count * BLOCK, left)
            out.write(self._read_block(block, n, count))
            left -= n
            if left > 0:
                block = block + count if contiguous else self._next_block(block)

    def extract(self, into, progress=None):
        """Extracts every file into `into`, a folder that must not exist yet."""
        into = os.path.abspath(into)
        os.makedirs(into)
        total = sum(f[1] for f in self.files) or 1
        done = 0
        for path, length, start, contiguous in self.files:
            target = os.path.join(into, *path.split("/"))
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with open(target, "xb") as out:
                self.read_file(start, length, contiguous, out)
            done += length
            if progress:
                progress(done / total)


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "info":
        for p in sys.argv[2:]:
            try:
                with Package(p) as pkg:
                    print(f"== {os.path.basename(p)}  {pkg.magic}  type {pkg.content_type:08X}  "
                          f"title {pkg.title_id:08X}  media {pkg.media_id:08X}  {pkg.display_name!r}")
                    for path, length, _, _ in pkg.files:
                        print(f"   {length:>11}  {path}")
            except (Error, OSError) as e:
                print(f"{os.path.basename(p)}: {e}")
    elif len(sys.argv) == 4 and sys.argv[1] == "extract":
        with Package(sys.argv[2]) as pkg:
            pkg.extract(sys.argv[3])
            print(f"{len(pkg.files)} files")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
