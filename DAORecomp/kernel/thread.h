#pragma once

#include "objects.h"

namespace dao::kernel
{
    struct Apc
    {
        uint32_t kernelRoutine;  // KernelRoutine(apc, &normal, &context, &arg1, &arg2), optional
        uint32_t normalRoutine;  // NormalRoutine(context, arg1, arg2), optional
        uint32_t normalContext;
        uint32_t arg1;
        uint32_t arg2;
        uint32_t guestApc;       // guest KAPC, 0 for host-queued APCs (timers)
    };

    struct ThreadParams
    {
        uint32_t stackSize = 0x40000;
        uint32_t startAddress = 0;
        uint32_t startContext = 0;
        uint32_t xapiStartup = 0;   // XapiThreadStartup(startAddress, startContext) when set
        uint32_t creationFlags = 0; // bit 0: suspended; bits 24-31: processor mask
        bool isMain = false;
        const char* name = nullptr;
    };

    // Thrown by ExTerminateThread to unwind out of recompiled code.
    struct ThreadExitException
    {
        uint32_t exitCode;
    };

    struct GuestThread final : KernelObject
    {
        GuestThread() : KernelObject(ObjectType::Thread) {}
        ~GuestThread() override;

        uint32_t threadId = 0;
        uint32_t pcr = 0;
        uint32_t kthread = 0;
        uint32_t tlsStatic = 0;
        uint32_t tlsDynamic = 0;
        uint32_t stackAlloc = 0, stackAllocSize = 0;
        uint32_t stackBase = 0;  // high address
        uint32_t stackLimit = 0; // low address
        ThreadParams params;

        HANDLE hostHandle = nullptr;
        uint32_t hostThreadId = 0;
        PPCContext* context = nullptr; // root context of the thread

        // Guarded by the dispatcher lock.
        int32_t suspendCount = 0;
        bool terminated = false;
        uint32_t exitCode = 0;
        std::deque<Apc> apcQueue;

        int32_t basePriority = 0;
        uint32_t affinity = 0;

        // Debugging: the most recent import this thread called.
        const char* lastImport = nullptr;
        uint32_t lastImportLr = 0;
        uint32_t lastImportR3 = 0;

        bool IsWaitable() const override { return true; }
        bool IsSignaled(GuestThread*) const override { return terminated; }

        static GuestThread* Current();
        static std::shared_ptr<GuestThread> CurrentRef();

        // Creates the guest structures and the host thread (which waits while suspended).
        static std::shared_ptr<GuestThread> Create(const ThreadParams& params);

        // Runs the thread body on the calling host thread (used for the main thread).
        uint32_t RunOnCurrentHostThread();

        // Gives a host-only thread (audio, timers, GPU interrupts) a guest identity
        // so it can call guest code.
        static GuestThread* AttachHostThread(const char* name);

        int32_t Resume();
        void QueueApc(const Apc& apc); // user-mode APC, delivered in alertable waits
        bool DeliverApcs();            // returns true if any ran; call without the dispatcher lock

        [[noreturn]] static void Exit(uint32_t exitCode);

        void SetLastError(uint32_t error);
        uint32_t GetLastError() const;

    private:
        bool Initialize();
        uint32_t Run();
    };

    std::shared_ptr<GuestThread> ThreadFromKThread(uint32_t kthread);

    // Records the import a thread is in (for DumpThreads).
    void NoteImport(const char* name, const PPCContext& ctx);
    // Logs every guest thread's last import; used by the DAO_DUMP_AFTER watchdog.
    void DumpThreads();
    // Samples a guest thread's host stack every millisecond for `milliseconds` and
    // writes one "host stack:" line per sample to `out` (see scripts/profile.py).
    void ProfileThread(uint32_t threadId, int milliseconds, const std::filesystem::path& out);
    // Up to `count` return addresses from `context` (unwound through .pdata), as
    // " <offset from the exe base>" each, for scripts/symbolize.py. Stops early,
    // without failing, if the stack can't be read.
    std::string UnwindHostStack(CONTEXT& context, int count);
    void ForEachThread(const std::function<void(GuestThread&)>& fn);

    // Calls a guest function on the current thread. Integer/pointer args go in r3..r10.
    uint64_t CallGuest(uint32_t function, std::initializer_list<uint64_t> args = {});

    // The context guest code is currently running with on this host thread.
    PPCContext* CurrentContext();

    // TLS slots for KeTlsAlloc (count comes from the XEX TLS header).
    void ConfigureTls(uint32_t slotCount, uint32_t rawDataAddress, uint32_t rawDataSize, uint32_t dataSize);
    uint32_t TlsAlloc();
    bool TlsFree(uint32_t slot);
    u32& TlsSlotRef(uint32_t slot); // slot storage in the current thread's TLS

    extern uint32_t g_processBlock; // KPROCESS-ish block for KTHREAD+0x84
}
