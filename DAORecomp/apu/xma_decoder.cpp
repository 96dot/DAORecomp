// Port of Xenia's XMA "new" decoder (xenia/apu/xma_context_new.cc) with the packet helpers from
// xma_helpers.h, the BitStream reader from xenia/base/bit_stream.cc, and ConvertFrame from
// xma_context.cc. Credits for the packet and frame handling go to the Xenia authors and to
// libertyv's xma2dec.c (koolkdev). Decoding itself is done by FFmpeg's xmaframes decoder.
#include "stdafx.h"
#include <cmath>
#include <cstdlib>
#include <string_view>
#include "apu/xma_decoder.h"
#include "kernel/memory.h"
#include "kernel/types.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
}

// Logs the first few occurrences of a warning that a bad stream could repeat on every packet.
#define XMA_WARN_LIMITED(...)                                                 \
    do                                                                        \
    {                                                                         \
        static std::atomic<uint32_t> s_warnings{ 0 };                         \
        if (s_warnings.fetch_add(1, std::memory_order_relaxed) < 8)           \
            LOGW(__VA_ARGS__);                                                \
    } while (0)

namespace dao::apu
{
    using dao::kernel::g_memory;
    using dao::kernel::u32;

    namespace
    {
        constexpr int kIdToSampleRate[4] = { 24000, 32000, 44100, 48000 };
        constexpr uint32_t kMaxFrameSizeInBits = kXmaMaxFrameSizeInBits;

        void CopyGuestDwords(uint32_t address, uint32_t* out)
        {
            const u32* guest = g_memory.Translate<u32>(address);
            for (uint32_t i = 0; i < kContextDwordCount; ++i)
                out[i] = guest[i];
        }

        // ------------------------------------------------------------ packet helpers (xma_helpers.h)

        uint8_t PacketFrameCount(const uint8_t* packet) { return packet[0] >> 2; }
        uint8_t PacketMetadata(const uint8_t* packet) { return packet[2] & 0x7; }
        bool IsPacketXma2(const uint8_t* packet) { return PacketMetadata(packet) == 1; }
        uint8_t PacketSkipCount(const uint8_t* packet) { return packet[3]; }

        // Bit offset of the first frame that starts in this packet, from the packet start.
        uint32_t PacketFrameOffset(const uint8_t* packet)
        {
            const uint16_t val = uint16_t(((packet[0] & 0x3) << 13) | (packet[1] << 5) | (packet[2] >> 3));
            return val + kXmaBitsPerPacketHeader;
        }

        // ------------------------------------------------------------ BitStream (xenia/base/bit_stream.cc)

        // Big-endian bit reader. Peek() takes at most 57 bits, as in Xenia.
        class BitReader
        {
        public:
            BitReader(const uint8_t* buffer, size_t sizeBits) : m_buffer(buffer), m_sizeBits(sizeBits) {}

            size_t offset() const { return m_offset; }
            size_t BitsRemaining() const { return m_sizeBits - m_offset; }

            void SetOffset(size_t bits) { m_offset = bits < m_sizeBits ? bits : m_sizeBits; }
            void Advance(size_t bits) { SetOffset(m_offset + bits); }

            uint64_t Peek(size_t numBits) const
            {
                const size_t maxByte = m_sizeBits >= 64 ? (m_sizeBits - 64) >> 3 : 0;
                const size_t offsetBytes = std::min(m_offset >> 3, maxByte);
                const size_t relOffsetBits = m_offset - (offsetBytes << 3);

                uint64_t bits = 0;
                const uint8_t* p = m_buffer + offsetBytes;
                for (int i = 0; i < 8; ++i)
                    bits = (bits << 8) | p[i];

                bits >>= 64 - (relOffsetBits + numBits);
                bits &= (uint64_t(1) << numBits) - 1;
                return bits;
            }

            uint64_t Read(size_t numBits)
            {
                const uint64_t value = Peek(numBits);
                Advance(numBits);
                return value;
            }

            // Copies numBits from the current position into dest, starting in the byte after the
            // first partial byte. Returns the bit offset of the copied data inside dest[0].
            // dest must already be zeroed, as the partial-byte masks assume.
            size_t Copy(uint8_t* dest, size_t numBits)
            {
                const size_t offsetBytes = m_offset >> 3;
                const size_t relOffsetBits = m_offset - (offsetBytes << 3);
                size_t bitsLeft = numBits;
                size_t outBytes = 0;

                if (relOffsetBits)
                {
                    const uint64_t bits = Peek(8 - relOffsetBits);
                    const uint8_t clearMask = uint8_t(~((uint8_t(1) << relOffsetBits) - 1));
                    dest[outBytes] &= clearMask;
                    dest[outBytes] |= uint8_t(bits);

                    bitsLeft -= 8 - relOffsetBits;
                    Advance(8 - relOffsetBits);
                    outBytes++;
                }

                if (bitsLeft >= 8)
                {
                    std::memcpy(dest + outBytes, m_buffer + offsetBytes + outBytes, bitsLeft / 8);
                    outBytes += bitsLeft / 8;
                    Advance((bitsLeft / 8) * 8);
                    bitsLeft -= (bitsLeft / 8) * 8;
                }

                if (bitsLeft)
                {
                    uint64_t bits = Peek(bitsLeft);
                    bits <<= 8 - bitsLeft;
                    const uint8_t clearMask = uint8_t((uint8_t(1) << bitsLeft) - 1);
                    dest[outBytes] &= clearMask;
                    dest[outBytes] |= uint8_t(bits);
                    Advance(bitsLeft);
                }
                return relOffsetBits;
            }

        private:
            const uint8_t* m_buffer;
            size_t m_offset = 0;
            size_t m_sizeBits;
        };

        // ------------------------------------------------------------ frame info

        struct PacketInfo
        {
            uint8_t frameCount = 0;
            uint8_t currentFrame = 0;
            uint32_t currentFrameSize = 0;

            bool IsLastFrameInPacket() const { return frameCount == 0 || currentFrame == frameCount - 1; }
        };

        // Scans the frames that begin in one packet and finds the one at frameOffset.
        PacketInfo GetPacketInfo(const uint8_t* packet, uint32_t frameOffset)
        {
            PacketInfo info{};

            const uint32_t firstFrameOffset = PacketFrameOffset(packet);
            BitReader stream(packet, kXmaBitsPerPacket);
            stream.SetOffset(firstFrameOffset);

            // Tail of a frame that began in the previous packet.
            if (frameOffset < firstFrameOffset)
            {
                info.currentFrame = 0;
                info.currentFrameSize = firstFrameOffset - frameOffset;
            }

            while (true)
            {
                if (stream.BitsRemaining() < kXmaBitsPerFrameHeader)
                {
                    // The 15-bit header runs into the next packet. Count the frame, with size 0,
                    // so the caller takes the split-header path.
                    if (stream.BitsRemaining() > 0)
                    {
                        if (stream.offset() == frameOffset)
                        {
                            info.currentFrame = info.frameCount;
                            info.currentFrameSize = 0;
                        }
                        info.frameCount++;
                    }
                    break;
                }

                const uint64_t frameSize = stream.Peek(kXmaBitsPerFrameHeader);
                if (frameSize == 0 || frameSize == kXmaMaxFrameLength)
                    break;

                if (stream.offset() == frameOffset)
                {
                    info.currentFrame = info.frameCount;
                    info.currentFrameSize = uint32_t(frameSize);
                }
                info.frameCount++;

                if (frameSize > stream.BitsRemaining())
                    break; // last frame, continues in the next packet

                stream.Advance(frameSize - 1);
                // The trailing bit says whether another frame follows in this packet.
                if (stream.Read(1) == 0)
                    break;
            }

            if (IsPacketXma2(packet))
            {
                const uint8_t xma2FrameCount = PacketFrameCount(packet);
                if (xma2FrameCount > info.frameCount)
                {
                    // Header split across the packet boundary. Trust the XMA2 count.
                    if (info.currentFrameSize == 0)
                        info.currentFrame = info.frameCount;
                    info.frameCount = xma2FrameCount;
                }
                else if (xma2FrameCount != info.frameCount)
                {
                    LOGT("XMA: XMA2 header count {} differs from scanned count {}", xma2FrameCount, info.frameCount);
                }
            }
            return info;
        }

        // Index of the packet that contains a bit offset, or -1 if the offset is out of range.
        int PacketNumber(uint32_t sizeBytes, uint32_t bitOffset)
        {
            if (bitOffset < kXmaBitsPerPacketHeader || bitOffset >= (uint64_t(sizeBytes) << 3))
                return -1;
            return int(bitOffset / kXmaBitsPerPacket);
        }

        // Next packet (within one buffer, host pointer) that starts a frame. Returns the bit offset of
        // that frame, or kXmaBitsPerPacketHeader if there is none.
        uint32_t NextPacketReadOffsetInBuffer(const uint8_t* buffer, uint32_t nextPacketIndex, uint32_t packetCount)
        {
            while (nextPacketIndex < packetCount)
            {
                const uint8_t* next = buffer + nextPacketIndex * kXmaBytesPerPacket;
                const uint32_t frameOffset = PacketFrameOffset(next);
                if (frameOffset <= kMaxFrameSizeInBits)
                    return nextPacketIndex * kXmaBitsPerPacket + frameOffset;

                // No frame starts in this packet. Follow its skip count to the next packet of the sub-stream.
                const uint8_t skip = PacketSkipCount(next);
                if (skip == 0xFF)
                    break;
                nextPacketIndex += skip + 1;
            }
            return kXmaBitsPerPacketHeader;
        }

        // Converts one decoded frame (planar float, FFmpeg) into 512 interleaved big-endian int16
        // samples per channel. Matches the x64 path of Xenia's ConvertFrame: scale, round to nearest,
        // saturate.
        void StoreBE16(uint8_t* dst, int32_t value)
        {
            const uint16_t v = uint16_t(int16_t(value));
            dst[0] = uint8_t(v >> 8);
            dst[1] = uint8_t(v & 0xFF);
        }

        int32_t FloatToPcm16(float sample)
        {
            constexpr float kScale = float((1 << 15) - 1);
            float v = sample * kScale;
            if (v != v)
                return 0; // NaN
            v = std::nearbyint(v);
            v = std::clamp(v, -32768.0f, 32767.0f);
            return int32_t(v);
        }
    }

    // ---------------------------------------------------------------- mode switch

    bool XmaUseRealDecoder()
    {
        static const bool s_real = [] {
            const char* value = std::getenv("DAO_XMA");
            return !(value && std::string_view(value) == "fake");
        }();
        return s_real;
    }

    // ---------------------------------------------------------------- XmaRealDecoder

    XmaRealDecoder::XmaRealDecoder()
    {
        m_codec = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
        if (!m_codec)
        {
            LOGW("XMA: FFmpeg has no xmaframes decoder");
            return;
        }
        m_packet = av_packet_alloc();
        m_frame = av_frame_alloc();
        // The codec context is opened on first use with the stream's rate and channel count.
        m_context = avcodec_alloc_context3(m_codec);
        if (!m_packet || !m_frame || !m_context)
        {
            LOGW("XMA: failed to allocate FFmpeg state for a context");
            m_codec = nullptr;
        }
    }

    XmaRealDecoder::~XmaRealDecoder()
    {
        avcodec_free_context(&m_context);
        av_frame_free(&m_frame);
        av_packet_free(&m_packet);
    }

    void XmaRealDecoder::Reset()
    {
        m_remainingBlocks = 0;
        m_currentFrameRemaining = 0;
        m_loopFrameOutputLimit = 0;
        m_loopStartSkipPending = false;
    }

    const uint8_t* XmaRealDecoder::InputBufferHost(uint32_t index) const
    {
        const uint32_t guest = m_buffers.input[index];
        return guest ? g_memory.Translate<uint8_t>(guest) : nullptr;
    }

    XmaOutputRing XmaRealDecoder::PrepareOutputRing(const XmaContextWords& data)
    {
        XmaOutputRing ring;
        ring.guestBase = m_buffers.output;
        ring.host = g_memory.Translate<uint8_t>(m_buffers.output);
        ring.capacity = data.Get(kOutputBufferBlockCount) * kXmaBytesPerBlock;
        ring.read = (data.Get(kOutputBufferReadOffset) * kXmaBytesPerBlock) % ring.capacity;
        ring.write = (data.Get(kOutputBufferWriteOffset) * kXmaBytesPerBlock) % ring.capacity;
        m_remainingBlocks = int32_t(ring.WriteCount() / kXmaBytesPerBlock);
        return ring;
    }

    void XmaRealDecoder::Work(uint32_t contextAddress, const XmaBuffers& buffers, XmaTraceStats* trace)
    {
        m_trace = trace;
        m_contextAddress = contextAddress;
        m_lastNote = XmaSkip::None;
        m_passFrames = 0;
        m_passBlocks = 0;

        if (!m_trace)
        {
            RunPass(contextAddress, buffers);
            return;
        }

        uint32_t before[kContextDwordCount];
        CopyGuestDwords(contextAddress, before);
        const XmaSkip reason = RunPass(contextAddress, buffers);
        uint32_t after[kContextDwordCount];
        CopyGuestDwords(contextAddress, after);

        XmaTraceStats& t = *m_trace;
        t.passes++;
        t.totalPasses++;
        if (reason != XmaSkip::None)
            t.skips[size_t(reason)]++;
        if (t.totalPasses <= XmaTraceStats::kFirstPasses || reason != XmaSkip::None)
        {
            XMA_TRACE_EVENT(t, "ctx {:08X} pass #{} {} frames={} blocks={} | before: {} | after: {}", contextAddress,
                t.totalPasses,
                reason == XmaSkip::None ? std::string("ok") : fmt::format("SKIP({})", XmaSkipName(reason)),
                m_passFrames, m_passBlocks, XmaDescribe(before), XmaDescribe(after));
        }
        m_trace = nullptr;
    }

    // One pass, as Xenia's XmaContextNew::Work(). Returns why the pass produced nothing (None if it wrote
    // output or consumed a frame).
    XmaSkip XmaRealDecoder::RunPass(uint32_t contextAddress, const XmaBuffers& buffers)
    {
        if (!Available() || !buffers.output)
            return XmaSkip::NoOutputBuffer;

        m_buffers = buffers;
        u32* guest = g_memory.Translate<u32>(contextAddress);

        XmaContextWords initial;
        for (uint32_t i = 0; i < kContextDwordCount; ++i)
            initial.dw[i] = guest[i];
        XmaContextWords data = initial;

        if (!data.Get(kOutputBufferValid))
            return XmaSkip::OutputInvalid;
        if (!data.Get(kOutputBufferBlockCount))
            return XmaSkip::ZeroBlocks; // Xenia would divide by zero here

        XmaOutputRing ring = PrepareOutputRing(data);

        if (data.IsConsumeOnly())
        {
            // Nothing to decode. Only drain a frame that is already in progress, so the output
            // offsets the game set are not reset.
            if (m_currentFrameRemaining == 0)
                return XmaSkip::ConsumeOnlyIdle;
            Consume(ring, data);
            data.Set(kOutputBufferWriteOffset, ring.write / kXmaBytesPerBlock);
            if (ring.Empty())
                data.Set(kOutputBufferValid, 0);
            StoreContextMerged(data, initial, contextAddress);
            return XmaSkip::None;
        }

        // Free blocks needed before a decode: one pass of subframes, plus the requested headroom.
        const uint32_t effectiveSdc = std::max<uint32_t>(1, data.Get(kSubframeDecodeCount));
        const int32_t minimumSubframeDecodeCount = int32_t(effectiveSdc + data.Get(kOutputBufferPadding));
        if (minimumSubframeDecodeCount > m_remainingBlocks)
        {
            StoreContextMerged(data, initial, contextAddress);
            return XmaSkip::NoSpace;
        }

        while (m_remainingBlocks >= minimumSubframeDecodeCount)
        {
            const uint32_t preDecodeOffset = data.Get(kInputBufferReadOffset);
            const uint8_t preRemainingSubframes = m_currentFrameRemaining;

            Decode(data);
            Consume(ring, data);

            // Keep draining a partially consumed frame. Work() clears enabled, and only a new kick
            // (XMAEnableContext) would deliver the rest.
            if ((!data.AnyInputValid() || data.Get(kErrorStatus) == 4) && m_currentFrameRemaining == 0)
            {
                m_lastNote = data.AnyInputValid() ? XmaSkip::FrameError : XmaSkip::NoInput;
                break;
            }

            // Decode made no progress and nothing is pending: stop instead of spinning.
            if (preRemainingSubframes == 0 && data.Get(kInputBufferReadOffset) == preDecodeOffset &&
                m_currentFrameRemaining == 0)
            {
                m_lastNote = XmaSkip::Stalled;
                break;
            }
        }

        if (initial.AnyInputValid())
        {
            data.Set(kOutputBufferWriteOffset, ring.write / kXmaBytesPerBlock);
        }
        else if (data.Get(kOutputBufferWriteOffset) != data.Get(kOutputBufferReadOffset))
        {
            // NFS: Carbon and NFS: MW use the write offset as a stall detector. Make them equal when starved.
            data.Set(kOutputBufferWriteOffset, data.Get(kOutputBufferReadOffset));
            data.Set(kOutputBufferValid, 0);
        }

        // Output is full: tell the game.
        if (m_remainingBlocks == 0 && ring.Empty())
            data.Set(kOutputBufferValid, 0);

        StoreContextMerged(data, initial, contextAddress);

        if (m_passBlocks == 0 && m_passFrames == 0)
        {
            if (!initial.AnyInputValid())
                return XmaSkip::NoInput;
            return m_lastNote != XmaSkip::None ? m_lastNote : XmaSkip::NoFrame;
        }
        return XmaSkip::None;
    }

    void XmaRealDecoder::SwapInputBuffer(XmaContextWords& data)
    {
        const uint32_t from = data.Get(kCurrentBuffer);
        if (from == 0)
            data.Set(kInputBuffer0Valid, 0);
        else
            data.Set(kInputBuffer1Valid, 0);
        data.Set(kCurrentBuffer, from ^ 1);
        data.Set(kInputBufferReadOffset, kXmaBitsPerPacketHeader);

        if (m_trace)
        {
            m_trace->swaps++;
            XMA_TRACE_EVENT(*m_trace, "ctx {:08X} swap buffer {} -> {} (input valid now ib0={} ib1={})", m_contextAddress,
                from, from ^ 1, data.Get(kInputBuffer0Valid), data.Get(kInputBuffer1Valid));
        }
    }

    void XmaRealDecoder::UpdateLoopStatus(XmaContextWords& data)
    {
        const uint32_t loopCount = data.Get(kLoopCount);
        if (loopCount == 0)
            return;

        const uint32_t loopStart = std::max<uint32_t>(kXmaBitsPerPacketHeader, data.Get(kLoopStart));
        const uint32_t loopEnd = std::max<uint32_t>(kXmaBitsPerPacketHeader, data.Get(kLoopEnd));
        if (data.Get(kInputBufferReadOffset) != loopEnd)
            return;

        data.Set(kInputBufferReadOffset, loopStart);
        m_loopStartSkipPending = true;
        if (loopCount != 255)
            data.Set(kLoopCount, loopCount - 1);
    }

    XmaRealDecoder::PacketHandle XmaRealDecoder::GetPacketHandle(const XmaContextWords& data, uint32_t bufferIndex,
        uint32_t packetIndex, uint32_t currentPacketCount) const
    {
        PacketHandle result{};
        const bool inNextBuffer = packetIndex >= currentPacketCount;
        if (inNextBuffer)
        {
            bufferIndex ^= 1;
            packetIndex -= currentPacketCount;
        }

        if (!data.IsInputValid(bufferIndex) || !m_buffers.input[bufferIndex])
            return result;
        if (packetIndex >= data.InputPacketCount(bufferIndex))
            return result;

        result.bufferIndex = bufferIndex;
        result.packetIndex = packetIndex;
        result.valid = true;
        return result;
    }

    const uint8_t* XmaRealDecoder::GetNextPacket(const XmaContextWords& data, uint32_t nextPacketIndex,
        uint32_t currentPacketCount) const
    {
        const PacketHandle handle = GetPacketHandle(data, data.Get(kCurrentBuffer), nextPacketIndex, currentPacketCount);
        if (!handle.valid)
            return nullptr;
        return InputBufferHost(handle.bufferIndex) + handle.packetIndex * kXmaBytesPerPacket;
    }

    uint32_t XmaRealDecoder::GetNextPacketReadOffset(const XmaContextWords& data, uint32_t nextPacketIndex,
        uint32_t currentPacketCount) const
    {
        const PacketHandle handle = GetPacketHandle(data, data.Get(kCurrentBuffer), nextPacketIndex, currentPacketCount);
        if (!handle.valid)
            return kXmaBitsPerPacketHeader;
        return NextPacketReadOffsetInBuffer(InputBufferHost(handle.bufferIndex), handle.packetIndex,
            data.InputPacketCount(handle.bufferIndex));
    }

    void XmaRealDecoder::Decode(XmaContextWords& data)
    {
        m_lastNote = XmaSkip::None;

        // No available data.
        if (!data.AnyInputValid())
        {
            m_lastNote = XmaSkip::NoInput;
            return;
        }

        if (m_currentFrameRemaining > 0)
            return;

        if (!data.CurrentInputValid())
        {
            SwapInputBuffer(data);
            if (!data.CurrentInputValid())
            {
                m_lastNote = XmaSkip::NoInput;
                return;
            }
        }

        const uint32_t current = data.Get(kCurrentBuffer);
        const uint8_t* currentInput = InputBufferHost(current);
        if (!currentInput)
        {
            SwapInputBuffer(data);
            m_lastNote = XmaSkip::NoInput;
            return;
        }

        m_inputBuffer.fill(0);

        // Detect the loop end frame before UpdateLoopStatus can move the offset.
        bool isLoopEndFrame = false;
        if (data.Get(kLoopCount) > 0)
        {
            const uint32_t loopEnd = std::max<uint32_t>(kXmaBitsPerPacketHeader, data.Get(kLoopEnd));
            isLoopEndFrame = data.Get(kInputBufferReadOffset) == loopEnd;
        }

        UpdateLoopStatus(data);

        if (!data.Get(kOutputBufferBlockCount))
        {
            m_lastNote = XmaSkip::ZeroBlocks;
            return;
        }

        // Some games kick with a read offset inside the packet header (Dirt 2). Clamp it.
        if (data.Get(kInputBufferReadOffset) < kXmaBitsPerPacketHeader)
            data.Set(kInputBufferReadOffset, kXmaBitsPerPacketHeader);

        const uint32_t currentPacketCount = data.CurrentPacketCount();
        const int packetIndex = PacketNumber(currentPacketCount * kXmaBytesPerPacket, data.Get(kInputBufferReadOffset));
        if (packetIndex < 0)
        {
            m_lastNote = XmaSkip::BadPacketIndex;
            return;
        }

        const uint8_t* packet = currentInput + uint32_t(packetIndex) * kXmaBytesPerPacket;
        const uint32_t packetFirstFrameOffset = PacketFrameOffset(packet);
        uint32_t relativeOffset = data.Get(kInputBufferReadOffset) % kXmaBitsPerPacket;

        // Read offset before the first frame of this packet: we are in the tail of a frame that
        // began in the previous packet. Skip to the first complete frame.
        if (relativeOffset < packetFirstFrameOffset)
        {
            data.Set(kInputBufferReadOffset, uint32_t(packetIndex) * kXmaBitsPerPacket + packetFirstFrameOffset);
            relativeOffset = packetFirstFrameOffset;
        }

        const uint8_t skipCount = PacketSkipCount(packet);

        // Full packet skip (XMA2 0xFF). No frame starts in this packet.
        if (skipCount == 0xFF)
        {
            const uint32_t nextPacketIndexSkip = uint32_t(packetIndex) + 1;
            const uint32_t nextInputOffset = GetNextPacketReadOffset(data, nextPacketIndexSkip, currentPacketCount);
            if (nextPacketIndexSkip >= currentPacketCount || nextInputOffset == kXmaBitsPerPacketHeader)
                SwapInputBuffer(data);
            data.Set(kInputBufferReadOffset, nextInputOffset);
            m_lastNote = XmaSkip::NoFrame;
            return;
        }

        PacketInfo packetInfo = GetPacketInfo(packet, relativeOffset);
        const uint32_t packetToSkip = uint32_t(skipCount) + 1;
        const uint32_t nextPacketIndex = uint32_t(packetIndex) + packetToSkip;

        // Frame header split across the packet boundary: read the 15-bit header from both packets.
        if (packetInfo.currentFrameSize == 0)
        {
            const uint8_t* nextPacket = GetNextPacket(data, nextPacketIndex, currentPacketCount);
            if (!nextPacket)
            {
                // The next buffer is not available yet. Give up on this buffer.
                SwapInputBuffer(data);
                m_lastNote = XmaSkip::SplitNoNext;
                return;
            }
            std::memcpy(m_inputBuffer.data(), packet + kXmaBytesPerPacketHeader, kXmaBytesPerPacketData);
            std::memcpy(m_inputBuffer.data() + kXmaBytesPerPacketData, nextPacket + kXmaBytesPerPacketHeader,
                kXmaBytesPerPacketData);

            BitReader combined(m_inputBuffer.data(), (kXmaBitsPerPacket - kXmaBitsPerPacketHeader) * 2);
            combined.SetOffset(relativeOffset - kXmaBitsPerPacketHeader);

            const uint64_t frameSize = combined.Peek(kXmaBitsPerFrameHeader);
            if (frameSize == kXmaMaxFrameLength)
            {
                XMA_WARN_LIMITED("XMA: split frame header resolved to 0x7FFF, setting error_status=4");
                data.Set(kErrorStatus, 4);
                m_lastNote = XmaSkip::FrameError;
                return;
            }
            packetInfo.currentFrameSize = uint32_t(frameSize);
        }

        BitReader stream(currentInput, size_t(packetIndex + 1) * kXmaBitsPerPacket);
        stream.SetOffset(data.Get(kInputBufferReadOffset));

        const uint64_t bitsToCopy = std::min<uint64_t>(stream.BitsRemaining(), packetInfo.currentFrameSize);
        if (bitsToCopy == 0)
        {
            SwapInputBuffer(data);
            m_lastNote = XmaSkip::NoFrame;
            return;
        }

        // Frame runs into the next packet: fetch it too.
        if (packetInfo.IsLastFrameInPacket() && stream.BitsRemaining() < packetInfo.currentFrameSize)
        {
            const uint8_t* nextPacket = GetNextPacket(data, nextPacketIndex, currentPacketCount);
            if (!nextPacket)
            {
                data.Set(kErrorStatus, 4);
                m_lastNote = XmaSkip::SplitNoNext;
                return;
            }
            std::memcpy(m_inputBuffer.data() + kXmaBytesPerPacketData, nextPacket + kXmaBytesPerPacketHeader,
                kXmaBytesPerPacketData);
        }

        // Copy the current packet's payload, then cut the frame bits out of the combined buffer.
        std::memcpy(m_inputBuffer.data(), packet + kXmaBytesPerPacketHeader, kXmaBytesPerPacketData);

        BitReader frameStream(m_inputBuffer.data(), (kXmaBitsPerPacket - kXmaBitsPerPacketHeader) * 2);
        frameStream.SetOffset(relativeOffset - kXmaBitsPerPacketHeader);

        m_frameBits.fill(0);
        const size_t paddingStart = frameStream.Copy(m_frameBits.data() + 1, packetInfo.currentFrameSize);

        m_rawFrame.fill(0);

        PrepareDecoder(data.Get(kSampleRate), data.Get(kIsStereo) != 0);
        if (m_trace)
            m_trace->packets++;
        PreparePacket(packetInfo.currentFrameSize, uint32_t(paddingStart));
        const bool decoded = DecodePacket();
        if (decoded)
        {
            m_passFrames++;
            if (m_trace)
                m_trace->frames++;

            ConvertFrame(data.Get(kIsStereo) != 0, m_rawFrame.data());
            m_currentFrameRemaining = uint8_t(4 << data.Get(kIsStereo));

            // Loop end: output only up to loop_subframe_end.
            if (isLoopEndFrame)
                m_loopFrameOutputLimit = uint8_t((data.Get(kLoopSubframeEnd) + 1) << data.Get(kIsStereo));
            else
                m_loopFrameOutputLimit = 0;

            // Loop start: drop the leading subframes given by loop_subframe_skip.
            if (m_loopStartSkipPending)
            {
                const uint8_t skip = uint8_t(data.Get(kLoopSubframeSkip) << data.Get(kIsStereo));
                if (skip < m_currentFrameRemaining)
                    m_currentFrameRemaining -= skip;
                m_loopStartSkipPending = false;
            }
        }
        else
        {
            m_lastNote = XmaSkip::FrameError;
            if (m_trace)
                m_trace->frameErrors++;
        }

        // Frame ends before the packet does: continue with the next frame in this packet.
        if (!packetInfo.IsLastFrameInPacket())
        {
            const uint32_t nextFrameOffset = uint32_t((data.Get(kInputBufferReadOffset) + bitsToCopy) % kXmaBitsPerPacket);
            data.Set(kInputBufferReadOffset, uint32_t(packetIndex) * kXmaBitsPerPacket + nextFrameOffset);
            return;
        }

        // Last frame in the packet: move on to the next packet that starts a frame.
        uint32_t nextInputOffset = GetNextPacketReadOffset(data, nextPacketIndex, currentPacketCount);

        if (nextPacketIndex >= currentPacketCount || nextInputOffset == kXmaBitsPerPacketHeader)
            SwapInputBuffer(data);

        if (nextInputOffset == kXmaBitsPerPacketHeader && data.AnyInputValid())
        {
            // At the start of the next buffer. Go to its first frame if it has one.
            const uint8_t* start = InputBufferHost(data.Get(kCurrentBuffer));
            if (start)
                nextInputOffset = PacketFrameOffset(start);
            if (nextInputOffset > kMaxFrameSizeInBits)
            {
                SwapInputBuffer(data);
                return;
            }
        }
        data.Set(kInputBufferReadOffset, nextInputOffset);
    }

    void XmaRealDecoder::Consume(XmaOutputRing& ring, const XmaContextWords& data)
    {
        if (!m_currentFrameRemaining)
            return;

        const uint32_t stereo = data.Get(kIsStereo);
        const uint32_t totalSubframes = kXmaSubframesPerChannel << stereo;
        const uint32_t padding = data.Get(kOutputBufferPadding);

        // Loop end truncation: the rest of the frame is discarded once the limit is reached.
        if (m_loopFrameOutputLimit > 0)
        {
            const uint32_t consumed = totalSubframes - m_currentFrameRemaining;
            if (consumed >= m_loopFrameOutputLimit)
            {
                m_remainingBlocks -= int32_t(padding);
                m_currentFrameRemaining = 0;
                m_loopFrameOutputLimit = 0;
                return;
            }
        }

        // Treat subframe_decode_count 0 as 1 so the pass always makes progress.
        const uint32_t effectiveSdc = std::max<uint32_t>(1, data.Get(kSubframeDecodeCount));
        int32_t subframesToWrite = std::min<int32_t>(m_currentFrameRemaining, int32_t(effectiveSdc));

        if (m_loopFrameOutputLimit > 0)
        {
            const uint32_t consumed = totalSubframes - m_currentFrameRemaining;
            const int32_t remainingUntilLimit = int32_t(m_loopFrameOutputLimit) - int32_t(consumed);
            if (subframesToWrite > remainingUntilLimit)
                subframesToWrite = remainingUntilLimit;
        }

        if (subframesToWrite <= 0)
            return;

        const uint32_t rawFrameReadOffset = totalSubframes - m_currentFrameRemaining;
        const uint32_t bytes = uint32_t(subframesToWrite) * kXmaBytesPerBlock;
        const uint8_t* pcm = m_rawFrame.data() + kXmaBytesPerBlock * rawFrameReadOffset;
        ring.Write(pcm, bytes);

        m_passBlocks += uint32_t(subframesToWrite);
        if (m_trace)
        {
            m_trace->blocks += uint32_t(subframesToWrite);
            AccumulatePcm(pcm, bytes, stereo != 0);
        }

        // Headroom from output_buffer_padding is charged once the frame is fully consumed.
        const int32_t headroom = (m_currentFrameRemaining - subframesToWrite == 0) ? int32_t(padding) : 0;

        m_remainingBlocks -= subframesToWrite + headroom;
        m_currentFrameRemaining = uint8_t(m_currentFrameRemaining - subframesToWrite);
    }

    // Adds the written PCM to the trace (per-channel energy). Interleaved big-endian int16.
    void XmaRealDecoder::AccumulatePcm(const uint8_t* pcm, uint32_t bytes, bool stereo)
    {
        const uint32_t channels = stereo ? 2 : 1;
        for (uint32_t k = 0; k + 1 < bytes; k += 2)
        {
            const int16_t value = int16_t(uint16_t((pcm[k] << 8) | pcm[k + 1]));
            const uint32_t channel = (k / 2) % channels;
            m_trace->sumSquares[channel] += double(value) * double(value);
            m_trace->samples[channel]++;
        }
    }

    int XmaRealDecoder::PrepareDecoder(uint32_t sampleRateId, bool twoChannel)
    {
        const int sampleRate = kIdToSampleRate[std::min<uint32_t>(sampleRateId, 3)];
        const int channels = twoChannel ? 2 : 1;

        if (m_context && m_context->sample_rate == sampleRate && m_context->ch_layout.nb_channels == channels)
            return 0;

        // Rate or channel count changed: recreate and reopen the codec, as Xenia does.
        avcodec_free_context(&m_context);
        m_context = avcodec_alloc_context3(m_codec);
        if (!m_context)
            return -1;

        m_context->sample_rate = sampleRate;
        av_channel_layout_default(&m_context->ch_layout, channels);
        m_context->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;

        if (avcodec_open2(m_context, m_codec, nullptr) < 0)
        {
            XMA_WARN_LIMITED("XMA: failed to open the xmaframes decoder ({} Hz, {} ch)", sampleRate, channels);
            return -1;
        }
        return 1;
    }

    void XmaRealDecoder::PreparePacket(uint32_t frameSize, uint32_t framePadding)
    {
        // FFmpeg reads the frame bits after a one-byte header: padding start (3 bits) and padding end (3 bits).
        m_packet->data = m_frameBits.data();
        const uint32_t bits = framePadding + frameSize;
        m_packet->size = int(1 + (bits / 8) + ((bits % 8) ? 1 : 0));

        const uint32_t paddingEnd = uint32_t(m_packet->size) * 8 - (8 + framePadding + frameSize);
        m_frameBits[0] = uint8_t(((framePadding & 7) << 5) | ((paddingEnd & 7) << 2));
    }

    bool XmaRealDecoder::DecodePacket()
    {
        if (!m_context)
            return false;

        int ret = avcodec_send_packet(m_context, m_packet);
        if (ret < 0)
        {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            XMA_WARN_LIMITED("XMA: error sending a packet for decoding: {} ({})", errbuf, ret);
            return false;
        }

        ret = avcodec_receive_frame(m_context, m_frame);
        if (ret == AVERROR(EAGAIN))
            return false; // the codec wants more input first (start-up)
        if (ret < 0)
        {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            XMA_WARN_LIMITED("XMA: error during decoding: {} ({})", errbuf, ret);
            return false;
        }
        return true;
    }

    void XmaRealDecoder::ConvertFrame(bool twoChannel, uint8_t* out) const
    {
        const float* channel0 = reinterpret_cast<const float*>(m_frame->data[0]);
        const float* channel1 = reinterpret_cast<const float*>(m_frame->data[1]);
        const bool stereo = twoChannel && channel1 != nullptr;

        // XMA frames are always 512 samples; anything FFmpeg did not return is written as silence.
        const uint32_t available = std::min<uint32_t>(uint32_t(std::max(m_frame->nb_samples, 0)), kXmaSamplesPerFrame);

        uint8_t* o = out;
        for (uint32_t i = 0; i < kXmaSamplesPerFrame; ++i)
        {
            const bool have = i < available;
            StoreBE16(o, have ? FloatToPcm16(channel0[i]) : 0);
            o += 2;
            if (stereo)
            {
                StoreBE16(o, have ? FloatToPcm16(channel1[i]) : 0);
                o += 2;
            }
        }
    }

    void XmaRealDecoder::StoreContextMerged(const XmaContextWords& data, const XmaContextWords& initial,
        uint32_t contextAddress)
    {
        // Start from what is in guest memory now, and overwrite only the fields the decoder owns.
        u32* guest = g_memory.Translate<u32>(contextAddress);
        XmaContextWords fresh;
        for (uint32_t i = 0; i < kContextDwordCount; ++i)
            fresh.dw[i] = guest[i];

        auto noteClear = [&](const char* what) {
            if (m_trace)
            {
                m_trace->validClears++;
                XMA_TRACE_EVENT(*m_trace, "ctx {:08X} decoder cleared {}", m_contextAddress, what);
            }
        };

        // DWORD 0: loop count and output write offset. Valid flags only clear when the decoder consumed them.
        fresh.Set(kLoopCount, data.Get(kLoopCount));
        fresh.Set(kOutputBufferWriteOffset, data.Get(kOutputBufferWriteOffset));
        if (initial.Get(kInputBuffer0Valid) && !data.Get(kInputBuffer0Valid))
        {
            fresh.Set(kInputBuffer0Valid, 0);
            noteClear("input buffer 0 valid");
        }
        if (initial.Get(kInputBuffer1Valid) && !data.Get(kInputBuffer1Valid))
        {
            fresh.Set(kInputBuffer1Valid, 0);
            noteClear("input buffer 1 valid");
        }

        // DWORD 1: output valid, cleared by the decoder only.
        if (initial.Get(kOutputBufferValid) && !data.Get(kOutputBufferValid))
        {
            fresh.Set(kOutputBufferValid, 0);
            noteClear("output valid");
        }

        // DWORD 2 and 4: read offset, error status, current buffer.
        fresh.Set(kInputBufferReadOffset, data.Get(kInputBufferReadOffset));
        fresh.Set(kErrorStatus, data.Get(kErrorStatus));
        fresh.Set(kCurrentBuffer, data.Get(kCurrentBuffer));

        // DWORD 9: output read offset.
        fresh.Set(kOutputBufferReadOffset, data.Get(kOutputBufferReadOffset));

        for (uint32_t i = 0; i < kContextDwordCount; ++i)
            guest[i] = fresh.dw[i];
    }
}
