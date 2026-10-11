#pragma once
// The Graphics tab of the game's Options menu: the runtime patches the game's own
// optionsmenu.gfx (from the disc, or a mod's copy) with the script of
// graphics_tab.as and serves it from userdata/generated over the disc and the mods.
// Settings change live where the renderer allows it and are saved when the menu
// commits its options.
// The script reads and changes the settings of config.toml through the engine's
// Flash callbacks (GetNumber, SendMessage), hooked here.

namespace dao::ui
{
    // Takes the engine graphics switches from the settings and installs the
    // patched menu. Call after the settings are loaded and the mods applied.
    void InitializeGraphicsMenu();

    // Writes optionsmenu.gfx with the Graphics tab to userdata/generated and serves
    // it at the movie's override path (options_movie.cpp). Returns false (the menu
    // stays the game's) if the movie can't be patched.
    bool InstallOptionsMovie();

    // Every frame, on the render thread: enforces the engine graphics settings
    // (shadows, post-processing) the game would otherwise restore.
    void ApplyEngineGraphicsOptions(uint8_t* base);
}
