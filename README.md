# Dragon Age: Origins — Recompiled

A static recompilation of the Xbox 360 version of *Dragon Age: Origins* into a native
x86-64 Windows program, with a Direct3D 12 renderer based on Xenia's. It adds:

- rendering at up to 4K (and higher), with FSR 1 / CAS scaling of the whole image
- temporal upscaling and anti-aliasing with AMD FSR 4 (RDNA 4) or FSR 3.1, using
  motion vectors generated for the game's 3D scenes
- HDR output: on an HDR display the 3D scenes are rendered in HDR (the game's
  tonemapping redone without compressing its highlights), upscaled by FSR in HDR
- 60 fps (the original is locked to 30)
- SDL3 input, including DualSense and other controllers
- a launcher for settings, mods (file overrides, with a converter for PC mods) and
  code patches
- a Graphics tab in the game's own Options menu (title and pause menus)
- an on-screen keyboard for naming characters, usable with a gamepad
- the downloadable content (Awakening and the rest), installed from your own packages

**This repository contains no game code or data.** You need your own copy of the
game: an ISO image of the Xbox 360 disc. The installer extracts it, translates the
game's code to C++ on your computer, and builds the program.

## Installing

You need:

- An x86-64 PC running Windows 10/11 or Linux, with a Direct3D 12 capable GPU and a
  processor with AVX (most from 2011 or later). The game is built for your processor:
  with AVX2 (Intel from 2013, AMD from 2015) it uses those newer instructions. A copy
  built that way and moved to a PC without them says so when it starts; run the
  installer on that PC to build it for its processor.
- The disc image of *Dragon Age: Origins* for Xbox 360 (title ID 454108C0). The
  installer checks that it is the supported release.
- About 25 GB of disk space: 8 GB for the game, the rest for the tools and the build.
  The first build takes 30–60 minutes, depending on the CPU.

### Windows

Download **DAORecomp.exe** from the releases and run it: it opens on its Install page,
drawn like the launcher (with the title screen from your disc once you pick it). Pick the
ISO and an install folder, accept the build tools license, and press **Install**.
Nothing else needs to be installed first: it downloads what it needs (including a
portable git if you have none). It builds the game into the install folder as a new
`DAORecomp.exe`, which then opens as the launcher: **Play** starts the game. It can also
add Start menu and desktop shortcuts, and add the game to Steam.

The disc image and the folder can be given to it (or the ISO dropped on it):

```
DAORecomp.exe --iso "D:\Games\Dragon Age Origins.iso" --dest "D:\Games\DAORecomp"
```

The tools and the build go into `%LOCALAPPDATA%\DAORecomp-build` (set `DAO_WORK_DIR` to
use another folder). Keep it and a newer installer only rebuilds what changed; delete it
to get the space back.

Windows may warn about `DAORecomp.exe` because it isn't code-signed (*More info* →
*Run anyway*).

#### Title Update 6 and downloadable content

*Awakening*, *Leliana's Song*, *The Darkspawn Chronicles*, *The Golems of Amgarrak* and
*Witch Hunt* need the game's Title Update 6 (the other DLC work without it). To use them,
copy the title update's package and the DLC packages from a console (or your backup of
them) and:

- give the installer the title update's package: *Title Update 6 (optional)* in the
  window, or `--title-update <package>`. It is checked (it must be the one for the
  English disc, media ID 186A1115), extracted to `title_update/` in the install folder,
  and the game's code is translated a second time from the updated executable (about as
  long again). The program then holds both versions and runs the updated one while
  `title_update/` is there.
- drop the DLC packages, or the folder holding them, on the launcher window. They are
  extracted to `userdata/content/454108C0/00000002/`, where the game finds them. A title
  update dropped there is installed too; if the program was built without it, it takes
  effect after running the installer again on the same folder.

Only packages in the read-only format Xbox Live delivers them in (LIVE/PIRS) are read;
the packages themselves are left as they are. At start, `title_update/default.xexp` is
checked to be Title Update 6's; anything else is passed over for the disc's version.

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
**Update** item with the release notes. It downloads the new release's `DAORecomp.exe`
into `update/` in the install folder, checks it against the SHA-256 GitHub lists for it,
and runs it on the same folder; it rebuilds only what changed (the disc image isn't
needed again) and keeps your saves, settings and mods. The checksum catches a damaged
or altered download; like the release itself, it comes from this GitHub repository.
While the version is 0.x, pre-releases are offered too.

The check asks `api.github.com/repos/96dot/DAORecomp/releases` for the list of releases;
the request carries only the program's version. Turn it off in *Configure → Advanced*
or with `check_updates = false` in the `[launcher]` section of `userdata/config.toml`.
Builds that aren't releases (`dev`, or a changed checkout of a release) don't check.
With the launcher hidden there is no check: open it now and then (hold Shift).

To update by hand, run a newer release's `DAORecomp.exe` and choose the same install
folder (or give it `--dest`). Under Wine or Proton the launcher only links to the
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
saves, save backups, installed DLC, logs, the shader cache, and the Options menu it
generates), `mods/`, and with the title update, `title_update/`.

When you save over a save, the version it replaces is kept in
`userdata/save_backups/`: one per save, the one before its latest overwrite (a save
the game crashed while writing doesn't replace it, and none is kept when the drive is
nearly full). To get one back, quit the game and copy its folder and `.name` file into
`userdata/content/454108C0/00000001/`, replacing the ones there. (A `<save>.new`
folder there is a backup that couldn't be put in place; the log says why.)

## Playing

- **Windows:** run `DAORecomp.exe` in the install folder. The launcher opens first
  (hold Shift while starting to show it when it is turned off). It is drawn with the
  title screen and menu art from your copy of the game (`guiexport.erf`, read at
  startup; none of it is shipped), falling back to plain shapes without it, and uses
  Windows' serif fonts (Palatino Linotype, Georgia or Times New Roman). It works with
  a mouse, the keyboard or a gamepad (Start plays). `--launcher` shows it anyway,
  `--mods` opens it on its Mods page, and `--play` skips it.

  *Configure → Advanced → Add to Steam* adds the game to Steam as a non-Steam game, for
  each Steam account on the PC, with library artwork drawn from the title screen. Steam
  starts it with `--play`, straight into the game (for Big Picture). Steam has to be
  closed for that: the launcher asks first, and starts it again after. If Steam gets
  stuck while closing, the launcher offers to end it after 15 seconds. If Windows still
  can't let go of it after that, Steam won't start again until the PC is restarted. The old
  `shortcuts.vdf` is kept beside it as `shortcuts.vdf.daorecomp-backup`.
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
- `scripts/build_setup_exe.py` builds `dist/DAORecomp.exe`, what players download: the
  launcher alone (`-DDAO_LAUNCHER_ONLY=ON`) with the installer appended to it, which it
  unpacks to run. The installer is `scripts/build_installer_exe.py`'s PyInstaller exe
  (it bundles the files tracked by git). CI builds both (the *Windows installer*
  workflow) and publishes `DAORecomp.exe` also as `DAORecomp-Installer.exe`, the name
  launchers up to v0.5.1 update with.
- `config/DAO.toml` and `config/DAO_switch_tables.toml` tell XenonRecomp where the game's
  functions and jump tables are; `scripts/patch_recompiler.py` adds the PowerPC
  instructions XenonRecomp lacked.
- Title Update 6 is a second build of the game in the same program:
  `config/DAO_tu6.toml` and `config/DAO_tu6_switch_tables.toml` describe it,
  `scripts/recomp.cmd tu6` translates it into `ppc_tu6/` (built when that folder exists),
  and `scripts/prefix_symbols.py` gives its functions names of their own (`tu6_sub_...`).
  The runtime picks the build at start (`DAORecomp/kernel/game_build.h`); code hooks are
  given both builds' functions with `GAME_HOOK`, and addresses both builds' with
  `GameAddress`. `scripts/match_build.py` finds a disc function's counterpart in the
  updated executable, and the analysis scripts take `DAO_VARIANT=tu6`.
- `scripts/stfs.py` lists and extracts Xbox 360 packages (`info`/`extract`); the
  installer uses it. `DAORecomp/content/stfs.cpp` is the same reader for the launcher
  (which has no Python): keep the two in step. `scripts/test_stfs.py` tests both on
  packages it makes (pass `build/DAORecomp/DAORecomp.exe` for the C++ one; CI runs the
  Python one), and `scripts/check_constants.py` checks that the IDs and the title
  update's hash in `installer/deps.py` match the program's.
- `scripts/` also has the development tools used to analyse the game and build the
  shaders (`build_xenia_shaders.py` needs FXC from the Windows SDK and xenia-canary's
  FidelityFX-CAS/FSR submodules; the compiled shaders are committed).
- `scripts/make_icon.py` draws the program's icon (`DAORecomp/res/daorecomp.ico`,
  original art); it needs Pillow, and the .ico is committed, so it's only run to
  change the icon.
- The in-game Graphics tab is ActionScript 2 (`DAORecomp/ui/graphics_tab.as`), which
  `scripts/as2c.py` compiles to `DAORecomp/ui/graphics_tab_as.h`; run
  `python scripts/as2c.py DAORecomp/ui/graphics_tab.as DAORecomp/ui/graphics_tab_as.h kGraphicsTabScript`
  after changing it (with `--check` at the end it only checks the header, as CI does).
  At start the runtime adds the script to the player's own `optionsmenu.gfx` and
  serves the result from `userdata/generated/`.
- `scripts/check_include_case.py` finds `#include`s whose case differs from the file on
  disk, which only breaks builds on Linux.
- `DAO_CONSOLE=1` shows the log (`userdata/dao.log`) as it's written, in the console
  the game was started from or a new one. The game has no console of its own, but
  output redirected to a file or a pipe gets the log without this. Closing a console it
  opened ends the game.
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
    as if dropped on the window (tests adding mods and installing packages).
  - `DAO_BUILD=disc`: runs the disc's build even when Title Update 6 is installed.
  - `DAO_EXTRACT_PACKAGE=<package>|<new folder>`: extracts an Xbox 360 package as the
    launcher does, prints the outcome and quits (used by `scripts/test_stfs.py`).
  - `DAO_CONVERT_MOD=<mod folder>`: converts that PC mod as the launcher's Convert does,
    logs the outcome to `userdata/dao.log` and quits (tests the converter).
  - `DAO_TEST_KEYBOARD=<seconds>`: shows the on-screen keyboard that long after the
    game starts and logs what was entered (tests it without creating a character).

Never commit game files or anything generated from them (`game/`, `ppc/`, `ppc_tu6/`,
`analysis/`, `title_update/`, `dlc/`, built executables); `.gitignore` excludes them.

## License

MIT (see `LICENSE`), except `xenia/`, which is Xenia's code under its BSD license
(`xenia/LICENSE`). The downloaded dependencies keep their own licenses; the FidelityFX
license is copied next to its DLLs. *Dragon Age* is a trademark of Electronic Arts; this
project is not affiliated with EA, BioWare or Microsoft.
