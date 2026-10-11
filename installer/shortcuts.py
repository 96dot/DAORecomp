"""Start menu and desktop shortcuts to the installed game (Windows)."""
import ctypes
import os
import subprocess
from pathlib import Path

NAME = "Dragon Age Origins Recompiled"

# Writes the shortcuts through Windows Script Host; the paths come in the
# environment (the links separated by |, which paths can't contain), so nothing in
# them is read as PowerShell.
_SCRIPT = ("$w = New-Object -ComObject WScript.Shell; foreach ($l in $env:DAO_LINKS -split '\\|') { "
           "$s = $w.CreateShortcut($l); $s.TargetPath = $env:DAO_TARGET; $s.WorkingDirectory = $env:DAO_FOLDER; "
           "$s.Description = 'Dragon Age: Origins (recompiled)'; $s.Save() }")
# Known folders (SHGetFolderPathW): the Start menu's Programs, the desktop.
_CSIDL_PROGRAMS = 0x0002
_CSIDL_DESKTOPDIRECTORY = 0x0010


def _known_folder(csidl):
    buffer = ctypes.create_unicode_buffer(260)
    if ctypes.windll.shell32.SHGetFolderPathW(None, csidl, None, 0, buffer) != 0:
        return None
    return Path(buffer.value)


def _folders():
    """The user's Start menu programs folder and desktop (wherever they're redirected)."""
    return [f for f in (_known_folder(_CSIDL_PROGRAMS), _known_folder(_CSIDL_DESKTOPDIRECTORY)) if f]


def exist():
    """Whether there's a Start menu shortcut (an install made with shortcuts; the
    desktop one may have been tidied away since)."""
    folders = _folders()
    return bool(folders) and (folders[0] / f"{NAME}.lnk").exists()


def create(reporter, dest):
    """Shortcuts to dest/DAORecomp.exe; a failure is only logged."""
    target = Path(dest) / "DAORecomp.exe"
    links = [folder / f"{NAME}.lnk" for folder in _folders()]
    # Windows' own PowerShell, not one found first in the install folder or on PATH.
    powershell = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32/WindowsPowerShell/v1.0/powershell.exe"
    env = dict(os.environ, DAO_LINKS="|".join(map(str, links)), DAO_TARGET=str(target), DAO_FOLDER=str(Path(dest)))
    try:
        result = subprocess.run([str(powershell), "-NoProfile", "-NonInteractive", "-Command", _SCRIPT], env=env,
                                capture_output=True, text=True, stdin=subprocess.DEVNULL,
                                creationflags=subprocess.CREATE_NO_WINDOW)
        problem = result.stderr.strip()[-300:]
    except OSError as e:
        problem = str(e)
    for link in links:
        if link.exists():
            reporter.log(f"Shortcut: {link}")
        else:
            reporter.log(f"Warning: can't create the shortcut {link}: {problem}")
