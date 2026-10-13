"""Checks that the game's IDs, the title updates' hashes and the build tools' license
link, kept in installer/deps.py for the installer, and the appended installer's
magic (installer/pipeline.py), have the same values where the program keeps them;
that the file and folder names both sides use are spelt the same; and that the
in-game Graphics tab offers the fields of view settings.h does.

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
    ("DAORecomp/content/content.h", "kTitleId", f"0x{deps.GAME_TITLE_ID:08X}"),
    ("DAORecomp/content/install.cpp", "kMediaId", f"0x{deps.GAME_MEDIA_ID:08X}"),
    ("DAORecomp/kernel/title_update.cpp", "kTu6XexpSha1", f'"{deps.TU6_XEXP_SHA1.upper()}"'),
    ("DAORecomp/kernel/title_update.cpp", "kTu7XexpSha1", f'"{deps.TU7_XEXP_SHA1.upper()}"'),
]

# (file, a name the installer uses, as a string literal there)
NAMES = [
    ("DAORecomp/program.cpp", python_string("installer/pipeline.py", "GAME_MARKER")),
    ("DAORecomp/kernel/title_update.cpp", python_string("installer/pipeline.py", "UPDATE_FOLDER")),
    ("DAORecomp/main.cpp", "DAORecomp.old.exe"),
    ("installer/pipeline.py", "DAORecomp.old.exe"),
    ("DAORecomp/launcher/install_runner.cpp", "DAORecomp-build"),
    ("DAORecomp/launcher/install_runner.cpp", python_string("scripts/build_installer_engine.py", "BUNDLE_NAME")),
    ("DAORecomp/launcher/install_runner.cpp", "python.exe"),
    ("scripts/build_installer_engine.py", "python.exe"),
    ("installer/util.py", "DAORecomp-build"),
]


def fov_lists():
    """settings.h's kFovSteps and kOriginalFov, and graphics_tab.as's FOV list and the
    value it labels original."""
    header, script = read("DAORecomp/settings.h"), read("DAORecomp/ui/graphics_tab.as")
    steps = re.search(r"kFovSteps\[\] = \{([^}]*)\}", header)
    original = re.search(r"kOriginalFov = (\d+);", header)
    tab = re.search(r"^var FOV = \[([^\]]*)\];", script, re.M)
    tab_original = re.search(r"FOV\[i\] == (\d+) \?", script)
    if not (steps and original and tab and tab_original):
        sys.exit("settings.h or graphics_tab.as: the field of view lists aren't where this script reads them")
    def numbers(found):
        return [int(n) for n in re.findall(r"\d+", found.group(1))]
    return numbers(steps), int(original.group(1)), numbers(tab), int(tab_original.group(1))


def main():
    failed = 0
    steps, original, tab, tab_original = fov_lists()
    if steps != tab or original != tab_original:
        print(f"graphics_tab.as: FOV {tab} (original {tab_original}), settings.h has {steps} (original {original})")
        failed += 1
    for path, name, expected in CHECKS:
        # Every `name = ...;` in the file (not `==`), so a second copy written that
        # way can't drift unseen.
        values = [v.strip() for v in re.findall(rf"\b{name}\s*=(?!=)\s*([^;]+);", read(path))]
        for value in values or [None]:
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
