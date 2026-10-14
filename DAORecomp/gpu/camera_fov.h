#pragma once
// The field of view settings (camera_fov.cpp): the main camera's, widened or
// narrowed in gameplay and in conversations and cutscenes separately.

namespace dao::gpu
{
    // Horizontal degrees at 16:9 (Settings::fov, Settings::conversationFov); the
    // original (kOriginalFov) leaves the game's own. Takes effect at the next frame.
    void SetCameraFov(int gameplay, int conversation);
    // The gameplay camera's field of view now, in horizontal degrees at 16:9: the
    // setting, or the game's own (91.5) with the original.
    double GameplayFovDegrees();

    // Horizontal degrees at 16:9 as the vertical field of view in radians, as the
    // camera takes it.
    double VerticalFov(double degrees);

    // Photo mode's field of view (vertical radians; 0: none) for the fly camera.
    void SetPhotoFov(double radians);
}
