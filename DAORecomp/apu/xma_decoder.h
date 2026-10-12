#pragma once
// XMA decoding for the pool contexts: the real decoder (a port of Xenia's XmaContextNew, using the
// vendored FFmpeg xmaframes codec) and the shared pieces the fake decoder in audio.cpp uses too.

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>

#include "apu/xma_layout.h"
#include "apu/xma_trace.h"
#include "gpu/xenia_host.h"

struct AVCodec;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace dao::apu
{
    // Guest addresses of a context's buffers. Input entries are 0 when unset.
    struct XmaBuffers
    {
        uint32_t input[2];
        uint32_t output;
    };

    // Output ring over the guest output buffer, with the offset rules of Xenia's RingBuffer.
    // Offsets are bytes. Every host write is preceded by the GPU write-watch notice.
    struct XmaOutputRing
    {
        uint32_t guestBase = 0; // guest address of the buffer
        uint8_t* host = nullptr;
        uint32_t capacity = 0;
        uint32_t read = 0;
        uint32_t write = 0;

        bool Empty() const { return read == write; }

        uint32_t WriteCount() const
        {
            if (read == write)
                return capacity;
            if (write < read)
                return read - write;
            return capacity - write + read;
        }

        void Write(const uint8_t* src, uint32_t count)
        {
            count = count < capacity ? count : capacity;
            if (!count)
                return;
            if (write + count < capacity)
            {
                WriteChunk(write, src, count);
                write += count;
            }
            else
            {
                const uint32_t left = capacity - write;
                const uint32_t right = count - left;
                WriteChunk(write, src, left);
                WriteChunk(0, src + left, right);
                write = right;
            }
        }

    private:
        void WriteChunk(uint32_t offset, const uint8_t* src, uint32_t count)
        {
            dao::gpu::PrepareGuestWrite(guestBase + offset, count);
            std::memcpy(host + offset, src, count);
        }
    };

    // Host-endian copy of the 16 context dwords, with field accessors.
    struct XmaContextWords
    {
        uint32_t dw[kContextDwordCount] = {};

        uint32_t Get(XmaField field) const { return GetBits(dw, field); }
        void Set(XmaField field, uint32_t value) { SetBits(dw, field, value); }

        bool IsInputValid(uint32_t index) const { return Get(index ? kInputBuffer1Valid : kInputBuffer0Valid) != 0; }
        bool AnyInputValid() const { return IsInputValid(0) || IsInputValid(1); }
        bool CurrentInputValid() const { return IsInputValid(Get(kCurrentBuffer)); }
        uint32_t InputPacketCount(uint32_t index) const
        {
            return Get(index ? kInputBuffer1PacketCount : kInputBuffer0PacketCount);
        }
        uint32_t CurrentPacketCount() const { return InputPacketCount(Get(kCurrentBuffer)); }
        bool IsConsumeOnly() const { return (InputPacketCount(0) | InputPacketCount(1)) == 0; }
    };

    // True unless the environment sets DAO_XMA=fake. Read once.
    bool XmaUseRealDecoder();

    // Per-context state of the real decoder (Xenia's XmaContextNew members). Owned by one pool
    // context and only used while the caller holds that context's slot lock.
    class XmaRealDecoder
    {
    public:
        XmaRealDecoder();
        ~XmaRealDecoder();
        XmaRealDecoder(const XmaRealDecoder&) = delete;
        XmaRealDecoder& operator=(const XmaRealDecoder&) = delete;

        // False if FFmpeg has no xmaframes decoder or allocation failed. Callers then fall back.
        bool Available() const { return m_codec && m_packet && m_frame; }

        // One Work() pass over the guest context at contextAddress. trace is null unless tracing is on.
        void Work(uint32_t contextAddress, const XmaBuffers& buffers, XmaTraceStats* trace);

        // Drops the host-side decode state (not the guest context).
        void Reset();

    private:
        struct PacketHandle
        {
            uint32_t bufferIndex = 0;
            uint32_t packetIndex = 0;
            bool valid = false;
        };

        XmaSkip RunPass(uint32_t contextAddress, const XmaBuffers& buffers);
        void StoreContextMerged(const XmaContextWords& data, const XmaContextWords& initial, uint32_t contextAddress);
        XmaOutputRing PrepareOutputRing(const XmaContextWords& data);
        void Decode(XmaContextWords& data);
        void Consume(XmaOutputRing& ring, const XmaContextWords& data);
        void AccumulatePcm(const uint8_t* pcm, uint32_t bytes, bool stereo);
        void UpdateLoopStatus(XmaContextWords& data);
        void SwapInputBuffer(XmaContextWords& data);
        int PrepareDecoder(uint32_t sampleRateId, bool twoChannel);
        void PreparePacket(uint32_t frameSize, uint32_t framePadding);
        bool DecodePacket();
        void ConvertFrame(bool twoChannel, uint8_t* out) const;

        PacketHandle GetPacketHandle(const XmaContextWords& data, uint32_t bufferIndex, uint32_t packetIndex,
            uint32_t currentPacketCount) const;
        const uint8_t* GetNextPacket(const XmaContextWords& data, uint32_t nextPacketIndex,
            uint32_t currentPacketCount) const;
        uint32_t GetNextPacketReadOffset(const XmaContextWords& data, uint32_t nextPacketIndex,
            uint32_t currentPacketCount) const;
        const uint8_t* InputBufferHost(uint32_t index) const;

        const AVCodec* m_codec = nullptr;
        AVCodecContext* m_context = nullptr;
        AVPacket* m_packet = nullptr;
        AVFrame* m_frame = nullptr;

        XmaBuffers m_buffers{};

        std::array<uint8_t, kXmaBytesPerPacketData * 2> m_inputBuffer{};   // two packets' payloads
        // Packet header byte + frame bits, then the 64 zero bytes FFmpeg may read past a packet
        // (AV_INPUT_BUFFER_PADDING_SIZE).
        std::array<uint8_t, 1 + 4096 + 64> m_frameBits{};
        std::array<uint8_t, kXmaBytesPerFrameChannel * 2> m_rawFrame{};    // one decoded frame, interleaved PCM

        int32_t m_remainingBlocks = 0;
        uint8_t m_currentFrameRemaining = 0;
        uint8_t m_loopFrameOutputLimit = 0;
        bool m_loopStartSkipPending = false;

        // Per-pass bookkeeping (always kept, cheap) and the optional trace sink.
        uint32_t m_contextAddress = 0;
        uint32_t m_passFrames = 0;
        uint32_t m_passBlocks = 0;
        XmaSkip m_lastNote = XmaSkip::None; // why the last Decode() call stopped without a frame
        XmaTraceStats* m_trace = nullptr;
    };
}
