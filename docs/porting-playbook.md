# Porting playbook (for Dragon Age II and later games)

A step-by-step plan for the next Xbox 360 recompilation, written from DAO's. It is the
*how*: what to reuse, what order to work in, how each DAO-specific piece was found, and
how to test without a person at the controller. The *why* and the gotchas are in
[x360-recomp-notes.md](x360-recomp-notes.md); read that first, this doc does not repeat
it.

Dragon Age II runs on an evolution of DAO's Eclipse engine, so many of DAO's hooks
should have counterparts. Its addresses will all differ: find each one again with the
method given here, never by copying an address.

## Working rules (they keep sessions short)

- **One session per phase** below. End each by updating a short status file in the new
  repo (`docs/status.md`: what works, open problems, key addresses) and start the next
  session from it, not from a long conversation.
- **Scripts summarise, they don't dump.** Any analysis or test script prints a few
  lines (counts, the first mismatch, pass/fail); full output goes to a file that is read
  only if needed. Logs go to `userdata/*.log`, never to the terminal.
- **Test harnesses live in the repo** (`scripts/test/`), not in a session scratchpad,
  so they are reused instead of rewritten. DAO's are there to copy.
- **Reviews (logic, readability, security) after big changes only**, not after every
  fix. Small follow-ups get one combined review.
- **Mechanical work** (stubs, porting a Python prototype to C++, compile-fixing) can go
  to a cheaper subagent with `scripts/check_tu.py` to compile-check one file.

## Phases, with what "done" means

| # | Phase | Done when | Reuse from DAO |
|---|-------|-----------|----------------|
| 1 | Binary | XEX decrypted, hash checked, image zero-filled to its size | `scripts/xiso.py`, loader in `kernel/module.cpp` |
| 2 | Recompile | XenonRecomp output builds with zero warnings | `patch_recompiler.py`, `recomp.cmd`, `find_switches.py`, `fix_functions.py`, `find_helpers.py`, `gen_stubs.py` |
| 3 | Kernel | steady frame loop with `DAO_GPU=null` (fake command processor) | all of `DAORecomp/kernel/`, `gpu/command_processor.cpp` |
| 4 | Rendering | title screen renders through Xenia's D3D12 | `xenia/` (changes in `xenia/PATCHES.md`), `gpu/xenia_host.cpp`, `gpu/xenia_memory.cpp` |
| 5 | Audio, input, settings | music and voices play, pad works, `config.toml` read | `apu/` (XMA decoder), `input/`, `settings.cpp` |
| 6 | Playable | new game, save, load, keyboard UI, 60 fps | `ui/keyboard.cpp`, save-name logic in `kernel/xam.cpp` |
| 7 | Graphics | resolution scale, FSR, HDR, in-game options | `xenia/.../dao_upscale.*`, HDR shaders, `ui/menu_movies.cpp` |
| 8 | Distribution | installer, launcher, updater, CI; built for the player's processor | `installer/`, `install.py`, `launcher/`, `.github/workflows/`, `cpu_check.cpp` |
| 9 | Content | DLC, title update, mods | `content/`, `mods/`, `scripts/stfs.py`, `scripts/match_build.py`, `scripts/pair_data.py`, `scripts/prefix_symbols.py` |

Rename the `dao` namespace, the `DAO_` environment variables and `DAORecomp` paths in
one pass when copying, with a script, before changing anything else.

## What to copy as is, adapt, or redo

- **As is:** the HLE kernel (threads, objects, memory, VFS, xam content), the XMA
  decoder, SDL3 input and audio output, the vendored Xenia GPU and its patches, the
  ERF/STFS/XDS/GFF readers, the installer pipeline and toolchain pinning, the launcher
  shell and updater, `CallGuest`, the `GAME_HOOK` machinery, the frame-history and
  timing tools.
- **Adapt:** `config/<Game>.toml` (new switches, function bounds, EH records), the
  installer's hashes and media IDs, the launcher art (from the new game's menus), the
  mod converter (new formats or versions).
- **Redo per game:** every hook address below, the Graphics tab ActionScript, the HDR
  tonemapper replacement (the new game's tonemap shader is different).

### Later title updates

Before recompiling another title update as a build of its own, compare its image with
the one already built: `xexdump <default.xex> <out> <default.xexp>` dumps a patched
image, and a word-by-word diff of each section shows what changed. DAO's Title Update
7 has 6's `.text` layout and `.data`, and differs in six instructions: five address a
`.data` global four bytes lower (0x83299A5C, not 0x83299A60; nothing else uses
either), and one is a version number (0x304D in 6, 0x32FF in 7) stored by the
constructor at 0x82620A10. Its image is 64 KB bigger, all of it `.reloc`. Code
generated from 7's patch with `config/DAO_tu6.toml` differs from 6's in exactly those
six instructions and `PPC_IMAGE_SIZE`. So 7 runs on 6's build: a hook sets the
version number, and the loader takes either image size, copying no further than the
build's (the function table follows the image).

## DAO's hooks, and how each was found

Each line: what it is, DAO's address (disc / TU6), how it was found, what to look for in
the next game.

- **Swap interrupt / present interval** (`sub_82CD4218` / `tu6_sub_82D9E0F8`): the
  function called with `frontbuffer | interval << 8`. Found from the `VdSwap` import's
  callers and the PM4 trace (interval 2 in 3D scenes). Clamp the interval to 1 for
  60 fps, after checking game logic doesn't count vblanks. Interval 0 flips at once:
  above 60 fps, pace the frames in `VdSwap` instead (unless unlimited).
- **Fence wait step** (`sub_82CC4338`): the D3D `BlockOnFence` spin loop. Found with
  `DAO_PROFILE` stack samples of the 100%-CPU render thread. Make it sleep.
- **Held shot hand-over** (TU6 only: `tu6_sub_82557DE0` ending the hold,
  `tu6_sub_82555060` the render job): the update thread polls the job's running byte
  with `Sleep(0)`, but the job requeues itself every frame. Found from a thread dump
  during a frozen conversation shot (the update thread in that loop), then per-second
  call counts and time spent in the two functions, with a key press marking the
  freezes. The job waits at its end for the poller.
- **Field of view** (camera update `sub_82265108` / `tu6_sub_82284BD8`, camera SetFov
  `sub_82650038` / `tu6_sub_82683E60`, main camera pointer `0x83211D44` / `0x832E227C`):
  the update gives the main camera its field of view every frame, gameplay's 60 degrees
  and conversation shots' own, through SetFov (vtable slot 3, `stfs f1,320(r3)`).
  Found by scanning guest memory for perspective matrices, then for the camera holding
  the angle; the camera's vtable gave SetFov, and a hook logging `ctx.lr` and the game
  mode (`[[r3+280]+120]+96`: 0 exploring, 4 conversation, 8 to 10 title and loading;
  5, which the update checks with 4, taken for cutscenes, unverified) the caller. Scale the angle
  in SetFov while the update runs.
- **Scene tiles** (`sub_82661640`, MSAA global `0x8316E35C`, tile rects `0x82144D64`):
  the function returning the predicated-tiling tile count, called every frame. Found
  from `DAO_GPU_FRAME_TRACE` (two resolves per scene) back to the code setting the
  tile rects. Forced to one tile without MSAA when a temporal upscaler is on.
- **Occlusion queries off** (renderer init `sub_8263BBF0`, flag byte `0x83211EC3`):
  the engine's own "queries disabled" path. Found from the query manager's vtable
  (Begin/End/GetResult), whose GetResult checks the flag; the hook sets it after the
  renderer's one-time init computed it.
- **Engine options** (options object `0x83211BB8`, render flag bytes next to it):
  found from the options registration table (`group, name, &field`); the Graphics tab
  writes shadows and frame-buffer effects there.
- **Options menu callbacks** (`sub_825AF710` SendMessage, `sub_825B5410` GetNumber):
  Scaleform `FSCommand` handlers, found from the Flash callback table's name strings.
  They carry the Graphics tab's messages.
- **Save writer** (`sub_82302150`): writes `scrnshot.bin` from the thumbnail texture in
  `r4`. Found by the file name string. Resolves stay on the GPU in our Xenia, so any
  game code that reads rendered pixels on the CPU (thumbnails, screenshots, readback
  for picking) needs `ReadBackGpuWrittenMemory` before it reads. Look for these early:
  the symptom is black or stale images.
- **Kernel-side, not hooks:** `XamShowKeyboardUI` (a stub returning the default text
  blocked naming characters; DAO now has an on-screen keyboard, `ui/keyboard.cpp`), `XamContentCreateEx` (licence mask, keep display names),
  save file names (`0x<crc32 of the UTF-16BE character name>` in DAO; check the new
  game's scheme on its saves before touching names).

To move hooks to a title update's build, use `scripts/match_build.py`, then check each
match by hand.

## Order things broke in DAO (expect the same)

1. Image tail overrun at load (zero-fill).
2. Missing instructions, then crashes from missed switch tables and split assembly
   functions, all far from their cause: run the finders before debugging crashes.
3. Hangs at the first GPU interrupt (the `0xE0000000` view offset).
4. Hang at a loading screen until XMA was really decoded.
5. 30 fps lock (present interval), then one-frame flicker at 60 fps (occlusion
   queries).
6. A write-watch race crash after ~5 minutes of play.
7. Coloured blocks in Bink movies (split `memset`).
8. Character naming impossible (keyboard UI stub).
9. "Save file is corrupt" after resuming (content display name rewritten).
10. Black save thumbnails (GPU-only resolves).
11. A GPU hang (`DXGI_ERROR_DEVICE_HUNG`) with FSR motion vectors on, only while
    another program used the GPU too. Several draws streamed out (D3D12 stream
    output) under one binding, appended; on AMD a preemption between them could
    hang the GPU. Fixed by binding each draw its own buffer part and counter and
    unbinding after it (about 0.7 ms a frame). `DAO_DRED=1` shows the hung draw;
    a small D3D11 compute load running alongside reproduces it.
12. Conversation camera frozen for seconds after a line (TU6): a `Sleep(0)` poll on
    a flag clear for microseconds, against the spin-wait sleep. Fixed by the other
    side waiting for the poller.

## Test recipes

Use `scripts/test/run_game.sh` (see [its README](../scripts/test/README.md)) and copy
it to the new repo. It does what every run needs: refuses to start while the game is
running, opens the window in the background, gives the game a test copy of the
settings and restores the user's, removes saves the run made, and collects the
captures and logs. `contact_sheet.py` turns a run into one image to look at,
`perf_summary.py` gives fps and CPU per thread, and `untile.py` shows tiled texture
data the game wrote.

**Scripted input** (`DAO_INPUT_SCRIPT`, a file or inline text): statements
`<time> <BUTTONS> [hold]` and `every <period> <BUTTONS> [hold] [from <t>] [until <t>]`,
separated by newlines or `;`, with `#` comments. DAO's sequences (load the latest save,
turn the camera, open Options, make a new save) are in `scripts/test/inputs/`. Build a
new one with captures every 2 s, read the times off the contact sheet, and keep it
there.

**Environment variables** (rename the prefix for the new game):

| Variable | Does |
|----------|------|
| `DAO_BACKGROUND=1` | window opens behind, focus kept |
| `DAO_CAPTURE_EVERY=<s>` | saves the game's output to `userdata/capture_N.png` |
| `DAO_INPUT_SCRIPT=...` | scripted gamepad, merged with the real one |
| `DAO_SKIP_LAUNCHER=1` or `--play` | straight to the game |
| `DAO_GPU=null` | fake command processor (kernel bring-up) |
| `DAO_GPU_TRACE=1/2`, `DAO_GPU_FRAME_TRACE=<swap>` | PM4 log; one frame's draws and resolves |
| `DAO_GPU_TIMING=1` | GPU ms per frame section, every 300 frames, in `xenia.log` |
| `DAO_DRED=1` | D3D12 breadcrumbs and page-fault data on device loss |
| `DAO_FRAME_HISTORY=1` | finds one-frame flickers, saves their draw lists |
| `DAO_THREAD_STATS=<s>`, `DAO_PROFILE=...` | per-thread CPU %, sampled guest stacks |
| `DAO_DUMP_AFTER=<s>` | thread dump with host stacks (`scripts/symbolize.py`) |
| `DAO_RES_SCALE`, `DAO_HDR`, `DAO_FRAME_RATE` | override settings for one run |
| `DAO_XMA=fake`, `DAO_AUDIO=null`, `DAO_AUDIO_DUMP=x.wav` | audio bring-up and checks |
| `DAO_TEST_KEYBOARD=<s>` | opens the on-screen keyboard |
| `DAO_PHOTO_DIR=<dir>` | where photo mode saves photos |
| `DAO_LAUNCHER_SCREENSHOTS=<dir>` | launcher screenshots, then exits |
| `DAO_LAUNCHER_DROP=<paths>` | with the above: files dropped on the launcher (see README) |
| `DAO_LAUNCHER_PAD=<steps>` | with the above: a virtual gamepad's script instead (see README) |
| `DAO_BUILD=disc` | forces the disc build when a title update is installed |
| `DAO_CONVERT_MOD`, `DAO_EXTRACT_PACKAGE` | run one converter or package step and exit |

**A test that "changed nothing":** first check the game ran at all (only two output
files means it didn't), and that the setting reached the game (`dao.log`).

## Starting checklist for Dragon Age II

1. New repo from DAO's: copy the reusable parts, rename prefixes, remove DAO's game
   hooks (keep the `GAME_HOOK` machinery), new `config/<Game>.toml`.
2. Hash the player's `default.xex` and record its media ID (title updates are per
   disc).
3. Phases 1–3 above; note every missing instruction and extend
   `patch_recompiler.py` (the DAO list is already applied).
4. Before rendering work, look for: the present interval, the query manager,
   predicated tiling, and any CPU reads of rendered data.
5. Count C++ EH use; check whether the game relies on catching exceptions.
6. Keep `docs/status.md` current at the end of every session.
