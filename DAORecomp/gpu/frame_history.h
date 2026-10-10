#pragma once
// Debug aid for one-frame rendering glitches (DAO_FRAME_HISTORY=1): a thread
// records every presented frame (half resolution) into a ~3 s ring, and flags a
// frame that differs from both neighbours while they match each other - a
// single-frame flicker - saving the three frames to userdata/flicker/.
// SaveFrameHistory (F12) writes the whole ring to userdata/history/<time>/.

namespace dao::gpu
{
    void StartFrameHistory(const std::filesystem::path& userRoot);
    void SaveFrameHistory();
}
