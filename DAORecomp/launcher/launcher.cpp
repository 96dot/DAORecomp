#include "stdafx.h"
#include "launcher/launcher.h"
#include "launcher/launcher_internal.h"

#include <imgui_internal.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_sdlrenderer3.h>

#include "text.h"

namespace dao::launcher
{
    namespace
    {
        // How long a view takes to fade in.
        constexpr float kFadeSeconds = 0.2f;

        void ApplyStyle(float scale)
        {
            ImGui::StyleColorsDark();
            ImGuiStyle& style = ImGui::GetStyle();
            style.WindowPadding = ImVec2(18, 14);
            style.FramePadding = ImVec2(10, 6);
            style.ItemSpacing = ImVec2(10, 7);
            style.FrameRounding = 0;
            style.GrabRounding = 0;
            style.ScrollbarRounding = 0;
            style.ScrollbarSize = 10;
            style.GrabMinSize = 14;
            style.WindowBorderSize = 0;
            style.ChildBorderSize = 0;
            style.FrameBorderSize = 1;
            style.PopupBorderSize = 1;
            style.ScaleAllSizes(scale);

            // The Options screen's: dark frames edged in gold, cream text.
            ImVec4* c = style.Colors;
            const ImVec4 gold = ImGui::ColorConvertU32ToFloat4(kGold);
            const ImVec4 goldBright = ImGui::ColorConvertU32ToFloat4(kGoldBright);
            auto withAlpha = [](ImVec4 color, float alpha) { return ImVec4(color.x, color.y, color.z, alpha); };
            c[ImGuiCol_Text] = ImGui::ColorConvertU32ToFloat4(kText);
            c[ImGuiCol_TextDisabled] = ImVec4(0.66f, 0.61f, 0.53f, 1.0f);
            c[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
            c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
            c[ImGuiCol_PopupBg] = ImVec4(0.06f, 0.05f, 0.04f, 0.97f);
            c[ImGuiCol_Border] = withAlpha(gold, 0.45f);
            c[ImGuiCol_FrameBg] = ImVec4(0.07f, 0.06f, 0.05f, 0.85f);
            c[ImGuiCol_FrameBgHovered] = ImVec4(0.16f, 0.12f, 0.08f, 0.9f);
            c[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.16f, 0.10f, 0.95f);
            c[ImGuiCol_Button] = ImVec4(0.10f, 0.08f, 0.06f, 0.9f);
            c[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.18f, 0.10f, 0.95f);
            c[ImGuiCol_ButtonActive] = ImVec4(0.36f, 0.24f, 0.12f, 1.0f);
            c[ImGuiCol_Header] = withAlpha(gold, 0.30f);
            c[ImGuiCol_HeaderHovered] = withAlpha(gold, 0.42f);
            c[ImGuiCol_HeaderActive] = withAlpha(gold, 0.55f);
            c[ImGuiCol_CheckMark] = goldBright;
            c[ImGuiCol_SliderGrab] = gold;
            c[ImGuiCol_SliderGrabActive] = goldBright;
            c[ImGuiCol_Separator] = withAlpha(gold, 0.4f);
            c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0.25f);
            c[ImGuiCol_ScrollbarGrab] = withAlpha(gold, 0.55f);
            c[ImGuiCol_ScrollbarGrabHovered] = withAlpha(goldBright, 0.75f);
            c[ImGuiCol_ScrollbarGrabActive] = goldBright;
            c[ImGuiCol_TableHeaderBg] = ImVec4(0.12f, 0.09f, 0.06f, 0.9f);
            c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.03f);
            c[ImGuiCol_NavCursor] = goldBright;
        }

        ImFont* LoadFont(const char* const* files, float size)
        {
            // Latin, Greek and Cyrillic (mod names), and the punctuation and arrows of
            // release notes (dashes, quotes, ellipses).
            static constexpr ImWchar kGlyphs[] = { 0x0020, 0x017F, 0x0370, 0x03FF, 0x0400, 0x04FF, 0x2010, 0x2027,
                0x2190, 0x21FF, 0 };
            // The serif fonts have no arrows, and some lack the other alphabets: Segoe
            // UI's fill in (merged glyphs don't replace the font's own).
            static constexpr const char* kFallbackFont = "C:\\Windows\\Fonts\\segoeui.ttf";
            ImFontAtlas& fonts = *ImGui::GetIO().Fonts;
            for (const char* const* file = files; *file; file++)
            {
                if (!fs::exists(*file))
                    continue;
                ImFont* font = fonts.AddFontFromFileTTF(*file, size, nullptr, kGlyphs);
                if (font && fs::exists(kFallbackFont))
                {
                    ImFontConfig merge;
                    merge.MergeMode = true;
                    fonts.AddFontFromFileTTF(kFallbackFont, size, &merge, kGlyphs);
                }
                return font;
            }
            return nullptr;
        }
    }

    bool ShouldShow(int argc, char** argv, const fs::path& userRoot)
    {
        for (int i = 1; i < argc; i++)
        {
            if (strcmp(argv[i], "--launcher") == 0 || strcmp(argv[i], "--mods") == 0)
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

    namespace
    {
        bool HasArgument(int argc, char** argv, const char* wanted)
        {
            if (argc < 2)
                return false;
            return std::ranges::any_of(std::span(argv + 1, argv + argc),
                [wanted](const char* arg) { return strcmp(arg, wanted) == 0; });
        }
    }

    bool ShowsMods(int argc, char** argv)
    {
        return HasArgument(argc, argv, "--mods");
    }

    bool AddsToSteam(int argc, char** argv)
    {
        return HasArgument(argc, argv, "--add-to-steam");
    }

    namespace
    {
        struct RunMode
        {
            // Open on the Mods page.
            bool showMods = false;
            // Add the game to Steam once open (as setup was asked to).
            bool addToSteam = false;
            // Setup (the launcher-only build): the Install view for `dest`, started
            // at once with `update` when the folder has the game's files.
            bool setup = false;
            bool update = false;
            fs::path dest, iso;
        };
    }

    // The window, until Play (true) or Close; `setup` gets what setup did.
    static bool RunWindow(const fs::path& gameRoot, const fs::path& userRoot, const RunMode& mode,
        SetupResult* setup)
    {
        // No window: the game starts without the launcher; setup has nothing to show.
        const auto noWindow = [&](const std::string& why) {
            LOGE("Launcher: {}; {}", why, setup ? "setup can't be shown" : "starting the game");
            if (setup)
                setup->failed = true;
            return true;
        };
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
            return noWindow(fmt::format("SDL_Init failed: {}", SDL_GetError()));

        // The window: 16:9 like the game's screens, at the display's scale; resizable
        // down to a size the menus still fit.
        constexpr float kWindowWidth = 1120.0f, kWindowHeight = 630.0f, kMinWidth = 800.0f, kMinHeight = 450.0f;
        const float scale = std::max(1.0f, SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay()));
        SDL_Window* window = SDL_CreateWindow(mode.setup ? "Dragon Age: Origins - Setup" : "Dragon Age: Origins - Launcher",
            int(kWindowWidth * scale),
            int(kWindowHeight * scale), SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
        // The exe's icon (MAINICON in res/daorecomp.rc) on the title bar and taskbar
        // button at the window's DPI (SDL's window class takes it from the exe at the
        // system's). Freed after the window.
        struct WindowIcons
        {
            HICON big = nullptr, small = nullptr;
            ~WindowIcons()
            {
                for (HICON icon : { big, small })
                    if (icon)
                        DestroyIcon(icon);
            }
        } icons;
        const HWND hwnd = window ? static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                       SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr))
                                 : nullptr;
        if (hwnd)
        {
            const UINT dpi = GetDpiForWindow(hwnd);
            const auto load = [dpi](int metric) {
                const int size = GetSystemMetricsForDpi(metric, dpi);
                return static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), L"MAINICON", IMAGE_ICON, size, size, 0));
            };
            icons.big = load(SM_CXICON);
            icons.small = load(SM_CXSMICON);
            if (icons.big)
                SendMessageW(hwnd, WM_SETICON, ICON_BIG, LPARAM(icons.big));
            if (icons.small)
                SendMessageW(hwnd, WM_SETICON, ICON_SMALL, LPARAM(icons.small));
        }
        // DAO_BACKGROUND=1 (automated tests): behind the other windows, unfocused.
        const char* background = getenv("DAO_BACKGROUND");
        if (window && background && atoi(background))
        {
            SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
            if (const auto hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                    SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr)))
                SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
        }
        if (window)
            SDL_ShowWindow(window);
        SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
        if (!renderer)
        {
            const std::string why = fmt::format("no window ({})", SDL_GetError());
            if (window)
                SDL_DestroyWindow(window);
            SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
            return noWindow(why);
        }
        SDL_SetWindowMinimumSize(window, int(kMinWidth * scale), int(kMinHeight * scale));
        if (!SDL_SetRenderVSync(renderer, 1))
            LOGW("Launcher: vsync is unavailable ({})", SDL_GetError());
        // The driver can turn vsync off anyway, so frames are also capped: the menus'
        // slow animations need no more than 60 fps (saving power on a handheld). The
        // cap is 5% above the refresh rate when that's lower, so a vsynced frame is
        // never held back; with no refresh rate known, 60 Hz.
        constexpr float kMaxMenuFps = 60.0f;
        const SDL_DisplayMode* display = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
        const float refresh = display && display->refresh_rate > 0 ? display->refresh_rate : kMaxMenuFps;
        const float maxFps = std::min(refresh, kMaxMenuFps) * 1.05f;
        const Uint64 minFrameNs = Uint64(1e9 / std::max(maxFps, 20.0f));

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        ApplyStyle(scale);

        // The vendored ImGui has no built-in font; use the system's serif fonts, like
        // the game's menus (Proton has a Times New Roman look-alike).
        static const char* const kRegular[] = { "C:\\Windows\\Fonts\\pala.ttf", "C:\\Windows\\Fonts\\georgia.ttf",
            "C:\\Windows\\Fonts\\times.ttf", "C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\arial.ttf",
            nullptr };
        static const char* const kItalic[] = { "C:\\Windows\\Fonts\\palai.ttf", "C:\\Windows\\Fonts\\georgiai.ttf",
            "C:\\Windows\\Fonts\\timesi.ttf", nullptr };
        static const char* const kBold[] = { "C:\\Windows\\Fonts\\palab.ttf", "C:\\Windows\\Fonts\\georgiab.ttf",
            "C:\\Windows\\Fonts\\timesbd.ttf", "C:\\Windows\\Fonts\\arialbd.ttf", nullptr };
        State st;
        st.scale = scale;
        g_state = &st;
        ImFont* regular = LoadFont(kRegular, 19.0f * scale);
        if (!regular)
        {
            g_state = nullptr;
            ImGui::DestroyContext();
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
            return noWindow("no system font found");
        }
        st.helpFont = LoadFont(kItalic, 17.0f * scale);
        st.headingFont = LoadFont(kBold, 20.0f * scale);
        // Drawn at the window's size: baked large enough to scale down.
        st.menuFont = LoadFont(kBold, 36.0f * scale);
        st.titleFont = LoadFont(kBold, 48.0f * scale);
        for (ImFont** font : { &st.helpFont, &st.headingFont, &st.menuFont, &st.titleFont })
        {
            if (!*font)
                *font = regular;
        }

        ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
        ImGui_ImplSDLRenderer3_Init(renderer);
        // The gamepads are fed by FeedGamepads: all of them, the left stick as the
        // d-pad, without the window switcher.
        ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, nullptr, 0);
        if (const char* padSteps = getenv("DAO_LAUNCHER_SCREENSHOTS") ? getenv("DAO_LAUNCHER_PAD") : nullptr)
            StartPadScript(st, padSteps);
        OpenGamepads(st);
        // With a gamepad, the focus shows from the start (a click hides it).
        if (!st.gamepads.empty())
            ImGui::SetNavCursorVisible(true);

        st.gameRoot = gameRoot;
        st.userRoot = userRoot;
        st.modsRoot = mods::ModsRoot(gameRoot);
        st.gameFound = fs::exists(gameRoot / "default.xex");
        st.window = window;
        st.renderer = renderer;
        st.setupOnly = mode.setup;
        st.steamPath = steam::FindSteam();
        if (mode.setup)
        {
            std::error_code ec;
            if (!fs::is_regular_file(gameRoot / kArtArchive, ec) && fs::is_regular_file(SetupArtRoot() / kArtArchive, ec))
                st.gameRoot = SetupArtRoot();
            st.installDest = Utf8(mode.dest);
            st.installIso = Utf8(mode.iso);
            // An update keeps the shortcuts there are (or the player removed).
            st.installShortcuts = !HasGameFiles(st);
            st.installSteam = !HasGameFiles(st);
            SetView(st, View::Install);
            // The launcher's Update: straight on, as there's nothing to choose.
            if (mode.update && HasGameFiles(st))
                StartInstall(st);
        }
        else
        {
            // (One that can't be read is left for the player to fix, not replaced with defaults.)
            if (std::optional<Settings> settings = TryReadSettings(userRoot))
                st.settings = *settings;
            else
                st.settingsUnreadable = true;
            mods::CleanUpInstalls(st.modsRoot);
            mods::CleanUpConversions(st.modsRoot);
            ReadMods(st, st.settings.mods);
            if (mode.showMods)
                SetView(st, View::Mods);
        }
        if (const char* dir = getenv("DAO_LAUNCHER_SCREENSHOTS"))
        {
            st.screenshotDir = dir;
            // A drop to test adding mods and installing packages with: paths separated by |.
            if (const wchar_t* drop = _wgetenv(L"DAO_LAUNCHER_DROP"))
            {
                for (const auto part : std::views::split(std::wstring_view(drop), L'|'))
                    st.dropped.push_back(std::wstring_view(part.begin(), part.end()));
                AddDroppedMods(st);
            }
        }
        // (Setup may have no art until a disc image is chosen.)
        if (std::error_code ec; !st.setupOnly || fs::is_regular_file(st.gameRoot / kArtArchive, ec))
            LoadArt(st, renderer);
        if (mode.addToSteam && !st.steamPath.empty())
            st.steamStep = SteamStep::Start;
        st.updater = std::make_unique<Updater>();
        if (st.settings.checkForUpdates && !mode.setup)
            st.updater->Check(st.settings.updateChannel);

        // Screenshot mode: kFramesPerShot frames per shot (main, the configure tabs,
        // mods, save backups, DLC, and the update's page if the check finds one), captured
        // on the last one, after the view has faded in.
        constexpr int kFramesPerShot = 20;
        constexpr int kModsShot = 1 + kConfigureTabs;
        constexpr int kSavesShot = kModsShot + 1;
        constexpr int kDlcShot = kSavesShot + 1;
        constexpr int kUpdateShot = kDlcShot + 1;
        // While minimized or in the background: at most this often, to save power.
        constexpr int kIdleWaitMs = 100;

        Uint64 frameStart = SDL_GetTicksNS();
        while ((!st.play && !st.quit) || st.steamStep == SteamStep::Working)
        {
            const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
            const bool background = !(flags & SDL_WINDOW_INPUT_FOCUS) && st.screenshotDir.empty();
            if (flags & SDL_WINDOW_MINIMIZED || background)
                SDL_WaitEventTimeout(nullptr, kIdleWaitMs);
            SDL_Event event;
            if (st.padScript)
                StepPadScript(st);
            while (SDL_PollEvent(&event))
            {
                ImGui_ImplSDL3_ProcessEvent(&event);
                HandleGamepadEvent(st, event);
                // (Not while adding to Steam: its result says what's become of Steam.)
                if ((event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) &&
                    st.steamStep != SteamStep::Working)
                    st.quit = true;
                // Files dropped on the window: one drop is its paths between
                // begin and complete.
                else if (event.type == SDL_EVENT_DROP_BEGIN)
                    st.dropped.clear();
                else if (event.type == SDL_EVENT_DROP_FILE && event.drop.data)
                    st.dropped.push_back(FromUtf8(event.drop.data));
                else if (event.type == SDL_EVENT_DROP_COMPLETE && st.view == View::Install)
                    TakeDroppedInstallFiles(st);
                else if (event.type == SDL_EVENT_DROP_COMPLETE)
                    AddDroppedMods(st);
            }
            FinishReadingMods(st);
            FinishAddingMods(st);
            FinishConverting(st);
            FinishReadingDlc(st);
            FinishSteamWork(st);
            // Installed: the new game's launcher takes over.
            st.installer.Poll();
            if (st.installer.GetState() == InstallRunner::State::Done)
                st.quit = true;
            if (flags & SDL_WINDOW_MINIMIZED)
                continue;

            // Before NewFrame, which handles ImGui's own uses of the keys.
            {
                const ImGuiContext& g = *ImGui::GetCurrentContext();
                const ImGuiWindow* nav = g.NavWindow;
                st.backKeysFree = g.OpenPopupStack.empty() && !g.ActiveId &&
                    (!nav || !nav->RootWindowForNav->ParentWindow);
            }
            ImGui_ImplSDLRenderer3_NewFrame();
            // (After the renderer's NewFrame, which makes the font texture the art uses.)
            UpdateSteam(st);
            ImGui_ImplSDL3_NewFrame();
            FeedGamepads(st);
            ImGui::NewFrame();

            // (A pad script captures when it says.)
            const int shot = st.screenshotDir.empty() || st.padScript ? -1 : st.screenshotFrame / kFramesPerShot;
            const bool shotStart = shot >= 0 && st.screenshotFrame % kFramesPerShot == 0;
            const bool shotNow = shot >= 0 && st.screenshotFrame % kFramesPerShot == kFramesPerShot - 1;
            // The mods page waits for mods being added, DLC for its reading, the update's
            // page for the check.
            const bool updateShot = shot == kUpdateShot;
            const bool shotWaits = (shot == kModsShot && (ReadingMods(st) || AddingMods(st))) ||
                (shot == kDlcShot && ReadingDlc(st)) ||
                (updateShot && st.updater->GetState() == Updater::State::Checking) ||
                (st.setupOnly && (st.isoArt.valid() || st.installer.GetState() == InstallRunner::State::Running));
            // (The update's page only when there's an update.)
            if (st.setupOnly)
            {
                // (Setup has only the Install view.)
                if (shot > 0)
                    st.quit = true;
            }
            else if (shot > kUpdateShot || (updateShot && !shotWaits && !FoundRelease(st)))
                st.quit = true;
            else if (shotStart && !shotWaits)
            {
                SetView(st, shot == 0 ? View::Main
                        : shot == kModsShot ? View::Mods
                        : shot == kSavesShot ? View::Saves
                        : shot == kDlcShot ? View::Dlc
                        : updateShot ? View::Update
                        : View::Configure);
                if (shot >= 1 && shot <= kConfigureTabs)
                    st.configureTab = shot - 1;
            }

            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            const Layout layout(viewport);
            ImGui::SetNextWindowPos(viewport->Pos);
            ImGui::SetNextWindowSize(viewport->Size);
            ImGui::Begin("launcher", nullptr,
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                    ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground |
                    ImGuiWindowFlags_NoScrollWithMouse);
            const View view = st.view;
            const bool viewChanged = st.viewChanged;
            // Screenshots skip the fade.
            st.fade = st.screenshotDir.empty()
                ? std::clamp(float(ImGui::GetTime() - st.viewStart) / kFadeSeconds, 0.0f, 1.0f)
                : 1.0f;
            DrawScene(ImGui::GetBackgroundDrawList(), st, layout, view == View::Main ? 0.0f : kPanelSceneDim,
                ImGui::GetTime());
            switch (view)
            {
            case View::Main:
                MainView(st, layout);
                break;
            case View::Configure:
                ConfigureView(st, layout);
                break;
            case View::Mods:
                ModsView(st, layout);
                break;
            case View::Saves:
                SavesView(st, layout);
                break;
            case View::Dlc:
                DlcView(st, layout);
                break;
            case View::Update:
                UpdateView(st, layout);
                break;
            case View::Install:
                InstallView(st, layout);
                break;
            }
            SteamPopup(st);
            // Focus moves to the new view's first item in the frame it's shown.
            if (viewChanged && st.view == view)
                st.viewChanged = false;
            ImGui::End();
            // A downloaded update: its installer replaces the game's files, so close (once
            // mods and Steam are done with, and not if the game is starting).
            if (st.updater->GetState() == Updater::State::Ready && !st.play && !ModsBusy(st) &&
                st.steamStep != SteamStep::Working && st.updater->Launch())
                st.quit = true;

            ImGui::Render();
            SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
            SDL_SetRenderDrawColor(renderer, 12, 9, 8, 255);
            SDL_RenderClear(renderer);
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
            if (shotNow || (st.padScript && st.padScript->capture))
            {
                const std::string name = shotNow ? fmt::format("launcher_{}.bmp", shot)
                                                 : fmt::format("launcher_pad_{}.bmp", st.padScript->shots);
                if (!shotNow)
                    st.padScript->shots++;
                const std::string path = Utf8(st.screenshotDir / name);
                SDL_Surface* surface = SDL_RenderReadPixels(renderer, nullptr);
                if (!surface || !SDL_SaveBMP(surface, path.c_str()))
                    LOGW("Launcher: can't save {}: {}", path, SDL_GetError());
                SDL_DestroySurface(surface);
            }
            if (!st.screenshotDir.empty() && !shotWaits)
                st.screenshotFrame++;
            SDL_RenderPresent(renderer);
            if (const Uint64 elapsed = SDL_GetTicksNS() - frameStart; elapsed < minFrameNs)
                SDL_DelayNS(minFrameNs - elapsed);
            frameStart = SDL_GetTicksNS();
        }

        // Mods still being read, added or converted: stopped (what's done is kept).
        if (ModsBusy(st) || ReadingMods(st))
        {
            st.modStop.request_stop();
            if (ReadingMods(st))
                st.modScan.wait();
            if (AddingMods(st))
                st.modInstall.wait();
            if (ConvertingMod(st))
                st.modConvert.wait();
            FinishReadingMods(st);
            FinishAddingMods(st);
            FinishConverting(st);
        }
        // (Not if the list couldn't be read: the saved one stays.)
        if (st.modsRead)
            st.settings.mods = mods::ToState(st.mods);
        if (!st.setupOnly && !st.settingsUnreadable)
            SaveSettings(userRoot, st.settings);
        else if (st.settingsUnreadable)
            LOGW("Launcher: config.toml couldn't be read, so it isn't saved over");
        // (Closed while installing: the installer is stopped with st.)
        if (setup && st.installer.GetState() == InstallRunner::State::Done)
            *setup = { st.installer.Installed(), st.installSteam && !st.steamPath.empty() };
        LOGI("Launcher: {}", st.play ? "play" : "quit");

        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        CloseGamepads(st);
        for (auto& [name, art] : st.art)
            SDL_DestroyTexture(art.texture);
        g_state = nullptr;
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
        return st.play;
    }

    bool Run(const fs::path& gameRoot, const fs::path& userRoot, bool showMods, bool addToSteam)
    {
        return RunWindow(gameRoot, userRoot, { .showMods = showMods, .addToSteam = addToSteam }, nullptr);
    }

    SetupResult RunSetup(const fs::path& dest, const fs::path& iso, bool update)
    {
        SetupResult result;
        RunWindow(dest / "game", dest / "userdata", { .setup = true, .update = update, .dest = dest, .iso = iso },
            &result);
        return result;
    }
}
