#include "stdafx.h"
#include "launcher/launcher.h"
#include "launcher/game_art.h"
#include "launcher/updater.h"

#include <shellapi.h>

#include <future>
#include <ranges>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_sdlrenderer3.h>

#include "content/install.h"
#include "mods/convert/convert.h"
#include "mods/mod_install.h"
#include "mods/mods.h"
#include "text.h"
#include "patches/patches.h"
#include "settings.h"
#include "version.h"

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
        constexpr const char* kScales[] = { "1x - 1280x720", "2x - 2560x1440", "3x - 3840x2160 (4K)",
            "4x - 5120x2880" };
        constexpr const char* kUpscalers[] = { "Bilinear", "FidelityFX CAS", "FidelityFX FSR 1" };
        constexpr const char* kTemporalUpscalers[] = { "Off", "Auto (FSR 4 where supported, else FSR 3.1)",
            "FidelityFX FSR 4", "FidelityFX FSR 3.1" };
        // The names of kAnisotropicLevels.
        constexpr const char* kAnisotropicNames[] = { "Game default", "2x", "4x", "8x", "16x" };
        static_assert(std::size(kAnisotropicNames) == std::size(kAnisotropicLevels));

        // The game's menus: cream serif text, gold edges, dark warm panels.
        constexpr ImU32 kText = IM_COL32(236, 226, 204, 255);
        constexpr ImU32 kTextDim = IM_COL32(190, 178, 154, 255);
        constexpr ImU32 kTextOff = IM_COL32(120, 110, 96, 255);
        constexpr ImU32 kGold = IM_COL32(176, 146, 92, 255);
        constexpr ImU32 kGoldBright = IM_COL32(232, 204, 140, 255);
        constexpr ImU32 kShadow = IM_COL32(0, 0, 0, 200);
        constexpr ImU32 kSky = IM_COL32(26, 16, 9, 255);
        // Messages about something wrong (no game data, a failed download).
        constexpr ImU32 kError = IM_COL32(242, 115, 89, 255);
        // A mod's warnings: what may not work with it.
        constexpr ImU32 kWarning = IM_COL32(236, 186, 92, 255);

        // The game's screens are laid out on a 1280x720 stage.
        constexpr float kStageWidth = 1280.0f, kStageHeight = 720.0f;

        // A sprite where a movie places it: on the stage (the logo, the Options screen)
        // or in the title screen's background clip.
        struct Layer
        {
            const char* sprite;
            float x, y;
            float alpha = 1.0f;
        };

        // The title screen (startmenu2.gfx). Its background is a clip at kSceneX,
        // kSceneY on the stage, scaled by kSceneScale; the game shows it kSceneDrop
        // lower (its title-safe anchoring), so the scene's layers are all drawn that
        // much lower, but not the logo or the menu. Positions are on the stage unless
        // they're in the clip.
        constexpr float kSceneScale = 2.0f / 3.0f, kSceneX = 0.1f, kSceneY = -40.2f, kSceneDrop = 13.0f;
        // The clouds: a strip of tiles (a, b, c, a; positions in the clip) scrolling
        // left, wrapping every kCloudLoop pixels in 900 s. Over them cloud2 is
        // multiplied into the sky at kCloudShadeOpacity: two copies kCloudShadeLoop
        // apart (with a gap between, as the movie has), wrapping every 200 s
        // (StartMenu.CLOUD*_ANIM_*).
        constexpr const char* kClouds[] = { "dxt1_cloud1a.jpg", "dxt1_cloud1b.jpg", "dxt1_cloud1c.jpg",
            "dxt1_cloud1a.jpg" };
        constexpr float kCloudTile = 2500.0f, kCloudLoop = 7498.0f, kCloudSpeed = kCloudLoop / 900.0f;
        constexpr const char* kCloudShade = "dxt1_cloud2.png";
        constexpr float kCloudShadeScale = 1.125f, kCloudShadeStart = 865.0f, kCloudShadeLoop = 3150.0f,
                        kCloudShadeSpeed = kCloudShadeLoop / 200.0f, kCloudShadeOpacity = 0.7f;
        // The layers on the stage, as drawn by DrawScene (on top of these come the
        // haze, the vignette, the mist, the embers and the fire glow).
        constexpr Layer kSunGlow = { "startmenu2_I13C", 112.0f, 245.3f, 0.8f };
        constexpr Layer kHills = { "startmenu2_I13F", 205.4f, 376.7f };
        constexpr Layer kMountains = { "startmenu2_I145", 38.7f, 249.9f };
        // The cliff and the rocks in front.
        constexpr Layer kForeground[] = {
            { "startmenu2_I14A", 702.1f, -40.2f },
            { "startmenu2_I149", 0.1f, 284.5f },
            { "startmenu2_I148", 297.4f, 573.8f },
        };
        // The swords and the ground they're in.
        constexpr Layer kSwords[] = {
            { "startmenu2_I15F", 271.4f, 17.8f },
            { "startmenu2_I15E", 0.1f, 521.1f },
        };
        constexpr Layer kSmoke = { "startmenu2_I165", -201.9f, 145.2f };
        // A soft glow: drifting through the haze tinted kHazeColor, and over the fires
        // reddening.
        constexpr const char* kGlow = "startmenu2_I142";
        constexpr ImU32 kHazeColor = IM_COL32(204, 153, 102, 255);
        constexpr float kFireX = 0.1f, kFireY = 276.0f;
        constexpr const char* kMist = "startmenu2_I150";
        constexpr float kMistX = -121.3f, kMistY = 426.7f;
        constexpr const char* kEmbers = "startmenu2_I161";
        constexpr float kEmbersX = 0.1f, kEmbersY = 432.0f;

        // The scene's tweens: the movie's 60 fps timelines reduced to linear keys
        // (seconds, then up to two values; one-value tweens leave b 0). Each starts at
        // 0 and loops at its last key.
        struct Key
        {
            float time, a, b;
        };
        // The haze glows' positions in the clip; the first passes across and restarts.
        constexpr Key kHaze1[] = { { 0.0f, -1197, 465 }, { 24.8f, 1461, 465 } };
        constexpr Key kHaze2[] = { { 0.0f, 811, 548 }, { 5.32f, 698, 514 }, { 15.25f, 722, 571 },
            { 24.8f, 811, 548 } };
        // The vignette's opacity: flickering, as by firelight.
        constexpr Key kVignette[] = { { 0.0f, 1.0f }, { 3.17f, 0.0f }, { 3.92f, 0.52f }, { 4.42f, 0.672f },
            { 4.72f, 0.441f }, { 4.9f, 0.238f }, { 5.12f, 0.91f }, { 5.28f, 0.379f }, { 5.47f, 0.711f },
            { 5.63f, 0.43f }, { 9.62f, 1.0f }, { 9.9f, 0.328f }, { 9.92f, 0.441f }, { 10.07f, 0.211f },
            { 13.7f, 0.0f }, { 13.95f, 0.809f }, { 14.15f, 0.969f }, { 14.35f, 0.789f }, { 14.52f, 1.0f },
            { 24.8f, 1.0f } };
        // The embers' opacity.
        constexpr Key kEmbersAlpha[] = { { 0.0f, 0.0f }, { 1.33f, 0.578f }, { 1.38f, 1.0f }, { 1.45f, 0.648f },
            { 1.58f, 0.172f }, { 2.5f, 0.648f }, { 2.55f, 0.129f }, { 2.62f, 1.0f }, { 2.75f, 0.379f },
            { 11.97f, 0.309f }, { 12.22f, 0.309f }, { 12.25f, 1.0f }, { 12.37f, 0.559f }, { 12.4f, 0.809f },
            { 12.67f, 0.488f }, { 16.62f, 0.488f }, { 19.27f, 0.0f } };
        // The fire glow's colour: its own times a, plus red b.
        constexpr Key kFireTint[] = { { 0.0f, 1.0f, 0.0f }, { 11.65f, 0.641f, 73.0f }, { 27.08f, 1.0f, 0.0f } };
        // The mist: its x in its clip, drifting right, and its opacity.
        constexpr Key kMistDrift[] = { { 0.0f, -48, 0 }, { 1.52f, 0, 1 }, { 165.88f, 629, 1 },
            { 174.4f, 946, 0 } };

        // Over the background, on the stage: a warm gradient multiplied in at
        // kWarmOpacity, opaque kWarmTop at y kWarmTopY to transparent kWarmBottom at
        // kWarmBottomY; and a shadow over the bottom, from transparent at
        // kEdgeShadowTop to black at kEdgeShadowBottom.
        constexpr float kWarmOpacity = 0.7f, kWarmTopY = -76.9f, kWarmBottomY = 844.8f;
        constexpr float kWarmTop[3] = { 244, 221, 11 }, kWarmBottom[3] = { 204, 102, 0 };
        constexpr float kEdgeShadowTop = 290.4f, kEdgeShadowBottom = 794.7f;
        // The vignette over the landscape, in the clip: black outside a circle, from
        // transparent at kVignetteInner from its centre to opaque at kVignetteOuter.
        constexpr float kVignetteWidth = 1920.0f, kVignetteHeight = 1200.0f, kVignetteInner = 950.2f,
                        kVignetteOuter = 1132.2f;

        // The logo where the game shows it (the movie's anchoring moves it), "ORIGINS"
        // right-aligned under it.
        constexpr Layer kLogo = { "startmenu2_I11A", 642.9f, 109.0f };
        constexpr const char* kOrigins = "startmenu2_I10F";
        constexpr float kOriginsRight = 1178.8f, kOriginsY = 212.0f;
        // Without the logo's art: the title's size, its top below the logo's, and the
        // subtitle's size relative to it.
        constexpr float kTitleTextSize = 64.0f, kTitleDrop = 20.0f, kSubtitleScale = 0.45f;
        // The menu: items right-aligned at kMenuRight, clickable from kMenuHitLeft; the
        // game's highlight bars (rolled over, pressed) behind the selected item, their
        // left end at kMenuBarX and middle at the item's.
        constexpr const char* kMenuBar = "startmenu2_IF4";
        constexpr const char* kMenuBarDown = "startmenu2_I101";
        constexpr float kMenuBarX = 766.0f, kMenuBarHalfHeight = 31.0f, kMenuHitLeft = 846.0f;
        constexpr float kMenuRight = 1170.0f, kMenuTop = 385.0f, kMenuStep = 34.0f, kMenuTextSize = 24.0f;
        // Without the bars' art: a band fading in from the left, this high and dark.
        constexpr float kMenuBandHalfHeight = 14.0f, kMenuBandOpacity = 0.8f;

        // The textures made from the game's art for the scene's blending, by name.
        constexpr const char* kGlowWhite = "glow (white)";
        constexpr const char* kGlowAdd = "glow (additive)";
        constexpr const char* kWarmOverlay = "warm overlay";
        constexpr const char* kVignetteMask = "vignette";

        // The Options screen (optionsmenu.gfx).
        constexpr const char* kPanelBackground = "OptionsMenu_I200";
        constexpr float kPanelBackgroundAlpha = 0.92f;
        // How much the title screen behind it is darkened.
        constexpr float kPanelSceneDim = 0.45f;
        constexpr Layer kHeaderBar = { "OptionsMenu_I1D6", 486.6f, 38.5f };
        constexpr Layer kHeaderFlourish = { "OptionsMenu_I1DE", 466.6f, 23.0f, 0.3f };
        constexpr float kHeaderTitleX = 1000.0f, kHeaderTitleY = 71.5f, kHeaderTitleSize = 34.0f;
        // The tabs: text at kTabTextX; the selected one's highlight the bar mirrored,
        // its curved end at kTabBarRight, its middle at the tab's; LB and RB under them.
        constexpr const char* kTabBar = "OptionsMenu_I1F7";
        constexpr float kTabBarRight = 547.3f, kTabBarHalfHeight = 31.0f, kTabTop = 89.0f, kTabStep = 34.0f,
                        kTabTextX = 258.0f, kTabTextSize = 22.0f;
        constexpr float kBumperGap = 40.0f, kBumperSize = 26.0f, kBumperStep = 62.0f, kBumperOpacity = 0.8f;
        constexpr const char* kSectionBar = "OptionsMenu_I17E";
        // The footer: a bar stretched under the button hints.
        constexpr const char* kFooterBar = "OptionsMenu_I203";
        constexpr float kFooterLeft = 232.0f, kFooterRight = 1049.0f, kFooterTop = 629.0f, kFooterHintY = 632.0f;
        // The contents between the header and the footer.
        constexpr float kContentsTop = 112.0f, kContentsBottom = 612.0f, kContentsRight = 1240.0f;
        constexpr float kConfigureLeft = 575.0f, kModsLeft = 80.0f;

        constexpr const char* kButtonA = "XBoxGamepad_BottomAction";
        constexpr const char* kButtonB = "XBoxGamepad_RightAction";
        constexpr const char* kButtonY = "XBoxGamepad_TopAction";
        constexpr const char* kButtonLB = "XBoxGamepad_LeftBumper";
        constexpr const char* kButtonRB = "XBoxGamepad_RightBumper";
        // Button hints: the icon's height, the gap to the label, the label's size.
        constexpr float kHintIcon = 30.0f, kHintGap = 8.0f, kHintTextSize = 19.0f;

        constexpr std::string_view kAtlases[] = { "atl_startmenu2_dxt5", "atl_startmenu2_dxt1",
            "atl_optionsmen_dxt5", "atl_shared_dxt5" };

        enum class View { Main, Configure, Mods, Update };
        // Graphics, Display, Patches, Advanced.
        constexpr int kConfigureTabs = 4;
        constexpr float kFadeSeconds = 0.2f;

        struct Texture
        {
            SDL_Texture* texture = nullptr;
            float width = 0, height = 0;
        };

        // A line of release notes, ready to show.
        struct NoteLine
        {
            enum class Kind
            {
                Blank,
                Heading,
                Text,
                Item,
            } kind;
            // Nesting (indentation steps).
            int level = 0;
            std::string text;
        };

        // What reading the mod list gives.
        struct ModScan
        {
            std::shared_ptr<const mods::GameFiles> gameFiles;
            std::vector<mods::ModInfo> mods;
        };

        // A conversion's outcome, and the mod described again after it.
        struct Converted
        {
            mods::ConvertResult result;
            mods::ModInfo mod;
        };

        struct State
        {
            fs::path gameRoot, userRoot, modsRoot;
            Settings settings;
            std::vector<mods::ModInfo> mods;
            int selectedMod = -1;
            View view = View::Main;
            bool viewChanged = true;
            double viewStart = 0;
            int configureTab = 0;
            // Whether default.xex is in gameRoot (checked once, at startup).
            bool gameFound = false;
            std::unique_ptr<Updater> updater;
            // Mods dropped on the window: the paths of the drop under way, the
            // install running in the background, and how the last one went.
            std::vector<fs::path> dropped;
            std::future<mods::InstallResult> modInstall;
            std::stop_source modStop;
            std::vector<mods::InstallResult::Line> modMessages;
            // A PC mod being converted in the background (its folder), stopped with modStop.
            std::future<Converted> modConvert;
            std::string convertingMod;
            // The mod list being read in the background (at the start, and on Refresh),
            // and whether it has been (until then, the saved list isn't replaced).
            std::future<ModScan> modScan;
            bool modsRead = false;
            // The game's file names, for the mods' warnings: read once (with the mod
            // list), shared with the background work.
            std::shared_ptr<const mods::GameFiles> gameFiles;
            // Scroll the mod list to the selected mod (just added).
            bool scrollToMod = false;
            // The release the updater found and its notes, taken from it once.
            std::optional<Release> release;
            std::vector<NoteLine> releaseNotes;
            // The menus' serif fonts: the title screen's items, titles, section
            // headings, help.
            ImFont* menuFont = nullptr;
            ImFont* titleFont = nullptr;
            ImFont* headingFont = nullptr;
            ImFont* helpFont = nullptr;
            std::unordered_map<std::string, Texture> art;
            float scale = 1.0f;
            // The opacity of the view's contents while it fades in.
            float fade = 1.0f;
            bool play = false;
            bool quit = false;
            // DAO_LAUNCHER_SCREENSHOTS=<dir>: show each view, save it as BMP, quit.
            fs::path screenshotDir;
            int screenshotFrame = 0;
            // Whether Escape and the gamepad's B mean Back this frame: not closing a popup,
            // ending an edit or leaving a nested child window.
            bool backKeysFree = false;
        };

        // The state, for the settings tabs' helpers.
        const State* g_state = nullptr;

        std::vector<NoteLine> ParseNotes(std::string_view notes);

        // The newer release the updater found, if any (copied once: it doesn't
        // change after).
        const Release* FoundRelease(State& st)
        {
            if (!st.release && st.updater->HasRelease())
            {
                st.release = st.updater->GetRelease();
                st.releaseNotes = ParseNotes(st.release->notes);
            }
            return st.release ? &*st.release : nullptr;
        }


        // Where the game's screens go in the window: the scene covers it; the menus fit
        // in it, anchored to its sides.
        struct Layout
        {
            ImVec2 pos, size;
            float cover = 1, coverX = 0, coverY = 0;
            float fit = 1, fitY = 0;

            explicit Layout(const ImGuiViewport* viewport) : pos(viewport->Pos), size(viewport->Size)
            {
                cover = std::max(size.x / kStageWidth, size.y / kStageHeight);
                coverX = (size.x - kStageWidth * cover) * 0.5f;
                coverY = (size.y - kStageHeight * cover) * 0.5f;
                fit = std::min(size.x / kStageWidth, size.y / kStageHeight);
                fitY = (size.y - kStageHeight * fit) * 0.5f;
            }

            ImVec2 Max() const { return ImVec2(pos.x + size.x, pos.y + size.y); }
            ImVec2 Scene(float x, float y) const
            {
                return ImVec2(pos.x + coverX + x * cover, pos.y + coverY + y * cover);
            }
            float Y(float y) const { return pos.y + fitY + y * fit; }
            ImVec2 Left(float x, float y) const { return ImVec2(pos.x + x * fit, Y(y)); }
            ImVec2 Right(float x, float y) const { return ImVec2(pos.x + size.x - (kStageWidth - x) * fit, Y(y)); }
            ImVec2 Center(float x, float y) const
            {
                return ImVec2(pos.x + size.x * 0.5f + (x - kStageWidth * 0.5f) * fit, Y(y));
            }
        };

        ImU32 WithAlpha(ImU32 color, float alpha)
        {
            const float a = float(color >> IM_COL32_A_SHIFT & 0xFF) * std::clamp(alpha, 0.0f, 1.0f);
            return (color & ~IM_COL32_A_MASK) | ImU32(a + 0.5f) << IM_COL32_A_SHIFT;
        }

        const Texture* Art(const State& st, const char* name)
        {
            const auto it = st.art.find(name);
            return it != st.art.end() ? &it->second : nullptr;
        }

        // A sprite with its top left at `at`, `scale` window pixels per sprite pixel;
        // mirrored, it extends left of `at` instead.
        void DrawTinted(ImDrawList* draw, const Texture& art, ImVec2 at, float scale, ImU32 tint, bool mirrored = false)
        {
            const float w = art.width * scale, h = art.height * scale;
            const ImVec2 min(mirrored ? at.x - w : at.x, at.y), max(min.x + w, min.y + h);
            draw->AddImage(ImTextureID(uintptr_t(art.texture)), min, max, ImVec2(mirrored ? 1.0f : 0.0f, 0),
                ImVec2(mirrored ? 0.0f : 1.0f, 1), tint);
        }

        void DrawSprite(ImDrawList* draw, const Texture& art, ImVec2 at, float scale, float alpha,
            bool mirrored = false)
        {
            DrawTinted(draw, art, at, scale, WithAlpha(IM_COL32_WHITE, alpha), mirrored);
        }

        // A sprite stretched over a rectangle.
        void DrawStretched(ImDrawList* draw, const Texture& art, ImVec2 min, ImVec2 max, float alpha)
        {
            draw->AddImage(ImTextureID(uintptr_t(art.texture)), min, max, ImVec2(0, 0), ImVec2(1, 1),
                WithAlpha(IM_COL32_WHITE, alpha));
        }

        void DrawLayer(ImDrawList* draw, const State& st, const Layout& layout, const Layer& layer)
        {
            if (const Texture* art = Art(st, layer.sprite))
                DrawSprite(draw, *art, layout.Scene(layer.x, layer.y + kSceneDrop), kSceneScale * layout.cover,
                    layer.alpha);
        }

        // A tween's values `t` seconds into its loops. (The first key is at 0, so the
        // key after the time in the loop always has one before it.)
        ImVec2 Tween(std::span<const Key> keys, double t)
        {
            const float loop = float(std::fmod(t, double(keys.back().time)));
            const Key* next =
                std::ranges::upper_bound(keys.data() + 1, keys.data() + keys.size() - 1, loop, {}, &Key::time);
            const Key& prev = next[-1];
            const float f = (loop - prev.time) / (next->time - prev.time);
            return ImVec2(prev.a + (next->a - prev.a) * f, prev.b + (next->b - prev.b) * f);
        }

        // Text with the game's drop shadow, faded with the view.
        void ShadowText(ImDrawList* draw, const State& st, ImFont* font, float size, ImVec2 at, ImU32 color,
            const char* text)
        {
            const float offset = std::max(1.0f, size / 16.0f);
            const float alpha = float(color >> IM_COL32_A_SHIFT & 0xFF) / 255.0f * st.fade;
            draw->AddText(font, size, ImVec2(at.x + offset, at.y + offset), WithAlpha(kShadow, alpha), text);
            draw->AddText(font, size, at, WithAlpha(color, st.fade), text);
        }

        // The title screen, animated as the game's: the clouds drifting, haze passing
        // over the landscape, the vignette flickering as by firelight, the embers and
        // the fire glowing, the mist drifting. `dim` darkens it behind the other views.
        void DrawScene(const State& st, const Layout& layout, float dim)
        {
            ImDrawList* draw = ImGui::GetBackgroundDrawList();
            draw->AddRectFilled(layout.pos, layout.Max(), kSky);
            // In double: the scene runs for as long as the launcher is open.
            const double t = ImGui::GetTime();
            const float scale = kSceneScale * layout.cover;
            // A point of the background.
            auto at = [&](float x, float y) {
                return layout.Scene(kSceneX + x * kSceneScale, kSceneY + kSceneDrop + y * kSceneScale);
            };

            const float cloudX = -float(std::fmod(t * kCloudSpeed, double(kCloudLoop)));
            for (size_t i = 0; i < std::size(kClouds); i++)
            {
                if (const Texture* cloud = Art(st, kClouds[i]))
                    DrawSprite(draw, *cloud, at(cloudX + kCloudTile * float(i), 0), kCloudTile / cloud->width * scale,
                        1.0f);
            }
            if (const Texture* shade = Art(st, kCloudShade))
            {
                // From its start, then wrapping.
                double x = kCloudShadeStart - t * kCloudShadeSpeed;
                if (x < 0.0)
                    x = -std::fmod(-x, double(kCloudShadeLoop));
                for (const float copy : { 0.0f, kCloudShadeLoop })
                    DrawSprite(draw, *shade, at(float(x) + copy, 0), kCloudShadeScale * scale, 1.0f);
            }
            DrawLayer(draw, st, layout, kSunGlow);

            const Texture* glowWhite = Art(st, kGlowWhite);
            auto haze = [&](std::span<const Key> keys) {
                const ImVec2 p = Tween(keys, t);
                if (glowWhite)
                    DrawTinted(draw, *glowWhite, at(p.x, p.y), scale, kHazeColor);
            };
            DrawLayer(draw, st, layout, kHills);
            haze(kHaze1);
            DrawLayer(draw, st, layout, kMountains);
            haze(kHaze2);
            for (const Layer& layer : kForeground)
                DrawLayer(draw, st, layout, layer);
            if (const Texture* vignette = Art(st, kVignetteMask))
                DrawStretched(draw, *vignette, at(0, 0), at(kVignetteWidth, kVignetteHeight), Tween(kVignette, t).x);

            if (const Texture* mist = Art(st, kMist))
            {
                const ImVec2 drift = Tween(kMistDrift, t);
                DrawSprite(draw, *mist, layout.Scene(kMistX + drift.x * kSceneScale, kMistY + kSceneDrop), scale,
                    drift.y);
            }
            for (const Layer& layer : kSwords)
                DrawLayer(draw, st, layout, layer);
            if (const Texture* embers = Art(st, kEmbers))
                DrawSprite(draw, *embers, layout.Scene(kEmbersX, kEmbersY + kSceneDrop), scale,
                    Tween(kEmbersAlpha, t).x);
            DrawLayer(draw, st, layout, kSmoke);
            if (const Texture* glow = Art(st, kGlow))
            {
                // Its colour times the tint's first value, plus the second in red.
                const ImVec2 tint = Tween(kFireTint, t);
                const ImVec2 p = layout.Scene(kFireX, kFireY + kSceneDrop);
                const int shade = int(tint.x * 255.0f + 0.5f);
                DrawTinted(draw, *glow, p, scale, IM_COL32(shade, shade, shade, 255));
                if (const Texture* add = Art(st, kGlowAdd))
                    DrawTinted(draw, *add, p, scale, IM_COL32(int(tint.y + 0.5f), 0, 0, 255));
            }

            if (const Texture* warm = Art(st, kWarmOverlay))
                DrawStretched(draw, *warm, layout.Scene(0, 0), layout.Scene(kStageWidth, kStageHeight), 1.0f);
            draw->AddRectFilledMultiColor(layout.Scene(0, kEdgeShadowTop), layout.Scene(kStageWidth, kEdgeShadowBottom),
                IM_COL32_BLACK_TRANS, IM_COL32_BLACK_TRANS, IM_COL32_BLACK, IM_COL32_BLACK);
            if (dim > 0.0f)
                draw->AddRectFilled(layout.pos, layout.Max(), WithAlpha(IM_COL32_BLACK, dim));
        }

        // The game's logo at the top right; without its art, the title in capitals.
        void DrawLogo(const State& st, const Layout& layout)
        {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const Texture* logo = Art(st, kLogo.sprite);
            const Texture* origins = Art(st, kOrigins);
            if (logo && origins)
            {
                DrawSprite(draw, *logo, layout.Right(kLogo.x, kLogo.y), layout.fit, st.fade);
                DrawSprite(draw, *origins, layout.Right(kOriginsRight - origins->width, kOriginsY), layout.fit,
                    st.fade);
                return;
            }
            ImFont* font = st.titleFont;
            const float size = kTitleTextSize * layout.fit;
            const char* title = "DRAGON AGE";
            const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0, title);
            const ImVec2 at = layout.Right(kOriginsRight, kLogo.y + kTitleDrop);
            ShadowText(draw, st, font, size, ImVec2(at.x - extent.x, at.y), kText, title);
            const char* subtitle = "O R I G I N S";
            const float subtitleSize = size * kSubtitleScale;
            const ImVec2 subtitleExtent = font->CalcTextSizeA(subtitleSize, FLT_MAX, 0, subtitle);
            ShadowText(draw, st, font, subtitleSize, ImVec2(at.x - subtitleExtent.x, at.y + size), kText, subtitle);
        }

        // An animated 0..1 for an item: towards 1 while `on`.
        float Ease(ImGuiID id, bool on, float seconds = 0.12f)
        {
            float* value = ImGui::GetStateStorage()->GetFloatRef(id, 0.0f);
            const float step = ImGui::GetIO().DeltaTime / seconds;
            *value = std::clamp(*value + (on ? step : -step), 0.0f, 1.0f);
            return *value;
        }

        // A button of the game's own menus, which draw their highlight instead of
        // ImGui's navigation cursor. Hot while hovered or focused by keys or gamepad.
        bool MenuButton(const char* id, ImVec2 min, ImVec2 size, bool enabled, bool& hot)
        {
            ImGui::SetCursorScreenPos(min);
            ImGui::BeginDisabled(!enabled);
            ImGui::PushStyleColor(ImGuiCol_NavCursor, IM_COL32_BLACK_TRANS);
            const bool pressed = ImGui::InvisibleButton(id, size, ImGuiButtonFlags_EnableNav);
            ImGui::PopStyleColor();
            ImGui::EndDisabled();
            hot = enabled && (ImGui::IsItemHovered() || (ImGui::IsItemFocused() && GImGui->NavCursorVisible));
            return pressed && enabled;
        }

        // A title screen menu item: right-aligned text, the game's highlight bar behind
        // it while it's selected. Also activated with Enter or the gamepad's A.
        bool MenuItem(const State& st, const Layout& layout, const char* label, float centerY, bool enabled = true)
        {
            const float rowHeight = kMenuStep * layout.fit;
            const ImVec2 min(layout.Right(kMenuHitLeft, 0).x, layout.Y(centerY) - rowHeight * 0.5f);
            bool hot = false;
            const bool pressed =
                MenuButton(label, min, ImVec2(layout.pos.x + layout.size.x - min.x, rowHeight), enabled, hot);
            const bool down = hot && ImGui::IsItemActive();
            const float shown = Ease(ImGui::GetItemID(), hot);

            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImVec2 bar = layout.Right(kMenuBarX, centerY - kMenuBarHalfHeight);
            if (shown > 0.0f)
            {
                if (const Texture* art = Art(st, down ? kMenuBarDown : kMenuBar))
                    DrawSprite(draw, *art, bar, layout.fit, shown * st.fade);
                else
                {
                    const ImU32 dark = WithAlpha(IM_COL32_BLACK, kMenuBandOpacity * shown * st.fade);
                    draw->AddRectFilledMultiColor(ImVec2(bar.x, layout.Y(centerY - kMenuBandHalfHeight)),
                        ImVec2(layout.pos.x + layout.size.x, layout.Y(centerY + kMenuBandHalfHeight)),
                        IM_COL32_BLACK_TRANS, dark, dark, IM_COL32_BLACK_TRANS);
                }
            }
            ImFont* font = st.menuFont;
            const float size = kMenuTextSize * layout.fit;
            const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0, label);
            const ImVec2 at = layout.Right(kMenuRight, centerY);
            ShadowText(draw, st, font, size, ImVec2(at.x - extent.x, at.y - extent.y * 0.5f),
                !enabled ? kTextOff : hot ? kText : kTextDim, label);
            return pressed;
        }

        // The width of a button hint, in stage pixels.
        float HintWidth(const State& st, const char* label)
        {
            return kHintIcon + kHintGap + st.headingFont->CalcTextSizeA(kHintTextSize, FLT_MAX, 0, label).x;
        }

        // A button hint of the game's footers at `at` (its top left; its top right when
        // the label comes first): the button's icon and the label. Clickable when
        // `clickable`; returns whether it was clicked.
        bool Hint(const State& st, const Layout& layout, const char* button, const char* label, ImVec2 at,
            bool labelFirst, bool clickable)
        {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const float icon = kHintIcon * layout.fit, gap = kHintGap * layout.fit, size = kHintTextSize * layout.fit;
            ImFont* font = st.headingFont;
            const ImVec2 text = font->CalcTextSizeA(size, FLT_MAX, 0, label);
            const float width = icon + gap + text.x;
            const ImVec2 min(labelFirst ? at.x - width : at.x, at.y);
            bool pressed = false, hot = false;
            if (clickable)
            {
                ImGui::SetCursorScreenPos(min);
                ImGui::PushID(label);
                pressed = ImGui::InvisibleButton("##hint", ImVec2(width, icon));
                hot = ImGui::IsItemHovered();
                ImGui::PopID();
            }
            const float iconX = labelFirst ? min.x + text.x + gap : min.x;
            const float textX = labelFirst ? min.x : min.x + icon + gap;
            if (const Texture* art = Art(st, button))
                DrawSprite(draw, *art, ImVec2(iconX, min.y), icon / art->height, st.fade);
            else
                draw->AddCircleFilled(ImVec2(iconX + icon * 0.5f, min.y + icon * 0.5f), icon * 0.45f,
                    WithAlpha(kGold, st.fade));
            ShadowText(draw, st, font, size, ImVec2(textX, min.y + (icon - text.y) * 0.5f), hot ? kGoldBright : kText,
                label);
            return pressed;
        }

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
            ImGui::PushFont(g_state->helpFont);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", text);
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }

        // A bulleted paragraph, wrapped to the window (ImGui's BulletText doesn't wrap).
        void WrappedBullet(const char* text)
        {
            ImGui::Bullet();
            ImGui::TextWrapped("%s", text);
        }

        // A section heading: the Options screen's gold bar with the title on it.
        void Section(const char* title)
        {
            const State& st = *g_state;
            // Space above; the bar's height (in lines) and its text's inset; without the
            // bar's art, the width of its gold fade.
            constexpr float kSpaceAbove = 6.0f, kBarLines = 1.6f, kInset = 10.0f, kFallbackWidth = 360.0f;
            ImGui::Dummy(ImVec2(0, kSpaceAbove * st.scale));
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImFont* font = st.headingFont;
            const float height = font->FontSize * kBarLines, inset = kInset * st.scale;
            ImDrawList* draw = ImGui::GetWindowDrawList();
            if (const Texture* bar = Art(st, kSectionBar))
                DrawStretched(draw, *bar, ImVec2(at.x - inset, at.y),
                    ImVec2(at.x - inset + bar->width * height / bar->height, at.y + height), st.fade);
            else
                draw->AddRectFilledMultiColor(at, ImVec2(at.x + kFallbackWidth * st.scale, at.y + height),
                    WithAlpha(kGold, 0.6f * st.fade), IM_COL32_BLACK_TRANS, IM_COL32_BLACK_TRANS,
                    WithAlpha(kGold, 0.6f * st.fade));
            ShadowText(draw, st, font, font->FontSize, ImVec2(at.x + inset, at.y + (height - font->FontSize) * 0.5f),
                kText, title);
            ImGui::Dummy(ImVec2(0, height));
        }

        // The width of the pickers and sliders.
        float RowWidth()
        {
            return std::min(ImGui::GetContentRegionAvail().x - 4 * g_state->scale, 460.0f * g_state->scale);
        }

        void Label(const char* text)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kTextDim));
            ImGui::TextUnformatted(text);
            ImGui::PopStyleColor();
        }

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

        bool PatchOn(const Settings& settings, const patches::Patch& patch)
        {
            const auto it = settings.patches.find(patch.id);
            return it != settings.patches.end() ? it->second : patch.defaultEnabled;
        }

        void SetView(State& st, View view)
        {
            st.view = view;
            st.viewChanged = true;
            st.viewStart = ImGui::GetTime();
        }

        // Mod folders' names compare as Windows' do, ignoring case.
        bool SameFolder(const std::string& a, const std::string& b)
        {
            return _stricmp(a.c_str(), b.c_str()) == 0;
        }

        bool AddingMods(const State& st)
        {
            return st.modInstall.valid();
        }

        bool ConvertingMod(const State& st)
        {
            return st.modConvert.valid();
        }

        // Mods being added or converted: the game waits for them, as do other changes.
        bool ModsBusy(const State& st)
        {
            return AddingMods(st) || ConvertingMod(st);
        }

        bool ReadingMods(const State& st)
        {
            return st.modScan.valid();
        }

        // Reads the mod list (in the saved order) in the background: every file of
        // every mod is looked at, and the game's file names read the first time.
        void ReadMods(State& st, std::vector<ModState> saved)
        {
            st.modScan = std::async(std::launch::async,
                [root = st.modsRoot, gameRoot = st.gameRoot, game = st.gameFiles, saved = std::move(saved),
                    stop = st.modStop.get_token()]() mutable {
                    if (!game)
                        game = std::make_shared<const mods::GameFiles>(mods::GameFiles::Read(gameRoot, stop));
                    return ModScan{ game, mods::Scan(root, saved, game.get()) };
                });
        }

        void AddDroppedMods(State& st);

        // When the mod list has been read: shown, and a drop that waited for it added.
        void FinishReadingMods(State& st)
        {
            if (!ReadingMods(st) || st.modScan.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                return;
            try
            {
                ModScan scan = st.modScan.get();
                st.gameFiles = std::move(scan.gameFiles);
                st.mods = std::move(scan.mods);
                st.modsRead = true;
            }
            catch (const std::exception& error)
            {
                LOGE("Mods: reading the mods failed: {}", error.what());
                st.modMessages = { { "Can't read the mods: something went wrong (see dao.log).", true } };
            }
            st.selectedMod = -1;
            if (!st.dropped.empty() && !st.play && !st.quit)
                AddDroppedMods(st);
        }

        // Adds the mods just dropped on the window, in the background (copying and
        // unpacking can take a while), and shows the Mods page.
        void AddDroppedMods(State& st)
        {
            // (After the list has been read, which they're added to.)
            if (ReadingMods(st))
            {
                if (!st.dropped.empty())
                    SetView(st, View::Mods);
                return;
            }
            std::vector<fs::path> dropped = std::move(st.dropped);
            st.dropped.clear();
            if (dropped.empty())
                return;
            SetView(st, View::Mods);
            if (ModsBusy(st))
            {
                st.modMessages = { { "Still working on the last mods: drop these again when it's done.", true } };
                return;
            }
            st.modMessages.clear();
            // Xbox 360 packages (DLC, the title update) are installed; the rest are mods.
            st.modInstall = std::async(std::launch::async,
                [root = st.modsRoot, gameRoot = st.gameRoot, userRoot = st.userRoot, dropped = std::move(dropped),
                    stop = st.modStop.get_token(), game = st.gameFiles]() mutable {
                    const std::vector<content::DroppedPackage> packages = content::TakePackages(dropped);
                    std::vector<mods::InstallResult::Line> installed;
                    content::InstallPackages(gameRoot, userRoot, packages, stop, installed);
                    mods::InstallResult result;
                    if (!dropped.empty())
                        result = mods::Install(root, dropped, stop, game.get());
                    result.lines.insert(result.lines.begin(), installed.begin(), installed.end());
                    return result;
                });
        }

        // When the background install is done: the new mods at the end of the list
        // (the last one selected) and the outcome.
        void FinishAddingMods(State& st)
        {
            if (!AddingMods(st) || st.modInstall.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                return;
            mods::InstallResult result;
            try
            {
                result = st.modInstall.get();
            }
            catch (const std::exception& error)
            {
                LOGE("Mods: adding failed: {}", error.what());
                result.lines.push_back({ "Can't add the mods: something went wrong (see dao.log).", true });
            }
            for (mods::ModInfo& mod : result.mods)
            {
                // (A folder deleted while the launcher was open may have the same name.)
                std::erase_if(st.mods, [&](const mods::ModInfo& old) { return SameFolder(old.folder, mod.folder); });
                st.mods.push_back(std::move(mod));
            }
            if (!result.mods.empty())
            {
                st.selectedMod = int(st.mods.size()) - 1;
                st.scrollToMod = true;
            }
            mods::FindOverlaps(st.mods);
            st.modMessages = std::move(result.lines);
        }

        // Converts the selected mod, made for the PC version, in the background (and
        // describes it again there: that reads every file).
        void ConvertSelectedMod(State& st)
        {
            const mods::ModInfo& mod = st.mods[st.selectedMod];
            st.convertingMod = mod.folder;
            st.modMessages.clear();
            st.modConvert = std::async(std::launch::async,
                [folder = st.modsRoot / FromUtf8(mod.folder), root = st.gameRoot, stop = st.modStop.get_token(),
                    game = st.gameFiles] {
                    Converted out{ mods::ConvertPcMod(folder, root, stop, game.get()), {} };
                    out.mod = mods::Describe(folder, game.get());
                    return out;
                });
        }

        // When the background conversion is done: the mod as it is now, and the outcome.
        void FinishConverting(State& st)
        {
            if (!ConvertingMod(st) || st.modConvert.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                return;
            Converted converted;
            bool described = false;
            try
            {
                converted = st.modConvert.get();
                described = true;
            }
            catch (const std::exception& error)
            {
                LOGE("Mods: converting failed: {}", error.what());
                converted.result.problems.push_back("Something went wrong (see dao.log).");
            }
            const mods::ConvertResult& result = converted.result;
            const std::string folder = std::exchange(st.convertingMod, {});
            const auto mod = std::ranges::find_if(st.mods,
                [&](const mods::ModInfo& m) { return SameFolder(m.folder, folder); });
            const std::string name = mod != st.mods.end() ? mod->name : folder;
            if (mod != st.mods.end() && described)
            {
                const bool enabled = mod->enabled;
                *mod = std::move(converted.mod);
                mod->enabled = enabled;
                mods::FindOverlaps(st.mods);
            }

            // The files left out: the first few (all are in dao.log).
            constexpr size_t kLeftOutShown = 8;
            auto& lines = st.modMessages;
            lines.clear();
            if (result.converted)
                lines.push_back({ fmt::format("Converted \"{}\": {} file{} written. Its PC files are kept in "
                                              "mods/.originals/{}/.",
                    name, result.files, result.files == 1 ? "" : "s", folder) });
            for (const std::string& problem : result.problems)
                lines.push_back({ fmt::format("Can't convert \"{}\": {}", name, problem), true });
            if (!result.leftOut.empty())
                lines.push_back({ fmt::format("{} of its files couldn't be converted and {} left out:",
                                      result.leftOut.size(), result.leftOut.size() == 1 ? "was" : "were"),
                    true });
            for (size_t i = 0; i < result.leftOut.size(); ++i)
            {
                LOGW("Mod {}: left out {}", folder, result.leftOut[i]);
                if (i < kLeftOutShown)
                    lines.push_back({ "    " + result.leftOut[i], true });
            }
            if (result.leftOut.size() > kLeftOutShown)
                lines.push_back({ fmt::format("    and {} more (see dao.log).", result.leftOut.size() - kLeftOutShown),
                    true });
            for (const std::string& note : result.notes)
                lines.push_back({ note });
            if (mod != st.mods.end() && mod->IsForPc())
                lines.push_back({ fmt::format("It still has files in the PC version's formats (such as {}), so it "
                                              "isn't loaded.",
                                      mod->pcExample),
                    true });
        }

        void MainView(State& st, const Layout& layout)
        {
            // Not while mods are being added or converted (they'd be left out).
            const bool canPlay = st.gameFound && !ModsBusy(st);
            DrawLogo(st, layout);

            if (MenuItem(st, layout, "Play", kMenuTop, canPlay) ||
                (canPlay && ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false)))
                st.play = true;
            if (st.viewChanged)
                ImGui::SetKeyboardFocusHere(canPlay ? -1 : 0);
            if (MenuItem(st, layout, "Configure", kMenuTop + kMenuStep))
                SetView(st, View::Configure);
            if (MenuItem(st, layout, "Mods", kMenuTop + 2 * kMenuStep))
                SetView(st, View::Mods);
            // Update appears above Exit once a newer release is found.
            const bool update = FoundRelease(st) != nullptr;
            if (update && MenuItem(st, layout, "Update", kMenuTop + 3 * kMenuStep))
                SetView(st, View::Update);
            if (MenuItem(st, layout, "Exit", kMenuTop + (update ? 4 : 3) * kMenuStep))
                st.quit = true;
            Hint(st, layout, kButtonA, "Select", layout.Right(kContentsRight, kFooterHintY), true, false);

            // The bottom left: the game data, a summary of the settings, the startup option.
            const ImGuiStyle& style = ImGui::GetStyle();
            const float height = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeightWithSpacing() +
                g_state->helpFont->FontSize + style.ItemSpacing.y;
            const float margin = 24.0f * st.scale;
            ImGui::SetCursorScreenPos(ImVec2(layout.pos.x + margin, layout.pos.y + layout.size.y - margin - height));
            ImGui::BeginGroup();
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, st.fade);
            if (!st.gameFound)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, kError);
                ImGui::Text("Game data not found in %s", Utf8(st.gameRoot).c_str());
                ImGui::PopStyleColor();
            }
            else
            {
                const auto enabledMods =
                    std::count_if(st.mods.begin(), st.mods.end(), [](auto& m) { return m.IsLoaded(); });
                const std::string modsOn = st.modsRead
                    ? fmt::format("{} mod{} on", enabledMods, enabledMods == 1 ? "" : "s")
                    : std::string("reading mods");
                Label(fmt::format("{}  \xC2\xB7  {}x{}  \xC2\xB7  {}  \xC2\xB7  {}  \xC2\xB7  {}", Version(),
                    1280 * st.settings.resolutionScale, 720 * st.settings.resolutionScale,
                    st.settings.unlockFrameRate ? "60 fps" : "30 fps",
                    st.settings.temporalUpscaler != TemporalUpscaler::Off ? "temporal FSR" : "no temporal upscaling",
                    modsOn)
                          .c_str());
            }
            ImGui::Checkbox("Show this launcher at startup", &st.settings.showLauncher);
            HelpText("When hidden, hold Shift while starting the game to bring it back.");
            ImGui::PopStyleVar();
            ImGui::EndGroup();
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
            ImGui::Checkbox("VSync", &s.vsync);
            ImGui::Checkbox("60 fps (unlock the 30 fps cap in 3D scenes)", &s.unlockFrameRate);

            Section("HDR (experimental)");
            ImGui::Checkbox("HDR output", &s.hdr);
            ImGui::BeginDisabled(!s.hdr);
            SliderRow("Paper white", &s.hdrPaperWhite, kHdrMinNits, 400.0f, "%.0f nits");
            SliderRow("Peak brightness", &s.hdrPeak, s.hdrPaperWhite, 4000.0f, "%.0f nits");
            ImGui::EndDisabled();
            HelpText("On an HDR display, 3D scenes are rendered in HDR, their highlights up to the "
                     "peak brightness; the rest is shown at paper white.");
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

        // Text without Markdown's emphasis and code marks, links as their text.
        std::string PlainText(std::string_view text)
        {
            std::string out;
            // The next "](" (searched forward only, so this stays linear).
            size_t close = text.find("](");
            for (size_t i = 0; i < text.size(); i++)
            {
                if (text[i] == '*' || text[i] == '`')
                    continue;
                if (text[i] == '[' && close != std::string_view::npos)
                {
                    if (close < i)
                        close = text.find("](", i);
                    const size_t end = close == std::string_view::npos ? close : text.find(')', close);
                    if (end == std::string_view::npos)
                        close = std::string_view::npos; // No more links.
                    else
                    {
                        out += text.substr(i + 1, close - i - 1);
                        i = end;
                        continue;
                    }
                }
                out += text[i];
            }
            return out;
        }

        // A release's notes (GitHub Markdown), simply: headings as sections, list
        // items as bullets, nesting by indentation (two spaces a level, up to four).
        std::vector<NoteLine> ParseNotes(std::string_view notes)
        {
            std::vector<NoteLine> lines;
            while (!notes.empty())
            {
                const size_t end = notes.find('\n');
                std::string_view line = notes.substr(0, end);
                notes.remove_prefix(end == std::string_view::npos ? notes.size() : end + 1);
                const size_t indent = line.find_first_not_of(' ');
                if (indent == std::string_view::npos)
                {
                    lines.push_back({ NoteLine::Kind::Blank });
                    continue;
                }
                line.remove_prefix(indent);
                if (line.starts_with('#'))
                {
                    line.remove_prefix(std::min(line.find_first_not_of('#'), line.size()));
                    line.remove_prefix(std::min(line.find_first_not_of(' '), line.size()));
                    lines.push_back({ NoteLine::Kind::Heading, 0, PlainText(line) });
                    continue;
                }
                const bool item = line.starts_with("- ") || line.starts_with("* ");
                if (item)
                    line.remove_prefix(2);
                const int level = int(std::min<size_t>(indent / 2, 4));
                lines.push_back({ item ? NoteLine::Kind::Item : NoteLine::Kind::Text, level, PlainText(line) });
            }
            return lines;
        }

        void ReleaseNotes(const std::vector<NoteLine>& lines)
        {
            for (const NoteLine& line : lines)
            {
                if (line.kind == NoteLine::Kind::Blank)
                {
                    ImGui::Spacing();
                    continue;
                }
                if (line.kind == NoteLine::Kind::Heading)
                {
                    Section(line.text.c_str());
                    continue;
                }
                const float shift = float(line.level) * ImGui::GetStyle().IndentSpacing;
                if (shift > 0.0f)
                    ImGui::Indent(shift);
                if (line.kind == NoteLine::Kind::Item)
                {
                    ImGui::Bullet();
                    ImGui::SameLine();
                }
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(line.text.c_str());
                ImGui::PopTextWrapPos();
                if (shift > 0.0f)
                    ImGui::Unindent(shift);
            }
        }

        // The update's page: what it is, downloading and starting its installer
        // (the launcher then closes, see Run), what's new.
        void UpdateContents(State& st)
        {
            Updater& updater = *st.updater;
            const Release* found = FoundRelease(st);
            if (!found)
                return;
            const Release& release = *found;
            const Updater::State state = updater.GetState();
            Section(release.name.c_str());
            ImGui::Text("This is %s; %s is available.", Version(), release.tag.c_str());
            ImGui::Spacing();
            if (!updater.CanInstall())
            {
                if (ImGui::Button("Open the release page"))
                    updater.OpenReleasePage();
                HelpText("The launcher can't run its installer here (under Wine or Proton, or in a folder the "
                         "installer didn't make): see the README's \"Updating\".");
            }
            else if (state == Updater::State::Downloading)
            {
                const float progress = updater.GetProgress();
                ImGui::ProgressBar(progress, ImVec2(RowWidth(), 0),
                    fmt::format("Downloading {}%", int(progress * 100.0f)).c_str());
            }
            else if (state == Updater::State::Ready)
                ImGui::TextUnformatted("Starting the installer...");
            else
            {
                if (state == Updater::State::Failed)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, kError);
                    ImGui::TextWrapped("The download failed: %s.", updater.GetError().c_str());
                    ImGui::PopStyleColor();
                }
                if (ImGui::Button(state == Updater::State::Failed ? "Try again" : "Download and install"))
                    updater.Download();
                ImGui::SameLine();
                if (ImGui::Button("Release page"))
                    updater.OpenReleasePage();
                HelpText(fmt::format("Downloads its installer ({:.0f} MB) from GitHub, checks it against its "
                                     "published checksum and runs it on this folder: it rebuilds the game in a few "
                                     "minutes and keeps your saves, settings and mods. The launcher closes while it "
                                     "runs.",
                    double(release.installerSize) / (1 << 20))
                             .c_str());
            }
            ReleaseNotes(st.releaseNotes);
        }

        // Asks before deleting the selected mod (to the Recycle Bin).
        void DeleteModPopup(State& st)
        {
            const float width = 560.0f * st.scale;
            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, FLT_MAX));
            if (!ImGui::BeginPopupModal("Delete mod", nullptr,
                    ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings))
                return;
            bool close = st.selectedMod < 0 || st.selectedMod >= int(st.mods.size());
            if (!close)
            {
                const mods::ModInfo& mod = st.mods[st.selectedMod];
                ImGui::PushTextWrapPos(0.0f);
                ImGui::Text("Delete \"%s\"?", mod.name.c_str());
                ImGui::PopTextWrapPos();
                HelpText(fmt::format("Its folder, mods/{}, goes to the Recycle Bin, with the PC files kept from "
                                     "converting it, if it was.",
                    mod.folder)
                             .c_str());
                ImGui::Spacing();
                if (ImGui::Button("Delete"))
                {
                    const std::string name = mod.name;
                    std::string error;
                    if (mods::RecycleMod(st.modsRoot, mod.folder, error))
                    {
                        st.mods.erase(st.mods.begin() + st.selectedMod);
                        st.selectedMod = -1;
                        mods::FindOverlaps(st.mods);
                        st.modMessages = { { fmt::format("Deleted \"{}\" (it's in the Recycle Bin).", name) } };
                    }
                    else
                        st.modMessages = { { fmt::format("Can't delete \"{}\": {}.", name, error), true } };
                    close = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                    ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
                    close = true;
                // Cancel is the default.
                if (ImGui::IsWindowAppearing())
                    ImGui::SetKeyboardFocusHere(-1);
            }
            if (close)
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        void ModsList(State& st)
        {
            HelpText("Each mod is a folder in mods/ that mirrors the game folder, e.g. "
                     "mods/MyMod/packages/core/override/. Where two change the same file, the one lower in the list "
                     "usually wins.");
            ImGui::Spacing();

            HelpText(mods::CanOpenArchives()
                    ? "To add a mod, drop its archive (.zip, .7z, .rar), folder or files on this window: its files "
                      "are sorted into place."
                    : "To add a mod, drop its folder or files on this window: its files are sorted into place.");
            ImGui::Spacing();
            HelpText("Downloadable content and Title Update 6 copied from an Xbox 360 are installed the same way: "
                     "drop the packages, or the folder holding them.");
            ImGui::Spacing();
            if (ReadingMods(st))
            {
                ImGui::TextUnformatted("Reading the mods...");
                return;
            }
            if (AddingMods(st))
                ImGui::TextUnformatted("Adding...");
            if (ConvertingMod(st))
                ImGui::Text("Converting %s...", st.convertingMod.c_str());
            ImGui::PushTextWrapPos(0.0f);
            for (const auto& line : st.modMessages)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, line.problem ? kError : kText);
                ImGui::TextUnformatted(line.text.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::PopTextWrapPos();
            ImGui::Spacing();

            if (ImGui::Button("Open mods folder"))
                OpenFolder(st.modsRoot);
            ImGui::SameLine();
            // The list changes under a conversion: not while one runs.
            ImGui::BeginDisabled(ConvertingMod(st));
            // (Nor while mods are added, which the new list would leave out.)
            ImGui::BeginDisabled(AddingMods(st));
            if (ImGui::Button("Refresh"))
                ReadMods(st, mods::ToState(st.mods));
            ImGui::EndDisabled();
            ImGui::SameLine();
            const bool canUp = st.selectedMod > 0;
            const bool canDown = st.selectedMod >= 0 && st.selectedMod + 1 < int(st.mods.size());
            ImGui::BeginDisabled(!canUp);
            if (ImGui::Button("Move up"))
            {
                std::swap(st.mods[st.selectedMod], st.mods[st.selectedMod - 1]);
                st.selectedMod--;
                mods::FindOverlaps(st.mods);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!canDown);
            if (ImGui::Button("Move down"))
            {
                std::swap(st.mods[st.selectedMod], st.mods[st.selectedMod + 1]);
                st.selectedMod++;
                mods::FindOverlaps(st.mods);
            }
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            // A PC mod: Convert (one at a time, not while mods are being added).
            const bool selected = st.selectedMod >= 0 && st.selectedMod < int(st.mods.size());
            if (selected && st.mods[st.selectedMod].IsForPc())
            {
                ImGui::SameLine();
                ImGui::BeginDisabled(ModsBusy(st));
                if (ImGui::Button("Convert"))
                    ConvertSelectedMod(st);
                ImGui::EndDisabled();
            }
            if (selected)
            {
                ImGui::SameLine();
                ImGui::BeginDisabled(ModsBusy(st));
                if (ImGui::Button("Delete"))
                    ImGui::OpenPopup("Delete mod");
                ImGui::EndDisabled();
            }
            DeleteModPopup(st);
            ImGui::Spacing();

            if (st.mods.empty())
            {
                HelpText("No mods installed. Put mod folders in the mods folder and press Refresh.");
                return;
            }

            const float detailsHeight = ImGui::GetTextLineHeightWithSpacing() * 6;
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
                    // A PC mod can't be turned on (its choice is kept for a converted one),
                    // nor the one being converted changed.
                    ImGui::BeginDisabled(mod.IsForPc() || mod.folder == st.convertingMod);
                    bool on = mod.IsLoaded();
                    if (ImGui::Checkbox("##on", &on))
                    {
                        mod.enabled = on;
                        mods::FindOverlaps(st.mods);
                    }
                    ImGui::EndDisabled();
                    ImGui::TableNextColumn();
                    if (ImGui::Selectable(mod.name.c_str(), st.selectedMod == i, ImGuiSelectableFlags_SpanAllColumns))
                        st.selectedMod = i;
                    if (st.scrollToMod && st.selectedMod == i)
                    {
                        ImGui::SetScrollHereY();
                        st.scrollToMod = false;
                    }
                    if (mod.IsForPc())
                    {
                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Text, kError);
                        ImGui::TextUnformatted("(PC version)");
                        ImGui::PopStyleColor();
                    }
                    if (const size_t warnings = mod.warnings.size() + !mod.overlap.empty())
                    {
                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Text, kWarning);
                        ImGui::Text("(%d warning%s)", int(warnings), warnings == 1 ? "" : "s");
                        ImGui::PopStyleColor();
                    }
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
                ImGui::BeginChild("details", ImVec2(0, 0), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground);
                ImGui::Text("mods/%s", mod.folder.c_str());
                if (mod.IsForPc())
                    HelpText(fmt::format("Made for the PC version of the game: {} of its files ({}{}) are in PC "
                                         "formats, which this Xbox 360 version can't read, so it isn't loaded. "
                                         "Convert makes them this version's (most models, textures and tables "
                                         "convert; the PC files are kept).",
                        mod.pcFiles, mod.pcExample, mod.pcFiles > 1 ? ", ..." : "")
                                 .c_str());
                else
                    HelpText(mod.description.empty() ? "No description." : mod.description.c_str());
                ImGui::PushStyleColor(ImGuiCol_Text, kWarning);
                ImGui::PushTextWrapPos(0.0f);
                for (const std::string& warning : mod.warnings)
                    WrappedBullet(warning.c_str());
                if (!mod.overlap.empty())
                    WrappedBullet(mod.overlap.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
                ImGui::EndChild();
            }
        }

        // Shown before the mods page is first opened: what to expect of mods.
        void ModsWarning(State& st)
        {
            ImGui::PushFont(st.headingFont);
            ImGui::TextUnformatted("Before you add mods");
            ImGui::PopFont();
            ImGui::Spacing();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted("Mods for Dragon Age: Origins are made for the PC version of the game. This is the "
                                   "Xbox 360 version, so not every mod works:");
            ImGui::Spacing();
            WrappedBullet("Mods that change models, textures, items or tables usually work. Those in the PC "
                              "version's file formats need converting first: select one and press Convert.");
            WrappedBullet("New areas and campaigns (add-ins), and menus or sounds made for the PC version, "
                              "can't work here.");
            WrappedBullet("Any mod can make the game crash, look wrong or break a save. The launcher warns of "
                              "what it finds in a mod, but it can't catch everything.");
            WrappedBullet("Back up your saves before trying a mod: they're in userdata/content/.");
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            ImGui::Spacing();
            if (ImGui::Button("I understand"))
            {
                st.settings.modsWarningSeen = true;
                // Focus and scroll start over on the list.
                SetView(st, View::Mods);
            }
            if (st.viewChanged)
                ImGui::SetKeyboardFocusHere(-1);
            ImGui::SameLine();
            if (ImGui::Button("Open saves folder"))
                OpenFolder(st.userRoot / "content");
        }

        // Each puts a tab's settings back to their defaults.
        void RestoreGraphics(Settings& s)
        {
            const Settings defaults;
            s.resolutionScale = defaults.resolutionScale;
            s.temporalUpscaler = defaults.temporalUpscaler;
            s.temporalSharpness = defaults.temporalSharpness;
            s.upscaler = defaults.upscaler;
            s.sharpness = defaults.sharpness;
            s.anisotropicFiltering = defaults.anisotropicFiltering;
            s.shadows = defaults.shadows;
            s.postProcessing = defaults.postProcessing;
        }

        void RestoreDisplay(Settings& s)
        {
            const Settings defaults;
            s.fullscreen = defaults.fullscreen;
            s.windowWidth = defaults.windowWidth;
            s.windowHeight = defaults.windowHeight;
            s.vsync = defaults.vsync;
            s.unlockFrameRate = defaults.unlockFrameRate;
            s.hdr = defaults.hdr;
            s.hdrPaperWhite = defaults.hdrPaperWhite;
            s.hdrPeak = defaults.hdrPeak;
        }

        using Contents = void (*)(State&);
        using Restore = void (*)(Settings&);
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

        // The Options screen's frame: its background, the header bar with the view's
        // title and the footer's button hints. Back also with Escape or the gamepad's
        // B, `restore` (Restore Defaults, if given) with its Y.
        void PanelFrame(State& st, const Layout& layout, const char* title, Restore restore)
        {
            if (const Texture* panel = Art(st, kPanelBackground))
                DrawStretched(ImGui::GetBackgroundDrawList(), *panel, layout.Center(0, 0),
                    layout.Center(kStageWidth, kStageHeight), kPanelBackgroundAlpha);
            ImDrawList* draw = ImGui::GetWindowDrawList();
            if (const Texture* bar = Art(st, kHeaderBar.sprite))
                DrawSprite(draw, *bar, layout.Right(kHeaderBar.x, kHeaderBar.y), layout.fit, st.fade);
            if (const Texture* flourish = Art(st, kHeaderFlourish.sprite))
                DrawSprite(draw, *flourish, layout.Right(kHeaderFlourish.x, kHeaderFlourish.y), layout.fit,
                    kHeaderFlourish.alpha * st.fade);
            ImFont* font = st.titleFont;
            const float size = kHeaderTitleSize * layout.fit;
            const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0, title);
            const ImVec2 at = layout.Right(kHeaderTitleX, kHeaderTitleY);
            ShadowText(draw, st, font, size, ImVec2(at.x - extent.x * 0.5f, at.y - extent.y * 0.5f), kText, title);

            const ImVec2 left = layout.Center(kFooterLeft, kFooterTop), right = layout.Center(kFooterRight, kFooterTop);
            if (const Texture* bar = Art(st, kFooterBar))
                DrawStretched(draw, *bar, left, ImVec2(right.x, left.y + bar->height * layout.fit), st.fade);
            Hint(st, layout, kButtonA, "Select", layout.Center(kFooterLeft + 1.0f, kFooterHintY), false, false);
            if (restore)
            {
                const char* label = "Restore Defaults";
                const ImVec2 hint = layout.Center(kStageWidth * 0.5f - HintWidth(st, label) * 0.5f, kFooterHintY);
                // ImGui's navigation also takes the gamepad's Y (to type into the focused
                // slider); owning it keeps Y to this.
                const ImGuiID owner = ImGui::GetID("restore defaults");
                ImGui::SetKeyOwner(ImGuiKey_GamepadFaceUp, owner);
                if (Hint(st, layout, kButtonY, label, hint, false, true) ||
                    (st.backKeysFree && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, ImGuiInputFlags_None, owner)))
                    restore(st.settings);
            }
            if (Hint(st, layout, kButtonB, "Back", layout.Center(kFooterRight, kFooterHintY), true, true) ||
                (st.backKeysFree && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                                        ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))))
                SetView(st, View::Main);
        }

        // A view's contents, scrolling between the header and the footer, from `left`
        // (stage pixels from the left) to the right side; scrolled to the top when the
        // view is new or `resetScroll`.
        void ShowContents(State& st, const Layout& layout, float left, Contents contents, bool resetScroll = false)
        {
            const ImVec2 min = layout.Left(left, kContentsTop), max = layout.Right(kContentsRight, kContentsBottom);
            ImGui::SetCursorScreenPos(min);
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, st.fade);
            if (resetScroll || st.viewChanged)
                ImGui::SetNextWindowScroll(ImVec2(0, 0));
            ImGui::BeginChild("contents", ImVec2(max.x - min.x, max.y - min.y), ImGuiChildFlags_NavFlattened,
                ImGuiWindowFlags_NoBackground);
            contents(st);
            ImGui::EndChild();
            ImGui::PopStyleVar();
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
                if (st.viewChanged && selected)
                    ImGui::SetKeyboardFocusHere(-1);
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
            if (st.backKeysFree && ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false))
                st.configureTab = (st.configureTab + kConfigureTabs - 1) % kConfigureTabs;
            if (st.backKeysFree && ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false))
                st.configureTab = (st.configureTab + 1) % kConfigureTabs;
            // LB and RB under the tabs.
            const float bumper = kBumperSize * layout.fit;
            const ImVec2 bumpers = layout.Left(kTabTextX, kTabTop + kTabStep * kConfigureTabs + kBumperGap);
            if (const Texture* lb = Art(st, kButtonLB))
                DrawSprite(draw, *lb, bumpers, bumper / lb->height, kBumperOpacity * st.fade);
            if (const Texture* rb = Art(st, kButtonRB))
                DrawSprite(draw, *rb, ImVec2(bumpers.x + kBumperStep * layout.fit, bumpers.y), bumper / rb->height,
                    kBumperOpacity * st.fade);

            ShowContents(st, layout, kConfigureLeft, kTabs[st.configureTab].contents, st.configureTab != previous);
        }

        void ModsView(State& st, const Layout& layout)
        {
            PanelFrame(st, layout, "Mods", nullptr);
            // (Screenshots show the list.)
            const bool warn = !st.settings.modsWarningSeen && st.screenshotDir.empty();
            ShowContents(st, layout, kModsLeft, warn ? ModsWarning : ModsList);
        }

        void UpdateView(State& st, const Layout& layout)
        {
            PanelFrame(st, layout, "Update", nullptr);
            ShowContents(st, layout, kModsLeft, UpdateContents);
        }

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

        void AddTexture(State& st, SDL_Renderer* renderer, std::string name, int width, int height, const uint8_t* rgba,
            SDL_BlendMode blend)
        {
            SDL_Texture* texture =
                SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, width, height);
            if (!texture)
            {
                LOGW("Launcher: can't make a texture for {}: {}", name, SDL_GetError());
                return;
            }
            SDL_UpdateTexture(texture, nullptr, rgba, width * 4);
            SDL_SetTextureBlendMode(texture, blend);
            st.art.emplace(std::move(name), Texture{ texture, float(width), float(height) });
        }

        // RGBA for SDL_BLENDMODE_MUL (dst * (src + 1 - src alpha)) multiplying by the
        // colour at `opacity`: the colour premultiplied by its alpha.
        void ForMultiply(std::span<uint8_t> rgba, float opacity)
        {
            for (size_t i = 0; i + 3 < rgba.size(); i += 4)
            {
                const float alpha = rgba[i + 3] / 255.0f * opacity;
                for (size_t c = 0; c < 3; c++)
                    rgba[i + c] = uint8_t(rgba[i + c] * alpha + 0.5f);
                rgba[i + 3] = uint8_t(alpha * 255.0f + 0.5f);
            }
        }

        // The names of the game's sprites the launcher draws.
        std::vector<std::string_view> ArtNames()
        {
            std::vector<std::string_view> names(std::begin(kClouds), std::end(kClouds));
            for (const Layer* layer :
                { &kSunGlow, &kHills, &kMountains, &kSmoke, &kLogo, &kHeaderBar, &kHeaderFlourish })
                names.push_back(layer->sprite);
            for (const auto& group : { std::span<const Layer>(kForeground), std::span<const Layer>(kSwords) })
            {
                for (const Layer& layer : group)
                    names.push_back(layer.sprite);
            }
            for (const char* name : { kCloudShade, kGlow, kMist, kEmbers, kOrigins, kMenuBar, kMenuBarDown,
                     kPanelBackground, kTabBar, kSectionBar, kFooterBar, kButtonA, kButtonB, kButtonY, kButtonLB,
                     kButtonRB })
                names.push_back(name);
            std::ranges::sort(names);
            const auto duplicates = std::ranges::unique(names);
            names.erase(duplicates.begin(), duplicates.end());
            return names;
        }

        // The warm overlay: a vertical gradient over the stage, multiplied in. Smooth,
        // so a few rows scaled up bilinearly do.
        void AddWarmOverlay(State& st, SDL_Renderer* renderer)
        {
            constexpr int kRows = 128;
            std::vector<uint8_t> warm(kRows * 4);
            for (int i = 0; i < kRows; i++)
            {
                const float y = (float(i) + 0.5f) / kRows * kStageHeight;
                const float f = std::clamp((kWarmBottomY - y) / (kWarmBottomY - kWarmTopY), 0.0f, 1.0f);
                for (int c = 0; c < 3; c++)
                    warm[i * 4 + c] = uint8_t(std::lerp(kWarmBottom[c], kWarmTop[c], f) + 0.5f);
                warm[i * 4 + 3] = uint8_t(f * 255.0f + 0.5f);
            }
            ForMultiply(warm, kWarmOpacity);
            AddTexture(st, renderer, kWarmOverlay, 1, kRows, warm.data(), SDL_BLENDMODE_MUL);
        }

        // The vignette: black, its alpha rising from the inner circle to the outer. At a
        // tenth of its size in the clip, scaled up bilinearly.
        void AddVignetteMask(State& st, SDL_Renderer* renderer)
        {
            constexpr int kWidth = 192, kHeight = 120;
            std::vector<uint8_t> mask(kWidth * kHeight * 4);
            for (int y = 0; y < kHeight; y++)
            {
                for (int x = 0; x < kWidth; x++)
                {
                    const float dx = ((float(x) + 0.5f) / kWidth - 0.5f) * kVignetteWidth;
                    const float dy = ((float(y) + 0.5f) / kHeight - 0.5f) * kVignetteHeight;
                    const float f = std::clamp(
                        (std::hypot(dx, dy) - kVignetteInner) / (kVignetteOuter - kVignetteInner), 0.0f, 1.0f);
                    mask[(size_t(y) * kWidth + x) * 4 + 3] = uint8_t(f * 255.0f + 0.5f);
                }
            }
            AddTexture(st, renderer, kVignetteMask, kWidth, kHeight, mask.data(), SDL_BLENDMODE_BLEND);
        }

        // The game's sprites the launcher draws, as textures, with the scene's
        // variants and gradients.
        void LoadArt(State& st, SDL_Renderer* renderer)
        {
            const std::vector<std::string_view> names = ArtNames();
            LoadGameArt(st.gameRoot, kAtlases, names, [&](const std::string& name, ArtImage& image) {
                SDL_BlendMode blend = SDL_BLENDMODE_BLEND;
                if (name == kGlow)
                {
                    // White, for tinting, blended and added.
                    std::vector<uint8_t> white = image.rgba;
                    for (size_t i = 0; i + 3 < white.size(); i += 4)
                        white[i] = white[i + 1] = white[i + 2] = 255;
                    AddTexture(st, renderer, kGlowWhite, image.width, image.height, white.data(), SDL_BLENDMODE_BLEND);
                    AddTexture(st, renderer, kGlowAdd, image.width, image.height, white.data(), SDL_BLENDMODE_ADD);
                }
                else if (name == kCloudShade)
                {
                    ForMultiply(image.rgba, kCloudShadeOpacity);
                    blend = SDL_BLENDMODE_MUL;
                }
                AddTexture(st, renderer, name, image.width, image.height, image.rgba.data(), blend);
            });

            std::string missing;
            size_t missingCount = 0;
            for (std::string_view name : names)
            {
                if (!st.art.contains(std::string(name)))
                {
                    missing += fmt::format(" {}", name);
                    missingCount++;
                }
            }
            if (missingCount)
                LOGW("Launcher: {} of {} images not found in the game's guiexport.erf:{}", missingCount, names.size(),
                    missing);

            AddWarmOverlay(st, renderer);
            AddVignetteMask(st, renderer);
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

    bool ShowsMods(int argc, char** argv)
    {
        if (argc < 2)
            return false;
        return std::ranges::any_of(std::span(argv + 1, argv + argc),
            [](const char* arg) { return strcmp(arg, "--mods") == 0; });
    }

    bool Run(const fs::path& gameRoot, const fs::path& userRoot, bool showMods)
    {
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
        {
            LOGE("Launcher: SDL_Init failed: {}; starting the game", SDL_GetError());
            return true;
        }

        // The window: 16:9 like the game's screens, at the display's scale; resizable
        // down to a size the menus still fit.
        constexpr float kWindowWidth = 1120.0f, kWindowHeight = 630.0f, kMinWidth = 800.0f, kMinHeight = 450.0f;
        const float scale = std::max(1.0f, SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay()));
        SDL_Window* window = SDL_CreateWindow("Dragon Age: Origins - Launcher", int(kWindowWidth * scale),
            int(kWindowHeight * scale), SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
        if (!renderer)
        {
            LOGE("Launcher: no window ({}); starting the game", SDL_GetError());
            if (window)
                SDL_DestroyWindow(window);
            SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
            return true;
        }
        SDL_SetWindowMinimumSize(window, int(kMinWidth * scale), int(kMinHeight * scale));
        if (!SDL_SetRenderVSync(renderer, 1))
            LOGW("Launcher: vsync is unavailable ({})", SDL_GetError());
        // The driver can turn vsync off anyway, so frames are also capped: the menus'
        // slow animations need no more than 60 fps (saving power on a handheld). The
        // cap is 5% above the refresh rate when that's lower, so a vsynced frame is
        // never held back; with no refresh rate known, 60 Hz.
        constexpr float kMaxMenuFps = 60.0f;
        const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
        const float refresh = mode && mode->refresh_rate > 0 ? mode->refresh_rate : kMaxMenuFps;
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
            LOGE("Launcher: no system font found; starting the game");
            g_state = nullptr;
            ImGui::DestroyContext();
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
            return true;
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

        st.gameRoot = gameRoot;
        st.userRoot = userRoot;
        st.modsRoot = mods::ModsRoot(gameRoot);
        st.gameFound = fs::exists(gameRoot / "default.xex");
        st.settings = ReadSettings(userRoot);
        mods::CleanUpInstalls(st.modsRoot);
        mods::CleanUpConversions(st.modsRoot);
        ReadMods(st, st.settings.mods);
        if (showMods)
            SetView(st, View::Mods);
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
        LoadArt(st, renderer);
        st.updater = std::make_unique<Updater>();
        if (st.settings.checkForUpdates)
            st.updater->Check();

        // Screenshot mode: kFramesPerShot frames per shot (main, the configure tabs,
        // mods, and the update's page if the check finds one), captured on the last
        // one, after the view has faded in.
        constexpr int kFramesPerShot = 20;
        constexpr int kModsShot = 1 + kConfigureTabs;
        constexpr int kUpdateShot = kModsShot + 1;
        // While minimized or in the background: at most this often, to save power.
        constexpr int kIdleWaitMs = 100;

        Uint64 frameStart = SDL_GetTicksNS();
        while (!st.play && !st.quit)
        {
            const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
            const bool background = !(flags & SDL_WINDOW_INPUT_FOCUS) && st.screenshotDir.empty();
            if (flags & SDL_WINDOW_MINIMIZED || background)
                SDL_WaitEventTimeout(nullptr, kIdleWaitMs);
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                ImGui_ImplSDL3_ProcessEvent(&event);
                if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                    st.quit = true;
                // Files dropped on the window: one drop is its paths between
                // begin and complete.
                else if (event.type == SDL_EVENT_DROP_BEGIN)
                    st.dropped.clear();
                else if (event.type == SDL_EVENT_DROP_FILE && event.drop.data)
                    st.dropped.push_back(FromUtf8(event.drop.data));
                else if (event.type == SDL_EVENT_DROP_COMPLETE)
                    AddDroppedMods(st);
            }
            FinishReadingMods(st);
            FinishAddingMods(st);
            FinishConverting(st);
            if (flags & SDL_WINDOW_MINIMIZED)
                continue;

            // Before NewFrame, which handles ImGui's own uses of the keys.
            {
                const ImGuiContext& g = *ImGui::GetCurrentContext();
                const ImGuiWindow* nav = g.NavWindow;
                st.backKeysFree = g.OpenPopupStack.empty() && !g.ActiveId &&
                    (!nav || !nav->ParentWindow || !nav->ParentWindow->ParentWindow);
            }
            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();

            const int shot = st.screenshotDir.empty() ? -1 : st.screenshotFrame / kFramesPerShot;
            const bool shotStart = shot >= 0 && st.screenshotFrame % kFramesPerShot == 0;
            const bool shotNow = shot >= 0 && st.screenshotFrame % kFramesPerShot == kFramesPerShot - 1;
            // The mods page waits for mods being added, the update's page for the check.
            const bool updateShot = shot == kUpdateShot;
            const bool shotWaits = (shot == kModsShot && (ReadingMods(st) || AddingMods(st))) ||
                (updateShot && st.updater->GetState() == Updater::State::Checking);
            // (The update's page only when there's an update.)
            if (shot > kUpdateShot || (updateShot && !shotWaits && !FoundRelease(st)))
                st.quit = true;
            else if (shotStart && !shotWaits)
            {
                SetView(st, shot == 0 ? View::Main
                        : shot == kModsShot ? View::Mods
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
            DrawScene(st, layout, view == View::Main ? 0.0f : kPanelSceneDim);
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
            case View::Update:
                UpdateView(st, layout);
                break;
            }
            // Focus moves to the new view's first item in the frame it's shown.
            if (viewChanged && st.view == view)
                st.viewChanged = false;
            ImGui::End();
            // A downloaded update: its installer replaces the game's files, so close.
            if (st.updater->GetState() == Updater::State::Ready && st.updater->Launch())
                st.quit = true;

            ImGui::Render();
            SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
            SDL_SetRenderDrawColor(renderer, 12, 9, 8, 255);
            SDL_RenderClear(renderer);
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
            if (shotNow)
            {
                const std::string path = Utf8(st.screenshotDir / fmt::format("launcher_{}.bmp", shot));
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
        SaveSettings(userRoot, st.settings);
        LOGI("Launcher: {}", st.play ? "play" : "quit");

        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        for (auto& [name, art] : st.art)
            SDL_DestroyTexture(art.texture);
        g_state = nullptr;
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
        return st.play;
    }
}
