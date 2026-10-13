"""Copies what DAORecomp builds of lmxxf's neural rendering runtime (MIT; the HIP port
of DLSS 5's neural rendering, github.com/lmxxf/dlss5-on-amd-9070xt-porting) from a
checkout of it into thirdparty/lmxxf-nr: the runtime's source, its HIP kernels' source
and recipe, and the shaders it compiles at run time. Never the network's weights, which
are NVIDIA's (players point the game at lmxxf's package for them).

    python scripts/vendor_lmxxf.py <checkout>
"""
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
OUT = REPO / "thirdparty" / "lmxxf-nr"

# The runtime (src/LmxxfNrRuntime.cpp) and what it includes, its licenses, its kernels'
# recipe, and its settings template (the one for this runtime: the network's options).
FILES = ["LICENSE", "include/LmxxfNrApi.h", "hip/HIP-API-LICENSE.txt", "hip/build-modules.ps1",
         "scripts/hip-re9-flags.txt"]
FOLDERS = {
    "src": ("LmxxfNrRuntime.cpp", "LmxxfProductionOptions.h", "runtime_strength_config.h", "native_*.h"),
    "Development/HIP": ("hip_api.h", "hip_d3d12_bridge.h", "hip_device_properties.h", "hip_reference_network.h",
                        "packed_weights.h", "submit_pulse.h", "submit_pulse_hip.h", "swin_persistent_network.h",
                        "swin_persistent_types.h"),
    "hip": ("*.hip", "*.inc"),
    "shaders": ("*.hlsl",),
}


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    source = Path(sys.argv[1])
    commit = subprocess.run(["git", "-C", str(source), "rev-parse", "HEAD"], capture_output=True, text=True,
                            check=True).stdout.strip()
    if OUT.exists():
        shutil.rmtree(OUT)
    copied = 0
    missing = [rel for rel in FILES if not (source / rel).is_file()]
    missing += [f"{folder}/{pattern}" for folder, patterns in FOLDERS.items() for pattern in patterns
                if not any(path.is_file() for path in (source / folder).glob(pattern))]
    if missing:
        sys.exit(f"Not in {source} (moved upstream?): {', '.join(missing)}")
    for rel in FILES:
        (OUT / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source / rel, OUT / rel)
        copied += 1
    for folder, patterns in FOLDERS.items():
        (OUT / folder).mkdir(parents=True, exist_ok=True)
        for pattern in patterns:
            for path in sorted((source / folder).glob(pattern)):
                if path.is_file():
                    shutil.copyfile(path, OUT / folder / path.name)
                    copied += 1
    (OUT / "SOURCE").write_text(f"https://github.com/lmxxf/dlss5-on-amd-9070xt-porting {commit}\n", encoding="utf-8")
    print(f"copied {copied} files from {commit}")


if __name__ == "__main__":
    main()
