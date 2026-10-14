#pragma once
// Photo mode (photo_mode.cpp, its overlay photo_overlay.cpp): the game paused and
// seen through a free camera, from the START menu's Photo Mode row (mainmenu.gfx,
// patched by menu_movies.cpp) or L3+R3.

namespace dao::ui
{
    void InitializePhotoMode();

    // On or off, at the game's next frame (if it can be on there: in exploration or
    // combat). Any thread.
    void TogglePhotoMode();

    // XamInputGetState: `buttons` without those photo mode takes for itself while
    // it's on (and after it, until they're released). Any thread.
    uint16_t PhotoModeFilterButtons(uint16_t buttons);

    // Between photo mode and its overlay.
    namespace photo
    {
        // The camera as the overlay sets it (read on the game thread).
        struct CameraSettings
        {
            double fov = 0;   // horizontal degrees at 16:9
            double roll = 0;  // degrees, clockwise
            double speed = 1; // times the fly camera's own
        };
        CameraSettings GetCameraSettings();
        // Whether the overlay asked for the camera to go back to where photo mode
        // started (once).
        bool TakeResetRequest();

        // Shown when photo mode is on, closed when it's off (from the game thread).
        // `fov` is the starting field of view (horizontal degrees at 16:9).
        void ShowOverlay(double fov);
        void CloseOverlay();
    }
}
