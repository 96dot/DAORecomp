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

from . import deps, pipeline
from .util import IS_WINDOWS, Cancelled, InstallError, Reporter


def default_dest():
    return Path.home() / "Games" / "DAORecomp"


class App:
    def __init__(self, root, iso="", dest=""):
        self.root = root
        self.events = queue.Queue()
        self.worker = None
        self.reporter = None
        self.result = None

        root.title("Dragon Age: Origins Recompiled - Installer")
        root.minsize(720, 480)
        frame = ttk.Frame(root, padding=12)
        frame.pack(fill="both", expand=True)
        frame.columnconfigure(1, weight=1)

        intro = ttk.Label(frame, text=f"Builds the game from your own copy of {deps.GAME_TITLE}.")
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

        self.accept = tk.BooleanVar(value=False)
        license_row = ttk.Frame(frame)
        license_row.grid(row=3, column=0, columnspan=3, sticky="w", pady=(6, 0))
        ttk.Checkbutton(license_row, variable=self.accept,
                        text="I accept the license of the Microsoft Visual C++ build tools and Windows SDK "
                             "(downloaded to build the game)").pack(side="left")
        link = ttk.Label(license_row, text="Read it", foreground="#2a6ad8", cursor="hand2")
        link.pack(side="left", padx=6)
        link.bind("<Button-1>", lambda e: webbrowser.open(deps.MSVC_LICENSE_URL))

        note = ttk.Label(frame, text="Needs about 25 GB: 8 GB for the game, the rest for tools and the build "
                                     "(in the installer's tools/ and build/ folders). The first build takes "
                                     "30-60 minutes.", foreground="#666")
        note.grid(row=4, column=0, columnspan=3, sticky="w", pady=6)
        # Wrap the texts at the window width (in real pixels, whatever the display scaling).
        frame.bind("<Configure>", lambda e: [w.configure(wraplength=max(e.width - 30, 200)) for w in (intro, note)])

        self.status = tk.StringVar(value="Ready.")
        ttk.Label(frame, textvariable=self.status).grid(row=5, column=0, columnspan=3, sticky="w", pady=(6, 2))
        self.bar = ttk.Progressbar(frame, mode="determinate", maximum=1.0)
        self.bar.grid(row=6, column=0, columnspan=3, sticky="ew")

        self.log = ScrolledText(frame, height=16, state="disabled", wrap="none",
                                font=("Consolas" if IS_WINDOWS else "Monospace", 9))
        self.log.grid(row=7, column=0, columnspan=3, sticky="nsew", pady=8)
        frame.rowconfigure(7, weight=1)

        buttons = ttk.Frame(frame)
        buttons.grid(row=8, column=0, columnspan=3, sticky="e")
        self.open_button = ttk.Button(buttons, text="Open install folder", command=self.open_dest,
                                      state="disabled")
        self.open_button.pack(side="left", padx=4)
        self.install_button = ttk.Button(buttons, text="Install", command=self.start)
        self.install_button.pack(side="left", padx=4)
        self.cancel_button = ttk.Button(buttons, text="Close", command=self.close)
        self.cancel_button.pack(side="left")

        root.protocol("WM_DELETE_WINDOW", self.close)
        root.after(100, self.pump)

    def pick_iso(self):
        path = filedialog.askopenfilename(title="Dragon Age: Origins disc image",
                                          filetypes=[("Disc images", "*.iso"), ("All files", "*")])
        if path:
            self.iso.set(path)

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
        if not self.iso.get().strip() or not self.dest.get().strip():
            messagebox.showerror("Installer", "Choose the disc image and the install folder.")
            return
        if not self.accept.get():
            messagebox.showerror("Installer", "The build tools license must be accepted to build the game.")
            return
        options = pipeline.Options(iso=Path(self.iso.get().strip()), dest=Path(self.dest.get().strip()),
                                   accept_license=True)
        self.reporter = Reporter(log=lambda t: self.events.put(("log", t)),
                                 status=lambda t: self.events.put(("status", t)),
                                 progress=lambda f: self.events.put(("progress", f)))
        self.install_button.configure(state="disabled")
        self.iso_button.configure(state="disabled")
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
        self.dest_button.configure(state="normal")
        self.cancel_button.configure(text="Close")
        if ok:
            self.open_button.configure(state="normal")
            messagebox.showinfo("Installer", f"Installed. Start the game with\n{self.result}")
        else:
            self.status.set(message)
            self.append("!! " + message)
            if message != "Cancelled.":
                messagebox.showerror("Installer", message + "\n\nRunning the installer again resumes "
                                                            "where it stopped.")

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


def main(iso="", dest=""):
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
    App(root, iso, dest)
    root.mainloop()
