# Fidelity review: where DAORecomp behaves differently from an Xbox 360

Reviewed: `origin/optimization` at `0de61f4` ("Conversations: end held shots on time"), 2026-10-12.
Report only: no code was changed. Every fix below is a proposal for the owner to apply and test.

## Summary

- **No high-severity finding survived verification.** Nothing found here is certain to freeze or
  corrupt the game with the default settings. Six medium findings and 22 low ones remain, plus
  notes (counts at the end).
- **Most serious confirmed deviation: the "VSync" option.** It doesn't control the PC's vsync. It
  changes the game's own vblank clock from 60 Hz to about 1 kHz. With VSync off, the 30/60 fps cap
  is gone and anything the game counts in vblanks runs about 16 times too fast (M1).
- **Most likely source of more "sometimes" bugs: kernel object signalling.** Our kernel hands a
  signalled event, semaphore or mutant to whichever thread reaches the dispatcher lock first, not
  to the waiter at the moment of the signal as the 360 does. Two quick `KeSetEvent`s can wake one
  thread instead of two, and a Set followed by a Reset can wake nobody. A thread leaving a critical
  section can take it straight back from the waiter it was handed to.
  - A standalone model of `objects.cpp` reproduces all three: 20 of 40 wakes lost in one pattern, and 59 or
    all 60 of 60 lost in another, on an idle machine.
  - A patch is ready (M2), with a diagnostic to run first.
- **Other medium findings:**
  - No audio output (no device, or `DAO_AUDIO=null`) stops the game's whole audio engine (M3).
  - A file read can race the GPU re-protecting its destination pages. On Windows the read fails,
    and the game may exit on a "dirty disc" error. Under Wine/Proton the read comes back silently
    truncated (M4).
  - Two GPU-side costs: one access fault and three `VirtualProtect` calls per rewritten 4 KB page
    (M5), and a 0–2 ms late wake-up on the per-frame flip wait with VSync on (M6).
- **The held-shot fix (`gpu/held_shot.cpp`) is correct and bounded.** Its only residual risk is a
  50 ms hitch, and only if the update thread's end-hold code waits on the parked worker (L8).

How the review was done:
- One agent made a mechanical inventory of the call sites. Five reviewers covered kernel waits and
  time, GPU glue, audio, file I/O and XAM, and the game hooks.
- A separate verifier then tried to refute every candidate, against the code, Xenia canary
  (`1c81b867` for the GPU) and NT/ReactOS semantics.
- What survived is below. A finding is **confirmed** when the deviation and its consequence follow
  from our code alone. It is **plausible** when the harm depends on what the game's code does,
  which this review can't see.

---

## 1. Deviation inventory

Every place found where the host knowingly or accidentally behaves unlike an Xbox 360. Columns:
where we deviate; what the 360 (or NT, or Xenia) does; what we do; why (a code comment or commit;
the history is squashed, so mostly comments); the guest patterns that would break. "→ Mx/Lx/Nx"
points to a finding below.

### Kernel: waits, sleeps, threads, time

| Where | Xbox 360 / NT / Xenia | Ours | Why | Guest patterns that break |
|---|---|---|---|---|
| `kernel/objects.cpp:37-41`, `111-224`, `599-657` | Waiters are satisfied inside KeSetEvent / KeReleaseSemaphore / KeReleaseMutant, first come first served (KiWaitTest) | Signal = set a flag and wake every waiter; whoever re-takes the dispatcher lock first gets it. `RemoveWaiter` swap-and-pop (`:24-30`), so not FIFO | Simplicity (one dispatcher lock, header comment) | Two Sets of an auto-reset event in quick succession; Set then Reset; critical-section and mutant handoff → **M2** |
| `kernel/objects.cpp:704-731` | `Sleep(0)` yields | After 64 calls <50 µs apart, each call sleeps `kSpinSleepMicroseconds` (100 µs; `ShortSleep`, `:665-697`) | Power: game threads from 100% to ~25% CPU (comment `:711-719`, notes) | Flags clear for microseconds (the held shot); loops doing 1–49 µs of work per `Sleep(0)` → **L1** |
| `kernel/objects.cpp:734-738` | Relative delay ends on a clock tick (probably 1 ms); Xenia turns <1 ms into a yield | `sleep_until`, rounded up to whole ms by MSVC STL: 1–2 ms with `timeBeginPeriod(1)` | `std::this_thread` | Only a direct sub-ms `KeDelayExecutionThread` (XAPI `Sleep` can't make one) → N (refuted) |
| `kernel/objects.cpp:601-607` | An already-signalled object satisfies an alertable wait (WAIT_0) before user APCs are considered | APCs checked first: returns USER_APC without taking the object | — | Alertable waits on signalled objects with a timer/IO APC queued: an extra loop → **L11** |
| `kernel/objects.cpp:286-300` | A timer's APC is one KAPC; not re-queued while pending; cancelled by NtCancelTimer/NtSetTimerEx | One APC pushed per firing, unbounded, never removed | — | Periodic timer APC + rarely-alertable thread: a burst of callbacks later → **L12** |
| `kernel/objects.cpp:336-350` | Kernel timer table | A detached `std::thread` per periodic `Set` | — | Thread churn if periodic timers are re-armed often → N |
| `kernel/objects.cpp:131-145` | `KePulseEvent` satisfies current waiters atomically | Set, unlock, yield, relock, reset | — | None: not imported by the game |
| `kernel/thread.cpp:325-332`, `objects.h:102` | Thread exit abandons owned mutants (waiters get ABANDONED_WAIT_0) | Not abandoned; `Mutant::owner` is a raw pointer that dangles after exit | — | Thread exiting while holding a mutant → **L7** |
| `kernel/xboxkrnl_threading.cpp:370-382`, `81-82` | `ObReferenceObjectByHandle` / `ObDereferenceObject` count references; Xenia keeps a 0x80-flag thread in its handle table | No reference taken; dereference is a no-op; a thread created with flag 0x80 is freed at exit | — | Reference, close the handle, wait on the pointer → **L9** |
| `kernel/xboxkrnl_threading.cpp:506-539` | XDK critical section spins `header.absolute × 256` times (0 for a plain `RtlInitializeCriticalSection`) | Always 1000 × `_mm_pause` | — | Costs 3–45 µs of CPU per contended Enter; widens M2's barging window → N |
| `kernel/xboxkrnl_threading.cpp:110-124` | Affinity binds the thread to a hardware thread | Only the guest-visible CPU number changes; host affinity untouched; threads with no processor mask report CPU 0. Every guest thread has its own PCR | Deliberate (no pinning) | Per-CPU data guarded only by "one thread per CPU" + raised IRQL → L16 |
| `kernel/xboxkrnl_threading.cpp:126-167` | Priorities apply within one hardware thread | Global host priorities: increment ≥2 → HIGHEST, ≥15 → TIME_CRITICAL | — | Busy high-priority guest threads starving a normal-priority lock holder on few-core hosts → **L17** |
| `kernel/xboxkrnl_threading.cpp:582-635` | Raising IRQL to DISPATCH stops preemption on that CPU and holds off its DPCs/interrupts | IRQL is a byte in the PCR; nothing else changes | — | Code protecting state shared with an ISR by raising IRQL only → **L16** |
| `kernel/xboxkrnl_threading.cpp:342-351` | NtCancelTimer returns the signal state | Always writes 0 (as Xenia) | Copied from Xenia | None likely (XAPI passes NULL) → N |
| `kernel/xboxkrnl_threading.cpp:279-283` | Releasing a semaphore past its limit raises STATUS_SEMAPHORE_LIMIT_EXCEEDED | Returns -1, count unchanged | — | Only buggy code → N |
| `kernel/xboxkrnl_threading.cpp:206-213` | Critical regions, L2 locking, FPU exceptions | No-ops | Host has no equivalent | None found |
| `kernel/module.cpp:262-278` | KeTimeStampBundle: ms since boot (large) | From 0 at process start, refreshed every 1–2 ms (Xenia does the same) | — | "0 = never" sentinels or cooldowns in the first seconds → N |
| `main.cpp:35-45` | `mftb` at 49.875 MHz, ~20 ns steps | QPC scaled to 49.875 MHz (100 ns steps); monotonic; agrees with `KeQueryPerformanceFrequency` | `patch_recompiler.py` | None (refuted) |
| `main.cpp:293` | — | `timeBeginPeriod(1)`; no opt-out of Windows 11's timer-resolution throttling for hidden windows | — | Minimized/occluded on Windows 11: every ms sleep ~15.6 ms → **L20** |
| `config/DAO.toml:12` (XenonRecomp) | `lwarx`/`stwcx.` reservation broken by any store | Value compare-and-swap (`reserved_as_local=false`) | Upstream XenonRecomp | Hand-written untagged LL/SC lock-free pops (ABA) → N |

### GPU

| Where | Xbox 360 / Xenia | Ours | Why | Guest patterns that break |
|---|---|---|---|---|
| `xenia/src/xenia/gpu/graphics_system.cc:103-117`, `134`, `178-180`; `settings.cpp:300` | vblank is 60 Hz, always | 60 Hz with VSync on; with VSync off `Sleep(1 ms)` per pass → ~500–1000 guest vblank interrupts/s | Upstream Xenia cvar, exposed as "VSync" | D3D's vblank-counted flips (frame cap gone); anything counted in vblanks → **M1** |
| `xenia/src/xenia/ui/d3d12/d3d12_presenter.cc:1231-1234` | — | The PC always presents with interval 0 (+ tearing allowed), whatever "VSync" says | Xenia | Tearing in borderless fullscreen; judder against the display → **L21** |
| `graphics_system.cc:118-177` | vblank exact, never late or bunched | Deadline pacing, NtDelayExecution (1 ms granularity), up to 1 s of missed beats delivered back to back, normal-priority thread, free-running against host refresh | Comment: keep picture and audio in step | One short frame after a stall of this thread; a beat with the display → **L14** |
| `xenia/src/xenia/gpu/pm4_command_processor_implement.h:744-769` | CP re-polls every few µs | VSync on (the default): `Sleep(wait/0x100 ms)`, 1–2 ms per poll. VSync off: yield ~1 ms, then 100 µs sleeps. `wait < 0x100`: hot spin | Upstream; DAO comment "core free for guest threads" | The per-frame flip wait: the CP sees the flip 0–2 ms late → **M6** |
| `DAORecomp/gpu/xenia_memory.cpp:218-244`, `266` | No cost on hardware; upstream Xenia unprotects the callbacks' common range (≤4 MB) | Only the touched page is unwatched; the callbacks already dropped 256 KB | Comment: "a later optimization" | Rewriting GPU-cached memory: one fault + 3 VirtualProtect per page → **M5** |
| `DAORecomp/kernel/xboxkrnl_io.cpp:245-256` | Reads land in memory | Watches dropped, then kernel-mode `ReadFile` with no lock held | Comment `:245` | GPU re-arms a page during the read → **M4** |
| `xenia/src/xenia/gpu/primitive_processor.cc:1666-1668` | — | Cache entries inside an invalidated range survive (bound tested against the wrong end) | Upstream bug | Index buffers rewritten in place that need conversion → **L4** |
| `DAORecomp/gpu/xenia_host.cpp:409-421` | Graphics interrupt = DPC at DISPATCH on the target CPU, held off by that CPU's IRQL; upstream Xenia sets IRQL 2 and `dpc_active` | Host thread, IRQL and `dpc_active` untouched, serialised by a recursive mutex; only the CPU number set | — | ISR code checking IRQL/DPC state; per-CPU exclusion by raising IRQL → **L16** |
| `xenia/src/xenia/gpu/command_processor.cc:411-420` | Read pointer written back every RB_BLKSZ while fetching | Written once per batch (upstream) | Upstream | Ring space frees later, up to one vblank → **L22** |
| `xenia/src/xenia/gpu/command_processor.cc:380-402` | — | CP idle wait times out after 100 ms (upstream 2 ms); event-driven | Comment: handheld power | Only zpass corrections polled there; queries are off → N |
| `DAORecomp/gpu/video.cpp:250-256` | D3D spins on the fence with a hang check | 100 µs high-resolution sleep after every failed step, from the first | Comment: power | Short fence waits pay ≥100 µs → N |
| `DAORecomp/gpu/video.cpp:269-275` | Flip at `last + interval` vblanks, the game's threshold | Interval 1, threshold 100 when the 60 fps unlock is on, for every present | 60 fps (notes: game logic doesn't count vblanks) | Anything paced per present at interval 2 (spinners, movies without BinkWait) → **L18** |
| `DAORecomp/gpu/video.cpp:365-378` | 2 tiles, 2× MSAA | 1 tile, no MSAA with a temporal upscaler | GPU cost | Code reading the second tile rect (none known) |
| `DAORecomp/gpu/d3d_queries.cpp:26-36` | Real occlusion counts | The engine's own "no queries" flag, forced after renderer init | Xenia's zpass counts unreliable | Decisions made at init before the flag is set (none seen) |
| `DAORecomp/gpu/held_shot.cpp:33-51` | The render job never waits | The job parks (≤50 ms) until the end-hold function returns | The held-shot freeze (0de61f4) | End-hold code that waits on the worker → **L8** |
| `xenia/src/xenia/gpu/command_processor.cc:93` (readback `none`) | The CPU can read resolved data | Resolves stay on the GPU; the CPU reads stale memory except where hooked (save thumbnail) | Upstream default | CPU readback of GPU results (luminance, picking) |
| `DAORecomp/gpu/xenia_host.cpp:199-200` | Live status registers | 0x1951 (vblank status) hard-wired to 1 in guest memory | "registers the game reads directly" | Reads of other live status registers (owner says none) |

### Audio

| Where | Xbox 360 / Xenia | Ours | Why | Guest patterns that break |
|---|---|---|---|---|
| `DAORecomp/apu/audio.cpp:49-52`, `93-106` | Render callback every 5.33 ms whatever the output; Xenia's no-audio driver keeps calling | Skipped while ≥8 frames are queued, so never again when nothing drains the queue | Comment `:49` (back-pressure) | Anything waiting on voice progress when there is no output → **M3** |
| `DAORecomp/apu/audio.cpp:108-132` | Hardware-clocked, effectively real-time | Normal-priority host thread; more than 20 ms late → schedule restarted, missed callbacks dropped silently | Comment `:114` | Audio clock slips; underruns under load → **L15** |
| `DAORecomp/apu/audio.cpp:797-805` | Xenia's `XMAInitializeContext` writes the Clear register: decoder state reset | Host decoder state kept (the comment says "as Xenia does"; Xenia does the opposite) | Wrong comment | A reused context starts with leftovers of the previous sound → **L3** |
| `DAORecomp/apu/xma_decoder.cpp:709-717` | Xenia advances past a bad frame | Sets error 4 and returns without advancing: the context never moves again | 714c2a4, memory safety | A misaligned offset or damaged packet silences that voice for good → **L10** |
| `DAORecomp/apu/audio.cpp:826-846` | Asynchronous, continuous while enabled | One synchronous pass per `XMAEnableContext` (as Xenia) | Comment `:839` | None (matches Xenia) |
| `DAORecomp/apu/audio.cpp:235-236`, `802-804` | Xenia re-reads buffer pointers from the context every pass | Pointers cached at Initialize | Comment `:798-800` | The game writing the context directly → N |
| `DAORecomp/apu/audio.cpp:848-872` | Xenia's disable waits for the in-flight pass | `wait=0` try-lock fails on any contention | — | Two guest threads on one context → N |
| `DAORecomp/apu/audio.cpp:667-683` | Real category volumes and change mask | 1.0, mask 0 | Stubs | Custom soundtracks only |
| `DAORecomp/apu/audio_output.cpp:80-91` | — | An underrun inserts silence and keeps the late frame: latency creeps to the 8-frame cap | — | Latency only |

### File I/O, XAM, network

| Where | Xbox 360 / NT / Xenia | Ours | Why | Guest patterns that break |
|---|---|---|---|---|
| `DAORecomp/kernel/xboxkrnl_io.cpp:660`, `692`, `720` | Search pattern kept from the first call; XAPI `FindNextFile` passes NULL; no match on the first call = NO_SUCH_FILE | Pattern read on every call; NULL matches everything; NO_MORE_FILES | — | `FindFirstFile("dir\\*.ext")` then `FindNextFile` → **L2** |
| `DAORecomp/kernel/xboxkrnl_io.cpp:1-2`, `215-232` | Async-handle reads return PENDING and complete later | Every read completes before returning (as Xenia) | Header comment | A thread blocks for the host read time (hitches on cold reads) → N |
| `DAORecomp/kernel/xboxkrnl_io.cpp:224-228` | APC only on success (NT, Xenia `NtReadFile`) | Also queued on EOF and on any scatter-read status | — | ReadFileEx-style readers: completion runs after a FALSE return → **L13** |
| `DAORecomp/kernel/xboxkrnl_io.cpp:186-190` | Missing parent = OBJECT_PATH_NOT_FOUND; share modes enforced | Parents created silently; everything shared | Convenience | Folder probes by create; exclusive-open locks → N |
| `DAORecomp/kernel/xboxkrnl_io.cpp:320-330` | Offset `~0` = append; specific errors (DISK_FULL) | `~0` = current position; every write failure = ACCESS_DENIED | — | Append logs; "storage full" message on save → N |
| `DAORecomp/kernel/xboxkrnl_io.cpp:514-549` | Rename honours ReplaceIfExists and RootDirectory | `fs::rename` always replaces; a bare target name fails | — | Rename-without-replace logic → N |
| `DAORecomp/kernel/xboxkrnl_io.cpp:342-343` | Flush | Real `FlushFileBuffers` (Xenia stubs it) | Durability | A save-time stall on an HDD |
| `DAORecomp/kernel/xam.cpp:1139-1149` | XMsg dispatched to XAM apps, which write their outputs | SUCCESS, outputs untouched | Stub | XMP getters (music state, volume, controller) read garbage → **L6** |
| `DAORecomp/kernel/xam.cpp:1121-1128` | Overlapped marked pending, event reset, completion APC to the caller | None of these | — | Polling `XHasOverlappedIoCompleted`; a still-signalled event → **L5** |
| `DAORecomp/kernel/xam.cpp:1048-1054`, `1062-1075` | Device selector / message box show UI, broadcast XN_SYS_UI 1→0 and wait for the user | Immediate result, no XN_SYS_UI; message box returns the default button | Stubs | A flow waiting for XN_SYS_UI(0); a "delete corrupt save?" box answered for the player |
| `DAORecomp/kernel/xam.cpp:1083-1088` | Dirty-disc UI (blocks forever) | TerminateProcess | Deliberate | Any failed read ends the game (see M4) |
| `DAORecomp/kernel/xam.cpp` (no sender) | Controller connect/disconnect sends XN_SYS_INPUTDEVICESCHANGED | Never sent; one virtual pad, always connected | Design | "Reconnect controller" prompt never shows |
| `DAORecomp/kernel/xam.cpp:63-68` | Xenia writes length 0xFFFFFFFF on failure (its own hack) | 0 | — | A game checking length for success |
| `DAORecomp/kernel/xam.cpp:863-918` | — | Save backup and "being written" marker only for content modes 2 and 5 | Host-only safety feature | Saves written with mode 4 (open always) get no backup → N |
| `DAORecomp/kernel/xam.cpp:980` | FATX order (creation order) | NTFS alphabetical | Host file system | Save-list order if the game doesn't sort |
| `DAORecomp/kernel/xnet.cpp` | Network | WSAStartup succeeds; sockets fail WSAENETDOWN; XNetGetTitleXnAddr NONE (not PENDING); link status 0 | No online | None found (no PENDING loops) |

### Game hooks and host-side lifetime

| Where | Xbox 360 | Ours | Why | Guest patterns that break |
|---|---|---|---|---|
| `DAORecomp/gpu/video.cpp:256` | (see GPU table) fence step | 100 µs sleeps | Power | — |
| `DAORecomp/gpu/video.cpp:276` | (see GPU table) swap interrupt | Interval clamp | 60 fps | — |
| `DAORecomp/gpu/video.cpp:327` | Save writer reads the thumbnail | Readback from the GPU first (5 s timeout) | GPU-only resolves | — |
| `DAORecomp/gpu/video.cpp:380` | (see GPU table) scene tiles | 1 tile, no MSAA | Upscaler | — |
| `DAORecomp/gpu/d3d_queries.cpp:39` | (see GPU table) | Queries off | Flicker at 60 fps | — |
| `DAORecomp/gpu/held_shot.cpp:53-54` | (see GPU table) | Job parks for the poller | Held-shot freeze | L8 |
| `DAORecomp/ui/graphics_menu.cpp:532`, `553` | Options menu callbacks | Handle `DAORecomp.*` messages; write engine options from the render-thread hook | Graphics tab | None found |
| `DAORecomp/kernel/xboxkrnl_rtl.cpp:543`; `gpu/xenia_host.cpp:52-56`; `kernel/xam.cpp:136-146` | Title-terminate notifications run on a system exit | Ignored; every exit is an immediate TerminateProcess | Simplicity | Window closed during a save write → **L19** |
| `DAORecomp/main.cpp:303-309`, `67-73` | — | Every empty code slot holds `UnknownIndirectCall` (logs and terminates) | Crash diagnosis | `FindFunction(x) != nullptr` is always true in the code range → N |

---

## 2. Findings

Ranked by severity, then by confidence. IDs: M = medium, L = low, N = note.
Every finding was checked by a separate verifier that tried to refute it. Refuted candidates are
listed under "Checked and clean".

### M1. "VSync" off makes the game's vblank clock about 1 kHz, removing its frame cap

- **Severity:** medium. **Verdict:** confirmed. The frame cap goes in all cases. Whether
  vblank-counted timing breaks is plausible (it depends on game code).
- **Where:**
  - `xenia/src/xenia/gpu/graphics_system.cc:103-117`, `134`, `178-180`: the "GPU Frame limiter"
    thread, which is the only source of guest vblank interrupts with Xenia's renderer.
  - `graphics_system.cc:306-316` (`MarkVblank`), then `DispatchInterruptCallback`, then
    `DAORecomp/gpu/xenia_host.cpp:409-421`: the guest ISR runs via `CallGuest`.
  - `DAORecomp/settings.cpp:156` and `:300` (`[video] vsync` → `cvars::vsync`).
  - The launcher checkbox (`DAORecomp/launcher/configure_view.cpp:136`) and the in-game Graphics
    tab (`ui/graphics_menu.cpp:59`, restart-only).
  - The null command processor's own 60 Hz thread (`gpu/command_processor.cpp:390-422`) only runs
    without Xenia (`video.cpp:156-159`).
- **Mechanism:**
  - `framerate_limit` is 0 (Xenia's default; DAO never sets it). With `vsync = true` the limiter
    is forced to 60 Hz. With `vsync = false` the loop takes the `else` branch: `MarkVblank()`, then
    `Sleep(1 ms)`. With `timeBeginPeriod(1)` that is about 500–1000 guest vblank interrupts a
    second.
  - The 360's vblank is 60 Hz whatever the display does. The game's D3D queues each flip for vblank
    number `last flip + interval` (`video.cpp:258-268`) and performs it in the vblank ISR. At
    ~1 kHz, interval 2 and interval 1 both pass within a few ms, so presents are limited only by
    CPU/GPU fences. The 30 fps lock and the 60 fps clamp both stop meaning anything.
  - Anything the game counts in vblanks (BlockUntilVerticalBlank-style waits, fades or timers in
    vblank units) runs about 16× fast. The guest ISR also runs about 1000 times a second.
  - The host side is unaffected either way. The presenter always presents with interval 0 and
    allows tearing (`d3d12_presenter.cc:1231-1234`), so the checkbox does nothing a player would
    expect from "VSync" (see L21). Upstream Xenia behaves the same; we inherited it.
- **What the player would see:** with VSync off, an uncapped and uneven frame rate (hundreds of
  fps in light scenes, more heat and power). Possibly speed-ups or glitches in anything paced by
  vblank count.
- **How to confirm:**
  - Existing log: `VdSwap #N` is logged every 600 swaps (`video.cpp:219-220`). With VSync on,
    consecutive lines are at least 10 s apart (≤60 fps). With VSync off in a 3D scene, less than
    10 s apart confirms it.
  - Temporary counter at the top of `CallInterruptCallback` (`xenia_host.cpp:409`):
    ```cpp
    static std::atomic<uint32_t> s_irq[2];
    static std::atomic<uint64_t> s_irqT0;
    if (source < 2) s_irq[source]++;
    if (const uint64_t now = GetTickCount64(); now - s_irqT0 > 10000)
    { s_irqT0 = now; LOGI("TEMP irq/10s: vblank {} cp {}", s_irq[0].exchange(0), s_irq[1].exchange(0)); }
    ```
    Expect about 600 vblanks per 10 s with VSync on and 5000–10000 with VSync off.
- **Proposed fix:** keep the guest vblank at 60 Hz whatever the host does. One condition, in
  `graphics_system.cc:106-110`:
  ```diff
  -            // If VSYNC is enabled, but frames are not limited,
  -            // lock framerate at default value of 60
  -            if (normalized_framerate_limit == 0 && cvars::vsync) {
  +            // DAO: the guest's vblank is the console's 60 Hz whether the host
  +            // waits for its display or not (with vsync off Xenia ran it every
  +            // millisecond: D3D's vblank-counted flips went through at once, and
  +            // anything counted in vblanks ran ~16x fast).
  +            if (normalized_framerate_limit == 0) {
                 normalized_framerate_limit = 60;
               }
  ```
  - With `vsync = false` and the limit at 60, the loop's `!cvars::vsync && normalized_framerate_limit > 0`
    branch paces to `1e9 / 60` ns with the same deadline logic. The `else` branch becomes
    unreachable.
  - A hand-set `[xenia] framerate_limit` still changes the guest rate. Consider ignoring it, or
    logging a warning.
  - Then either relabel the option (it no longer does anything visible), or make it control host
    presentation (L21).
  - **Fidelity risk:** none. Players lose an uncapped frame rate the game was never built for.
  - **Test:** VSync off, 60 fps unlock off: swaps ≤30/s in 3D. Unlock on: ≤60/s. The temporary
    counter shows about 600 vblanks per 10 s in both VSync modes.

### M2. A signal is taken by whichever thread reaches the dispatcher lock first, not by the waiter at the moment of the signal

- **Severity:** medium. **Verdict:** the deviation is confirmed, and a standalone model of our
  dispatcher reproduces it. Harm in DAO is plausible (it depends on the game's wait patterns).
- **Where:** all in `DAORecomp/kernel/objects.cpp` unless noted.
  - `:12-15`: `DispatchWaiter` holds only a condition variable.
  - `:24-30`: `RemoveWaiter` swaps with the back element, so waiters are not kept in order.
  - `:37-41`: `NotifyDispatcher` only notifies.
  - `:111-120`: `Event::Set`. The state is a bool, and it notifies only `if (!previous)`.
  - `:122-129` `Event::Reset`; `:153-164` `Semaphore::Release`; `:203-224` `Mutant::Release`;
    `:286-291` timer `Fire`.
  - `:599-657`: `WaitMultiple` re-tests the objects only once the woken thread holds the lock.
  - Critical sections are built on this: `kernel/xboxkrnl_threading.cpp:506-558`.
- **What the 360 does:** the NT-family kernel satisfies waiters inside `KeSetEvent`,
  `KeReleaseSemaphore` and `KeReleaseMutant`, under the dispatcher lock (ReactOS
  `KxUnwaitThreadForEvent` and `KiWaitTest`).
  - Setting a synchronization event with a waiter releases that waiter, and the event stays clear.
  - Setting a notification event releases every current waiter before the call returns.
  - Semaphores and mutants are handed out first come, first served.
  - Xenia uses host Win32 events, which behave the same way.
- **What we do:** set the flag and wake every registered waiter. Each woken thread must then be
  scheduled and win `s_dispatchMutex`, which takes about 5–50 µs on an idle core and milliseconds
  on a busy one. Whoever gets there first takes the signal. Three guest patterns break:
  1. **Two Sets of an auto-reset event in quick succession.** The second finds `signaled` already
     true and does nothing (`:117`).
     - With two waiters, only one wakes.
     - With one waiter that handles one job per wake (the most likely shape: a producer sets once
       per queued job), the second job waits for the next Set. If no further Set comes, it never
       runs.
     - On the 360 the second Set is kept, because the first was already handed to the waiter.
  2. **Set, then Reset of a notification event** (a hand-made pulse). The woken waiters find the
     event clear and go back to sleep: the wake is lost. A setter that then waits for those
     workers deadlocks.
  3. **Critical section and mutant barging.**
     - `RtlLeaveCriticalSection` sets the critical section's event. The releaser re-enters within
       its 1000-pause spin (about 3–45 µs) and its `WaitSingle` consumes the still-signalled event
       before the woken waiter runs.
     - On the 360 the waiter owns the section after one Leave, and a re-entering releaser blocks.
     - The verifier proved our critical section never *loses* a wake. The harm is unfairness only:
       millisecond-scale waits, not multi-second ones.
- **Model results** (the dispatcher extracted into a standalone program; current code vs the fix
  below; 20 runs each; on the 360 every wake would be delivered):

  | Scenario | Now | With the fix |
  |---|---|---|
  | Auto-reset event, 2 waiters, Set; Set | 20 of 40 woken (23 under load) | 40 of 40 |
  | Auto-reset event, 1 waiter taking one item per wake, Set; Set | 20 of 40 consumed | 40 of 40 |
  | Manual-reset event, 3 waiters, Set; Reset | 0–1 of 60 woken (22 under load) | 60 of 60 |
  | Critical section held 50 µs in a loop, other thread waiting | owner re-takes it up to 10× (61× ≈ 3 ms on saturated cores) | at most 1–2 |
  | 4 threads on one critical section, 20 µs hold | 17k ops/s, uneven | 27–28k ops/s, even |

- **What the player would see:** a job that sometimes runs a frame (or a whole period) late:
  streaming, AI or audio work. Pattern 2 gives a freeze that clears "when something else happens".
  Pattern 3 gives millisecond hitches under critical-section contention. All of it is worse on a
  busy PC or the Steam Deck, where woken threads take longer to run.
- **How to confirm, before applying the fix:** temporary code in `objects.cpp`. The dispatcher
  lock is held at every call site, so the static map is safe.
  ```cpp
  // TEMP M2: signals the 360 would deliver that we lose, or that a newcomer takes.
  static void M2Note(const char* what, const KernelObject& object)
  {
      static std::unordered_map<const char*, uint32_t> counts;
      const uint32_t n = ++counts[what];
      if (n <= 20 || (n & (n - 1)) == 0)
      {
          const GuestThread* t = GuestThread::Current();
          LOGI("TEMP M2 {} #{}: object {:08X} ({} waiters) thread {} in {} lr {:08X}", what, n, object.guestObject,
              object.waiters.size(), t ? t->threadId : 0, t && t->lastImport ? t->lastImport : "-",
              t ? t->lastImportLr : 0);
      }
  }
  static bool Consumes(const KernelObject& o)
  {
      return (o.type == ObjectType::Event && !static_cast<const Event&>(o).manualReset) || o.type == ObjectType::Semaphore
          || o.type == ObjectType::Mutant || (o.type == ObjectType::Timer && !static_cast<const Timer&>(o).manualReset);
  }
  // Event::Set, before `signaled = true;`:
  //     if (previous && !manualReset && !waiters.empty()) M2Note("auto-reset Set lost", *this);
  // Event::Reset, before `signaled = false;`:
  //     if (signaled && !waiters.empty()) M2Note("Reset before woken waiters ran", *this);
  // WaitMultiple: `uint32_t wakes = 0;` before the loop, `wakes++;` after each cv wait / wait_until;
  // in the wait-any branch, inside `if (objects[i]->IsSignaled(self))`, before Acquire:
  //     if (!registration.registered && !objects[i]->waiters.empty() && Consumes(*objects[i]))
  //         M2Note("taken ahead of woken waiters", *objects[i]);
  //     if (wakes > 1) M2Note("woken more than once before taking it", *objects[i]);
  ```
  - Play 10–15 minutes: start, load a save, a conversation, combat, menus. Then grep `dao.log` for
    `TEMP M2`.
  - `auto-reset Set lost` or `Reset before…` means the lost-wake patterns occur. The `lr` names the
    caller of KeSetEvent or RtlLeaveCriticalSection.
  - `taken ahead` or `woken more than once` with `RtlEnterCriticalSection` is barging.
  - All counts zero means the fix is low priority, though still correct.
- **Proposed fix:** satisfy wait-any waits at signal time, in order, as KiWaitTest does.
  - Wait-all waits are still only woken to check again, which is also what NT does.
  - Thread objects, XAM listeners and APC wakeups keep `NotifyDispatcher`.
  - The patch applies cleanly to `0de61f4` (`git apply --check`). It is the full change to
    `DAORecomp/kernel/objects.cpp`:
  ```diff
  --- a/DAORecomp/kernel/objects.cpp
  +++ b/DAORecomp/kernel/objects.cpp
  @@ -7,11 +7,18 @@
   {
       static std::mutex s_dispatchMutex;
   
  -    // A thread's wakeup. Notified with the dispatcher lock held: the thread only
  -    // leaves its wait, and so its registrations, with the lock.
  +    // A thread's wakeup, and its wait. Used with the dispatcher lock held: the
  +    // thread only leaves its wait, and so its registrations, with the lock.
       struct DispatchWaiter
       {
           std::condition_variable cv;
  +        // Set by WaitMultiple while registered. A wait for any one object is
  +        // satisfied by the thread that signals the object (SignalWaiters).
  +        std::span<const ObjectRef> objects;
  +        GuestThread* thread = nullptr;
  +        bool waitAny = false;
  +        bool satisfied = false;
  +        uint32_t status = 0;
       };
   
       static thread_local DispatchWaiter t_waiter;
  @@ -23,10 +30,9 @@
   
       static void RemoveWaiter(KernelObject& object)
       {
  +        // In order: waits are satisfied first come, first served.
           auto& waiters = object.waiters;
  -        const auto it = std::find(waiters.begin(), waiters.end(), &t_waiter);
  -        *it = waiters.back();
  -        waiters.pop_back();
  +        waiters.erase(std::find(waiters.begin(), waiters.end(), &t_waiter));
       }
   
       std::unique_lock<std::mutex> LockDispatcher()
  @@ -47,6 +53,38 @@
           RemoveWaiter(object);
       }
   
  +    // Called with the dispatcher lock held when an event, semaphore, mutant or
  +    // timer has become signaled. As the console's kernel does (KiWaitTest), the
  +    // waits for any one object that include it are satisfied here, in the order
  +    // they started, while it stays signaled: the woken thread returns already
  +    // owning the object. (Only waking them let whichever thread took the lock
  +    // first have it: a second Set of an auto-reset event, or a Reset after a
  +    // Set, before the woken thread ran was lost, and a thread leaving a critical
  +    // section could take it back before the waiter it was handed to.)
  +    // Waits for all of several objects are only woken to check again, as on NT.
  +    static void SignalWaiters(KernelObject& object)
  +    {
  +        for (DispatchWaiter* waiter : object.waiters)
  +        {
  +            if (waiter->satisfied || !object.IsSignaled(waiter->thread))
  +                continue;
  +            if (waiter->waitAny)
  +            {
  +                const auto& objects = waiter->objects;
  +                const auto it = std::find_if(objects.begin(), objects.end(),
  +                    [&](const ObjectRef& o) { return o.get() == &object; });
  +                if (it != objects.end())
  +                {
  +                    object.Acquire(waiter->thread);
  +                    waiter->status = uint32_t(X_STATUS_WAIT_0 + (it - objects.begin())) + object.WaitStatusAdjust();
  +                    waiter->satisfied = true;
  +                    object.SyncGuest();
  +                }
  +            }
  +            waiter->cv.notify_one();
  +        }
  +    }
  +
       // ------------------------------------------------------------------- time
   
       // FILETIME epoch (1601) offset from the Unix epoch, in 100 ns.
  @@ -113,9 +151,9 @@
           auto lock = LockDispatcher();
           const int32_t previous = signaled;
           signaled = true;
  -        SyncGuest();
           if (!previous)
  -            NotifyDispatcher(*this);
  +            SignalWaiters(*this);
  +        SyncGuest();
           return previous;
       }
   
  @@ -130,15 +168,14 @@
   
       int32_t Event::Pulse()
       {
  -        // Wake current waiters without leaving the event set: approximate by
  -        // setting and letting one dispatcher pass consume it.
  +        // Satisfies the waits there are now and leaves the event clear.
           auto lock = LockDispatcher();
           const int32_t previous = signaled;
  -        signaled = true;
  -        NotifyDispatcher(*this);
  -        lock.unlock();
  -        std::this_thread::yield();
  -        lock.lock();
  +        if (!previous)
  +        {
  +            signaled = true;
  +            SignalWaiters(*this);
  +        }
           signaled = false;
           SyncGuest();
           return previous;
  @@ -157,9 +194,9 @@
           if (int64_t(count) + adjustment > limit)
               return -1;
           count += adjustment;
  -        SyncGuest();
           if (adjustment > 0)
  -            NotifyDispatcher(*this);
  +            SignalWaiters(*this);
  +        SyncGuest();
           return previous;
       }
   
  @@ -217,9 +254,9 @@
           }
           if (recursion == 0)
               owner = nullptr;
  -        SyncGuest();
           if (!owner)
  -            NotifyDispatcher(*this);
  +            SignalWaiters(*this);
  +        SyncGuest();
           return previous;
       }
   
  @@ -287,8 +324,8 @@
               if (timer->generation != entry.generation)
                   return;
               timer->signaled = true;
  +            SignalWaiters(*timer);
               timer->SyncGuest();
  -            NotifyDispatcher(*timer);
               if (timer->apcRoutine)
               {
                   if (auto thread = timer->apcThread.lock())
  @@ -569,12 +606,18 @@
           {
               std::span<const ObjectRef> objects;
               GuestThread* apcThread;
  +            GuestThread* thread;
  +            bool waitAny;
               bool registered = false;
   
               void Add()
               {
                   if (registered)
                       return;
  +                t_waiter.objects = objects;
  +                t_waiter.thread = thread;
  +                t_waiter.waitAny = waitAny;
  +                t_waiter.satisfied = false;
                   for (const auto& object : objects)
                       AddWaiter(*object);
                   if (apcThread)
  @@ -591,13 +634,24 @@
                       RemoveWaiter(*object);
                   if (apcThread)
                       RemoveWaiter(*apcThread);
  +                t_waiter.objects = {};
  +                t_waiter.waitAny = false;
  +                t_waiter.satisfied = false;
               }
   
               ~Registration() { Remove(); }
  -        } registration{ objects, alertable ? self : nullptr };
  +        } registration{ objects, alertable ? self : nullptr, self, !waitAll };
   
           for (;;)
           {
  +            // Satisfied by the thread that signaled an object (first: the
  +            // object is already taken for this thread).
  +            if (t_waiter.satisfied)
  +            {
  +                const uint32_t status = t_waiter.status;
  +                return status;
  +            }
  +
               if (alertable && self && !self->apcQueue.empty())
               {
                   registration.Remove();
  @@ -644,7 +698,7 @@
               {
                   t_waiter.cv.wait(lock);
               }
  -            else if (t_waiter.cv.wait_until(lock, deadline) == std::cv_status::timeout)
  +            else if (t_waiter.cv.wait_until(lock, deadline) == std::cv_status::timeout && !t_waiter.satisfied)
               {
                   // Satisfied at the deadline: taken on the next pass.
                   bool satisfied = waitAll;
  ```
  The verifier checked these points:
  - A waiter registered on several objects is granted once (`satisfied` is tested first).
  - The granted status is returned before pending APCs are looked at, so a taken object is never
    dropped for USER_APC.
  - A timeout racing a grant returns the grant, as NT does.
  - Mutants are tested per waiter, and abandonment comes through `WaitStatusAdjust`.
  - The waiter lists aren't modified while they are iterated.
  - The suspension wait (`thread.cpp:305`) and `DelayExecution` registrations have `waitAny=false`
    and are only woken.

  **Fidelity risk:** moderate, because every wait goes through this code.
  - A contended critical-section Leave now always switches to the waiter, as on the 360. A very
    hot critical section could convoy: more context switches. The model showed higher, evenly
    shared throughput, but Windows wake latency differs.
  - No game code can depend on the old stealing, since the 360 never did it.
- **How to test:**
  1. Build the standalone model (Appendix A) on Windows: `clang-cl /std:c++20 /O2 /EHsc harness.cpp`,
     then again with `/DNEW`. The fix should give 40, 40 and 60 in S1–S3, at most 1–2 in S4, and
     `bad 0` in S5, with S6 throughput no lower.
  2. In game, keep the M2 diagnostics: "auto-reset Set lost" and "taken ahead" should drop to 0.
  3. `DAO_THREAD_STATS=5` and `DAO_FRAME_HISTORY=1` in fixed scenes (title idle, camp, a
     conversation, combat), plus a stopwatch on a save load, before and after. Swaps/s doubling
     between two threads means a convoy on a hot critical section; the "woken more than once" log
     names it.
  4. Repeat the held-shot conversation test (it uses `ShortSleep`, so it is unaffected) and a Steam
     Deck run.

### M3. With no audio output, the game's audio engine stops for good

- **Severity:** medium. **Verdict:** confirmed for `DAO_AUDIO=null` and for any PC where SDL
  can't open a playback device. Mostly refuted for headphone/HDMI/Bluetooth switches: SDL3 keeps
  pulling from a lost device ("zombie" device) and migrates default-device streams, so there is
  only a short gap. Whether a specific game wait then blocks for good is plausible.
- **Where:**
  - `DAORecomp/apu/audio.cpp:49-52`: limits (skip at 8 queued frames, drop beyond 16).
  - `:93-106` `RunClientCallbacks`, which skips at `:96-100`.
  - `:632-642` `PopFrame`, the only drain, called only from SDL's `FeedDevice`
    (`audio_output.cpp:37-74`).
  - `:744-747`: drop-oldest in Submit.
  - `DAORecomp/main.cpp:332-334`: `DAO_AUDIO=null`; `InitializeAudioOutput()`'s failure is ignored.
  - `audio_output.cpp:77-101`: device open, which fails at `:79-83` and `:86-90`.
- **Mechanism:**
  - The worker calls the guest's render callback only while fewer than 8 frames are queued. With
    no consumer, the queue fills about 43 ms after the client registers, and the callback is
    never called again.
  - On the 360 the callback fires every 5.33 ms whatever the output. Xenia's no-audio driver
    acknowledges each frame at once, so its guest keeps running.
  - The game imports only `XAudioRegisterRenderDriverClient` / `XAudioSubmitRenderDriverFrame`, so
    that callback is the statically linked XAudio2 engine's whole processing pass: voice
    processing, the XMA kicks (`XMAEnableContext`), `OnBufferEnd` / `OnStreamEnd`, and voice
    positions.
  - With no callback, no voice ever finishes and no XMA is decoded.
- **What the player would see:** on a PC without a working output device, a conversation that
  never advances past a voiced line, or a loading screen that never ends (wherever the game waits
  for a sound). The owner's own runs with `DAO_AUDIO=null` can show hangs that don't exist with
  audio.
- **How to confirm:**
  - Run with `DAO_AUDIO=null`. The log shows `XAudio: frame queue full (8 frames), skipping render callbacks until it drains`
    about 43 ms after "XAudio render worker started", and nothing after.
  - Start a voiced conversation. Lines that don't auto-advance, or a load that never ends, confirm
    the impact.
  - In players' logs, look for "Audio output: can't open the playback device".
  - For device switches: add a temporary count of consecutive skips with a `LOGW` at 200 (about 1 s)
    and unplug headphones mid-game. It should not fire.
- **Proposed fix:** call the guest whenever the host consumer isn't alive. Submit's drop-oldest
  (16 frames) already bounds memory.
  ```cpp
  // audio.cpp, anonymous namespace
  std::atomic<int64_t> g_lastPull{ 0 }; // steady_clock ticks of the host's last PopFrame call; 0 = never
  bool HostConsumerLive()
  {
      const int64_t last = g_lastPull.load(std::memory_order_relaxed);
      return last && std::chrono::steady_clock::now().time_since_epoch().count() - last <
          std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(250)).count();
  }
  // RunClientCallbacks: skip only while a consumer is draining the queue
  if (HostConsumerLive() && QueuedFrames() >= kWorkerQueueLimit) { LogQueueFullOnce(); return; }
  // PopFrame, first line (every call, also when the queue is empty):
  g_lastPull.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
  ```
  - Optionally clear `g_frames` when the consumer comes back, to avoid about 85 ms of stale audio.
  - **Fidelity risk:** none; this is what the 360 does.
  - **Test:** under `DAO_AUDIO=null`, a temporary per-second count of callbacks shows about 187.
    The underrun count in a normal session is unchanged.

### M4. A file read into GPU-watched memory fails (Windows) or comes back short (Wine) when the GPU re-protects a page during the read

- **Severity:** medium (rare, but a hard exit or silent corruption). **Verdict:** the mechanism
  is confirmed. How often it happens is plausible: it depends on the game reading into pages next
  to resources being drawn.
- **Where:**
  - `DAORecomp/kernel/xboxkrnl_io.cpp:245-256`: `PrepareGuestWrite`, then `ReadFile` with no lock
    held, and any error becomes `X_STATUS_UNSUCCESSFUL`.
  - `:257-261`: only `read == 0` is treated specially.
  - `DAORecomp/gpu/xenia_memory.cpp:282-288`: `PrepareGuestWrite` takes and then releases
    `global_critical_region`.
  - Watches are re-armed by the GPU thread (`ProtectPhysicalPages`, `:95-113`, all three views)
    from `xenia/src/xenia/gpu/d3d12/d3d12_shared_memory.cc:365` (armed before the upload copy),
    `xenia/src/xenia/gpu/shared_memory.cc:299`, `:326-367` and `primitive_processor.cc:1508`.
  - `DAORecomp/kernel/xam.cpp:1083-1088`: `XamShowDirtyDiscErrorUI` calls TerminateProcess.
- **Mechanism:**
  - `PrepareGuestWrite` invalidates and unprotects the target pages, then releases the lock.
    During the whole read (milliseconds on a cold cache or an SD card), the GPU thread can upload
    a resource sharing one of those 4 KB pages and write-protect it again.
  - **Windows:** a kernel-mode copy into a read-only user page doesn't raise a user-mode
    exception, so our vectored handler (`main.cpp:84-87`) never sees it. `ReadFile` fails with
    `ERROR_NOACCESS` (998), and the game gets `X_STATUS_UNSUCCESSFUL`. Xenia hit exactly this and
    reads through an unprotected mapping, invalidating after the copy (its `xfile.cc:127-136`
    comment).
  - **Wine/Proton:**
    - If the faulting page is not the first, `pread` returns a short count and Wine reports
      success (`virtual_locked_pread`). `ReadAt` returns SUCCESS with fewer bytes: silently
      truncated data, with no log line at all.
    - If the first page faults, the result is 998, as on Windows.
  - The 360 never fails such a read.
- **What the player would see:**
  - **Windows:** a read error. If the game treats it as a damaged disc, the game exits.
    Otherwise a missing asset or a stalled load.
  - **Steam Deck:** random corrupt data in a loaded asset: a garbled texture or mesh, or a crash
    later.
- **How to confirm:**
  - Windows: search existing logs for `ReadFile '...' failed (998)`, especially just before
    `XamShowDirtyDiscErrorUI - a read failed`.
  - Wine: temporarily log in `ReadAt` when `read < length` and the end of file wasn't reached.
  - Exposure: temporarily count reads whose guest address is `>= 0xA0000000`. At 0 the bug can't
    occur.
- **Proposed fix:** read into host memory, then copy in user mode, where a re-armed page faults
  into the handler as usual. Replace `xboxkrnl_io.cpp:241-256` with:
  ```cpp
  DWORD read = 0, error = ERROR_SUCCESS;
  const uint32_t guest = g_memory.ToGuest(buffer);
  auto readHost = [&](uint8_t* to, uint32_t want, uint64_t at, DWORD& got) {
      OVERLAPPED ov{};
      ov.Offset = DWORD(at);
      ov.OffsetHigh = DWORD(at >> 32);
      got = 0;
      if (ReadFile(file.host, to, want, &got, &ov))
          return DWORD(ERROR_SUCCESS);
      const DWORD e = GetLastError(); // before anything else can overwrite it
      return e == ERROR_HANDLE_EOF ? DWORD(ERROR_SUCCESS) : e;
  };
  if (guest >= 0xA0000000 && length)
  {
      // The GPU can write-protect these pages again at any moment (from its own
      // thread); a kernel copy into one fails (ERROR_NOACCESS; a short read under
      // Wine) instead of faulting into HandleWatchedWrite. Read into host memory and
      // copy in user mode, where such a fault is handled.
      constexpr uint32_t kChunk = 1u << 20;
      thread_local std::unique_ptr<uint8_t[]> bounce(new uint8_t[kChunk]);
      while (read < length)
      {
          const uint32_t want = std::min(kChunk, length - read);
          DWORD got;
          if ((error = readHost(bounce.get(), want, offset + read, got)) != ERROR_SUCCESS)
              break;
          dao::gpu::PrepareGuestWrite(guest + read, got);
          memcpy(buffer + read, bounce.get(), got);
          read += got;
          if (got < want)
              break;
      }
  }
  else
      error = readHost(buffer, length, offset, read);
  if (error != ERROR_SUCCESS)
  {
      LOGW("ReadFile '{}' failed ({})", file.guestPath, error);
      bytesRead = 0;
      return X_STATUS_UNSUCCESSFUL;
  }
  ```
  - Keep the existing tail (`bytesRead = read`, position, END_OF_FILE).
  - The extra copy is about 100 µs per MB, small next to disk time.
  - **Fidelity risk:** none.
  - **Test:** no 998 errors and no short reads in a long session with a cold file cache
    (after a reboot) and on the Deck.

### M5. Each rewritten 4 KB page of GPU-cached memory costs an access fault and three `VirtualProtect` calls

- **Severity:** medium (performance; Steam Deck and Wine most). **Verdict:** confirmed as a
  deviation from upstream Xenia. The size of the cost depends on how much watched memory the game
  rewrites per frame. No correctness problem.
- **Where:**
  - `DAORecomp/gpu/xenia_memory.cpp:218-221`: the callbacks' returned ranges are discarded.
  - `:223-244`: only `[first, last]` is unwatched.
  - `:266`: the fault path passes length 1 with `unwatch_exact_range=false`.
  - `:95-113`: three views, so three `VirtualProtect` calls.
- **Mechanism:**
  - On a fault the callbacks already widen their own invalidation:
    - `SharedMemory::MemoryInvalidationCallback` drops the valid bits of the whole 64-page
      (256 KB) block up to GPU-written pages (`shared_memory.cc:575-616`).
    - `PrimitiveProcessor` drops whole 256 KB buckets (`primitive_processor.cc:1581-1588`).
  - Both return that range. Upstream `PhysicalHeap::TriggerCallbacks` unprotects the common part
    (capped at 4 MB) because "it's a lot cheaper to upload some excess data than to catch access
    violations".
  - We unprotect only the faulting page. So we pay both the re-upload of up to 256 KB and one more
    fault, with three more `VirtualProtect` calls, for every further page written.
  - Under Wine each fault is a signal round trip plus three `mprotect` calls, about 10–30 µs.
    1 MB of dynamic data rewritten per frame (vertices, UI, particles) is about 256 faults, about
    3–8 ms of game-thread CPU per frame on the Deck. The real hardware has no such cost.
- **What the player would see:** a lower CPU-bound frame rate, worst on the Steam Deck, and spikes
  when large buffers are refilled.
- **How to confirm:**
  - Add a temporary `static std::atomic<uint64_t> s_faults, s_faultTicks;` in
    `Memory::HandleAccessViolation`: count the calls where `TriggerPhysicalMemoryCallbacks` returns
    true, and add the QPC ticks spent.
  - Log faults per frame and µs per frame every 600 swaps from the VdSwap hook (`video.cpp:218`).
  - More than about 100 faults per frame in towns or combat makes the fix worthwhile. Fewer than
    about 20 shows no practical impact. Under Wine the real cost is several times the measured
    handler time, because signal delivery isn't counted.
- **Proposed fix:** replace `xenia_memory.cpp:218-244` (variables as in that function):
  ```cpp
          const uint32_t start = first << kPageShift;
          const uint32_t size = (last - first + 1) << kPageShift;
          if (!unprotect)
              unwatch_exact_range = true; // nothing to save by widening
          // Each callback returns the range it holds nothing in any more (it may have
          // widened its own invalidation); unwatch their common part, at most a 4 MB
          // window around the touched pages, as Xenia's PhysicalHeap::TriggerCallbacks
          // does: re-uploading a little extra is cheaper than a fault per page.
          uint32_t keepFirst = 0, keepLast = UINT32_MAX; // bytes, inclusive
          for (auto* callback : state_->callbacks)
          {
              const auto range = callback->first(callback->second, start, size, unwatch_exact_range);
              if (!unwatch_exact_range)
              {
                  keepFirst = std::max(keepFirst, range.first);
                  const uint32_t extra = std::max(range.second, 1u) - 1;
                  keepLast = std::min(keepLast, extra > UINT32_MAX - range.first ? UINT32_MAX : range.first + extra);
              }
          }
          uint32_t unwatchFirst = first, unwatchLast = last;
          if (!unwatch_exact_range)
          {
              constexpr uint32_t kMaxExcess = 4u << 20;
              const uint32_t endByte = start + size - 1;
              keepFirst = std::max(std::min(keepFirst, start), start & ~(kMaxExcess - 1));
              keepLast = std::min({ std::max(keepLast, endByte), endByte | (kMaxExcess - 1), kPhysicalSize - 1 });
              unwatchFirst = keepFirst >> kPageShift;
              unwatchLast = keepLast >> kPageShift;
          }
          if (unprotect)
          {
              uint32_t runStart = UINT32_MAX;
              for (uint32_t page = unwatchFirst; page <= unwatchLast + 1; page++)
              {
                  const bool watched = page <= unwatchLast && state_->IsWatched(page);
                  // ... the existing loop body, unchanged
              }
          }
          return true;
  ```
  - This is safe only while every callback's returned range contains the touched range and
    everything in it is really invalidated. Both current callbacks satisfy this; upstream makes
    the same assumption.
  - Apply L4's one-line fix first, so the primitive processor really drops what its returned range
    claims.
  - Only pages that are already watched (and so were armed only because they were writable) get
    unprotected.
  - **Fidelity risk:** none if the invariant above holds.
  - **Test:** the fault counter before and after; `DAO_FRAME_HISTORY=1` for flicker; an A/B with
    `DAO_GPU_INVALIDATE_ALL` for stale geometry or textures.

### M6. With VSync on (the default), the GPU command thread polls the per-frame flip wait every 1–2 ms

- **Severity:** medium (frame time, every frame, default settings). **Verdict:** confirmed that
  the code path does this. How much it costs is plausible: it depends on the `wait` value the
  game's D3D puts in the packet, which we can't see.
- **Where:**
  - `xenia/src/xenia/gpu/pm4_command_processor_implement.h:744-769`.
  - The vsync default: `DAORecomp/settings.h:47`, pushed at `settings.cpp:300`.
  - `xenia/src/xenia/base/threading_win.cc:164-170` (`::Sleep(ms)`); `main.cpp:293`
    (`timeBeginPeriod(1)`).
- **Mechanism:**
  - When a `WAIT_REG_MEM` doesn't match and `wait >= 0x100`, the command processor submits the
    pending host work (`PrepareForWait`). With VSync on it then sleeps `wait / 0x100` ms, which is
    at least `::Sleep(1)`: 1–2 ms per poll.
  - The DAO polling (yield for about 1 ms, then 100 µs sleeps) runs only with VSync **off**.
  - With `wait < 0x100` the loop spins hot with no yield (the empty `else` at `:768-769`). That is
    upstream behaviour, and a power problem rather than a latency one.
  - The per-frame flip wait (`docs/x360-recomp-notes.md`, "Submitting before waiting") is released
    by the vblank ISR on the vblank thread. The command processor notices 0–2 ms later (about 1 ms
    on average). Only then does it process the swap and the next frame's commands.
  - Real hardware re-polls every `wait` GPU clocks, a few microseconds.
  - The frame's draws are already submitted, so this isn't GPU idle time. It is latency on the
    command processor's critical path, and it matters when the CP or render thread is near the
    16.7 ms budget: 60 fps on the Steam Deck.
  - If D3D uses `wait >= 0x1000`, each poll sleeps 16 ms or more, which would be serious.
- **What the player would see:** missed 60 fps frames in heavy scenes on the Deck: stutter
  toward 45 fps, where the same scene would hold 60 with the fix.
- **How to confirm:**
  1. `DAO_THREAD_STATS=5`. If the "GPU Commands" thread is near 100% of a core, the flip wait uses
     `wait < 0x100` (hot spin). This finding's latency claim is then refuted, and the spin is a
     power problem instead.
  2. Temporary instrumentation in `WAIT_REG_MEM`: on exit, if it ever didn't match, add the count
     and the µs spent per `(poll_reg_addr, wait, is_memory)`, and dump the table every 600 swaps.
     It shows the real `wait` values and the time lost.
  3. A/B the fix below against frame time on the Deck at 60 fps.
- **Proposed fix:** make the polling independent of presentation vsync. Lines 746–762 (from
  `if (!cvars::vsync) {` to the end of its `else` block, after `PrepareForWait();`) become:
  ```cpp
          {
            // DAO: polls, but after a short while in brief sleeps, so the core is
            // free for the guest's threads during waits of most of a frame. (Not
            // Sleep(wait / 0x100 ms): the console re-polls every `wait` GPU clocks,
            // and a 1-2 ms sleep made the per-frame flip wait end that much late.)
            const uint64_t now = Clock::QueryHostUptimeMillis();
            if (!poll_start) {
              poll_start = now + 1;
            }
            if (now >= poll_start) {
              xe::dao_host::ShortSleep(100);
            } else {
              xe::threading::MaybeYield();
            }
          }
  ```
  - `ReturnFromWait()` is an empty base method with no D3D12 override, so dropping it changes
    nothing.
  - Optionally give the `wait < 0x100` branch a `MaybeYield()`.
  - **Fidelity risk:** none; this is closer to the hardware's poll rate.
  - **Cost:** about 1 ms of yielding, then 100 µs wakes during frame-long waits.
  - **Test:** the WAIT_REG_MEM table's µs per frame drops; frame time on the Deck at 60 fps.

---

### L1. The Sleep(0) throttle also slows loops that do real work between yields

- **Severity:** low–medium. **Verdict:** confirmed in our code. Harm is plausible: it depends on
  whether the game has such loops.
- **Where:** `DAORecomp/kernel/objects.cpp:704-731` (`kPollGap` 50 µs, `kPollsBeforeSleeping` 64);
  `ShortSleep` at `:665-697`.
- **Mechanism:**
  - The gap is measured from the end of the previous `Sleep(0)` to the start of this one. That
    makes it the guest's work between calls, which is correct for telling polls from work.
  - But 50 µs is generous. A loop doing 1–49 µs of real work per item and yielding with `Sleep(0)`
    ("be nice" worker or streaming loops) counts as polling. After 64 iterations (about 3 ms)
    every yield becomes a 100 µs+ sleep (high-resolution timer on Windows, which typically lands
    at 0.1–0.6 ms; about 150–200 µs under Wine).
  - That makes a 20 µs-per-item loop 5–7× slower and a 49 µs one about 3×.
  - A pure poll's gap (a guest load and compare, the XAPI thunk, `NoteImport`) is about 0.2–2 µs.
  - The same mechanism produced the held-shot freeze. It is listed separately because it slows
    work rather than missing flags.
- **What the player would see:** slower loading or streaming, or per-frame latency where the game
  hands work between threads with `Sleep(0)`.
- **How to confirm:** a temporary per-thread gap histogram in `DelayExecution`'s `*timeout == 0`
  branch, after `now` is computed:
  ```cpp
  thread_local uint32_t hist[6], sleeps; thread_local auto lastLog = now;
  using namespace std::chrono_literals;
  const auto gap = now - lastReturn;
  hist[gap < 2us ? 0 : gap < 5us ? 1 : gap < 10us ? 2 : gap < 20us ? 3 : gap < 50us ? 4 : 5]++;
  if (polls >= kPollsBeforeSleeping) sleeps++;
  if (now - lastLog > 10s)
  {
      auto* t = GuestThread::Current();
      LOGI("TEMP Sleep0 thread {} <2:{} <5:{} <10:{} <20:{} <50:{} >=50:{} sleeps {}", t ? t->threadId : 0,
          hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], sleeps);
      std::fill(std::begin(hist), std::end(hist), 0); sleeps = 0; lastLog = now;
  }
  ```
  - A thread with many sleeps and most of its gaps in the 10–50 µs buckets has a work-and-yield
    loop that the throttle is slowing.
  - The update thread's gaps (the 25% CPU case) should be under 2–5 µs. If so, the fix below keeps
    its saving.
  - Also log `ShortSleep`'s measured length once, to learn its real duration on the owner's PC and
    on the Deck.
- **Proposed fix:** `constexpr auto kPollGap = std::chrono::microseconds(10);` (`objects.cpp:720`).
  - **Fidelity risk:** low. Only unusual poll loops whose body takes over 10 µs keep yielding at
    full rate, costing more CPU.
  - **Test:** the histogram before and after, `DAO_THREAD_STATS` CPU% of the update thread (it
    should stay near 25%), and a held-shot conversation.

### L2. `NtQueryDirectoryFile` takes the search pattern from every call, so `FindNextFile` returns everything

- **Severity:** low–medium. **Verdict:** the deviation is confirmed. Harm is plausible, and
  deterministic if it applies.
- **Where:**
  - `DAORecomp/kernel/xboxkrnl_io.cpp:660`: pattern read per call.
  - `:662-686`: the entry list is built once per handle.
  - `:692`: filter; `:618`: `WildcardMatch("")` is true.
  - `:720`: NO_MORE_FILES even when the first scan found nothing.
  - `DAORecomp/kernel/vfs.h:58-61`: the enumeration state has no pattern field.
- **Mechanism:**
  - XAPI `FindFirstFile` passes the mask. `FindNextFile` calls `NtQueryDirectoryFile` with
    `FileName = NULL` and no restart; NT and the 360 keep the mask from the first call.
  - Xenia does the same (`xfile.cc:56-77` keeps `find_engine_`), and returns `NO_SUCH_FILE` when
    the first scan matches nothing.
  - We match each entry against the current call's pattern. NULL becomes `""`, which matches
    everything. So `FindFirstFile("dir\\*.erf")` returns the first `.erf`, and every
    `FindNextFile` after it returns every remaining entry: other types, subfolders, mod overlay
    files.
  - A first scan with no match gives `ERROR_NO_MORE_FILES` instead of `ERROR_FILE_NOT_FOUND`.
- **What the player would see:** the game trying to load files of the wrong type from an
  enumerated folder (packages, DLC, the override folder): load errors or skipped content, more
  likely with mods installed.
- **How to confirm:** temporary
  `LOGI("TEMP QDF '{}' pat '{}' restart {} -> '{}'", file->guestPath, pattern, restart, name);`
  just before the `CompleteIo` at `:716`.
  - Confirmed if a handle's first call has a mask such as `*.erf` and later calls (empty pattern)
    return names that don't match it.
  - Ruled out if every mask is `*` or `*.*`, or the game passes the mask on every call.
- **Proposed fix:** keep the pattern from the first call, as NT does. Don't copy Xenia's "restart
  on every non-empty name": a guest that passes the same mask each time would loop on the first
  entry forever.
  ```cpp
  // vfs.h, File:
  std::string searchPattern;
  bool returnedAny = false;

  // NtQueryDirectoryFile, replacing `const std::string pattern = AnsiString(fileName);`
  // and the lock below it:
  std::lock_guard lock(file->mutex);
  if (const std::string requested = AnsiString(fileName);
      !requested.empty() && (restart || !file->enumerated || file->searchPattern.empty()))
      file->searchPattern = requested; // XAPI passes it only on FindFirstFile; later NULLs keep it
  const std::string& pattern = file->searchPattern;
  // in the `if (restart || !file->enumerated)` rebuild: file->returnedAny = false;
  // when an entry is returned (before CompleteIo): file->returnedAny = true;
  // at the end, instead of NO_MORE_FILES:
  const uint32_t status = file->returnedAny ? X_STATUS_NO_MORE_FILES : X_STATUS_NO_SUCH_FILE;
  ```
  - **Fidelity risk:** low.
  - **Test:** the temporary log with a mask, and the game's DLC and mod listings unchanged.

### L3. `XMAInitializeContext` keeps the previous sound's decoder state

- **Severity:** low. **Verdict:** the deviation is confirmed. The audible effect is plausible.
- **Where:** `DAORecomp/apu/audio.cpp:797-805`. Its comment at `:801`, "kept across a
  re-Initialize, as Xenia does", is wrong. The reset function is `xma_decoder.cpp:316-322`.
- **Mechanism:**
  - Xenia's `XMAInitializeContext` fills the context, then writes the Clear register (0x1A80,
    `Context0Clear`). `XmaContextNew::ClearLocked` then:
    - clears the valid bits and the output offsets, and sets the read offset to 32;
    - sets `current_frame_remaining_subframes_ = 0`, `loop_frame_output_limit_ = 0` and
      `loop_start_skip_pending_ = false`.
  - We keep those three host fields. When XAudio2 reuses a voice's context for a new sound after
    a pass that ended mid-frame, the first pass writes up to 3 (mono) or 7 (stereo) leftover
    subframes of the old sound. That is roughly 8–19 ms, or a click. A stale loop-skip trims the
    new sound's start instead.
- **What the player would see:** clicks or snippets of the previous sound at the start of effects
  or voice lines.
- **How to confirm:** add a temporary accessor
  `uint8_t PendingSubframes() const { return m_currentFrameRemaining; }` and
  `LOGI("TEMP XMA init with {} pending subframes", ...)` in Initialize when it is non-zero.
  `DAO_AUDIO_DUMP=x.wav` lets you listen.
- **Proposed fix:** in the `if (slot)` block, add `ResetSlotDecoders(*slot);` (the slot mutex is
  already held), and correct the comment.
  - Keep honouring `init->inputReadOffset`. Xenia's forced 32 is a side effect of its Clear, and
    the hardware's behaviour is unknown.
  - **Fidelity risk:** if the game re-initialises for a gapless continuation of the same stream,
    those subframes are dropped, exactly as in Xenia.

### L4. The primitive processor's cache keeps entries that lie wholly inside an invalidated range

- **Severity:** low–medium. **Verdict:** the code bug is confirmed (inherited from upstream
  Xenia). A visible effect is plausible.
- **Where:** `xenia/src/xenia/gpu/primitive_processor.cc:1666-1668`:
  ```cpp
  if (entry_key.base < physical_address_end) {
    uint32_t entry_end = entry_key.base + entry_key.GetSizeBytes();
    if (entry_end > physical_address_end) {   // should be > physical_address_start
  ```
- **Mechanism:**
  - Only entries that straddle the *end* of the invalidated range are dropped. An entry fully
    inside the written range (or inside the 256 KB-aligned range on a fault) survives.
  - Its pages are unwatched afterwards, so the next draw with the same key reuses stale converted
    indices.
  - This affects draws with at least 4096 indices (`primitive_processor_cache_min_indices`) that
    need host conversion: triangle fans, line loops, quads, a 16-bit primitive reset other than
    0xFFFF, or 32-bit indices with reset. It also needs the game to rewrite that index buffer in
    place with the same base, count and format.
- **What the player would see:** garbled or wrong geometry on such dynamic meshes, intermittent.
- **How to confirm:** set `primitive_processor_cache_min_indices` to -1 (`[xenia]` in config.toml,
  or the cvar default at `primitive_processor.cc:60`), which disables the cache. If an
  intermittent geometry glitch disappears, this is it.
- **Proposed fix:** `:1668` becomes `if (entry_end > physical_address_start) {`. That drops exactly
  the overlapping entries, and matches the range the callback returns (which M5 relies on).
  **Fidelity risk:** none (a few more re-conversions).

### L5. Async `XamShowKeyboardUI` doesn't mark its XOVERLAPPED pending or reset its event, and drops the completion APC

- **Severity:** low–medium. **Verdict:** the deviation is confirmed. Harm is plausible: naming
  works in the owner's tests, so the game probably waits on the event directly.
- **Where:** `DAORecomp/kernel/xam.cpp:1121-1128`; completion helper at `:39-60`. Xenia does it
  in `kernel_state.cc:1182-1193` (`CompleteOverlappedDeferredEx`).
- **Mechanism:**
  - We return `ERROR_IO_PENDING` but leave `InternalLow` (`o->result`) as the guest left it
    (usually 0). So `XHasOverlappedIoCompleted` and `XGetOverlappedResult` report immediate
    success while the keyboard is still open.
  - A manual-reset event left set by an earlier XAM call makes the wait return at once; that is
    the realistic intermittent case.
  - `o->context` is never set, and the completion runs on the host "Keyboard" thread, where
    `CurrentRef()` is null. So a completion APC is silently dropped.
  - `result` is written before `length`, so a poller on another thread can see completion and
    then read a stale length.
- **What the player would see:** an empty or old character name, or the game moving on before the
  keyboard closes. That is intermittent, depending on the event's previous state.
- **How to confirm:** temporarily log `o->result`, `o->event` (and whether it is signalled) and
  `o->completionRoutine` on entry, then name a character.
- **Proposed fix:** before starting the thread:
  ```cpp
  auto* o = g_memory.Translate<X_XOVERLAPPED>(overlapped);
  o->length = 0;
  o->extendedError = 0;
  o->result = X_ERROR_IO_PENDING;
  if (o->event)
      if (auto e = LookupHandle<Event>(o->event, ObjectType::Event))
          e->Reset();
  std::shared_ptr<GuestThread> caller = GuestThread::CurrentRef();
  std::thread([run, overlapped, caller] { /* ... */ CompleteOverlapped(overlapped, result, /*...*/, 0, caller); }).detach();
  ```
  - `CompleteOverlapped` takes a new `caller` parameter for the APC when `o->context` doesn't
    resolve.
  - Write `length` and `extendedError`, then `std::atomic_thread_fence(std::memory_order_release)`,
    then `result`, then set the event.
  - **Fidelity risk:** none (this is Xenia's behaviour).

### L6. `XMsgInProcessCall` / `XMsgStartIORequest` never write their outputs

- **Severity:** low (medium if the game calls an XMP getter). **Verdict:** the stub is confirmed.
  Harm is plausible.
- **Where:** `DAORecomp/kernel/xam.cpp:1139-1149`.
- **Mechanism:** the game imports both, so it sends some message. Xenia's XMP app (app 0xFA)
  writes through pointers in the argument block:
  - `GetStatus` 0x00070009 `{client, state*}`;
  - `GetVolume` 0x0007000B `{client, float*}`;
  - `GetPlaybackController` 0x0007001B `{client, controller*, locked*}`;
  - `GetPlaybackBehavior` 0x00070029 (three pointers).

  We return success and leave the caller's locals as whatever was on its stack. The most likely
  calls (override/restore background music) are input-only and unaffected.
- **What the player would see:** if the game asks who controls the music, its own music sometimes
  doesn't play or is muted, varying from run to run.
- **How to confirm:** run with trace logging and grep `XMsgInProcessCall(app FA, msg 0007000(9|B)|0007001B|00070029`.
  If none appear, this doesn't apply.
- **Proposed fix** (same block in `XMsgStartIORequest`, with `buffer`):
  ```cpp
  if (app == 0xFA && param1)
  {
      const u32* a = g_memory.Translate<u32>(param1);
      auto put = [](uint32_t p, uint32_t v) { if (p) *g_memory.Translate<u32>(p) = v; };
      switch (message)
      {
      case 0x00070009: put(a[1], 0); break;                                          // XMPGetStatus: idle
      case 0x0007000B: if (a[1]) *g_memory.Translate<be<float>>(a[1]) = 1.0f; break; // XMPGetVolume
      case 0x0007001B: put(a[1], 0); put(a[2], 0); break;                            // the title controls playback, unlocked
      case 0x00070029: put(a[1], 0); put(a[2], 0); put(a[3], 0); break;             // playback behaviour
      }
  }
  ```
  **Fidelity risk:** low (Xenia's values).

### L7. A thread that exits holding a mutant doesn't abandon it, and `Mutant::owner` dangles

- **Severity:** low. **Verdict:** confirmed in our code. Harm is plausible: it needs a thread to
  exit while owning a mutant.
- **Where:** `DAORecomp/kernel/objects.h:102` (a raw `GuestThread*`); `kernel/thread.cpp:325-332`
  (the exit path); `objects.cpp:192-201` (`SyncGuest` reads `owner->kthread`).
- **Mechanism:**
  - On NT and the 360 (KeRundownThread), the waiters get `STATUS_ABANDONED_WAIT_0`. Here they
    block forever.
  - Once the `GuestThread` is freed, `SyncGuest` reads freed memory.
  - A new thread allocated at the same address "owns" the mutant recursively.
- **What the player would see:** a hang after a worker thread ends, typically at a level
  transition or on quit.
- **How to confirm:** at thread exit, log every live mutant whose `owner == this` (with the
  registry below).
- **Proposed fix:**
  ```cpp
  // objects.cpp
  static std::mutex s_mutantsMutex;          // taken after the dispatcher lock
  static std::unordered_set<Mutant*> s_mutants;
  // Mutant::Mutant: { std::lock_guard l(s_mutantsMutex); s_mutants.insert(this); }
  // Mutant::~Mutant() override { std::lock_guard l(s_mutantsMutex); s_mutants.erase(this); }
  void AbandonMutants(GuestThread* thread)   // dispatcher lock held
  {
      std::lock_guard l(s_mutantsMutex);
      for (Mutant* m : s_mutants)
          if (m->owner == thread)
          {
              m->owner = nullptr;
              m->recursion = 0;
              m->abandoned = true;
              SignalWaiters(*m);             // NotifyDispatcher(*m) without M2's patch
              m->SyncGuest();
          }
  }
  // thread.cpp Run(), in the exit block before NotifyDispatcher(*this): AbandonMutants(this);
  ```
  **Fidelity risk:** none.

### L8. The held-shot fix parks the render job until the end-hold function returns, not until the poll has seen the byte

- **Severity:** low. **Verdict:** plausible; it depends on TU6 code after the poll. The fix itself
  is correct and bounded: never a freeze, at worst a 50 ms hitch.
- **Where:** `DAORecomp/gpu/held_shot.cpp:33-39` (`s_awaitedJob` set on entry at `:36`, cleared
  when `original` returns at `:38`); `:42-51` (the job parks in 100 µs steps while matched, at most
  `kHandOverTimeout` = 50 ms, `:25`).
- **Mechanism:**
  - The handshake only needs "the poller has read the clear byte". The release condition is
    "EndHold returned".
  - If the end-hold code, after seeing the byte clear, waits on anything the parked worker does
    next (a job flush, a frame-end event, a lock held by the job's caller), the two wait for each
    other until the 50 ms timeout. That is a 50 ms hitch at each shot end.
  - If the end-hold code runs jobs inline on the update thread itself, the hook parks the poller
    on itself.
  - If the end-hold function recurses, the inner return zeroes `s_awaitedJob` early and the old
    freeze returns.
- **What the player would see:** a 3-frame hitch at conversation cuts, if any of the above holds.
- **How to confirm:** time the park loop:
  ```cpp
  const auto t0 = std::chrono::steady_clock::now();
  /* existing loop */
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
  if (us > 1000) LOGI("TEMP held shot: job {:08X} parked {} us ({})", job, us, s_awaitedJob == job ? "timeout" : "released");
  ```
  Also log how long `original` takes in `EndHoldHook`. Parks of 100–300 µs rule this out. Any
  "timeout", or parks close to EndHold's whole duration, confirm it.
- **Proposed fix (cheap hardening):**
  - Never park the poller on itself: record
    `std::atomic<DWORD> s_awaitingThread = GetCurrentThreadId()` in `EndHoldHook`, and return early
    in `HeldShotJobHook` when `GetCurrentThreadId() == s_awaitingThread`.
  - Narrower release, only if the log shows long parks: have `DelayExecution(0)` bump an atomic
    counter when called on the awaiting thread. The job then parks until that counter has advanced
    past its value at park start, plus one more 100 µs step, or until EndHold returns, or the
    timeout.
  - **Fidelity risk:** low. A rare preemption costs one frame, not a freeze.

### L9. `ObReferenceObjectByHandle` takes no reference; a thread created with flag 0x80 isn't kept alive

- **Severity:** low. **Verdict:** confirmed in our code. Harm is plausible but unusual.
- **Where:** `DAORecomp/kernel/xboxkrnl_threading.cpp:370-380` (no reference; `objectType` not
  checked); `:382` (`ObDereferenceObject` is a no-op); `:81-82` (flag 0x80 returns the KTHREAD
  with no handle); `kernel/thread.cpp:145-159` (Unregister); `objects.cpp:85-98` (the header is
  freed with the object).
- **Mechanism:**
  - The pattern "reference, NtClose the handle, then wait on or set the pointer" leaves a pointer
    to a freed header.
  - If `KernelAlloc` reuses that address, `ObjectFromGuestHeader` binds the wait to the wrong
    object: a lost signal or a hang.
  - A thread created with flag 0x80 and joined through its KTHREAD after exit hits "Unknown
    dispatcher object type 6", so the wait returns `X_STATUS_ABANDONED_WAIT_0`
    (`xboxkrnl_threading.cpp:398-401`).
  - Xenia retains in both cases. The common XDK use (XAPI SetThreadPriority / XSetThreadProcessor:
    reference, use, dereference with the handle still open) is safe here.
- **How to confirm:**
  - Grep `dao.log` for "KeWaitForSingleObject: unknown object" and "Unknown dispatcher object
    type".
  - Check the existing `ExCreateThread ... flags` lines (`:78`) for bit 0x80.
- **Proposed fix:**
  ```cpp
  static std::mutex s_obRefsMutex;
  static std::unordered_multimap<uint32_t, ObjectRef> s_obRefs;
  // ObReferenceObjectByHandle, before returning success:
  { std::lock_guard l(s_obRefsMutex); s_obRefs.emplace(object->guestObject, object); }
  void ObDereferenceObject(uint32_t object)
  {
      ObjectRef doomed; // released after the lock
      std::lock_guard l(s_obRefsMutex);
      if (auto it = s_obRefs.find(object); it != s_obRefs.end()) { doomed = std::move(it->second); s_obRefs.erase(it); }
  }
  // ExCreateThread with 0x80: keep a handle for the caller, as Xenia does
  if (flags & kReturnKThreadPtr) { CreateHandle(thread); *handleOut = thread->kthread; }
  ```
  **Fidelity risk:** a leak if the guest never dereferences, which is also what the hardware would
  do.

### L10. An out-of-range XMA frame size stops that context for good

- **Severity:** low. **Verdict:** a confirmed deviation from Xenia. Its trigger is unlikely with
  valid data.
- **Where:** `DAORecomp/apu/xma_decoder.cpp:709-717` (`error_status = 4` at `:714`, then a return
  without advancing).
- **Mechanism:**
  - Every later pass re-reads the same header, so the read offset and valid bits never move, and
    nothing clears the error until the game re-initialises.
  - Xenia has no such guard: it advances and resyncs.
  - The second condition is dead: `BitsRemaining()` is at least 16353, which is above
    `kMaxFrameSizeInBits` (16352).
  - The trigger needs a misaligned read offset (a garbage 15-bit size from the split-header path)
    or a damaged packet.
- **What the player would see:** one voice or music track goes silent and never "finishes". If the
  game waits for it, a stall.
- **How to confirm:** add `XMA_WARN_LIMITED("XMA: frame size {} bits out of range", packetInfo.currentFrameSize);`
  in that branch; it logs nothing today. With `DAO_XMA_TRACE=1`, look for a context stuck with
  error 4 and repeated FrameError notes at a constant read offset.
- **Proposed fix:** keep the memory-safety check, but resync to the next packet:
  ```cpp
  if (packetInfo.currentFrameSize > kMaxFrameSizeInBits || packetInfo.currentFrameSize > frameStream.BitsRemaining())
  {
      XMA_WARN_LIMITED("XMA: frame size {} bits out of range, resyncing at the next packet", packetInfo.currentFrameSize);
      const uint32_t nextInputOffset = GetNextPacketReadOffset(data, nextPacketIndex, currentPacketCount);
      if (nextPacketIndex >= currentPacketCount || nextInputOffset == kXmaBitsPerPacketHeader)
          SwapInputBuffer(data);
      data.Set(kInputBufferReadOffset, nextInputOffset);
      m_lastNote = XmaSkip::FrameError;
      if (m_trace) m_trace->frameErrors++;
      return;
  }
  ```
  This is the same pattern as the last-frame path at `:767-770`. **Fidelity risk:** a short glitch
  instead of silence.

### L11. An alertable wait returns USER_APC instead of taking an object that is already signalled

- **Severity:** low. **Verdict:** confirmed.
- **Where:** `DAORecomp/kernel/objects.cpp:601-607` runs before the object tests at `:609-637`.
- **Mechanism:** ReactOS `KeWaitForSingleObject` tests the object and returns WAIT_0 before
  `KiCheckAlertability`. We deliver the APCs and return USER_APC without taking the object. The
  caller normally loops and gets it, so this usually costs one extra pass. A caller that treats
  USER_APC as "nothing happened, go do something else" acts later than on the 360.
- **Proposed fix:** move the `:601-607` block below the wait-any/wait-all tests, just before
  `if (poll)`. With M2's patch, the `satisfied` check stays first. **Fidelity risk:** none.
- **How to confirm:** rarely matters. Count alertable waits that return USER_APC while an object
  was signalled.

### L12. A periodic timer queues one APC per firing, without limit

- **Severity:** low. **Verdict:** confirmed in our code. Harm is plausible.
- **Where:** `DAORecomp/kernel/objects.cpp:292-300`.
- **Mechanism:** the 360's timer has one KAPC, and re-inserting it while it is queued fails;
  NtSetTimerEx and NtCancelTimer also remove it. We push one APC per firing and never remove any.
  A periodic timer whose thread is rarely in an alertable wait therefore runs a burst of
  callbacks later, and even after a Cancel.
- **How to confirm:** `LOGI` in `NtSetTimerEx` when `routine != 0` (with the period), and when
  `apcQueue` holds more than one timer APC.
- **Proposed fix:** in `Fire`, push only if no host-queued APC with the same routine and context is
  already in the queue:
  `!std::any_of(q.begin(), q.end(), [&](const Apc& a) { return !a.guestApc && a.normalRoutine == timer->apcRoutine && a.normalContext == timer->apcContext; })`.
  Also remove such an APC in `Timer::Set` and `Timer::Cancel`.

### L13. The read-completion APC is queued even when the read failed

- **Severity:** low. **Verdict:** confirmed deviation. Harm is plausible but unlikely.
- **Where:** `DAORecomp/kernel/xboxkrnl_io.cpp:224-228`, reached from `:279` with
  `X_STATUS_END_OF_FILE` and from the scatter read at `:307` with any status.
- **Mechanism:** NT does nothing more for a synchronously returned error, and Xenia's
  `NtReadFile` queues the APC only on success. A ReadFileEx-style caller sees a FALSE return
  (EOF), cleans up, and then its completion routine runs on an OVERLAPPED it may have freed. Plain
  XAPI ReadFile passes no routine, so it is unaffected.
- **Proposed fix:** `if ((apcRoutine & ~1u) && apcContext && status == X_STATUS_SUCCESS)`.
  **Fidelity risk:** none.
- **How to confirm:** a temporary `LOGW` in `CompleteIo` when `status != X_STATUS_SUCCESS && (apcRoutine & ~1u)`.

### L14. Vblank delivery: a normal-priority thread, ~1 ms jitter, and catch-up bursts after a stall

- **Severity:** low. **Verdict:** plausible.
- **Where:** `xenia/src/xenia/gpu/graphics_system.cc:97-183` (thread at `:99`; catch-up at
  `:163-177`, `kMaxVblankCatchUp` 1 s at `:122`); `NanoSleep` is `NtDelayExecution`
  (`base/threading_win.cc:151-161`).
- **Mechanism:**
  - The 360's vblank interrupt preempts everything and is never late.
  - Ours runs on a normal-priority thread, while guest threads with base priority ≥2 run at
    HIGHEST.
  - Each beat can be 0–1 ms late (1 ms timer resolution on Windows).
  - After a real stall of this thread (preempted on a 4-core Deck, or waiting on `s_interruptMutex`
    behind a slow GPU interrupt), it delivers up to 1 s of missed beats back to back. That gives
    about one short frame through the flip queue, not zero-length frames.
  - A free-running 60.000 Hz clock against a 59.94 Hz display drops or doubles a frame about every
    17 s, as upstream does.
- **What the player would see:** a short run of uneven frames after a host hitch, and judder that
  comes and goes on fixed-refresh displays.
- **How to confirm:** temporary counters in the loop, logged every 10 s: passes that didn't sleep,
  and the largest `now - next_vblank_deadline`. Zero bursts in a normal session rules it out.
- **Proposed fix:** `SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);` right after
  `AttachGuestThread` at `:101` (the thread sleeps more than 99% of the time).
  - Optionally cap the catch-up at a few beats, if the audio-sync reasoning in the comment allows:
    `else if (now - next_vblank_deadline > 3 * period) next_vblank_deadline = now;`.
  - **Fidelity risk:** low.

### L15. The XAudio worker runs at normal priority, and drops callbacks silently when more than 20 ms late

- **Severity:** low. **Verdict:** plausible.
- **Where:** `DAORecomp/apu/audio.cpp:108-132`. There is no `SetThreadPriority`; the lag reset at
  `:125-130` logs nothing.
- **Mechanism:**
  - The 360's audio driver is effectively real-time, and Xenia's worker runs at priority 24
    (host HIGHEST).
  - Ours competes with guest threads mapped to HIGHEST or TIME_CRITICAL, and it also runs the
    synchronous XMA decode passes kicked from inside the callback.
  - More than 20 ms late, the schedule restarts and the missed callbacks are never run. The
    guest's audio clock slips against wall time, and SDL underruns.
- **What the player would see:** crackles and drop-outs under CPU load (loading, busy scenes,
  the Deck).
- **How to confirm:** add a rate-limited `LOGW("XAudio: {} ms late, {} callbacks dropped", ...)` in
  the reset branch, and watch the existing "game audio is not keeping up (N underruns)" warning
  alongside `DAO_THREAD_STATS`.
- **Proposed fix:** `SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);` (or
  TIME_CRITICAL) after `AttachHostThread`, plus the warning above. **Fidelity risk:** low.

### L16. GPU interrupts run without DPC context, and a guest thread raising IRQL doesn't hold them off

- **Severity:** low. **Verdict:** plausible.
- **Where:** `DAORecomp/gpu/xenia_host.cpp:409-421` (only `PCR+0x10C`, the CPU number, is set);
  IRQL is a PCR byte (`kernel/xboxkrnl_threading.cpp:582-635`). Upstream: `kernel_state.cc`
  `EmulateCPInterruptDPC` / `BeginDPCImpersonation` (pinned `1c81b867`, lines 1387-1443).
- **Mechanism:**
  - On the 360 the graphics interrupt runs as a DPC at DISPATCH level on the target hardware
    thread, and is held off while that CPU's IRQL is at DISPATCH or above.
  - Upstream Xenia at least sets the ISR's IRQL to 2 and `dpc_active` to 1.
  - Ours runs at IRQL 0, with `dpc_active` 0, on a host thread, concurrently with any guest thread
    "on" that CPU.
  - Guest code that reads IRQL or `KeIsExecutingDpc` inline, or that protects state shared with
    the ISR only by `KeRaiseIrqlToDpcLevel` on the same CPU (imported by the game), behaves
    differently.
- **What the player would see:** at most, rarely, a frame shown twice or a missed signal around
  presents.
- **How to confirm:** a temporary atomic per-CPU "raised" count (keyed by the PCR CPU byte),
  incremented in `KeRaiseIrqlToDpcLevel` and `KfAcquireSpinLock` when the old IRQL is below 2, and
  decremented in `KfLowerIrql` and `KfReleaseSpinLock` when the new one is below 2.
  `CallInterruptCallback` counts entries while `raised[cpu] > 0`. A non-zero count means the
  hardware's hold-off is being violated.
- **Proposed fix (the DPC-context part):**
  ```cpp
  uint8_t& irql = *g_memory.Translate<uint8_t>(thread->pcr + 0x18);
  const uint8_t oldIrql = irql;
  irql = 2;
  *g_memory.Translate<u32>(thread->pcr + kPcrDpcActive) = 1;
  CallGuest(callback, { source, userData });
  *g_memory.Translate<u32>(thread->pcr + kPcrDpcActive) = 0;
  irql = oldIrql;
  ```
  Making the ISR wait while `raised[cpu] > 0` is only worth considering if the diagnostic shows
  hits.

### L17. Guest priorities become global host priorities, without affinity

- **Severity:** low. **Verdict:** plausible.
- **Where:** `DAORecomp/kernel/xboxkrnl_threading.cpp:126-167` (increment ≥2 → HIGHEST,
  ≥15 → TIME_CRITICAL).
- **Mechanism:**
  - On the 360 a priority only orders threads within one hardware thread.
  - On the host, if at least as many busy HIGHEST/TIME_CRITICAL guest threads exist as logical
    CPUs (the Deck has 8), a normal-priority thread holding a lock they spin on starves until
    Windows' starvation boost, about 4 s. That is exactly this review's bug class.
  - Proton may largely ignore these priorities.
  - Xenia canary now ignores guest priorities by default (`ignore_thread_priorities`, changed
    2026-04-09).
- **How to confirm:** `LOGI` each `KeSetBasePriorityThread` call (thread id, start address,
  increment). During any stall, `DAO_THREAD_STATS` showing a HIGHEST thread at about 100% of a
  core while another thread does nothing confirms it.
- **Proposed fix (only if seen):** cap the mapping at ABOVE_NORMAL, or ignore guest priorities as
  Xenia now does.

### L18. The 60 fps clamp applies to every present, not only 3D scenes

- **Severity:** low. **Verdict:** plausible.
- **Where:** `DAORecomp/gpu/video.cpp:269-275`. Every present with interval ≥1 becomes interval 1,
  threshold 100, while the unlock is on: menus, loading screens and movies too.
- **Mechanism:** Bink paces itself by its own clock, so movies are normally fine. Anything that
  advances one step per present at interval 2 (a loading spinner, a menu animation, a movie
  played without BinkWait) runs twice as fast.
- **How to confirm:** F10 toggles the unlock at runtime (`main.cpp`). Compare an intro movie or
  loading-screen animation with it on and off. A temporary histogram of the `interval` values the
  hook sees (logged every 600 calls) shows where interval 2 is used.
- **Proposed fix (only if seen):** clamp interval 2 only during 3D frames, for example with a flag
  set by `SceneTilesHook` for the current frame.

### L19. Closing the window ends the process at once, even in the middle of writing a save

- **Severity:** low (mitigated by the save backups). **Verdict:** plausible.
- **Where:**
  - `DAORecomp/gpu/xenia_host.cpp:52-56` (window close).
  - `kernel/xam.cpp:136-146` (XamLoaderTerminateTitle / LaunchTitle).
  - `kernel/xboxkrnl_rtl.cpp:543` (`ExRegisterTitleTerminateNotification` is a no-op).
  - `ui/graphics_menu.cpp:368` (Manage mods).
- **Mechanism:** a system-initiated quit on the 360 runs the title's terminate notifications. Our
  window close runs nothing and calls `TerminateProcess`. Closing during a save write leaves it
  partial.
  - When the save overwrote an older one, the backup in `userdata/save_backups/` (`xam.cpp:768-831`)
    keeps the previous version.
  - A brand-new save slot would just be corrupt.
- **How to confirm:** close the window right after starting a save, then look for
  `XamContentCreateEx: '…' wasn't closed cleanly` at the next overwrite, or a corrupt slot.
- **Proposed fix:** in the window-close handler, if a save is being written (`s_savesWriting` in
  `xam.cpp` is non-empty), wait up to about 10 s for it to empty before `TerminateProcess`. This
  needs a small `bool SavesBeingWritten()` accessor. **Fidelity risk:** none (a slower exit only
  while saving).

### L20. On Windows 11, a minimized or hidden window can lose the 1 ms timer resolution

- **Severity:** low. **Verdict:** plausible.
- **Where:** `DAORecomp/main.cpp:293` (`timeBeginPeriod(1)`); nothing opts out of Windows 11's
  power throttling.
- **Mechanism:**
  - Windows 11 may ignore a process's timer-resolution request while its windows are minimized,
    occluded or inaudible. It is unclear whether DAO's audio counts as audible.
  - Then every ms sleep becomes about 15.6 ms: guest `KeDelayExecutionThread`, the vblank pacing
    (bursty), and the audio worker's 5.33 ms steps, which overrun `kMaxLag` and drop callbacks
    (L15).
- **How to confirm:** a temporary thread logging `NtQueryTimerResolution`'s current value every
  5 s. Minimize the game on Windows 11: 156250 confirms it, 10000 rules it out.
- **Proposed fix:** right after `timeBeginPeriod(1)`:
  ```cpp
  PROCESS_POWER_THROTTLING_STATE state{ PROCESS_POWER_THROTTLING_CURRENT_VERSION,
      PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION, 0 };
  SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
  ```
  It fails harmlessly on older Windows. **Fidelity risk:** none.

### L21. The "VSync" option never controls the PC's own presentation

- **Severity:** low (visual). **Verdict:** confirmed.
- **Where:** `xenia/src/xenia/ui/d3d12/d3d12_presenter.cc:1231-1234` (always `Present(0, …ALLOW_TEARING)`)
  and `:315-320` (`d3d12_allow_variable_refresh_rate_and_tearing` defaults to true, `:26-31`);
  `DAORecomp/launcher/configure_view.cpp:136`.
- **Mechanism:**
  - Guest frames come from a free-running 60 Hz clock that isn't locked to the display.
  - In borderless fullscreen with tearing support on a fixed-refresh display, every present flips
    immediately, so tear lines can show whatever the box says.
  - Windowed mode is composed by DWM, so it doesn't tear, but beats against the display (L14).
- **How to confirm:** fullscreen on a non-VRR 60 Hz monitor with VSync on, pan the camera, and
  look for tear lines.
- **Proposed fix:** decide what the option means once M1 is fixed.
  - Either rename it, or make "on" present with `Present(1, 0)` (no tearing flag).
  - The presenter thread then blocks up to one refresh. Xenia avoided interval 1 for high-refresh
    displays, so test at 144 Hz too.

### L22. The ring buffer's read pointer is written back once per batch

- **Severity:** low. **Verdict:** plausible; same as upstream.
- **Where:** `xenia/src/xenia/gpu/command_processor.cc:411-420`.
- **Mechanism:** the hardware writes the read pointer back every RB_BLKSZ while fetching. Ours does
  it after a whole batch. A guest waiting for ring space while the command processor sits in a
  `WAIT_REG_MEM` inside the batch (the flip wait) gets the space only when the batch ends, up to
  one vblank later. A deadlock would need the space-waiting thread to be the one that releases
  that wait. The known wait is released by the vblank ISR on another thread, so this is a latency
  issue only.
- **How to confirm:** a temporary log in `WAIT_REG_MEM` for waits longer than 1 s (address,
  reference, value). With `DAO_DUMP_AFTER`, look for the CP thread in `WAIT_REG_MEM` while the
  render thread is in D3D's ring-space wait.
- **Proposed fix:** none unless seen.

---

### Notes (real, but small or unlikely to matter)

| ID | Where | What | How to check / fix |
|---|---|---|---|
| N1 | `DAORecomp/gpu/video.cpp:250-256` | The fence-wait hook sleeps 100 µs from the first failed step: a fence that completes 5 µs later still costs ≥100 µs (about 1 ms per step on Windows 10 before 1803, which has no high-resolution waitable timers) | Time each fence wait with a temporary per-thread start time; log p50/p99. Fix: spin (`_mm_pause`) for about 30 µs, timed from the wait's first step, before sleeping |
| N2 | `DAORecomp/gpu/video.cpp:291-298`, `kernel/thread.cpp:486-491`, `main.cpp:303-309` | "Is this a recompiled function?" guards test `FindFunction(x) != nullptr`, but every empty slot holds `UnknownIndirectCall`, so they never reject. A bad pointer reaches `UnknownIndirectCall`, which terminates and logs a garbage `ctr` | `bool IsRecompiledFunction(uint32_t a) { auto* f = g_memory.FindFunction(a); return f && f != UnknownIndirectCall; }` (make the trampoline visible); set `ctx.ctr` in `CallGuest` before calling |
| N3 | `DAORecomp/main.cpp:567-578` | If the guest main thread returned, `main()` would return and run static destructors while every other thread still runs | `TerminateProcess(GetCurrentProcess(), code)` after the log line, like every other exit |
| N4 | `DAORecomp/kernel/xboxkrnl_threading.cpp:512-525` | Critical sections always spin 1000 × `_mm_pause` and ignore the spin count `RtlInitializeCriticalSectionAndSpinCount` stores (`:502`); a plain one spins 0 on the 360. Costs 3–45 µs of CPU per contended Enter, and widens M2's barging window | Spin `cs->header.absolute * 256` times (0 = none) |
| N5 | `DAORecomp/kernel/objects.cpp:336-350` | A detached `std::thread` per periodic timer `Set`; old ones exit at their next wake. Re-arming every frame with a 1 s period keeps about 60 threads alive | Reschedule inside `TimerService` (add `periodMs` to `Entry`; push the next firing in `Run` when `Fire` reports alive and current) |
| N6 | `DAORecomp/kernel/module.cpp:262-278` | KeTimeStampBundle starts at 0 at process start (Xenia does the same); the 360's is ms since boot | Add a constant offset (for example 2 minutes) if a start-up cooldown misbehaves |
| N7 | `DAORecomp/kernel/xboxkrnl_threading.cpp:342-351`, `279-283` | `NtCancelTimer` always reports state 0 (as Xenia); releasing a semaphore past its limit returns -1 instead of raising | None needed |
| N8 | `DAORecomp/apu/audio.cpp:235-236`, `802-804`, `459` | XMA buffer pointers are cached at Initialize; Xenia re-reads context dwords 5/6/7 every pass. Matters only if the game writes the context directly (`XMASetInputBuffer*` isn't imported) | Temporary `LOGW` once if `dw[5..7] != ToPhysical(cached)`; no warning = ruled out |
| N9 | `DAORecomp/apu/audio.cpp:848-872` | `XMADisableContext(wait=0)` fails (`X_E_FALSE`) whenever the slot mutex is held, including by a brief accessor; Xenia always waits | Always take the lock (waits at most one pass) |
| N10 | `xenia/src/xenia/gpu/graphics_system.cc:274-276` | Every unhandled MMIO write logs "Unknown GPU register" at the default log level | Count the lines in `xenia.log`; silence the known ones if frequent |
| N11 | `xenia/src/xenia/gpu/graphics_system.cc:292-303` | Interrupt callback and its argument are set without synchronisation while the vblank thread reads them (upstream) | Store the argument first, then the callback with release ordering |
| N12 | `DAORecomp/kernel/xboxkrnl_io.cpp:186-190`, `320-330`, `514-549` | Parents created on create; share modes ignored; offset `~0` = current position instead of append; every write failure = ACCESS_DENIED (no DISK_FULL); rename always replaces and ignores RootDirectory | Fix when a game path needs it; DISK_FULL is the one a player could see ("storage full" on save) |
| N13 | `DAORecomp/kernel/xam.cpp:863-918` | The save backup and "being written" marker cover only content modes 2 and 5; a save written with mode 4 (open always) gets neither | Check the `XamContentCreateEx(... flags ..)` log line for the mode the game saves with |
| N14 | `DAORecomp/kernel/xam.cpp:1048-1075` | Device selector and message box complete at once without broadcasting XN_SYS_UI 1→0; the message box returns the default button | The game gets past these today; keep in mind for "delete corrupt save?" prompts |
| N15 | `DAORecomp/kernel/xboxkrnl_io.cpp:1-2`, `215-236`; `vfs.cpp:271`, `277`, `299` | All reads complete synchronously (as Xenia), with the file's mutex held; `fs::exists` / `is_directory` per mod overlay under `s_vfsMutex` on opens and attribute queries | `DAO_PROFILE` samples in ReadFile / NtCreateFile on the main or render thread during hitches |
| N16 | XenonRecomp (`config/DAO.toml:12`, `reserved_as_local=false`) | `lwarx`/`stwcx.` become a value compare-and-swap, so hand-written untagged lock-free pops could hit ABA (compiler intrinsics and the tagged SList are safe) | Inspect the generated `__sync_bool_compare_and_swap` sites in `ppc/` locally |
| N17 | `xenia/src/xenia/gpu/command_processor.h:127-128`, `pm4_command_processor_implement.h:673`, `graphics_system.cc:310` | `counter_` is incremented from the vblank and CP threads without atomics (upstream). A lost increment is ~1e-7 per event and can't make a waited-for value unreachable | `std::atomic<uint32_t>`, relaxed |

---

## 3. Checked and clean

Reviewed and found sound, or a candidate that the verifiers refuted:

**Kernel**
- Timeout conversion (`objects.cpp:62-75`):
  - relative and absolute times, zero (an immediate poll, as NT), huge values clamped, absolute
    times in the past;
  - MSVC's `condition_variable::wait_until` / `sleep_until` round up and loop, so they never wake
    early;
  - the timeout re-check at `:647-656` is right.
- Registration before blocking, under the dispatcher lock: no notify-before-wait race; spurious
  wakeups are handled.
- Return values:
  - KeSetEvent / KeResetEvent / NtSetEvent return the previous state;
  - semaphore limits; mutant recursion; MUTANT_NOT_OWNED for a non-owner;
  - wait-all atomicity; WaitAny index and abandoned arithmetic; waitType 0 = WaitAll;
  - NtSetTimerEx's previous state; timer generations against stale firings.
- `FinishAlertableWait`'s second `DeliverApcs` only drains APCs queued meanwhile; it isn't a double
  delivery.
- Critical sections:
  - layout (lockCount 0x10, recursion 0x14, owner 0x18, spin count in `header.absolute`);
  - Enter/Leave/TryEnter ordering and the `owningThread = 0` race;
  - a wakeup can't be lost (the verifier's proof: after a contended Leave, only consuming the event
    gets in);
  - the event isn't lost by rebinding unless the guest rewrites the header.
- SList pop and flush match NT and Xenia: the sequence is kept, depth is handled, and the 64-bit
  CAS is coherent with the guest's inline push.
- `KfAcquireSpinLock`: its owner value (the PCR) is unique per thread.
- Lock order: under `s_dispatchMutex` only `s_headerMutex` is reachable (via a destructor), never
  the reverse. `s_threadsMutex` and `s_handleMutex` aren't nested in it. No logging under the
  dispatcher lock.
- Create-suspended vs Resume. TLS (`TlsFree` zeroing every thread's slot matches Windows).
  `thread_local` state on attached host threads. Handle counter wrap (after about 1 billion).
- `mftb` (`main.cpp:35-45`): QPC-based, monotonic, scaled to 49.875 MHz, matching
  `KeQueryPerformanceFrequency`. The 100 ns steps are not a problem (refuted K12).
- **Refuted:** sub-millisecond `KeDelayExecutionThread` sleeping 1–2 ms is likely the hardware's
  tick behaviour too; Xenia's yield is the outlier (K4).

**GPU**
- Ring buffer:
  - the write pointer is a seq_cst store plus an auto-reset event (no lost wake);
  - the read pointer is written back big-endian, only after the batch's CP-side effects (memory
    writes, EVENT_WRITE_SHD, scratch, interrupts), so the guest never sees rptr or a fence ahead
    of those.
- Interrupts:
  - serialised across the vblank and CP threads, with no re-entry;
  - an attached guest identity with its own PCR and stack;
  - no other host lock held across `CallGuest`.
- VdSwap packet; `VdInitializeScalerCommandBuffer`; `VdSetGraphicsInterruptCallback`;
  `VdEnableRingBufferRPtrWriteBack`.
- Write watches:
  - armed before the upload copy, so no stale data is left marked valid;
  - every unwatched page has been invalidated by every callback;
  - the double-fault retry; recommit zeroing via `PrepareGuestWrite` outside the heap lock;
  - two-page stores.
- `ReadBackGpuWrittenMemory`: 5 s timeout, shared state in a `shared_ptr`.
- The presenter never blocks the CP on host vsync. `NtSetEventBoostPriority` under Wine is
  `NtSetEvent`.
- The null command processor's vblank thread is unused when Xenia is active.
- Hooks:
  - no hook reads argument registers after calling the original, or clobbers r1/r2/r13/cr/ctr/f
    registers;
  - `ReturnValue` saves and restores r1;
  - `GAME_HOOK` machinery and `__imp__tu6_` naming; every data address goes through `GameAddress`;
  - `held_shot.cpp` is TU6-only by design and bounded.
- The Graphics tab: menu state under its mutex; engine bytes written only from the render-thread
  hook (`atomic_ref`).
- **Refuted:** the TU6 data addresses' differing offsets are a relink, not an error; check them
  with `scripts/match_build.py` (H9). The CP's 100 ms idle wait is a timeout, not latency (G9). The
  `counter_` race is moot (N17).

**Audio**
- XMA context layout and bitfields match Xenia's `XMA_CONTEXT_DATA` (big-endian).
  `XmaContextInit` / loop data structs.
- Units: input offsets in bits, output offsets in 256-byte blocks, 2048-byte packets.
- Render callback contract: the argument cell, 6×256 big-endian float frames channel-major, 5.1
  channel order, speaker config.
- `StoreContextMerged` field ownership; ring full/empty (same as Xenia).
- Lock order slot → kick and client → frame. Release and `Disable(1)` wait for an in-flight pass;
  Enable can't return before a racing pass ends.
- Deadline pacing without drift; no logging on hot paths.
- **Refuted:** the mutex held across the callback (Xenia's unregister blocks the same way, A5).
  One decode pass per `XMAEnableContext` matches Xenia (A7). Device migration mostly doesn't stall
  SDL3 (part of A1).

**File I/O and XAM**
- The IO status block is written before the event is set or the APC queued.
- Synchronous handles return the real status; EOF gives END_OF_FILE with 0 bytes; a partial read is
  SUCCESS with a count.
- `FILE_USE_FILE_POINTER_POSITION` and a NULL offset; the file position is updated.
- Scatter reads (4 KB segments, as Xenia).
- Dispositions and their Information values; COLLISION; FILE_IS_A_DIRECTORY / NOT_A_DIRECTORY;
  PATH vs NAME not found.
- Case-insensitive lookups; `..` can't leave a device; device-name refusals; symbolic links.
- Big-endian layouts: FILE_NETWORK_OPEN, BASIC, STANDARD, directory entries, XCONTENT_DATA,
  X_INPUT_STATE.
- XOVERLAPPED completion order (result, length, extended error, then event, then APC); XamEnumerate.
- XNotify mask/area arithmetic. A listener stays signalled while its queue isn't empty. Start-up
  notifications go to the first system listener.
- Input: 250 Hz poll, the packet number changes only on a state change, mutex-protected; users
  other than 0 are DEVICE_NOT_CONNECTED.
- Network stubs return failures, never PENDING (no wait loops). `WSAWaitForMultipleEvents` is a
  real kernel wait.
- Profile settings are written atomically (temporary file + `MoveFileEx` write-through). The save
  backup and marker logic is sound for modes 2 and 5. `s_contentMutex` serialises content.
- File, enumerator and listener handles are released on NtClose.
- Arguments 9 and later are handled (`function.h:42-48`).
- **Refuted:** `XamUserReadProfileSettings` returning INSUFFICIENT_BUFFER without completing the
  overlapped is Xenia's and XAM's contract (F7).

**Host lifetime and settings**
- `s_settings` is written only at load.
- The input thread.
- `PPCQueryTimebase`.
- Interrupt dispatch.
- No per-frame allocations, file I/O or logging on any per-frame hook path. VdSwap logs every 600
  swaps.

## 4. Not covered

- **Game code.** What the game actually does on any of the plausible paths:
  - whether it uses auto-reset events with several waiters or Set/Reset pulses (M2);
  - whether a voiced line waits for the voice to end (M3);
  - the `wait` value of its flip `WAIT_REG_MEM` (M6);
  - its `FindFirstFile` masks (L2);
  - what TU6's end-hold code does after its poll (L8);
  - whether anything counts vblanks (M1, L18);
  - whether it writes XMA contexts directly (N8).

  The diagnostics given for each finding answer these on the owner's PC.
- XenonRecomp's actual translation of `lwarx`/`stwcx.` and `divw` (the generated code is off-limits
  here; N16).
- The real 360 `KeDelayExecutionThread` tick, `ShortSleep`'s measured length on the owner's PC and
  the Deck, and Windows wake latencies. The M2 model was run on Linux.
- Texture cache, render target cache, upscaler internals, async pipeline compilation, and the
  strict/fast/fake zpass paths.
- `DAORecomp/gpu/command_processor.cpp` (the null command processor, only used without D3D12),
  `frame_history.cpp` and the debug samplers (opt-in).
- `kernel/xboxkrnl_memory.cpp` beyond `PrepareGuestWrite` users. `ui/keyboard.cpp` internals.
- FFmpeg's XMA decoder and the `DAO_XMA=fake` path.
- SDL3's behaviour on device loss, checked from SDL's source (release-3.2.x) rather than run; the
  repo's pinned SDL revision wasn't in the checkout.
- The launcher, installer, mod converter and updater (not runtime fidelity).

## 5. Counts, and what to do first

| Severity | Count |
|---|---|
| High | 0 |
| Medium | 6 (M1–M6) |
| Low | 22 (L1–L22) |
| Note | 17 (N1–N17) |

**Fix first:**
1. **M1, VSync and the vblank clock.** A one-condition change, confirmed, no fidelity risk.
2. **M2, signal handoff.** The patch is ready. Run the M2 diagnostic first: it says how much this
   game hits it, and where.
3. **M3, audio with no output.** Small and confirmed. It also stops `DAO_AUDIO=null` test runs
   from producing false hangs.
4. **M4, file reads into watched memory.** Rare, but the result is a hard exit on Windows or
   silent corruption on the Deck.
5. **M6, flip-wait polling.** A small change on every frame at default settings. Measure with the
   `WAIT_REG_MEM` table first.

Cheap one-liners worth taking along: L4 (`primitive_processor.cc:1668`), L13 (APC only on success),
L3 (`ResetSlotDecoders` on Initialize) and L20 (timer-resolution opt-out).

**Run locally first (no code change needed):**
- Grep existing `dao.log` / `xenia.log` files for:
  - `failed (998)` and `XamShowDirtyDiscErrorUI` (M4);
  - `Unknown dispatcher object type` and `KeWaitForSingleObject: unknown object` (L9);
  - `XAudio: frame queue full` and `can't open the playback device` (M3);
  - `Unknown GPU register` (N10).
- Time the `VdSwap #N` lines with VSync off (M1).
- `DAO_THREAD_STATS=5` in a 3D scene: is the "GPU Commands" thread near 100% of a core (M6)?

**Then the temporary diagnostics, in this order:**
1. The M2 counters (10–15 minutes of play).
2. The L1 Sleep(0) gap histogram.
3. The M5 fault counter and the M6 `WAIT_REG_MEM` table.
4. The L8 held-shot park timing.

---

## Appendix A. Standalone model of the dispatcher (for M2)

The current `objects.cpp` dispatcher logic, extracted into one file, with M2's fix behind `-DNEW`.
Build it twice and compare:
- Windows: `clang-cl /std:c++20 /O2 /EHsc harness.cpp` and `... /DNEW`.
- Linux: `g++ -std=c++20 -O2 -pthread harness.cpp` and `-DNEW`.

Scenarios:
- S1–S3: the lost wakes.
- S4: critical-section barging.
- S5: a stress test of the invariants (exclusion, tokens, timeouts, wait-all).
- S6: critical-section throughput.

<details>
<summary>harness.cpp</summary>

```cpp
// Standalone model of DAORecomp/kernel/objects.cpp dispatcher (OLD = current, NEW = proposed K1 fix).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <random>
#include <span>
#include <thread>
#include <vector>
#include <string>
#include <immintrin.h>

constexpr uint32_t X_STATUS_WAIT_0 = 0, X_STATUS_TIMEOUT = 0x102, X_STATUS_ABANDONED_WAIT_0 = 0x80, X_STATUS_USER_APC = 0xC0;
struct GuestThread { int id; };
static thread_local GuestThread* t_self;
struct DispatchWaiter;
struct KernelObject : std::enable_shared_from_this<KernelObject> {
    virtual ~KernelObject() = default;
    virtual bool IsSignaled(GuestThread*) const { return false; }
    virtual void Acquire(GuestThread*) {}
    virtual uint32_t WaitStatusAdjust() { return 0; }
    virtual void SyncGuest() {}
    std::vector<DispatchWaiter*> waiters;
};
using ObjectRef = std::shared_ptr<KernelObject>;
static std::mutex s_dispatchMutex;
static std::unique_lock<std::mutex> LockDispatcher() { return std::unique_lock(s_dispatchMutex); }

struct DispatchWaiter {
    std::condition_variable cv;
#ifdef NEW
    std::span<const ObjectRef> objects;
    GuestThread* thread = nullptr;
    bool waitAny = false;
    bool satisfied = false;
    uint32_t status = 0;
#endif
};
static thread_local DispatchWaiter t_waiter;
static void AddWaiter(KernelObject& o) { o.waiters.push_back(&t_waiter); }
static void RemoveWaiter(KernelObject& object) {
    auto& waiters = object.waiters;
#ifdef NEW
    waiters.erase(std::find(waiters.begin(), waiters.end(), &t_waiter));
#else
    const auto it = std::find(waiters.begin(), waiters.end(), &t_waiter);
    *it = waiters.back();
    waiters.pop_back();
#endif
}
static void NotifyDispatcher(KernelObject& object) { for (auto* w : object.waiters) w->cv.notify_one(); }
#ifdef NEW
static void SignalWaiters(KernelObject& object)
{
    for (DispatchWaiter* waiter : object.waiters)
    {
        if (waiter->satisfied || !object.IsSignaled(waiter->thread))
            continue;
        if (waiter->waitAny)
        {
            const auto& objects = waiter->objects;
            const auto it = std::find_if(objects.begin(), objects.end(),
                [&](const ObjectRef& o) { return o.get() == &object; });
            if (it != objects.end())
            {
                object.Acquire(waiter->thread);
                waiter->status = uint32_t(X_STATUS_WAIT_0 + (it - objects.begin())) + object.WaitStatusAdjust();
                waiter->satisfied = true;
                object.SyncGuest();
            }
        }
        waiter->cv.notify_one();
    }
}
#define SIGNAL SignalWaiters
#else
#define SIGNAL NotifyDispatcher
#endif

struct Event final : KernelObject {
    Event(bool m, bool s) : manualReset(m), signaled(s) {}
    bool manualReset, signaled;
    bool IsSignaled(GuestThread*) const override { return signaled; }
    void Acquire(GuestThread*) override { if (!manualReset) signaled = false; }
    int32_t Set() {
        auto lock = LockDispatcher();
        const int32_t previous = signaled;
        signaled = true;
        if (!previous) SIGNAL(*this);
        SyncGuest();
        return previous;
    }
    int32_t Reset() { auto lock = LockDispatcher(); int32_t p = signaled; signaled = false; return p; }
};
struct Semaphore final : KernelObject {
    Semaphore(int32_t c, int32_t l) : count(c), limit(l) {}
    int32_t count, limit;
    bool IsSignaled(GuestThread*) const override { return count > 0; }
    void Acquire(GuestThread*) override { count--; }
    int32_t Release(int32_t adj) {
        auto lock = LockDispatcher();
        const int32_t previous = count;
        if (int64_t(count) + adj > limit) return -1;
        count += adj;
        if (adj > 0) SIGNAL(*this);
        SyncGuest();
        return previous;
    }
};
struct Mutant final : KernelObject {
    GuestThread* owner = nullptr; uint32_t recursion = 0; bool abandoned = false;
    bool IsSignaled(GuestThread* w) const override { return owner == nullptr || owner == w; }
    void Acquire(GuestThread* w) override { owner = w; recursion++; }
    uint32_t WaitStatusAdjust() override { if (abandoned) { abandoned = false; return X_STATUS_ABANDONED_WAIT_0; } return 0; }
    int32_t Release(GuestThread* caller) {
        auto lock = LockDispatcher();
        if (owner != caller) return -1;
        const int32_t previous = int32_t(recursion);
        recursion--;
        if (recursion == 0) owner = nullptr;
        if (!owner) SIGNAL(*this);
        SyncGuest();
        return previous;
    }
};

static std::atomic<int> g_apcPending[64];
uint32_t WaitMultiple(std::span<const ObjectRef> objects, bool waitAll, const int64_t* timeout, bool alertable)
{
    GuestThread* self = t_self;
    const bool infinite = timeout == nullptr;
    const bool poll = timeout && *timeout == 0;
    const auto deadline = infinite || poll ? std::chrono::steady_clock::time_point{} : std::chrono::steady_clock::now() + std::chrono::nanoseconds(-*timeout * 100);
    auto lock = LockDispatcher();
    struct Registration {
        std::span<const ObjectRef> objects;
        GuestThread* apcThread;
#ifdef NEW
        GuestThread* thread;
        bool waitAny;
#endif
        bool registered = false;
        void Add() {
            if (registered) return;
#ifdef NEW
            t_waiter.objects = objects;
            t_waiter.thread = thread;
            t_waiter.waitAny = waitAny;
            t_waiter.satisfied = false;
#endif
            for (const auto& o : objects) AddWaiter(*o);
            registered = true;
        }
        void Remove() {
            if (!registered) return;
            registered = false;
            for (const auto& o : objects) RemoveWaiter(*o);
#ifdef NEW
            t_waiter.objects = {};
            t_waiter.waitAny = false;
            t_waiter.satisfied = false;
#endif
        }
        ~Registration() { Remove(); }
#ifdef NEW
    } registration{ objects, alertable ? self : nullptr, self, !waitAll };
#else
    } registration{ objects, alertable ? self : nullptr };
#endif
    for (;;) {
#ifdef NEW
        if (t_waiter.satisfied) {
            const uint32_t status = t_waiter.status;
            return status;
        }
#endif
        if (waitAll) {
            bool all = true;
            for (const auto& o : objects) all = all && o->IsSignaled(self);
            if (all) {
                uint32_t status = X_STATUS_WAIT_0;
                for (const auto& o : objects) { o->Acquire(self); status |= o->WaitStatusAdjust(); o->SyncGuest(); }
                return status;
            }
        } else {
            for (size_t i = 0; i < objects.size(); i++)
                if (objects[i]->IsSignaled(self)) { objects[i]->Acquire(self); objects[i]->SyncGuest(); return uint32_t(X_STATUS_WAIT_0 + i) + objects[i]->WaitStatusAdjust(); }
        }
        if (poll) return X_STATUS_TIMEOUT;
        registration.Add();
        if (infinite) t_waiter.cv.wait(lock);
        else if (t_waiter.cv.wait_until(lock, deadline) == std::cv_status::timeout
#ifdef NEW
                 && !t_waiter.satisfied
#endif
                 ) {
            bool satisfied = waitAll;
            for (const auto& o : objects) satisfied = waitAll ? satisfied && o->IsSignaled(self) : satisfied || o->IsSignaled(self);
            if (!satisfied) return X_STATUS_TIMEOUT;
        }
    }
}
uint32_t WaitSingle(const ObjectRef& o, const int64_t* t = nullptr) { return WaitMultiple(std::span(&o, 1), false, t, false); }

// Critical section model (RtlEnter/LeaveCriticalSection, host-endian).
struct CS { std::atomic<int32_t> lockCount{ -1 }; std::atomic<int> owner{ 0 }; int recursion = 0; ObjectRef event = std::make_shared<Event>(false, false); };
void Enter(CS& cs) {
    const int self = t_self->id;
    for (int spin = 0; spin < 1000; spin++) {
        int32_t expected = -1;
        if (cs.lockCount.load(std::memory_order_relaxed) == -1 && cs.lockCount.compare_exchange_strong(expected, 0)) { cs.owner = self; cs.recursion = 1; return; }
        if (cs.owner == self) break;
        _mm_pause();
    }
    if (cs.lockCount.fetch_add(1) + 1 != 0) {
        if (cs.owner == self) { cs.recursion++; return; }
        WaitSingle(cs.event);
    }
    cs.owner = self; cs.recursion = 1;
}
void Leave(CS& cs) {
    if (--cs.recursion != 0) { cs.lockCount.fetch_sub(1); return; }
    cs.owner = 0;
    if (cs.lockCount.fetch_sub(1) - 1 >= 0) static_cast<Event*>(cs.event.get())->Set();
}

using namespace std::chrono;
static void Spawn(std::vector<std::thread>& v, int id, auto fn) { v.emplace_back([id, fn] { GuestThread t{ id }; t_self = &t; fn(); }); }

int main() {
    GuestThread mainT{ 1 }; t_self = &mainT;
    // S1: auto-reset event, two waiters, two back-to-back Sets.
    { int woken = 0; for (int rep = 0; rep < 20; rep++) {
        auto e = std::make_shared<Event>(false, false); ObjectRef eo = e; std::atomic<int> n{ 0 }; std::vector<std::thread> ts;
        for (int i = 0; i < 2; i++) Spawn(ts, 10 + i, [&] { int64_t t = -2'000'000; if (WaitSingle(eo, &t) == 0) n++; });
        std::this_thread::sleep_for(20ms); e->Set(); e->Set();
        for (auto& t : ts) t.join(); woken += n; }
      printf("S1 auto-reset, 2 waiters, Set;Set: woken %d of 40 (NT: 40)\n", woken); }
    // S2: one consumer, one item per wake; Set;Set -> both consumed?
    { int got = 0; for (int rep = 0; rep < 20; rep++) {
        auto e = std::make_shared<Event>(false, false); ObjectRef eo = e; std::atomic<int> n{ 0 }; std::vector<std::thread> ts;
        Spawn(ts, 10, [&] { for (int k = 0; k < 2; k++) { int64_t t = -2'000'000; if (WaitSingle(eo, &t) == 0) n++; } });
        std::this_thread::sleep_for(20ms); e->Set(); e->Set();
        for (auto& t : ts) t.join(); got += n; }
      printf("S2 auto-reset, 1 waiter, Set;Set: consumed %d of 40 (NT: 40)\n", got); }
    // S3: manual event, three waiters, Set;Reset.
    { int woken = 0; for (int rep = 0; rep < 20; rep++) {
        auto e = std::make_shared<Event>(true, false); ObjectRef eo = e; std::atomic<int> n{ 0 }; std::vector<std::thread> ts;
        for (int i = 0; i < 3; i++) Spawn(ts, 10 + i, [&] { int64_t t = -2'000'000; if (WaitSingle(eo, &t) == 0) n++; });
        std::this_thread::sleep_for(20ms); e->Set(); e->Reset();
        for (auto& t : ts) t.join(); woken += n; }
      printf("S3 manual, 3 waiters, Set;Reset: woken %d of 60 (NT: 60)\n", woken); }
    // S4: CS barging. O loops Enter/work/Leave; A enters every 5 ms; how many O cycles pass before A gets it.
    { CS cs; std::atomic<bool> stop{ false }; std::atomic<long> oIter{ 0 }; std::vector<std::thread> ts;
      Spawn(ts, 20, [&] { while (!stop) { Enter(cs); { auto t0 = steady_clock::now(); while (steady_clock::now() - t0 < 50us); } Leave(cs); oIter++; } });
      std::vector<long> passes; std::vector<double> lat;
      Spawn(ts, 21, [&] { for (int r = 0; r < 100; r++) { std::this_thread::sleep_for(5ms); long b = oIter; auto t0 = steady_clock::now(); Enter(cs); lat.push_back(duration<double, std::micro>(steady_clock::now() - t0).count()); passes.push_back(oIter - b); Leave(cs); } stop = true; });
      for (auto& t : ts) t.join();
      std::sort(passes.begin(), passes.end()); std::sort(lat.begin(), lat.end());
      printf("S4 CS barging: O cycles while A waits: median %ld p90 %ld max %ld; A latency us median %.0f max %.0f; O total %ld\n",
          passes[50], passes[90], passes.back(), lat[50], lat.back(), oIter.load()); }
    // S5: stress invariants (CS exclusion, semaphore tokens, mutant exclusion, WaitAny w/ timeouts, WaitAll).
    { CS cs; long shared = 0; auto sem = std::make_shared<Semaphore>(3, 3); ObjectRef so = sem; auto ev = std::make_shared<Event>(false, false); ObjectRef eo = ev;
      auto mu = std::make_shared<Mutant>(); ObjectRef mo = mu; std::atomic<int> held{ 0 }, maxHeld{ 0 }, inMutant{ 0 }, bad{ 0 }; std::atomic<long> evGot{ 0 }, evSet{ 0 }, csOps{ 0 };
      std::vector<std::thread> ts;
      for (int i = 0; i < 8; i++) Spawn(ts, 30 + i, [&, i] { std::mt19937 rng(i); GuestThread* me = t_self;
        for (int k = 0; k < 20000; k++) {
          switch (rng() % 5) {
          case 0: Enter(cs); shared++; Leave(cs); csOps++; break;
          case 1: { ObjectRef list[2] = { so, eo }; int64_t t = -int64_t(rng() % 3000); uint32_t s = WaitMultiple(list, false, (rng() & 1) ? &t : nullptr, false);
                    if (s == 0) { int h = ++held; int m = maxHeld; while (h > m && !maxHeld.compare_exchange_weak(m, h)); --held; if (sem->Release(1) < 0) bad++; }
                    else if (s == 1) evGot++; else if (s != X_STATUS_TIMEOUT) bad++; } break;
          case 2: { int64_t t = -int64_t(rng() % 3000); uint32_t s = WaitSingle(mo, (rng() & 1) ? &t : nullptr); if (s == 0) { if (++inMutant != 1) bad++; std::this_thread::yield(); --inMutant; mu->Release(me); } else if (s != X_STATUS_TIMEOUT) bad++; } break;
          case 3: if (ev->Set() == 0) evSet++; break;
          case 4: { ObjectRef list[2] = { so, mo }; int64_t t = -int64_t(rng() % 3000); uint32_t s = WaitMultiple(list, true, &t, false);
                    if (s == 0) { if (++inMutant != 1) bad++; --inMutant; mu->Release(me); sem->Release(1); } else if (s != X_STATUS_TIMEOUT) bad++; } break;
          } } });
      for (auto& t : ts) t.join();
      long leftover = ev->signaled ? 1 : 0;
      printf("S5 stress: shared %ld (expect %s), sem count %d (expect 3), maxHeld %d (<=3), mutant owner %p rec %u, bad %d, event sets %ld = got %ld + leftover %ld\n",
          shared, std::to_string(csOps.load()).c_str(), sem->count, maxHeld.load(), (void*)mu->owner, mu->recursion, bad.load(), evSet.load(), evGot.load(), leftover); }

    // S6: throughput: 4 threads hammer one CS (hold 1us, 2us outside) for 1 s.
    { CS cs; std::atomic<bool> stop{ false }; std::atomic<long> ops{ 0 }; std::vector<std::thread> ts; std::vector<long> per(4);
      for (int i = 0; i < 4; i++) Spawn(ts, 40 + i, [&, i] { long n = 0; while (!stop) { Enter(cs); auto t0 = steady_clock::now(); while (steady_clock::now() - t0 < 20us); Leave(cs); t0 = steady_clock::now(); while (steady_clock::now() - t0 < 0us); n++; } per[i] = n; ops += n; });
      std::this_thread::sleep_for(1s); stop = true; for (auto& t : ts) t.join();
      printf("S6 CS throughput 4 threads: %ld ops/s, per thread %ld %ld %ld %ld\n", ops.load(), per[0], per[1], per[2], per[3]); }
}
```

</details>
