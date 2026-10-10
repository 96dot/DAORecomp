"""Compiles the vendored Xenia D3D12 shaders (xenia/src/xenia/{gpu,ui}/shaders)
to DXBC headers in <shaders>/bytecode/d3d12_5_1 with FXC, skipping up-to-date ones."""
import concurrent.futures
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "xenia" / "tools_build" / "compile_shader_dxbc.py"
STAGES = {"vs", "hs", "ds", "gs", "ps", "cs"}


def jobs():
    for sub in ("gpu", "ui"):
        shader_dir = ROOT / "xenia" / "src" / "xenia" / sub / "shaders"
        out_dir = shader_dir / "bytecode" / "d3d12_5_1"
        includes = list(shader_dir.glob("*.xesli")) + list(shader_dir.glob("*.hlsli"))
        newest_include = max((p.stat().st_mtime for p in includes), default=0)
        for src in sorted(list(shader_dir.glob("*.xesl")) + list(shader_dir.glob("*.hlsl"))):
            ident = src.name.rsplit(".", 1)[0].replace(".", "_")
            if ident[-2:] not in STAGES:
                continue
            out = out_dir / f"{ident}.h"
            if out.exists() and out.stat().st_mtime >= max(src.stat().st_mtime, newest_include):
                continue
            yield src, out


def compile_one(job):
    src, out = job
    r = subprocess.run([sys.executable, str(SCRIPT), str(src), str(out)], capture_output=True, text=True)
    return src.name, r.returncode, r.stderr


def main():
    todo = list(jobs())
    failed = 0
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        for name, code, err in pool.map(compile_one, todo):
            if code:
                failed += 1
                print(f"FAILED {name}\n{err}")
    print(f"{len(todo) - failed}/{len(todo)} shaders compiled")
    return 1 if failed else 0


sys.exit(main())
