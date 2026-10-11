// The host side of the Options menu's Graphics tab (graphics_tab.as): the settings
// it shows, read and changed through the engine's Flash callbacks, applied at once
// where they can be, and saved to config.toml when the menu commits its options.
#include "stdafx.h"
#include "ui/graphics_menu.h"
#include "gpu/video.h"
#include "gpu/xenia_host.h"
#include "kernel/function.h"
#include "kernel/vfs.h"
#include "settings.h"

#include <bitset>
#include <cmath>

#include "xenia/base/cvar.h"

DECLARE_double(dao_temporal_sharpness);
DECLARE_int32(anisotropic_override);
DECLARE_double(present_hdr_paper_white);
DECLARE_double(present_hdr_peak);

// The engine's Flash callbacks: the movie (as a ref-counted pointer) a callback
// returns its value to.
PPC_EXTERN_FUNC(sub_82282DE0);

namespace dao::ui
{
    namespace
    {
        // The settings of the tab, by the names the script uses.
        enum class Key
        {
            Fullscreen,
            ResolutionScale,
            UnlockFrameRate,
            VSync,
            TemporalUpscaler,
            TemporalSharpness,
            Upscaler,
            Sharpness,
            AnisotropicFiltering,
            Shadows,
            PostProcessing,
            Hdr,
            HdrPaperWhite,
            HdrPeak,
            Count
        };

        struct KeyInfo
        {
            std::string_view name;
            bool restart; // takes effect at the next start
        };

        constexpr KeyInfo kKeys[] = {
            { "Fullscreen", false },
            { "ResolutionScale", true },
            { "UnlockFrameRate", false },
            { "VSync", true },
            { "TemporalUpscaler", true },
            { "TemporalSharpness", false },
            { "Upscaler", false },
            { "Sharpness", false },
            { "AnisotropicFiltering", false },
            { "Shadows", false },
            { "PostProcessing", false },
            { "Hdr", true },
            { "HdrPaperWhite", false },
            { "HdrPeak", false },
        };
        static_assert(std::size(kKeys) == size_t(Key::Count));

        constexpr std::string_view kPrefix = "DAORecomp.";
        constexpr std::string_view kPendingPrefix = "Pending.";
        // The slider steps of the sharpness settings (0 to 10).
        constexpr double kSliderSteps = 10.0;

        // The engine's switches for the PC version's EnableShadows and
        // EnableFrameBufferEffects options (applied by its options code at 82274168).
        constexpr uint32_t kRenderFlags = 0x83211E54;
        constexpr uint32_t kRenderFlagShadows = 2;
        constexpr uint32_t kFrameBufferEffects = 0x83211E0B; // byte

        // GFx value types (the Flash callbacks' arguments and results).
        constexpr uint32_t kGfxNull = 1;
        constexpr uint32_t kGfxNumber = 3;
        constexpr uint32_t kGfxString = 4;

        std::atomic<bool> s_shadows{ true };
        std::atomic<bool> s_postProcessing{ true };

        // The menu's view of the settings, and the keys it changed since the last save.
        std::mutex s_menuMutex;
        std::optional<Settings> s_menu;
        std::bitset<size_t(Key::Count)> s_dirty;

        Settings& Menu()
        {
            if (!s_menu)
                s_menu = GetSettings();
            // F10 toggles it too.
            s_menu->unlockFrameRate = gpu::FrameRateUnlocked();
            return *s_menu;
        }

        std::optional<Key> FindKey(std::string_view name)
        {
            for (size_t i = 0; i < std::size(kKeys); ++i)
            {
                if (kKeys[i].name == name)
                    return Key(i);
            }
            return std::nullopt;
        }

        // The output sharpening slider (0 to 10, sharper upward) for the upscaler:
        // FSR's sharpness reduction from 2 to 0 stops, CAS's extra sharpness 0 to 1.
        double SharpnessSteps(const Settings& s)
        {
            return s.upscaler == Upscaler::Cas ? s.sharpness * kSliderSteps : (2.0 - s.sharpness) * kSliderSteps / 2.0;
        }

        void SetSharpnessSteps(Settings& s, double steps)
        {
            steps = std::clamp(steps, 0.0, kSliderSteps);
            s.sharpness = float(s.upscaler == Upscaler::Cas ? steps / kSliderSteps : 2.0 - steps * 2.0 / kSliderSteps);
        }

        double Value(const Settings& s, Key key)
        {
            switch (key)
            {
            case Key::Fullscreen: return s.fullscreen;
            case Key::ResolutionScale: return s.resolutionScale;
            case Key::UnlockFrameRate: return s.unlockFrameRate;
            case Key::VSync: return s.vsync;
            case Key::TemporalUpscaler: return int(s.temporalUpscaler);
            case Key::TemporalSharpness: return std::round(s.temporalSharpness * kSliderSteps);
            case Key::Upscaler: return int(s.upscaler);
            case Key::Sharpness: return std::round(SharpnessSteps(s));
            case Key::AnisotropicFiltering: return s.anisotropicFiltering;
            case Key::Shadows: return s.shadows;
            case Key::PostProcessing: return s.postProcessing;
            case Key::Hdr: return s.hdr;
            case Key::HdrPaperWhite: return s.hdrPaperWhite;
            case Key::HdrPeak: return s.hdrPeak;
            case Key::Count: break;
            }
            return 0.0;
        }

        // Sets a value from the script, within the limits ReadSettings keeps.
        void SetValue(Settings& s, Key key, double value)
        {
            const int whole = int(std::lround(std::clamp(value, -1e6, 1e6)));
            switch (key)
            {
            case Key::Fullscreen: s.fullscreen = whole != 0; break;
            case Key::ResolutionScale: s.resolutionScale = std::clamp(whole, 1, 7); break;
            case Key::UnlockFrameRate: s.unlockFrameRate = whole != 0; break;
            case Key::VSync: s.vsync = whole != 0; break;
            case Key::TemporalUpscaler:
                s.temporalUpscaler = TemporalUpscaler(std::clamp(whole, 0, int(TemporalUpscaler::Fsr3)));
                break;
            case Key::TemporalSharpness:
                s.temporalSharpness = float(std::clamp(value, 0.0, kSliderSteps) / kSliderSteps);
                break;
            case Key::Upscaler:
            {
                // The sharpening slider keeps its position.
                const double steps = SharpnessSteps(s);
                s.upscaler = Upscaler(std::clamp(whole, 0, int(Upscaler::Fsr)));
                SetSharpnessSteps(s, steps);
                break;
            }
            case Key::Sharpness: SetSharpnessSteps(s, value); break;
            case Key::AnisotropicFiltering:
                if (std::ranges::find(kAnisotropicLevels, whole) != std::end(kAnisotropicLevels))
                    s.anisotropicFiltering = whole;
                break;
            case Key::Shadows: s.shadows = whole != 0; break;
            case Key::PostProcessing: s.postProcessing = whole != 0; break;
            case Key::Hdr: s.hdr = whole != 0; break;
            case Key::HdrPaperWhite: s.hdrPaperWhite = float(std::clamp(value, 80.0, 1000.0)); break;
            case Key::HdrPeak: s.hdrPeak = float(std::clamp(value, 80.0, 10000.0)); break;
            case Key::Count: break;
            }
        }

        // Copies the fields behind a key (the upscaler and its sharpness go together).
        void CopyKey(Settings& to, const Settings& from, Key key)
        {
            switch (key)
            {
            case Key::Fullscreen: to.fullscreen = from.fullscreen; break;
            case Key::ResolutionScale: to.resolutionScale = from.resolutionScale; break;
            case Key::UnlockFrameRate: to.unlockFrameRate = from.unlockFrameRate; break;
            case Key::VSync: to.vsync = from.vsync; break;
            case Key::TemporalUpscaler: to.temporalUpscaler = from.temporalUpscaler; break;
            case Key::TemporalSharpness: to.temporalSharpness = from.temporalSharpness; break;
            case Key::Upscaler:
            case Key::Sharpness:
                to.upscaler = from.upscaler;
                to.sharpness = from.sharpness;
                break;
            case Key::AnisotropicFiltering: to.anisotropicFiltering = from.anisotropicFiltering; break;
            case Key::Shadows: to.shadows = from.shadows; break;
            case Key::PostProcessing: to.postProcessing = from.postProcessing; break;
            case Key::Hdr: to.hdr = from.hdr; break;
            case Key::HdrPaperWhite: to.hdrPaperWhite = from.hdrPaperWhite; break;
            case Key::HdrPeak: to.hdrPeak = from.hdrPeak; break;
            case Key::Count: break;
            }
        }

        // Applies a changed setting now; restart-only ones wait for the next start.
        void ApplyLive(const Settings& s, Key key)
        {
            switch (key)
            {
            case Key::Fullscreen: gpu::XeniaSetFullscreen(s.fullscreen); break;
            case Key::UnlockFrameRate: gpu::SetFrameRateUnlocked(s.unlockFrameRate); break;
            case Key::TemporalSharpness: cvars::dao_temporal_sharpness = s.temporalSharpness; break;
            case Key::Upscaler:
            case Key::Sharpness: gpu::XeniaSetOutputScaling(s.upscaler, s.sharpness); break;
            case Key::AnisotropicFiltering:
                cvars::anisotropic_override = XeniaAnisotropicLevel(s.anisotropicFiltering);
                break;
            case Key::Shadows: s_shadows = s.shadows; break;
            case Key::PostProcessing: s_postProcessing = s.postProcessing; break;
            case Key::HdrPaperWhite: cvars::present_hdr_paper_white = s.hdrPaperWhite; break;
            case Key::HdrPeak: cvars::present_hdr_peak = s.hdrPeak; break;
            default: break;
            }
        }

        // The value the script reads for "DAORecomp.<name>", if it's a setting.
        std::optional<double> MenuNumber(std::string_view name)
        {
            std::lock_guard lock(s_menuMutex);
            const Settings& s = Menu();
            if (name.starts_with(kPendingPrefix))
            {
                const auto key = FindKey(name.substr(kPendingPrefix.size()));
                if (!key)
                    return std::nullopt;
                return kKeys[size_t(*key)].restart && Value(s, *key) != Value(GetSettings(), *key) ? 1.0 : 0.0;
            }
            const auto key = FindKey(name);
            if (!key)
                return std::nullopt;
            return Value(s, *key);
        }

        void SetMenuValue(Key key, double value)
        {
            std::lock_guard lock(s_menuMutex);
            Settings& s = Menu();
            const Settings before = s;
            SetValue(s, key, value);
            if (Value(s, key) == Value(before, key) && s.sharpness == before.sharpness)
                return;
            s_dirty.set(size_t(key));
            ApplyLive(s, key);
            LOGI("Graphics menu: {} = {}", kKeys[size_t(key)].name, Value(s, key));
        }

        void RestoreDefaults()
        {
            std::lock_guard lock(s_menuMutex);
            Settings& s = Menu();
            const Settings defaults;
            for (size_t i = 0; i < size_t(Key::Count); ++i)
            {
                // The display mode is the player's setup more than a setting.
                const Key key = Key(i);
                if (key == Key::Fullscreen ||
                    (Value(s, key) == Value(defaults, key) && s.sharpness == defaults.sharpness))
                    continue;
                CopyKey(s, defaults, key);
                s_dirty.set(i);
                ApplyLive(s, key);
            }
            LOGI("Graphics menu: defaults restored");
        }

        // Saves the changed settings over what config.toml holds now (keeping the
        // rest of the file, and not saving environment overrides of this run).
        void SaveMenuSettings()
        {
            std::lock_guard lock(s_menuMutex);
            if (s_dirty.none())
                return;
            const auto& root = kernel::VfsUserRoot();
            Settings file = ReadSettings(root);
            for (size_t i = 0; i < size_t(Key::Count); ++i)
            {
                if (s_dirty.test(i))
                    CopyKey(file, *s_menu, Key(i));
            }
            SaveSettings(root, file);
            s_dirty.reset();
            LOGI("Graphics menu: settings saved");
        }

        // "Set <key> <value>" or "RestoreDefaults", after "DAORecomp.".
        void HandleMessage(std::string_view message)
        {
            if (message == "RestoreDefaults")
            {
                RestoreDefaults();
                return;
            }
            constexpr std::string_view kSet = "Set ";
            if (!message.starts_with(kSet))
            {
                LOGW("Graphics menu: unknown message '{}'", message);
                return;
            }
            message.remove_prefix(kSet.size());
            const size_t space = message.find(' ');
            const auto key = FindKey(message.substr(0, space));
            if (!key || space == std::string_view::npos)
            {
                LOGW("Graphics menu: bad message 'Set {}'", message);
                return;
            }
            const std::string text(message.substr(space + 1));
            char* end = nullptr;
            const double value = strtod(text.c_str(), &end);
            if (end == text.c_str() || !std::isfinite(value))
            {
                LOGW("Graphics menu: bad value 'Set {}'", message);
                return;
            }
            SetMenuValue(*key, value);
        }

        // A NUL-terminated guest string the engine passed (at most 256 characters).
        std::string_view GuestString(uint8_t* base, uint32_t address)
        {
            if (!address)
                return {};
            const char* text = reinterpret_cast<const char*>(base + address);
            return std::string_view(text, strnlen(text, 256));
        }

        // Returns a value to the Flash call being handled, as the engine's own
        // callbacks do (GetNumber, 825B5410): the movie from 82282DE0(&slot,
        // handler), its vtable+0xA0 given the value, then released.
        void ReturnValue(PPCContext& ctx, uint8_t* base, uint32_t handler, uint32_t type, double number)
        {
            // Scratch below the stack pointer, the callees' frames below that.
            const uint32_t stack = ctx.r1.u32;
            const uint32_t frame = (stack - 0x80) & ~0xFu;
            const uint32_t slot = frame + 0x50;
            const uint32_t value = frame + 0x60;
            PPC_STORE_U32(frame, stack);
            ctx.r1.u64 = frame;

            ctx.r3.u64 = slot;
            ctx.r4.u64 = handler;
            sub_82282DE0(ctx, base);
            const uint32_t movie = PPC_LOAD_U32(slot);
            if (movie)
            {
                PPC_STORE_U32(value, type);
                PPC_STORE_U32(value + 4, 0);
                PPC_STORE_U64(value + 8, std::bit_cast<uint64_t>(number));
                ctx.r3.u64 = movie;
                ctx.r4.u64 = value;
                PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(PPC_LOAD_U32(movie) + 0xA0));
                ctx.r3.u64 = movie;
                ctx.r4.u64 = 0;
                PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(PPC_LOAD_U32(movie + 8) + 4));
            }
            ctx.r1.u64 = stack;
        }
    }

    void InitializeGraphicsMenu()
    {
        const Settings& s = GetSettings();
        s_shadows = s.shadows;
        s_postProcessing = s.postProcessing;
        InstallOptionsMovie();
    }

    void ApplyEngineGraphicsOptions(uint8_t* base)
    {
        // What the engine had when a switch was forced off, restored when it's back
        // on (render thread only).
        static std::optional<uint32_t> s_savedShadows;
        static std::optional<uint8_t> s_savedEffects;

        const uint32_t flags = PPC_LOAD_U32(kRenderFlags);
        if (!s_shadows.load(std::memory_order_relaxed))
        {
            if (!s_savedShadows)
            {
                s_savedShadows = flags & kRenderFlagShadows;
                LOGI("Engine graphics: dynamic shadows off");
            }
            if (flags & kRenderFlagShadows)
                PPC_STORE_U32(kRenderFlags, flags & ~kRenderFlagShadows);
        }
        else if (s_savedShadows)
        {
            PPC_STORE_U32(kRenderFlags, flags | *s_savedShadows);
            s_savedShadows.reset();
            LOGI("Engine graphics: dynamic shadows on");
        }

        const uint8_t effects = base[kFrameBufferEffects];
        if (!s_postProcessing.load(std::memory_order_relaxed))
        {
            if (!s_savedEffects)
            {
                s_savedEffects = effects;
                LOGI("Engine graphics: post-processing off");
            }
            if (effects)
                base[kFrameBufferEffects] = 0;
        }
        else if (s_savedEffects)
        {
            base[kFrameBufferEffects] = *s_savedEffects;
            s_savedEffects.reset();
            LOGI("Engine graphics: post-processing on");
        }
    }
}

// FSCommand SendMessage (ExternalCommands.SendMessage): r4 = the message. The
// tab's own messages stop here; the menu's commit also saves its settings.
extern "C" PPC_FUNC(__imp__sub_825AF710);
PPC_FUNC(sub_825AF710)
{
    using namespace dao::ui;
    std::string_view message = GuestString(base, ctx.r4.u32);
    if (message.starts_with(kPrefix))
    {
        HandleMessage(message.substr(kPrefix.size()));
        return;
    }
    if (message == "ClientOptions.CommitOptions")
        SaveMenuSettings();
    __imp__sub_825AF710(ctx, base);
}

// ExternalInterface GetNumber: r3 = the handler, r4 = the arguments (GFx values:
// type at +0, data at +8), r5 = their count. Answers "DAORecomp.<name>".
extern "C" PPC_FUNC(__imp__sub_825B5410);
PPC_FUNC(sub_825B5410)
{
    using namespace dao::ui;
    if (ctx.r5.u32 == 1 && PPC_LOAD_U32(ctx.r4.u32) == kGfxString)
    {
        const std::string_view name = GuestString(base, PPC_LOAD_U32(ctx.r4.u32 + 8));
        if (name.starts_with(kPrefix))
        {
            const auto number = MenuNumber(name.substr(kPrefix.size()));
            if (!number)
                LOGW("Graphics menu: unknown value '{}'", name);
            ReturnValue(ctx, base, ctx.r3.u32, number ? kGfxNumber : kGfxNull, number.value_or(0.0));
            return;
        }
    }
    __imp__sub_825B5410(ctx, base);
}
