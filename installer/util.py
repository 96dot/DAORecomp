"""Logging, processes, downloads and archives shared by the installer steps."""
import hashlib
import os
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import threading
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

IS_WINDOWS = sys.platform == "win32"
HOST = "windows" if IS_WINDOWS else "linux"
EXE = ".exe" if IS_WINDOWS else ""
# Running as the bundled installer executable (PyInstaller).
FROZEN = getattr(sys, "frozen", False)

if IS_WINDOWS:
    # Programs run by name are found on PATH only, never in the current folder.
    os.environ["NoDefaultCurrentDirectoryInExePath"] = "1"


def _work_root():
    if os.environ.get("DAO_WORK_DIR"):
        return Path(os.environ["DAO_WORK_DIR"]).expanduser().absolute()
    if not FROZEN:
        return Path(__file__).resolve().parent.parent  # the repository checkout
    base = os.environ.get("LOCALAPPDATA") if IS_WINDOWS else None
    base = Path(base) if base else Path.home() / ".local" / "share"
    return base / "DAORecomp-build"


# The project source as shipped: the checkout, or the copy inside the executable.
SOURCE = Path(sys._MEIPASS) / "source" if FROZEN else Path(__file__).resolve().parent.parent
# Where the build happens: the project source plus tools/, build/ and ppc/. The
# executable copies its source here first (see pipeline.sync_source).
REPO = _work_root()

# git to run; toolchain.ensure_git may switch it to a downloaded MinGit.
GIT = ["git"]


def _version():
    """The release this installer is (its tag), for the build and the launcher's update
    check: the executable carries it in VERSION; a source checkout of a tag is that tag
    (with -dirty if changed, which the launcher treats as a dev build); anything else is "dev".
    It goes into the build as a C++ string, so only plain characters are kept."""
    stamp = SOURCE / "VERSION"
    if stamp.exists():
        return _plain(stamp.read_text(encoding="utf-8"))
    # The system's git: this runs before any is downloaded, and only in a checkout
    # (which has git).
    try:
        tag = subprocess.run(["git", "-C", str(SOURCE), "describe", "--tags", "--exact-match", "--dirty"],
                             capture_output=True, text=True, stdin=subprocess.DEVNULL,
                             creationflags=subprocess.CREATE_NO_WINDOW if IS_WINDOWS else 0).stdout.strip()
    except OSError:
        tag = ""
    return _plain(tag)


def _plain(version):
    version = re.sub(r"[^A-Za-z0-9._+-]", "", version.strip())
    return version or "dev"


VERSION = _version()


class InstallError(Exception):
    """A failure with a message meant for the user."""


class Cancelled(InstallError):
    def __init__(self):
        super().__init__("Cancelled.")


class Reporter:
    """Where the steps send their output. The GUI overrides the callbacks."""

    def __init__(self, log=None, status=None, progress=None):
        self._log = log or (lambda text: print(text, flush=True))
        self._status = status or (lambda text: print(f"\n== {text}", flush=True))
        self._progress = progress or (lambda fraction: None)
        self.cancel_event = threading.Event()

    def log(self, text):
        self._log(text)

    def status(self, text):
        self._status(text)

    def progress(self, fraction):
        """fraction in 0..1, or None for 'busy, unknown length'."""
        self._progress(fraction)

    def check_cancel(self):
        if self.cancel_event.is_set():
            raise Cancelled()


def clean_env(extra=None):
    """The environment for build tools: no Visual Studio/compiler variables that
    could pull in another toolchain."""
    env = {k: v for k, v in os.environ.items()
           if k.upper() not in ("INCLUDE", "LIB", "LIBPATH", "CC", "CXX", "CFLAGS", "CXXFLAGS", "LDFLAGS")}
    env.update(extra or {})
    return env


def run(reporter, args, cwd=None, env=None, log_file=None, quiet=False):
    """Runs a command, streaming its output to the reporter (and log_file)."""
    args = [str(a) for a in args]
    reporter.log("$ " + " ".join(f'"{a}"' if " " in a else a for a in args))
    flags = subprocess.CREATE_NO_WINDOW if IS_WINDOWS else 0
    try:
        process = subprocess.Popen(args, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   stdin=subprocess.DEVNULL, creationflags=flags)
    except FileNotFoundError:
        raise InstallError(f"{args[0]} not found.")
    if log_file:
        Path(log_file).parent.mkdir(parents=True, exist_ok=True)
    out = open(log_file, "w", encoding="utf-8", errors="replace") if log_file else None
    tail = []
    try:
        for raw in process.stdout:
            line = raw.decode("utf-8", errors="replace").rstrip()
            if out:
                out.write(line + "\n")
            tail = (tail + [line])[-40:]
            if not quiet:
                reporter.log(line)
            if reporter.cancel_event.is_set():
                process.kill()
        process.wait()
    finally:
        if out:
            out.close()
    reporter.check_cancel()
    if process.returncode != 0:
        if quiet:
            for line in tail:
                reporter.log(line)
        where = f" (full output: {log_file})" if log_file else ""
        raise InstallError(f"{Path(args[0]).name} failed with exit code {process.returncode}{where}.")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def download(reporter, dl, dest_dir):
    """Downloads a deps.Download into dest_dir (resumable by re-running), checks its SHA-256."""
    dest_dir.mkdir(parents=True, exist_ok=True)
    name = dl.url.rsplit("/", 1)[1].replace("%2B", "+")
    path = dest_dir / name
    if path.exists() and path.stat().st_size == dl.size and sha256_file(path) == dl.sha256:
        reporter.log(f"{name}: already downloaded")
        return path
    partial = path.with_name(name + ".part")
    have = partial.stat().st_size if partial.exists() else 0
    if have >= dl.size:
        # Complete (an earlier run stopped before checking it), or too long to be it.
        if have == dl.size and sha256_file(partial) == dl.sha256:
            partial.replace(path)
            return path
        partial.unlink()
        have = 0
    reporter.log(f"Downloading {dl.url} ({dl.size / 2**20:.0f} MB)")
    request = urllib.request.Request(dl.url, headers={"User-Agent": "dao-recomp-installer"})
    if have:
        request.add_header("Range", f"bytes={have}-")
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            if have and response.status != 206:
                have = 0  # the server ignored the range; start over
            with open(partial, "ab" if have else "wb") as out:
                done, last = have, -1
                while chunk := response.read(1 << 20):
                    done += len(chunk)
                    if done > dl.size:
                        raise InstallError(f"{name}: the download is larger than it should be. Try again later.")
                    out.write(chunk)
                    reporter.progress(done / dl.size)
                    percent = done * 100 // dl.size
                    if percent // 10 != last:
                        last = percent // 10
                        reporter.log(f"  {percent}%")
                    reporter.check_cancel()
    except (urllib.error.URLError, TimeoutError, ConnectionError) as e:
        # What arrived is kept: running again carries on from there.
        raise InstallError(f"Can't download {name} ({getattr(e, 'reason', e)}). Check the internet connection "
                           f"and try again.")
    if sha256_file(partial) != dl.sha256:
        partial.unlink()
        raise InstallError(f"{name}: checksum mismatch (download corrupted or changed upstream). Try again.")
    partial.replace(path)
    return path


def _plain_member(member):
    """Whether a tar entry is a file, folder or link that stays inside the folder."""
    def inside(name):
        path = Path(name)
        return (bool(name) and not name.startswith(("/", "\\")) and not path.is_absolute() and not path.drive
                and ".." not in path.parts)
    if not (member.isfile() or member.isdir() or member.issym() or member.islnk()):
        return False
    if not inside(member.name):
        return False
    if member.issym():
        return inside(os.path.normpath(os.path.join(os.path.dirname(member.name), member.linkname)))
    return not member.islnk() or inside(member.linkname)


def extract(reporter, archive, top_dir, dest):
    """Extracts a .tar.xz/.tar.gz/.zip to dest. top_dir: the archive's single top-level
    directory (its contents become dest), or "" if the files sit at the top."""
    reporter.log(f"Extracting {archive.name}")
    staging = dest.with_name(dest.name + ".extract")
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True)
    if archive.suffix == ".zip":
        with zipfile.ZipFile(archive) as z:
            names = z.namelist()
            root = staging.resolve()
            for i, name in enumerate(names):
                target = (staging / name).resolve()
                if root not in target.parents and target != root:
                    raise InstallError(f"{archive.name}: unsafe path {name}")
                z.extract(name, staging)
                if i % 200 == 0:
                    reporter.progress(i / len(names))
                    reporter.check_cancel()
        if not IS_WINDOWS:  # zipfile drops the executable bits
            for path in staging.rglob("*"):
                if path.is_file():
                    path.chmod(0o755)
    else:
        with tarfile.open(archive) as tar:
            members = tar.getmembers()
            for i, member in enumerate(members):
                if hasattr(tarfile, "data_filter"):
                    tar.extract(member, staging, filter="data")
                else:
                    # An older Python: the "data" filter's main checks by hand.
                    if not _plain_member(member):
                        raise InstallError(f"{archive.name}: unsafe entry {member.name}")
                    tar.extract(member, staging)
                if i % 200 == 0:
                    reporter.progress(i / len(members))
                    reporter.check_cancel()
    if dest.exists():
        shutil.rmtree(dest)
    (staging / top_dir).replace(dest) if top_dir else staging.replace(dest)
    if staging.exists():
        shutil.rmtree(staging)


def human_size(n):
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n:.1f} {unit}" if unit != "B" else f"{n} B"
        n /= 1024


def check_host():
    if platform.machine().lower() not in ("amd64", "x86_64"):
        raise InstallError(f"This needs an x86-64 computer (this one is {platform.machine()}).")
    if sys.version_info < (3, 10):
        raise InstallError("This needs Python 3.10 or newer.")
    if not IS_WINDOWS and not shutil.which("git"):  # Windows downloads MinGit if needed
        raise InstallError("git was not found. Install git and run this again.")
