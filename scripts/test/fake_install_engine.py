"""Stands in for the installer, to test the launcher's install screen quickly: prints
--progress-json output (status, progress, log lines, then done) over a few seconds.
A --dest containing "fail" ends with an error instead.

DAO_INSTALL_ENGINE=<this file's absolute path> build/DAORecomp/DAORecomp.exe, then Install
(a .py engine runs with the Python on PATH)."""
import json
import sys
import time

args = sys.argv[1:]
dest = args[args.index("--dest") + 1]


def emit(**item):
    sys.stdout.write(json.dumps(item) + "\n")
    sys.stdout.flush()


emit(log="args: " + " ".join(args))
emit(status="[1/7] Checking the game files")
emit(progress=None)
for i in range(30):
    emit(log=f"line {i} with é accents")
emit(status="[6/7] Building the game (this takes a while)")
emit(progress=0.42)
time.sleep(3)
if "fail" in dest:
    emit(error="Fake failure: the disc image isn't Dragon Age: Origins.")
    sys.exit(1)
emit(done=dest + "\\DAORecomp.exe")
