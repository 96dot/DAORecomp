"""Builds the installer that scripts/build_setup_exe.py appends to DAORecomp.exe:
python.org's embeddable Python (signed by the Python Software Foundation) and
installer.pyz, the project source with the installer and truststore. The launcher
unpacks the folder and runs `python.exe installer.pyz`. (It replaced a PyInstaller
program, which antivirus deleted as a false positive.) Run on Windows:

    python scripts/build_installer_engine.py        -> dist/installer-engine/

Only files tracked by git are bundled (no game data, tools or build output).
"""
import os
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))

from installer import deps, util  # noqa: E402

WORK = REPO / "build" / "installer-engine"
OUT = REPO / "dist" / "installer-engine"
# truststore (downloads checked with Windows' certificate verifier), pinned by hash.
REQUIREMENTS = REPO / "scripts" / "installer-requirements.txt"
BUNDLE_NAME = "installer.pyz"
# Its entry point, as install.py (installer/__main__.py is run as a package module).
MAIN_SOURCE = 'import sys\nfrom installer.__main__ import main\nsys.exit(main())\n'
# The same date for every file, so the same source gives the same bundle.
ZIP_DATE = (2020, 1, 1, 0, 0, 0)


def version():
    """The release it is: the tag CI builds it for, else the checkout's (util.VERSION:
    its tag, -dirty if changed, or "dev")."""
    if os.environ.get("GITHUB_REF_TYPE") == "tag":
        return re.sub(r"[^A-Za-z0-9._+-]", "", os.environ["GITHUB_REF_NAME"]) or "dev"
    return util.VERSION


def add(bundle, name, data):
    """Adds `name` to the zip `bundle`, dated ZIP_DATE (the same input, the same bytes)."""
    info = zipfile.ZipInfo(name, ZIP_DATE)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o644 << 16
    bundle.writestr(info, data)


def main():
    if sys.platform != "win32":
        sys.exit("The installer is built on Windows (Linux users run install.py).")
    files = subprocess.run(["git", "-C", str(REPO), "ls-files", "-z"], capture_output=True, check=True,
                           text=True).stdout.split("\0")
    files = sorted(f for f in files if f and not f.startswith(".github/") and (REPO / f).is_file())
    tag = version()

    # truststore's wheel (a zip; pip only downloads it), its files added to the bundle's _lib/.
    wheels = WORK / "wheels"
    if wheels.exists():
        shutil.rmtree(wheels)
    subprocess.run([sys.executable, "-m", "pip", "download", "-q", "--disable-pip-version-check",
                    "--require-hashes", "--only-binary=:all:", "--no-deps", "-r", REQUIREMENTS,
                    "-d", wheels], check=True)

    if OUT.exists():
        shutil.rmtree(OUT)
    OUT.mkdir(parents=True)
    archive = util.download(util.Reporter(), deps.EMBED_PYTHON, WORK / "downloads")
    with zipfile.ZipFile(archive) as z:
        names = [n for n in z.namelist() if not n.endswith("/")]
        # (Its files are all at the top: anything else isn't the package it should be.)
        if any("/" in n or "\\" in n or n.startswith(".") for n in names):
            sys.exit(f"{archive.name} has files in folders")
        z.extractall(OUT, names)
    paths = list(OUT.glob("python3*._pth"))
    stdlib = list(OUT.glob("python3*.zip"))
    if len(paths) != 1 or len(stdlib) != 1 or not (OUT / "python.exe").is_file():
        sys.exit(f"{archive.name} isn't an embeddable Python (no python.exe, ._pth file and standard library)")
    # Where it imports from: its standard library, the bundle and its libraries, then
    # its folder (its extension modules); the ._pth file also isolates it from the PC's
    # Python settings.
    paths[0].write_text(f"{stdlib[0].name}\n{BUNDLE_NAME}\n{BUNDLE_NAME}\\_lib\n.\n", encoding="utf-8")

    with zipfile.ZipFile(OUT / BUNDLE_NAME, "w") as bundle:
        add(bundle, "__main__.py", MAIN_SOURCE)
        add(bundle, "VERSION", tag + "\n")
        for rel in files:
            add(bundle, rel, (REPO / rel).read_bytes())
        for wheel in sorted(wheels.glob("*.whl")):
            with zipfile.ZipFile(wheel) as w:
                # (With its .dist-info: its license.)
                for name in w.namelist():
                    if not name.endswith("/"):
                        add(bundle, f"_lib/{name}", w.read(name))
    size = sum(p.stat().st_size for p in OUT.iterdir())
    print(f"built {OUT} ({len(files)} source files, {tag}; {size / 2**20:.1f} MB)")


if __name__ == "__main__":
    main()
