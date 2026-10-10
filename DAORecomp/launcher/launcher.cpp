#include "stdafx.h"
#include "launcher/launcher.h"

#include <shellapi.h>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_sdlrenderer3.h>

#include "mods/mods.h"
#include "patches/patches.h"
#include "settings.h"

namespace dao::launcher
{
    namespace fs = std::filesystem;

    namespace
    {
        struct WindowSize
        {
            int width, height;
        };
        constexpr WindowSize kWindowSizes[] = {
            { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 },
        };
        constexpr const char* kScales[] = { "1x - 1280x720", "2x - 2560x1440", "3x - 3840x2160 (4K)", "4x - 5120x2880" };
        constexpr const char* kUpscalers[] = { "Bilinear", "FidelityFX CAS", "FidelityFX FSR 1" };
        constexpr const char* kTemporalUpscalers[] = { "Off", "Auto (FSR 4 where supported, else FSR 3.1)",
            "FidelityFX FSR 4", "FidelityFX FSR 3.1" };

        struct State
        {
            fs::path gameRoot, userRoot, modsRoot;
            Settings settings;
            std::vector<mods::ModInfo> mods;
            int selectedMod = -1;
            ImFont* titleFont = nullptr;
            bool play = false;
            bool quit = false;
            // DAO_LAUNCHER_SCREENSHOTS=<dir>: show each tab, save it as BMP, quit.
            fs::path screenshotDir;
            int screenshotFrame = 0;
        };

        void OpenFolder(const fs::path& path)
        {
            std::error_code ec;
            fs::create_directories(path, ec);
            ShellExecuteW(nullptr, L"open", path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }

        void OpenFile(const fs::path& path)
        {
            ShellExecuteW(nullptr, L"open", path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }

        void HelpText(const char* text)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", text);
            ImGui::PopStyleColor();
        }

        void Section(const char* title)
        {
            ImGui::Spacing();
            ImGui::SeparatorText(title);
        }

        bool PatchOn(const Settings& settings, const patches::Patch& patch)
        {
            const auto it = settings.patches.find(patch.id);
            return it != settings.patches.end() ? it->second : patch.defaultEnabled;
        }

        void PlayTab(State& st)
        {
            ImGui::Spacing();
            ImGui::PushFont(st.titleFont);
            ImGui::TextUnformatted("Dragon Age: Origins");
            ImGui::PopFont();
            HelpText("Xbox 360 static recompilation");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            const bool gameFound = fs::exists(st.gameRoot / "default.xex");
            ImGui::Text("Game data:  %s", gameFound ? st.gameRoot.string().c_str() : "not found");
            const auto enabledMods = std::count_if(st.mods.begin(), st.mods.end(), [](auto& m) { return m.enabled; });
            ImGui::Text("Mods:  %d of %d enabled", int(enabledMods), int(st.mods.size()));
            int patchesOn = 0;
            for (const auto& patch : patches::All())
                patchesOn += PatchOn(st.settings, patch);
            ImGui::Text("Patches:  %d on", patchesOn);
            ImGui::Text("Rendering:  %dx%d%s%s", 1280 * st.settings.resolutionScale, 720 * st.settings.resolutionScale,
                st.settings.unlockFrameRate ? ", 60 fps" : ", 30 fps",
                st.settings.temporalUpscaler != TemporalUpscaler::Off ? ", temporal FSR" : "");

            ImGui::Spacing();
            ImGui::Spacing();
            ImGui::BeginDisabled(!gameFound);
            if (ImGui::Button("Play", ImVec2(ImGui::GetFontSize() * 12, ImGui::GetFontSize() * 2.2f)) ||
                (gameFound && ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false)))
                st.play = true;
            ImGui::EndDisabled();
            ImGui::SetItemDefaultFocus();

            ImGui::Spacing();
            ImGui::Checkbox("Show this launcher at startup", &st.settings.showLauncher);
            HelpText("When hidden, hold Shift while starting the game to bring it back.");

            ImGui::Spacing();
            if (ImGui::Button("Open user data folder"))
                OpenFolder(st.userRoot);
            ImGui::SameLine();
            if (ImGui::Button("Quit"))
                st.quit = true;
        }

        void SettingsTab(State& st)
        {
            Settings& s = st.settings;
            ImGui::PushItemWidth(ImGui::GetFontSize() * 18);

            Section("Display");
            int scale = std::clamp(s.resolutionScale, 1, int(std::size(kScales))) - 1;
            if (ImGui::Combo("Rendering resolution", &scale, kScales, int(std::size(kScales))))
                s.resolutionScale = scale + 1;
            HelpText("The game renders at 1280x720; higher scales render every pass at that multiple.");

            ImGui::Checkbox("Fullscreen", &s.fullscreen);
            ImGui::BeginDisabled(s.fullscreen);
            const std::string current = fmt::format("{}x{}", s.windowWidth, s.windowHeight);
            if (ImGui::BeginCombo("Window size", current.c_str()))
            {
                for (const auto& size : kWindowSizes)
                {
                    const std::string label = fmt::format("{}x{}", size.width, size.height);
                    const bool selected = size.width == s.windowWidth && size.height == s.windowHeight;
                    if (ImGui::Selectable(label.c_str(), selected))
                    {
                        s.windowWidth = size.width;
                        s.windowHeight = size.height;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            ImGui::Checkbox("VSync", &s.vsync);
            ImGui::Checkbox("60 fps (unlock the 30 fps cap in 3D scenes)", &s.unlockFrameRate);

            Section("Scaling");
            int temporal = int(s.temporalUpscaler);
            if (ImGui::Combo("Temporal upscaling", &temporal, kTemporalUpscalers, int(std::size(kTemporalUpscalers))))
                s.temporalUpscaler = TemporalUpscaler(temporal);
            ImGui::BeginDisabled(s.temporalUpscaler == TemporalUpscaler::Off);
            ImGui::SliderFloat("Temporal sharpening", &s.temporalSharpness, 0.0f, 1.0f, "%.2f");
            ImGui::EndDisabled();
            HelpText("Upscales and anti-aliases the 3D scenes to the window size using motion vectors; menus and "
                     "the UI use the upscaler below. In a window no larger than the rendering resolution, it only "
                     "anti-aliases.");
            ImGui::Spacing();
            int upscaler = int(s.upscaler);
            if (ImGui::Combo("Upscaler", &upscaler, kUpscalers, int(std::size(kUpscalers))))
                s.upscaler = Upscaler(upscaler);
            if (s.upscaler == Upscaler::Fsr)
                ImGui::SliderFloat("Sharpness reduction", &s.sharpness, 0.0f, 2.0f, "%.2f stops");
            else if (s.upscaler == Upscaler::Cas)
                ImGui::SliderFloat("Extra sharpness", &s.sharpness, 0.0f, 1.0f, "%.2f");
            HelpText("Used when the window size differs from the rendering resolution.");

            Section("HDR (experimental)");
            ImGui::Checkbox("HDR output", &s.hdr);
            ImGui::BeginDisabled(!s.hdr);
            ImGui::SliderFloat("Paper white", &s.hdrPaperWhite, 80.0f, 400.0f, "%.0f nits");
            ImGui::SliderFloat("Peak brightness", &s.hdrPeak, s.hdrPaperWhite, 4000.0f, "%.0f nits");
            ImGui::EndDisabled();

            Section("Advanced");
            if (ImGui::Button("Open config.toml"))
                OpenFile(st.userRoot / "config.toml");
            HelpText("Extra Xenia GPU options go in the [xenia] section. Settings are saved when you press Play "
                     "or close the launcher.");
            ImGui::PopItemWidth();
        }

        void ModsTab(State& st)
        {
            HelpText("Each mod is a folder in mods/ that mirrors the game folder, e.g. "
                     "mods/MyMod/packages/core/override/. Mods lower in the list win over mods above them.");
            ImGui::Spacing();

            if (ImGui::Button("Open mods folder"))
                OpenFolder(st.modsRoot);
            ImGui::SameLine();
            if (ImGui::Button("Refresh"))
            {
                st.mods = mods::Scan(st.modsRoot, mods::ToState(st.mods));
                st.selectedMod = -1;
            }
            ImGui::SameLine();
            const bool canUp = st.selectedMod > 0;
            const bool canDown = st.selectedMod >= 0 && st.selectedMod + 1 < int(st.mods.size());
            ImGui::BeginDisabled(!canUp);
            if (ImGui::Button("Move up"))
            {
                std::swap(st.mods[st.selectedMod], st.mods[st.selectedMod - 1]);
                st.selectedMod--;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!canDown);
            if (ImGui::Button("Move down"))
            {
                std::swap(st.mods[st.selectedMod], st.mods[st.selectedMod + 1]);
                st.selectedMod++;
            }
            ImGui::EndDisabled();
            ImGui::Spacing();

            if (st.mods.empty())
            {
                HelpText("No mods installed. Put mod folders in the mods folder and press Refresh.");
                return;
            }

            const float detailsHeight = ImGui::GetTextLineHeightWithSpacing() * 4;
            constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
            if (ImGui::BeginTable("mods", 5, flags, ImVec2(0, -detailsHeight)))
            {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 3.0f);
                ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("Author", ImGuiTableColumnFlags_WidthStretch, 1.5f);
                ImGui::TableSetupColumn("Files", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableHeadersRow();
                for (int i = 0; i < int(st.mods.size()); i++)
                {
                    auto& mod = st.mods[i];
                    ImGui::PushID(i);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Checkbox("##on", &mod.enabled);
                    ImGui::TableNextColumn();
                    if (ImGui::Selectable(mod.name.c_str(), st.selectedMod == i, ImGuiSelectableFlags_SpanAllColumns))
                        st.selectedMod = i;
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(mod.version.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(mod.author.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", int(mod.fileCount));
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (st.selectedMod >= 0 && st.selectedMod < int(st.mods.size()))
            {
                const auto& mod = st.mods[st.selectedMod];
                ImGui::Text("mods/%s", mod.folder.c_str());
                HelpText(mod.description.empty() ? "No description." : mod.description.c_str());
            }
        }

        void PatchesTab(State& st)
        {
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

        void ApplyStyle(float scale)
        {
            ImGui::StyleColorsDark();
            ImGuiStyle& style = ImGui::GetStyle();
            style.WindowPadding = ImVec2(18, 14);
            style.FramePadding = ImVec2(10, 6);
            style.ItemSpacing = ImVec2(10, 8);
            style.FrameRounding = 4;
            style.TabRounding = 4;
            style.GrabRounding = 4;
            style.ScrollbarRounding = 4;
            style.WindowBorderSize = 0;
            style.ScaleAllSizes(scale);

            // Dark warm grey with Dragon Age's blood red as the accent.
            ImVec4* c = style.Colors;
            const ImVec4 accent(0.62f, 0.10f, 0.10f, 1.0f), accentHover(0.75f, 0.16f, 0.15f, 1.0f),
                accentActive(0.85f, 0.22f, 0.18f, 1.0f);
            c[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.08f, 0.08f, 1.0f);
            c[ImGuiCol_FrameBg] = ImVec4(0.17f, 0.15f, 0.15f, 1.0f);
            c[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.20f, 0.19f, 1.0f);
            c[ImGuiCol_FrameBgActive] = ImVec4(0.30f, 0.22f, 0.21f, 1.0f);
            c[ImGuiCol_Button] = accent;
            c[ImGuiCol_ButtonHovered] = accentHover;
            c[ImGuiCol_ButtonActive] = accentActive;
            c[ImGuiCol_Header] = ImVec4(0.45f, 0.10f, 0.10f, 0.6f);
            c[ImGuiCol_HeaderHovered] = ImVec4(0.55f, 0.14f, 0.13f, 0.8f);
            c[ImGuiCol_HeaderActive] = accentActive;
            c[ImGuiCol_Tab] = ImVec4(0.17f, 0.15f, 0.15f, 1.0f);
            c[ImGuiCol_TabHovered] = accentHover;
            c[ImGuiCol_TabSelected] = accent;
            c[ImGuiCol_CheckMark] = ImVec4(0.92f, 0.35f, 0.28f, 1.0f);
            c[ImGuiCol_SliderGrab] = accentHover;
            c[ImGuiCol_SliderGrabActive] = accentActive;
            c[ImGuiCol_NavCursor] = ImVec4(0.95f, 0.75f, 0.45f, 1.0f);
        }

        ImFont* LoadFont(const char* const* files, float size)
        {
            for (const char* const* file = files; *file; file++)
            {
                if (fs::exists(*file))
                    return ImGui::GetIO().Fonts->AddFontFromFileTTF(*file, size);
            }
            return nullptr;
        }
    }

    bool ShouldShow(int argc, char** argv, const fs::path& userRoot)
    {
        for (int i = 1; i < argc; i++)
        {
            if (strcmp(argv[i], "--launcher") == 0)
                return true;
            if (strcmp(argv[i], "--play") == 0)
                return false;
        }
        if (getenv("DAO_SKIP_LAUNCHER"))
            return false;
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
            return true;
        return ReadSettings(userRoot).showLauncher;
    }

    bool Run(const fs::path& gameRoot, const fs::path& userRoot)
    {
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
        {
            LOGE("Launcher: SDL_Init failed: {}; starting the game", SDL_GetError());
            return true;
        }

        const float scale = std::max(1.0f, SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay()));
        SDL_Window* window = SDL_CreateWindow("Dragon Age: Origins - Launcher", int(860 * scale), int(600 * scale),
            SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
        if (!renderer)
        {
            LOGE("Launcher: no window ({}); starting the game", SDL_GetError());
            if (window)
                SDL_DestroyWindow(window);
            SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
            return true;
        }
        SDL_SetRenderVSync(renderer, 1);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        ApplyStyle(scale);

        // The vendored ImGui has no built-in font; use the system UI font.
        static const char* const kRegular[] = { "C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\arial.ttf",
            nullptr };
        static const char* const kBold[] = { "C:\\Windows\\Fonts\\seguisb.ttf", "C:\\Windows\\Fonts\\segoeuib.ttf",
            "C:\\Windows\\Fonts\\arialbd.ttf", nullptr };
        State st;
        if (!LoadFont(kRegular, 18.0f * scale))
        {
            LOGE("Launcher: no system font found; starting the game");
            ImGui::DestroyContext();
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
            return true;
        }
        st.titleFont = LoadFont(kBold, 34.0f * scale);
        if (!st.titleFont)
            st.titleFont = io.Fonts->Fonts[0];

        ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
        ImGui_ImplSDLRenderer3_Init(renderer);

        st.gameRoot = gameRoot;
        st.userRoot = userRoot;
        st.modsRoot = mods::ModsRoot(gameRoot);
        st.settings = ReadSettings(userRoot);
        st.mods = mods::Scan(st.modsRoot, st.settings.mods);
        if (const char* dir = getenv("DAO_LAUNCHER_SCREENSHOTS"))
            st.screenshotDir = dir;

        while (!st.play && !st.quit)
        {
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                ImGui_ImplSDL3_ProcessEvent(&event);
                if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                    st.quit = true;
            }
            if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)
            {
                SDL_Delay(20);
                continue;
            }

            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();

            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(viewport->WorkPos);
            ImGui::SetNextWindowSize(viewport->WorkSize);
            ImGui::Begin("launcher", nullptr,
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                    ImGuiWindowFlags_NoBringToFrontOnFocus);
            // Screenshot mode: 10 frames per tab, captured on the last one.
            constexpr int kFramesPerTab = 10;
            const int shotTab = st.screenshotDir.empty() ? -1 : st.screenshotFrame / kFramesPerTab;
            const bool shotNow = shotTab >= 0 && st.screenshotFrame % kFramesPerTab == kFramesPerTab - 1;
            if (ImGui::BeginTabBar("tabs"))
            {
                using TabFn = void (*)(State&);
                constexpr std::pair<const char*, TabFn> kTabs[] = {
                    { "Play", PlayTab }, { "Settings", SettingsTab }, { "Mods", ModsTab }, { "Patches", PatchesTab } };
                for (int i = 0; i < int(std::size(kTabs)); i++)
                {
                    const ImGuiTabItemFlags flags =
                        shotTab == i && st.screenshotFrame % kFramesPerTab == 0 ? ImGuiTabItemFlags_SetSelected : 0;
                    if (ImGui::BeginTabItem(kTabs[i].first, nullptr, flags))
                    {
                        kTabs[i].second(st);
                        ImGui::EndTabItem();
                    }
                }
                ImGui::EndTabBar();
                if (shotTab >= int(std::size(kTabs)))
                    st.quit = true;
            }
            ImGui::End();

            ImGui::Render();
            SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
            SDL_SetRenderDrawColor(renderer, 23, 20, 20, 255);
            SDL_RenderClear(renderer);
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
            if (shotNow)
            {
                if (SDL_Surface* surface = SDL_RenderReadPixels(renderer, nullptr))
                {
                    const auto path = st.screenshotDir / fmt::format("launcher_{}.bmp", shotTab);
                    SDL_SaveBMP(surface, path.string().c_str());
                    SDL_DestroySurface(surface);
                }
            }
            if (!st.screenshotDir.empty())
                st.screenshotFrame++;
            SDL_RenderPresent(renderer);
        }

        st.settings.mods = mods::ToState(st.mods);
        SaveSettings(userRoot, st.settings);
        LOGI("Launcher: {}", st.play ? "play" : "quit");

        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
        return st.play;
    }
}
