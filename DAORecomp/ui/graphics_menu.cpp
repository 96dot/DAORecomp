// The host side of the Options menu's Graphics tab (graphics_tab.as): the settings
// it shows, read and changed through the engine's Flash callbacks, applied at once
// where they can be, and saved to config.toml when the menu commits its options
// (or Manage mods closes the game for the launcher). Leaving the menu without
// committing keeps the live changes for this session; a later commit saves them.
#include "stdafx.h"
#include "ui/graphics_menu.h"
#include "gpu/video.h"
#include "gpu/xenia_host.h"
#include "kernel/function.h"
#include "kernel/vfs.h"
#include "program.h"
#include "settings.h"

#include <bitset>
#include <cmath>

#include "xenia/base/cvar.h"

DECLARE_double(dao_temporal_sharpness);
DECLARE_int32(anisotropic_override);
DECLARE_double(present_hdr_paper_white);
DECLARE_double(present_hdr_peak);
DECLARE_int32(dao_motion_blur);
DECLARE_bool(dao_present_vsync);

namespace dao::ui
{
    namespace
    {
        // The settings of the tab, by the names the script uses.
        enum class Key
        {
            Fullscreen,
            ResolutionScale,
            FrameRate,
            VSync,
            TemporalUpscaler,
            TemporalSharpness,
            Upscaler,
            Sharpness,
            AnisotropicFiltering,
            Shadows,
            PostProcessing,
            MotionBlur,
            Hdr,
            HdrPaperWhite,
            HdrPeak,
            HdrQuality,
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
            { "FrameRate", false },
            { "VSync", false },
            { "TemporalUpscaler", true },
            { "TemporalSharpness", false },
            { "Upscaler", false },
            { "Sharpness", false },
            { "AnisotropicFiltering", false },
            { "Shadows", false },
            { "PostProcessing", false },
            { "MotionBlur", false },
            { "Hdr", true },
            { "HdrPaperWhite", false },
            { "HdrPeak", false },
            { "HdrQuality", true },
        };
        static_assert(std::size(kKeys) == size_t(Key::Count));

        constexpr std::string_view kPrefix = "DAORecomp.";
        constexpr std::string_view kPendingPrefix = "Pending.";
        // The slider steps of the sharpness settings (0 to 10).
        constexpr double kSliderSteps = 10.0;

        // The engine's switches for the PC version's EnableShadows and
        // EnableFrameBufferEffects options, which its options code (82274168;
        // 82293E88 in Title Update 6) sets from those options: the shadows bit of
        // the big-endian render flags word at 83211E54 (832E238C; in its last
        // byte) and a byte.
        constexpr kernel::GameAddress kRenderFlagsLowByte{ 0x83211E57, 0x832E238F };
        constexpr uint8_t kRenderFlagShadows = 2;
        constexpr kernel::GameAddress kFrameBufferEffects{ 0x83211E0B, 0x832E23D4 };
        // The options object (a pointer) and its two options (bytes, at the same
        // offsets in both builds).
        constexpr kernel::GameAddress kOptions{ 0x83211BB8, 0x832E20F0 };
        constexpr uint32_t kOptionEnableShadows = 0x510;
        constexpr uint32_t kOptionEnableFrameBufferEffects = 0x520;

        // GFx value types (the Flash callbacks' arguments and results).
        constexpr uint32_t kGfxNull = 1;
        constexpr uint32_t kGfxNumber = 3;
        constexpr uint32_t kGfxString = 4;

        // The settings, set by InitializeGraphicsMenu and the menu.
        std::atomic<bool> s_shadows{ true };
        std::atomic<bool> s_postProcessing{ true };

        // The longest guest string read, and how many unexpected ones are logged
        // (any movie, a mod's too, can send them).
        constexpr size_t kMaxGuestString = 256;
        constexpr int kMaxWarnings = 16;
        std::atomic<int> s_warnings;

        template <typename... Args>
        void Warn(fmt::format_string<Args...> format, Args&&... args)
        {
            if (s_warnings.fetch_add(1, std::memory_order_relaxed) < kMaxWarnings)
                LOGW("Graphics menu: {}", fmt::format(format, std::forward<Args>(args)...));
        }

        // The menu's view of the settings, and the keys it changed since the last save.
        std::mutex s_menuMutex;
        std::optional<Settings> s_menu;
        std::bitset<size_t(Key::Count)> s_dirty;

        Settings& Menu()
        {
            if (!s_menu)
                s_menu = GetSettings();
            // F10 toggles it too.
            s_menu->frameRate = gpu::FrameRate();
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
            case Key::FrameRate: return s.frameRate;
            case Key::VSync: return s.vsync;
            case Key::TemporalUpscaler: return int(s.temporalUpscaler);
            case Key::TemporalSharpness: return std::round(s.temporalSharpness * kSliderSteps);
            case Key::Upscaler: return int(s.upscaler);
            case Key::Sharpness: return std::round(SharpnessSteps(s));
            case Key::AnisotropicFiltering: return s.anisotropicFiltering;
            case Key::Shadows: return s.shadows;
            case Key::PostProcessing: return s.postProcessing;
            case Key::MotionBlur: return s.motionBlur;
            case Key::Hdr: return s.hdr;
            case Key::HdrPaperWhite: return s.hdrPaperWhite;
            case Key::HdrPeak: return s.hdrPeak;
            case Key::HdrQuality: return s.hdrHighQuality;
            case Key::Count: break;
            }
            return 0.0;
        }

        // Sets a value from the script, within the limits ClampSettings keeps.
        void SetValue(Settings& s, Key key, double value)
        {
            const int whole = int(std::lround(std::clamp(value, -1e6, 1e6)));
            switch (key)
            {
            case Key::Fullscreen: s.fullscreen = whole != 0; break;
            case Key::ResolutionScale: s.resolutionScale = whole; break;
            case Key::FrameRate: s.frameRate = whole; break;
            case Key::VSync: s.vsync = whole != 0; break;
            case Key::TemporalUpscaler:
                s.temporalUpscaler = TemporalUpscaler(std::clamp(whole, 0, int(TemporalUpscaler::Fsr3)));
                break;
            case Key::TemporalSharpness:
                s.temporalSharpness = float(value / kSliderSteps);
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
            case Key::AnisotropicFiltering: s.anisotropicFiltering = whole; break;
            case Key::Shadows: s.shadows = whole != 0; break;
            case Key::PostProcessing: s.postProcessing = whole != 0; break;
            case Key::MotionBlur: s.motionBlur = whole; break;
            case Key::Hdr: s.hdr = whole != 0; break;
            case Key::HdrPaperWhite: s.hdrPaperWhite = float(value); break;
            case Key::HdrPeak: s.hdrPeak = float(value); break;
            case Key::HdrQuality: s.hdrHighQuality = whole != 0; break;
            case Key::Count: break;
            }
            ClampSettings(s);
        }

        // Whether the fields behind a key are equal (the upscaler and its sharpness
        // go together).
        bool SameKey(const Settings& a, const Settings& b, Key key)
        {
            if (key == Key::Upscaler || key == Key::Sharpness)
                return a.upscaler == b.upscaler && a.sharpness == b.sharpness;
            return Value(a, key) == Value(b, key);
        }

        // Copies the fields behind a key (the upscaler and its sharpness go together).
        void CopyKey(Settings& to, const Settings& from, Key key)
        {
            switch (key)
            {
            case Key::Fullscreen: to.fullscreen = from.fullscreen; break;
            case Key::ResolutionScale: to.resolutionScale = from.resolutionScale; break;
            case Key::FrameRate: to.frameRate = from.frameRate; break;
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
            case Key::MotionBlur: to.motionBlur = from.motionBlur; break;
            case Key::Hdr: to.hdr = from.hdr; break;
            case Key::HdrPaperWhite: to.hdrPaperWhite = from.hdrPaperWhite; break;
            case Key::HdrPeak: to.hdrPeak = from.hdrPeak; break;
            case Key::HdrQuality: to.hdrHighQuality = from.hdrHighQuality; break;
            case Key::Count: break;
            }
        }

        // Applies a changed setting now; restart-only ones wait for the next start.
        void ApplyLive(const Settings& s, Key key)
        {
            switch (key)
            {
            case Key::Fullscreen: gpu::XeniaSetFullscreen(s.fullscreen); break;
            case Key::FrameRate: gpu::SetFrameRate(s.frameRate); break;
            case Key::VSync: cvars::dao_present_vsync = s.vsync; break;
            case Key::TemporalSharpness: cvars::dao_temporal_sharpness = s.temporalSharpness; break;
            case Key::Upscaler:
            case Key::Sharpness: gpu::XeniaSetOutputScaling(s.upscaler, s.sharpness); break;
            case Key::AnisotropicFiltering:
                cvars::anisotropic_override = XeniaAnisotropicLevel(s.anisotropicFiltering);
                break;
            case Key::Shadows: s_shadows = s.shadows; break;
            case Key::PostProcessing: s_postProcessing = s.postProcessing; break;
            case Key::MotionBlur: cvars::dao_motion_blur = s.motionBlur; break;
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
            const double before = Value(s, key);
            const Upscaler upscalerBefore = s.upscaler;
            const float sharpnessBefore = s.sharpness;
            const float peakBefore = s.hdrPeak;
            SetValue(s, key, value);
            // The peak stays at least the paper white.
            if (key != Key::HdrPeak && s.hdrPeak != peakBefore)
            {
                s_dirty.set(size_t(Key::HdrPeak));
                ApplyLive(s, Key::HdrPeak);
            }
            if (Value(s, key) == before && s.upscaler == upscalerBefore && s.sharpness == sharpnessBefore)
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
                if (key == Key::Fullscreen || SameKey(s, defaults, key))
                    continue;
                CopyKey(s, defaults, key);
                s_dirty.set(i);
                ApplyLive(s, key);
            }
            LOGI("Graphics menu: defaults restored");
        }

        // Saves the changed settings over what config.toml holds now (keeping the
        // rest of the file, and not saving environment overrides of this run). A file
        // that can't be parsed is left for the player to fix.
        void SaveMenuSettings()
        {
            std::lock_guard lock(s_menuMutex);
            if (s_dirty.none())
                return;
            const auto& root = kernel::VfsUserRoot();
            std::optional<Settings> file = TryReadSettings(root);
            if (!file)
            {
                LOGE("Graphics menu: config.toml can't be read; settings not saved");
                return;
            }
            const Settings& menu = Menu();
            for (size_t i = 0; i < size_t(Key::Count); ++i)
            {
                if (s_dirty.test(i))
                    CopyKey(*file, menu, Key(i));
            }
            SaveSettings(root, *file);
            s_dirty.reset();
            LOGI("Graphics menu: settings saved");
        }

        // Starts the launcher on its Mods page (this game folder's) and ends the
        // game, as closing its window does. Returns only if the launcher can't be
        // started. Logs first: the launcher starts a new dao.log.
        void OpenMods()
        {
            SaveMenuSettings();
            const std::filesystem::path program = ProgramPath();
            if (program.empty())
            {
                Warn("can't find DAORecomp.exe (error {})", GetLastError());
                return;
            }
            LOGI("Graphics menu: closing for the launcher's Mods page");
            if (!StartProgram(program, L"--mods " + QuoteArgument(kernel::VfsGameRoot())))
            {
                Warn("can't start the launcher (error {})", GetLastError());
                return;
            }
            TerminateProcess(GetCurrentProcess(), 0);
        }

        // "Set <key> <value>", "RestoreDefaults" or "OpenMods", after "DAORecomp.".
        void HandleMessage(std::string_view message)
        {
            if (message == "RestoreDefaults")
            {
                RestoreDefaults();
                return;
            }
            if (message == "OpenMods")
            {
                OpenMods();
                return;
            }
            constexpr std::string_view kSet = "Set ";
            if (!message.starts_with(kSet))
            {
                Warn("unknown message '{}'", message);
                return;
            }
            message.remove_prefix(kSet.size());
            const size_t space = message.find(' ');
            const auto key = FindKey(message.substr(0, space));
            if (!key || space == std::string_view::npos)
            {
                Warn("bad message 'Set {}'", message);
                return;
            }
            const std::string text(message.substr(space + 1));
            char* end = nullptr;
            const double value = strtod(text.c_str(), &end);
            if (end == text.c_str() || !std::isfinite(value))
            {
                Warn("bad value 'Set {}'", message);
                return;
            }
            SetMenuValue(*key, value);
        }

        // A NUL-terminated guest string the engine passed (at most kMaxGuestString
        // characters).
        std::string_view GuestString(uint8_t* base, uint32_t address)
        {
            if (!address)
                return {};
            const char* text = reinterpret_cast<const char*>(base + address);
            return std::string_view(text, strnlen(text, kMaxGuestString));
        }

        // The engine's Flash callbacks: the movie (as a ref-counted pointer) a
        // callback returns its value to, f(&slot, handler).
        constexpr kernel::GameAddress kCallbackMovie{ 0x82282DE0, 0x822A3BF8 };

        // Returns a value to the Flash call being handled, as the engine's own
        // callbacks do (GetNumber, 825B5410): the movie from kCallbackMovie(&slot,
        // handler), its vtable+0xA0 given the value, then released.
        void ReturnValue(PPCContext& ctx, uint8_t* base, uint32_t handler, uint32_t type, double number)
        {
            // A 0x80-byte frame below the stack pointer (the callees' frames below
            // it): the back chain at +0, the movie slot at +0x50, the GFx value
            // (type, 0, the double) at +0x60.
            const uint32_t stack = ctx.r1.u32;
            const uint32_t frame = (stack - 0x80) & ~0xFu;
            const uint32_t slot = frame + 0x50;
            const uint32_t value = frame + 0x60;
            PPC_STORE_U32(frame, stack);
            ctx.r1.u64 = frame;

            ctx.r3.u64 = slot;
            ctx.r4.u64 = handler;
            kernel::g_memory.Call(kCallbackMovie.Get(), ctx);
            const uint32_t movie = PPC_LOAD_U32(slot);
            if (movie)
            {
                PPC_STORE_U32(value, type);
                PPC_STORE_U32(value + 4, 0);
                PPC_STORE_U64(value + 8, std::bit_cast<uint64_t>(number));
                ctx.r3.u64 = movie;
                ctx.r4.u64 = value;
                kernel::g_memory.Call(PPC_LOAD_U32(PPC_LOAD_U32(movie) + 0xA0), ctx);
                ctx.r3.u64 = movie;
                ctx.r4.u64 = 0;
                kernel::g_memory.Call(PPC_LOAD_U32(PPC_LOAD_U32(movie + 8) + 4), ctx);
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
        // Whether a switch is forced off (render thread only).
        static bool s_shadowsForced = false;
        static bool s_effectsForced = false;

        const bool shadows = s_shadows.load(std::memory_order_relaxed);
        const bool effects = s_postProcessing.load(std::memory_order_relaxed);
        if (shadows && effects && !s_shadowsForced && !s_effectsForced)
            return;

        // Switched back on: the engine's own option, as its options code applies it.
        const uint32_t options = PPC_LOAD_U32(kOptions.Get());
        auto option = [&](uint32_t offset) { return !options || PPC_LOAD_U8(options + offset) != 0; };

        // The flags' other bits are the engine's, changed as it renders: only this
        // one changes, atomically.
        std::atomic_ref<uint8_t> flags(base[kRenderFlagsLowByte.Get()]);
        if (!shadows)
        {
            if (flags.load(std::memory_order_relaxed) & kRenderFlagShadows)
                flags.fetch_and(uint8_t(~kRenderFlagShadows));
            if (!s_shadowsForced)
                LOGI("Engine graphics: dynamic shadows off");
            s_shadowsForced = true;
        }
        else if (s_shadowsForced)
        {
            if (option(kOptionEnableShadows))
                flags.fetch_or(kRenderFlagShadows);
            s_shadowsForced = false;
            LOGI("Engine graphics: dynamic shadows on");
        }

        if (!effects)
        {
            if (PPC_LOAD_U8(kFrameBufferEffects.Get()))
                PPC_STORE_U8(kFrameBufferEffects.Get(), 0);
            if (!s_effectsForced)
                LOGI("Engine graphics: post-processing off");
            s_effectsForced = true;
        }
        else if (s_effectsForced)
        {
            PPC_STORE_U8(kFrameBufferEffects.Get(), uint8_t(option(kOptionEnableFrameBufferEffects)));
            s_effectsForced = false;
            LOGI("Engine graphics: post-processing on");
        }
    }
}

// FSCommand SendMessage (ExternalCommands.SendMessage): r4 = the message. The
// tab's own messages stop here; the menu's commit also saves its settings.
static void SendMessageHook(PPCContext& ctx, uint8_t* base, PPCFunc* original)
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
    original(ctx, base);
}
GAME_HOOK(sub_825AF710, tu6_sub_825E47B0, SendMessageHook)

// ExternalInterface GetNumber: r3 = the handler, r4 = the arguments (GFx values:
// type at +0, data at +8), r5 = their count. Answers "DAORecomp.<name>".
static void GetNumberHook(PPCContext& ctx, uint8_t* base, PPCFunc* original)
{
    using namespace dao::ui;
    if (ctx.r5.u32 == 1 && PPC_LOAD_U32(ctx.r4.u32) == kGfxString)
    {
        const std::string_view name = GuestString(base, PPC_LOAD_U32(ctx.r4.u32 + 8));
        if (name.starts_with(kPrefix))
        {
            const auto number = MenuNumber(name.substr(kPrefix.size()));
            if (!number)
                Warn("unknown value '{}'", name);
            ReturnValue(ctx, base, ctx.r3.u32, number ? kGfxNumber : kGfxNull, number.value_or(0.0));
            return;
        }
    }
    original(ctx, base);
}
GAME_HOOK(sub_825B5410, tu6_sub_825E8C28, GetNumberHook)
