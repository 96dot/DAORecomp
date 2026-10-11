#pragma once
// The Graphics tab of the game's Options menu: the runtime patches the game's own
// optionsmenu.gfx (from the disc, or a mod's copy) with the script of
// graphics_tab.as and serves it from userdata/generated over the disc and the mods.
// The script reads and changes the settings of config.toml through the engine's
// Flash callbacks (GetNumber, SendMessage), hooked here.

namespace dao::ui
{
    // Applies the engine graphics settings and installs the patched menu. Call
    // after the settings are loaded and the mods applied.
    void InitializeGraphicsMenu();

    // options_movie.cpp: writes optionsmenu.gfx with the Graphics tab to
    // userdata/generated and layers that folder over the disc and the mods.
    // Returns false (the menu stays the game's) if the movie can't be patched.
    bool InstallOptionsMovie();

    // Every frame, on the render thread: enforces the engine graphics settings
    // (shadows, post-processing) the game would otherwise restore.
    void ApplyEngineGraphicsOptions(uint8_t* base);
}
