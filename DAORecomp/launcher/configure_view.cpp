#include "stdafx.h"
#include "launcher/launcher_internal.h"

#include "patches/patches.h"
#include "program.h"
#include "text.h"
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
        // In TemporalUpscaler's order.
        constexpr const char* kTemporalUpscalers[] = { "Off", "Auto (FSR 4 where supported, else FSR 3.1)",
            "FidelityFX FSR 4", "FidelityFX FSR 3.1", "Intel XeSS-SR" };
        static_assert(std::size(kTemporalUpscalers) == size_t(TemporalUpscaler::Xess) + 1);
        // The names of kAnisotropicLevels.
        constexpr const char* kAnisotropicNames[] = { "Game default", "2x", "4x", "8x", "16x" };
        static_assert(std::size(kAnisotropicNames) == std::size(kAnisotropicLevels));
        // The names of kFrameRates.
        constexpr const char* kFrameRateNames[] = { "30 fps (original)", "60 fps", "90 fps", "120 fps", "144 fps",
            "165 fps", "240 fps", "Unlimited" };
        static_assert(std::size(kFrameRateNames) == std::size(kFrameRates));
        constexpr const char* kMotionBlurNames[] = { "Off", "Low", "High" };
        static_assert(std::size(kMotionBlurNames) == kMaxMotionBlur + 1);
        constexpr const char* kLowLatencyNames[] = { "Off", "On", "On + Boost" };
        static_assert(std::size(kLowLatencyNames) == kMaxLowLatency + 1);
        // The names of kNeuralNetworks, and of the passes and styles.
        constexpr const char* kNeuralNetworkNames[] = { "Auto (720 at 1x, 1080 above)",
            "Quality (1080-line network)", "Balanced (900-line network)", "Performance (720-line network)" };
        static_assert(std::size(kNeuralNetworkNames) == std::size(kNeuralNetworks));
        constexpr const char* kNeuralPassNames[] = { "1x", "2x (stronger)", "3x (strongest)" };
        static_assert(std::size(kNeuralPassNames) == kMaxNeuralPasses);
        constexpr const char* kNeuralStyleNames[] = { "0 (NVIDIA's default)", "1 (lmxxf's default)",
            "2 (NVIDIA's third)" };
        static_assert(std::size(kNeuralStyleNames) == kMaxNeuralStyle + 1);

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
            // (The value applied: one set in config.toml between the steps too.)
            const std::string text = *value == kOriginalFov ? fmt::format("{}\u00B0 (original)", kOriginalFov)
                                                            : fmt::format("{}\u00B0", *value);
            const bool changed = Row(label, [&] {
                return ImGui::SliderInt("##value", &step, 0, int(std::size(kFovSteps)) - 1, text.c_str(),
                    ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput);
            });
            if (changed)
                *value = kFovSteps[step];
            return changed;
        }

        // Whether the install has neural rendering's runtime and the kernels the
        // installer compiled for this PC's AMD RDNA 4 GPU.
        bool NeuralInstalled(State& st)
        {
            if (!st.neuralInstalled)
            {
                const fs::path here = ProgramPath().parent_path();
                std::error_code ec;
                st.neuralInstalled = fs::is_regular_file(here / "LmxxfNrRuntime.dll", ec) &&
                    fs::is_regular_file(here / "neural" / "HIP" / "SHA256SUMS", ec);
            }
            return *st.neuralInstalled;
        }

        // Whether settings.neuralWeights is known not to have the weights. Checked when
        // it changes, on a thread of its own: a \\server\ path typed in can take
        // seconds to answer. Not known meanwhile (and a path typed after one that doesn't
        // answer is checked once it has).
        bool WeightsMissing(State& st)
        {
            const std::string& weights = st.settings.neuralWeights;
            auto& check = st.weightsCheck;
            if (check && check->done.load(std::memory_order_acquire))
            {
                st.checkedWeights = check->path;
                st.weightsFound = check->found;
                check.reset();
            }
            if (!check && weights != st.checkedWeights)
            {
                check = std::make_shared<State::WeightsCheck>();
                check->path = weights;
                auto run = [check] {
                    std::error_code ec;
                    check->found = !check->path.empty() &&
                        fs::is_regular_file(FromUtf8(check->path) / kNeuralWeightsFile, ec);
                    check->done.store(true, std::memory_order_release);
                };
                try
                {
                    std::thread(run).detach();
                }
                catch (const std::system_error&)
                {
                    run();
                }
            }
            return weights == st.checkedWeights && !st.weightsFound;
        }

        void NeuralRenderingRows(State& st)
        {
            Settings& s = st.settings;
            const bool installed = NeuralInstalled(st);
            ImGui::BeginDisabled(!installed || s.temporalUpscaler == TemporalUpscaler::Off);
            ImGui::Checkbox("Neural rendering (AMD RDNA 4, experimental)", &s.neuralRendering);
            ImGui::BeginDisabled(!s.neuralRendering);
            PathRow(st, "Neural rendering weights", PathField::NeuralWeights);
            if (s.neuralRendering && WeightsMissing(st))
                ColoredText(kWarning, s.neuralWeights.empty()
                        ? "Choose the folder of lmxxf's package (or its weights folder)."
                        : "No weights (block0-ffn.f16) in this folder: it stays off.");
            SliderRow("Neural rendering strength", &s.neuralStrength, 0.0f, 1.0f, "%.2f");
            int network = int(std::ranges::find(kNeuralNetworks, s.neuralNetwork) - std::begin(kNeuralNetworks));
            if (network >= int(std::size(kNeuralNetworks)))
                network = 0;
            if (ComboRow("Neural rendering quality", &network, kNeuralNetworkNames,
                    int(std::size(kNeuralNetworkNames))))
                s.neuralNetwork = kNeuralNetworks[network];
            int passes = std::clamp(s.neuralPasses, 1, kMaxNeuralPasses) - 1;
            if (ComboRow("Neural rendering passes", &passes, kNeuralPassNames, kMaxNeuralPasses))
                s.neuralPasses = passes + 1;
            ImGui::BeginDisabled(s.neuralPasses < 2);
            ImGui::Checkbox("Protect skin from the extra passes", &s.neuralSkinProtect);
            ImGui::EndDisabled();
            int style = std::clamp(s.neuralStyle, 0, kMaxNeuralStyle);
            if (ComboRow("Neural rendering style", &style, kNeuralStyleNames, kMaxNeuralStyle + 1))
                s.neuralStyle = style;
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            HelpText(installed
                    ? "Runs the 3D scenes through the neural rendering network of NVIDIA's DLSS 5 before "
                      "temporal upscaling, with lmxxf's port of it to AMD RDNA 4 (MIT). Its weights are "
                      "NVIDIA's and don't come with the game: download lmxxf's package from "
                      "github.com/lmxxf/dlss5-on-amd-9070xt-porting and choose its folder. With HDR "
                      "output, it keeps the highlights above paper white. The quality is the network's "
                      "size, which the 3D scenes are fitted to: on an RX 9070 XT about 9.5 ms of GPU time "
                      "a frame at 1080 lines, 7 ms at 900, less at 720 (Auto: 720 at 1x, 1080 above). "
                      "Each pass costs that again and strengthens the look; skin protection keeps skin as "
                      "the first pass made it. The styles are NVIDIA's three (0 is NVIDIA's default, 1 "
                      "lmxxf's). Strength applies at once in the game; the rest on restart."
                    : "Needs an AMD Radeon RX 9000 series GPU (RDNA 4) and the AMD driver on Windows: the "
                      "installer compiles its kernels for it. Install again with that GPU to get it.");
        }

        bool PatchOn(const Settings& settings, const patches::Patch& patch)
        {
            const auto it = settings.patches.find(patch.id);
            return it != settings.patches.end() ? it->second : patch.defaultEnabled;
        }

        void GraphicsTab(State& st)
        {
            TakeDialogChoices(st);
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
            // XeSS doesn't sharpen.
            ImGui::BeginDisabled(
                s.temporalUpscaler == TemporalUpscaler::Off || s.temporalUpscaler == TemporalUpscaler::Xess);
            SliderRow("Temporal sharpening", &s.temporalSharpness, 0.0f, 1.0f, "%.2f");
            ImGui::EndDisabled();
            HelpText("Upscales and anti-aliases the 3D scenes to the window size using motion vectors; menus and "
                     "the UI use the upscaler below. In a window no larger than the rendering resolution, it only "
                     "anti-aliases. XeSS-SR runs on most GPUs of recent years, best on Intel Arc; where it "
                     "can't, FSR is used. XeSS-SR doesn't sharpen: the output sharpening below applies to it. "
                     "Off is fastest on a Steam Deck.");
            ImGui::Spacing();
            ImGui::BeginDisabled(s.temporalUpscaler == TemporalUpscaler::Off);
            ImGui::Checkbox("Frame generation (FSR 3.1)", &s.frameGeneration);
            ImGui::EndDisabled();
            HelpText("Generates a frame between each two of the 3D scenes: twice the frames on the display, for "
                     "half a frame more latency. Best with the frame rate at half the display's refresh rate "
                     "(60 fps on a 120 Hz display); when the display can't show twice the frame rate, only the "
                     "real frames are shown.");
            ImGui::Spacing();
            NeuralRenderingRows(st);
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
            ComboRow("Low latency", &s.lowLatency, kLowLatencyNames, int(std::size(kLowLatencyNames)));
            HelpText("AMD Radeon Anti-Lag 2 (a current AMD driver): each frame starts as late as it can still be on "
                     "time, so the controller is read closer to when the frame is shown. Boost is the same as On "
                     "on AMD GPUs. Off while frame generation is on (it would cost frames).");

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
            // (Not the weights' folder: it's where the player's download is.)
            s.neuralRendering = defaults.neuralRendering;
            s.neuralStrength = defaults.neuralStrength;
            s.neuralNetwork = defaults.neuralNetwork;
            s.neuralPasses = defaults.neuralPasses;
            s.neuralSkinProtect = defaults.neuralSkinProtect;
            s.neuralStyle = defaults.neuralStyle;
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
            s.lowLatency = defaults.lowLatency;
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

    // The weights' folder in `folder` (lmxxf's package or a folder in it): the one
    // with block0-ffn.f16, `folder` or up to kMaxDepth levels down.
    std::optional<fs::path> FindNeuralWeights(const fs::path& folder)
    {
        constexpr int kMaxDepth = 4;
        // (A folder as large as a drive, or a slow network share, isn't searched through:
        // the limits are checked between entries, so one slow listing can still overrun
        // the time. On the folder dialog's thread, with Browse off meanwhile.)
        constexpr int kMaxEntries = 20000;
        constexpr auto kMaxTime = std::chrono::seconds(5);
        std::error_code ec;
        if (fs::is_regular_file(folder / kNeuralWeightsFile, ec))
            return folder;
        const auto deadline = std::chrono::steady_clock::now() + kMaxTime;
        int entries = 0;
        for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec))
        {
            if (++entries > kMaxEntries || std::chrono::steady_clock::now() > deadline)
                break;
            // (Links to folders aren't followed.)
            std::error_code entryEc;
            if (!it->is_directory(entryEc))
                continue;
            if (fs::is_regular_file(it->path() / kNeuralWeightsFile, entryEc))
                return it->path();
            // (Its contents would be a level deeper.)
            if (it.depth() + 1 >= kMaxDepth)
                it.disable_recursion_pending();
        }
        return std::nullopt;
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
