"""Checks out the pinned git sources (deps.GIT_SOURCES) under the repository."""
import subprocess
from pathlib import Path

from . import deps
from .util import REPO, InstallError, clean_env, run

# Marks checkouts this installer created, which it may move to another commit.
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
    run(reporter, ["git", "-C", path, *args], env=GIT_ENV)


def _head(path):
    result = subprocess.run(["git", "-C", str(path), "rev-parse", "HEAD"], capture_output=True, text=True,
                            env=GIT_ENV)
    return result.stdout.strip() if result.returncode == 0 else None


def _git_dir(path):
    result = subprocess.run(["git", "-C", str(path), "rev-parse", "--absolute-git-dir"], capture_output=True,
                            text=True, env=GIT_ENV)
    return result.stdout.strip() if result.returncode == 0 else None


def ensure(reporter, source):
    path = REPO / source.path
    name = source.url.rsplit("/", 1)[1].removesuffix(".git")
    is_checkout = (path / ".git").exists()
    if is_checkout and _head(path) == source.commit:
        reporter.log(f"{name}: up to date ({source.commit[:10]})")
        if source.submodules:
            _git(reporter, path, "submodule", "update", "--init", "--recursive", "--depth", "1")
        return

    if is_checkout:
        git_dir = _git_dir(path)
        if not git_dir or not (Path(git_dir) / MARKER).exists():
            raise InstallError(f"{path} is a git checkout at another commit than the pinned "
                               f"{source.commit[:10]}. Move it away (or check out that commit) and run again.")
    elif path.exists() and any(path.iterdir()):
        raise InstallError(f"{path} exists but is not a git checkout. Move it away and run again.")
    else:
        reporter.log(f"{name}: fetching {source.commit[:10]} from {source.url}")
        path.mkdir(parents=True, exist_ok=True)
        _git(reporter, path, "init", "-q")
        (path / ".git" / MARKER).write_text("Created by install.py; it may change this checkout.\n")
        _git(reporter, path, "remote", "add", "origin", source.url)

    fetch = ["fetch", "-q", "--depth", "1"]
    if source.sparse:
        _git(reporter, path, "sparse-checkout", "set", "--no-cone", *source.sparse)
        fetch.append("--filter=blob:none")
    reporter.check_cancel()
    _git(reporter, path, *fetch, "origin", source.commit)
    reporter.check_cancel()
    _git(reporter, path, "checkout", "-q", "--force", "FETCH_HEAD")
    if source.submodules:
        _git(reporter, path, "submodule", "update", "--init", "--recursive", "--depth", "1")


def ensure_all(reporter):
    for i, source in enumerate(deps.GIT_SOURCES):
        reporter.progress(i / len(deps.GIT_SOURCES))
        reporter.check_cancel()
        ensure(reporter, source)
    reporter.progress(1.0)
