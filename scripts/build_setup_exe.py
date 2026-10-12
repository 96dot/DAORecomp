"""Builds what players download: dist/DAORecomp.exe, the launcher with its Install view
and no game code, carrying the installer (dist/DAORecomp-Installer.exe, built first by
build_installer_exe.py). Run from a git checkout on Windows:
    python scripts/build_setup_exe.py [--skip-installer]
Running it accepts the Microsoft build tools license for their download, as
install.py --accept-msvc-license does.
"""
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
sys.path.insert(0, str(REPO / "scripts"))

import build_installer_exe  # noqa: E402
from installer import pipeline  # noqa: E402
from installer.util import InstallError, Reporter  # noqa: E402


def main():
    if "--skip-installer" not in sys.argv:
        build_installer_exe.main()
    engine = REPO / "dist" / f"{build_installer_exe.NAME}.exe"
    if not engine.exists():
        sys.exit(f"{engine} isn't built.")
    try:
        pipeline.build_setup(Reporter(), engine, REPO / "dist" / "DAORecomp.exe")
    except InstallError as e:
        sys.exit(f"Error: {e}")


if __name__ == "__main__":
    main()
