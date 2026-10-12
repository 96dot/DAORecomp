"""Tkinter front end: pick the ISO and the install folder, watch the log."""
import os
import queue
import subprocess
import threading
import tkinter as tk
import webbrowser
from pathlib import Path
from tkinter import filedialog, messagebox, ttk
from tkinter.scrolledtext import ScrolledText

from . import deps, pipeline, shortcuts, toolchain
from .util import IS_WINDOWS, REPO, VERSION, Cancelled, InstallError, Reporter


def default_dest():
    return Path.home() / "Games" / "DAORecomp"


class App:
    def __init__(self, root, iso="", dest="", update=False):
        self.root = root
        self.events = queue.Queue()
        self.worker = None
        self.reporter = None
        self.result = None

        root.title(f"Dragon Age: Origins Recompiled - Installer {VERSION}")
        root.minsize(720, 480)
        frame = ttk.Frame(root, padding=12)
        frame.pack(fill="both", expand=True)
        frame.columnconfigure(1, weight=1)

        intro = ttk.Label(frame, text=(
            f"Updates the game in {dest or 'the install folder'} to {VERSION}; your saves, settings and mods are "
            "kept." if update else
            f"Builds the game from your own copy of {deps.GAME_TITLE}. To update an install, choose its "
            "folder: the disc image is needed only the first time."))
        intro.grid(row=0, column=0, columnspan=3, sticky="w", pady=(0, 10))

        self.iso = tk.StringVar(value=str(iso))
        self.dest = tk.StringVar(value=str(dest or default_dest()))
        ttk.Label(frame, text="Disc image (.iso):").grid(row=1, column=0, sticky="w")
        ttk.Entry(frame, textvariable=self.iso).grid(row=1, column=1, sticky="ew", padx=6)
        self.iso_button = ttk.Button(frame, text="Browse...", command=self.pick_iso)
        self.iso_button.grid(row=1, column=2)
        ttk.Label(frame, text="Install folder:").grid(row=2, column=0, sticky="w", pady=4)
        ttk.Entry(frame, textvariable=self.dest).grid(row=2, column=1, sticky="ew", padx=6)
        self.dest_button = ttk.Button(frame, text="Browse...", command=self.pick_dest)
        self.dest_button.grid(row=2, column=2)
        self.title_update = tk.StringVar()
        ttk.Label(frame, text="Title Update 6 or 7 (optional):").grid(row=3, column=0, sticky="w")
        ttk.Entry(frame, textvariable=self.title_update).grid(row=3, column=1, sticky="ew", padx=6)
        self.update_button = ttk.Button(frame, text="Browse...", command=self.pick_title_update)
        self.update_button.grid(row=3, column=2)

        # Accepted on an earlier run if the CRT/SDK are already set up.
        self.accept = tk.BooleanVar(value=toolchain.winsysroot_ready())
        license_row = ttk.Frame(frame)
        license_row.grid(row=4, column=0, columnspan=3, sticky="w", pady=(6, 0))
        ttk.Checkbutton(license_row, variable=self.accept,
                        text="I accept the license of the Microsoft Visual C++ build tools and Windows SDK "
                             "(downloaded to build the game)").pack(side="left")
        link = ttk.Label(license_row, text="Read it", foreground="#2a6ad8", cursor="hand2")
        link.pack(side="left", padx=6)
        link.bind("<Button-1>", lambda e: webbrowser.open(deps.MSVC_LICENSE_URL))

        # An update keeps the install's choice.
        self.make_shortcuts = tk.BooleanVar(value=IS_WINDOWS and (shortcuts.exist() if update else True))
        if IS_WINDOWS:
            ttk.Checkbutton(frame, variable=self.make_shortcuts, text="Create Start menu and desktop shortcuts").grid(
                row=5, column=0, columnspan=3, sticky="w", pady=(4, 0))

        note = ttk.Label(frame, text="Needs about 25 GB: 8 GB for the game, the rest for the tools and the "
                                     f"build in {REPO} (keep it for quick updates, or delete it after "
                                     "installing). The first build takes 30-60 minutes; with the title update, "
                                     "about twice as long, and more space.", foreground="#666")
        note.grid(row=6, column=0, columnspan=3, sticky="w", pady=6)
        # Wrap the texts at the window width (in real pixels, whatever the display scaling).
        frame.bind("<Configure>", lambda e: [w.configure(wraplength=max(e.width - 30, 200)) for w in (intro, note)])

        self.status = tk.StringVar(value="Ready.")
        ttk.Label(frame, textvariable=self.status).grid(row=7, column=0, columnspan=3, sticky="w", pady=(6, 2))
        self.bar = ttk.Progressbar(frame, mode="determinate", maximum=1.0)
        self.bar.grid(row=8, column=0, columnspan=3, sticky="ew")

        self.log = ScrolledText(frame, height=16, state="disabled", wrap="none",
                                font=("Consolas" if IS_WINDOWS else "Monospace", 9))
        self.log.grid(row=9, column=0, columnspan=3, sticky="nsew", pady=8)
        frame.rowconfigure(9, weight=1)

        buttons = ttk.Frame(frame)
        buttons.grid(row=10, column=0, columnspan=3, sticky="e")
        self.open_button = ttk.Button(buttons, text="Open install folder", command=self.open_dest,
                                      state="disabled")
        self.open_button.pack(side="left", padx=4)
        self.play_button = ttk.Button(buttons, text="Play", command=self.play, state="disabled")
        self.play_button.pack(side="left", padx=4)
        self.install_button = ttk.Button(buttons, text="Install", command=self.start)
        self.install_button.pack(side="left", padx=4)
        self.cancel_button = ttk.Button(buttons, text="Close", command=self.close)
        self.cancel_button.pack(side="left")

        root.protocol("WM_DELETE_WINDOW", self.close)
        root.after(100, self.pump)
        # From the launcher's Update: go ahead if nothing needs choosing.
        if update and self.accept.get() and dest and pipeline.game_extracted(Path(dest).expanduser() / "game"):
            root.after(300, self.start)

    def pick_iso(self):
        path = filedialog.askopenfilename(title="Dragon Age: Origins disc image",
                                          filetypes=[("Disc images", "*.iso"), ("All files", "*")])
        if path:
            self.iso.set(path)

    def pick_title_update(self):
        # Its package from the console (e.g. tu00000001_00000000): needed by Awakening
        # and the DLC campaigns after it; an install keeps it.
        path = filedialog.askopenfilename(title="Dragon Age: Origins Title Update 6 or 7 package",
                                          filetypes=[("All files", "*")])
        if path:
            self.title_update.set(path)

    def pick_dest(self):
        path = filedialog.askdirectory(title="Install folder", mustexist=False)
        if path:
            self.dest.set(path)

    def append(self, text):
        self.log.configure(state="normal")
        self.log.insert("end", text + "\n")
        if int(self.log.index("end-1c").split(".")[0]) > 5000:
            self.log.delete("1.0", "1000.0")
        self.log.see("end")
        self.log.configure(state="disabled")

    def start(self):
        iso, dest = self.iso.get().strip(), self.dest.get().strip()
        if not dest:
            messagebox.showerror("Installer", "Choose the install folder.")
            return
        if not iso and not pipeline.game_extracted(Path(dest).expanduser() / "game"):
            messagebox.showerror("Installer", "Choose the disc image (the install folder has no game files yet).")
            return
        if not self.accept.get():
            messagebox.showerror("Installer", "The build tools license must be accepted to build the game.")
            return
        title_update = self.title_update.get().strip()
        options = pipeline.Options(iso=Path(iso) if iso else None, dest=Path(dest),
                                   title_update=Path(title_update) if title_update else None,
                                   accept_license=True, shortcuts=self.make_shortcuts.get())
        self.reporter = Reporter(log=lambda t: self.events.put(("log", t)),
                                 status=lambda t: self.events.put(("status", t)),
                                 progress=lambda f: self.events.put(("progress", f)))
        self.install_button.configure(state="disabled")
        self.play_button.configure(state="disabled")
        self.iso_button.configure(state="disabled")
        self.update_button.configure(state="disabled")
        self.dest_button.configure(state="disabled")
        self.cancel_button.configure(text="Cancel")
        self.worker = threading.Thread(target=self.work, args=(options,), daemon=True)
        self.worker.start()

    def work(self, options):
        try:
            self.result = pipeline.run_all(self.reporter, options)
            self.events.put(("done", None))
        except Cancelled:
            self.events.put(("failed", "Cancelled."))
        except InstallError as e:
            self.events.put(("failed", str(e)))
        except Exception as e:  # report anything unexpected instead of dying silently
            import traceback
            self.events.put(("log", traceback.format_exc()))
            self.events.put(("failed", f"Unexpected error: {e}"))

    def pump(self):
        try:
            while True:
                kind, value = self.events.get_nowait()
                if kind == "log":
                    self.append(value)
                elif kind == "status":
                    self.status.set(value)
                    self.append("== " + value)
                elif kind == "progress":
                    if value is None:
                        if str(self.bar["mode"]) != "indeterminate":
                            self.bar.configure(mode="indeterminate")
                            self.bar.start(15)
                    else:
                        if str(self.bar["mode"]) != "determinate":
                            self.bar.stop()
                            self.bar.configure(mode="determinate")
                        self.bar["value"] = value
                elif kind in ("done", "failed"):
                    self.finish(kind == "done", value)
        except queue.Empty:
            pass
        self.root.after(100, self.pump)

    def finish(self, ok, message):
        self.bar.stop()
        self.bar.configure(mode="determinate")
        self.bar["value"] = 1.0 if ok else 0
        self.worker = None
        self.install_button.configure(state="normal")
        self.iso_button.configure(state="normal")
        self.update_button.configure(state="normal")
        self.dest_button.configure(state="normal")
        self.cancel_button.configure(text="Close")
        if ok:
            self.open_button.configure(state="normal")
            self.play_button.configure(state="normal")
            self.play_button.focus_set()
            self.status.set(f"Installed {VERSION}. Press Play to start the game (or run {self.result}).")
        else:
            self.status.set(message)
            self.append("!! " + message)
            if message != "Cancelled.":
                messagebox.showerror("Installer", message + "\n\nRunning the installer again resumes "
                                                            "where it stopped.")

    def play(self):
        """Starts the installed game (its launcher) and closes the installer."""
        game = Path(self.result)
        try:
            if IS_WINDOWS:
                subprocess.Popen([str(game)], cwd=game.parent, creationflags=subprocess.DETACHED_PROCESS)
            else:
                subprocess.Popen([str(game)], cwd=game.parent, start_new_session=True)
        except OSError as e:
            messagebox.showerror("Installer", f"Can't start {game}: {e}")
            return
        self.root.destroy()

    def open_dest(self):
        path = str(Path(self.dest.get()).expanduser())
        if IS_WINDOWS:
            os.startfile(path)
        else:
            subprocess.Popen(["xdg-open", path])

    def close(self):
        if self.worker:
            if messagebox.askyesno("Installer", "Stop the installation? Running it again resumes."):
                self.reporter.cancel_event.set()
                self.status.set("Stopping...")
            return
        self.root.destroy()


def main(iso="", dest="", update=False):
    if IS_WINDOWS:
        try:  # sharp text on scaled displays
            import ctypes
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except (AttributeError, OSError):
            pass
    root = tk.Tk()
    try:
        ttk.Style(root).theme_use("vista" if IS_WINDOWS else "clam")
    except tk.TclError:
        pass
    App(root, iso, dest, update)
    root.mainloop()
