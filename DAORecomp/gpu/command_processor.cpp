#include "stdafx.h"
#include "gpu/command_processor.h"
#include "gpu/video.h"
#include "gpu/xenia_host.h"
#include "kernel/memory.h"
#include "kernel/thread.h"

using namespace dao::kernel;

namespace dao::gpu
{
    namespace
    {
        enum : uint32_t
        {
            PM4_ME_INIT = 0x48,
            PM4_NOP = 0x10,
            PM4_INDIRECT_BUFFER_PFP = 0x37,
            PM4_INDIRECT_BUFFER = 0x3F,
            PM4_WAIT_REG_MEM = 0x3C,
            PM4_REG_RMW = 0x21,
            PM4_COND_WRITE = 0x45,
            PM4_EVENT_WRITE = 0x46,
            PM4_EVENT_WRITE_SHD = 0x58,
            PM4_EVENT_WRITE_EXT = 0x59,
            PM4_EVENT_WRITE_ZPD = 0x5B,
            PM4_MEM_WRITE = 0x3D,
            PM4_INTERRUPT = 0x54,
            PM4_SET_CONSTANT = 0x2D,
            PM4_SET_CONSTANT2 = 0x55,
            PM4_LOAD_ALU_CONSTANT = 0x2F,
            PM4_SET_SHADER_CONSTANTS = 0x56,
            PM4_XE_SWAP = 0x64, // written by our VdSwap (same convention as Xenia)
        };

        std::array<std::atomic<uint32_t>, kRegisterCount> s_registers{};

        std::mutex s_wakeMutex;
        std::condition_variable s_wake;
        std::atomic<uint32_t> s_writePointer{ 0 };
        uint32_t s_readPointer = 0;
        uint32_t s_ringBase = 0;   // physical
        uint32_t s_ringDwords = 0;

        std::atomic<uint64_t> s_framesPresented{ 0 };
        std::atomic<bool> s_started{ false };

        bool s_traceOpcodes = false;
        int s_tracePackets = 0; // DAO_GPU_TRACE=2: log this many packets in full
        std::array<bool, 128> s_seenOpcodes{};

        u32* Physical(uint32_t address)
        {
            return g_memory.Translate<u32>(0xA0000000u | (address & 0x1FFFFFFCu));
        }

        // Applies the Xenos endian mode (low 2 bits of GPU addresses) to a value.
        uint32_t GpuSwap(uint32_t value, uint32_t endian)
        {
            switch (endian & 3)
            {
            case 1: return ((value & 0x00FF00FFu) << 8) | ((value >> 8) & 0x00FF00FFu);
            case 2: return _byteswap_ulong(value);
            case 3: return (value << 16) | (value >> 16);
            default: return value;
            }
        }

        // Stores a GPU-side value: raw (host-order) store of the endian-swapped value.
        void GpuStore(uint32_t address, uint32_t value)
        {
            *reinterpret_cast<uint32_t*>(Physical(address)) = GpuSwap(value, address & 3);
        }

        uint32_t GpuLoad(uint32_t address)
        {
            return GpuSwap(*reinterpret_cast<const uint32_t*>(Physical(address)), address & 3);
        }

        void WriteRegister(uint32_t index, uint32_t value)
        {
            index &= kRegisterCount - 1;
            if (index == reg::COHER_STATUS_HOST)
                value &= ~0x80000000u; // cache flushes complete instantly

            s_registers[index].store(value, std::memory_order_relaxed);
            *g_memory.Translate<u32>(kRegisterWindow + index * 4) = value;

            // Scratch registers are mirrored to memory when enabled in SCRATCH_UMSK.
            if (index >= reg::SCRATCH_REG0 && index < reg::SCRATCH_REG0 + 8)
            {
                const uint32_t n = index - reg::SCRATCH_REG0;
                if (s_registers[reg::SCRATCH_UMSK].load() & (1u << n))
                {
                    // Scratch writeback is always big-endian (like Xenia's store_and_swap).
                    const uint32_t address = s_registers[reg::SCRATCH_ADDR].load() + n * 4;
                    *Physical(address) = value;
                }
            }
        }

        bool Compare(uint32_t function, uint32_t value, uint32_t reference)
        {
            switch (function & 7)
            {
            case 0: return false;
            case 1: return value < reference;
            case 2: return value <= reference;
            case 3: return value == reference;
            case 4: return value != reference;
            case 5: return value >= reference;
            case 6: return value > reference;
            default: return true;
            }
        }

        // Reads dwords from either the ring (wrapping) or a linear indirect buffer.
        struct Reader
        {
            uint32_t base;     // physical
            uint32_t size;     // dwords (0 = linear)
            uint32_t position; // dwords

            uint32_t Read()
            {
                const uint32_t index = size ? position % size : position;
                position++;
                return *Physical(base + index * 4);
            }

            void Skip(uint32_t count) { position += count; }
        };

        void ExecuteBuffer(Reader& reader, uint32_t end);

        void ExecuteType3(Reader& reader, uint32_t header)
        {
            const uint32_t opcode = (header >> 8) & 0x7F;
            const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
            const uint32_t dataStart = reader.position;

            if (s_tracePackets > 0)
            {
                s_tracePackets--;
                std::string words;
                Reader peek = reader;
                for (uint32_t i = 0; i < std::min(count, 8u); i++)
                    words += fmt::format(" {:08X}", peek.Read());
                LOGI("GPU: pm4 {:02X} n{}{}", opcode, count, words);
            }
            if (s_traceOpcodes && !s_seenOpcodes[opcode])
            {
                s_seenOpcodes[opcode] = true;
                LOGI("GPU: first PM4 opcode {:02X} (count {})", opcode, count);
            }

            switch (opcode)
            {
            case PM4_INDIRECT_BUFFER:
            case PM4_INDIRECT_BUFFER_PFP:
            {
                const uint32_t address = reader.Read();
                const uint32_t length = reader.Read() & 0xFFFFF;
                Reader sub{ address & ~3u, 0, 0 };
                ExecuteBuffer(sub, length);
                break;
            }

            case PM4_WAIT_REG_MEM:
            {
                const uint32_t waitInfo = reader.Read();
                const uint32_t pollAddress = reader.Read();
                const uint32_t reference = reader.Read();
                const uint32_t mask = reader.Read();
                const uint32_t wait = reader.Read();
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                for (;;)
                {
                    const uint32_t value = (waitInfo & 0x10) ? GpuLoad(pollAddress) : ReadRegister(pollAddress);
                    if (Compare(waitInfo, value & mask, reference))
                        break;
                    if (std::chrono::steady_clock::now() > deadline)
                    {
                        LOGW("GPU: WAIT_REG_MEM timed out ({} {:08X} & {:08X} = {:08X} vs {:08X}, fn {})",
                            (waitInfo & 0x10) ? "mem" : "reg", pollAddress, mask, value, reference, waitInfo & 7);
                        break;
                    }
                    if (wait >= 0x100)
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                    else
                        std::this_thread::yield();
                }
                break;
            }

            case PM4_REG_RMW:
            {
                const uint32_t info = reader.Read();
                const uint32_t andMask = reader.Read();
                const uint32_t orMask = reader.Read();
                const uint32_t index = info & 0x1FFF;
                uint32_t value = ReadRegister(index);
                value &= (info & 0x80000000u) ? ReadRegister(andMask & 0x1FFF) : andMask;
                value |= (info & 0x40000000u) ? ReadRegister(orMask & 0x1FFF) : orMask;
                WriteRegister(index, value);
                break;
            }

            case PM4_COND_WRITE:
            {
                const uint32_t waitInfo = reader.Read();
                const uint32_t pollAddress = reader.Read();
                const uint32_t reference = reader.Read();
                const uint32_t mask = reader.Read();
                const uint32_t writeAddress = reader.Read();
                const uint32_t writeData = reader.Read();
                const uint32_t value = (waitInfo & 0x10) ? GpuLoad(pollAddress) : ReadRegister(pollAddress);
                if (Compare(waitInfo, value & mask, reference))
                {
                    if (waitInfo & 0x100)
                        GpuStore(writeAddress, writeData);
                    else
                        WriteRegister(writeAddress, writeData);
                }
                break;
            }

            case PM4_EVENT_WRITE_SHD:
            {
                const uint32_t initiator = reader.Read();
                const uint32_t address = reader.Read();
                const uint32_t value = reader.Read();
                const uint32_t data = (initiator & 0x80000000u) ? uint32_t(s_framesPresented.load()) : value;
                GpuStore(address, data);
                break;
            }

            case PM4_EVENT_WRITE_EXT:
            {
                // Screen extents: report "everything" (6 x u16).
                reader.Read();
                const uint32_t address = reader.Read();
                const uint16_t extents[6] = { 0, 0x3FFF, 0, 0x3FFF, 0, 1 };
                auto* out = reinterpret_cast<uint16_t*>(Physical(address));
                for (int i = 0; i < 6; i++)
                    out[i] = (address & 3) ? _byteswap_ushort(extents[i]) : extents[i];
                break;
            }

            case PM4_EVENT_WRITE_ZPD:
                // Occlusion queries: leave the result untouched (the game sees "pending" -> visible).
                break;

            case PM4_MEM_WRITE:
            {
                const uint32_t address = reader.Read();
                for (uint32_t i = 1; i < count; i++)
                    GpuStore((address & ~3u) + (i - 1) * 4 | (address & 3), reader.Read());
                break;
            }

            case PM4_INTERRUPT:
            {
                const uint32_t cpuMask = reader.Read();
                for (uint32_t cpu = 0; cpu < 6; cpu++)
                {
                    if (cpuMask & (1u << cpu))
                        DispatchInterrupt(1, cpu);
                }
                break;
            }

            case PM4_SET_CONSTANT:
            {
                const uint32_t offsetType = reader.Read();
                const uint32_t index = offsetType & 0x7FF;
                static constexpr uint32_t kBases[] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
                const uint32_t type = (offsetType >> 16) & 0xFF;
                const uint32_t base = type < 5 ? kBases[type] : 0x2000;
                for (uint32_t i = 1; i < count; i++)
                    WriteRegister(base + index + i - 1, reader.Read());
                break;
            }

            case PM4_SET_CONSTANT2:
            case PM4_SET_SHADER_CONSTANTS:
            {
                const uint32_t index = reader.Read() & 0xFFFF;
                for (uint32_t i = 1; i < count; i++)
                    WriteRegister(index + i - 1, reader.Read());
                break;
            }

            case PM4_LOAD_ALU_CONSTANT:
            {
                const uint32_t address = reader.Read() & 0x3FFFFFFF;
                const uint32_t offsetType = reader.Read();
                const uint32_t size = reader.Read() & 0xFFF;
                static constexpr uint32_t kBases[] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
                const uint32_t type = (offsetType >> 16) & 0xFF;
                const uint32_t base = (type < 5 ? kBases[type] : 0x2000) + (offsetType & 0x7FF);
                for (uint32_t i = 0; i < size; i++)
                    WriteRegister(base + i, *Physical(address + i * 4));
                break;
            }

            case PM4_XE_SWAP:
            {
                const uint64_t frame = ++s_framesPresented;
                if (frame == 1 || frame % 600 == 0)
                    LOGI("GPU: frame {} presented", frame);
                break;
            }

            default:
                break;
            }

            reader.position = dataStart + count;
        }

        void ExecuteBuffer(Reader& reader, uint32_t end)
        {
            while (reader.position != end)
            {
                const uint32_t header = reader.Read();
                switch (header >> 30)
                {
                case 0:
                {
                    const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
                    const uint32_t index = header & 0x7FFF;
                    const bool oneRegister = (header >> 15) & 1;
                    for (uint32_t i = 0; i < count; i++)
                        WriteRegister(oneRegister ? index : index + i, reader.Read());
                    break;
                }
                case 1:
                {
                    const uint32_t r0 = reader.Read();
                    const uint32_t r1 = reader.Read();
                    WriteRegister(header & 0x7FF, r0);
                    WriteRegister((header >> 11) & 0x7FF, r1);
                    break;
                }
                case 2:
                    break; // nop
                case 3:
                    if (header == 0xFFFFFFFF || header == 0) // stray filler
                        break;
                    ExecuteType3(reader, header);
                    break;
                }

                // A linear buffer can't overrun; for the ring, `end` is reached exactly
                // unless a packet straddles it (which D3D never emits).
                if (reader.size == 0 && reader.position > end)
                    break;
            }
        }

        void CommandProcessorThread()
        {
            GuestThread::AttachHostThread("GPU command processor");
            SetThreadDescription(GetCurrentThread(), L"GPU command processor");

            for (;;)
            {
                uint32_t writePointer;
                {
                    std::unique_lock lock(s_wakeMutex);
                    s_wake.wait_for(lock, std::chrono::milliseconds(5),
                        [] { return s_writePointer.load() != s_readPointer; });
                    writePointer = s_writePointer.load();
                }
                if (writePointer == s_readPointer)
                    continue;

                Reader reader{ s_ringBase, s_ringDwords, s_readPointer };
                const uint32_t end = writePointer >= s_readPointer ? writePointer : writePointer + s_ringDwords;
                ExecuteBuffer(reader, end);
                s_readPointer = writePointer % s_ringDwords;

                WriteRegister(reg::CP_RB_RPTR, s_readPointer);
                if (g_video.readPointerWriteBack)
                    *Physical(g_video.readPointerWriteBack) = s_readPointer;
            }
        }

        void VblankThread()
        {
            GuestThread::AttachHostThread("GPU vblank");
            SetThreadDescription(GetCurrentThread(), L"GPU vblank");

            using clock = std::chrono::steady_clock;
            const auto period = std::chrono::nanoseconds(1'000'000'000 / 60);
            auto next = clock::now() + period;
            for (;;)
            {
                std::this_thread::sleep_until(next);
                next += period;
                WriteRegister(reg::D1MODE_VBLANK_STATUS, ReadRegister(reg::D1MODE_VBLANK_STATUS) | 1);
                DispatchInterrupt(0, 2);
            }
        }
    }

    void StartCommandProcessor()
    {
        if (s_started.exchange(true))
            return;
        if (const char* trace = getenv("DAO_GPU_TRACE"))
        {
            s_traceOpcodes = true;
            s_tracePackets = atoi(trace) >= 2 ? 400 : 0;
        }
        s_ringBase = g_video.ringBufferBase;
        s_ringDwords = (1u << (g_video.ringBufferSizeLog2 + 3)) / 4;
        LOGI("GPU: command processor on ring {:08X} ({} dwords)", s_ringBase, s_ringDwords);
        std::thread(CommandProcessorThread).detach();
        std::thread(VblankThread).detach();
    }

    void OnRegisterWrite(uint32_t index, uint32_t value)
    {
        index &= kRegisterCount - 1;
        if (index == reg::CP_RB_WPTR)
        {
            {
                std::lock_guard lock(s_wakeMutex);
                s_writePointer.store(value % std::max(s_ringDwords, 1u));
            }
            s_wake.notify_one();
            return;
        }
        WriteRegister(index, value);
    }

    uint32_t ReadRegister(uint32_t index)
    {
        return s_registers[index & (kRegisterCount - 1)].load(std::memory_order_relaxed);
    }

    void DispatchInterrupt(uint32_t source, uint32_t cpu)
    {
        if (g_video.interruptCallback)
            CallInterruptCallback(g_video.interruptCallback, g_video.interruptUserData, source, cpu);
    }

    uint64_t FramesPresented()
    {
        return s_framesPresented.load();
    }
}

void DaoGpuRegisterWrite(uint32_t address, uint32_t value)
{
    if (dao::gpu::XeniaActive())
        dao::gpu::XeniaWriteRegister(address, value);
    else
        dao::gpu::OnRegisterWrite((address & 0xFFFF) / 4, value);
    if ((address & 0xFFFF) / 4 == dao::gpu::reg::CP_RB_WPTR)
        dao::gpu::FrameKicked();
}
