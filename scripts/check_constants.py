"""Checks that the game's IDs, Title Update 6's hash and the build tools' license
link, kept in installer/deps.py for the installer, and the appended installer's
magic (installer/pipeline.py), have the same values where the program keeps them;
and that the file and folder names both sides use are spelt the same.

    python -I scripts/check_constants.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
from installer import deps  # noqa: E402


def read(path):
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        return f.read()


def python_string(path, name):
    """The value of `name = "..."` (or b"...") at the top level of a Python file."""
    found = re.search(rf'^{name} = b?"([^"]+)"', read(path), re.M)
    if not found:
        sys.exit(f"{path}: no {name} = \"...\" (this script reads it from there)")
    return found.group(1)


ENGINE_MAGIC = python_string("installer/pipeline.py", "ENGINE_MAGIC")

# (file, the constant's name there, its value for the installer)
CHECKS = [
    ("DAORecomp/launcher/install_view.cpp", "kMsvcLicenseUrl", f'"{deps.MSVC_LICENSE_URL}"'),
    ("DAORecomp/launcher/install_runner.cpp", "kEngineMagic", f'"{ENGINE_MAGIC}"'),
    ("DAORecomp/content/install.cpp", "kTitleId", f"0x{deps.GAME_TITLE_ID:08X}"),
    ("DAORecomp/content/install.cpp", "kMediaId", f"0x{deps.GAME_MEDIA_ID:08X}"),
    ("DAORecomp/kernel/title_update.cpp", "kTu6XexpSha1", f'"{deps.TU6_XEXP_SHA1.upper()}"'),
]

# (file, a name the installer uses, as a string literal there)
NAMES = [
    ("DAORecomp/program.cpp", python_string("installer/pipeline.py", "GAME_MARKER")),
    ("DAORecomp/kernel/title_update.cpp", python_string("installer/pipeline.py", "UPDATE_FOLDER")),
    ("DAORecomp/main.cpp", "DAORecomp.old.exe"),
    ("installer/pipeline.py", "DAORecomp.old.exe"),
    ("DAORecomp/launcher/install_runner.cpp", "DAORecomp-build"),
    ("installer/util.py", "DAORecomp-build"),
]


def main():
    failed = 0
    for path, name, expected in CHECKS:
        found = re.search(rf"\b{name}\s*=\s*([^;]+);", read(path))
        value = found.group(1).strip() if found else None
        if value is None or value.upper() != expected.upper():
            print(f"{path}: {name} is {value}, installer/deps.py has {expected}")
            failed += 1
    for path, name in NAMES:
        if f'"{name}"' not in read(path):
            print(f'{path}: no "{name}" (the name the other side uses)')
            failed += 1
    print("constants agree" if not failed else f"{failed} differ")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
