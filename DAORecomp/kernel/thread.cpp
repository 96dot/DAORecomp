#include "stdafx.h"
#include <fstream>
#include "thread.h"
#include "text.h"
#include "memory.h"

namespace dao::kernel
{
    uint32_t g_processBlock;

    static thread_local GuestThread* t_current;
    static thread_local PPCContext* t_context;

    static std::mutex s_threadsMutex;
    static std::unordered_map<uint32_t, std::shared_ptr<GuestThread>> s_threads; // by threadId, until exit
    static std::unordered_map<uint32_t, std::weak_ptr<GuestThread>> s_byKThread;
    static std::atomic<uint32_t> s_nextThreadId{ 1 };

    // TLS layout per thread: [static .tls block][dynamic slots]
    static uint32_t s_tlsSlotCount = 64;
    static uint32_t s_tlsRawAddress, s_tlsRawSize, s_tlsDataSize;
    static std::mutex s_tlsMutex;
    static std::vector<bool> s_tlsUsed;

    constexpr uint32_t kStackRedZone = 0x100;
    constexpr uint32_t kHostStackSize = 16 * 1024 * 1024;

    // ------------------------------------------------------------- lifetime

    GuestThread::~GuestThread()
    {
        if (stackAlloc)
            VirtualHeap64K().Release(stackAlloc);
        if (pcr)
            KernelFree(pcr);
        if (tlsStatic)
            KernelFree(tlsStatic);
        if (context)
        {
            context->~PPCContext();
            _aligned_free(context);
        }
        if (hostHandle)
            ::CloseHandle(hostHandle);
        // kthread is guestObject (freed by ~KernelObject).
    }

    GuestThread* GuestThread::Current()
    {
        return t_current;
    }

    std::shared_ptr<GuestThread> GuestThread::CurrentRef()
    {
        return t_current ? std::static_pointer_cast<GuestThread>(t_current->shared_from_this()) : nullptr;
    }

    PPCContext* CurrentContext()
    {
        return t_context;
    }

    bool GuestThread::Initialize()
    {
        threadId = s_nextThreadId++;

        // Guest stack from the top of the 64 KB region, like the console.
        stackAllocSize = (std::max<uint32_t>(params.stackSize, 0x10000) + 0xFFFF) & ~0xFFFFu;
        stackAlloc = VirtualHeap64K().Alloc(stackAllocSize, 0x10000, true, X_PAGE_READWRITE, true, 0x70000000, 0x7F000000);
        if (!stackAlloc)
        {
            LOGE("Failed to allocate a {:#x} byte guest stack", stackAllocSize);
            return false;
        }
        stackLimit = stackAlloc;
        stackBase = stackAlloc + stackAllocSize;

        // TLS: static block (initialised from the XEX .tls raw data) followed by the slots.
        const uint32_t staticSize = (s_tlsDataSize + 15) & ~15u;
        tlsStatic = KernelAlloc(staticSize + s_tlsSlotCount * 4);
        tlsDynamic = tlsStatic + staticSize;
        pcr = KernelAlloc(kPcrSize);
        kthread = KernelAlloc(kKThreadSize);
        if (!tlsStatic || !pcr || !kthread)
        {
            LOGE("Out of kernel memory for a thread's TLS, PCR and KTHREAD");
            return false;
        }
        if (s_tlsRawSize)
            memcpy(g_memory.Translate(tlsStatic), g_memory.Translate(s_tlsRawAddress), s_tlsRawSize);

        auto w32 = [](uint32_t address, uint32_t value) { *g_memory.Translate<u32>(address) = value; };
        auto w8 = [](uint32_t address, uint8_t value) { *g_memory.Translate<uint8_t>(address) = value; };

        const uint32_t cpu = (params.creationFlags >> 24) ? std::countr_zero(params.creationFlags >> 24) : 0;

        w32(pcr + kPcrTlsPtr, tlsStatic);
        w32(pcr + kPcrPcrPtr, pcr);
        w32(pcr + kPcrStackBase, stackBase);
        w32(pcr + kPcrStackEnd, stackLimit);
        w32(pcr + kPcrCurrentThread, kthread);
        w8(pcr + kPcrCurrentCpu, uint8_t(cpu));
        w32(pcr + kPcrDpcActive, 0);
        w32(pcr + kPcrPrcb, pcr + 0x100);

        auto* header = g_memory.Translate<X_DISPATCH_HEADER>(kthread);
        header->type = kTypeThread;
        header->size = uint8_t(kKThreadSize / 4);
        header->signalState = 0;
        w32(kthread + kKThreadStackBase, stackBase);
        w32(kthread + kKThreadStackLimit, stackLimit);
        w32(kthread + kKThreadStackKernel, stackBase - 0xF0);
        w32(kthread + kKThreadTlsAddress, tlsDynamic);
        w8(kthread + kKThreadState, 1);
        w8(kthread + kKThreadPriority, 9);
        w8(kthread + kKThreadProcessType, 1); // title
        w32(kthread + kKThreadProcess, g_processBlock);
        w8(kthread + kKThreadCurrentCpu, uint8_t(cpu));
        w32(kthread + kKThreadThreadId, threadId);
        w32(kthread + kKThreadStartAddress, params.startAddress);
        w32(kthread + kKThreadCreationFlags, params.creationFlags);
        *g_memory.Translate<u64>(kthread + kKThreadCreateTime) = SystemTime100ns();

        guestObject = kthread;
        ownsGuestObject = true;

        void* storage = _aligned_malloc(sizeof(PPCContext), alignof(PPCContext));
        context = new (storage) PPCContext{};
        context->r1.u64 = stackBase - kStackRedZone;
        context->r13.u64 = pcr;
        context->fpscr.loadFromHost();

        affinity = params.creationFlags >> 24;
        return true;
    }

    static void Register(const std::shared_ptr<GuestThread>& thread)
    {
        BindGuestHeader(thread, thread->kthread);
        std::lock_guard lock(s_threadsMutex);
        s_threads[thread->threadId] = thread;
        s_byKThread[thread->kthread] = thread;
    }

    static void Unregister(GuestThread* thread)
    {
        std::shared_ptr<GuestThread> keep;
        std::lock_guard lock(s_threadsMutex);
        auto it = s_threads.find(thread->threadId);
        if (it != s_threads.end())
        {
            keep = std::move(it->second); // released after the lock
            s_threads.erase(it);
        }
        // (Unless a newer thread already has a KTHREAD at the same address.)
        if (auto k = s_byKThread.find(thread->kthread);
            k != s_byKThread.end() && (k->second.expired() || k->second.lock().get() == thread))
            s_byKThread.erase(k);
    }

    std::shared_ptr<GuestThread> ThreadFromKThread(uint32_t kthread)
    {
        std::lock_guard lock(s_threadsMutex);
        auto it = s_byKThread.find(kthread);
        return it != s_byKThread.end() ? it->second.lock() : nullptr;
    }

    void NoteImport(const char* name, const PPCContext& ctx)
    {
        if (auto* thread = t_current)
        {
            thread->lastImport = name;
            thread->lastImportLr = uint32_t(ctx.lr);
            thread->lastImportR3 = ctx.r3.u32;
        }
    }

    // Fills `offsets` with up to `count` return addresses from `context`; returns
    // how many. No C++ objects here: the unwinding is guarded with SEH (a
    // corrupted stack can fault).
    static int UnwindOffsets(CONTEXT& context, uint64_t* offsets, int count)
    {
        const auto exeBase = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
        int n = 0;
        __try
        {
            while (n < count && context.Rip)
            {
                offsets[n++] = context.Rip - exeBase;
                DWORD64 imageBase;
                auto* entry = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
                if (!entry)
                    break;
                void* handlerData;
                DWORD64 establisher;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, entry, &context, &handlerData,
                    &establisher, nullptr);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return n;
    }

    static constexpr int kMaxStackDepth = 64;

    static std::string FormatOffsets(const uint64_t* offsets, int n)
    {
        std::string out;
        for (int i = 0; i < n; i++)
            out += fmt::format(" {:X}", offsets[i]);
        return out;
    }

    std::string UnwindHostStack(CONTEXT& context, int count)
    {
        uint64_t offsets[kMaxStackDepth];
        return FormatOffsets(offsets, UnwindOffsets(context, offsets, std::min(count, kMaxStackDepth)));
    }

    std::string SampleHostStack(uint32_t hostThreadId, int count)
    {
        HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, hostThreadId);
        if (!handle)
            return "?";
        // Nothing allocated while the thread is suspended: it may hold the heap
        // lock.
        uint64_t offsets[kMaxStackDepth];
        int n = 0;
        if (SuspendThread(handle) != DWORD(-1))
        {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(handle, &context))
                n = UnwindOffsets(context, offsets, std::min(count, kMaxStackDepth));
            ResumeThread(handle);
        }
        ::CloseHandle(handle);
        return FormatOffsets(offsets, n);
    }

    void DumpThreads()
    {
        ForEachThread([](GuestThread& t) {
            const std::string name = t.params.name ? t.params.name : fmt::format("{:08X}", t.params.startAddress);
            LOGI("thread {:>2} {:<28} last import {} (r3 {:08X}) lr {:08X}{}", t.threadId, name,
                t.lastImport ? t.lastImport : "-", t.lastImportR3, t.lastImportLr, t.terminated ? " [exited]" : "");
            if (!t.terminated && t.hostThreadId)
                LOGI("    host stack:{}", SampleHostStack(t.hostThreadId, 12));
        });
    }

    void ProfileThread(uint32_t threadId, int milliseconds, const std::filesystem::path& out)
    {
        uint32_t hostThreadId = 0;
        ForEachThread([&](GuestThread& t) {
            if (t.threadId == threadId && !t.terminated)
                hostThreadId = t.hostThreadId;
        });
        if (!hostThreadId)
            return;
        std::ofstream file(out);
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        int samples = 0;
        while (std::chrono::steady_clock::now() < end)
        {
            file << "host stack:" << SampleHostStack(hostThreadId, 24) << '\n';
            samples++;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        LOGI("Profiled thread {}: {} samples -> {}", threadId, samples, Utf8(out));
    }

    void ForEachThread(const std::function<void(GuestThread&)>& fn)
    {
        std::vector<std::shared_ptr<GuestThread>> threads;
        {
            std::lock_guard lock(s_threadsMutex);
            for (auto& [id, thread] : s_threads)
                threads.push_back(thread);
        }
        for (auto& thread : threads)
            fn(*thread);
    }

    // ------------------------------------------------------------- running

    uint32_t GuestThread::Run()
    {
        t_current = this;
        t_context = context;

        {
            const std::string name = params.name ? std::string(params.name)
                : fmt::format("Guest {} ({:08X})", threadId, params.startAddress);
            const std::wstring wide(name.begin(), name.end());
            SetThreadDescription(GetCurrentThread(), wide.c_str());
        }

        // Wait out a create-suspended start.
        {
            auto lock = LockDispatcher();
            while (suspendCount > 0)
                WaitDispatcherChange(lock, *this);
        }

        LOGI("Thread {} start {:08X}({:08X}) stack {:08X}-{:08X}", threadId, params.startAddress,
            params.startContext, stackLimit, stackBase);

        uint32_t code = 0;
        try
        {
            if (params.xapiStartup)
                code = uint32_t(CallGuest(params.xapiStartup, { params.startAddress, params.startContext }));
            else
                code = uint32_t(CallGuest(params.startAddress, { params.startContext }));
        }
        catch (const ThreadExitException& e)
        {
            code = e.exitCode;
        }

        LOGI("Thread {} exited with {:#x}", threadId, code);
        {
            auto lock = LockDispatcher();
            exitCode = code;
            terminated = true;
            SyncGuest();
            g_memory.Translate<X_DISPATCH_HEADER>(kthread)->signalState = 1;
            NotifyDispatcher(*this);
        }

        t_current = nullptr;
        t_context = nullptr;
        return code;
    }

    std::shared_ptr<GuestThread> GuestThread::Create(const ThreadParams& params)
    {
        auto thread = std::make_shared<GuestThread>();
        thread->params = params;
        thread->suspendCount = (params.creationFlags & 1) ? 1 : 0;
        if (!thread->Initialize())
            return nullptr;
        Register(thread);

        if (!params.isMain)
        {
            struct Start { std::shared_ptr<GuestThread> thread; };
            auto* start = new Start{ thread };
            DWORD hostId = 0;
            thread->hostHandle = ::CreateThread(nullptr, kHostStackSize, [](void* p) -> DWORD {
                std::unique_ptr<Start> start(static_cast<Start*>(p));
                auto self = start->thread;
                const uint32_t code = self->Run();
                Unregister(self.get());
                return code;
            }, start, STACK_SIZE_PARAM_IS_A_RESERVATION, &hostId);
            if (!thread->hostHandle)
            {
                delete start;
                Unregister(thread.get());
                return nullptr;
            }
            thread->hostThreadId = hostId;
        }
        return thread;
    }

    uint32_t GuestThread::RunOnCurrentHostThread()
    {
        hostThreadId = GetCurrentThreadId();
        const uint32_t code = Run();
        Unregister(this);
        return code;
    }

    GuestThread* GuestThread::AttachHostThread(const char* name)
    {
        if (t_current)
            return t_current;

        ThreadParams params;
        params.stackSize = 0x40000;
        params.name = name;
        auto thread = std::make_shared<GuestThread>();
        thread->params = params;
        if (!thread->Initialize())
            return nullptr;
        Register(thread);
        thread->hostThreadId = GetCurrentThreadId();
        t_current = thread.get();
        t_context = thread->context;
        LOGI("Attached host thread '{}' as guest thread {}", name, thread->threadId);
        return thread.get();
    }

    int32_t GuestThread::Resume()
    {
        auto lock = LockDispatcher();
        const int32_t previous = suspendCount;
        if (suspendCount > 0)
            suspendCount--;
        NotifyDispatcher(*this);
        return previous;
    }

    void GuestThread::QueueApc(const Apc& apc)
    {
        {
            auto lock = LockDispatcher();
            apcQueue.push_back(apc);
            NotifyDispatcher(*this);
        }
    }

    bool GuestThread::DeliverApcs()
    {
        bool any = false;
        for (;;)
        {
            Apc apc;
            {
                auto lock = LockDispatcher();
                if (apcQueue.empty())
                    break;
                apc = apcQueue.front();
                apcQueue.pop_front();
            }
            any = true;

            if (apc.guestApc)
                *g_memory.Translate<uint8_t>(apc.guestApc + 3) = 0; // enqueued = false

            if (apc.kernelRoutine)
            {
                // KernelRoutine may rewrite the normal routine and its arguments.
                const uint32_t scratch = KernelAlloc(16);
                auto* s = g_memory.Translate<u32>(scratch);
                s[0] = apc.normalRoutine;
                s[1] = apc.normalContext;
                s[2] = apc.arg1;
                s[3] = apc.arg2;
                CallGuest(apc.kernelRoutine, { apc.guestApc, scratch, scratch + 4, scratch + 8, scratch + 12 });
                apc.normalRoutine = s[0];
                apc.normalContext = s[1];
                apc.arg1 = s[2];
                apc.arg2 = s[3];
                KernelFree(scratch);
            }

            if (apc.normalRoutine)
                CallGuest(apc.normalRoutine, { apc.normalContext, apc.arg1, apc.arg2 });
        }
        return any;
    }

    void GuestThread::Exit(uint32_t exitCode)
    {
        throw ThreadExitException{ exitCode };
    }

    void GuestThread::SetLastError(uint32_t error)
    {
        *g_memory.Translate<u32>(kthread + kKThreadLastError) = error;
    }

    uint32_t GuestThread::GetLastError() const
    {
        return *g_memory.Translate<u32>(kthread + kKThreadLastError);
    }

    // ------------------------------------------------------------- calls

    uint64_t CallGuest(uint32_t function, std::initializer_list<uint64_t> args)
    {
        PPCContext* current = t_context;
        if (!current)
        {
            LOGE("CallGuest({:08X}) from a thread without a guest context", function);
            __debugbreak();
            return 0;
        }

        PPCFunc* host = g_memory.FindFunction(function);
        if (!host)
        {
            LOGE("CallGuest: no recompiled function at {:08X}", function);
            __debugbreak();
            return 0;
        }

        alignas(PPCContext) PPCContext ctx;
        ctx.r1.u64 = (current->r1.u32 - kStackRedZone) & ~0xFull;
        ctx.r13 = current->r13;
        ctx.fpscr = current->fpscr;
        ctx.lr = 0;

        PPCRegister* regs[] = { &ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10 };
        size_t i = 0;
        for (uint64_t arg : args)
        {
            if (i < std::size(regs))
                regs[i++]->u64 = arg;
        }

        t_context = &ctx;
        struct Restore
        {
            PPCContext* current;
            PPCContext* ctx;
            ~Restore()
            {
                current->fpscr = ctx->fpscr;
                t_context = current;
            }
        } restore{ current, &ctx };

        host(ctx, g_memory.base);
        return ctx.r3.u64;
    }

    // ------------------------------------------------------------- TLS

    void ConfigureTls(uint32_t slotCount, uint32_t rawDataAddress, uint32_t rawDataSize, uint32_t dataSize)
    {
        s_tlsSlotCount = std::max<uint32_t>(slotCount, 64);
        s_tlsRawAddress = rawDataAddress;
        s_tlsRawSize = rawDataSize;
        s_tlsDataSize = std::max(dataSize, rawDataSize);
        s_tlsUsed.assign(s_tlsSlotCount, false);
        LOGI("TLS: {} slots, raw {:08X}+{:#x}, data {:#x}", s_tlsSlotCount, rawDataAddress, rawDataSize, dataSize);
    }

    uint32_t TlsAlloc()
    {
        std::lock_guard lock(s_tlsMutex);
        for (uint32_t i = 0; i < s_tlsUsed.size(); i++)
        {
            if (!s_tlsUsed[i])
            {
                s_tlsUsed[i] = true;
                return i;
            }
        }
        LOGE("Out of TLS slots");
        return 0xFFFFFFFF;
    }

    bool TlsFree(uint32_t slot)
    {
        {
            std::lock_guard lock(s_tlsMutex);
            if (slot >= s_tlsUsed.size() || !s_tlsUsed[slot])
                return false;
            s_tlsUsed[slot] = false;
        }
        ForEachThread([slot](GuestThread& thread) {
            g_memory.Translate<u32>(thread.tlsDynamic)[slot] = 0;
        });
        return true;
    }

    u32& TlsSlotRef(uint32_t slot)
    {
        static u32 dummy;
        auto* thread = GuestThread::Current();
        if (!thread || slot >= s_tlsSlotCount)
            return dummy;
        return g_memory.Translate<u32>(thread->tlsDynamic)[slot];
    }
}
