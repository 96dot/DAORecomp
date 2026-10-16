// Conversations' held shots (title update code only): while a shot is held, a
// render job runs every frame, setting a "running" byte at its start and clearing
// it at its end. To end the hold, the update thread polls that byte with Sleep(0)
// until it reads clear. But the job queues itself again at once, so the byte is
// clear for only a few microseconds per frame. Once a run of Sleep(0) calls is
// long, each one sleeps kSpinSleepMicroseconds (DelayExecution in
// kernel/objects.cpp, to save power), so the update thread missed that window for
// seconds at a time, and the shot froze until it caught it. So while the update
// thread waits for the job, the job waits at its end (byte clear) until the update
// thread has seen it. The disc build's code ends a hold without waiting for a job,
// and without Title Update 6's code this file compiles to nothing.
#include "stdafx.h"
#include "kernel/function.h"
#include "kernel/game_build.h"
#include "kernel/objects.h"

#if DAO_HAS_TU6
namespace
{
    // The job the update thread is waiting for (guest address), or 0.
    std::atomic<uint32_t> s_awaitedJob{ 0 };
    // The thread waiting for it (host id): a job it runs itself is never held, or
    // it would wait for itself.
    std::atomic<DWORD> s_awaitingThread{ 0 };

    // The longest a run of the job waits for the update thread, in case it never
    // looks: a few frames.
    constexpr auto kHandOverTimeout = std::chrono::milliseconds(50);

    // The shot's fields the update thread reads (from the TU6 code): set while
    // held, and the job.
    constexpr uint32_t kShotHeldOffset = 386;
    constexpr uint32_t kShotJobOffset = 388;

    // The update thread ending the hold: r3 is the shot.
    void EndHoldHook(PPCContext& ctx, uint8_t* base, PPCFunc* original)
    {
        const uint32_t shot = ctx.r3.u32;
        uint32_t job = PPC_LOAD_U8(shot + kShotHeldOffset) ? PPC_LOAD_U32(shot + kShotJobOffset) : 0;
        DWORD thread = GetCurrentThreadId();
        s_awaitedJob = job;
        s_awaitingThread = thread;
        original(ctx, base);
        // Cleared only if still this call's: should another thread have ended a hold
        // meanwhile, nothing is left awaited with no one waiting.
        s_awaitingThread.compare_exchange_strong(thread, 0);
        s_awaitedJob.compare_exchange_strong(job, 0);
    }

    // The render job: r3 is the job. Once it returns, its running byte is clear.
    void HeldShotJobHook(PPCContext& ctx, uint8_t* base, PPCFunc* original)
    {
        const uint32_t job = ctx.r3.u32;
        original(ctx, base);
        if (!job || s_awaitedJob != job || s_awaitingThread == GetCurrentThreadId())
            return;
        const auto deadline = std::chrono::steady_clock::now() + kHandOverTimeout;
        while (s_awaitedJob == job && std::chrono::steady_clock::now() < deadline)
            dao::kernel::ShortSleep(dao::kernel::kSpinSleepMicroseconds);
    }
}
GAME_HOOK_ONE(tu6_sub_82557DE0, EndHoldHook)
GAME_HOOK_ONE(tu6_sub_82555060, HeldShotJobHook)
#endif
