# Notes for future Xbox 360 recompilations

What this project (Dragon Age: Origins, title 454108C0, XenonRecomp + a custom runtime)
taught us that should carry over to the next Xbox 360 game. Addresses given as examples
are DAO's; the patterns are what matter.

The step-by-step plan for the next port (what to reuse, how each hook was found, test
recipes) is in [porting-playbook.md](porting-playbook.md).

## Order of work that worked

1. Decrypt the XEX, recompile, and get the generated C++ to build with zero warnings
   before writing any runtime code.
2. High-level kernel (HLE) until the game reaches a steady frame loop, with a fake PM4
   command processor that only acknowledges the GPU.
3. Real rendering by vendoring Xenia's Direct3D 12 GPU code (see below), then input,
   audio and settings.
4. Frame pacing (60 fps), then resolution scaling, HDR and upscalers.
5. Performance work. Measure first (see Measuring), and decide by the weakest target
   device.
6. Launcher, installer and updates, then mods.

## Getting the binary

- XenonUtils allocates only the sum of the blocks of a *basic*-compressed XEX, but
  reports the larger image size from the header. Reading the tail overruns, so the
  runtime loader must zero-fill up to the reported size.
- Check the user's `default.xex` by hash before doing anything, so a different region
  or title update fails early with a clear message.

## XenonRecomp

- **Instructions:** expect missing instructions. We added about 35
  (`scripts/patch_recompiler.py`):
  - update-form loads and stores;
  - `bdzf` and `bdnzt`;
  - `eqv`, `addme`, `subfze`, `addc`;
  - `frsqrte`;
  - VMX128 `vsel128`, `vnor128` and `vcmpbfp128`.

  Re-patch, rebuild and regenerate as one script, and **check XenonRecomp's exit code**
  (ours was ignored for a while).
- **Jump tables:** XenonAnalyse found 42 jump tables, the real number was 603.
  `scripts/find_switches.py` finds them by concrete execution of the dispatch code. It
  handles nops, reordering, `bgtlr` guards, chained dispatch, `mr` copies and debug
  reloads. Write a finder like it early; missed switches crash far from their cause.
- **Function boundaries:**
  - **Frameless leaf functions with switches** need explicit bounds.
    `scripts/fix_functions.py` writes them into `config/DAO.toml`.
  - **Hand-written assembly with no `.pdata`** (CRT `memset`, `memcpy`) can be split at
    a conditional return such as a `bdzlr` tail into several "functions". The caller
    then falls into the wrong one. DAO's `memset` skipped the last 1–2 bytes when
    `len % 4` was 2 or 3, which showed up as coloured 8×8 blocks in Bink videos.
    `fix_functions.py` now finds such splits: a start that nothing references, right
    after an instruction that falls through.
  - **C++ exception-handling records** placed before functions (`__CxxFrameHandler`,
    `__C_specific_handler` thunks) must go in `invalid_instructions`.
  - XenonRecomp has no exception support, and DAO uses C++ EH in about 1100 functions.
    Count a game's EH use early: it decides whether throws need emulating.
- **Register save/restore helpers:** find them by byte pattern (`scripts/find_helpers.py`)
  rather than by hand.
- **Config paths:** XenonRecomp prefixes the config file's directory to the TOML's paths
  as plain text. Pass the config by bare name, with the working directory set to its
  folder. An absolute config path aborted with 0xC0000409.
- **Overriding guest functions:** the generated code has a weak alias
  `sub_X -> __imp__sub_X`. Define `PPC_FUNC(sub_X)` and call `__imp__sub_X` from it.
  That one hook point covers frame pacing, menu hooks and engine options.

## Runtime and kernel

- **Physical memory:** the `0xE0000000` view is offset by `+0x1000`
  (`E0000000 + x` = physical `x + 0x1000`), because D3D computes GPU addresses that way.
  With the plain mapping, the interrupt request block hangs.
- **GPU registers:** register writes all go through `PPC_MM_STORE_U32` (17 sites in
  DAO). A force-included hook header routes them to the command processor. Reads are
  plain loads, so the command processor mirrors registers into guest memory.
- **Endianness:** scratch-register writeback is big-endian. GPU memory operations use the
  low 2 address bits as the endian mode.
- **Callbacks and calling conventions:**
  - The XAudio render driver callback gets a *pointer* to a cell holding the argument,
    as in Xenia.
  - NetDll's WSA event and wait functions and `inet_addr` take no caller argument; the
    rest of NetDll does.
- **XMA:** audio must actually be decoded. DAO hangs at a loading screen without it. We
  ported Xenia's new XMA context onto its FFmpeg fork (xmaframes).
- **Spin waits:** guest spin waits (`KeDelayExecution(0)` loops) should sleep briefly.
  This took the game threads from 100% CPU to about 25% each at 60 fps.
  - A sleeping poll can miss a flag the other thread clears for only microseconds
    (DAO's conversation camera froze for seconds). Make that thread wait for the
    poller rather than shorten the sleep.
- **GPU write watches:** two threads can fault on the same watched page. The handler
  must retry if the page has become writable meanwhile.
- **Diagnosing aborts:** log the host stack on `abort()` and `terminate`. Silent
  0xC0000409 aborts are otherwise undiagnosable.

## Rendering

- **Xenia's GPU vs reimplementing D3D9:** we first planned a native reimplementation of
  the game's static D3D9 on a modern API, as UnleashedRecomp does. Vendoring Xenia's
  D3D12 GPU (changes listed in `xenia/PATCHES.md`) got correct rendering in a day. It
  still allowed 4K (resolution scale with tiled resources), HDR output and FSR 3/4,
  added as patches. Start there, and replace pieces only when they block a goal.
- **Frame rate:**
  - 30 fps locks are usually the present interval. In DAO the swap-interrupt handler
    queued flips for vblank `last + 2` in 3D scenes. Clamping the interval to 1 in a
    `PPC_FUNC` override gave 60 fps at normal game speed, because game logic ran on its
    own thread and nothing read the vblank counters. Interval 0 flips at once, for
    rates above 60 paced on the host; the guest's vblanks stay at 60 Hz.
  - Check both of those conditions before unlocking.
- **Submitting before waiting:** a GPU `WAIT_REG_MEM` on the flip comes after a frame's
  draws but before its swap, so the whole frame sat unsubmitted. Submit before waiting.
- **Occlusion queries:** Xenia's results were garbage (0, 1, wrapped values), and at
  60 fps objects flickered out for a frame. Xenia's strict mode didn't help. The fix was
  the engine's own "no queries" path (a flag byte that makes queries report full
  visibility). Look for such switches in the engine before fixing the emulation.
- **Motion vectors for temporal upscalers:**
  - Re-run opaque scene draws' vertex shaders with stream output, using current and
    previous constants. Previous ones are matched by shader, vertex/index base and
    count.
  - Gotcha: with predicated tiling and `window_offset_in_edram`, tile 2 draws into a
    different host depth target than the one the resolve reads, and the last tile's
    resolve clears depth. Copy scene depth at the first resolve after a tile's draws.
  - Stream output defeats post-transform vertex reuse on AMD under Windows (scene
    vertex-shader invocations 1104k vs 485k). Budget for it, and measure on the target
    device.
  - Forcing one 1280×720 tile and no MSAA (a hook on the tile-count function) helps the
    upscaler path.
- **Sources for this work:** Xenia's PM4/ucode traces and its shader dump cvar
  (`dump_shaders`) are the fastest way to understand the game's passes: tonemapper,
  LUTs, the RGBM-like scene format.

## Measuring

- At 1× resolution, GPU timings are paced by the CPU: clocks drop and idle gaps get
  counted. Measure GPU cost GPU-bound (resolution scale 3), or use busy % × frame time.
- **Tools that paid off:**
  - thread dumps symbolized from the link map;
  - per-thread CPU % and sampled guest stacks;
  - GPU time per frame section and pipeline statistics;
  - a frame history that finds one-frame flickers automatically and saves the draw
    list of each frame;
  - scripted gamepad input (`DAO_INPUT_SCRIPT`) and periodic captures, to repeat a run
    without a person.
- Turn off driver-level vsync and frame caps (and set the game's frame rate to
  unlimited, vsync off) before comparing fps.
- Ask for numbers from the weakest target device (here the Steam Deck) before choosing
  between optimizations whose costs depend on the GPU and driver.

## Game UI and data

- The menus are Scaleform (Flash 8, ActionScript 2) `.gfx` files inside ERF archives.
  An AS2 decompiler, plus a small compiler (`scripts/as2c.py`), let us add a whole
  Graphics tab. The runtime patches the player's own movie at startup and serves it
  through the VFS, so no game files ship.
  - Gotcha: the `CFX` length field can be too small, so inflate without trusting it.
  - Gotcha: a named `DefineFunction2` doesn't push the function, so emit anonymous ones.
- Engine options are often a table of `(group, name, &field)` registrations plus an
  apply-on-commit diff. Find that table and you can drive settings from outside.
- Xbox 360 textures (`.xds`) have a 52-byte trailer holding the format and size. Their
  data is 16-bit byte-swapped and Xenos-tiled.
- The disc has no `packages/core/override` folder, but the engine opens and lists it.
  Anything served there must make its parent folders resolvable and listable too.

## Mods

- PC mods are not Xbox 360 mods:
  - PC GFF files say `PC  ` (Xbox 360 ones say `X360` and are big-endian);
  - PC textures are `.dds` (Xbox 360 ones are `.xds`);
  - PC archives have UTF-16 headers.

  Loading PC files crashes the engine, so detect them and refuse them, and offer to
  convert them (`DAORecomp/mods/convert/`). What converting took:
  - GFF: not just a byte swap. The 360's structs have other field offsets and
    alignments, so the tree is read and written again. Take each struct's layout from
    the game's own files of the same type; derive one the way the game lays them out
    only when the game has none. The engine reads fields by label and falls back to a
    default for a missing one, so extra or missing fields are safe.
  - Some structs changed: the PC's animation curves (`CURV`) keep keyframes and
    tangents as struct lists, the 360's as one float list.
  - Meshes: vertices in the 360's formats (packed normals and tangents, half floats)
    and declarations; vertex and index data padded as the game's own files are.
  - Textures: `.dds` to `.xds` (tiled, with the trailer), PC normal maps (DXT5 with X
    in alpha) to DXN, as the 360's are.
  - Distant models: the 360 has smaller copies of models, materials and textures
    (`_1`–`_3`, `_l2`/`_l3`, `l2`/`l3`). A mod replacing only the full one shows the
    game's own from a distance, so the copies are made from the mod's too.
  - Menu atlases name their pictures differently on the two versions; a PC layout
    makes the 360's menus fail. Use the game's layout when the mod's pages have every
    picture in the same place, otherwise leave the atlas out.
  - Scripts (`.ncs`) are the same format on both, but a script may call engine
    functions newer than the 360's (it has up to 871): decode the calls to warn.
  - Mods are untrusted input: bound every archive, GFF and texture read, refuse
    overlapping archive entries and device names, cap what a conversion unpacks,
    check free space, and do the work aside so an interrupted conversion changes
    nothing.
- File-override mods layered over the disc in a load order (a VFS overlay) cover most
  needs, without touching game files.
- Paths on Windows: never use `path::string()` for user or mod paths. It's in the ANSI
  code page and throws for other alphabets. Convert to and from UTF-8 explicitly
  (`DAORecomp/text.h`).
- Windows' `tar.exe` unpacks zip, 7z and rar, but can't read zip entry names outside
  the ANSI code page. Run it detached, in a kill-on-close job, with a time limit.

## Title updates and DLC

- When newer DLC needs a title update, build both executables into one program rather
  than choosing one: recompile the patched image (XenonRecomp's `patch_file_path`) into
  a second folder, prefix its symbols (`sub_`, `__imp__sub_`, the save/restore helpers,
  the entry point and the function table), and pick the build's function table at
  start. Each build's generated code finds its table at its own image's end.
- Most hooks move with the update: find the counterparts by matching code with branch
  offsets and address loads masked (`scripts/match_build.py`), then check by hand.
- A title update is made for one disc (media ID); other languages' discs have their own.
- Mount the title update as `update:` (read-only), apart from the disc: the game reads
  updated files from there itself.
- DLC packages (content type 2) need a licence mask of all ones from
  `XamContentCreateEx`, or the game ignores them.
- Opening existing content must keep its display name. The game passes a fresh one
  (e.g. the time now), and rewriting it made saves show as corrupt.
- To overwrite a save, the game opens it (`XamContentCreateEx` flags 0x13: open
  existing), then "creates always" (0x12) the same name, which deletes the old one first. Keep it somewhere the game doesn't enumerate
  before deleting (DAORecomp keeps one backup per save in `userdata/save_backups/`).
- DAO's TU6 shows a "don't remove the storage device" notice before it lists DLC, and
  waits for A: scripted test input has to press it.
- Read packages as untrusted: check every name, folder index, block number and size,
  and keep extracted paths under MAX_PATH (the game opens files without `\\?\`).
- Don't put the title update in a folder the updater owns: the launcher empties
  `update/` at every start, so the title update lives in `title_update/`.

## Distribution

- **No game files in the repo or the installer.** The installer:
  - extracts the player's ISO;
  - checks its hash;
  - recompiles;
  - builds with a pinned toolchain.

  The pinned toolchain is portable LLVM plus an `xwin` Windows SDK/CRT, so there's no
  Visual Studio and the same toolchain works from Linux. The game still runs there
  through Proton/Wine, which is untested. The regenerated code must come out
  byte-identical to the developer's.
- **Toolchain gotchas:** use `-winsysroot "<dir>"` (dash form), set
  `CMAKE_TRY_COMPILE_CONFIGURATION Release` (xwin has no debug CRT), and pass
  `/manifest:no`.
- Pin every dependency by commit or sha256, and make re-runs incremental (about 2 min).
- Self-update: the release asset's sha256 comes from GitHub's `digest` field, is
  verified before running, and the installer updates in place in about 25 s.

## Process

- Reviews after each major change (logic, security, readability; separate agents)
  caught real bugs every time. Examples: a crash on folder names in other alphabets,
  archives with links reaching outside the mods folder, and a null dereference in a
  test mode.
- Keep test harnesses scripted and repeatable: screenshot modes, simulated drops, input
  scripts. Back up and restore the user's config around them.
- When a fix "doesn't help", check that the test was valid first. Examples: a run that
  never started the game, or GPU timings taken while CPU-paced.

## Checklist for the next game

1. Hash the XEX, decrypt it, and check the image size against what was allocated.
2. Run XenonRecomp, list the missing instructions, and patch them in one script that
   checks exit codes.
3. Find switches with a concrete-execution finder; find helper functions by pattern.
4. Check for split hand-written assembly functions and EH records.
5. Bring up the HLE kernel to the frame loop with a fake command processor, then
   vendor Xenia's GPU.
6. Decode XMA early.
7. Find the present interval, occlusion-query use and engine option tables before
   tuning rendering.
8. Build in the debug tools above before you need them.
9. Installer and update path, with no game files, from the start.
