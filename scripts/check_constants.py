"""Checks that the game's IDs and Title Update 6's hash, kept in installer/deps.py
for the installer, have the same values where the program keeps them.

    python -I scripts/check_constants.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
from installer import deps  # noqa: E402

# (file, the constant's name there, its value in deps.py)
CHECKS = [
    ("DAORecomp/content/install.cpp", "kTitleId", f"0x{deps.GAME_TITLE_ID:08X}"),
    ("DAORecomp/content/install.cpp", "kMediaId", f"0x{deps.GAME_MEDIA_ID:08X}"),
    ("DAORecomp/kernel/title_update.cpp", "kTu6XexpSha1", f'"{deps.TU6_XEXP_SHA1.upper()}"'),
]


def main():
    failed = 0
    for path, name, expected in CHECKS:
        with open(os.path.join(ROOT, path), encoding="utf-8") as f:
            found = re.search(rf"\b{name}\s*=\s*([^;]+);", f.read())
        value = found.group(1).strip() if found else None
        if value is None or value.upper() != expected.upper():
            print(f"{path}: {name} is {value}, installer/deps.py has {expected}")
            failed += 1
    print("constants agree" if not failed else f"{failed} differ")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
