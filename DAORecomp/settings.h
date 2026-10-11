#pragma once
// userdata/config.toml: user-facing settings. Written with defaults on first run;
// the launcher edits it through ReadSettings/SaveSettings.

namespace dao
{
    enum class Upscaler { Bilinear, Cas, Fsr };
    // Temporal upscaling and anti-aliasing of the 3D scenes (FidelityFX API).
    enum class TemporalUpscaler { Off, Auto, Fsr4, Fsr3 };

    // A folder under mods/, in load order (later mods win over earlier ones).
    struct ModState
    {
        std::string folder;
        bool enabled = true;
    };

    struct Settings
    {
        // Show the launcher window before starting the game.
        bool showLauncher = true;

        // Internal rendering resolution multiplier (the game renders at 1280x720):
        // 2 = 2560x1440, 3 = 3840x2160.
        int resolutionScale = 1;
        // Scaling/sharpening applied when presenting to a window of another size.
        Upscaler upscaler = Upscaler::Fsr;
        // FSR: sharpness reduction in stops (0 = sharpest, 2 = softest).
        // CAS: additional sharpness (0..1).
        float sharpness = 0.2f;
        // Upscales the 3D scenes to the window size with motion vectors (the UI
        // is scaled separately); Auto picks FSR 4 where supported, else FSR 3.1.
        TemporalUpscaler temporalUpscaler = TemporalUpscaler::Auto;
        float temporalSharpness = 0.3f; // 0..1
        bool vsync = true;
        // Presents every vblank where the game asks for every second one (its 3D
        // scenes are locked to 30 fps through the D3D presentation interval).
        bool unlockFrameRate = true;
        // HDR output (scRGB swap chain; SDR image expanded on HDR displays).
        bool hdr = false;
        float hdrPaperWhite = 200.0f; // nits
        float hdrPeak = 1000.0f;      // nits
        // Anisotropic texture filtering: 0 = the game's own, else 2, 4, 8 or 16.
        int anisotropicFiltering = 0;
        bool fullscreen = false;
        int windowWidth = 1280;
        int windowHeight = 720;

        // Code patches by id (patches/patches.h); missing ids use the patch default.
        std::map<std::string, bool, std::less<>> patches;
        std::vector<ModState> mods;
    };

    // Reads config.toml as written (no environment overrides, no side effects).
    Settings ReadSettings(const std::filesystem::path& userRoot);
    // Rewrites config.toml from the settings, keeping the [xenia] section as it is.
    void SaveSettings(const std::filesystem::path& userRoot, const Settings& settings);

    // Reads the file (creating it if missing), applies environment overrides and
    // the [xenia] cvar overrides. Must run before the graphics system is created.
    const Settings& LoadSettings(const std::filesystem::path& userRoot);
    const Settings& GetSettings();
}
