# Unattended test runs

Scripts to run the game without a person at the controller and check the result
without watching it. They run from Git Bash (or any POSIX shell) on Windows, with
Python 3 and Pillow.

```sh
# Load the latest save and turn the camera for 100 s at 4K with FSR 4, motion vectors off:
scripts/test/run_game.sh out/fsr 100 scripts/test/inputs/camera.txt \
    video.resolution_scale=3 'video.temporal_upscaler="fsr4"' xenia.dao_motion_vectors=false
python scripts/test/contact_sheet.py out/fsr        # one image of the whole run

# Frame rate and CPU per thread, from 75 s on:
EXTRA="DAO_THREAD_STATS=2" CAPTURE=20 scripts/test/run_game.sh out/perf 110 scripts/test/inputs/load_latest.txt
python scripts/test/perf_summary.py out/perf/dao.log 75
```

`run_game.sh` does the bookkeeping every run needs:

- refuses to start while the game is running (someone may be playing);
- opens the window in the background (`DAO_BACKGROUND=1`), so it doesn't take focus;
- gives the game a test copy of `userdata/config.toml` (launcher hidden, intro logos
  skipped, plus the `section.key=value` overrides) and puts the user's back afterwards,
  checking it byte for byte, even when interrupted;
- records the saves in `userdata/content` (and backups in `userdata/save_backups`) and
  afterwards removes saves the run made
  (`KEEP_SAVES=1` keeps them) and warns if an existing one changed;
- collects `capture_N.png` (every `CAPTURE` s, default 2), `dao.log` and `xenia.log`
  into the output folder, and prints any abort or device-loss lines.

If a run leaves only the logs and no captures, the game didn't start: check `EXE` and
the command first.

## Input scripts

`DAO_INPUT_SCRIPT` takes a file or inline text; statements are separated by newlines or
`;`, and `#` starts a comment:

```
<time s> <BUTTONS> [hold s]                            e.g. 27 A 0.2
every <period s> <BUTTONS> [hold s] [from <s>] [until <s>]
```

Buttons, joined with `+` (`A+LB`): `A B X Y START BACK LB RB LT RT LS RS GUIDE`, the d-pad
`UP DOWN LEFT RIGHT`, and full stick deflections `LUP LDOWN LLEFT LRIGHT`,
`RUP RDOWN RLEFT RRIGHT`. The input is merged with any real controller.

`inputs/` holds DAO's: `load_latest.txt`, `camera.txt`, `walk.txt` (walks back and
forth while turning the camera), `options_menu.txt`, `save_game.txt` (its load steps
follow the developer's saves; adjust them) and `sandbox_overwrite_save.txt` (overwrites
a save: only in a sandbox). To make a
new one, run with captures every 2 s, read the times off the contact sheet, and keep
the result here.

## Sandbox runs

To test with someone else's saves and settings (a bug only their save shows), or with
input that changes saves, use a sandbox instead of the repo's `userdata`: a folder with a
`game` junction to the real game folder and a copy of their `userdata` (with
`skip_intro_logos = true` under `[patches]`, so the input scripts' times fit).

```sh
cmd //c mklink /J "C:\Sandbox\game" "C:\path\to\DAO\game"   # then copy userdata in
DAO_SANDBOX=C:/Sandbox scripts/test/run_sandbox.sh out/walk 100 scripts/test/inputs/walk.txt
```

`run_sandbox.sh` refuses to start while the game is running and opens the window in the
background, like `run_game.sh`, but doesn't restore settings or saves: the copy is there
to be changed. Remove a sandbox's `game` junction with `rmdir`, never `rm -r`, which
deletes the real game files through it.

## Flicker and motion

`flicker.py <dir>...` scores flicker in consecutive frames: a frame history
(`DAO_FRAME_HISTORY=1`, F12 saves the last 180 frames) or captures taken every frame.
Compare the ratio against the same walk with the effect off (`walk.txt` gives the same
walk each time); it also lists the frames and blocks most like a one-frame flash.

## Launcher and installer

- `fake_steam.py <folder>`: a fake Steam for *Add to Steam* (with `DAO_STEAM_ROOT`, below);
  `--dump` shows what the launcher wrote.
- `fake_install_engine.py`: stands in for the installer
  (`DAO_INSTALL_ENGINE=<its absolute path>`), to test the install screen in seconds.
- `installer_cancel.py`: Cancel must end a silent build step and what it started at once,
  also when the step has exited and a child of it still holds its output.
- `installer_play.py <install folder>`: the installer's Play button after an install.
- `capture_window.ps1 <png>`: the launcher window's contents, even behind other windows;
  `watch_windows.ps1`: logs the game window's size, position and DPI on every change.

## Content

- `convert_regress.py <work dir> <PC mod>=<earlier conversion>...`: converts copies of PC
  mods with the current build and lists the files that changed since an earlier
  conversion (the work folder must be away from the inputs).
- `dlc_install.py <packages> <userdata/content>`: installs DLC packages as the launcher
  does.

## Other tools

- `untile.py`: raw Xbox 360 tiled 32bpp texture data (a save's `scrnshot.bin`, a memory
  dump) to PNG, to see what the CPU got from the GPU.
- `set_config.py`, `saves.py`: the config and save guards `run_game.sh` uses.
- `DAO_STEAM_ROOT=<folder>`: the launcher's *Add to Steam* uses that folder as Steam's
  (with `userdata/<account>/config/` inside), to test it without touching the real Steam.
