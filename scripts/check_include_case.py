"""Finds #includes whose case differs from the file on disk: harmless on Windows,
a build failure on Linux (case-sensitive). Run after install.py has fetched tools/.

usage: check_include_case.py [winsysroot]   (default: tools/winsysroot)

Windows SDK headers are only checked reliably on Linux: xwin names the files
differently there (and adds lowercase symlinks), so the CI run is the one to trust.
"""
import os
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SOURCES = ["DAORecomp", "DAORecompLib", "xenia/src"]
INCLUDE_DIRS = [
    "DAORecomp", "xenia", "xenia/src", "ppc",
    "tools/xenia-canary", "tools/xenia-canary/third_party",
    "tools/xenia-canary/third_party/fmt/include", "tools/xenia-canary/third_party/imgui",
    "tools/xenia-canary/third_party/DirectX-Headers/include",
    "tools/xenia-canary/third_party/DirectXShaderCompiler/include",
    "tools/xenia-canary/third_party/FFmpeg", "tools/xenia-canary/third_party/ffmpeg-xenia",
    "tools/xenia-canary/third_party/xxhash", "tools/xenia-canary/third_party/snappy",
    "tools/xenia-canary/third_party/dxbc", "tools/xenia-canary/third_party/tomlplusplus/include",
    "thirdparty/SDL/include", "tools/XenonRecomp/XenonUtils", "tools/XenonRecomp/thirdparty/simde",
    "tools/XenonRecomp/thirdparty/fmt/include", "tools/XenonRecomp/thirdparty/tomlplusplus/include",
    "tools/FidelityFX-SDK/Kits/FidelityFX/api/include",
    "tools/FidelityFX-SDK/Kits/FidelityFX/upscalers/include",
    "tools/FidelityFX-SDK/Kits/FidelityFX/framegeneration/include",
]
INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.M)

_listing = {}


def entries(directory):
    """{lowercase name: actual name} of a directory (cached)."""
    if directory not in _listing:
        try:
            names = os.listdir(directory)
        except OSError:
            names = []
        _listing[directory] = {n.lower(): n for n in names}
    return _listing[directory]


def resolve(base, include):
    """(found, exact): whether base/include exists ignoring case, and with exactly this case."""
    current, exact = str(base), True
    for part in include.replace("\\", "/").split("/"):
        if part in ("", "."):
            continue
        if part == "..":
            current = os.path.dirname(current)
            continue
        actual = entries(current).get(part.lower())
        if actual is None:
            return False, False
        exact = exact and actual == part
        current = os.path.join(current, actual)
    return True, exact


def main():
    sysroot = Path(sys.argv[1]) if len(sys.argv) > 1 else REPO / "tools" / "winsysroot"
    system_dirs = [p for p in sysroot.glob("Windows Kits/10/Include/*/*")]
    system_dirs += list(sysroot.glob("VC/Tools/MSVC/*/include"))
    if not system_dirs:
        print(f"note: no xwin sysroot at {sysroot}; Windows SDK includes are not checked")
    dirs = [REPO / d for d in INCLUDE_DIRS]

    problems = 0
    for root in SOURCES:
        for path in sorted((REPO / root).rglob("*")):
            if path.suffix not in (".h", ".hpp", ".c", ".cc", ".cpp", ".inc", ".hlsl", ".hlsli"):
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            for match in INCLUDE.finditer(text):
                kind, include = match.groups()
                bases = ([path.parent] if kind == '"' else []) + dirs
                results = [resolve(b, include) for b in bases]
                if any(exact for found, exact in results if found):
                    continue
                if any(found for found, _ in results):
                    line = text.count("\n", 0, match.start()) + 1
                    print(f"{path.relative_to(REPO)}:{line}: case differs on disk: {include}")
                    problems += 1
                    continue
                # Windows SDK/CRT: xwin adds all-lowercase symlinks, so either case works.
                sdk = [resolve(b, include) for b in system_dirs]
                if any(exact for found, exact in sdk if found) or (
                        include == include.lower() and any(found for found, _ in sdk)):
                    continue
                if any(found for found, _ in sdk):
                    line = text.count("\n", 0, match.start()) + 1
                    print(f"{path.relative_to(REPO)}:{line}: Windows SDK header case differs: {include}")
                    problems += 1
    print(f"{problems} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
