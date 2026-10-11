#pragma once
// userdata/config.toml: user-facing settings. Written with defaults on first run;
// the launcher and the game's Graphics menu edit it through ReadSettings/SaveSettings.

namespace dao
{
    enum class Upscaler { Bilinear, Cas, Fsr };
    // Temporal upscaling and anti-aliasing of the 3D scenes (FidelityFX API).
    enum class TemporalUpscaler { Off, Auto, Fsr4, Fsr3 };
    // Settings::anisotropicFiltering values: 0 = the game default.
    inline constexpr int kAnisotropicLevels[] = { 0, 2, 4, 8, 16 };
    // Xenia's anisotropic_override for one of kAnisotropicLevels (-1: the game's own).
    inline int XeniaAnisotropicLevel(int level)
    {
        return level ? 1 + std::countr_zero(unsigned(level)) : -1;
    }

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
        // Anisotropic texture filtering, one of kAnisotropicLevels.
        int anisotropicFiltering = 0;
        // The engine's dynamic character shadows and its post-processing (frame
        // buffer effects): switches of the PC version left in the 360 engine.
        bool shadows = true;
        bool postProcessing = true;
        bool fullscreen = false;
        int windowWidth = 1280;
        int windowHeight = 720;

        // Code patches by id (patches/patches.h); missing ids use the patch default.
        std::map<std::string, bool, std::less<>> patches;
        std::vector<ModState> mods;
    };

    // The limits ClampSettings keeps.
    constexpr int kMaxResolutionScale = 7;
    constexpr float kHdrMinNits = 80.0f;
    constexpr float kHdrMaxPaperWhite = 1000.0f;
    constexpr float kHdrMaxPeak = 10000.0f;

    // Brings every value within its limits (the HDR peak at least the paper white).
    void ClampSettings(Settings& settings);

    // Reads config.toml as written (no environment overrides, no side effects):
    // defaults if it's missing, nothing if it can't be parsed.
    std::optional<Settings> TryReadSettings(const std::filesystem::path& userRoot);
    // TryReadSettings, with defaults if the file can't be parsed.
    Settings ReadSettings(const std::filesystem::path& userRoot);
    // Rewrites config.toml from the settings, keeping the [xenia] section as it is.
    void SaveSettings(const std::filesystem::path& userRoot, const Settings& settings);

    // Reads the file (creating it if missing), applies environment overrides and
    // the [xenia] cvar overrides. Must run before the graphics system is created.
    const Settings& LoadSettings(const std::filesystem::path& userRoot);
    const Settings& GetSettings();
}
