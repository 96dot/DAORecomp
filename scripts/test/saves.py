"""saves.py snapshot <file> | check <file> [--keep-new]

Guards the user's saves (userdata/content) around a test run. snapshot records every
file's size and modification time; check reports files the run changed or removed, and
removes the files and folders it created (unless --keep-new). Exit status 1 if an
existing save changed or disappeared."""
import json
import os
import sys
from pathlib import Path

CONTENT = Path("userdata/content")


def scan():
    files, dirs = {}, []
    if CONTENT.is_dir():
        for root, names, filenames in os.walk(CONTENT):
            dirs.extend(str(Path(root, n).relative_to(CONTENT)) for n in names)
            for n in filenames:
                p = Path(root, n)
                st = p.stat()
                files[str(p.relative_to(CONTENT))] = [st.st_size, st.st_mtime_ns]
    return files, dirs


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("snapshot", "check"):
        sys.exit(__doc__)
    record = Path(sys.argv[2])
    if sys.argv[1] == "snapshot":
        files, dirs = scan()
        record.write_text(json.dumps({"files": files, "dirs": dirs}), encoding="utf-8")
        return 0

    before = json.loads(record.read_text(encoding="utf-8"))
    files, dirs = scan()
    changed = [f for f in before["files"] if f in files and files[f] != before["files"][f]]
    missing = [f for f in before["files"] if f not in files]
    new = [f for f in files if f not in before["files"]]
    for f in changed:
        print(f"WARNING: save file changed by the run: {f}")
    for f in missing:
        print(f"WARNING: save file gone after the run: {f}")
    if new and "--keep-new" not in sys.argv:
        for f in new:
            (CONTENT / f).unlink()
        # Deepest first, and only folders the run created and left empty.
        for d in sorted(set(dirs) - set(before["dirs"]), key=len, reverse=True):
            try:
                (CONTENT / d).rmdir()
            except OSError:
                pass
        print(f"Removed {len(new)} file(s) the run saved.")
    elif new:
        print(f"Kept {len(new)} new file(s): " + ", ".join(new[:4]))
    return 1 if changed or missing else 0


if __name__ == "__main__":
    sys.exit(main())
