#include "stdafx.h"
#include <cmath>
#include <cstdlib>
#include <string_view>
#include "apu/xma_trace.h"

namespace dao::apu
{
    bool XmaTraceOn()
    {
        static const bool s_on = [] {
            const char* value = std::getenv("DAO_XMA_TRACE");
            return value && std::string_view(value) == "1";
        }();
        return s_on;
    }

    const char* XmaSkipName(XmaSkip reason)
    {
        switch (reason)
        {
        case XmaSkip::None: return "none";
        case XmaSkip::NoOutputBuffer: return "no-output-buffer";
        case XmaSkip::OutputInvalid: return "output-invalid";
        case XmaSkip::ZeroBlocks: return "zero-blocks";
        case XmaSkip::ConsumeOnlyIdle: return "consume-only-idle";
        case XmaSkip::NoSpace: return "no-space";
        case XmaSkip::NoInput: return "no-valid-input";
        case XmaSkip::Stalled: return "decode-stalled";
        case XmaSkip::BadPacketIndex: return "bad-packet-index";
        case XmaSkip::SplitNoNext: return "split-no-next-packet";
        case XmaSkip::FrameError: return "frame-error";
        case XmaSkip::NoFrame: return "no-frame";
        case XmaSkip::Count: break;
        }
        return "?";
    }

    std::string XmaDescribe(const uint32_t* dw)
    {
        return fmt::format(
            "ib0 v{} n{} | ib1 v{} n{} | cur{} rd{} | out v{} blk{} rd{} wr{} | sdc{} pad{} rate{} stereo{} | "
            "loop n{} start{} end{} subEnd{} skip{} | err{}",
            GetBits(dw, kInputBuffer0Valid), GetBits(dw, kInputBuffer0PacketCount),
            GetBits(dw, kInputBuffer1Valid), GetBits(dw, kInputBuffer1PacketCount),
            GetBits(dw, kCurrentBuffer), GetBits(dw, kInputBufferReadOffset),
            GetBits(dw, kOutputBufferValid), GetBits(dw, kOutputBufferBlockCount),
            GetBits(dw, kOutputBufferReadOffset), GetBits(dw, kOutputBufferWriteOffset),
            GetBits(dw, kSubframeDecodeCount), GetBits(dw, kOutputBufferPadding),
            GetBits(dw, kSampleRate), GetBits(dw, kIsStereo),
            GetBits(dw, kLoopCount), GetBits(dw, kLoopStart), GetBits(dw, kLoopEnd),
            GetBits(dw, kLoopSubframeEnd), GetBits(dw, kLoopSubframeSkip),
            GetBits(dw, kErrorStatus));
    }

    void XmaTraceSummary(uint32_t address, const uint32_t* dw, XmaTraceStats& t,
        std::chrono::steady_clock::time_point now)
    {
        const double seconds = std::chrono::duration<double>(now - t.lastSummary).count();

        std::string skips;
        for (size_t i = 1; i < size_t(XmaSkip::Count); ++i)
        {
            if (t.skips[i])
                skips += fmt::format(" {}={}", XmaSkipName(XmaSkip(i)), t.skips[i]);
        }

        auto rms = [&](int channel) {
            return t.samples[channel] ? std::sqrt(t.sumSquares[channel] / double(t.samples[channel])) / 32768.0 : 0.0;
        };

        LOGI("XMA trace: ctx {:08X} {:.1f}s kicks={} passes={} frames={} frameErr={} packets={} swaps={} validClears={} "
             "blocks={} rms L={:.4f} R={:.4f} skips:[{} ] suppressed={} | ch={} rate={} | {}",
            address, seconds, t.kicks, t.passes, t.frames, t.frameErrors, t.packets, t.swaps, t.validClears,
            t.blocks, rms(0), rms(1), skips, t.suppressed, t.channelCount, t.sampleRateId, XmaDescribe(dw));

        t.kicks = 0;
        t.passes = 0;
        std::fill(std::begin(t.skips), std::end(t.skips), 0u);
        t.swaps = 0;
        t.validClears = 0;
        t.packets = 0;
        t.frames = 0;
        t.frameErrors = 0;
        t.blocks = 0;
        t.samples[0] = t.samples[1] = 0;
        t.sumSquares[0] = t.sumSquares[1] = 0.0;
        t.suppressed = 0;
        t.budget = XmaTraceStats::kEventsPerInterval;
        t.lastSummary = now;
    }
}
