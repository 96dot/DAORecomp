"""Logging, processes, downloads and archives shared by the installer steps."""
import hashlib
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import threading
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
IS_WINDOWS = sys.platform == "win32"
HOST = "windows" if IS_WINDOWS else "linux"
EXE = ".exe" if IS_WINDOWS else ""


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
    reporter.log(f"Downloading {dl.url} ({dl.size / 2**20:.0f} MB)")
    request = urllib.request.Request(dl.url, headers={"User-Agent": "dao-recomp-installer"})
    have = partial.stat().st_size if partial.exists() else 0
    if 0 < have < dl.size:
        request.add_header("Range", f"bytes={have}-")
    with urllib.request.urlopen(request, timeout=60) as response:
        if have and response.status != 206:
            have = 0  # the server ignored the range; start over
        with open(partial, "ab" if have else "wb") as out:
            done, last = have, -1
            while chunk := response.read(1 << 20):
                out.write(chunk)
                done += len(chunk)
                reporter.progress(min(done / dl.size, 1.0))
                percent = done * 100 // dl.size
                if percent // 10 != last:
                    last = percent // 10
                    reporter.log(f"  {percent}%")
                reporter.check_cancel()
    if sha256_file(partial) != dl.sha256:
        partial.unlink()
        raise InstallError(f"{name}: checksum mismatch (download corrupted or changed upstream). Try again.")
    partial.replace(path)
    return path


def extract_tar(reporter, archive, top_dir, dest):
    """Extracts a .tar.xz/.tar.gz whose contents sit in top_dir, so that top_dir becomes dest."""
    reporter.log(f"Extracting {archive.name}")
    staging = dest.with_name(dest.name + ".extract")
    if staging.exists():
        shutil.rmtree(staging)
    with tarfile.open(archive) as tar:
        members = tar.getmembers()
        for i, member in enumerate(members):
            if hasattr(tarfile, "data_filter"):
                tar.extract(member, staging, filter="data")
            else:
                tar.extract(member, staging)
            if i % 200 == 0:
                reporter.progress(i / len(members))
                reporter.check_cancel()
    if dest.exists():
        shutil.rmtree(dest)
    (staging / top_dir).replace(dest)
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
    if not shutil.which("git"):
        raise InstallError("git was not found. Install git and run this again.")
