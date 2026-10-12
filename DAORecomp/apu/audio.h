#pragma once
// Host side of the XAudio render driver and the XMA context pool. Frames the game's XAudio2
// submits are queued here for a host audio backend to consume. XMA decoding is in
// xma_decoder.cpp; DAO_XMA=fake (or an FFmpeg without the xmaframes decoder) gives silence.

#include <cstdint>

namespace dao::apu
{
    constexpr uint32_t kFrameChannels = 6;
    constexpr uint32_t kFrameChannelSamples = 256; // 256 samples at 48 kHz = 5.333 ms
    constexpr uint32_t kFrameSampleCount = kFrameChannels * kFrameChannelSamples;

    // Pops the oldest queued frame into out. Layout is channel-major, host-endian floats:
    // channel c, sample s is at out[c * kFrameChannelSamples + s]. Returns false when empty.
    bool PopFrame(float* out6x256);

    uint32_t QueuedFrames();
}
