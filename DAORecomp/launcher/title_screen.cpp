#include "stdafx.h"
#include "launcher/launcher_internal.h"

#include "launcher/game_art.h"
#include "text.h"
#include "version.h"

namespace dao::launcher
{
    namespace
    {
        // The temporal upscaler in the title screen's summary line.
        const char* TemporalUpscalerLabel(TemporalUpscaler upscaler)
        {
            switch (upscaler)
            {
            case TemporalUpscaler::Off: return "no temporal upscaling";
            case TemporalUpscaler::Xess: return "XeSS-SR";
            case TemporalUpscaler::Dlss: return "DLSS 4.5";
            default: return "temporal FSR";
            }
        }

        // The title screen (startmenu2.gfx). Its background is a clip at kSceneX,
        // kSceneY on the stage, scaled by kSceneScale; the game shows it kSceneDrop
        // lower (its title-safe anchoring), so the scene's layers are all drawn that
        // much lower, but not the logo or the menu. Positions are on the stage unless
        // they're in the clip.
        constexpr float kSceneScale = 2.0f / 3.0f, kSceneX = 0.1f, kSceneY = -40.2f, kSceneDrop = 13.0f;
        // The sky, behind the clouds.
        constexpr ImU32 kSky = IM_COL32(26, 16, 9, 255);
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

        constexpr std::string_view kAtlases[] = { "atl_startmenu2_dxt5", "atl_startmenu2_dxt1",
            "atl_optionsmen_dxt5", "atl_shared_dxt5" };

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
    }

    // The title screen, animated as the game's: the clouds drifting, haze passing
    // over the landscape, the vignette flickering as by firelight, the embers and
    // the fire glowing, the mist drifting; as it is `t` seconds in (a double: the
    // scene runs for as long as the launcher is open). `dim` darkens it behind the
    // other views.
    void DrawScene(ImDrawList* draw, const State& st, const Layout& layout, float dim, double t)
    {
        draw->AddRectFilled(layout.pos, layout.Max(), kSky);
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

    std::optional<LogoArt> FindLogoArt(const State& st)
    {
        LogoArt art = { Art(st, kLogo.sprite), Art(st, kOrigins) };
        if (!art.logo || !art.origins)
            return std::nullopt;
        art.logoAt = ImVec2(kLogo.x, kLogo.y);
        art.originsAt = ImVec2(kOriginsRight - art.origins->width, kOriginsY);
        art.min = ImVec2(std::min(art.logoAt.x, art.originsAt.x), std::min(art.logoAt.y, art.originsAt.y));
        art.max = ImVec2(std::max(art.logoAt.x + art.logo->width, kOriginsRight),
            std::max(art.logoAt.y + art.logo->height, art.originsAt.y + art.origins->height));
        return art;
    }

    // The logo alone, `width` wide, centred on `center`.
    void DrawLogoArt(ImDrawList* draw, const LogoArt& art, ImVec2 center, float width)
    {
        const float scale = width / (art.max.x - art.min.x);
        const ImVec2 origin(center.x - width * 0.5f, center.y - (art.max.y - art.min.y) * scale * 0.5f);
        auto place = [&](ImVec2 at) {
            return ImVec2(origin.x + (at.x - art.min.x) * scale, origin.y + (at.y - art.min.y) * scale);
        };
        DrawSprite(draw, *art.logo, place(art.logoAt), scale, 1.0f);
        DrawSprite(draw, *art.origins, place(art.originsAt), scale, 1.0f);
    }

    void MainView(State& st, const Layout& layout)
    {
        // Not while mods are being added or converted (they'd be left out), or the
        // game added to Steam.
        const bool canPlay = st.gameFound && !ModsBusy(st) && st.steamStep == SteamStep::None;
        DrawLogo(st, layout);

        // The items' rows, from the top.
        float row = kMenuTop;
        const auto item = [&](const char* label, bool enabled = true) {
            const bool pressed = MenuItem(st, layout, label, row, enabled);
            row += kMenuStep;
            return pressed;
        };
        if (item("Play", canPlay) || (canPlay && ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false)))
            st.play = true;
        if (st.viewChanged)
            ImGui::SetKeyboardFocusHere(canPlay ? -1 : 0);
        if (item("Configure"))
            SetView(st, View::Configure);
        if (item("Mods"))
            SetView(st, View::Mods);
        if (item("Save Backups"))
            SetView(st, View::Saves);
        if (item("Downloadable Content"))
            SetView(st, View::Dlc);
        // Update appears above Exit once a newer release is found.
        if (FoundRelease(st) && item("Update"))
            SetView(st, View::Update);
        if (item("Exit"))
            st.quit = true;
        Hint(st, layout, PadButton::A, "Select", layout.Right(kContentsRight, kFooterHintY), true, false);

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
        else if (st.settingsUnreadable)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, kError);
            ImGui::TextUnformatted("config.toml can't be read (see dao.log): changes here won't be saved");
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
                st.settings.frameRate ? FrameRateName(st.settings.frameRate) : std::string("unlimited fps"),
                TemporalUpscalerLabel(st.settings.temporalUpscaler),
                modsOn)
                      .c_str());
        }
        ImGui::Checkbox("Show this launcher at startup", &st.settings.showLauncher);
        HelpText("When hidden, hold Shift while starting the game to bring it back.");
        ImGui::PopStyleVar();
        ImGui::EndGroup();
    }

    namespace
    {
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
