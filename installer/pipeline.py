"""The install steps: game files from the ISO, sources, toolchain, recompilation, build, install."""
import hashlib
import json
import os
import re
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path

from . import deps, sources, toolchain
from .util import EXE, IS_WINDOWS, REPO, InstallError, check_host, clean_env, human_size, run

sys.path.insert(0, str(REPO / "scripts"))
import xiso  # noqa: E402

BUILD = REPO / "build" / "installer"
GAME_MARKER = "dao-extracted.txt"
# Disc content the game never reads.
SKIP_PREFIXES = ("$SystemUpdate/",)


@dataclass
class Options:
    iso: Path = None
    dest: Path = None
    accept_license: bool = False
    llvm: str = "auto"
    jobs: int = 0  # 0 = Ninja's default


# ----------------------------------------------------------------- game files

def _iso_xex_sha1(iso):
    with open(iso, "rb") as f:
        base, entries = xiso.read_entries(f)
        for path, is_dir, sector, size in entries:
            if path.lower() == "default.xex" and not is_dir:
                f.seek(base + sector * xiso.SECTOR)
                return hashlib.sha1(f.read(size)).hexdigest()
    return None


def _file_sha1(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def check_iso(reporter, iso):
    if not iso.is_file():
        raise InstallError(f"{iso} is not a file.")
    try:
        sha1 = _iso_xex_sha1(iso)
    except (ValueError, OSError) as e:
        raise InstallError(f"{iso}: {e}")
    if sha1 is None:
        raise InstallError(f"{iso} has no default.xex; it is not an Xbox 360 game disc image.")
    if sha1 != deps.GAME_XEX_SHA1:
        raise InstallError(f"{iso} is not the supported game: this recompilation is made for "
                           f"{deps.GAME_TITLE} and its default.xex differs (SHA-1 {sha1}).")
    reporter.log(f"{iso.name}: {deps.GAME_TITLE}")


def extract_game(reporter, iso, game):
    marker = game / GAME_MARKER
    xex = game / "default.xex"
    if marker.exists() and xex.exists() and _file_sha1(xex) == deps.GAME_XEX_SHA1:
        reporter.log(f"Game files: already extracted to {game}")
        return
    with open(iso, "rb") as f:
        _, entries = xiso.read_entries(f)
    wanted = {p for p, d, _, _ in entries if not d and not p.startswith(SKIP_PREFIXES)}
    total = sum(z for p, d, _, z in entries if p in wanted)
    free = shutil.disk_usage(_existing_parent(game)).free
    if free < total + 2**30:
        raise InstallError(f"Not enough space for the game files in {game}: {human_size(total)} needed, "
                           f"{human_size(free)} free.")
    reporter.log(f"Extracting {len(wanted)} files ({human_size(total)}) to {game}")
    game.mkdir(parents=True, exist_ok=True)

    def progress(done, total, path):
        reporter.progress(done / total if total else 1.0)
        reporter.check_cancel()

    xiso.extract(iso, game, wanted, progress)
    if _file_sha1(xex) != deps.GAME_XEX_SHA1:
        raise InstallError(f"{xex} does not match after extraction; the disc image may be damaged.")
    marker.write_text(f"Extracted from {iso.name} by install.py\n")


def _existing_parent(path):
    path = Path(path).absolute()
    while not path.exists():
        path = path.parent
    return path


# ---------------------------------------------------------------- recompiling

def _toml_string(value):
    # JSON string escapes are valid TOML basic-string escapes.
    return json.dumps(str(value))


def _configure(reporter, tc, source, build, defines, native_linux=False):
    args = [tc.cmake, "-G", "Ninja", "-S", source, "-B", build, "-Wno-dev",
            f"-DCMAKE_MAKE_PROGRAM={tc.ninja}", "-DCMAKE_BUILD_TYPE=Release"]
    if native_linux:
        args += [f"-DCMAKE_C_COMPILER={tc.tool('clang')}", f"-DCMAKE_CXX_COMPILER={tc.tool('clang++')}"]
    else:
        args += [f"-DCMAKE_TOOLCHAIN_FILE={REPO / 'cmake' / 'clang-cl-xwin.cmake'}",
                 f"-DDAO_LLVM_BIN={tc.llvm_bin}", f"-DDAO_WINSYSROOT={tc.winsysroot}"]
    args += [f"-D{k}={v}" for k, v in defines.items()]
    run(reporter, args, env=clean_env(), log_file=Path(build).with_suffix(".configure.log"), quiet=True)


def _build(reporter, tc, build, target, jobs):
    args = [tc.ninja, "-C", build, target]
    if jobs:
        args += ["-j", str(jobs)]
    progress = re.compile(r"^\[(\d+)/(\d+)\]")
    log_file = Path(build).with_suffix(".build.log")
    original = reporter.log

    def log(line):
        match = progress.match(line)
        if match:
            reporter.progress(int(match.group(1)) / int(match.group(2)))
        original(line)

    reporter.log = log
    try:
        run(reporter, args, env=clean_env(), log_file=log_file)
    finally:
        reporter.log = original


def build_recompiler(reporter, tc, jobs):
    """Patches XenonRecomp and builds it for this computer."""
    run(reporter, [sys.executable, "-I", REPO / "scripts" / "patch_recompiler.py",
                   REPO / "tools" / "XenonRecomp" / "XenonRecomp" / "recompiler.cpp"])
    build = BUILD / "xenonrecomp"
    _configure(reporter, tc, REPO / "tools" / "XenonRecomp", build, {}, native_linux=not IS_WINDOWS)
    _build(reporter, tc, build, "XenonRecomp", jobs)
    return build / "XenonRecomp" / ("XenonRecomp" + EXE)


def recompile(reporter, recompiler, game):
    """Translates default.xex to C++ in ppc/."""
    out = BUILD / "ppc_gen"
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    config = (REPO / "config" / "DAO.toml").read_text(encoding="utf-8")
    replacements = {
        "file_path": game / "default.xex",
        "out_directory_path": out,
        "switch_table_file_path": REPO / "config" / "DAO_switch_tables.toml",
    }
    for key, value in replacements.items():
        config, n = re.subn(rf"^{key}\s*=.*$", lambda m: f"{key} = {_toml_string(value.as_posix())}", config,
                            count=1, flags=re.M)
        if n != 1:
            raise InstallError(f"config/DAO.toml has no {key} setting.")
    gen_config = BUILD / "DAO.gen.toml"
    gen_config.write_text(config, encoding="utf-8")

    reporter.progress(None)
    # XenonRecomp prefixes the config file's directory to the paths in it as text, so
    # pass the config by bare name (from its directory) for the absolute paths to work.
    run(reporter, [recompiler, gen_config.name, REPO / "tools" / "XenonRecomp" / "XenonUtils" / "ppc_context.h"],
        cwd=BUILD, log_file=BUILD / "recomp.log", quiet=True)
    if not (out / "ppc_func_mapping.cpp").exists():
        raise InstallError(f"XenonRecomp produced no code (see {BUILD / 'recomp.log'}).")
    # Copy only what changed, so rebuilding after an update recompiles little.
    run(reporter, [sys.executable, "-I", REPO / "scripts" / "sync_dir.py", out, REPO / "ppc"])


# ------------------------------------------------------------------- building

def build_game(reporter, tc, jobs):
    build = BUILD / "release"
    _configure(reporter, tc, REPO, build, {"DAO_PPC_DIR": (REPO / "ppc").as_posix()})
    _build(reporter, tc, build, "DAORecomp", jobs)
    return build / "DAORecomp"


RUN_SH = """#!/bin/sh
# Starts Dragon Age: Origins (recompiled) with Wine. Direct3D 12 needs vkd3d-proton
# in the Wine prefix (Proton has it; for plain Wine: `winetricks vkd3d`).
# With Steam, you can instead add DAORecomp.exe as a non-Steam game and force Proton.
cd "$(dirname "$0")" || exit 1
exec "${WINE:-wine}" DAORecomp.exe "$@"
"""


def install(reporter, built, dest):
    dest.mkdir(parents=True, exist_ok=True)
    files = ["DAORecomp.exe", "amd_fidelityfx_loader_dx12.dll", "amd_fidelityfx_upscaler_dx12.dll"]
    for name in files:
        shutil.copy2(built / name, dest / name)
    ffx = REPO / "tools" / "FidelityFX-SDK" / "Kits" / "FidelityFX" / "docs" / "license.md"
    if ffx.exists():
        shutil.copy2(ffx, dest / "amd_fidelityfx_license.md")
    for folder in ("userdata", "mods"):
        (dest / folder).mkdir(exist_ok=True)
    shutil.copy2(REPO / "mods" / "README.txt", dest / "mods" / "README.txt")
    if not IS_WINDOWS:
        run_sh = dest / "run.sh"
        run_sh.write_text(RUN_SH)
        run_sh.chmod(0o755)
    reporter.log(f"Installed to {dest}")


# ----------------------------------------------------------------------- all

STEPS = 7


def _step(reporter, n, text):
    reporter.check_cancel()
    reporter.status(f"[{n}/{STEPS}] {text}")
    reporter.progress(None)


def _check_license(options):
    if not options.accept_license and not toolchain.winsysroot_ready():
        raise InstallError("The Microsoft Visual C++ build tools license must be accepted "
                           f"({deps.MSVC_LICENSE_URL}).")


def _prepare(reporter, options, first_step):
    """Sources, toolchain and the recompiler: everything that doesn't need the game."""
    _step(reporter, first_step, "Fetching the source code")
    sources.ensure_all(reporter)
    _step(reporter, first_step + 1, "Setting up the compiler")
    tc = toolchain.ensure_toolchain(reporter, options.accept_license, options.llvm)
    _step(reporter, first_step + 2, "Building the recompiler")
    recompiler = build_recompiler(reporter, tc, options.jobs)
    return tc, recompiler


def prepare_only(reporter, options):
    """Sets up everything except the game (for checking a system, or CI)."""
    check_host()
    _check_license(options)
    _prepare(reporter, options, 3)
    reporter.status("Ready: sources, toolchain and recompiler are set up.")


def run_all(reporter, options):
    iso, dest = Path(options.iso).expanduser().absolute(), Path(options.dest).expanduser().absolute()
    game = dest / "game"

    _step(reporter, 1, "Checking the disc image")
    check_host()
    check_iso(reporter, iso)
    _check_license(options)
    free = shutil.disk_usage(_existing_parent(REPO)).free
    if free < 12 * 2**30:
        reporter.log(f"Warning: {human_size(free)} free in {REPO}; the tools and build need about 12 GB.")

    _step(reporter, 2, "Extracting the game files")
    extract_game(reporter, iso, game)

    tc, recompiler = _prepare(reporter, options, 3)
    reporter.status(f"[5/{STEPS}] Recompiling the game code (a few minutes)")
    recompile(reporter, recompiler, game)

    _step(reporter, 6, "Building the game (this takes a while)")
    built = build_game(reporter, tc, options.jobs)

    _step(reporter, 7, "Installing")
    install(reporter, built, dest)
    reporter.progress(1.0)
    launch = "DAORecomp.exe" if IS_WINDOWS else "run.sh"
    reporter.status(f"Done. Start the game with {dest / launch}")
    return dest / launch
