"""Builds DAORecomp-Installer.exe: the installer plus the project source in one file
(PyInstaller), so users need neither Python nor git. Run from a git checkout:

    python scripts/build_installer_exe.py        -> dist/DAORecomp-Installer.exe

Only files tracked by git are bundled (no game data, tools or build output).
"""
import os
import re
import shutil
import subprocess
import sys
import venv
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
WORK = REPO / "build" / "installer-exe"
PYINSTALLER = "pyinstaller==6.22.3"
NAME = "DAORecomp-Installer"


def main():
    if sys.platform != "win32":
        sys.exit("The installer executable is built on Windows (Linux users run install.py).")
    files = subprocess.run(["git", "-C", str(REPO), "ls-files", "-z"], capture_output=True, check=True,
                           text=True).stdout.split("\0")
    files = [f for f in files if f and not f.startswith(".github/")]

    # The source the executable carries.
    staging = WORK / "source"
    if staging.exists():
        shutil.rmtree(staging)
    for rel in files:
        dst = staging / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(REPO / rel, dst)
    # The release it is: the tag CI builds it for, else as installer/util.py names a
    # checkout (its tag, -dirty if changed, or "dev").
    if os.environ.get("GITHUB_REF_TYPE") == "tag":
        version = os.environ["GITHUB_REF_NAME"]
    else:
        version = subprocess.run(["git", "-C", str(REPO), "describe", "--tags", "--exact-match", "--dirty"],
                                 capture_output=True, text=True).stdout.strip()
    version = re.sub(r"[^A-Za-z0-9._+-]", "", version) or "dev"
    (staging / "VERSION").write_text(version + "\n", encoding="utf-8")
    print(f"bundling {len(files)} source files ({version})")

    env_dir = WORK / "venv"
    python = env_dir / "Scripts" / "python.exe"
    if not python.exists():
        venv.EnvBuilder(with_pip=True).create(env_dir)
    subprocess.run([python, "-m", "pip", "install", "-q", "--disable-pip-version-check", PYINSTALLER], check=True)

    subprocess.run([
        python, "-m", "PyInstaller", "--noconfirm", "--clean", "--onefile", "--console",
        "--name", NAME,
        "--distpath", REPO / "dist", "--workpath", WORK / "pyinstaller", "--specpath", WORK,
        # The helper scripts the installer imports from the bundled source at run time.
        "--paths", REPO / "scripts",
        "--hidden-import", "xiso", "--hidden-import", "sync_dir", "--hidden-import", "patch_recompiler",
        "--add-data", f"{staging}{os.pathsep}source",
        REPO / "install.py",
    ], check=True)
    exe = REPO / "dist" / f"{NAME}.exe"
    print(f"built {exe} ({exe.stat().st_size / 2**20:.1f} MB)")


if __name__ == "__main__":
    main()
