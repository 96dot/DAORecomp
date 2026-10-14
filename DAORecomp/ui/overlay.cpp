#include "stdafx.h"
#include "ui/overlay.h"

#include "kernel/vfs.h"
#include "launcher/game_art.h"
#include "text.h"
#include "ui/palette.h"

#include "xenia/ui/graphics_provider.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/immediate_drawer.h"
#include "xenia/ui/presenter.h"
#include "xenia/ui/window.h"

namespace fs = std::filesystem;

namespace dao::ui
{
    namespace
    {
        constexpr std::string_view kAtlases[] = { "atl_optionsmen_dxt5", "atl_shared_dxt5" };
        constexpr std::string_view kArtNames[] = { art::kPanel, art::kSectionBar, art::kFooterBar, art::kButtonA,
            art::kButtonB, art::kButtonX, art::kButtonY, art::kButtonLB, art::kButtonRB, art::kButtonLT,
            art::kButtonRT, art::kButtonStart, art::kButtonBack, art::kLeftStick, art::kRightStick, art::kDpad,
            art::kDpadUpDown, art::kDpadUp, art::kDpadDown, art::kDpadLeft, art::kDpadRight, art::kArrowLeft,
            art::kPlate, art::kPlateLit, art::kTitlePlate, art::kBar, art::kDivider };

        Overlay* s_overlay = nullptr; // kept for the process's life

        // Far larger than any of the game's sprites used (a 1280x720 menu's): a
        // modded atlas's oversized sprite isn't kept as a texture.
        constexpr int kMaxSpriteSize = 2048;

        ImFont* AddFont(ImGuiIO& io, std::initializer_list<const wchar_t*> files, float size)
        {
            wchar_t windows[MAX_PATH] = {};
            if (!GetWindowsDirectoryW(windows, MAX_PATH))
                return nullptr;
            for (const wchar_t* file : files)
            {
                const fs::path path = fs::path(windows) / L"Fonts" / file;
                std::error_code ec;
                if (!fs::is_regular_file(path, ec))
                    continue;
                ImFontConfig config;
                config.OversampleH = 2;
                return io.Fonts->AddFontFromFileTTF(Utf8(path).c_str(), size, &config, kGlyphRanges);
            }
            return nullptr;
        }
    }

    Overlay::~Overlay() = default;

    const Sprite* Overlay::Art(const char* name) const
    {
        const auto it = art.find(name);
        return it != art.end() && it->second.texture ? &it->second : nullptr;
    }

    Overlay* GetOverlay(const gpu::XeniaWindow& host)
    {
        if (s_overlay)
            return s_overlay;
        auto overlay = std::make_unique<Overlay>();
        overlay->immediate = host.provider->CreateImmediateDrawer();
        if (!overlay->immediate)
            return nullptr;
        overlay->immediate->SetPresenter(host.presenter);
        // Above everything Xenia draws.
        constexpr size_t kZOrder = 100;
        overlay->drawer = std::make_unique<xe::ui::ImGuiDrawer>(host.window, kZOrder);
        ImGuiIO& io = overlay->drawer->GetIO();
        // The dialogs pick by the controller themselves, not by ImGui's navigation.
        io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad);
        io.IniFilename = nullptr;
        // Baked large: drawn at the window's scale, which can be 2-3x the stage's.
        constexpr float kFontSize = 56.0f;
        overlay->regular = AddFont(io, { L"pala.ttf", L"georgia.ttf", L"times.ttf", L"segoeui.ttf" }, kFontSize);
        overlay->bold = AddFont(io, { L"palab.ttf", L"georgiab.ttf", L"timesbd.ttf", L"arialbd.ttf" }, kFontSize);
        overlay->italic = AddFont(io, { L"palai.ttf", L"georgiai.ttf", L"timesi.ttf" }, kFontSize);
        ImFont* fallback = io.Fonts->Fonts.empty() ? nullptr : io.Fonts->Fonts[0];
        for (ImFont** font : { &overlay->regular, &overlay->bold, &overlay->italic })
            if (!*font)
                *font = overlay->regular ? overlay->regular : fallback;
        // Builds the font atlas with the fonts above.
        overlay->drawer->SetPresenterAndImmediateDrawer(host.presenter, overlay->immediate.get());

        launcher::LoadGameArt(kernel::VfsGameRoot(), kAtlases, kArtNames,
            [&](const std::string& name, launcher::ArtImage& image) {
                if (image.width > kMaxSpriteSize || image.height > kMaxSpriteSize)
                {
                    LOGW("Overlay: {} is {}x{}, too large; not used", name, image.width, image.height);
                    return;
                }
                Sprite sprite;
                sprite.texture = overlay->immediate->CreateTexture(uint32_t(image.width), uint32_t(image.height),
                    xe::ui::ImmediateTextureFilter::kLinear, false, image.rgba.data());
                sprite.width = float(image.width);
                sprite.height = float(image.height);
                overlay->art[name] = std::move(sprite);
            });
        if (overlay->art.size() < std::size(kArtNames))
            LOGW("Overlay: {} of {} images found in the game's guiexport.erf", overlay->art.size(),
                std::size(kArtNames));
        s_overlay = overlay.release();
        return s_overlay;
    }

    Stage::Stage(const ImGuiIO& io)
    {
        fit = std::min(io.DisplaySize.x / kWidth, io.DisplaySize.y / kHeight);
        originX = (io.DisplaySize.x - kWidth * fit) * 0.5f;
        originY = (io.DisplaySize.y - kHeight * fit) * 0.5f;
    }

    void ShadowText(ImDrawList* draw, ImFont* font, float size, ImVec2 at, ImU32 color, std::string_view text,
        float wrapWidth)
    {
        const float offset = std::max(1.0f, size / 16.0f);
        draw->AddText(font, size, ImVec2(at.x + offset, at.y + offset), palette::kShadow, text.data(),
            text.data() + text.size(), wrapWidth);
        draw->AddText(font, size, at, color, text.data(), text.data() + text.size(), wrapWidth);
    }

    void DrawArt(ImDrawList* draw, const Overlay& overlay, const char* name, ImVec2 min, ImVec2 max, float alpha,
        bool mirrored)
    {
        if (const Sprite* sprite = overlay.Art(name))
            draw->AddImage(sprite->Id(), min, max, ImVec2(mirrored ? 1.0f : 0.0f, 0), ImVec2(mirrored ? 0.0f : 1.0f, 1),
                palette::WithAlpha(IM_COL32_WHITE, alpha));
    }

    void DrawArtSliced(ImDrawList* draw, const Overlay& overlay, const char* name, ImVec2 min, ImVec2 max,
        float ends, float alpha)
    {
        const Sprite* sprite = overlay.Art(name);
        if (!sprite)
            return;
        // The ends at the sprite's own proportions, at most half the width each.
        const float end = std::min(ends * sprite->width * (max.y - min.y) / sprite->height, (max.x - min.x) * 0.5f);
        const ImU32 color = palette::WithAlpha(IM_COL32_WHITE, alpha);
        const float xs[] = { min.x, min.x + end, max.x - end, max.x };
        const float us[] = { 0.0f, ends, 1.0f - ends, 1.0f };
        for (int i = 0; i < 3; ++i)
            draw->AddImage(sprite->Id(), ImVec2(xs[i], min.y), ImVec2(xs[i + 1], max.y), ImVec2(us[i], 0),
                ImVec2(us[i + 1], 1), color);
    }

    float ArtHeight(const Overlay& overlay, const char* name, float width)
    {
        const Sprite* sprite = overlay.Art(name);
        return sprite ? sprite->height * width / sprite->width : 0.0f;
    }

    namespace
    {
        // Each of a hint's icons that the art has, at `height` pixels: its width.
        template<typename F>
        void ForIcons(const Overlay& overlay, const Hint& hint, float height, F&& f)
        {
            for (const char* name : { hint.icon, hint.icon2 })
                if (const Sprite* sprite = name ? overlay.Art(name) : nullptr)
                    f(*sprite, sprite->width * height / sprite->height);
        }
    }

    float HintsWidth(const Overlay& overlay, const Stage& stage, std::span<const Hint> hints, const HintStyle& style)
    {
        const float icon = style.icon * stage.fit, gap = style.gap * stage.fit, space = style.space * stage.fit;
        const float size = style.textSize * stage.fit;
        float width = 0;
        for (const Hint& hint : hints)
        {
            ForIcons(overlay, hint, icon, [&](const Sprite&, float w) { width += w + gap; });
            width += overlay.bold->CalcTextSizeA(size, FLT_MAX, 0, hint.label.data(),
                hint.label.data() + hint.label.size()).x + space;
        }
        return hints.empty() ? 0 : width - space;
    }

    void DrawHints(ImDrawList* draw, const Overlay& overlay, const Stage& stage, std::span<const Hint> hints,
        const HintStyle& style, float left, float top)
    {
        const float icon = style.icon * stage.fit, gap = style.gap * stage.fit, space = style.space * stage.fit;
        const float size = style.textSize * stage.fit;
        float x = left;
        for (const Hint& hint : hints)
        {
            ForIcons(overlay, hint, icon, [&](const Sprite& sprite, float w) {
                draw->AddImage(sprite.Id(), ImVec2(x, top), ImVec2(x + w, top + icon));
                x += w + gap;
            });
            const ImVec2 extent = overlay.bold->CalcTextSizeA(size, FLT_MAX, 0, hint.label.data(),
                hint.label.data() + hint.label.size());
            ShadowText(draw, overlay.bold, size, ImVec2(x, top + (icon - extent.y) * 0.5f), palette::kText,
                hint.label);
            x += extent.x + space;
        }
    }

    namespace
    {
        // The menus' layout on the stage. The panel: its left, width and top, the
        // shading's width, the title plate, the rows, and the divider under them.
        constexpr float kPanelLeft = 40.0f, kPanelWidth = 400.0f, kPanelTop = 40.0f, kShadeWidth = 560.0f;
        constexpr float kTitleWidth = 360.0f, kTitleMinHeight = 60.0f, kTitleSize = 26.0f;
        constexpr float kRowGap = 6.0f, kRowHeight = 44.0f, kRowTextSize = 20.0f, kInset = 26.0f;
        constexpr float kPlateEnds = 0.09f;
        constexpr float kArrowHeight = 22.0f, kValueWidth = 150.0f;
        constexpr float kDividerWidth = 380.0f;
        // A message's plate past its text, each side, and its height for the text's.
        constexpr float kMessagePad = 34.0f, kMessageHeight = 1.9f;
        // The controls along the bottom, on the pop-ups' bar.
        constexpr float kBarLeft = 30.0f, kBarTop = 664.0f, kBarBottom = 708.0f, kHintTop = 673.0f;
        constexpr float kBarEnds = 0.06f;
        constexpr HintStyle kHintStyle{ 26.0f, 5.0f, 16.0f, 16.0f };
    }

    float DrawPanel(ImDrawList* draw, const Overlay& overlay, const Stage& stage, std::string_view title,
        std::span<const PanelRow> rows, int selected, float shadeBelow)
    {
        using namespace palette;
        const auto px = [&](float size) { return size * stage.fit; };
        const float titleHeight = std::max(ArtHeight(overlay, art::kTitlePlate, kTitleWidth), kTitleMinHeight);
        const float rowsTop = kPanelTop + titleHeight + kRowGap;
        const float rowsBottom = rowsTop + float(rows.size()) * (kRowHeight + kRowGap);
        const float dividerHeight = ArtHeight(overlay, art::kDivider, kDividerWidth);

        // Shading behind it, fading out to the right.
        const ImVec2 shadeMin = stage.At(0, kPanelTop - 20.0f);
        const ImVec2 shadeMax = stage.At(kPanelLeft + kShadeWidth, rowsBottom + dividerHeight + 10.0f + shadeBelow);
        draw->AddRectFilledMultiColor(ImVec2(0, shadeMin.y), shadeMax, IM_COL32(0, 0, 0, 200), IM_COL32(0, 0, 0, 0),
            IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 200));

        // The title on its plate.
        const float middle = kPanelLeft + kPanelWidth * 0.5f;
        DrawArt(draw, overlay, art::kTitlePlate, stage.At(middle - kTitleWidth * 0.5f, kPanelTop),
            stage.At(middle + kTitleWidth * 0.5f, kPanelTop + titleHeight));
        const float titleSize = px(kTitleSize);
        const ImVec2 titleExtent =
            overlay.bold->CalcTextSizeA(titleSize, FLT_MAX, 0, title.data(), title.data() + title.size());
        const ImVec2 titleMiddle = stage.At(middle, kPanelTop + titleHeight * 0.5f);
        ShadowText(draw, overlay.bold, titleSize,
            ImVec2(titleMiddle.x - titleExtent.x * 0.5f, titleMiddle.y - titleExtent.y * 0.5f), kGoldBright, title);

        for (int i = 0; i < int(rows.size()); ++i)
        {
            const PanelRow& row = rows[i];
            const float top = rowsTop + i * (kRowHeight + kRowGap);
            const ImVec2 rowMin = stage.At(kPanelLeft, top);
            const ImVec2 rowMax = stage.At(kPanelLeft + kPanelWidth, top + kRowHeight);
            const bool lit = i == selected;
            DrawArtSliced(draw, overlay, lit ? art::kPlateLit : art::kPlate, rowMin, rowMax, kPlateEnds,
                lit ? 1.0f : 0.85f);
            const float size = px(kRowTextSize);
            const float textY = (rowMin.y + rowMax.y - size) * 0.5f;
            ShadowText(draw, overlay.bold, size, ImVec2(stage.At(kPanelLeft + kInset, 0).x, textY),
                lit ? kGoldBright : kText, row.name);
            if (row.value.empty())
                continue;

            // The value, between the Options screen's arrows when selected.
            const float right = stage.At(kPanelLeft + kPanelWidth - kInset, 0).x;
            const float left = right - px(kValueWidth);
            const ImVec2 extent = overlay.regular->CalcTextSizeA(size, FLT_MAX, 0, row.value.c_str());
            ShadowText(draw, overlay.regular, size, ImVec2((left + right - extent.x) * 0.5f, textY),
                lit ? kText : kTextDim, row.value);
            const float arrowHeightPerWidth = ArtHeight(overlay, art::kArrowLeft, 1.0f);
            if (lit && row.changes && arrowHeightPerWidth > 0)
            {
                const float h = px(kArrowHeight), w = h / arrowHeightPerWidth;
                const float y = (rowMin.y + rowMax.y - h) * 0.5f;
                DrawArt(draw, overlay, art::kArrowLeft, ImVec2(left, y), ImVec2(left + w, y + h));
                DrawArt(draw, overlay, art::kArrowLeft, ImVec2(right - w, y), ImVec2(right, y + h), 1.0f, true);
            }
        }

        DrawArt(draw, overlay, art::kDivider, stage.At(middle - kDividerWidth * 0.5f, rowsBottom),
            stage.At(middle + kDividerWidth * 0.5f, rowsBottom + dividerHeight));
        return rowsBottom + dividerHeight;
    }

    void DrawMessage(ImDrawList* draw, const Overlay& overlay, const Stage& stage, ImFont* font, float size, float y,
        std::string_view text, float alpha)
    {
        const float pixels = size * stage.fit;
        const ImVec2 extent = font->CalcTextSizeA(pixels, FLT_MAX, 0, text.data(), text.data() + text.size());
        const float middle = stage.At(Stage::kWidth * 0.5f, 0).x, top = stage.At(0, y).y;
        const float pad = kMessagePad * stage.fit, height = extent.y * kMessageHeight;
        DrawArtSliced(draw, overlay, art::kPlateLit, ImVec2(middle - extent.x * 0.5f - pad, top),
            ImVec2(middle + extent.x * 0.5f + pad, top + height), kPlateEnds, alpha);
        ShadowText(draw, font, pixels, ImVec2(middle - extent.x * 0.5f, top + (height - extent.y) * 0.5f),
            palette::WithAlpha(palette::kText, alpha), text);
    }

    void DrawControls(ImDrawList* draw, const Overlay& overlay, const Stage& stage, std::span<const Hint> hints)
    {
        DrawArtSliced(draw, overlay, art::kBar, stage.At(kBarLeft, kBarTop),
            stage.At(Stage::kWidth - kBarLeft, kBarBottom), kBarEnds, 0.95f);
        const float width = HintsWidth(overlay, stage, hints, kHintStyle);
        DrawHints(draw, overlay, stage, hints, kHintStyle, stage.At(Stage::kWidth * 0.5f, 0).x - width * 0.5f,
            stage.At(0, kHintTop).y);
    }
}
