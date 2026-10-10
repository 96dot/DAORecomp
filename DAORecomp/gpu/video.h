#pragma once

namespace dao::gpu
{
    struct VideoState
    {
        uint32_t displayWidth = 1280;
        uint32_t displayHeight = 720;

        uint32_t interruptCallback = 0;
        uint32_t interruptUserData = 0;

        uint32_t ringBufferBase = 0;
        uint32_t ringBufferSizeLog2 = 0;
        uint32_t readPointerWriteBack = 0;
        uint32_t writeBackBlockSizeLog2 = 0;
        uint32_t gpuIdentifierAddress = 0;

        std::atomic<uint64_t> swapCount{ 0 };
    };

    extern VideoState g_video;

    // Whether 30 fps scenes are presented at 60 (video.unlock_frame_rate; F10 toggles it).
    bool FrameRateUnlocked();
    void SetFrameRateUnlocked(bool unlocked);
}
