"""Checks that cancelling the installer ends a silent build step at once, and the
processes it started: runs a silent Python that starts a silent child sleeping 20 s
through installer.util.run and cancels it after 0.2 s, twice: with the step still
running, and with the step already exited (its child still holding the output open).
Expect each "cancelled after" well under a second, and no Python left but this one
(Windows)."""
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from installer import util  # noqa: E402


class Reporter:
    def __init__(self):
        self.cancel_event = threading.Event()

    def log(self, line):
        pass

    def check_cancel(self):
        if self.cancel_event.is_set():
            raise util.Cancelled()


CHILD = "subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(20)'])"
for name, step in (("step running", f"import subprocess, sys, time; {CHILD}; time.sleep(20)"),
                   ("step exited", f"import subprocess, sys; {CHILD}")):
    reporter = Reporter()
    threading.Timer(0.2, reporter.cancel_event.set).start()
    start = time.time()
    try:
        util.run(reporter, [sys.executable, "-c", step])
        print(f"{name}: FAIL: not cancelled")
    except util.Cancelled:
        print(f"{name}: cancelled after %.2f s" % (time.time() - start))
time.sleep(0.5)
if sys.platform == "win32":
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq python.exe", "/FO", "CSV"], capture_output=True,
                         text=True).stdout
    print("python processes now:", out.count("python.exe"), "(this one: 1)")
