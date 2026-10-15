# Contributing

Thanks for helping. This page is the map: how the code is laid out, how changes are
tested, and the traps found so far. It links to the details rather than repeating
them: the README's [Developing](README.md#developing) section (the build, the
launcher's and the renderer's diagnostic switches) and
[scripts/test/README.md](scripts/test/README.md) (the test kit and its switches).

You need your own copy of the game to work on anything that runs it. The repository
has no game code or data, and must never get any.

## Words used here

- **Guest:** the game's own code and memory, as the Xbox 360 ran them. **Host:** the
  PC program around them.
- **XenonRecomp:** the tool that translates the game's PowerPC (PPC) machine code into
  C++, function by function (into `ppc/`, generated on your PC by the installer).
- **Xenia:** the Xbox 360 emulator whose Direct3D 12 renderer draws the game's GPU
  commands here. Its code is vendored in `xenia/` and patched.
- **Hook:** our C++ put in place of one of the game's translated functions.
- **Title update (TU):** the game's patches 6 and 7, a second build of its code that
  the program also contains.

## Ground rules

1. **No game data, ever.** Nothing from the disc, title updates or DLC, and nothing
   generated from them: `game/`, `ppc/`, `ppc_tu6/`, `analysis/`, `userdata/`, `dlc/`,
   `update/`, `title_update/`, extracted Flash movies (`.gfx`), texture or model dumps,
   built executables. `.gitignore` keeps out the usual folders; check `git status`
   before every commit anyway. Screenshots in an issue or a pull request are fine.
2. **Frame rate is never traded away.** A change that costs displayed frames, or adds
   stutter, is a regression whatever it buys (lower latency, less CPU, neater code).
   Anything touching presentation, vsync, pacing, sleeps, fences or the GPU thread is
   frame-rate-critical: measure it before and after (see [Testing](#testing)).
   Low-power PCs such as the Steam Deck come first.
3. **Third-party binaries come from the installer, not git.** The installer downloads
   pinned versions (SHA-256 checked, `installer/deps.py`): LLVM, FidelityFX, XeSS,
   Streamline and NVIDIA's DLLs. Never commit or bundle them. The only third-party code
   in git is source under `thirdparty/` (lmxxf's neural rendering, the Anti-Lag 2
   header), each with its license and where it came from. Never add NVIDIA's DLSS 5
   neural-rendering weights in any form: players point the launcher at their own copy.
4. **Untested features are marked experimental**, in the launcher, the game's menus
   and the README (as DLSS and Reflex are, built without an NVIDIA GPU to test on).

## What you need

- The game's disc image: the English, Italian and Spanish edition (media ID 186A1115,
  the disc's ID that the installer checks; it's also disc 1 of that edition's Ultimate
  Edition). For the DLC and Title Update 6 or 7, their packages.
- A checkout, built once by the installer: see the README's
  [Linux (or from the source on Windows)](README.md#linux-or-from-the-source-on-windows)
  for what to install first and the command. It fetches the tools into `tools/` (CMake,
  Ninja, LLVM, and the Windows SDK and C++ runtime through xwin, so no Visual Studio is
  needed), translates the game from your ISO and builds it in `build/`. After a change,
  build again with its Ninja (`tools\ninja\ninja.exe -C build DAORecomp` on Windows,
  `tools/ninja/ninja -C build DAORecomp` on Linux), or run the installer again: it
  rebuilds only what changed. On Linux the program runs under Wine or Proton.
- A GPU with Direct3D 12. Most work so far was tested on an AMD RX 9070 XT; reports
  from other GPUs (NVIDIA, Intel, Steam Deck) are especially useful.

## Map of the code

| Where | What |
|---|---|
| `DAORecomp/main.cpp` | Start-up: settings, the launcher or the game, the guest's main thread. |
| `DAORecomp/kernel/` | The Xbox 360 system functions the game calls (`xam.cpp`, `xboxkrnl_*.cpp`), memory, threads, files (`vfs.cpp`), which build runs (`game_build.h`), code hooks. |
| `DAORecomp/gpu/` | The bridge to Xenia's renderer (`xenia_host.cpp`), the frame rate (`video.cpp`), the camera's field of view, frame captures. |
| `DAORecomp/apu/` | Audio: the 360's XMA audio format and the game's audio thread. |
| `DAORecomp/input/` | SDL3 controllers and keyboard, and `DAO_INPUT_SCRIPT` for scripted tests. |
| `DAORecomp/ui/` | What's drawn over or into the game: the Graphics tab and START menu rows (ActionScript in `.as` files), photo mode, cheats, the on-screen keyboard, the shared overlay. |
| `DAORecomp/launcher/` | The launcher (Dear ImGui on SDL3): Configure, Mods, Save Backups, DLC, Install, Update. |
| `DAORecomp/content/` | Xbox 360 packages (the STFS format), installing DLC and title updates, saves and their backups. |
| `DAORecomp/mods/` | Mods, and the PC-to-360 mod converter (`convert/`). The `mods/` folder at the top is the player's, not code. |
| `DAORecomp/patches/` | Optional code patches the launcher lists. |
| `DAORecomp/settings.*` | `userdata/config.toml`: every setting, read and written in one place. |
| `xenia/src/xenia/` | Xenia's renderer. Our additions are the `dao_*` files: upscaling, motion vectors, HDR, frame generation, neural rendering and the frame rate guard (`gpu/d3d12/`), low latency and NVIDIA Streamline (`ui/d3d12/`), frame pacing (`ui/dao_frame_pacer.cc`), and their shaders. Changes inside Xenia's own files are kept small and marked `DAO`. |
| `thirdparty/` | lmxxf's neural rendering source and the Anti-Lag 2 header (in git); SDL (fetched by the installer). |
| `installer/` | The installer (Python): checks the ISO, downloads the tools, translates the game and builds it. |
| `config/` | XenonRecomp's description of the game's code: functions, jump tables (`*_switch_tables.toml`), exception records. |
| `scripts/` | Analysis, translation and build tools; `scripts/test/` is the test kit. |
| `docs/` | `x360-recomp-notes.md` (what this port taught), `porting-playbook.md` (how to do the next game). |

## How our code meets the game's

The game's translated code (`ppc/`) runs as it did on the console. Our code changes
what it does in three ways:

- **System calls.** The game calls the console's system functions;
  `DAO_IMPORT_AS(name, function)` (`kernel/function.h`) puts our implementation in
  place of one. The others are stubs (`kernel/import_stubs.cpp`, generated by
  `scripts/gen_stubs.py`) that report the call and return 0.
- **Hooks.** `GAME_HOOK(disc, tu6, hook)` (`kernel/game_build.h`) replaces a translated
  function in both builds of the game, the disc's and the title update's, with one body
  that can call the original. Any guest address, of a function or of data, that moved
  between the builds is a `GameAddress{ disc, tu6 }`. Each address has a comment saying
  what's there; say how you found it, too, when it wasn't obvious.
  `scripts/match_build.py` finds a disc function's counterpart in the title update (it's
  very slow on data addresses: find those through a neighbouring small function).
- **Renderer.** The game draws through the console's GPU, which Xenia's renderer
  emulates; our upscaling and effects come in at its resolves and frame swaps
  (`DaoUpscalerImpl::OnSwap` in `xenia/src/xenia/gpu/d3d12/dao_upscale.cc` is the place
  to start).

### Adding a setting

A setting touches several places; follow an existing one of the same kind (a list of
choices like `lowLatency`, a number like `fov`):

1. `DAORecomp/settings.h`: the field, with a comment; for a list, its enum, `kMax...` and
   name function.
2. `DAORecomp/settings.cpp`: reading it (`TryReadSettings`); writing it (`SaveSettings`
   writes the whole file from one text with a `{}` for each value: add the line and
   its comment there, and the value at the same position in the arguments after it);
   `ClampSettings` for numbers; the start-up log (`LoadSettings`).
3. The launcher (`DAORecomp/launcher/configure_view.cpp`): a row, with its list of names
   tied to the enum by a `static_assert`.
4. If the game's Graphics tab shows it: `DAORecomp/ui/graphics_menu.cpp` (its key, whether
   it waits for a restart, `ApplyLive`) and `DAORecomp/ui/graphics_tab.as`, then
   `python scripts/as2c.py DAORecomp/ui/graphics_tab.as DAORecomp/ui/graphics_tab_as.h kGraphicsTabScript`.
5. If the renderer reads it every frame: a field of `xe::dao_live::Values` in
   `xenia/src/xenia/dao_live_settings.h` (the threads that run each frame never read
   settings directly).
6. The README.

`scripts/check_constants.py` (run by CI) compares values that `settings.h`, the
Graphics tab script and the installer share: run it after changing one.

## Testing

### Checks (no game needed; CI runs them)

```
python -I scripts/check_constants.py     # values shared by the installer and the program
python -I scripts/check_include_case.py  # include case (breaks only the Linux build)
python -I scripts/test_stfs.py           # the package readers
python scripts/as2c.py DAORecomp/ui/graphics_tab.as DAORecomp/ui/graphics_tab_as.h kGraphicsTabScript --check
python scripts/as2c.py DAORecomp/ui/pause_menu.as DAORecomp/ui/pause_menu_as.h kPauseMenuScript --check
```

`check_include_case.py` checks against the Windows SDK in `tools/winsysroot`, which the
installer fetches. `scripts/test/release_list_test.cpp` tests the launcher's choice of
update (its build command is in `scripts/test/README.md`).

### Running the game unattended

Most changes can only be judged by running the game. `scripts/test/` has the kit; its
[README](scripts/test/README.md) has the details and the examples.

- `run_game.sh` runs your build with scripted controller input and captures frames every
  few seconds, then collects the logs. It puts your settings back, and removes the saves
  the run made (it warns if an existing one changed).
- `run_sandbox.sh` runs against a **sandbox** instead: a folder with a junction to the
  game files and a copy of a `userdata`, which it doesn't restore (the copy is there to
  be changed). Use one for anything that saves, or for someone else's save. Remove its
  junction with `rmdir`, never `rm -r` (that deletes the real game files through it).
- `contact_sheet.py` turns a run's captures into one image. For the frame rate, run with
  `EXTRA="DAO_THREAD_STATS=2"` and read it with `perf_summary.py`; the `VdSwap #`
  lines in `dao.log` count the game's frames, a quick check that it kept running.
  `flicker.py` scores flicker in a frame history (`DAO_FRAME_HISTORY=1`).
- Runs open their window in the background (`DAO_BACKGROUND=1`), so you can keep using
  the PC; don't start one while the game is open.

What to run depends on the change:

- **Renderer, presentation or pacing:** measure before and after, at 60 fps with vsync on
  and with the frame rate unlimited (the override `video.frame_rate=0`; at 60 fps the
  GPU's clocks drop with the load, which hides costs). Go through the temporal upscalers
  you can (Off, FSR 4, FSR 3.1, XeSS), HDR on and off, FSR Frame Generation, and photo
  mode.
- **Hooks:** both builds, with and without a title update (`DAO_BUILD=disc` runs the
  disc's).
- **Launcher:** `DAO_LAUNCHER_SCREENSHOTS` saves each view as an image; with it,
  `DAO_LAUNCHER_PAD` drives the launcher with a virtual controller (README,
  *Developing*).

## Traps found so far

- **Text files:** Python on Windows reads and writes in the ANSI code page, and
  `Path.write_text` writes CRLF. Open files as UTF-8 and keep LF line endings (a failed
  write once emptied the README).
- **Include case:** Windows ignores it and the Linux build fails; the Windows SDK's
  headers are all lowercase (`shlobj.h`).
- **The player's build uses only committed files:** the installer bundles the files git
  tracks, so a new source file must be committed before `scripts/build_setup_exe.py`.
- **Shaders:** `scripts/build_xenia_shaders.py` rebuilds the committed bytecode headers.
  Two of Xenia's own FXAA shaders fail to build with it (their `FAILED` lines are
  expected). Restore any header that changed only in its line endings
  (`git checkout -- <file>`).
- **Stream output (motion vectors):** several draws streamed out under one binding hung
  AMD GPUs when another program's GPU work preempted between them. Fixed in 3b22ace:
  each draw writes its own part of the buffer, bound for that draw only. Unbinding with
  a null view crashes vkd3d-proton (Wine, Proton): unbind with a zero-filled view
  (PR #1).
- **Presentation:** never skip showing a frame to cut latency. Skipping one on a 60 Hz
  display dropped the shown frame rate below 60. A frame that's drawn but not shown
  must give back its place in the swap chain's queue, or every later frame waits
  100 ms.
- **Guest busy-waits:** the game spins on flags in places, and the power-saving
  `Sleep(0)` heuristic can stall one. What found it: a temporary hotkey that writes a
  marker to the log when the problem shows, and per-second counts of calls to the
  hooked guest functions around it.
- **Low latency:** AMD Anti-Lag 2 cost frames with our frame generation's pacing, so low
  latency is forced off while FSR Frame Generation is on (the setting is kept). With
  NVIDIA Streamline on, the direct queue is created through its proxy, but the game
  keeps the native queue: the proxy's `GetDevice` returns Streamline's device, which
  broke neural rendering.
- **Captures:** text like `…1.50X SOURCE: LOCAL` along the top edge is the AMD driver's
  FSR 4 overlay, not the game's. The Fade's heavy blur is the level's own effect.
- **Saves in tests:** runs that load make autosaves, and some saves load into a
  conversation, which breaks an input script's timing. Use a sandbox, and set its saves
  aside and back afterwards.
- **Shell scripts:** in sh, `${X:+env A=1} B=1 cmd` with `X` empty runs `B=1` as the
  command, silently, so a test script built that way never starts the game. If a run
  leaves only logs and no captures, the game never started.

## Sending a change

- One change per pull request, with what you tested it on (GPU, the build of the game,
  the settings) and how (which input script, before/after numbers for anything that
  could cost frames).
- Commit subjects name the area, then what changed for the player:
  `Launcher: a dev update channel, for dev releases between releases`. The body says
  why, and what was measured.
- Comments explain why, not what; follow the style of the file you're in.

## Releases (maintainers)

- Before a release, big changes get three reviews: logic (including anything that can
  cost frames), readability (including whether the docs still match the code) and
  security (files, downloads, the network, memory patching).
- A tag publishes a release: the *Windows installer* workflow builds `DAORecomp.exe` and
  publishes it (also as `DAORecomp-Installer.exe`, for launchers before v0.5.2).
- `vX.Y.Z` is a release; `vX.Y.Z-dev.N` is a dev release, offered only to launchers on the
  Dev update channel. All 0.x releases are marked pre-release on GitHub; the tag decides
  the channel.
- The workflow publishes GitHub's generated notes: replace them
  (`gh release edit <tag> --notes-file <file>`) with what's new for the player, how to
  install or update, and credit for contributions.

## Learning more

- [README](README.md), *Developing*: the build, the toolchain, the diagnostic switches.
- [docs/x360-recomp-notes.md](docs/x360-recomp-notes.md): how the recompilation was
  made, and what broke on the way.
- [docs/porting-playbook.md](docs/porting-playbook.md): each hook and how it was found.
- [scripts/test/README.md](scripts/test/README.md): the test kit.
