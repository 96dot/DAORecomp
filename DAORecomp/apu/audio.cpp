// XAudio render-driver clients and the XMA context pool.
//
// Render: the game's statically linked XAudio2 registers a render callback. A host worker paces
// each registered callback at the console's 5.333 ms frame interval; the 6x256 frames it submits
// are queued for a host audio backend (dao::apu::PopFrame).
//
// XMA: contexts live in physical memory. A decoder worker runs a pass on each enabled context.
// The default pass is the real decoder (apu/xma_decoder.cpp, a port of Xenia's XmaContextNew on
// FFmpeg's xmaframes codec). DAO_XMA=fake selects the fake pass below, which only writes silence
// and is kept for debugging.
#include "stdafx.h"
#include "apu/audio.h"
#include "apu/xma_decoder.h"
#include "gpu/xenia_host.h"
#include "kernel/function.h"
#include "kernel/memory.h"
#include "kernel/thread.h"
#include "kernel/types.h"

// Logs the first few calls of an import; later calls are silent.
#define APU_LOG_FIRST(...)                                                    \
    do                                                                        \
    {                                                                         \
        static std::atomic<uint32_t> s_calls{ 0 };                            \
        if (s_calls.fetch_add(1, std::memory_order_relaxed) < 3)              \
            LOGI(__VA_ARGS__);                                                \
    } while (0)

namespace dao::apu
{
    using namespace dao::kernel;

    namespace
    {
        constexpr uint32_t X_E_INVALIDARG = 0x80070057;
        constexpr uint32_t X_E_OUTOFMEMORY = 0x8007000E;
        constexpr uint32_t X_E_FALSE = 0x80000000;

        // ------------------------------------------------------------ render clients

        // Driver handles are 0x4155xxxx; the low 16 bits are the client index.
        constexpr uint32_t kDriverTag = 0x41550000;
        constexpr uint32_t kMaxClients = 8;
        constexpr uint32_t kInvalidClient = kMaxClients;

        // Xenia's default X_AUDIO_FLAGS: DolbyDigital | DolbyProLogic.
        constexpr uint32_t kSpeakerConfig = 0x00010001;

        // The worker skips the callback while this many frames wait for the host.
        constexpr size_t kWorkerQueueLimit = 8;
        // If nothing consumes the queue, the oldest frames are dropped beyond this.
        constexpr size_t kQueueDropLimit = 16;

        using Frame = std::array<float, kFrameSampleCount>;

        std::mutex g_frameMutex;
        std::deque<Frame> g_frames;

        struct Client
        {
            bool inUse = false;
            uint32_t callback = 0;
            uint32_t callbackArg = 0; // guest cell holding the callback argument (see XAudioRegisterRenderDriverClient)
        };

        // Recursive: the worker holds it while running a guest callback, and that callback's
        // XAudioSubmitRenderDriverFrame re-enters it on the same thread.
        std::recursive_mutex g_clientMutex;
        Client g_clients[kMaxClients];

        uint32_t ClientIndex(uint32_t driver)
        {
            if ((driver & 0xFFFF0000) != kDriverTag)
                return kInvalidClient;
            const uint32_t index = driver & 0xFFFF;
            return index < kMaxClients ? index : kInvalidClient;
        }

        // Frame n is due at origin + n * 256 / 48000 s. 256 / 48000 s = 16000000 / 3 ns.
        // Deadlines come from the origin rather than from the previous wake, so no drift accumulates.
        std::chrono::nanoseconds FrameOffset(uint64_t frame)
        {
            return std::chrono::nanoseconds(frame * 16'000'000ull / 3);
        }

        void LogQueueFullOnce()
        {
            static std::atomic<bool> s_logged{ false };
            if (!s_logged.exchange(true))
                LOGW("XAudio: frame queue full ({} frames), skipping render callbacks until it drains", kWorkerQueueLimit);
        }

        void RunClientCallbacks()
        {
            std::lock_guard lock(g_clientMutex);
            if (QueuedFrames() >= kWorkerQueueLimit)
            {
                LogQueueFullOnce();
                return;
            }
            for (Client& client : g_clients)
            {
                if (client.inUse)
                    CallGuest(client.callback, { client.callbackArg });
            }
        }

        void RenderWorkerMain()
        {
            GuestThread::AttachHostThread("XAudio worker");
            SetThreadDescription(GetCurrentThread(), L"XAudio worker");

            using Clock = std::chrono::steady_clock;
            // If the worker falls this far behind, restart the schedule rather than bursting callbacks.
            constexpr auto kMaxLag = std::chrono::milliseconds(20);

            Clock::time_point origin = Clock::now();
            uint64_t frame = 0;
            for (;;)
            {
                std::this_thread::sleep_until(origin + FrameOffset(frame));
                RunClientCallbacks();
                ++frame;

                const Clock::time_point now = Clock::now();
                if (now - (origin + FrameOffset(frame)) > kMaxLag)
                {
                    origin = now;
                    frame = 0;
                }
            }
        }

        void EnsureRenderWorkerStarted()
        {
            static std::once_flag s_started;
            std::call_once(s_started, [] {
                std::thread(RenderWorkerMain).detach();
                LOGI("XAudio render worker started (48 kHz, 256-sample frames)");
            });
        }

        // ------------------------------------------------------------ XMA pool geometry

        constexpr uint32_t kXmaContextCount = 320;
        constexpr uint32_t kXmaContextSize = 64;
        constexpr uint32_t kXmaContextAlignment = 256;
        constexpr uint32_t kXmaMaxBlocksPerPacket = 8; // staging limit of the fake decoder

        // Guest XMA_CONTEXT_INIT, as read by XMAInitializeContext.
        struct XmaLoopData
        {
            u32 loopStart;
            u32 loopEnd;
            uint8_t loopCount;
            uint8_t loopSubframeEnd;
            uint8_t loopSubframeSkip;
        };
        static_assert(sizeof(XmaLoopData) == 12);

        struct XmaContextInit
        {
            u32 input0Ptr;
            u32 input0PacketCount;
            u32 input1Ptr;
            u32 input1PacketCount;
            u32 inputReadOffset;
            u32 outputPtr;
            u32 outputBlockCount;
            u32 workBuffer;
            u32 subframeDecodeCount;
            u32 channelCount;
            u32 sampleRate;
            XmaLoopData loop;
        };
        static_assert(sizeof(XmaContextInit) == 56);

        u32* ContextDwords(uint32_t address)
        {
            return address ? g_memory.Translate<u32>(address) : nullptr;
        }

        // Dword value stored in the context for a buffer address: physical for physical-view
        // addresses, as Xenia stores it. The decoders use the guest addresses cached in XmaSlot.
        uint32_t ToPhysical(uint32_t guest)
        {
            if (!guest)
                return 0;
            return IsPhysicalAddress(guest) ? PhysicalAddressOf(guest) : guest;
        }

        // ------------------------------------------------------------ fake packet decoder

        // Fake decoder boundary: Decode() stages the output of one packet, and TakeBlocks() hands
        // it out as 256-byte blocks. The fake stages silence, 4 blocks per packet for mono and 8 for
        // stereo, the fixed count Xenia's XmaContextFake uses.
        class XmaPacketDecoder
        {
        public:
            // Called only when nothing is pending.
            void Decode(const uint8_t* packet, bool stereo)
            {
                (void)packet; // the fake decoder ignores the packet data
                m_pendingBlocks = stereo ? 8 : 4;
            }

            uint32_t PendingBlocks() const { return m_pendingBlocks; }

            // Copies up to maxBlocks staged blocks to dst (256 bytes each) and returns the count.
            uint32_t TakeBlocks(uint8_t* dst, uint32_t maxBlocks)
            {
                const uint32_t count = std::min(maxBlocks, m_pendingBlocks);
                memset(dst, 0, count * kXmaBytesPerBlock); // silence
                m_pendingBlocks -= count;
                return count;
            }

            void Reset() { m_pendingBlocks = 0; }

        private:
            uint32_t m_pendingBlocks = 0;
        };

        // ------------------------------------------------------------ XMA contexts

        // Host-side state for one pool context. `mutex` is held by a decode pass, by the guest XMA
        // exports that touch the context, and by XMADisableContext(wait). It is never held while
        // waiting on the kick mutex, so the lock order is slot -> kick (the worker takes no slot lock
        // while it waits).
        struct XmaSlot
        {
            std::mutex mutex;
            bool allocated = false;              // guarded by mutex
            std::atomic<bool> enabled{ false };  // set by XMAEnableContext, cleared when a pass starts
            uint32_t inputGuest[2] = { 0, 0 };   // guest addresses of the input buffers (XMAInitializeContext)
            uint32_t outputGuest = 0;            // guest address of the output ring
            XmaPacketDecoder fake;               // state of the fake pass (DAO_XMA=fake)
            std::unique_ptr<XmaRealDecoder> real; // state of the real decoder, created on first pass
            XmaTraceStats trace;                 // DAO_XMA_TRACE=1 only; guarded by mutex
        };

        XmaSlot g_xmaSlots[kXmaContextCount];
        std::atomic<uint32_t> g_xmaBase{ 0 }; // guest (physical-view) address of context 0

        std::mutex g_kickMutex;
        std::condition_variable g_kickCv;
        bool g_kicked = false; // guarded by g_kickMutex

        void ResetSlotDecoders(XmaSlot& slot)
        {
            slot.fake.Reset();
            if (slot.real)
                slot.real->Reset();
        }

        int XmaContextIndex(uint32_t address)
        {
            const uint32_t base = g_xmaBase.load(std::memory_order_acquire);
            if (!base || address < base)
                return -1;
            const uint32_t offset = address - base;
            if (offset % kXmaContextSize || offset / kXmaContextSize >= kXmaContextCount)
                return -1;
            return int(offset / kXmaContextSize);
        }

        uint32_t XmaContextAddress(uint32_t index)
        {
            return g_xmaBase.load(std::memory_order_acquire) + index * kXmaContextSize;
        }

        XmaSlot* FindXmaSlot(uint32_t address)
        {
            const int index = XmaContextIndex(address);
            return index < 0 ? nullptr : &g_xmaSlots[index];
        }

        // Allocates the context array on first use, in physical memory (not cached).
        bool EnsureXmaPool()
        {
            static std::mutex s_initMutex;
            std::lock_guard lock(s_initMutex);
            if (g_xmaBase.load(std::memory_order_acquire))
                return true;
            const uint32_t base = PhysicalAlloc(kXmaContextCount * kXmaContextSize, kXmaContextAlignment, 0x1000,
                X_PAGE_READWRITE | X_PAGE_NOCACHE, 0, 0xFFFFFFFF);
            if (!base)
            {
                LOGE("XMA: failed to allocate the {} context pool", kXmaContextCount);
                return false;
            }
            g_xmaBase.store(base, std::memory_order_release);
            LOGI("XMA context pool: {} contexts of {} bytes at {:08X}, decoder: {}", kXmaContextCount, kXmaContextSize,
                base, XmaUseRealDecoder() ? "FFmpeg xmaframes" : "fake (silence)");
            return true;
        }

        uint32_t AllocateXmaContext()
        {
            if (!EnsureXmaPool())
                return 0;
            for (uint32_t i = 0; i < kXmaContextCount; ++i)
            {
                XmaSlot& slot = g_xmaSlots[i];
                std::lock_guard lock(slot.mutex);
                if (slot.allocated)
                    continue;
                slot.allocated = true;
                slot.enabled.store(false, std::memory_order_release);
                ResetSlotDecoders(slot);
                const uint32_t address = XmaContextAddress(i);
                memset(g_memory.Translate(address), 0, kXmaContextSize);
                return address;
            }
            return 0;
        }

        void ReleaseXmaContext(uint32_t address)
        {
            const int index = XmaContextIndex(address);
            if (index < 0)
            {
                LOGW("XMAReleaseContext: {:08X} is not a context from the pool", address);
                return;
            }
            XmaSlot& slot = g_xmaSlots[index];
            std::lock_guard lock(slot.mutex);
            if (!slot.allocated)
                return;
            slot.allocated = false;
            slot.enabled.store(false, std::memory_order_release);
            ResetSlotDecoders(slot);
            memset(g_memory.Translate(address), 0, kXmaContextSize);
        }

        // ------------------------------------------------------------ fake decode pass

        bool AnyInputValid(const u32* ctx)
        {
            return GetBits(ctx, kInputBuffer0Valid) || GetBits(ctx, kInputBuffer1Valid);
        }

        void SwapInputBuffer(u32* ctx)
        {
            if (GetBits(ctx, kCurrentBuffer) == 0)
                SetBits(ctx, kInputBuffer0Valid, 0);
            else
                SetBits(ctx, kInputBuffer1Valid, 0);
            SetBits(ctx, kCurrentBuffer, GetBits(ctx, kCurrentBuffer) ^ 1);
            SetBits(ctx, kInputBufferReadOffset, kXmaBitsPerPacketHeader);
        }

        void UpdateLoopStatus(u32* ctx)
        {
            const uint32_t loopCount = GetBits(ctx, kLoopCount);
            if (!loopCount)
                return;

            const uint32_t loopStart = std::max(kXmaBitsPerPacketHeader, GetBits(ctx, kLoopStart));
            const uint32_t loopEnd = std::max(kXmaBitsPerPacketHeader, GetBits(ctx, kLoopEnd));
            if (GetBits(ctx, kInputBufferReadOffset) != loopEnd)
                return;

            SetBits(ctx, kInputBufferReadOffset, loopStart);
            if (loopCount != 255)
                SetBits(ctx, kLoopCount, loopCount - 1);
        }

        // Takes the next packet from the current input buffer and hands it to the decoder.
        void ProcessPacket(u32* ctx, XmaSlot& slot)
        {
            if (!AnyInputValid(ctx) || slot.fake.PendingBlocks())
                return;

            UpdateLoopStatus(ctx);

            const uint32_t current = GetBits(ctx, kCurrentBuffer);
            const uint32_t packetCount = GetBits(ctx, current ? kInputBuffer1PacketCount : kInputBuffer0PacketCount);
            const uint32_t index = GetBits(ctx, kInputBufferReadOffset) / kXmaBitsPerPacket;

            // Packets used up: switch buffers and try again on the next loop iteration.
            if (index >= packetCount)
            {
                SwapInputBuffer(ctx);
                return;
            }

            const uint8_t* packet = g_memory.Translate<uint8_t>(slot.inputGuest[current] + index * kXmaBytesPerPacket);

            const uint32_t next = index + 1;
            if (next >= packetCount)
                SwapInputBuffer(ctx);
            else
                SetBits(ctx, kInputBufferReadOffset, next * kXmaBitsPerPacket + kXmaBitsPerPacketHeader);

            slot.fake.Decode(packet, GetBits(ctx, kIsStereo));
        }

        // Writes as many staged blocks as the subframe count allows. Returns the blocks written.
        uint32_t Consume(XmaOutputRing& ring, XmaSlot& slot, uint32_t subframeDecodeCount)
        {
            const uint32_t count = std::min(slot.fake.PendingBlocks(), subframeDecodeCount);
            if (!count)
                return 0;
            std::array<uint8_t, kXmaMaxBlocksPerPacket * kXmaBytesPerBlock> staged;
            const uint32_t taken = slot.fake.TakeBlocks(staged.data(), count);
            ring.Write(staged.data(), taken * kXmaBytesPerBlock);
            return taken;
        }

        // One pass of the fake decoder over a context, ported from XmaContextFake::Work(). The caller
        // holds slot.mutex.
        void RunFakePass(uint32_t address, XmaSlot& slot)
        {
            u32* ctx = g_memory.Translate<u32>(address);
            if (!GetBits(ctx, kOutputBufferValid) || !slot.outputGuest)
                return;

            const uint32_t subframeDecodeCount = GetBits(ctx, kSubframeDecodeCount);
            const uint32_t blockCount = GetBits(ctx, kOutputBufferBlockCount);
            // Xenia spins forever on a zero subframe count. Nothing can be produced, so skip the pass.
            if (!subframeDecodeCount || !blockCount)
                return;

            XmaOutputRing ring;
            ring.guestBase = slot.outputGuest;
            ring.host = g_memory.Translate<uint8_t>(slot.outputGuest);
            ring.capacity = blockCount * kXmaBytesPerBlock;
            ring.read = (GetBits(ctx, kOutputBufferReadOffset) * kXmaBytesPerBlock) % ring.capacity;
            ring.write = (GetBits(ctx, kOutputBufferWriteOffset) * kXmaBytesPerBlock) % ring.capacity;

            int32_t freeBlocks = int32_t(ring.WriteCount() / kXmaBytesPerBlock);
            const int32_t minimum = int32_t(subframeDecodeCount) * 2 - 1;
            if (minimum > freeBlocks)
                return; // not enough room for one pass; nothing is changed

            while (freeBlocks >= minimum)
            {
                ProcessPacket(ctx, slot);
                freeBlocks -= int32_t(Consume(ring, slot, subframeDecodeCount));
                if (!AnyInputValid(ctx) || GetBits(ctx, kErrorStatus) == 4)
                    break;
            }

            SetBits(ctx, kOutputBufferWriteOffset, ring.write / kXmaBytesPerBlock);
            if (ring.Empty())
                SetBits(ctx, kOutputBufferValid, 0);
        }

        // Runs a pass on one context with the selected decoder. The caller holds slot.mutex.
        void RunDecodePass(uint32_t address, XmaSlot& slot)
        {
            if (XmaUseRealDecoder())
            {
                if (!slot.real)
                    slot.real = std::make_unique<XmaRealDecoder>();
                if (slot.real->Available())
                {
                    slot.real->Work(address, XmaBuffers{ { slot.inputGuest[0], slot.inputGuest[1] }, slot.outputGuest },
                        XmaTraceOn() ? &slot.trace : nullptr);
                    return;
                }
                // FFmpeg has no usable xmaframes decoder: use the fake pass instead.
            }
            RunFakePass(address, slot);
        }

        // Runs one pass if the context is enabled. Returns true if a pass ran.
        bool RunEnabledContext(uint32_t index)
        {
            XmaSlot& slot = g_xmaSlots[index];
            if (!slot.enabled.load(std::memory_order_acquire))
                return false;

            std::lock_guard lock(slot.mutex);
            if (!slot.allocated || !slot.enabled.load(std::memory_order_acquire))
                return false;
            slot.enabled.store(false, std::memory_order_release);
            RunDecodePass(XmaContextAddress(index), slot);
            return true;
        }

        void CopyContextDwords(uint32_t address, uint32_t* out)
        {
            const u32* dwords = g_memory.Translate<u32>(address);
            for (uint32_t i = 0; i < kContextDwordCount; ++i)
                out[i] = dwords[i];
        }

        // Trace: one XMA call that touches a context (kick, state change, init). Caller holds slot.mutex.
        void TraceCallLocked(XmaSlot& slot, uint32_t address, const std::string& what)
        {
            XmaTraceStats& t = slot.trace;
            t.kicks++;
            t.totalCalls++;
            uint32_t dw[kContextDwordCount];
            CopyContextDwords(address, dw);
            if (t.totalCalls <= XmaTraceStats::kFirstCalls)
                LOGI("XMA trace: call #{} ctx {:08X} {} enabled={} | {}", t.totalCalls, address, what,
                    slot.enabled.load(), XmaDescribe(dw));
            else
                XMA_TRACE_EVENT(t, "ctx {:08X} {} enabled={} | {}", address, what, slot.enabled.load(), XmaDescribe(dw));
        }

        void TraceCall(uint32_t address, const std::string& what)
        {
            XmaSlot* slot = FindXmaSlot(address);
            if (!slot)
            {
                LOGI("XMA trace: {} on a non-pool context {:08X}", what, address);
                return;
            }
            std::lock_guard lock(slot->mutex);
            TraceCallLocked(*slot, address, what);
        }

        // Trace: summary line for each context that was active in the last 2 s. Runs on the decoder thread.
        void TraceSummaries()
        {
            static std::chrono::steady_clock::time_point s_lastScan{};
            const auto now = std::chrono::steady_clock::now();
            if (now - s_lastScan < std::chrono::milliseconds(250))
                return;
            s_lastScan = now;

            for (uint32_t i = 0; i < kXmaContextCount; ++i)
            {
                XmaSlot& slot = g_xmaSlots[i];
                std::lock_guard lock(slot.mutex);
                if (!slot.allocated)
                    continue;
                XmaTraceStats& t = slot.trace;
                if (t.lastSummary == std::chrono::steady_clock::time_point{})
                {
                    t.lastSummary = now;
                    continue;
                }
                if (now - t.lastSummary < std::chrono::seconds(2))
                    continue;
                if (t.kicks || t.passes)
                {
                    uint32_t dw[kContextDwordCount];
                    const uint32_t address = XmaContextAddress(i);
                    CopyContextDwords(address, dw);
                    XmaTraceSummary(address, dw, t, now);
                }
                else
                {
                    t.lastSummary = now;
                }
            }
        }

        // The decoder thread never calls guest code, so it needs no guest thread identity.
        void DecoderThreadMain()
        {
            SetThreadDescription(GetCurrentThread(), L"XMA decoder");
            for (;;)
            {
                if (XmaTraceOn())
                    TraceSummaries();

                bool didWork = false;
                for (uint32_t i = 0; i < kXmaContextCount; ++i)
                {
                    if (RunEnabledContext(i))
                        didWork = true;
                }
                if (didWork)
                    continue;

                // Sleeps until a kick. The 2 ms timeout is only a safety net.
                std::unique_lock lock(g_kickMutex);
                g_kickCv.wait_for(lock, std::chrono::milliseconds(2), [] { return g_kicked; });
                g_kicked = false;
            }
        }

        void EnsureDecoderStarted()
        {
            static std::once_flag s_started;
            std::call_once(s_started, [] {
                std::thread(DecoderThreadMain).detach();
                LOGI("XMA decoder worker started");
            });
        }

        // Wakes the decoder. Called after the game changes a context through the XMA exports.
        void OnContextKicked(uint32_t address)
        {
            (void)address;
            EnsureDecoderStarted();
            {
                std::lock_guard lock(g_kickMutex);
                g_kicked = true;
            }
            g_kickCv.notify_one();
        }

        // Runs fn with the context's dwords and slot (null when the address is not a pool context).
        // The slot lock is held for the call, so a decode pass cannot interleave with it.
        template <typename Fn>
        void WithContext(uint32_t address, Fn&& fn)
        {
            u32* dwords = ContextDwords(address);
            if (!dwords)
                return;
            XmaSlot* slot = FindXmaSlot(address);
            std::unique_lock<std::mutex> lock;
            if (slot)
                lock = std::unique_lock<std::mutex>(slot->mutex);
            fn(dwords, slot);
        }

        uint32_t ReadField(uint32_t address, XmaField field)
        {
            uint32_t value = 0;
            WithContext(address, [&](u32* dwords, XmaSlot*) { value = GetBits(dwords, field); });
            return value;
        }

        void WriteField(uint32_t address, XmaField field, uint32_t value)
        {
            WithContext(address, [&](u32* dwords, XmaSlot*) { SetBits(dwords, field, value); });
        }
    }

    // ---------------------------------------------------------------- host queue

    bool PopFrame(float* out6x256)
    {
        if (!out6x256)
            return false;
        std::lock_guard lock(g_frameMutex);
        if (g_frames.empty())
            return false;
        std::copy(g_frames.front().begin(), g_frames.front().end(), out6x256);
        g_frames.pop_front();
        return true;
    }

    uint32_t QueuedFrames()
    {
        std::lock_guard lock(g_frameMutex);
        return uint32_t(g_frames.size());
    }
}

namespace dao::imp
{
    using namespace dao::kernel;
    using namespace dao::apu;

    // ------------------------------------------------------------ render driver

    uint32_t XAudioGetSpeakerConfig(u32* config)
    {
        APU_LOG_FIRST("XAudioGetSpeakerConfig");
        if (!config)
            return X_E_INVALIDARG;
        *config = kSpeakerConfig;
        return 0;
    }

    uint32_t XAudioGetVoiceCategoryVolumeChangeMask(uint32_t driver, u32* mask)
    {
        APU_LOG_FIRST("XAudioGetVoiceCategoryVolumeChangeMask driver={:08X}", driver);
        if (!mask)
            return X_E_INVALIDARG;
        *mask = 0;
        return 0;
    }

    uint32_t XAudioGetVoiceCategoryVolume(uint32_t category, be<float>* volume)
    {
        APU_LOG_FIRST("XAudioGetVoiceCategoryVolume category={}", category);
        if (!volume)
            return X_E_INVALIDARG;
        *volume = 1.0f;
        return 0;
    }

    uint32_t XAudioRegisterRenderDriverClient(u32* callbackPtr, u32* driverPtr)
    {
        if (!callbackPtr || !driverPtr)
            return X_E_INVALIDARG;
        const uint32_t callback = callbackPtr[0];
        const uint32_t callbackArg = callbackPtr[1];
        if (!callback)
            return X_E_INVALIDARG;

        // The client receives the address of a guest cell that holds callbackArg (Xenia does the same).
        const uint32_t argCell = KernelAlloc(4, 4);
        if (!argCell)
            return X_E_OUTOFMEMORY;
        *g_memory.Translate<u32>(argCell) = callbackArg;

        std::lock_guard lock(g_clientMutex);
        for (uint32_t i = 0; i < kMaxClients; ++i)
        {
            if (g_clients[i].inUse)
                continue;
            g_clients[i] = Client{ true, callback, argCell };
            *driverPtr = kDriverTag | i;
            EnsureRenderWorkerStarted();
            APU_LOG_FIRST("XAudioRegisterRenderDriverClient callback={:08X} arg={:08X} -> client {}", callback, callbackArg, i);
            return 0;
        }
        KernelFree(argCell);
        LOGW("XAudioRegisterRenderDriverClient: all {} client slots are in use", kMaxClients);
        return X_E_OUTOFMEMORY;
    }

    uint32_t XAudioUnregisterRenderDriverClient(uint32_t driver)
    {
        APU_LOG_FIRST("XAudioUnregisterRenderDriverClient driver={:08X}", driver);
        const uint32_t index = ClientIndex(driver);
        if (index == kInvalidClient)
            return X_E_INVALIDARG;
        std::lock_guard lock(g_clientMutex);
        if (g_clients[index].inUse)
            KernelFree(g_clients[index].callbackArg);
        g_clients[index] = Client{};
        return 0;
    }

    uint32_t XAudioSubmitRenderDriverFrame(uint32_t driver, be<float>* samples)
    {
        const uint32_t index = ClientIndex(driver);
        if (index == kInvalidClient || !samples)
            return X_E_INVALIDARG;

        // Input is kFrameChannels * kFrameChannelSamples big-endian floats, channel-major.
        Frame frame;
        for (uint32_t i = 0; i < kFrameSampleCount; ++i)
            frame[i] = float(samples[i]);

        std::lock_guard clientLock(g_clientMutex);
        if (!g_clients[index].inUse)
            return X_E_INVALIDARG;

        std::lock_guard lock(g_frameMutex);
        g_frames.push_back(frame);
        while (g_frames.size() > kQueueDropLimit)
            g_frames.pop_front();

        APU_LOG_FIRST("XAudioSubmitRenderDriverFrame client {}", index);
        return 0;
    }

    // ------------------------------------------------------------ XMA

    uint32_t XMACreateContext(u32* contextOut)
    {
        APU_LOG_FIRST("XMACreateContext");
        if (!contextOut)
            return X_STATUS_INVALID_PARAMETER;
        const uint32_t context = AllocateXmaContext();
        *contextOut = context;
        return context ? X_STATUS_SUCCESS : X_STATUS_NO_MEMORY;
    }

    uint32_t XMAReleaseContext(uint32_t context)
    {
        APU_LOG_FIRST("XMAReleaseContext context={:08X}", context);
        ReleaseXmaContext(context);
        return 0;
    }

    uint32_t XMAInitializeContext(uint32_t context, const XmaContextInit* init)
    {
        APU_LOG_FIRST("XMAInitializeContext context={:08X}", context);
        if (!init || !ContextDwords(context))
            return X_E_INVALIDARG;

        WithContext(context, [&](u32* dwords, XmaSlot* slot) {
            memset(dwords, 0, kXmaContextSize);
            dwords[kInputBuffer0PtrDword] = ToPhysical(uint32_t(init->input0Ptr));
            SetBits(dwords, kInputBuffer0PacketCount, init->input0PacketCount);
            dwords[kInputBuffer1PtrDword] = ToPhysical(uint32_t(init->input1Ptr));
            SetBits(dwords, kInputBuffer1PacketCount, init->input1PacketCount);
            SetBits(dwords, kInputBufferReadOffset, init->inputReadOffset);
            dwords[kOutputBufferPtrDword] = ToPhysical(uint32_t(init->outputPtr));
            SetBits(dwords, kOutputBufferBlockCount, init->outputBlockCount);
            SetBits(dwords, kSubframeDecodeCount, init->subframeDecodeCount);
            // Xenia stores is_stereo as channel_count >= 1. Kept as-is; verify against real games.
            SetBits(dwords, kIsStereo, init->channelCount >= 1);
            SetBits(dwords, kSampleRate, init->sampleRate);
            SetBits(dwords, kLoopStart, init->loop.loopStart);
            SetBits(dwords, kLoopEnd, init->loop.loopEnd);
            SetBits(dwords, kLoopCount, init->loop.loopCount);
            SetBits(dwords, kLoopSubframeEnd, init->loop.loopSubframeEnd);
            SetBits(dwords, kLoopSubframeSkip, init->loop.loopSubframeSkip);

            if (slot)
            {
                // The decoders read the guest addresses the game passed, which are always valid host
                // addresses. The dword values above are physical and are only kept for the guest.
                // Host decode state (pending frame, loop flags) is kept across a re-Initialize, as Xenia does.
                slot->inputGuest[0] = uint32_t(init->input0Ptr);
                slot->inputGuest[1] = uint32_t(init->input1Ptr);
                slot->outputGuest = uint32_t(init->outputPtr);

                if (XmaTraceOn())
                {
                    slot->trace.channelCount = uint32_t(init->channelCount);
                    slot->trace.sampleRateId = uint32_t(init->sampleRate);
                    TraceCallLocked(*slot, context,
                        fmt::format("Initialize ch={} rate={} in0={:08X} n{} in1={:08X} n{} out={:08X} blk{} sdc{} rd{} "
                                    "loop n{} start{} end{} subEnd{} skip{}",
                            uint32_t(init->channelCount), uint32_t(init->sampleRate), uint32_t(init->input0Ptr),
                            uint32_t(init->input0PacketCount), uint32_t(init->input1Ptr),
                            uint32_t(init->input1PacketCount), uint32_t(init->outputPtr),
                            uint32_t(init->outputBlockCount), uint32_t(init->subframeDecodeCount),
                            uint32_t(init->inputReadOffset), uint32_t(init->loop.loopCount),
                            uint32_t(init->loop.loopStart), uint32_t(init->loop.loopEnd),
                            uint32_t(init->loop.loopSubframeEnd), uint32_t(init->loop.loopSubframeSkip)));
                }
            }
        });
        return 0;
    }

    uint32_t XMAEnableContext(uint32_t context)
    {
        APU_LOG_FIRST("XMAEnableContext context={:08X}", context);
        const int index = XmaContextIndex(context);
        if (index >= 0)
        {
            XmaSlot& slot = g_xmaSlots[index];
            {
                std::lock_guard lock(slot.mutex);
                slot.enabled.store(true, std::memory_order_release);
                if (XmaTraceOn())
                    TraceCallLocked(slot, context, "Enable");
            }
            // Xenia's kick waits for the pass to finish (WaitForWorkDone in the dedicated-thread path), so
            // the game's next call (often Disable(wait=1)) sees the pass done. A background pass alone can be
            // cancelled by that Disable before it runs, so run it here, before returning.
            RunEnabledContext(uint32_t(index));
        }
        OnContextKicked(context);
        return 0;
    }

    uint32_t XMADisableContext(uint32_t context, uint32_t wait)
    {
        APU_LOG_FIRST("XMADisableContext context={:08X} wait={}", context, wait);
        XmaSlot* slot = FindXmaSlot(context);
        if (!slot)
            return 0;

        // Clear first, so a pass that has not started yet will not run. Then take the slot lock:
        // blocking when wait is set (an in-progress pass finishes first), try-lock otherwise.
        slot->enabled.store(false, std::memory_order_release);
        uint32_t result = 0;
        if (wait)
        {
            std::lock_guard lock(slot->mutex);
        }
        else
        {
            std::unique_lock<std::mutex> lock(slot->mutex, std::try_to_lock);
            if (!lock.owns_lock())
                result = X_E_FALSE;
        }
        if (XmaTraceOn())
            TraceCall(context, fmt::format("Disable wait={} result={:08X}", wait, result));
        return result;
    }

    uint32_t XMASetInputBuffer0Valid(uint32_t context)
    {
        APU_LOG_FIRST("XMASetInputBuffer0Valid context={:08X}", context);
        WriteField(context, kInputBuffer0Valid, 1);
        if (XmaTraceOn())
            TraceCall(context, "SetInputBuffer0Valid");
        OnContextKicked(context);
        return 0;
    }

    uint32_t XMAIsInputBuffer0Valid(uint32_t context)
    {
        APU_LOG_FIRST("XMAIsInputBuffer0Valid context={:08X}", context);
        return ReadField(context, kInputBuffer0Valid);
    }

    uint32_t XMASetInputBuffer1Valid(uint32_t context)
    {
        APU_LOG_FIRST("XMASetInputBuffer1Valid context={:08X}", context);
        WriteField(context, kInputBuffer1Valid, 1);
        if (XmaTraceOn())
            TraceCall(context, "SetInputBuffer1Valid");
        OnContextKicked(context);
        return 0;
    }

    uint32_t XMAIsInputBuffer1Valid(uint32_t context)
    {
        APU_LOG_FIRST("XMAIsInputBuffer1Valid context={:08X}", context);
        return ReadField(context, kInputBuffer1Valid);
    }

    uint32_t XMASetInputBufferReadOffset(uint32_t context, uint32_t offset)
    {
        APU_LOG_FIRST("XMASetInputBufferReadOffset context={:08X} offset={}", context, offset);
        WriteField(context, kInputBufferReadOffset, offset);
        if (XmaTraceOn())
            TraceCall(context, fmt::format("SetInputBufferReadOffset {}", offset));
        OnContextKicked(context);
        return 0;
    }

    uint32_t XMAGetInputBufferReadOffset(uint32_t context)
    {
        APU_LOG_FIRST("XMAGetInputBufferReadOffset context={:08X}", context);
        return ReadField(context, kInputBufferReadOffset);
    }

    uint32_t XMASetOutputBufferValid(uint32_t context)
    {
        APU_LOG_FIRST("XMASetOutputBufferValid context={:08X}", context);
        WriteField(context, kOutputBufferValid, 1);
        if (XmaTraceOn())
            TraceCall(context, "SetOutputBufferValid");
        OnContextKicked(context);
        return 0;
    }

    uint32_t XMAIsOutputBufferValid(uint32_t context)
    {
        APU_LOG_FIRST("XMAIsOutputBufferValid context={:08X}", context);
        return ReadField(context, kOutputBufferValid);
    }

    uint32_t XMASetOutputBufferReadOffset(uint32_t context, uint32_t offset)
    {
        APU_LOG_FIRST("XMASetOutputBufferReadOffset context={:08X} offset={}", context, offset);
        WriteField(context, kOutputBufferReadOffset, offset);
        if (XmaTraceOn())
            TraceCall(context, fmt::format("SetOutputBufferReadOffset {}", offset));
        OnContextKicked(context);
        return 0;
    }

    uint32_t XMAGetOutputBufferReadOffset(uint32_t context)
    {
        APU_LOG_FIRST("XMAGetOutputBufferReadOffset context={:08X}", context);
        return ReadField(context, kOutputBufferReadOffset);
    }

    uint32_t XMAGetOutputBufferWriteOffset(uint32_t context)
    {
        APU_LOG_FIRST("XMAGetOutputBufferWriteOffset context={:08X}", context);
        return ReadField(context, kOutputBufferWriteOffset);
    }
}

DAO_IMPORT_AS(XAudioGetSpeakerConfig, dao::imp::XAudioGetSpeakerConfig)
DAO_IMPORT_AS(XAudioGetVoiceCategoryVolume, dao::imp::XAudioGetVoiceCategoryVolume)
DAO_IMPORT_AS(XAudioGetVoiceCategoryVolumeChangeMask, dao::imp::XAudioGetVoiceCategoryVolumeChangeMask)
DAO_IMPORT_AS(XAudioRegisterRenderDriverClient, dao::imp::XAudioRegisterRenderDriverClient)
DAO_IMPORT_AS(XAudioUnregisterRenderDriverClient, dao::imp::XAudioUnregisterRenderDriverClient)
DAO_IMPORT_AS(XAudioSubmitRenderDriverFrame, dao::imp::XAudioSubmitRenderDriverFrame)

DAO_IMPORT_AS(XMACreateContext, dao::imp::XMACreateContext)
DAO_IMPORT_AS(XMAReleaseContext, dao::imp::XMAReleaseContext)
DAO_IMPORT_AS(XMAInitializeContext, dao::imp::XMAInitializeContext)
DAO_IMPORT_AS(XMAEnableContext, dao::imp::XMAEnableContext)
DAO_IMPORT_AS(XMADisableContext, dao::imp::XMADisableContext)
DAO_IMPORT_AS(XMASetInputBuffer0Valid, dao::imp::XMASetInputBuffer0Valid)
DAO_IMPORT_AS(XMAIsInputBuffer0Valid, dao::imp::XMAIsInputBuffer0Valid)
DAO_IMPORT_AS(XMASetInputBuffer1Valid, dao::imp::XMASetInputBuffer1Valid)
DAO_IMPORT_AS(XMAIsInputBuffer1Valid, dao::imp::XMAIsInputBuffer1Valid)
DAO_IMPORT_AS(XMASetInputBufferReadOffset, dao::imp::XMASetInputBufferReadOffset)
DAO_IMPORT_AS(XMAGetInputBufferReadOffset, dao::imp::XMAGetInputBufferReadOffset)
DAO_IMPORT_AS(XMASetOutputBufferValid, dao::imp::XMASetOutputBufferValid)
DAO_IMPORT_AS(XMAIsOutputBufferValid, dao::imp::XMAIsOutputBufferValid)
DAO_IMPORT_AS(XMASetOutputBufferReadOffset, dao::imp::XMASetOutputBufferReadOffset)
DAO_IMPORT_AS(XMAGetOutputBufferReadOffset, dao::imp::XMAGetOutputBufferReadOffset)
DAO_IMPORT_AS(XMAGetOutputBufferWriteOffset, dao::imp::XMAGetOutputBufferWriteOffset)
