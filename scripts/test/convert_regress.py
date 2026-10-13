"""convert_regress.py <work dir> <PC mod folder>=<earlier conversion>...

Converts copies of PC mods with the current build (DAO_CONVERT_MOD, as the launcher's
Convert does) in <work dir>, and compares each with an earlier conversion of the same
mod: files the same, different, or new. Run it after changing the converter to see
exactly what changed. The PC folders are only read; a work folder whose copies would
overlap an input or an earlier conversion, or be links, is refused."""
import filecmp
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXE = Path(os.environ.get("EXE", ROOT / "build" / "DAORecomp" / "DAORecomp.exe"))


def main():
    if len(sys.argv) < 3 or any("=" not in a for a in sys.argv[2:]):
        print(__doc__)
        return 2
    work = Path(sys.argv[1]).resolve()
    cases = [tuple(Path(p).resolve() for p in case.split("=", 1)) for case in sys.argv[2:]]
    # The work copies are deleted first: never an input or an earlier conversion (of any
    # case), a folder holding one, or a link (deleting through it empties its target).
    inputs = [p for case in cases for p in case]
    for pc, _ in cases:
        for old in (work / pc.name, work / ".originals" / pc.name):
            if old.is_symlink() or (hasattr(old, "is_junction") and old.is_junction()):
                print(f"{old} is a link: use a work folder away from the inputs")
                return 2
            old = old.resolve()
            for kept in inputs:
                if old == kept or old in kept.parents or kept in old.parents:
                    print(f"{old} overlaps {kept}: use a work folder away from the inputs")
                    return 2
    work.mkdir(parents=True, exist_ok=True)
    for pc, earlier in cases:
        mod = work / pc.name
        for old in (mod, work / ".originals" / pc.name):
            if old.exists():
                shutil.rmtree(old)
        shutil.copytree(pc, mod)
        env = dict(os.environ, DAO_CONVERT_MOD=str(mod), DAO_LOG_STDOUT="1")
        r = subprocess.run([str(EXE)], cwd=ROOT, env=env, capture_output=True, text=True, errors="replace",
                           timeout=900)
        print(f"== {pc.name}: exit {r.returncode}")
        same, differ, new = 0, [], []
        for folder, _, files in os.walk(mod):
            for f in files:
                rel = os.path.relpath(os.path.join(folder, f), mod)
                old = earlier / rel
                if not old.exists():
                    new.append(rel)
                elif filecmp.cmp(mod / rel, old, shallow=False):
                    same += 1
                else:
                    differ.append(rel)
        print(f"   same {same}, differ {len(differ)}, not in earlier {len(new)}")
        for rel in differ[:8]:
            print("    differs:", rel)
        for rel in new[:8]:
            print("    new only:", rel)
    if (work / ".converting").exists():
        print("WARNING: a .converting work folder was left behind")
    return 0


if __name__ == "__main__":
    sys.exit(main())
