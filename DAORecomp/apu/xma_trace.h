#pragma once
// Opt-in XMA tracing, enabled with DAO_XMA_TRACE=1 (off by default). Call sites pass a null
// XmaTraceStats* when XmaTraceOn() is false, so the traced paths cost a single null check.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include "apu/xma_layout.h"
#include "log.h"

namespace dao::apu
{
    bool XmaTraceOn();

    // Why a pass produced no output, or why decoding stopped early.
    enum class XmaSkip : uint8_t
    {
        None,
        NoOutputBuffer,
        OutputInvalid,
        ZeroBlocks,
        ConsumeOnlyIdle,
        NoSpace,
        NoInput,
        Stalled,
        BadPacketIndex,
        SplitNoNext,
        FrameError,
        NoFrame,
        Count
    };
    const char* XmaSkipName(XmaSkip reason);

    struct XmaTraceStats
    {
        static constexpr uint32_t kEventsPerInterval = 40;
        static constexpr uint32_t kFirstCalls = 20;
        static constexpr uint32_t kFirstPasses = 20;

        // Since the last summary line.
        uint32_t kicks = 0;         // XMA calls that change the context (enable, valid, offsets, init)
        uint32_t passes = 0;        // decode passes run
        uint32_t skips[size_t(XmaSkip::Count)] = {};
        uint32_t swaps = 0;         // input buffer swaps
        uint32_t validClears = 0;   // valid bits the decoder cleared (input, output)
        uint32_t packets = 0;       // packets handed to the decoder
        uint32_t frames = 0;        // frames decoded
        uint32_t frameErrors = 0;   // decode calls that produced no frame (errors and warm-up)
        uint32_t blocks = 0;        // 256-byte output blocks written
        uint64_t samples[2] = {};   // PCM samples per output channel
        double sumSquares[2] = {};  // sum of squared int16 samples per output channel
        uint32_t suppressed = 0;    // events dropped by the interval budget
        uint32_t budget = kEventsPerInterval;

        // Lifetime.
        uint32_t totalCalls = 0;
        uint32_t totalPasses = 0;
        uint32_t channelCount = 0;  // raw channel_count from the last XMAInitializeContext
        uint32_t sampleRateId = 0;  // raw sample_rate from the last XMAInitializeContext
        std::chrono::steady_clock::time_point lastSummary{};
    };

    // One line describing the context fields. dw holds the 16 host-endian dwords.
    std::string XmaDescribe(const uint32_t* dw);

    // Writes the summary line for a context and clears its interval counters.
    void XmaTraceSummary(uint32_t address, const uint32_t* dw, XmaTraceStats& trace,
        std::chrono::steady_clock::time_point now);
}

// One budgeted event. Once the interval budget is spent, events are only counted.
#define XMA_TRACE_EVENT(trace, ...)                                           \
    do                                                                        \
    {                                                                         \
        if ((trace).budget)                                                   \
        {                                                                     \
            --(trace).budget;                                                 \
            LOGI("XMA trace: " __VA_ARGS__);                                  \
        }                                                                     \
        else                                                                  \
        {                                                                     \
            ++(trace).suppressed;                                             \
        }                                                                     \
    } while (0)
