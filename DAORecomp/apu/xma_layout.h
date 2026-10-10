#pragma once
// XMA_CONTEXT_DATA layout (Xenia's xma_context.h). A context is 16 big-endian dwords in guest memory.
// Each field is a bit range inside one dword, counted from bit 0. That matches Xenia's bitfield
// struct once every dword is byte-swapped on a little-endian host.

#include <cstdint>

namespace dao::apu
{
    struct XmaField
    {
        uint32_t dword;
        uint32_t shift;
        uint32_t width;
    };

    // Works on guest dwords (be<uint32_t>) and on host-endian copies (uint32_t) alike.
    template <typename Word>
    inline uint32_t GetBits(const Word* dwords, XmaField field)
    {
        const uint32_t mask = (1u << field.width) - 1;
        const uint32_t word = uint32_t(dwords[field.dword]);
        return (word >> field.shift) & mask;
    }

    template <typename Word>
    inline void SetBits(Word* dwords, XmaField field, uint32_t value)
    {
        const uint32_t mask = ((1u << field.width) - 1) << field.shift;
        uint32_t word = uint32_t(dwords[field.dword]);
        word = (word & ~mask) | ((value << field.shift) & mask);
        dwords[field.dword] = word;
    }

    // DWORD 0
    constexpr XmaField kInputBuffer0PacketCount{ 0, 0, 12 };
    constexpr XmaField kLoopCount{ 0, 12, 8 };
    constexpr XmaField kInputBuffer0Valid{ 0, 20, 1 };
    constexpr XmaField kInputBuffer1Valid{ 0, 21, 1 };
    constexpr XmaField kOutputBufferBlockCount{ 0, 22, 5 };
    constexpr XmaField kOutputBufferWriteOffset{ 0, 27, 5 };
    // DWORD 1
    constexpr XmaField kInputBuffer1PacketCount{ 1, 0, 12 };
    constexpr XmaField kLoopSubframeStart{ 1, 12, 2 };
    constexpr XmaField kLoopSubframeEnd{ 1, 14, 3 };
    constexpr XmaField kLoopSubframeSkip{ 1, 17, 3 };
    constexpr XmaField kSubframeDecodeCount{ 1, 20, 4 };
    constexpr XmaField kOutputBufferPadding{ 1, 24, 3 };
    constexpr XmaField kSampleRate{ 1, 27, 2 };
    constexpr XmaField kIsStereo{ 1, 29, 1 };
    constexpr XmaField kOutputBufferValid{ 1, 31, 1 };
    // DWORD 2
    constexpr XmaField kInputBufferReadOffset{ 2, 0, 26 };
    constexpr XmaField kErrorStatus{ 2, 26, 5 };
    // DWORD 3
    constexpr XmaField kLoopStart{ 3, 0, 26 };
    // DWORD 4
    constexpr XmaField kLoopEnd{ 4, 0, 26 };
    constexpr XmaField kCurrentBuffer{ 4, 31, 1 };
    // DWORD 9
    constexpr XmaField kOutputBufferReadOffset{ 9, 0, 5 };

    // Dwords holding the buffer addresses (physical, as Xenia stores them).
    constexpr uint32_t kInputBuffer0PtrDword = 5;
    constexpr uint32_t kInputBuffer1PtrDword = 6;
    constexpr uint32_t kOutputBufferPtrDword = 7;
    constexpr uint32_t kContextDwordCount = 16;

    // Stream constants (Xenia's XmaContext).
    constexpr uint32_t kXmaBytesPerPacket = 2048;
    constexpr uint32_t kXmaBytesPerPacketHeader = 4;
    constexpr uint32_t kXmaBytesPerPacketData = kXmaBytesPerPacket - kXmaBytesPerPacketHeader; // 2044
    constexpr uint32_t kXmaBitsPerPacket = kXmaBytesPerPacket * 8;                             // 16384
    constexpr uint32_t kXmaBitsPerPacketHeader = 32;
    constexpr uint32_t kXmaBitsPerFrameHeader = 15;
    constexpr uint32_t kXmaMaxFrameLength = 0x7FFF;
    constexpr uint32_t kXmaMaxFrameSizeInBits = 0x4000 - kXmaBitsPerPacketHeader;
    constexpr uint32_t kXmaBytesPerBlock = 256; // output granule; the context counts output in these
    constexpr uint32_t kXmaSamplesPerFrame = 512;
    constexpr uint32_t kXmaBytesPerFrameChannel = kXmaSamplesPerFrame * 2; // 16-bit samples
    constexpr uint32_t kXmaSubframesPerChannel = kXmaBytesPerFrameChannel / kXmaBytesPerBlock; // 4
}
