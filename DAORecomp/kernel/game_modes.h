#pragma once
// The game's mode controller (one game mode runs at a time) and its main camera, as
// photo mode (ui/photo_mode.cpp) and the field of view settings (gpu/camera_fov.cpp)
// read them.
#include "kernel/game_build.h"

namespace dao::kernel
{
    // The modes, as the controller's constructor (sub_82316410) makes them.
    enum GameMode : int32_t
    {
        kModeExplore = 0,
        kModeCombat = 1,
        kModeDead = 2,
        kModeMenu = 3, // the START menu and the other menus over the world
        kModeConversation = 4,
        kModeCutscene = 5,
        kModeFlyCam = 6, // the development fly camera (photo mode's)
        kModeFixedCamera = 7,
        kModePregame = 8, // before a game: the title screen, character creation
    };

    // The controller's current mode (a pointer), and the mode's number in it.
    inline constexpr uint32_t kCurrentModeOffset = 120;
    inline constexpr uint32_t kModeNumberOffset = 96;

    // The main camera (a pointer). Its vtable's slot 3 is SetFov(camera, vertical
    // field of view in radians).
    inline constexpr GameAddress kMainCamera{ 0x83211D44, 0x832E227C };
    inline constexpr uint32_t kSetFovSlot = 3;
}
