"""installer_play.py <install folder>

Opens the installer's window as after a finished install of <install folder> and
presses its Play button (which should start the game and close the window)."""
import sys
import tkinter as tk
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from installer import gui  # noqa: E402

dest = Path(sys.argv[1])
root = tk.Tk()
app = gui.App(root, "", str(dest))
app.result = dest / "DAORecomp.exe"
app.finish(True, None)
print("play button:", app.play_button["state"])
root.after(500, app.play_button.invoke)
root.mainloop()
print("installer window closed")
