"""Checks out the pinned git sources (deps.GIT_SOURCES) under the repository."""
import subprocess
from pathlib import Path

from . import deps, util
from .util import IS_WINDOWS, REPO, InstallError, clean_env, run

# Marks checkouts this installer created, which it may move to another commit. It holds
# the URL and sparse list the checkout was made with, so a change to either is redone.
MARKER = "dao-installer"

# LF line endings everywhere (patch scripts and generated code expect them), long paths on
# Windows, quiet checkouts.
GIT_ENV = clean_env({
    "GIT_CONFIG_COUNT": "3",
    "GIT_CONFIG_KEY_0": "core.autocrlf", "GIT_CONFIG_VALUE_0": "false",
    "GIT_CONFIG_KEY_1": "advice.detachedHead", "GIT_CONFIG_VALUE_1": "false",
    "GIT_CONFIG_KEY_2": "core.longpaths", "GIT_CONFIG_VALUE_2": "true",
    "GIT_TERMINAL_PROMPT": "0",
})


def _git(reporter, path, *args):
    run(reporter, [*util.GIT, "-C", path, *args], env=GIT_ENV)


def _git_output(path, *args):
    result = subprocess.run([*util.GIT, "-C", str(path), *args], capture_output=True, text=True, env=GIT_ENV,
                            stdin=subprocess.DEVNULL,
                            creationflags=subprocess.CREATE_NO_WINDOW if IS_WINDOWS else 0)
    return result.stdout.strip() if result.returncode == 0 else None


def _head(path):
    return _git_output(path, "rev-parse", "HEAD")


def _git_dir(path):
    return _git_output(path, "rev-parse", "--absolute-git-dir")


LEGACY_MARKER = "Created by install.py; it may change this checkout.\n"


def _marker_text(source):
    return "\n".join(["Created by install.py; it may change this checkout.", source.url, *source.sparse]) + "\n"


def ensure(reporter, source):
    path = REPO / source.path
    name = source.url.rsplit("/", 1)[1].removesuffix(".git")
    is_checkout = (path / ".git").exists()
    git_dir = _git_dir(path) if is_checkout else None
    marker = Path(git_dir) / MARKER if git_dir else None
    ours = bool(marker and marker.exists())
    head = _head(path) if is_checkout else None
    # Up to date: at the commit, and (for a checkout made here) with this URL and sparse list.
    if head == source.commit and ours and marker.read_text() == LEGACY_MARKER:
        marker.write_text(_marker_text(source))  # made before markers held them: as pinned then
    if head == source.commit and (not ours or marker.read_text() == _marker_text(source)):
        reporter.log(f"{name}: up to date ({source.commit[:10]})")
        if source.submodules:
            _git(reporter, path, "submodule", "update", "--init", "--recursive", "--depth", "1")
        return

    if is_checkout:
        # (A git init stopped before its marker: nothing else is there yet.)
        if git_dir and not ours and head is None and [p.name for p in path.iterdir()] == [".git"]:
            ours = True
        if not ours:
            raise InstallError(f"{path} is a git checkout at another commit than the pinned "
                               f"{source.commit[:10]}. Move it away (or check out that commit) and run again.")
    elif path.exists() and any(path.iterdir()):
        raise InstallError(f"{path} exists but is not a git checkout. Move it away and run again.")
    else:
        path.mkdir(parents=True, exist_ok=True)
        _git(reporter, path, "init", "-q")
    reporter.log(f"{name}: fetching {source.commit[:10]} from {source.url}")
    # (Each run sets the remote, so an interrupted first run carries on, and a new URL is used.)
    if _git_output(path, "remote", "get-url", "origin") is None:
        _git(reporter, path, "remote", "add", "origin", source.url)
    else:
        _git(reporter, path, "remote", "set-url", "origin", source.url)

    fetch = ["fetch", "-q", "--depth", "1"]
    if source.sparse:
        _git(reporter, path, "sparse-checkout", "set", "--no-cone", *source.sparse)
        fetch.append("--filter=blob:none")
    elif (Path(_git_dir(path)) / "info" / "sparse-checkout").exists():
        _git(reporter, path, "sparse-checkout", "disable")
    reporter.check_cancel()
    _git(reporter, path, *fetch, "origin", source.commit)
    reporter.check_cancel()
    _git(reporter, path, "checkout", "-q", "--force", "FETCH_HEAD")
    if source.submodules:
        _git(reporter, path, "submodule", "update", "--init", "--recursive", "--depth", "1")
    (Path(_git_dir(path)) / MARKER).write_text(_marker_text(source))


def ensure_all(reporter):
    for i, source in enumerate(deps.GIT_SOURCES):
        reporter.progress(i / len(deps.GIT_SOURCES))
        reporter.check_cancel()
        ensure(reporter, source)
    reporter.progress(1.0)
