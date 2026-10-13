"""Checks that the game's IDs, the title updates' hashes and the build tools' license
link, kept in installer/deps.py for the installer, and the appended installer's
magic (installer/pipeline.py), have the same values where the program keeps them;
that the file and folder names both sides use are spelt the same; and that the
in-game Graphics tab offers the fields of view and neural rendering options settings.h
does.

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
    # Neural rendering's: the installer's layout, which the launcher and the game check.
    ("xenia/src/xenia/gpu/d3d12/dao_upscale_neural.cc", python_string("installer/neural.py", "FOLDER")),
    ("xenia/src/xenia/gpu/d3d12/dao_upscale_neural.cc", python_string("installer/neural.py", "CONFIG_FOLDER")),
    ("xenia/src/xenia/gpu/d3d12/dao_upscale_neural.cc", "default-config.txt"),
    ("installer/neural.py", "default-config.txt"),
    ("DAORecomp/launcher/configure_view.cpp", python_string("installer/neural.py", "FOLDER")),
    ("DAORecomp/ui/graphics_menu.cpp", python_string("installer/neural.py", "FOLDER")),
    ("installer/neural.py", "SHA256SUMS"),
    ("DAORecomp/launcher/configure_view.cpp", "SHA256SUMS"),
    ("DAORecomp/ui/graphics_menu.cpp", "SHA256SUMS"),
    ("installer/pipeline.py", "LmxxfNrRuntime.dll"),
    ("xenia/src/xenia/gpu/d3d12/dao_upscale_neural.cc", "LmxxfNrRuntime.dll"),
    ("DAORecomp/launcher/configure_view.cpp", "LmxxfNrRuntime.dll"),
    ("DAORecomp/ui/graphics_menu.cpp", "LmxxfNrRuntime.dll"),
    ("DAORecomp/launcher/launcher_internal.h", "block0-ffn.f16"),
    ("xenia/src/xenia/gpu/d3d12/dao_upscale_neural.cc", "block0-ffn.f16"),
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


def neural_lists():
    """[(the option, settings.h's values, graphics_tab.as's)] for neural rendering's
    network sizes (kNeuralNetworks), passes (1 to kMaxNeuralPasses) and styles (0 to
    kMaxNeuralStyle)."""
    header, script = read("DAORecomp/settings.h"), read("DAORecomp/ui/graphics_tab.as")
    networks = re.search(r"kNeuralNetworks\[\] = \{([^}]*)\}", header)
    passes = re.search(r"kMaxNeuralPasses = (\d+);", header)
    styles = re.search(r"kMaxNeuralStyle = (\d+);", header)
    if not (networks and passes and styles):
        sys.exit("settings.h: neural rendering's lists aren't where this script reads them")
    out = []
    for key, values in [("NeuralNetwork", [int(n) for n in re.findall(r"\d+", networks.group(1))]),
                        ("NeuralPasses", list(range(1, int(passes.group(1)) + 1))),
                        ("NeuralStyle", list(range(0, int(styles.group(1)) + 1)))]:
        row = re.search(rf'key: "{key}"[^}}]*?values: \[([^\]]*)\]', script, re.S)
        if not row:
            sys.exit(f"graphics_tab.as: no {key} row with values (this script reads it)")
        out.append((key, values, [int(n) for n in re.findall(r"\d+", row.group(1))]))
    return out


def main():
    failed = 0
    steps, original, tab, tab_original = fov_lists()
    if steps != tab or original != tab_original:
        print(f"graphics_tab.as: FOV {tab} (original {tab_original}), settings.h has {steps} (original {original})")
        failed += 1
    for key, values, tab_values in neural_lists():
        if values != tab_values:
            print(f"graphics_tab.as: {key} {tab_values}, settings.h has {values}")
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
