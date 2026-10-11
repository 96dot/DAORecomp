# Dragon Age: Origins — Recompiled

A static recompilation of the Xbox 360 version of *Dragon Age: Origins* into a native
x86-64 Windows program, with a Direct3D 12 renderer based on Xenia's. It adds:

- rendering at up to 4K (and higher), with FSR 1 / CAS scaling of the whole image
- temporal upscaling and anti-aliasing with AMD FSR 4 (RDNA 4) or FSR 3.1, using
  motion vectors generated for the game's 3D scenes
- 60 fps (the original is locked to 30)
- SDL3 input, including DualSense and other controllers
- a launcher for settings, mods (file overrides) and code patches

**This repository contains no game code or data.** You need your own copy of the
game: an ISO image of the Xbox 360 disc. The installer extracts it, translates the
game's code to C++ on your computer, and builds the program.

## Installing

You need:

- An x86-64 PC running Windows 10/11 or Linux, with a Direct3D 12 capable GPU.
- The disc image of *Dragon Age: Origins* for Xbox 360 (title ID 454108C0). The
  installer checks that it is the supported release.
- About 25 GB of disk space: 8 GB for the game, the rest for the tools and the build.
  The first build takes 30–60 minutes, depending on the CPU.

### Windows

Run **DAORecomp-Installer.exe**. Pick the ISO and an install folder, accept the build
tools license, and press **Install**. Nothing else needs to be installed first: the
installer downloads what it needs (including a portable git if you have none).

From a terminal instead:

```
DAORecomp-Installer.exe --iso "D:\Games\Dragon Age Origins.iso" --dest "D:\Games\DAORecomp"
```

The tools and the build go into `%LOCALAPPDATA%\DAORecomp-build` (set `DAO_WORK_DIR` to
use another folder). Keep it and a newer installer only rebuilds what changed; delete it
to get the space back.

Windows may warn about the installer because it isn't code-signed (*More info* →
*Run anyway*).

### Linux (or from the source on Windows)

Install Python 3.10+ with Tkinter, git and a C++ standard library (Debian/Ubuntu:
`sudo apt install build-essential python3-tk git`; Fedora:
`sudo dnf install gcc-c++ python3-tkinter git`; Arch: `sudo pacman -S base-devel tk git`). Then:

```
git clone <this repository> DAORecomp
cd DAORecomp
python3 install.py
```

or `python3 install.py --iso "/path/to/Dragon Age Origins.iso" --dest ~/Games/DAORecomp`.
Here the tools and the build go into `tools/` and `build/` inside the checkout, and
after a `git pull` the installer rebuilds only what changed.

Running the installer again resumes an interrupted install.

### What it downloads

Everything is pinned to exact versions and checked (`installer/deps.py`):

- Source code: Xenia's third-party libraries, XenonRecomp, SDL 3, the AMD FidelityFX SDK
  (headers and the signed FSR DLLs).
- LLVM/Clang 22 (or your system's LLVM 18+ if it has `clang-cl`, `lld-link`, `llvm-lib`
  and `llvm-rc`), CMake and Ninja, and on Windows without git, MinGit.
- The Microsoft C/C++ runtime headers/libraries and the Windows SDK, fetched by
  [xwin](https://github.com/Jake-Shadle/xwin). These are under
  [Microsoft's license](https://go.microsoft.com/fwlink/?LinkId=2086102), which you
  are asked to accept. No Visual Studio installation is needed.

The install folder gets the game files (`game/`), the program, `userdata/` (settings,
saves, logs, and the shader cache) and `mods/`.

## Playing

- **Windows:** run `DAORecomp.exe` in the install folder. The launcher opens first
  (hold Shift while starting to show it when it is turned off).
- **Linux:** the program is a Windows executable; run it with Wine or Proton.
  - Steam: *Add a Non-Steam Game* → `DAORecomp.exe`, then in its properties force a
    Proton version (Proton includes vkd3d-proton for Direct3D 12).
  - Wine: install vkd3d-proton into the prefix (`winetricks vkd3d`), then run `./run.sh`.

  Linux support is untested so far. The program needs Wine's support for placeholder
  memory views (`VirtualAlloc2`/`MapViewOfFile3`) and Direct3D 12 through vkd3d-proton.
  FSR 4 needs AMD's Windows driver and is unlikely to work under Proton; choose FSR 3.1
  in the launcher.

Shaders are compiled the first time something is drawn, and that draw is skipped
until then, so new places can show objects popping in for a moment. Compiled shaders
are kept in `userdata/shader_cache/` and compiled again in the background at every
start, so this happens once per place.

## Developing

- `CMakeLists.txt` builds the runtime (`DAORecomp/`), Xenia's GPU/UI code
  (`xenia/src`, vendored and patched) and the recompiled game code (`ppc/`, generated).
- `cmake/clang-cl-xwin.cmake` is the toolchain the installer uses on both systems.
- `scripts/build_installer_exe.py` builds `dist/DAORecomp-Installer.exe` (PyInstaller; it
  bundles the files tracked by git). CI builds it too (the *Windows installer* workflow).
- `config/DAO.toml` and `config/DAO_switch_tables.toml` tell XenonRecomp where the game's
  functions and jump tables are; `scripts/patch_recompiler.py` adds the PowerPC
  instructions XenonRecomp lacked.
- `scripts/` also has the development tools used to analyse the game and build the
  shaders (`build_xenia_shaders.py` needs FXC from the Windows SDK and xenia-canary's
  FidelityFX-CAS/FSR submodules; the compiled shaders are committed).
- `scripts/check_include_case.py` finds `#include`s whose case differs from the file on
  disk, which only breaks builds on Linux.
- Diagnostics, set as environment variables (results in `userdata/xenia.log`):
  - `DAO_GPU_TIMING=1`: GPU time per section of a frame and pipeline statistics per
    kind of draw and per shader pair, every 300 frames (`GT` lines; slows rendering).
  - `DAO_DRED=1`: when the GPU device is lost, the operations it was running and the
    memory it faulted on (Direct3D 12 DRED).
  - `DAO_LAUNCHER_SCREENSHOTS=<folder>`: saves each launcher view as a BMP and quits.

Never commit game files or anything generated from them (`game/`, `ppc/`, `analysis/`,
built executables); `.gitignore` excludes them.

## License

MIT (see `LICENSE`), except `xenia/`, which is Xenia's code under its BSD license
(`xenia/LICENSE`). The downloaded dependencies keep their own licenses; the FidelityFX
license is copied next to its DLLs. *Dragon Age* is a trademark of Electronic Arts; this
project is not affiliated with EA, BioWare or Microsoft.
