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

`inputs/` holds DAO's: `load_latest.txt`, `camera.txt`, `options_menu.txt` and
`save_game.txt` (its load steps follow the developer's saves; adjust them). To make a
new one, run with captures every 2 s, read the times off the contact sheet, and keep
the result here.

## Other tools

- `untile.py`: raw Xbox 360 tiled 32bpp texture data (a save's `scrnshot.bin`, a memory
  dump) to PNG, to see what the CPU got from the GPU.
- `set_config.py`, `saves.py`: the config and save guards `run_game.sh` uses.
- `DAO_STEAM_ROOT=<folder>`: the launcher's *Add to Steam* uses that folder as Steam's
  (with `userdata/<account>/config/` inside), to test it without touching the real Steam.
