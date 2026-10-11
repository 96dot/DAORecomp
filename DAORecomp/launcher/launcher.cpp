#include "stdafx.h"
#include "launcher/launcher.h"

#include <shellapi.h>

#include <fstream>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>
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
            { 1280, 720 }, { 1280, 800 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 },
        };
        constexpr const char* kScales[] = { "1x - 1280x720", "2x - 2560x1440", "3x - 3840x2160 (4K)", "4x - 5120x2880" };
        constexpr const char* kUpscalers[] = { "Bilinear", "FidelityFX CAS", "FidelityFX FSR 1" };
        constexpr const char* kTemporalUpscalers[] = { "Off", "Auto (FSR 4 where supported, else FSR 3.1)",
            "FidelityFX FSR 4", "FidelityFX FSR 3.1" };
        // Settings::anisotropicFiltering values and their names.
        constexpr int kAnisotropicLevels[] = { 0, 2, 4, 8, 16 };
        constexpr const char* kAnisotropicNames[] = { "Game default", "2x", "4x", "8x", "16x" };

        // The PC launcher's look: sepia art on black, dark red buttons with gold edges.
        constexpr ImU32 kGold = IM_COL32(176, 142, 82, 255);
        constexpr ImU32 kGoldBright = IM_COL32(232, 196, 120, 255);
        constexpr ImU32 kButtonTop = IM_COL32(98, 38, 24, 255);
        constexpr ImU32 kButtonBottom = IM_COL32(48, 16, 10, 255);
        constexpr ImU32 kButtonTopHot = IM_COL32(134, 54, 32, 255);
        constexpr ImU32 kButtonBottomHot = IM_COL32(70, 24, 14, 255);
        constexpr ImU32 kButtonText = IM_COL32(244, 230, 204, 255);
        constexpr ImU32 kLogo = IM_COL32(214, 206, 190, 255);

        enum class View { Main, Configure, Mods };

        struct State
        {
            fs::path gameRoot, userRoot, modsRoot;
            Settings settings;
            std::vector<mods::ModInfo> mods;
            int selectedMod = -1;
            View view = View::Main;
            bool viewChanged = true;
            ImFont* buttonFont = nullptr;
            ImFont* logoFont = nullptr;
            ImFont* logoSmallFont = nullptr;
            SDL_Texture* art = nullptr;
            int artWidth = 0, artHeight = 0;
            float scale = 1.0f;
            bool play = false;
            bool quit = false;
            // DAO_LAUNCHER_SCREENSHOTS=<dir>: show each view, save it as BMP, quit.
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

        // The game's splash screen (splash.dds: 1280x720 DXT1, red art on light grey),
        // decoded and recoloured to sepia on black. Empty if missing or not DXT1.
        std::vector<uint8_t> LoadArt(const fs::path& gameRoot, int& width, int& height)
        {
            std::ifstream file(gameRoot / "splash.dds", std::ios::binary);
            std::vector<uint8_t> dds((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            auto u32 = [&](size_t offset) {
                return uint32_t(dds[offset]) | uint32_t(dds[offset + 1]) << 8 | uint32_t(dds[offset + 2]) << 16 |
                    uint32_t(dds[offset + 3]) << 24;
            };
            if (dds.size() < 128 || memcmp(dds.data(), "DDS ", 4) || memcmp(&dds[84], "DXT1", 4))
                return {};
            height = int(u32(12));
            width = int(u32(16));
            const int blocksX = (width + 3) / 4, blocksY = (height + 3) / 4;
            if (width <= 0 || height <= 0 || width > 4096 || height > 4096 ||
                dds.size() < 128 + size_t(blocksX) * blocksY * 8)
                return {};

            std::vector<uint8_t> rgb(size_t(width) * height * 3);
            for (int by = 0; by < blocksY; by++)
            {
                for (int bx = 0; bx < blocksX; bx++)
                {
                    const uint8_t* block = &dds[128 + (size_t(by) * blocksX + bx) * 8];
                    const uint32_t c0 = block[0] | block[1] << 8, c1 = block[2] | block[3] << 8;
                    int palette[4][3];
                    for (int i = 0; i < 2; i++)
                    {
                        const uint32_t c = i ? c1 : c0;
                        palette[i][0] = int(c >> 11) * 255 / 31;
                        palette[i][1] = int((c >> 5) & 63) * 255 / 63;
                        palette[i][2] = int(c & 31) * 255 / 31;
                    }
                    for (int ch = 0; ch < 3; ch++)
                    {
                        palette[2][ch] = c0 > c1 ? (2 * palette[0][ch] + palette[1][ch]) / 3
                                                 : (palette[0][ch] + palette[1][ch]) / 2;
                        palette[3][ch] = c0 > c1 ? (palette[0][ch] + 2 * palette[1][ch]) / 3 : 0;
                    }
                    uint32_t bits = block[4] | block[5] << 8 | block[6] << 16 | uint32_t(block[7]) << 24;
                    for (int i = 0; i < 16; i++, bits >>= 2)
                    {
                        const int x = bx * 4 + i % 4, y = by * 4 + i / 4;
                        if (x < width && y < height)
                        {
                            for (int ch = 0; ch < 3; ch++)
                                rgb[(size_t(y) * width + x) * 3 + ch] = uint8_t(palette[bits & 3][ch]);
                        }
                    }
                }
            }

            // The art (saturated or dark) in sepia shaded by its red, the background black
            // with a faint warm vignette, and the glowing spots kept bright.
            auto smooth = [](float e0, float e1, float x) {
                const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
                return t * t * (3.0f - 2.0f * t);
            };
            std::vector<uint8_t> rgba(size_t(width) * height * 4);
            for (int y = 0; y < height; y++)
            {
                for (int x = 0; x < width; x++)
                {
                    const uint8_t* p = &rgb[(size_t(y) * width + x) * 3];
                    const float r = p[0] / 255.0f, g = p[1] / 255.0f, b = p[2] / 255.0f;
                    const float maxC = std::max({ r, g, b }), minC = std::min({ r, g, b });
                    const float lum = 0.299f * r + 0.587f * g + 0.114f * b;
                    const float sat = (maxC - minC) / std::max(maxC, 1e-3f);
                    const float mask = std::max(smooth(0.12f, 0.45f, sat), smooth(0.25f, 0.08f, lum));
                    const float dx = (x - width * 0.55f) / width, dy = (y - height * 0.5f) / height;
                    const float vignette = 1.0f - std::clamp(std::sqrt(dx * dx + dy * dy) * 1.4f, 0.0f, 0.8f);
                    const float shade = 0.16f + std::pow(r, 1.5f) * 1.05f;
                    const float glow = sat > 0.2f ? smooth(0.75f, 0.95f, lum) * 0.6f : 0.0f;
                    const float sepia[3] = { 0.55f, 0.33f, 0.20f }, background[3] = { 0.07f, 0.05f, 0.04f },
                                glowColor[3] = { 1.0f, 0.85f, 0.55f };
                    for (int ch = 0; ch < 3; ch++)
                    {
                        const float v = background[ch] * vignette * (1.0f - mask) + sepia[ch] * shade * mask +
                            glowColor[ch] * glow;
                        rgba[(size_t(y) * width + x) * 4 + ch] = uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
                    }
                    rgba[(size_t(y) * width + x) * 4 + 3] = 255;
                }
            }
            return rgba;
        }

        // The art covering the window, its centre a little right of the window's.
        void DrawBackground(const State& st, float dim)
        {
            ImDrawList* draw = ImGui::GetBackgroundDrawList();
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            const ImVec2 pos = viewport->Pos, size = viewport->Size;
            draw->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(12, 9, 8, 255));
            if (st.art)
            {
                const float cover = std::max(size.x / st.artWidth, size.y / st.artHeight);
                const float w = st.artWidth * cover, h = st.artHeight * cover;
                const float x = pos.x + std::clamp(size.x * 0.62f - w * 0.5f, size.x - w, 0.0f);
                const float y = pos.y + (size.y - h) * 0.5f;
                const ImU32 tint = IM_COL32(int(255 * (1 - dim)), int(255 * (1 - dim)), int(255 * (1 - dim)), 255);
                draw->AddImage(ImTextureID(uintptr_t(st.art)), ImVec2(x, y), ImVec2(x + w, y + h), ImVec2(0, 0),
                    ImVec2(1, 1), tint);
            }
        }

        // "DRAGON AGE" in small capitals with "ORIGINS" under its right end, at the top right.
        void DrawLogo(const State& st)
        {
            ImDrawList* draw = ImGui::GetForegroundDrawList();
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImFont* big = st.logoFont;
            ImFont* small = st.logoSmallFont;
            const float bigSize = big->FontSize, smallSize = small->FontSize;
            struct Run
            {
                const char* text;
                bool capital;
            };
            constexpr Run kRuns[] = { { "D", true }, { "RAGON ", false }, { "A", true }, { "GE", false } };
            float width = 0.0f;
            for (const Run& run : kRuns)
                width += (run.capital ? big : small)->CalcTextSizeA(run.capital ? bigSize : smallSize, FLT_MAX, 0, run.text).x;
            const float margin = 28.0f * st.scale;
            float x = viewport->Pos.x + viewport->Size.x - margin - width;
            const float baseline = viewport->Pos.y + margin + bigSize;
            for (const Run& run : kRuns)
            {
                ImFont* font = run.capital ? big : small;
                const float size = run.capital ? bigSize : smallSize;
                // Fonts draw from the top of the line: align the baselines.
                const float y = baseline - size * 0.82f;
                draw->AddText(font, size, ImVec2(x + 2 * st.scale, y + 2 * st.scale), IM_COL32(0, 0, 0, 200), run.text);
                draw->AddText(font, size, ImVec2(x, y), kLogo, run.text);
                x += font->CalcTextSizeA(size, FLT_MAX, 0, run.text).x;
            }
            const char* origins = "O R I G I N S";
            const float originsSize = smallSize * 0.62f;
            const ImVec2 originsExtent = small->CalcTextSizeA(originsSize, FLT_MAX, 0, origins);
            const ImVec2 originsPos(x - originsExtent.x, baseline + 6 * st.scale);
            draw->AddText(small, originsSize, ImVec2(originsPos.x + 1, originsPos.y + 1), IM_COL32(0, 0, 0, 200), origins);
            draw->AddText(small, originsSize, originsPos, kLogo, origins);
        }

        // A button in the PC launcher's style; also activated with Enter or the gamepad.
        bool MenuButton(const State& st, const char* label, float width, bool enabled = true)
        {
            const float height = 38.0f * st.scale;
            ImGui::BeginDisabled(!enabled);
            const bool pressed = ImGui::InvisibleButton(label, ImVec2(width, height));
            ImGui::EndDisabled();
            const bool hot = enabled && (ImGui::IsItemHovered() || ImGui::IsItemFocused());
            const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const float radius = 3.0f * st.scale;
            draw->AddRectFilled(ImVec2(min.x + 3, min.y + 3), ImVec2(max.x + 3, max.y + 3), IM_COL32(0, 0, 0, 140), radius);
            draw->AddRectFilledMultiColor(min, max, hot ? kButtonTopHot : kButtonTop, hot ? kButtonTopHot : kButtonTop,
                hot ? kButtonBottomHot : kButtonBottom, hot ? kButtonBottomHot : kButtonBottom);
            draw->AddRect(min, max, hot ? kGoldBright : kGold, radius, 0, (hot ? 2.0f : 1.4f) * st.scale);
            draw->AddLine(ImVec2(min.x + 4, min.y + 2 * st.scale), ImVec2(max.x - 4, min.y + 2 * st.scale),
                IM_COL32(255, 220, 170, hot ? 70 : 40), 1.0f);
            const char* end = ImGui::FindRenderedTextEnd(label);
            ImFont* font = st.buttonFont;
            const ImVec2 text = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, label, end);
            const ImVec2 at(min.x + (width - text.x) * 0.5f, min.y + (height - text.y) * 0.5f);
            draw->AddText(font, font->FontSize, ImVec2(at.x + 1, at.y + 1), IM_COL32(0, 0, 0, 220), label, end);
            draw->AddText(font, font->FontSize, at, enabled ? kButtonText : IM_COL32(150, 130, 120, 255), label, end);
            return pressed && enabled;
        }

        void SetView(State& st, View view)
        {
            st.view = view;
            st.viewChanged = true;
        }

        void MainView(State& st)
        {
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            const bool gameFound = fs::exists(st.gameRoot / "default.xex");
            const float buttonWidth = 210.0f * st.scale;

            ImGui::SetCursorPos(ImVec2(36.0f * st.scale, viewport->Size.y * 0.30f));
            ImGui::BeginGroup();
            if (MenuButton(st, "Play", buttonWidth, gameFound) ||
                (gameFound && ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false)))
                st.play = true;
            if (st.viewChanged)
                ImGui::SetKeyboardFocusHere(-1);
            ImGui::Dummy(ImVec2(0, 6 * st.scale));
            if (MenuButton(st, "Configure", buttonWidth))
                SetView(st, View::Configure);
            ImGui::Dummy(ImVec2(0, 6 * st.scale));
            if (MenuButton(st, "Mods", buttonWidth))
                SetView(st, View::Mods);
            ImGui::Dummy(ImVec2(0, 6 * st.scale));
            if (MenuButton(st, "Exit", buttonWidth))
                st.quit = true;
            ImGui::EndGroup();

            // The bottom: the game data, a summary of the settings, the startup option.
            const float line = ImGui::GetTextLineHeightWithSpacing();
            ImGui::SetCursorPos(ImVec2(36.0f * st.scale, viewport->Size.y - line * 3.2f - 18 * st.scale));
            if (!gameFound)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.35f, 1.0f));
                ImGui::Text("Game data not found in %s", st.gameRoot.string().c_str());
                ImGui::PopStyleColor();
            }
            else
            {
                const auto enabledMods =
                    std::count_if(st.mods.begin(), st.mods.end(), [](auto& m) { return m.enabled; });
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::Text("%dx%d%s%s  |  %d mod%s on", 1280 * st.settings.resolutionScale,
                    720 * st.settings.resolutionScale, st.settings.unlockFrameRate ? ", 60 fps" : ", 30 fps",
                    st.settings.temporalUpscaler != TemporalUpscaler::Off ? ", temporal FSR" : "", int(enabledMods),
                    enabledMods == 1 ? "" : "s");
                ImGui::PopStyleColor();
            }
            ImGui::SetCursorPosX(36.0f * st.scale);
            ImGui::Checkbox("Show this launcher at startup", &st.settings.showLauncher);
            ImGui::SetCursorPosX(36.0f * st.scale);
            HelpText("When hidden, hold Shift while starting the game to bring it back.");
        }

        void GraphicsTab(State& st)
        {
            Settings& s = st.settings;
            ImGui::PushItemWidth(ImGui::GetFontSize() * 18);

            Section("Resolution");
            int scale = std::clamp(s.resolutionScale, 1, int(std::size(kScales))) - 1;
            if (ImGui::Combo("Rendering resolution", &scale, kScales, int(std::size(kScales))))
                s.resolutionScale = scale + 1;
            HelpText("The game renders at 1280x720; higher scales render every pass at that multiple. On a "
                     "Steam Deck, keep 1x.");

            Section("Anti-aliasing and scaling");
            int temporal = int(s.temporalUpscaler);
            if (ImGui::Combo("Temporal upscaling", &temporal, kTemporalUpscalers, int(std::size(kTemporalUpscalers))))
                s.temporalUpscaler = TemporalUpscaler(temporal);
            ImGui::BeginDisabled(s.temporalUpscaler == TemporalUpscaler::Off);
            ImGui::SliderFloat("Temporal sharpening", &s.temporalSharpness, 0.0f, 1.0f, "%.2f");
            ImGui::EndDisabled();
            HelpText("Upscales and anti-aliases the 3D scenes to the window size using motion vectors; menus and "
                     "the UI use the upscaler below. In a window no larger than the rendering resolution, it only "
                     "anti-aliases. Off is fastest on a Steam Deck.");
            ImGui::Spacing();
            int upscaler = int(s.upscaler);
            if (ImGui::Combo("Upscaler", &upscaler, kUpscalers, int(std::size(kUpscalers))))
                s.upscaler = Upscaler(upscaler);
            if (s.upscaler == Upscaler::Fsr)
                ImGui::SliderFloat("Sharpness reduction", &s.sharpness, 0.0f, 2.0f, "%.2f stops");
            else if (s.upscaler == Upscaler::Cas)
                ImGui::SliderFloat("Extra sharpness", &s.sharpness, 0.0f, 1.0f, "%.2f");
            HelpText("Used when the window size differs from the rendering resolution.");

            Section("Textures");
            int anisotropic = int(std::find(std::begin(kAnisotropicLevels), std::end(kAnisotropicLevels),
                                      s.anisotropicFiltering) -
                std::begin(kAnisotropicLevels));
            if (anisotropic >= int(std::size(kAnisotropicLevels)))
                anisotropic = 0;
            if (ImGui::Combo("Anisotropic filtering", &anisotropic, kAnisotropicNames, int(std::size(kAnisotropicNames))))
                s.anisotropicFiltering = kAnisotropicLevels[anisotropic];
            HelpText("Sharper textures on surfaces seen at an angle, such as floors and roads. The game's default "
                     "is the fastest.");
            ImGui::PopItemWidth();
        }

        void DisplayTab(State& st)
        {
            Settings& s = st.settings;
            ImGui::PushItemWidth(ImGui::GetFontSize() * 18);

            Section("Window");
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

            Section("Frame rate");
            ImGui::Checkbox("VSync", &s.vsync);
            ImGui::Checkbox("60 fps (unlock the 30 fps cap in 3D scenes)", &s.unlockFrameRate);

            Section("HDR (experimental)");
            ImGui::Checkbox("HDR output", &s.hdr);
            ImGui::BeginDisabled(!s.hdr);
            ImGui::SliderFloat("Paper white", &s.hdrPaperWhite, 80.0f, 400.0f, "%.0f nits");
            ImGui::SliderFloat("Peak brightness", &s.hdrPeak, s.hdrPaperWhite, 4000.0f, "%.0f nits");
            ImGui::EndDisabled();
            ImGui::PopItemWidth();
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

        void AdvancedTab(State& st)
        {
            ImGui::Spacing();
            if (ImGui::Button("Open user data folder"))
                OpenFolder(st.userRoot);
            HelpText("Saves, the configuration and the logs.");
            ImGui::Spacing();
            if (ImGui::Button("Open config.toml"))
                OpenFile(st.userRoot / "config.toml");
            HelpText("Extra Xenia GPU options go in the [xenia] section. Settings are saved when you press Play "
                     "or close the launcher.");
        }

        void ModsList(State& st)
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

        // A dark panel over the dimmed art with the view's title, its contents and Back
        // (also Escape or the gamepad's B).
        void PanelView(State& st, const char* title, int forcedTab, void (*contents)(State&, int))
        {
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            const float margin = 24.0f * st.scale;
            ImGui::SetCursorPos(ImVec2(margin, margin));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.04f, 0.035f, 0.90f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(kGold));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f * st.scale);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18 * st.scale, 14 * st.scale));
            const float backHeight = 38.0f * st.scale + margin;
            ImGui::BeginChild("panel", ImVec2(viewport->Size.x - 2 * margin, viewport->Size.y - margin - backHeight - 8),
                ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
            ImGui::PushFont(st.buttonFont);
            ImGui::TextUnformatted(title);
            ImGui::PopFont();
            ImGui::Spacing();
            if (st.viewChanged)
                ImGui::SetKeyboardFocusHere();
            contents(st, forcedTab);
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(2);

            ImGui::SetCursorPos(ImVec2(margin, viewport->Size.y - backHeight + 4));
            if (MenuButton(st, "Back", 160.0f * st.scale) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
                SetView(st, View::Main);
        }

        void ConfigureContents(State& st, int forcedTab)
        {
            if (ImGui::BeginTabBar("configure"))
            {
                using TabFn = void (*)(State&);
                constexpr std::pair<const char*, TabFn> kTabs[] = {
                    { "Graphics", GraphicsTab }, { "Display", DisplayTab }, { "Patches", PatchesTab },
                    { "Advanced", AdvancedTab } };
                for (int i = 0; i < int(std::size(kTabs)); i++)
                {
                    const ImGuiTabItemFlags flags = forcedTab == i ? ImGuiTabItemFlags_SetSelected : 0;
                    if (ImGui::BeginTabItem(kTabs[i].first, nullptr, flags))
                    {
                        ImGui::BeginChild("tab");
                        kTabs[i].second(st);
                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }
                }
                ImGui::EndTabBar();
            }
        }

        void ModsContents(State& st, int)
        {
            ModsList(st);
        }

        void ApplyStyle(float scale)
        {
            ImGui::StyleColorsDark();
            ImGuiStyle& style = ImGui::GetStyle();
            style.WindowPadding = ImVec2(18, 14);
            style.FramePadding = ImVec2(10, 6);
            style.ItemSpacing = ImVec2(10, 8);
            style.FrameRounding = 3;
            style.TabRounding = 3;
            style.GrabRounding = 3;
            style.ScrollbarRounding = 3;
            style.WindowBorderSize = 0;
            style.ChildBorderSize = 1;
            style.ScaleAllSizes(scale);

            // Dark warm brown with the buttons' red and gold.
            ImVec4* c = style.Colors;
            const ImVec4 accent(0.40f, 0.15f, 0.09f, 1.0f), accentHover(0.53f, 0.21f, 0.12f, 1.0f),
                accentActive(0.62f, 0.26f, 0.15f, 1.0f), gold(0.69f, 0.56f, 0.32f, 1.0f);
            c[ImGuiCol_Text] = ImVec4(0.93f, 0.89f, 0.82f, 1.0f);
            c[ImGuiCol_TextDisabled] = ImVec4(0.60f, 0.55f, 0.50f, 1.0f);
            c[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
            c[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.06f, 0.05f, 0.98f);
            c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.12f, 0.10f, 1.0f);
            c[ImGuiCol_FrameBgHovered] = ImVec4(0.23f, 0.17f, 0.13f, 1.0f);
            c[ImGuiCol_FrameBgActive] = ImVec4(0.29f, 0.20f, 0.15f, 1.0f);
            c[ImGuiCol_Button] = accent;
            c[ImGuiCol_ButtonHovered] = accentHover;
            c[ImGuiCol_ButtonActive] = accentActive;
            c[ImGuiCol_Header] = ImVec4(0.40f, 0.15f, 0.09f, 0.6f);
            c[ImGuiCol_HeaderHovered] = ImVec4(0.53f, 0.21f, 0.12f, 0.8f);
            c[ImGuiCol_HeaderActive] = accentActive;
            c[ImGuiCol_Tab] = ImVec4(0.14f, 0.10f, 0.08f, 1.0f);
            c[ImGuiCol_TabHovered] = accentHover;
            c[ImGuiCol_TabSelected] = accent;
            c[ImGuiCol_TabSelectedOverline] = gold;
            c[ImGuiCol_CheckMark] = ImVec4(0.91f, 0.77f, 0.47f, 1.0f);
            c[ImGuiCol_SliderGrab] = gold;
            c[ImGuiCol_SliderGrabActive] = ImVec4(0.91f, 0.77f, 0.47f, 1.0f);
            c[ImGuiCol_Separator] = ImVec4(0.45f, 0.36f, 0.22f, 0.6f);
            c[ImGuiCol_NavCursor] = ImVec4(0.91f, 0.77f, 0.47f, 1.0f);
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
        SDL_Window* window = SDL_CreateWindow("Dragon Age: Origins - Launcher", int(900 * scale), int(600 * scale),
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

        // The vendored ImGui has no built-in font; use the system fonts (Proton has Arial
        // and Times New Roman look-alikes).
        static const char* const kRegular[] = { "C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\arial.ttf",
            nullptr };
        static const char* const kBold[] = { "C:\\Windows\\Fonts\\seguisb.ttf", "C:\\Windows\\Fonts\\segoeuib.ttf",
            "C:\\Windows\\Fonts\\arialbd.ttf", nullptr };
        static const char* const kSerif[] = { "C:\\Windows\\Fonts\\palab.ttf", "C:\\Windows\\Fonts\\georgiab.ttf",
            "C:\\Windows\\Fonts\\timesbd.ttf", "C:\\Windows\\Fonts\\times.ttf", nullptr };
        State st;
        st.scale = scale;
        if (!LoadFont(kRegular, 18.0f * scale))
        {
            LOGE("Launcher: no system font found; starting the game");
            ImGui::DestroyContext();
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
            return true;
        }
        st.buttonFont = LoadFont(kBold, 21.0f * scale);
        if (!st.buttonFont)
            st.buttonFont = io.Fonts->Fonts[0];
        st.logoFont = LoadFont(kSerif, 58.0f * scale);
        st.logoSmallFont = st.logoFont ? LoadFont(kSerif, 44.0f * scale) : nullptr;
        if (!st.logoFont || !st.logoSmallFont)
            st.logoFont = st.logoSmallFont = st.buttonFont;

        ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
        ImGui_ImplSDLRenderer3_Init(renderer);

        st.gameRoot = gameRoot;
        st.userRoot = userRoot;
        st.modsRoot = mods::ModsRoot(gameRoot);
        st.settings = ReadSettings(userRoot);
        st.mods = mods::Scan(st.modsRoot, st.settings.mods);
        if (const char* dir = getenv("DAO_LAUNCHER_SCREENSHOTS"))
            st.screenshotDir = dir;
        {
            const std::vector<uint8_t> art = LoadArt(gameRoot, st.artWidth, st.artHeight);
            if (!art.empty())
            {
                st.art = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, st.artWidth,
                    st.artHeight);
                if (st.art)
                    SDL_UpdateTexture(st.art, nullptr, art.data(), st.artWidth * 4);
            }
            if (!st.art)
                LOGW("Launcher: no splash.dds art; plain background");
        }

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

            // Screenshot mode: 10 frames per shot (main, the configure tabs, mods),
            // captured on the last one.
            constexpr int kFramesPerShot = 10;
            constexpr int kShots = 6;
            const int shot = st.screenshotDir.empty() ? -1 : st.screenshotFrame / kFramesPerShot;
            const bool shotStart = shot >= 0 && st.screenshotFrame % kFramesPerShot == 0;
            const bool shotNow = shot >= 0 && st.screenshotFrame % kFramesPerShot == kFramesPerShot - 1;
            int forcedTab = -1;
            if (shotStart)
            {
                SetView(st, shot == 0 ? View::Main : shot == kShots - 1 ? View::Mods : View::Configure);
                forcedTab = shot >= 1 && shot <= 4 ? shot - 1 : -1;
            }
            if (shot >= kShots)
                st.quit = true;

            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(viewport->WorkPos);
            ImGui::SetNextWindowSize(viewport->WorkSize);
            ImGui::Begin("launcher", nullptr,
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                    ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground);
            const View view = st.view;
            const bool viewChanged = st.viewChanged;
            DrawBackground(st, view == View::Main ? 0.0f : 0.55f);
            switch (view)
            {
            case View::Main:
                DrawLogo(st);
                MainView(st);
                break;
            case View::Configure:
                PanelView(st, "Configure", forcedTab, ConfigureContents);
                break;
            case View::Mods:
                PanelView(st, "Mods", -1, ModsContents);
                break;
            }
            // Focus moves to the new view's first item in the frame it's shown.
            if (viewChanged && st.view == view)
                st.viewChanged = false;
            ImGui::End();

            ImGui::Render();
            SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
            SDL_SetRenderDrawColor(renderer, 12, 9, 8, 255);
            SDL_RenderClear(renderer);
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
            if (shotNow)
            {
                if (SDL_Surface* surface = SDL_RenderReadPixels(renderer, nullptr))
                {
                    const auto path = st.screenshotDir / fmt::format("launcher_{}.bmp", shot);
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
        if (st.art)
            SDL_DestroyTexture(st.art);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
        return st.play;
    }
}
