# Dragon Age: Origins — Recompiled

A static recompilation of the Xbox 360 version of *Dragon Age: Origins* into a native
x86-64 Windows program, with a Direct3D 12 renderer based on Xenia's. It adds:

- rendering at up to 4K (and higher), with FSR 1 / CAS scaling of the whole image
- temporal upscaling and anti-aliasing with AMD FSR 4 (RDNA 4) or FSR 3.1, using
  motion vectors generated for the game's 3D scenes
- 60 fps (the original is locked to 30)
- SDL3 input, including DualSense and other controllers
- a launcher for settings, mods (file overrides, with a converter for PC mods) and
  code patches
- a Graphics tab in the game's own Options menu (title and pause menus)
- an on-screen keyboard for naming characters, usable with a gamepad

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
installer downloads what it needs (including a portable git if you have none). When it
is done, **Play** starts the game; it can also add Start menu and desktop shortcuts.

From a terminal instead:

```
DAORecomp-Installer.exe --iso "D:\Games\Dragon Age Origins.iso" --dest "D:\Games\DAORecomp" --shortcuts
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

### Updating

The launcher looks for a newer release when it opens and, if there is one, shows an
**Update** item with the release notes. It downloads the new installer into `update/`
in the install folder, checks it against the SHA-256 GitHub lists for it, and runs it
on the same folder; the installer rebuilds only what changed (the disc image isn't
needed again) and keeps your saves, settings and mods. The checksum catches a damaged
or altered download; like the release itself, it comes from this GitHub repository.
While the version is 0.x, pre-releases are offered too.

The check asks `api.github.com/repos/96dot/DAORecomp/releases` for the list of releases;
the request carries only the program's version. Turn it off in *Configure → Advanced*
or with `check_updates = false` in the `[launcher]` section of `userdata/config.toml`.
Builds that aren't releases (`dev`, or a changed checkout of a release) don't check.
With the launcher hidden there is no check: open it now and then (hold Shift).

To update by hand, run a newer installer and choose the same install folder (or
`--dest` alone from a terminal). Under Wine or Proton the launcher only links to the
release page: update the checkout (`git pull`, or check out the new tag) and run
`python3 install.py --dest <install folder>`.

### What it downloads

Everything is pinned to exact versions and checked (`installer/deps.py`):

- Source code: Xenia's third-party libraries (and rapidjson, for the update check),
  XenonRecomp, SDL 3, the AMD FidelityFX SDK (headers and the signed FSR DLLs).
- LLVM/Clang 22 (or your system's LLVM 18+ if it has `clang-cl`, `lld-link`, `llvm-lib`
  and `llvm-rc`), CMake and Ninja, and on Windows without git, MinGit.
- The Microsoft C/C++ runtime headers/libraries and the Windows SDK, fetched by
  [xwin](https://github.com/Jake-Shadle/xwin). These are under
  [Microsoft's license](https://go.microsoft.com/fwlink/?LinkId=2086102), which you
  are asked to accept. No Visual Studio installation is needed.

The install folder gets the game files (`game/`), the program, `userdata/` (settings,
saves, logs, the shader cache, and the Options menu it generates) and `mods/`.

## Playing

- **Windows:** run `DAORecomp.exe` in the install folder. The launcher opens first
  (hold Shift while starting to show it when it is turned off). It is drawn with the
  title screen and menu art from your copy of the game (`guiexport.erf`, read at
  startup; none of it is shipped), falling back to plain shapes without it, and uses
  Windows' serif fonts (Palatino Linotype, Georgia or Times New Roman). It works with
  a mouse, the keyboard or a gamepad (Start plays). `--launcher` shows it anyway,
  `--mods` opens it on its Mods page, and `--play` skips it.
- **Linux:** the program is a Windows executable; run it with Wine or Proton.
  - Steam: *Add a Non-Steam Game* → `DAORecomp.exe`, then in its properties force a
    Proton version (Proton includes vkd3d-proton for Direct3D 12).
  - Wine: install vkd3d-proton into the prefix (`winetricks vkd3d`), then run `./run.sh`.

  Linux support is untested so far. The program needs Wine's support for placeholder
  memory views (`VirtualAlloc2`/`MapViewOfFile3`) and Direct3D 12 through vkd3d-proton.
  FSR 4 needs AMD's Windows driver and is unlikely to work under Proton; choose FSR 3.1
  in the launcher.

Most graphics settings can also be changed in the game: *Options → Graphics*. Most
apply at once; when you leave the menu, the game lists the ones that take effect
the next time it starts. Turning off dynamic shadows and post-processing there (or in the
launcher) makes the game faster on low-power devices such as the Steam Deck.

Mods go in `mods/` (see `mods/README.txt`); the easiest way to add one is to drop its
archive, folder or files on the launcher window (archives are opened with Windows'
`tar.exe`, in Windows 10 1803 and later). PC mods with models, textures or archives
are marked as such and not loaded until converted (select one and press *Convert*); the
launcher also warns of what it finds in a mod that may not work on this version. The
first time the Mods page opens it explains this (`mods_warning_seen` in the
`[launcher]` section of `userdata/config.toml` records that it was read). In the game,
*Options → Graphics → Manage mods* closes it and opens the launcher's Mods page.

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
- The in-game Graphics tab is ActionScript 2 (`DAORecomp/ui/graphics_tab.as`), which
  `scripts/as2c.py` compiles to `DAORecomp/ui/graphics_tab_as.h`; run
  `python scripts/as2c.py DAORecomp/ui/graphics_tab.as DAORecomp/ui/graphics_tab_as.h kGraphicsTabScript`
  after changing it (with `--check` at the end it only checks the header, as CI does).
  At start the runtime adds the script to the player's own `optionsmenu.gfx` and
  serves the result from `userdata/generated/`.
- `scripts/check_include_case.py` finds `#include`s whose case differs from the file on
  disk, which only breaks builds on Linux.
- Diagnostics, set as environment variables (results in `userdata/xenia.log`):
  - `DAO_GPU_TIMING=1`: GPU time per section of a frame and pipeline statistics per
    kind of draw and per shader pair, every 300 frames (`GT` lines; slows rendering).
  - `DAO_DRED=1`: when the GPU device is lost, the operations it was running and the
    memory it faulted on (Direct3D 12 DRED).
  - `DAO_LAUNCHER_SCREENSHOTS=<folder>`: saves each launcher view (the title screen,
    the four Configure tabs, Mods: its list, even before the first-open notice was
    read) as `launcher_0.bmp` to `launcher_5.bmp`, and the Update page as
    `launcher_6.bmp` when the update check finds a release, then quits.
  - `DAO_LAUNCHER_DROP=<path>|<path>...`: with `DAO_LAUNCHER_SCREENSHOTS`, adds those
    as mods as if dropped on the window (tests adding mods).
  - `DAO_CONVERT_MOD=<mod folder>`: converts that PC mod as the launcher's Convert does,
    logs the outcome to `userdata/dao.log` and quits (tests the converter).
  - `DAO_TEST_KEYBOARD=<seconds>`: shows the on-screen keyboard that long after the
    game starts and logs what was entered (tests it without creating a character).

Never commit game files or anything generated from them (`game/`, `ppc/`, `analysis/`,
built executables); `.gitignore` excludes them.

## License

MIT (see `LICENSE`), except `xenia/`, which is Xenia's code under its BSD license
(`xenia/LICENSE`). The downloaded dependencies keep their own licenses; the FidelityFX
license is copied next to its DLLs. *Dragon Age* is a trademark of Electronic Arts; this
project is not affiliated with EA, BioWare or Microsoft.
