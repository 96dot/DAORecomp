#include "stdafx.h"
#include "launcher/launcher_internal.h"

#include "patches/patches.h"
#include "version.h"

namespace dao::launcher
{
    namespace
    {
        struct WindowSize
        {
            int width, height;
        };
        constexpr WindowSize kWindowSizes[] = {
            { 1280, 720 }, { 1280, 800 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 },
        };

        constexpr const char* kScales[] = { "1x - 1280x720", "2x - 2560x1440", "3x - 3840x2160 (4K)",
            "4x - 5120x2880" };
        constexpr const char* kUpscalers[] = { "Bilinear", "FidelityFX CAS", "FidelityFX FSR 1" };
        constexpr const char* kTemporalUpscalers[] = { "Off", "Auto (FSR 4 where supported, else FSR 3.1)",
            "FidelityFX FSR 4", "FidelityFX FSR 3.1" };
        // The names of kAnisotropicLevels.
        constexpr const char* kAnisotropicNames[] = { "Game default", "2x", "4x", "8x", "16x" };
        static_assert(std::size(kAnisotropicNames) == std::size(kAnisotropicLevels));
        // The names of kFrameRates.
        constexpr const char* kFrameRateNames[] = { "30 fps (original)", "60 fps", "90 fps", "120 fps", "144 fps",
            "165 fps", "240 fps", "Unlimited" };
        static_assert(std::size(kFrameRateNames) == std::size(kFrameRates));
        constexpr const char* kMotionBlurNames[] = { "Off", "Low", "High" };
        static_assert(std::size(kMotionBlurNames) == kMaxMotionBlur + 1);

        // A setting as the game's menus show it: the name above the picker or slider,
        // which `widget` adds as "##value", returning whether it changed.
        template <typename Widget>
        bool Row(const char* label, Widget&& widget)
        {
            Label(label);
            ImGui::PushID(label);
            ImGui::SetNextItemWidth(RowWidth());
            const bool changed = widget();
            ImGui::PopID();
            return changed;
        }

        bool ComboRow(const char* label, int* value, const char* const* items, int count)
        {
            return Row(label, [&] { return ImGui::Combo("##value", value, items, count); });
        }

        bool SliderRow(const char* label, float* value, float min, float max, const char* format)
        {
            return Row(label, [&] { return ImGui::SliderFloat("##value", value, min, max, format); });
        }

        // A field of view: a slider over kFovSteps (a value between them, from
        // config.toml, starts at the closest).
        bool FovRow(const char* label, int* value)
        {
            int step = 0;
            for (int i = 1; i < int(std::size(kFovSteps)); ++i)
            {
                if (std::abs(kFovSteps[i] - *value) < std::abs(kFovSteps[step] - *value))
                    step = i;
            }
            const std::string text = kFovSteps[step] == kOriginalFov ? fmt::format("{}\u00B0 (original)", kOriginalFov)
                                                                     : fmt::format("{}\u00B0", kFovSteps[step]);
            const bool changed = Row(label, [&] {
                return ImGui::SliderInt("##value", &step, 0, int(std::size(kFovSteps)) - 1, text.c_str(),
                    ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput);
            });
            if (changed)
                *value = kFovSteps[step];
            return changed;
        }

        bool PatchOn(const Settings& settings, const patches::Patch& patch)
        {
            const auto it = settings.patches.find(patch.id);
            return it != settings.patches.end() ? it->second : patch.defaultEnabled;
        }

        void GraphicsTab(State& st)
        {
            Settings& s = st.settings;

            Section("Resolution");
            int scale = std::clamp(s.resolutionScale, 1, int(std::size(kScales))) - 1;
            if (ComboRow("Rendering resolution", &scale, kScales, int(std::size(kScales))))
                s.resolutionScale = scale + 1;
            HelpText("The game renders at 1280x720; higher scales render every pass at that multiple. On a "
                     "Steam Deck, keep 1x.");

            Section("Anti-aliasing and scaling");
            int temporal = int(s.temporalUpscaler);
            if (ComboRow("Temporal upscaling", &temporal, kTemporalUpscalers, int(std::size(kTemporalUpscalers))))
                s.temporalUpscaler = TemporalUpscaler(temporal);
            ImGui::BeginDisabled(s.temporalUpscaler == TemporalUpscaler::Off);
            SliderRow("Temporal sharpening", &s.temporalSharpness, 0.0f, 1.0f, "%.2f");
            ImGui::EndDisabled();
            HelpText("Upscales and anti-aliases the 3D scenes to the window size using motion vectors; menus and "
                     "the UI use the upscaler below. In a window no larger than the rendering resolution, it only "
                     "anti-aliases. Off is fastest on a Steam Deck.");
            ImGui::Spacing();
            ImGui::BeginDisabled(s.temporalUpscaler == TemporalUpscaler::Off);
            ImGui::Checkbox("Frame generation (FSR 3.1)", &s.frameGeneration);
            ImGui::EndDisabled();
            HelpText("Generates a frame between each two of the 3D scenes: twice the frames on the display, for "
                     "half a frame more latency. Best with the frame rate at half the display's refresh rate "
                     "(60 fps on a 120 Hz display); when the display can't show twice the frame rate, only the "
                     "real frames are shown.");
            ImGui::Spacing();
            int upscaler = int(s.upscaler);
            if (ComboRow("Upscaler", &upscaler, kUpscalers, int(std::size(kUpscalers))))
                s.upscaler = Upscaler(upscaler);
            if (s.upscaler == Upscaler::Fsr)
                SliderRow("Sharpness reduction", &s.sharpness, 0.0f, 2.0f, "%.2f stops");
            else if (s.upscaler == Upscaler::Cas)
                SliderRow("Extra sharpness", &s.sharpness, 0.0f, 1.0f, "%.2f");
            HelpText("Used when the window size differs from the rendering resolution.");

            Section("Textures");
            int anisotropic = int(std::find(std::begin(kAnisotropicLevels), std::end(kAnisotropicLevels),
                                      s.anisotropicFiltering) -
                std::begin(kAnisotropicLevels));
            if (anisotropic >= int(std::size(kAnisotropicLevels)))
                anisotropic = 0;
            if (ComboRow("Anisotropic filtering", &anisotropic, kAnisotropicNames, int(std::size(kAnisotropicNames))))
                s.anisotropicFiltering = kAnisotropicLevels[anisotropic];
            HelpText("Sharper textures on surfaces seen at an angle, such as floors and roads. The game's default "
                     "is the fastest.");

            Section("Effects");
            ImGui::Checkbox("Dynamic shadows", &s.shadows);
            ImGui::Checkbox("Post-processing effects", &s.postProcessing);
            HelpText("Turning these off makes the game faster on low-power devices such as the Steam Deck.");
            ImGui::Spacing();
            ComboRow("Motion blur", &s.motionBlur, kMotionBlurNames, int(std::size(kMotionBlurNames)));
            HelpText("Blurs what moves (characters, objects, the camera). Needs temporal upscaling; costs GPU "
                     "time while things move, more at High.");

            Section("Camera");
            FovRow("Field of view", &s.fov);
            FovRow("Field of view in conversations", &s.conversationFov);
            HelpText("Degrees across at 16:9; 91 is the game's own. In conversations and cutscenes, each shot's "
                     "own angle is widened or narrowed in the same proportion.");
            ImGui::Spacing();
            HelpText("Most of these settings can also be changed in the game, under Options > Graphics.");
        }

        void DisplayTab(State& st)
        {
            Settings& s = st.settings;

            Section("Window");
            ImGui::Checkbox("Fullscreen", &s.fullscreen);
            ImGui::BeginDisabled(s.fullscreen);
            Row("Window size", [&] {
                const std::string current = fmt::format("{}x{}", s.windowWidth, s.windowHeight);
                if (!ImGui::BeginCombo("##value", current.c_str()))
                    return false;
                bool changed = false;
                for (const auto& size : kWindowSizes)
                {
                    const std::string label = fmt::format("{}x{}", size.width, size.height);
                    const bool selected = size.width == s.windowWidth && size.height == s.windowHeight;
                    if (ImGui::Selectable(label.c_str(), selected))
                    {
                        s.windowWidth = size.width;
                        s.windowHeight = size.height;
                        changed = true;
                    }
                }
                ImGui::EndCombo();
                return changed;
            });
            ImGui::EndDisabled();

            Section("Frame rate");
            // (ClampSettings keeps it one of them.)
            int frameRate = int(std::ranges::find(kFrameRates, s.frameRate) - std::begin(kFrameRates));
            if (ComboRow("Frame rate", &frameRate, kFrameRateNames, int(std::size(kFrameRateNames))))
                s.frameRate = kFrameRates[frameRate];
            HelpText("The game locks its 3D scenes to 30 fps. Above 60 is experimental.");
            ImGui::Checkbox("VSync", &s.vsync);
            HelpText("Waits for the display's refresh: no tearing, and no more frames shown than the display "
                     "refreshes.");

            Section("HDR (experimental)");
            ImGui::Checkbox("HDR output", &s.hdr);
            ImGui::BeginDisabled(!s.hdr);
            SliderRow("Paper white", &s.hdrPaperWhite, kHdrMinNits, 400.0f, "%.0f nits");
            SliderRow("Peak brightness", &s.hdrPeak, s.hdrPaperWhite, 4000.0f, "%.0f nits");
            ImGui::Checkbox("HDR quality: High (16-bit scRGB)", &s.hdrHighQuality);
            ImGui::EndDisabled();
            HelpText("On an HDR display, 3D scenes are rendered in HDR, their highlights up to the "
                     "peak brightness; menus are shown at paper white. HDR quality High keeps that HDR "
                     "in 16 bits from the temporal upscaler to the display: smoother gradients, with "
                     "more GPU memory traffic (no frame rate change measured). Takes effect on restart.");
        }

        void PatchesTab(State& st)
        {
            Section("Patches");
            HelpText("Built-in changes to the game's behaviour.");
            ImGui::Spacing();
            for (const auto& patch : patches::All())
            {
                bool on = PatchOn(st.settings, patch);
                const std::string label(patch.name);
                if (ImGui::Checkbox(label.c_str(), &on))
                    st.settings.patches[std::string(patch.id)] = on;
                ImGui::Indent();
                HelpText(std::string(patch.description).c_str());
                ImGui::Unindent();
                ImGui::Spacing();
            }
        }

        void AdvancedTab(State& st)
        {
            Section("Files");
            if (ImGui::Button("Open user data folder"))
                OpenFolder(st.userRoot);
            HelpText("Saves, the configuration and the logs.");
            ImGui::Spacing();
            if (ImGui::Button("Open config.toml"))
                OpenFile(st.userRoot / "config.toml");
            HelpText("Extra Xenia GPU options go in the [xenia] section. Settings are saved when you press Play "
                     "or close the launcher.");

            Section("Steam");
            ImGui::BeginDisabled(st.steamPath.empty() || st.steamStep != SteamStep::None);
            if (ImGui::Button("Add to Steam"))
                st.steamStep = SteamStep::Start;
            ImGui::EndDisabled();
            HelpText(st.steamPath.empty()
                    ? "Steam isn't installed. (Under Proton or Wine, add the game from Steam itself.)"
                    : "Adds the game to your Steam library, with artwork from its menus. Started from Steam (Big "
                      "Picture too), it goes straight into the game. Steam has to close for it: it asks first, and "
                      "starts Steam again after.");

            Section("Updates");
            Label(fmt::format("Version {}", Version()).c_str());
            if (!Updater::IsRelease())
            {
                HelpText("A development build: it's updated by building the source, not from the releases.");
                return;
            }
            ImGui::Checkbox("Check for updates when the launcher opens", &st.settings.checkForUpdates);
            Updater& updater = *st.updater;
            if (const Release* release = FoundRelease(st))
            {
                ImGui::Text("%s is available.", release->tag.c_str());
                ImGui::SameLine();
                if (ImGui::Button("See the update"))
                    SetView(st, View::Update);
            }
            else
            {
                const Updater::State state = updater.GetState();
                ImGui::BeginDisabled(state == Updater::State::Checking);
                if (ImGui::Button("Check now"))
                    updater.Check();
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (state == Updater::State::Checking)
                    ImGui::TextUnformatted("Checking...");
                else if (state == Updater::State::UpToDate)
                    ImGui::TextUnformatted("Up to date.");
                else if (state == Updater::State::Failed)
                    ImGui::Text("Can't check: %s.", updater.GetError().c_str());
            }
            HelpText("Asks github.com/96dot/DAORecomp for its list of releases; the request carries only this "
                     "version number.");
        }

        // Each puts a tab's settings back to their defaults.
        void RestoreGraphics(Settings& s)
        {
            const Settings defaults;
            s.resolutionScale = defaults.resolutionScale;
            s.temporalUpscaler = defaults.temporalUpscaler;
            s.temporalSharpness = defaults.temporalSharpness;
            s.frameGeneration = defaults.frameGeneration;
            s.upscaler = defaults.upscaler;
            s.sharpness = defaults.sharpness;
            s.anisotropicFiltering = defaults.anisotropicFiltering;
            s.shadows = defaults.shadows;
            s.postProcessing = defaults.postProcessing;
            s.motionBlur = defaults.motionBlur;
            s.fov = defaults.fov;
            s.conversationFov = defaults.conversationFov;
        }

        void RestoreDisplay(Settings& s)
        {
            const Settings defaults;
            s.fullscreen = defaults.fullscreen;
            s.windowWidth = defaults.windowWidth;
            s.windowHeight = defaults.windowHeight;
            s.vsync = defaults.vsync;
            s.frameRate = defaults.frameRate;
            s.hdr = defaults.hdr;
            s.hdrPaperWhite = defaults.hdrPaperWhite;
            s.hdrPeak = defaults.hdrPeak;
            s.hdrHighQuality = defaults.hdrHighQuality;
        }

        struct Tab
        {
            const char* name;
            Contents contents;
            // Restore Defaults, if the tab has it.
            Restore restore;
        };
        constexpr Tab kTabs[] = {
            { "Graphics", GraphicsTab, RestoreGraphics },
            { "Display", DisplayTab, RestoreDisplay },
            { "Patches", PatchesTab, nullptr },
            { "Advanced", AdvancedTab, nullptr },
        };
        static_assert(std::size(kTabs) == kConfigureTabs);
    }

    void ConfigureView(State& st, const Layout& layout)
    {
        const int previous = st.configureTab;
        PanelFrame(st, layout, "Configure", kTabs[st.configureTab].restore);

        // The tabs on the left, as the Options screen's; LB and RB also switch them.
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float rowHeight = kTabStep * layout.fit;
        for (int i = 0; i < kConfigureTabs; i++)
        {
            const float top = kTabTop + kTabStep * i, center = top + kTabBarHalfHeight;
            const ImVec2 rowMin(layout.pos.x, layout.Y(center) - rowHeight * 0.5f);
            const bool selected = i == st.configureTab;
            bool hot = false;
            ImGui::PushID(i);
            const ImVec2 rowSize(layout.Left(kTabBarRight, 0).x - rowMin.x, rowHeight);
            if (MenuButton("##tab", rowMin, rowSize, true, hot))
                st.configureTab = i;
            if ((st.viewChanged || st.focusConfigureTab) && selected)
            {
                ImGui::SetKeyboardFocusHere(-1);
                st.focusConfigureTab = false;
            }
            const float shown = Ease(ImGui::GetItemID(), hot || selected);
            ImGui::PopID();
            if (const Texture* bar = Art(st, kTabBar); bar && shown > 0.0f)
                DrawSprite(draw, *bar, layout.Left(kTabBarRight, top), layout.fit,
                    shown * (selected ? 1.0f : 0.5f) * st.fade, true);
            const float size = kTabTextSize * layout.fit;
            ShadowText(draw, st, st.menuFont, size,
                ImVec2(layout.Left(kTabTextX, 0).x, layout.Y(center) - size * 0.5f),
                selected || hot ? kText : kTextDim, kTabs[i].name);
        }
        // As Back: not while a picker is open or a slider held.
        // (The focus goes to the new tab next frame: the old one's items are gone.)
        if (st.backKeysFree && ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false))
        {
            st.configureTab = (st.configureTab + kConfigureTabs - 1) % kConfigureTabs;
            st.focusConfigureTab = true;
        }
        if (st.backKeysFree && ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false))
        {
            st.configureTab = (st.configureTab + 1) % kConfigureTabs;
            st.focusConfigureTab = true;
        }
        // LB and RB under the tabs.
        const float bumper = kBumperSize * layout.fit;
        const ImVec2 bumpers = layout.Left(kTabTextX, kTabTop + kTabStep * kConfigureTabs + kBumperGap);
        DrawPadButton(draw, st, PadButton::LB, bumpers, bumper, kBumperOpacity * st.fade);
        DrawPadButton(draw, st, PadButton::RB, ImVec2(bumpers.x + kBumperStep * layout.fit, bumpers.y), bumper,
            kBumperOpacity * st.fade);

        ShowContents(st, layout, kConfigureLeft, kTabs[st.configureTab].contents, st.configureTab != previous);
    }
}
