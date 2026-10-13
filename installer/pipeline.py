"""The install steps: game files from the ISO, sources, toolchain, recompilation, build, install."""
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import zipfile
from dataclasses import dataclass
from pathlib import Path

from . import deps, neural, shortcuts, sources, toolchain
from .util import (BUNDLE, EXE, BUNDLED, GIT, IS_WINDOWS, REPO, SCRIPTS, SOURCE, VERSION, InstallError, check_host,
                   clean_env, human_size, run)

sys.path.insert(0, SCRIPTS)
import patch_recompiler  # noqa: E402
import prefix_symbols  # noqa: E402
import stfs  # noqa: E402
import sync_dir  # noqa: E402
import xiso  # noqa: E402

BUILD = REPO / "build" / "installer"
GAME_MARKER = "dao-extracted.txt"
# Disc content the game never reads.
SKIP_PREFIXES = ("$SystemUpdate/",)


@dataclass
class Options:
    iso: Path = None
    dest: Path = None
    # Title Update 6's or 7's package (optional; an install keeps it in dest/title_update).
    title_update: Path = None
    accept_license: bool = False
    llvm: str = "auto"
    jobs: int = 0  # 0 = Ninja's default
    # Start menu and desktop shortcuts to the game (Windows).
    shortcuts: bool = False


# --------------------------------------------------------------------- source

SOURCE_MANIFEST = "dao-source-files.txt"
# The bundle's files that aren't the project's (scripts/build_installer_engine.py).
BUNDLE_ONLY = ("__main__.py",)
BUNDLE_LIBS = "_lib/"


def _source_files():
    """The project's files under SOURCE, as relative POSIX paths."""
    if BUNDLED:
        # Not the bundle's own: its entry point and the libraries the installer imports.
        with zipfile.ZipFile(BUNDLE) as bundle:
            files = [rel for rel in bundle.namelist()
                     if not rel.endswith("/") and rel not in BUNDLE_ONLY and not rel.startswith(BUNDLE_LIBS)]
    else:
        # A checkout: only its tracked files (never the game's, nor builds).
        out = subprocess.run([*GIT, "-C", str(SOURCE), "ls-files", "-z"], capture_output=True, check=True,
                             creationflags=subprocess.CREATE_NO_WINDOW if IS_WINDOWS else 0).stdout
        files = (rel for rel in out.decode("utf-8").split("\0") if rel and (SOURCE / rel).is_file())
    # Not VERSION: the bundle's own (util.VERSION), which doesn't belong in a checkout.
    return sorted(rel for rel in files if rel != "VERSION")


def _plain_relative(rel):
    path = Path(rel)
    return (bool(rel) and not rel.startswith(("/", "\\")) and not path.is_absolute() and not path.drive
            and ".." not in path.parts)


def sync_source(reporter):
    """Copies the project source into the work folder (when that is somewhere else), changing
    only files that differ (so rebuilds stay incremental) and removing ones it dropped."""
    if not BUNDLED and SOURCE.resolve() == REPO.resolve():
        return
    try:
        files = _source_files()
    except (OSError, subprocess.CalledProcessError) as e:
        raise InstallError(f"Can't list the project's files in {SOURCE}: {e}")
    REPO.mkdir(parents=True, exist_ok=True)
    manifest = REPO / SOURCE_MANIFEST
    old = set(manifest.read_text(encoding="utf-8").splitlines()) if manifest.exists() else set()
    changed = 0
    for rel in files:
        src, dst = SOURCE / rel, REPO / rel
        data = src.read_bytes()
        # (The size first: most files are unchanged, and then only the source is read.)
        if not dst.is_file() or dst.stat().st_size != len(data) or dst.read_bytes() != data:
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(data)
            changed += 1
    # The manifest is only ever this function's, but a damaged one mustn't reach outside.
    for rel in old - set(files):
        if _plain_relative(rel):
            (REPO / rel).unlink(missing_ok=True)
    manifest.write_text("\n".join(files), encoding="utf-8")
    reporter.log(f"Source: {len(files)} files in {REPO} ({changed} updated)")


# ----------------------------------------------------------------- game files

def _iso_xex_sha1(iso):
    with open(iso, "rb") as f:
        base, entries = xiso.read_entries(f)
        for path, is_dir, sector, size in entries:
            if path.lower() == "default.xex" and not is_dir:
                return xiso.hash_file(f, base, sector, size)
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


NO_GAME_FILES = "No game files in {} yet: choose the disc image."
_extracted = {}  # (default.xex or default.xexp, size, mtime) -> whether it's the supported one


def game_extracted(game):
    """Whether game/ holds a finished extraction of the supported game (default.xex is
    hashed once per version of the file)."""
    xex = Path(game) / "default.xex"
    if not (Path(game) / GAME_MARKER).exists() or not xex.exists():
        return False
    stat = xex.stat()
    key = (str(xex.resolve()), stat.st_size, stat.st_mtime_ns)
    if key not in _extracted:
        _extracted[key] = _file_sha1(xex) == deps.GAME_XEX_SHA1
    return _extracted[key]


def extract_game(reporter, iso, game):
    xex = game / "default.xex"
    if game_extracted(game):
        reporter.log(f"Game files: already extracted to {game}")
        return
    if iso is None:
        raise InstallError(NO_GAME_FILES.format(game))
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
    (game / GAME_MARKER).write_text(f"Extracted from {iso.name} by install.py\n")


UPDATE_FOLDER = "title_update"


def title_update_installed(update):
    """Whether title_update/ holds Title Update 6 or 7 (its default.xexp, hashed once per
    version of the file)."""
    xexp = Path(update) / "default.xexp"
    if not xexp.is_file():
        return False
    stat = xexp.stat()
    key = (str(xexp.resolve()), stat.st_size, stat.st_mtime_ns)
    if key not in _extracted:
        _extracted[key] = _file_sha1(xexp) in deps.TU_XEXP_SHA1S
    return _extracted[key]


def _remove_folder(path):
    """Removes a folder of ours; a symlink or junction in its place goes, but not what it
    points to."""
    if path.is_symlink() or getattr(path, "is_junction", lambda: False)():
        os.rmdir(path)
    elif path.exists():
        shutil.rmtree(path)


def install_title_update(reporter, package, update):
    """Extracts Title Update 6's or 7's package (the player's file, checked first) into
    title_update/. 7 replaces 6 (it holds 6's changes); 7 over 7 and 6 over 7 are
    refused, and a folder holding anything else is left alone."""
    work = update.with_name(update.name + ".partial")
    xexp = update / "default.xexp"
    installed = deps.TU_XEXP_SHA1S.get(_file_sha1(xexp), 0) if xexp.is_file() else 0
    if installed == 7:
        reporter.log(f"Title Update {installed} is already installed in {update}.")
        return
    if not installed and update.exists() and any(update.iterdir()):
        raise InstallError(f"{update} holds something other than Title Update 6 or 7; move it away to "
                           "install this one.")
    try:
        with stfs.Package(package) as pkg:
            if pkg.title_id != deps.GAME_TITLE_ID or pkg.content_type != stfs.CONTENT_INSTALLER:
                raise InstallError(f"{package.name} is not a title update for {deps.GAME_TITLE}.")
            if pkg.media_id != deps.GAME_MEDIA_ID:
                raise InstallError(f"{package.name} is the title update for another edition of the game (disc "
                                   f"{pkg.media_id:08X}); yours needs the one for disc {deps.GAME_MEDIA_ID:08X}.")
            _remove_folder(work)
            reporter.log(f"Extracting {pkg.display_name} ({len(pkg.files)} files) to {update}")

            def progress(fraction):
                reporter.progress(fraction)
                reporter.check_cancel()

            pkg.extract(work, progress)
    except stfs.Error as e:
        raise InstallError(f"{package.name}: {e}.")
    except OSError as e:
        raise InstallError(f"Can't extract {package} to {work}: {e}")
    xexp = work / "default.xexp"
    number = deps.TU_XEXP_SHA1S.get(_file_sha1(xexp), 0) if xexp.is_file() else 0
    if not number:
        _remove_folder(work)
        raise InstallError(f"{package.name} is not Title Update 6 or 7, the ones the game is made for.")
    if number <= installed:
        _remove_folder(work)
        reporter.log(f"Title Update {installed} is already installed in {update}.")
        return
    # The old one is kept as title_update.old until the new one is in place, and put back
    # if it can't be.
    old = update.with_name(update.name + ".old")
    _remove_folder(old)
    moved_old = False
    try:
        if update.exists():
            update.rename(old)
            moved_old = True
        work.rename(update)
    except OSError as e:
        if moved_old and not update.exists():
            old.rename(update)
        _remove_folder(work)
        raise InstallError(f"Can't install the title update: it can't be moved to {update}: {e}")
    try:
        _remove_folder(old)
    except OSError:
        pass


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
    patch_recompiler.patch(str(REPO / "tools" / "XenonRecomp" / "XenonRecomp" / "recompiler.cpp"))
    build = BUILD / "xenonrecomp"
    _configure(reporter, tc, REPO / "tools" / "XenonRecomp", build, {}, native_linux=not IS_WINDOWS)
    _build(reporter, tc, build, "XenonRecomp", jobs)
    return build / "XenonRecomp" / ("XenonRecomp" + EXE)


def recompile(reporter, recompiler, game, update=None):
    """Translates default.xex to C++ in ppc/; with title_update/ (Title Update 6 or 7),
    the patched executable to ppc_tu6/, its names prefixed so both builds link together."""
    name = "ppc_tu6" if update else "ppc"
    config_name = "DAO_tu6" if update else "DAO"
    out = BUILD / f"{name}_gen"
    for stale in BUILD.glob(f"{out.name}.old-*"):  # (from runs whose rmtree couldn't finish)
        shutil.rmtree(stale, ignore_errors=True)
    if out.exists():
        # Renamed first: Windows can finish deleting a folder after rmtree returns
        # (a scanner holding a file), and recreating it then fails.
        old = out.with_name(f"{out.name}.old-{os.getpid()}")
        out.rename(old)
        shutil.rmtree(old, ignore_errors=True)
    out.mkdir(parents=True)
    config = (REPO / "config" / f"{config_name}.toml").read_text(encoding="utf-8")
    replacements = {
        "file_path": game / "default.xex",
        "out_directory_path": out,
        "switch_table_file_path": REPO / "config" / f"{config_name}_switch_tables.toml",
    }
    if update:
        replacements["patch_file_path"] = update / "default.xexp"
    for key, value in replacements.items():
        config, n = re.subn(rf"^{key}\s*=.*$", lambda m: f"{key} = {_toml_string(value.as_posix())}", config,
                            count=1, flags=re.M)
        if n != 1:
            raise InstallError(f"config/{config_name}.toml has no {key} setting.")
    gen_config = BUILD / f"{config_name}.gen.toml"
    gen_config.write_text(config, encoding="utf-8")

    reporter.progress(None)
    # XenonRecomp prefixes the config file's directory to the paths in it as text, so
    # pass the config by bare name (from its directory) for the absolute paths to work.
    log_file = BUILD / f"recomp_{name}.log"
    run(reporter, [recompiler, gen_config.name, REPO / "tools" / "XenonRecomp" / "XenonUtils" / "ppc_context.h"],
        cwd=BUILD, log_file=log_file, quiet=True)
    if not (out / "ppc_func_mapping.cpp").exists():
        raise InstallError(f"XenonRecomp produced no code (see {log_file}).")
    if update:
        # The prefix DAORecompLib/CMakeLists.txt (PPCFuncMappings_tu6) and the hooks
        # (GAME_HOOK's tu6_sub_...) expect, as scripts/recomp.cmd gives it.
        prefix_symbols.apply(out, "tu6")
    # Copy only what changed, so rebuilding after an update recompiles little.
    changed, removed, unchanged = sync_dir.sync(out, REPO / name)
    reporter.log(f"Generated code ({name}): {changed} files updated, {removed} removed, {unchanged} unchanged")


# ------------------------------------------------------------------- building

# What clang defines for x86-64-v3's instructions (AVX2, BMI1/2, FMA, F16C, LZCNT, MOVBE);
# DAORecomp/cpu_check.cpp checks the same at run time.
V3_MACROS = ("__AVX2__", "__BMI__", "__BMI2__", "__FMA__", "__F16C__", "__LZCNT__", "__MOVBE__")


def target_cpu(reporter, tc):
    """The -march to build the game for. The game is built on the PC that plays it, so for
    this PC's processor: x86-64-v3 when it has those instructions (about 2-3% less
    processor time per frame, measured), else sandybridge (any AVX processor). The exe
    checks again when it starts (DAORecomp/cpu_check.cpp), for an install copied to another
    PC. Asks clang-cl, which every toolchain the installer accepts has."""
    try:
        out = subprocess.run([str(tc.tool("clang-cl")), "/nologo", "/clang:-march=native", "/clang:-dM", "/E",
                              "/TC", "-"], input="", capture_output=True, text=True, errors="replace",
                             check=True, timeout=120, env=clean_env(),
                             creationflags=subprocess.CREATE_NO_WINDOW if IS_WINDOWS else 0).stdout
    except (OSError, subprocess.SubprocessError) as e:
        reporter.log(f"Can't read this processor's features ({e}): building for any AVX processor")
        return "sandybridge"
    defined = {line.split()[1] for line in out.splitlines() if line.startswith("#define ")}
    if "__AVX__" not in defined:
        raise InstallError("This PC's processor has no AVX, which the game needs (most processors from 2011 "
                           "or later have it).")
    if all(m in defined for m in V3_MACROS):
        reporter.log("Building for this processor's instructions (x86-64-v3)")
        return "x86-64-v3"
    reporter.log("Building for any AVX processor")
    return "sandybridge"


def build_game(reporter, tc, jobs, with_tu6=False):
    build = BUILD / "release"
    # Without Title Update 6, none of its (possibly old) generated code is built in.
    tu6 = (REPO / "ppc_tu6").as_posix() if with_tu6 else ""
    _configure(reporter, tc, REPO, build, {"DAO_PPC_DIR": (REPO / "ppc").as_posix(), "DAO_PPC_TU6_DIR": tu6,
                                           "DAO_VERSION": VERSION, "DAO_MARCH": target_cpu(reporter, tc)})
    _build(reporter, tc, build, "DAORecomp", jobs)
    return build / "DAORecomp"


RUN_SH = """#!/bin/sh
# Starts Dragon Age: Origins (recompiled) with Wine. Direct3D 12 needs vkd3d-proton
# in the Wine prefix (Proton has it; for plain Wine: `winetricks vkd3d`).
# With Steam, you can instead add DAORecomp.exe as a non-Steam game and force Proton.
cd "$(dirname "$0")" || exit 1
exec "${WINE:-wine}" DAORecomp.exe "$@"
"""


def _copy_over(src, dst):
    """Copies src over dst whole or not at all: next to it first, then swapped in."""
    new = dst.with_name(dst.name + ".new")
    try:
        shutil.copy2(src, new)
        os.replace(new, dst)
    finally:
        new.unlink(missing_ok=True)


def _copy_when_free(reporter, src, dst, wait=30.0, interval=0.5):
    """Copies over dst, waiting up to `wait` seconds while it's in use (the launcher
    closing after starting an update)."""
    for attempt in range(int(wait / interval)):
        try:
            _copy_over(src, dst)
            return
        except PermissionError:
            if attempt == 0:
                reporter.log(f"{dst.name} is in use; waiting for it to close")
            time.sleep(interval)
    raise InstallError(f"{dst} is in use. Close the game and run the installer again.")


def _replace_exe(reporter, built, exe):
    """The program that runs the installer may be this exe (setup in the install
    folder, or the launcher updating it): Windows doesn't let a running exe be
    replaced, but lets it be renamed. Then the new exe deletes the old one
    (DAORecomp.old.exe) when it starts; if the copy fails, the old one is put back."""
    try:
        _copy_over(built, exe)
        return
    except PermissionError:
        if not IS_WINDOWS:
            raise
    old = exe.with_name("DAORecomp.old.exe")
    try:
        old.unlink(missing_ok=True)
        exe.rename(old)
    except OSError:
        old = None  # Still running from an earlier update, or not ours to move: copying waits.
    try:
        _copy_when_free(reporter, built, exe)
    except BaseException:
        if old is not None:
            exe.unlink(missing_ok=True)
            old.rename(exe)
        raise


def install(reporter, built, dest, with_shortcuts=False):
    dest.mkdir(parents=True, exist_ok=True)
    # The DLLs first: while the game runs they can't be replaced, and the exe is left as it was.
    for name in ("amd_fidelityfx_loader_dx12.dll", "amd_fidelityfx_upscaler_dx12.dll",
                 "amd_fidelityfx_framegeneration_dx12.dll", "libxess.dll", "LmxxfNrRuntime.dll"):
        _copy_when_free(reporter, built / name, dest / name)
    _replace_exe(reporter, built / "DAORecomp.exe", dest / "DAORecomp.exe")
    ffx = REPO / "tools" / "FidelityFX-SDK" / "Kits" / "FidelityFX" / "docs" / "license.md"
    if ffx.exists():
        shutil.copy2(ffx, dest / "amd_fidelityfx_license.md")
    # libxess.dll's license and third-party notices go with it (its license's terms).
    shutil.copy2(toolchain.XESS_DIR / "LICENSE.txt", dest / "intel_xess_license.txt")
    shutil.copy2(toolchain.XESS_DIR / "third-party-programs.txt", dest / "intel_xess_third_party_programs.txt")
    neural.install(reporter, BUILD / "neural", dest)
    for folder in ("userdata", "mods"):
        (dest / folder).mkdir(exist_ok=True)
    shutil.copy2(REPO / "mods" / "README.txt", dest / "mods" / "README.txt")
    if not IS_WINDOWS:
        run_sh = dest / "run.sh"
        run_sh.write_text(RUN_SH)
        run_sh.chmod(0o755)
    if with_shortcuts and IS_WINDOWS:
        shortcuts.create(reporter, dest)
    reporter.log(f"Installed {VERSION} to {dest}")


# ------------------------------------------------- the exe players download

# The footer after the installer appended to the launcher-only DAORecomp.exe:
# magic, the installer's offset and size (little-endian u64) and its SHA-256, as
# DAORecomp/launcher/install_runner.cpp reads it. The installer is its folder's files
# (scripts/build_installer_engine.py), each as: the name's length (u32) and the name
# (UTF-8, "/" between folders), the size (u64), the SHA-256, the data.
ENGINE_MAGIC = b"DAORecompEngine2"
# The installer's files: one folder of plainly named files, no device names (the
# launcher's PlainEngineName takes no others).
ENGINE_NAME = re.compile(r"(?!\.)(?!(?:con|prn|aux|nul|com\d|lpt\d)(?:\.|$))[A-Za-z0-9._-]{1,64}(?<!\.)", re.I)


def append_engine(launcher, engine_dir, out):
    """Writes `out`: the launcher-only exe with the installer's files appended (Windows
    ignores data after an image; the launcher unpacks them to run the installer)."""
    image = Path(launcher).read_bytes()
    engine_dir = Path(engine_dir)
    data = bytearray()
    for path in sorted(engine_dir.iterdir()):
        if not path.is_file() or not ENGINE_NAME.fullmatch(path.name):
            raise InstallError(f"{path}: the installer's folder takes only plainly named files")
        name = path.name.encode("utf-8")
        content = path.read_bytes()
        data += struct.pack("<I", len(name)) + name + struct.pack("<Q", len(content))
        data += hashlib.sha256(content).digest() + content
    footer = ENGINE_MAGIC + struct.pack("<QQ", len(image), len(data)) + hashlib.sha256(data).digest()
    Path(out).write_bytes(image + data + footer)


def build_setup(reporter, engine, out, jobs=0):
    """Builds what players download: DAORecomp.exe with the launcher and its Install view
    but no game code (DAO_LAUNCHER_ONLY), with the installer's folder `engine` in it.
    Accepts the Microsoft build tools license for the download (as the CI does)."""
    check_host()
    if not IS_WINDOWS:
        raise InstallError("The launcher-only exe is built on Windows.")
    toolchain.ensure_git(reporter)
    sources.ensure_all(reporter)
    # (The pinned LLVM, never one the build machine happens to have.)
    tc = toolchain.ensure_toolchain(reporter, True, "download")
    build = BUILD / "setup"
    # For any AVX processor: this is the exe players download.
    _configure(reporter, tc, REPO, build, {"DAO_LAUNCHER_ONLY": "ON", "DAO_VERSION": VERSION,
                                           "DAO_MARCH": "sandybridge"})
    _build(reporter, tc, build, "DAORecomp", jobs)
    append_engine(build / "DAORecomp" / "DAORecomp.exe", engine, out)
    reporter.log(f"Built {out} ({human_size(Path(out).stat().st_size)})")


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
    sync_source(reporter)
    toolchain.ensure_git(reporter)
    sources.ensure_all(reporter)
    toolchain.ensure_xess(reporter)
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
    """Installs, or updates an install (the disc image is needed until the game files are
    extracted)."""
    iso = Path(options.iso).expanduser().absolute() if options.iso else None
    dest = Path(options.dest).expanduser().absolute()
    game = dest / "game"

    _step(reporter, 1, "Checking the disc image" if iso else "Checking the game files")
    check_host()
    if iso:
        check_iso(reporter, iso)
    elif not game_extracted(game):
        raise InstallError(NO_GAME_FILES.format(game))
    _check_license(options)
    free = shutil.disk_usage(_existing_parent(REPO)).free
    if free < 12 * 2**30:
        reporter.log(f"Warning: {human_size(free)} free in {REPO}; the tools and build need about 12 GB.")

    _step(reporter, 2, "Extracting the game files")
    extract_game(reporter, iso, game)
    update = dest / UPDATE_FOLDER
    if options.title_update:
        install_title_update(reporter, Path(options.title_update).expanduser().absolute(), update)
    with_tu6 = title_update_installed(update)
    if update.exists() and not with_tu6:
        reporter.log(f"Warning: {update} doesn't hold Title Update 6 or 7; the game is built without it.")

    tc, recompiler = _prepare(reporter, options, 3)
    reporter.status(f"[5/{STEPS}] Recompiling the game code (a few minutes)")
    recompile(reporter, recompiler, game)
    if with_tu6:
        recompile(reporter, recompiler, game, update)

    _step(reporter, 6, "Building the game (this takes a while)")
    built = build_game(reporter, tc, options.jobs, with_tu6)

    _step(reporter, 7, "Installing")
    install(reporter, built, dest, options.shortcuts)
    reporter.progress(1.0)
    launch = "DAORecomp.exe" if IS_WINDOWS else "run.sh"
    reporter.status(f"Done. Start the game with {dest / launch}")
    return dest / launch
