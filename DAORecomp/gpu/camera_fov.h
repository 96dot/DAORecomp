#pragma once
// The field of view settings (camera_fov.cpp): the main camera's, widened or
// narrowed in gameplay and in conversations and cutscenes separately.

namespace dao::gpu
{
    // Horizontal degrees at 16:9 (Settings::fov, Settings::conversationFov); the
    // original (kOriginalFov) leaves the game's own. Takes effect at the next frame.
    void SetCameraFov(int gameplay, int conversation);
}
