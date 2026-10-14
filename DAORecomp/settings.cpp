#include "stdafx.h"
#include "settings.h"
#include "files.h"
#include "text.h"

#include <fstream>
#include <sstream>

#include "xenia/base/cvar.h"

namespace dao
{
    namespace
    {
        namespace fs = std::filesystem;

        Settings s_settings;

        // (toml::parse_file takes the path as text in the ANSI code page.)
        toml::table ParseToml(const fs::path& path)
        {
            std::ifstream file(path, std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            return toml::parse(text, Utf8(path));
        }

        constexpr const char* kXeniaComment = "# Any Xenia GPU option, by its cvar name, e.g. anisotropic_override = 4.\n";

        const char* UpscalerName(Upscaler upscaler)
        {
            switch (upscaler)
            {
            case Upscaler::Bilinear: return "bilinear";
            case Upscaler::Cas: return "cas";
            default: return "fsr";
            }
        }

        // Also the values of the dao_temporal_upscaler Xenia option.
        const char* TemporalUpscalerName(TemporalUpscaler upscaler)
        {
            switch (upscaler)
            {
            case TemporalUpscaler::Off: return "off";
            case TemporalUpscaler::Fsr4: return "fsr4";
            case TemporalUpscaler::Fsr3: return "fsr3";
            case TemporalUpscaler::Xess: return "xess";
            default: return "auto";
            }
        }

        // A TOML string literal (quoted and escaped).
        std::string Quote(const std::string& text)
        {
            std::ostringstream out;
            out << toml::value<std::string>(text);
            return out.str();
        }

        // The [xenia] section of an existing config, verbatim, up to the next section.
        std::string XeniaSection(const std::filesystem::path& path)
        {
            std::ifstream file(path);
            std::string line, section;
            bool inside = false;
            while (std::getline(file, line))
            {
                if (!line.empty() && line[0] == '[')
                    inside = line.rfind("[xenia]", 0) == 0;
                if (inside)
                    section += line + '\n';
            }
            return section;
        }

        // Sets a Xenia cvar from a TOML value through the cvar's own config loader.
        void SetXeniaCvar(const std::string& name, const toml::node& value)
        {
            if (!cvar::ConfigVars)
                return;
            const auto it = cvar::ConfigVars->find(name);
            if (it == cvar::ConfigVars->end())
            {
                LOGW("config.toml: unknown Xenia option '{}'", name);
                return;
            }
            // (TOML allows nan and inf, which no option means; the GPU would get them as constants.)
            if (const auto* number = value.as_floating_point(); number && !std::isfinite(number->get()))
            {
                LOGW("config.toml: '{}' isn't a number; left as it was", name);
                return;
            }
            it->second->LoadConfigValue(&value);
            LOGI("config.toml: {} = {}", name, it->second->config_value());
        }
    }

    void ClampSettings(Settings& s)
    {
        // (A NaN or infinity gets past std::clamp: those are the default instead.)
        const Settings defaults;
        for (auto [value, fallback] : { std::pair{ &s.sharpness, defaults.sharpness },
                 { &s.temporalSharpness, defaults.temporalSharpness }, { &s.hdrPaperWhite, defaults.hdrPaperWhite },
                 { &s.hdrPeak, defaults.hdrPeak }, { &s.neuralStrength, defaults.neuralStrength } })
        {
            if (!std::isfinite(*value))
                *value = fallback;
        }
        s.resolutionScale = std::clamp(s.resolutionScale, 1, kMaxResolutionScale);
        s.sharpness = std::clamp(s.sharpness, 0.0f, 2.0f);
        s.temporalSharpness = std::clamp(s.temporalSharpness, 0.0f, 1.0f);
        s.neuralStrength = std::clamp(s.neuralStrength, 0.0f, 1.0f);
        if (std::ranges::find(kNeuralNetworks, s.neuralNetwork) == std::end(kNeuralNetworks))
            s.neuralNetwork = 0;
        s.neuralPasses = std::clamp(s.neuralPasses, 1, kMaxNeuralPasses);
        s.neuralStyle = std::clamp(s.neuralStyle, 0, kMaxNeuralStyle);
        if (std::ranges::find(kAnisotropicLevels, s.anisotropicFiltering) == std::end(kAnisotropicLevels))
            s.anisotropicFiltering = 0;
        if (std::ranges::find(kFrameRates, s.frameRate) == std::end(kFrameRates))
            s.frameRate = defaults.frameRate;
        s.hdrPaperWhite = std::clamp(s.hdrPaperWhite, kHdrMinNits, kHdrMaxPaperWhite);
        s.hdrPeak = std::clamp(s.hdrPeak, s.hdrPaperWhite, kHdrMaxPeak);
        s.motionBlur = std::clamp(s.motionBlur, 0, kMaxMotionBlur);
        s.lowLatency = std::clamp(s.lowLatency, 0, kMaxLowLatency);
        s.fov = std::clamp(s.fov, kMinFov, kMaxFov);
        s.conversationFov = std::clamp(s.conversationFov, kMinFov, kMaxFov);
        s.windowWidth = std::clamp(s.windowWidth, 320, 16384);
        s.windowHeight = std::clamp(s.windowHeight, 180, 16384);
    }

    std::string_view MotionBlurName(int motionBlur)
    {
        return motionBlur >= kMaxMotionBlur ? "high" : motionBlur > 0 ? "low" : "off";
    }

    std::string_view LowLatencyName(int lowLatency)
    {
        return lowLatency >= kMaxLowLatency ? "boost" : lowLatency > 0 ? "on" : "off";
    }

    std::string_view HdrQualityName(bool high)
    {
        return high ? "high" : "standard";
    }

    std::string FrameRateName(int frameRate)
    {
        return frameRate ? fmt::format("{} fps", frameRate) : std::string("unlimited");
    }

    Settings ReadSettings(const std::filesystem::path& userRoot)
    {
        return TryReadSettings(userRoot).value_or(Settings{});
    }

    std::optional<Settings> TryReadSettings(const std::filesystem::path& userRoot)
    {
        Settings s;
        const auto path = userRoot / "config.toml";
        if (!std::filesystem::exists(path))
            return s;

        toml::table table;
        try
        {
            table = ParseToml(path);
        }
        catch (const toml::parse_error& error)
        {
            LOGE("config.toml: {} (line {}); using defaults", error.description(), error.source().begin.line);
            return std::nullopt;
        }

        s.showLauncher = table["launcher"]["show"].value_or(s.showLauncher);
        s.checkForUpdates = table["launcher"]["check_updates"].value_or(s.checkForUpdates);
        s.modsWarningSeen = table["launcher"]["mods_warning_seen"].value_or(s.modsWarningSeen);

        const auto video = table["video"];
        s.resolutionScale = video["resolution_scale"].value_or(s.resolutionScale);
        const std::string upscaler = video["upscaler"].value_or(std::string("fsr"));
        s.upscaler = upscaler == "bilinear" ? Upscaler::Bilinear : upscaler == "cas" ? Upscaler::Cas : Upscaler::Fsr;
        s.sharpness = video["sharpness"].value_or(s.sharpness);
        const std::string temporal = video["temporal_upscaler"].value_or(std::string("auto"));
        s.temporalUpscaler = temporal == "off" ? TemporalUpscaler::Off
            : temporal == "fsr4"               ? TemporalUpscaler::Fsr4
            : temporal == "fsr3"               ? TemporalUpscaler::Fsr3
            : temporal == "xess"               ? TemporalUpscaler::Xess
                                               : TemporalUpscaler::Auto;
        s.temporalSharpness = video["temporal_sharpness"].value_or(s.temporalSharpness);
        s.frameGeneration = video["frame_generation"].value_or(s.frameGeneration);
        s.neuralRendering = video["neural_rendering"].value_or(s.neuralRendering);
        s.neuralWeights = video["neural_weights"].value_or(s.neuralWeights);
        s.neuralStrength = video["neural_strength"].value_or(s.neuralStrength);
        s.neuralNetwork = video["neural_network"].value_or(s.neuralNetwork);
        s.neuralPasses = video["neural_passes"].value_or(s.neuralPasses);
        s.neuralSkinProtect = video["neural_skin_protect"].value_or(s.neuralSkinProtect);
        s.neuralStyle = video["neural_style"].value_or(s.neuralStyle);
        s.vsync = video["vsync"].value_or(s.vsync);
        const std::string lowLatency = video["low_latency"].value_or(std::string());
        for (int level = 0; level <= kMaxLowLatency; ++level)
        {
            if (lowLatency == LowLatencyName(level))
                s.lowLatency = level;
        }
        // (Before frame_rate, unlock_frame_rate chose between 60 and 30.)
        s.frameRate = video["frame_rate"].value_or(video["unlock_frame_rate"].value_or(true) ? 60 : 30);
        s.fullscreen = video["fullscreen"].value_or(s.fullscreen);
        s.anisotropicFiltering = video["anisotropic_filtering"].value_or(s.anisotropicFiltering);
        s.shadows = video["shadows"].value_or(s.shadows);
        s.postProcessing = video["post_processing"].value_or(s.postProcessing);
        const std::string motionBlur = video["motion_blur"].value_or(std::string());
        for (int level = 0; level <= kMaxMotionBlur; ++level)
        {
            if (motionBlur == MotionBlurName(level))
                s.motionBlur = level;
        }
        s.fov = video["fov"].value_or(s.fov);
        s.conversationFov = video["conversation_fov"].value_or(s.conversationFov);
        s.hdr = video["hdr"].value_or(s.hdr);
        s.hdrPaperWhite = video["hdr_paper_white"].value_or(s.hdrPaperWhite);
        s.hdrPeak = video["hdr_peak"].value_or(s.hdrPeak);
        // (Anything but "high" is Standard.)
        s.hdrHighQuality = video["hdr_quality"].value_or(std::string()) == HdrQualityName(true);
        s.windowWidth = video["window_width"].value_or(s.windowWidth);
        s.windowHeight = video["window_height"].value_or(s.windowHeight);

        if (const auto* patches = table["patches"].as_table())
        {
            for (const auto& [key, value] : *patches)
            {
                if (const auto enabled = value.value<bool>())
                    s.patches[std::string(key.str())] = *enabled;
            }
        }
        if (const auto* mods = table["mods"].as_array())
        {
            for (const auto& node : *mods)
            {
                const auto* mod = node.as_table();
                if (!mod)
                    continue;
                ModState state;
                state.folder = (*mod)["folder"].value_or(std::string());
                state.enabled = (*mod)["enabled"].value_or(true);
                if (!state.folder.empty())
                    s.mods.push_back(std::move(state));
            }
        }
        ClampSettings(s);
        return s;
    }

    void SaveSettings(const std::filesystem::path& userRoot, const Settings& s)
    {
        const auto path = userRoot / "config.toml";
        std::string xenia = XeniaSection(path);
        if (xenia.empty())
            xenia = "[xenia]\n";

        std::string out = fmt::format(R"(# Dragon Age: Origins recomp settings (also edited by the launcher).

[launcher]
# Show the launcher before starting the game (hold Shift at startup to show it anyway).
show = {}
# Look for a newer release on GitHub (github.com/96dot/DAORecomp) when the launcher opens.
check_updates = {}
# The warning that not every mod works was accepted (it's shown before the Mods screen).
mods_warning_seen = {}

[video]
# Internal rendering resolution multiplier (the game renders at 1280x720):
# 1 = 720p, 2 = 1440p, 3 = 2160p (4K). Needs a GPU with tiled resources.
resolution_scale = {}
# Scaling used when the window size differs from the rendering size:
# "fsr" (AMD FidelityFX Super Resolution 1 + RCAS), "cas" (FidelityFX CAS), "bilinear".
upscaler = "{}"
# FSR: sharpness reduction in stops (0 = sharpest, 2 = softest). CAS: extra sharpness 0..1.
sharpness = {}
# Temporal upscaling and anti-aliasing of the 3D scenes to the window size, with motion
# vectors (the UI is scaled by the upscaler above): "off", "auto" (FSR 4 where supported,
# otherwise FSR 3.1), "fsr4", "fsr3", "xess" (Intel XeSS-SR; FSR where it can't run).
# Sharpening 0..1 (FSR only).
temporal_upscaler = "{}"
temporal_sharpness = {}
# Frame generation (AMD FSR 3.1), with temporal upscaling: a frame generated between each
# two of the 3D scenes doubles the frames shown, for half a frame more latency. Best with
# frame_rate at half the display's refresh rate (60 fps on a 120 Hz display): when the
# display can't show twice the frame rate, only the real frames are shown.
frame_generation = {}
# Neural rendering (lmxxf's HIP port of DLSS 5's neural rendering): the 3D scenes run
# through NVIDIA's neural rendering network before temporal upscaling. AMD RDNA 4
# (RX 9000 series) with a current AMD driver and temporal upscaling, in SDR or HDR; on an
# RX 9070 XT it costs about 9.5 ms a frame at 1080 lines, 7 ms at 900. The network's
# weights don't come with the game:
# neural_weights is the folder with them (block0-ffn.f16) in lmxxf's package.
# Strength 0..1 (0 skips the network). The options of lmxxf's packages: neural_network is
# the network's size, 0 (Auto: 720 lines at resolution_scale 1, 1080 above), 1080, 900 or
# 720 (smaller is faster; the scenes are fitted to it); neural_passes runs it 1 to 3 times
# a frame (a stronger look; each pass costs as much as the first), and with more than one
# neural_skin_protect keeps skin as the first pass made it; neural_style is one of
# NVIDIA's three styles, 0 (NVIDIA's default), 1 (lmxxf's default) or 2.
neural_rendering = {}
neural_weights = {}
neural_strength = {}
neural_network = {}
neural_passes = {}
neural_skin_protect = {}
neural_style = {}
# Wait for the display's refresh when presenting: no tearing, and no more frames shown
# than the display refreshes.
vsync = {}
# Low latency (AMD Radeon Anti-Lag 2, with a current AMD driver): each frame starts as late
# as it can still be on time, so the controller is read closer to when the frame is shown.
# "off", "on", "boost" (Anti-Lag 2 has no boost: the same as "on"). Off while frame generation
# is on (it would cost frames).
low_latency = "{}"
# Frames per second: 30 (the game's own lock in its 3D scenes), 60, 90, 120, 144, 165, 240,
# or 0 for unlimited. Above 60 is experimental.
frame_rate = {}
# HDR output. On an HDR display, SDR white is shown at hdr_paper_white nits and
# highlights go up to hdr_peak nits: the 3D scenes are rendered in HDR (the
# highlights the game compresses), menus are the game's image at paper white.
# Harmless on SDR displays (the SDR image, linearized).
hdr = {}
hdr_paper_white = {}
hdr_peak = {}
# "standard": the 3D scenes' HDR passed on in 10 bits (PQ); "high": 16-bit scRGB from the
# temporal upscaler to the display (smoother gradients, more memory traffic).
hdr_quality = "{}"
# Anisotropic texture filtering: 0 (game default), 2, 4, 8 or 16.
anisotropic_filtering = {}
# The engine's dynamic character shadows and post-processing (frame buffer effects);
# turning them off makes the game faster on low-power devices.
shadows = {}
post_processing = {}
# Motion blur of the 3D scenes (camera, objects, characters): "off" (the default), "low",
# "high". Only with temporal upscaling (it uses its motion vectors); costs GPU time where the
# scene moves, more at High.
motion_blur = "{}"
# The camera's field of view, in degrees across at 16:9 (80 to 120; 91 is the game's own):
# in gameplay, and in conversations and cutscenes (scaling each shot's own).
fov = {}
conversation_fov = {}
fullscreen = {}
window_width = {}
window_height = {}

# Code patches (true = on). Unlisted patches use their default.
[patches]
)",
            s.showLauncher, s.checkForUpdates, s.modsWarningSeen, s.resolutionScale, UpscalerName(s.upscaler), s.sharpness,
            TemporalUpscalerName(s.temporalUpscaler), s.temporalSharpness, s.frameGeneration, s.neuralRendering,
            Quote(s.neuralWeights), s.neuralStrength, s.neuralNetwork, s.neuralPasses, s.neuralSkinProtect,
            s.neuralStyle, s.vsync, LowLatencyName(s.lowLatency), s.frameRate,
            s.hdr, s.hdrPaperWhite, s.hdrPeak, HdrQualityName(s.hdrHighQuality), s.anisotropicFiltering, s.shadows, s.postProcessing,
            MotionBlurName(s.motionBlur), s.fov, s.conversationFov, s.fullscreen,
            s.windowWidth, s.windowHeight);
        for (const auto& [id, enabled] : s.patches)
        {
            // Quoted unless a bare TOML key (one quoted by hand may hold anything).
            const bool bare = !id.empty() && std::ranges::all_of(id, [](char c)
                { return std::isalnum(uint8_t(c)) || c == '_' || c == '-'; });
            out += fmt::format("{} = {}\n", bare ? id : Quote(id), enabled);
        }

        out += "\n# Mods: folders under mods/, in load order (later mods win over earlier ones).\n";
        for (const auto& mod : s.mods)
            out += fmt::format("[[mods]]\nfolder = {}\nenabled = {}\n\n", Quote(mod.folder), mod.enabled);
        out += '\n' + std::string(kXeniaComment) + xenia;

        // A failed write leaves the old config.
        if (!WriteFileAtomically(path, out))
            LOGE("config.toml: failed to write {}", Utf8(path));
    }

    const Settings& LoadSettings(const std::filesystem::path& userRoot)
    {
        if (!std::filesystem::exists(userRoot / "config.toml"))
            SaveSettings(userRoot, Settings{});

        Settings& s = s_settings;
        s = ReadSettings(userRoot);

        // DAO_RES_SCALE overrides the file (handy for testing).
        if (const char* env = getenv("DAO_RES_SCALE"))
            s.resolutionScale = std::clamp(atoi(env), 1, kMaxResolutionScale);
        if (const char* env = getenv("DAO_HDR"))
            s.hdr = atoi(env) != 0;
        // (Only a number: atoi would read anything else as 0, unlimited.)
        if (const char* env = getenv("DAO_FRAME_RATE"); env && *env)
        {
            char* end = nullptr;
            const long rate = strtol(env, &end, 10);
            if (!*end && rate >= 0 && rate <= 1000)
            {
                s.frameRate = int(rate);
                ClampSettings(s);
            }
        }

        SetXeniaCvar("draw_resolution_scale_x", toml::value<int64_t>(s.resolutionScale));
        SetXeniaCvar("draw_resolution_scale_y", toml::value<int64_t>(s.resolutionScale));
        SetXeniaCvar("dao_temporal_upscaler", toml::value<std::string>(std::string(TemporalUpscalerName(s.temporalUpscaler))));
        SetXeniaCvar("dao_temporal_sharpness", toml::value<double>(s.temporalSharpness));
        SetXeniaCvar("dao_frame_generation", toml::value<bool>(s.frameGeneration));
        SetXeniaCvar("dao_neural_rendering", toml::value<bool>(NeuralRenderingOn(s)));
        SetXeniaCvar("dao_neural_weights", toml::value<std::string>(s.neuralWeights));
        SetXeniaCvar("dao_neural_strength", toml::value<double>(s.neuralStrength));
        SetXeniaCvar("dao_neural_network",
            toml::value<std::string>(s.neuralNetwork ? std::to_string(s.neuralNetwork) : "auto"));
        SetXeniaCvar("dao_neural_passes", toml::value<int64_t>(s.neuralPasses));
        SetXeniaCvar("dao_neural_skin_protect", toml::value<bool>(s.neuralSkinProtect));
        SetXeniaCvar("dao_neural_style", toml::value<int64_t>(s.neuralStyle));
        // (Not Xenia's own vsync cvar: the guest's vblanks stay at the console's 60 Hz.)
        SetXeniaCvar("dao_present_vsync", toml::value<bool>(s.vsync));
        SetXeniaCvar("dao_low_latency", toml::value<int64_t>(s.lowLatency));
        SetXeniaCvar("present_hdr", toml::value<bool>(s.hdr));
        SetXeniaCvar("present_hdr_paper_white", toml::value<double>(s.hdrPaperWhite));
        SetXeniaCvar("present_hdr_peak", toml::value<double>(s.hdrPeak));
        SetXeniaCvar("dao_hdr_scrgb", toml::value<bool>(s.hdrHighQuality));
        SetXeniaCvar("dao_motion_blur", toml::value<int64_t>(s.motionBlur));
        if (s.anisotropicFiltering)
            SetXeniaCvar("anisotropic_override", toml::value<int64_t>(XeniaAnisotropicLevel(s.anisotropicFiltering)));
        try
        {
            const toml::table table = ParseToml(userRoot / "config.toml");
            if (const auto* xenia = table["xenia"].as_table())
            {
                for (const auto& [key, value] : *xenia)
                    SetXeniaCvar(std::string(key.str()), value);
            }
        }
        catch (const toml::parse_error&)
        {
            // Already reported by ReadSettings.
        }

        LOGI("Settings: resolution scale {}, upscaler {}, sharpness {}, temporal upscaler {}, {}x{}{}, {}{}",
            s.resolutionScale, UpscalerName(s.upscaler), s.sharpness, TemporalUpscalerName(s.temporalUpscaler),
            s.windowWidth, s.windowHeight, s.fullscreen ? " fullscreen" : "", FrameRateName(s.frameRate),
            s.vsync ? ", vsync" : "");
        if (s.frameGeneration)
            LOGI("Settings: frame generation");
        if (NeuralRenderingOn(s))
            LOGI("Settings: neural rendering, strength {}, network {}, {} pass(es){}, style {}, weights in {}",
                s.neuralStrength, s.neuralNetwork ? std::to_string(s.neuralNetwork) : "auto", s.neuralPasses,
                s.neuralSkinProtect ? " (skin protected)" : "", s.neuralStyle, s.neuralWeights);
        if (s.motionBlur)
            LOGI("Settings: motion blur {}", MotionBlurName(s.motionBlur));
        if (s.lowLatency)
            LOGI("Settings: low latency {}", LowLatencyName(s.lowLatency));
        if (s.fov != kOriginalFov || s.conversationFov != kOriginalFov)
            LOGI("Settings: field of view {}, in conversations {}", s.fov, s.conversationFov);
        if (s.hdr)
            LOGI("Settings: HDR output, paper white {} nits, peak {} nits, quality {}", s.hdrPaperWhite, s.hdrPeak,
                HdrQualityName(s.hdrHighQuality));
        return s;
    }

    bool NeuralRenderingOn(const Settings& s)
    {
        return s.neuralRendering && !s.neuralWeights.empty() && s.temporalUpscaler != TemporalUpscaler::Off;
    }

    const Settings& GetSettings()
    {
        return s_settings;
    }
}
