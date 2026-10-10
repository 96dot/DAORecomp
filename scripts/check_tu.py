"""Compile-checks one DAORecomp source file with the real build flags, writing the
object to a temp dir (never touches build/ outputs, safe to run alongside ninja).
Usage: python scripts/check_tu.py DAORecomp/kernel/foo.cpp"""
import os
import pathlib
import re
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parent.parent
env = root / "scripts" / "env.cmd"
src = pathlib.Path(sys.argv[1]).resolve()

cmds = subprocess.run(f'"{env}" ninja -C "{root / "build"}" -t commands DAORecomp',
                      capture_output=True, text=True, shell=True).stdout
template_src = str(root / "DAORecomp" / "kernel" / "xam.cpp")
line = next(l for l in cmds.splitlines() if "clang-cl" in l and l.rstrip().endswith(template_src))
out = pathlib.Path(tempfile.gettempdir()) / f"dao_check_tu_{src.stem}_{os.getpid()}.obj"
line = re.sub(r"/Fo\S+", lambda m: f"/Fo{out}", line)
line = re.sub(r"/Fd\S+", "", line)
# Dependency-file flags point into build/; drop them.
line = re.sub(r"(/clang:)?-M[DT]\S*|(/clang:)?-MF\s*\S+", "", line)
line = line.rstrip()[: -len(template_src)] + str(src)

r = subprocess.run(f'"{env}" {line}', capture_output=True, text=True, shell=True, cwd=root / "build")
sys.stdout.write(r.stdout[-8000:])
sys.stderr.write(r.stderr[-8000:])
out.unlink(missing_ok=True)
print("OK" if r.returncode == 0 else f"FAILED ({r.returncode})")
sys.exit(r.returncode)
