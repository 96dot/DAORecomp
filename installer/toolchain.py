"""Build tools: CMake/Ninja (pip, private venv), LLVM (system or pinned download) and
the MSVC CRT + Windows SDK splatted by xwin."""
import glob
import os
import re
import shutil
import subprocess
import venv
from dataclasses import dataclass
from pathlib import Path

from . import deps
from .util import EXE, HOST, IS_WINDOWS, REPO, InstallError, clean_env, download, extract_tar, run

TOOLS = REPO / "tools"
DOWNLOADS = TOOLS / "downloads"
VENV = TOOLS / "venv"
LLVM_DIR = TOOLS / "llvm"
XWIN_DIR = TOOLS / "xwin"
WINSYSROOT = TOOLS / "winsysroot"


@dataclass
class Toolchain:
    cmake: Path
    ninja: Path
    llvm_bin: Path
    winsysroot: Path

    def tool(self, name):
        return self.llvm_bin / (name + EXE)


# ------------------------------------------------------------------ cmake/ninja

def ensure_build_tools(reporter):
    bindir = VENV / ("Scripts" if IS_WINDOWS else "bin")
    cmake, ninja = bindir / ("cmake" + EXE), bindir / ("ninja" + EXE)
    stamp = VENV / "dao-packages.txt"
    wanted = "\n".join(deps.PIP_PACKAGES)
    if cmake.exists() and ninja.exists() and stamp.exists() and stamp.read_text() == wanted:
        reporter.log(f"CMake and Ninja: ready ({', '.join(deps.PIP_PACKAGES)})")
        return cmake, ninja
    if not (bindir / ("python" + EXE)).exists():
        reporter.log(f"Creating a Python virtual environment in {VENV}")
        try:
            venv.EnvBuilder(with_pip=True, clear=True).create(VENV)
        except Exception as e:
            raise InstallError(f"Could not create a Python virtual environment ({e}). On Debian/Ubuntu "
                               "install the python3-venv package.")
    run(reporter, [bindir / ("python" + EXE), "-m", "pip", "install", "--disable-pip-version-check",
                   "-q", *deps.PIP_PACKAGES], env=clean_env())
    stamp.write_text(wanted)
    return cmake, ninja


# ------------------------------------------------------------------------ LLVM

LLVM_TOOLS = ["clang-cl", "lld-link", "llvm-lib", "llvm-rc"] + ([] if IS_WINDOWS else ["clang", "clang++"])


def llvm_version(bindir):
    """The major version of the LLVM in bindir if it has every tool the build needs."""
    if not all((Path(bindir) / (t + EXE)).exists() for t in LLVM_TOOLS):
        return None
    try:
        out = subprocess.run([str(Path(bindir) / ("clang-cl" + EXE)), "--version"], capture_output=True,
                             text=True, timeout=30).stdout
    except (OSError, subprocess.TimeoutExpired):
        return None
    match = re.search(r"clang version (\d+)", out)
    return int(match.group(1)) if match else None


def _system_llvm_candidates():
    if os.environ.get("DAO_LLVM_BIN"):
        yield Path(os.environ["DAO_LLVM_BIN"])
    for name in ("clang-cl", "lld-link"):
        found = shutil.which(name)
        if found:
            yield Path(os.path.realpath(found)).parent
            yield Path(found).parent
    if IS_WINDOWS:
        yield Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "LLVM" / "bin"
    else:
        # Debian/Ubuntu keep complete toolchains in /usr/lib/llvm-<N>/bin.
        for d in sorted(glob.glob("/usr/lib/llvm-*/bin"), key=lambda p: -int(re.sub(r"\D", "", p) or 0)):
            yield Path(d)
        yield Path("/usr/lib/llvm/bin")


def ensure_llvm(reporter, choice="auto"):
    """choice: 'auto' (pinned copy in tools/llvm, else a system LLVM >= LLVM_MIN_MAJOR,
    else download), 'download', or a directory containing the tools."""
    if choice not in ("auto", "download"):
        bindir = Path(choice)
        if (bindir / "bin").is_dir():
            bindir = bindir / "bin"
        version = llvm_version(bindir)
        if version is None:
            raise InstallError(f"{bindir} lacks one of {', '.join(LLVM_TOOLS)}.")
        reporter.log(f"LLVM {version}: {bindir}")
        return bindir

    pinned = LLVM_DIR / "bin"
    version = llvm_version(pinned)
    if version is not None:
        reporter.log(f"LLVM {version}: {pinned}")
        return pinned
    if choice == "auto":
        for bindir in _system_llvm_candidates():
            version = llvm_version(bindir)
            if version is not None and version >= deps.LLVM_MIN_MAJOR:
                reporter.log(f"LLVM {version} (system): {bindir}")
                return bindir
        reporter.log(f"No usable LLVM {deps.LLVM_MIN_MAJOR}+ with {', '.join(LLVM_TOOLS)} found; "
                     f"downloading LLVM {deps.LLVM_VERSION}.")

    dl = deps.LLVM[HOST]
    archive = download(reporter, dl, DOWNLOADS)
    extract_tar(reporter, archive, dl.top_dir, LLVM_DIR)
    if llvm_version(pinned) is None:
        raise InstallError(f"The LLVM download in {LLVM_DIR} lacks one of {', '.join(LLVM_TOOLS)}.")
    archive.unlink()
    return pinned


# ------------------------------------------------------------------ xwin splat

def _winsysroot_stamp():
    return f"xwin {deps.XWIN_VERSION}, CRT {deps.MSVC_CRT_VERSION}, SDK {deps.WINDOWS_SDK_VERSION}\n"


def winsysroot_ready():
    """Whether the CRT/SDK are set up (so their license was accepted on an earlier run)."""
    stamp = WINSYSROOT / "dao-xwin.txt"
    return stamp.exists() and stamp.read_text() == _winsysroot_stamp()


def ensure_winsysroot(reporter, accept_license):
    stamp = WINSYSROOT / "dao-xwin.txt"
    wanted = _winsysroot_stamp()
    if winsysroot_ready():
        reporter.log(f"MSVC CRT and Windows SDK: ready ({wanted.strip()})")
        return WINSYSROOT
    if not accept_license:
        raise InstallError("Building needs the Microsoft Visual C++ runtime headers/libraries and the Windows "
                           f"SDK, which xwin downloads from Microsoft. Accept their license "
                           f"({deps.MSVC_LICENSE_URL}) to continue.")

    xwin = XWIN_DIR / ("xwin" + EXE)
    if not xwin.exists():
        dl = deps.XWIN[HOST]
        archive = download(reporter, dl, DOWNLOADS)
        extract_tar(reporter, archive, dl.top_dir, XWIN_DIR)
        archive.unlink()

    if WINSYSROOT.exists():
        shutil.rmtree(WINSYSROOT)
    cache = XWIN_DIR / "cache"
    args = [xwin, "--accept-license", "--cache-dir", cache, "--log-level", "warn",
            "--crt-version", deps.MSVC_CRT_VERSION, "--sdk-version", deps.WINDOWS_SDK_VERSION,
            "--http-retry", "3", "--arch", "x86_64", "--variant", "desktop",
            "splat", "--use-winsysroot-style", "--preserve-ms-arch-notation", "--output", WINSYSROOT]
    if IS_WINDOWS:
        args.append("--disable-symlinks")  # case-insensitive file system; symlinks need privileges
    reporter.log("Downloading the MSVC CRT and Windows SDK with xwin (about 85 MB)")
    reporter.progress(None)
    run(reporter, args, env=clean_env())
    shutil.rmtree(cache, ignore_errors=True)
    stamp.write_text(wanted)
    return WINSYSROOT


def ensure_toolchain(reporter, accept_license, llvm_choice="auto"):
    cmake, ninja = ensure_build_tools(reporter)
    reporter.check_cancel()
    llvm_bin = ensure_llvm(reporter, llvm_choice)
    reporter.check_cancel()
    winsysroot = ensure_winsysroot(reporter, accept_license)
    return Toolchain(cmake, ninja, llvm_bin, winsysroot)
