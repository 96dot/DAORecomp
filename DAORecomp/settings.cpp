#include "stdafx.h"
#include "settings.h"

#include <fstream>
#include <sstream>

#include "xenia/base/cvar.h"

namespace dao
{
    namespace
    {
        Settings s_settings;

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
            it->second->LoadConfigValue(&value);
            LOGI("config.toml: {} = {}", name, it->second->config_value());
        }
    }

    void ClampSettings(Settings& s)
    {
        s.resolutionScale = std::clamp(s.resolutionScale, 1, kMaxResolutionScale);
        s.sharpness = std::clamp(s.sharpness, 0.0f, 2.0f);
        s.temporalSharpness = std::clamp(s.temporalSharpness, 0.0f, 1.0f);
        if (std::ranges::find(kAnisotropicLevels, s.anisotropicFiltering) == std::end(kAnisotropicLevels))
            s.anisotropicFiltering = 0;
        s.hdrPaperWhite = std::clamp(s.hdrPaperWhite, kHdrMinNits, kHdrMaxPaperWhite);
        s.hdrPeak = std::clamp(s.hdrPeak, s.hdrPaperWhite, kHdrMaxPeak);
        s.windowWidth = std::max(320, s.windowWidth);
        s.windowHeight = std::max(180, s.windowHeight);
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
            table = toml::parse_file(path.string());
        }
        catch (const toml::parse_error& error)
        {
            LOGE("config.toml: {} (line {}); using defaults", error.description(), error.source().begin.line);
            return std::nullopt;
        }

        s.showLauncher = table["launcher"]["show"].value_or(s.showLauncher);
        s.checkForUpdates = table["launcher"]["check_updates"].value_or(s.checkForUpdates);

        const auto video = table["video"];
        s.resolutionScale = video["resolution_scale"].value_or(s.resolutionScale);
        const std::string upscaler = video["upscaler"].value_or(std::string("fsr"));
        s.upscaler = upscaler == "bilinear" ? Upscaler::Bilinear : upscaler == "cas" ? Upscaler::Cas : Upscaler::Fsr;
        s.sharpness = video["sharpness"].value_or(s.sharpness);
        const std::string temporal = video["temporal_upscaler"].value_or(std::string("auto"));
        s.temporalUpscaler = temporal == "off" ? TemporalUpscaler::Off
            : temporal == "fsr4"               ? TemporalUpscaler::Fsr4
            : temporal == "fsr3"               ? TemporalUpscaler::Fsr3
                                               : TemporalUpscaler::Auto;
        s.temporalSharpness = video["temporal_sharpness"].value_or(s.temporalSharpness);
        s.vsync = video["vsync"].value_or(s.vsync);
        s.unlockFrameRate = video["unlock_frame_rate"].value_or(s.unlockFrameRate);
        s.fullscreen = video["fullscreen"].value_or(s.fullscreen);
        s.anisotropicFiltering = video["anisotropic_filtering"].value_or(s.anisotropicFiltering);
        s.shadows = video["shadows"].value_or(s.shadows);
        s.postProcessing = video["post_processing"].value_or(s.postProcessing);
        s.hdr = video["hdr"].value_or(s.hdr);
        s.hdrPaperWhite = video["hdr_paper_white"].value_or(s.hdrPaperWhite);
        s.hdrPeak = video["hdr_peak"].value_or(s.hdrPeak);
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
# otherwise FSR 3.1), "fsr4", "fsr3". Sharpening 0..1.
temporal_upscaler = "{}"
temporal_sharpness = {}
vsync = {}
# The game locks its 3D scenes to 30 fps; true lets them run at 60.
unlock_frame_rate = {}
# HDR output. On an HDR display, SDR white is shown at hdr_paper_white nits and
# highlights are expanded up to hdr_peak nits. Harmless on SDR displays.
hdr = {}
hdr_paper_white = {}
hdr_peak = {}
# Anisotropic texture filtering: 0 (game default), 2, 4, 8 or 16.
anisotropic_filtering = {}
# The engine's dynamic character shadows and post-processing (frame buffer effects);
# turning them off makes the game faster on low-power devices.
shadows = {}
post_processing = {}
fullscreen = {}
window_width = {}
window_height = {}

# Code patches (true = on). Unlisted patches use their default.
[patches]
)",
            s.showLauncher, s.checkForUpdates, s.resolutionScale, UpscalerName(s.upscaler), s.sharpness,
            TemporalUpscalerName(s.temporalUpscaler), s.temporalSharpness, s.vsync, s.unlockFrameRate,
            s.hdr, s.hdrPaperWhite, s.hdrPeak, s.anisotropicFiltering, s.shadows, s.postProcessing, s.fullscreen,
            s.windowWidth, s.windowHeight);
        for (const auto& [id, enabled] : s.patches)
            out += fmt::format("{} = {}\n", id, enabled);

        out += "\n# Mods: folders under mods/, in load order (later mods win over earlier ones).\n";
        for (const auto& mod : s.mods)
            out += fmt::format("[[mods]]\nfolder = {}\nenabled = {}\n\n", Quote(mod.folder), mod.enabled);
        out += '\n' + std::string(kXeniaComment) + xenia;

        // Write a temporary file and swap it in, so a failed write leaves the old config.
        const auto temp = path.string() + ".tmp";
        {
            std::ofstream file(temp, std::ios::binary | std::ios::trunc);
            file << out;
            if (!file)
            {
                LOGE("config.toml: failed to write {}", temp);
                return;
            }
        }
        if (!MoveFileExA(temp.c_str(), path.string().c_str(), MOVEFILE_REPLACE_EXISTING))
            LOGE("config.toml: failed to replace {} ({})", path.string(), GetLastError());
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
        if (const char* env = getenv("DAO_UNLOCK_FPS"))
            s.unlockFrameRate = atoi(env) != 0;

        SetXeniaCvar("draw_resolution_scale_x", toml::value<int64_t>(s.resolutionScale));
        SetXeniaCvar("draw_resolution_scale_y", toml::value<int64_t>(s.resolutionScale));
        SetXeniaCvar("dao_temporal_upscaler", toml::value<std::string>(std::string(TemporalUpscalerName(s.temporalUpscaler))));
        SetXeniaCvar("dao_temporal_sharpness", toml::value<double>(s.temporalSharpness));
        SetXeniaCvar("vsync", toml::value<bool>(s.vsync));
        SetXeniaCvar("present_hdr", toml::value<bool>(s.hdr));
        SetXeniaCvar("present_hdr_paper_white", toml::value<double>(s.hdrPaperWhite));
        SetXeniaCvar("present_hdr_peak", toml::value<double>(s.hdrPeak));
        if (s.anisotropicFiltering)
            SetXeniaCvar("anisotropic_override", toml::value<int64_t>(XeniaAnisotropicLevel(s.anisotropicFiltering)));
        try
        {
            const toml::table table = toml::parse_file((userRoot / "config.toml").string());
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

        LOGI("Settings: resolution scale {}, upscaler {}, sharpness {}, temporal upscaler {}, {}x{}{}{}",
            s.resolutionScale, UpscalerName(s.upscaler), s.sharpness, TemporalUpscalerName(s.temporalUpscaler),
            s.windowWidth, s.windowHeight, s.fullscreen ? " fullscreen" : "",
            s.unlockFrameRate ? ", frame rate unlocked" : "");
        if (s.hdr)
            LOGI("Settings: HDR output, paper white {} nits, peak {} nits", s.hdrPaperWhite, s.hdrPeak);
        return s;
    }

    const Settings& GetSettings()
    {
        return s_settings;
    }
}
