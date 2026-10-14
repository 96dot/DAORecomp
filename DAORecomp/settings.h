#pragma once
// userdata/config.toml: user-facing settings. Written with defaults on first run;
// the launcher and the game's Graphics menu edit it through ReadSettings/SaveSettings.

namespace dao
{
    enum class Upscaler { Bilinear, Cas, Fsr };
    // Temporal upscaling and anti-aliasing of the 3D scenes (AMD FidelityFX API, or
    // Intel XeSS-SR, which falls back to FSR where it can't run).
    enum class TemporalUpscaler { Off, Auto, Fsr4, Fsr3, Xess };
    // Settings::fov and conversationFov: the game's own field of view (60 degrees
    // vertical, 91.5 across at 16:9: 91 stands for it and leaves it as it is), and
    // the limits.
    inline constexpr int kOriginalFov = 91;
    inline constexpr int kMinFov = 80;
    inline constexpr int kMaxFov = 120;
    // The values offered (the in-game Graphics tab's FOV list too).
    inline constexpr int kFovSteps[] = { 80, 85, 91, 95, 100, 105, 110, 115, 120 };
    // Settings::anisotropicFiltering values: 0 = the game default.
    inline constexpr int kAnisotropicLevels[] = { 0, 2, 4, 8, 16 };
    // The frame rates offered (0: unlimited). 30 is the game's own lock in its 3D
    // scenes; above 60 is experimental.
    inline constexpr int kFrameRates[] = { 30, 60, 90, 120, 144, 165, 240, 0 };
    // Above 60 fps, or unlimited: the game's presents aren't held to vblanks.
    inline bool FrameRateAbove60(int frameRate) { return frameRate == 0 || frameRate > 60; }
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
        // Look for a newer release on GitHub when the launcher opens.
        bool checkForUpdates = true;
        // The warning that not every mod works was accepted (shown before the Mods
        // screen first opens).
        bool modsWarningSeen = false;

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
        float temporalSharpness = 0.3f; // 0..1, FSR's (XeSS doesn't sharpen)
        // Frame generation (FSR 3.1): with temporal upscaling, a frame generated
        // between each two of the 3D scenes, shown half a frame apart: up to twice the
        // frames on the display (only real ones while it can't show that many), half a
        // frame more latency.
        bool frameGeneration = false;
        // Neural rendering (lmxxf's HIP port of DLSS 5's neural rendering): the 3D
        // scenes through NVIDIA's network before temporal upscaling. AMD RDNA 4 only,
        // in SDR or HDR. The network's weights don't come with the game: neuralWeights is
        // the folder with them (block0-ffn.f16) in lmxxf's package, as UTF-8.
        bool neuralRendering = false;
        std::string neuralWeights;
        float neuralStrength = 1.0f; // 0..1
        // The options of lmxxf's packages: the network's size (kNeuralNetworks: 0 =
        // by the 3D scenes' size, else its height, smaller is faster), its passes a
        // frame (1 to 3, a stronger style, each as costly as the first), with more
        // than one whether skin keeps the first pass's look, and its style (0 to 2).
        int neuralNetwork = 0;
        int neuralPasses = 1;
        bool neuralSkinProtect = false;
        int neuralStyle = 1;
        // Waits for the display's refresh when presenting: no tearing, and no more
        // frames shown than the display refreshes.
        bool vsync = true;
        // Low latency (AMD Radeon Anti-Lag 2 or NVIDIA Reflex): each frame starts as late
        // as it can still be on time, so the controller is read closer to when it's shown.
        // 0 off, 1 on, 2 (kMaxLowLatency) on with Reflex's boost (Anti-Lag 2 has none: as 1).
        int lowLatency = 0;
        // One of kFrameRates. The game locks its 3D scenes to 30 fps through the D3D
        // presentation interval: 60 presents them every vblank, and above 60 every
        // frame at once, paced to the rate unless unlimited (gpu/video.cpp).
        int frameRate = 60;
        // HDR output (scRGB swap chain; SDR image expanded on HDR displays).
        bool hdr = false;
        float hdrPaperWhite = 200.0f; // nits
        float hdrPeak = 1000.0f;      // nits
        // HDR quality High: the 3D scenes' HDR stays scRGB (16-bit float) from the
        // upscaler to the display, instead of PQ in 10 bits: smoother gradients, more
        // memory traffic (Standard, false, is the default). Meant for temporal
        // upscaling: without it, HDR scenes at another size than the window's are
        // stretched without the output upscaler's FSR 1 or CAS.
        bool hdrHighQuality = false;
        // Motion blur of the 3D scenes, from temporal upscaling's motion vectors (so
        // only with it): 0 off, 1 low, up to kMaxMotionBlur high.
        int motionBlur = 0;
        // The camera's field of view in gameplay, and in conversations and cutscenes
        // (scaling each shot's own): horizontal degrees at 16:9, kMinFov to kMaxFov
        // (gpu/camera_fov.cpp).
        int fov = kOriginalFov;
        int conversationFov = kOriginalFov;
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
    // Motion blur's highest level (Settings::motionBlur); the in-game Graphics tab and the
    // launcher list one name per level.
    constexpr int kMaxMotionBlur = 2;
    // Low latency's highest level (Settings::lowLatency); the in-game Graphics tab and the
    // launcher list one name per level.
    constexpr int kMaxLowLatency = 2;
    // Neural rendering's network sizes (Settings::neuralNetwork), passes and styles.
    constexpr int kNeuralNetworks[] = { 0, 1080, 900, 720 };
    constexpr int kMaxNeuralPasses = 3;
    constexpr int kMaxNeuralStyle = 2;
    constexpr float kHdrMinNits = 80.0f;
    constexpr float kHdrMaxPaperWhite = 1000.0f;
    constexpr float kHdrMaxPeak = 10000.0f;

    // Brings every value within its limits (the HDR peak at least the paper white).
    void ClampSettings(Settings& settings);

    // Motion blur as config.toml writes it: "off", "low", "high".
    std::string_view MotionBlurName(int motionBlur);

    // Low latency as config.toml writes it: "off", "on", "boost".
    std::string_view LowLatencyName(int lowLatency);

    // The HDR quality as config.toml writes it: "high", "standard".
    std::string_view HdrQualityName(bool high);

    // A frame rate for the log: "60 fps", "unlimited".
    std::string FrameRateName(int frameRate);

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
    // Whether neural rendering runs: on, with its weights' folder, and before
    // temporal upscaling (without it, it isn't loaded).
    bool NeuralRenderingOn(const Settings& s);
}
