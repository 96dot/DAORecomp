#pragma once
// What draws over the game window (the on-screen keyboard, photo mode): Xenia's
// ImGui drawer with serif fonts and the game's own menu art, read from the
// player's guiexport.erf (nothing of the game is shipped). Made once, on the UI
// thread, and kept for the process's life.

#include "gpu/xenia_host.h"

#include "third_party/imgui/imgui.h"

namespace xe::ui
{
    class ImGuiDrawer;
    class ImmediateDrawer;
    class ImmediateTexture;
}

namespace dao::ui
{
    // The game's art: the Options screen's (atl_optionsmen_dxt5) panel shading,
    // section and footer bars and value arrow; the main menu's button plates, the save
    // screen's title plate and the pop-ups' bar and divider; and the controller's
    // buttons (those atl_shared_dxt5).
    namespace art
    {
        inline constexpr const char* kPanel = "OptionsMenu_I200";
        inline constexpr const char* kSectionBar = "OptionsMenu_I17E";
        inline constexpr const char* kFooterBar = "OptionsMenu_I203";
        inline constexpr const char* kArrowLeft = "OptionsMenu_I4B";
        inline constexpr const char* kPlate = "MainMenu_I7B";
        inline constexpr const char* kPlateLit = "MainMenu_I7F";
        inline constexpr const char* kTitlePlate = "SaveScreen_I32";
        inline constexpr const char* kBar = "PopupLayer_I18E";
        inline constexpr const char* kDivider = "PopupLayer_IB5";
        inline constexpr const char* kButtonA = "XBoxGamepad_BottomAction";
        inline constexpr const char* kButtonB = "XBoxGamepad_RightAction";
        inline constexpr const char* kButtonX = "XBoxGamepad_LeftAction";
        inline constexpr const char* kButtonY = "XBoxGamepad_TopAction";
        inline constexpr const char* kButtonLB = "XBoxGamepad_LeftBumper";
        inline constexpr const char* kButtonRB = "XBoxGamepad_RightBumper";
        inline constexpr const char* kButtonLT = "XBoxGamepad_LeftTrigger";
        inline constexpr const char* kButtonRT = "XBoxGamepad_RightTrigger";
        inline constexpr const char* kButtonStart = "XBoxGamepad_Start";
        inline constexpr const char* kButtonBack = "XBoxGamepad_Select";
        inline constexpr const char* kLeftStick = "XBoxGamepad_LeftStick";
        inline constexpr const char* kRightStick = "XBoxGamepad_RightStick";
        inline constexpr const char* kDpad = "XBoxGamepad_DPad";
        inline constexpr const char* kDpadUpDown = "XBoxGamepad_LSwithUpAndDownArrows";
        inline constexpr const char* kDpadUp = "XBoxGamepad_DPad_Up";
        inline constexpr const char* kDpadDown = "XBoxGamepad_DPad_Down";
        inline constexpr const char* kDpadLeft = "XBoxGamepad_DPad_Left";
        inline constexpr const char* kDpadRight = "XBoxGamepad_DPad_Right";
    }

    struct Sprite
    {
        std::unique_ptr<xe::ui::ImmediateTexture> texture;
        float width = 0, height = 0;

        ImTextureID Id() const { return ImTextureID(uintptr_t(texture.get())); }
    };

    struct Overlay
    {
        std::unique_ptr<xe::ui::ImmediateDrawer> immediate;
        std::unique_ptr<xe::ui::ImGuiDrawer> drawer;
        ImFont* regular = nullptr;
        ImFont* bold = nullptr;
        ImFont* italic = nullptr;
        std::unordered_map<std::string, Sprite> art;

        ~Overlay();

        // Null if the game's art doesn't have it.
        const Sprite* Art(const char* name) const;
    };

    // UI thread. Null if the drawers can't be made.
    Overlay* GetOverlay(const gpu::XeniaWindow& host);

    // The characters the fonts have (and the game's, likely): Latin.
    inline constexpr ImWchar kGlyphRanges[] = { 0x0020, 0x00FF, 0x0100, 0x017F, 0x2010, 0x2027, 0 };

    // The game's 1280x720 stage, fitted (centred) to the display.
    struct Stage
    {
        static constexpr float kWidth = 1280.0f, kHeight = 720.0f;

        float fit = 1, originX = 0, originY = 0;

        Stage() = default;
        explicit Stage(const ImGuiIO& io);
        ImVec2 At(float x, float y) const { return ImVec2(originX + x * fit, originY + y * fit); }
    };

    // Text with the game's drop shadow (wrapped at `wrapWidth` pixels, if not 0).
    void ShadowText(ImDrawList* draw, ImFont* font, float size, ImVec2 at, ImU32 color, std::string_view text,
        float wrapWidth = 0);

    // A sprite of the game's art stretched over a rectangle (`mirrored`: left to
    // right), if the art has it.
    void DrawArt(ImDrawList* draw, const Overlay& overlay, const char* name, ImVec2 min, ImVec2 max,
        float alpha = 1.0f, bool mirrored = false);
    // A bar or plate stretched across a rectangle: its ends (`ends`, a fraction of
    // its width each) keep their shape, its middle stretches.
    void DrawArtSliced(ImDrawList* draw, const Overlay& overlay, const char* name, ImVec2 min, ImVec2 max,
        float ends, float alpha = 1.0f);
    // Its height at `width` pixels (0 if the art doesn't have it).
    float ArtHeight(const Overlay& overlay, const char* name, float width);

    // A row of controller hints: each button's (or buttons') icons, then what it does.
    struct Hint
    {
        std::string_view label;
        const char* icon;
        const char* icon2 = nullptr;
    };
    struct HintStyle
    {
        float icon, gap, space, textSize; // stage units
    };
    // The row's width in pixels.
    float HintsWidth(const Overlay& overlay, const Stage& stage, std::span<const Hint> hints, const HintStyle& style);
    // Drawn from `left` (pixels), icons from `top`.
    void DrawHints(ImDrawList* draw, const Overlay& overlay, const Stage& stage, std::span<const Hint> hints,
        const HintStyle& style, float left, float top);

    // The menus over the game (photo mode's settings, the cheats), on the stage.

    // A row of a menu panel: its name, and its value on the right (none if empty),
    // between the Options screen's arrows when it's selected and left and right
    // change it.
    struct PanelRow
    {
        std::string_view name;
        std::string value;
        bool changes = true;
    };
    // A menu panel down the stage's left: the title on the save screen's title
    // plate, then a main-menu plate a row (the highlighted one for the `selected`
    // row), the pop-ups' divider under them, all on shading fading out to the right
    // that reaches `shadeBelow` (stage units) under the divider. Returns the
    // divider's bottom (stage units).
    float DrawPanel(ImDrawList* draw, const Overlay& overlay, const Stage& stage, std::string_view title,
        std::span<const PanelRow> rows, int selected, float shadeBelow = 0);
    // Where the menus' passing messages go (stage units: over the controls' bar),
    // their size, and their fade at the end (seconds).
    inline constexpr float kToastY = 606.0f, kToastSize = 19.0f;
    inline constexpr double kToastFade = 0.5;
    // A message's alpha with `left` seconds to go, fading out over the last `fade`.
    inline float FadeOut(double left, double fade)
    {
        return float(std::min(1.0, left / fade));
    }
    // A line of text centred across the stage at `y` (stage units) on a
    // highlighted plate, faded by `alpha`.
    void DrawMessage(ImDrawList* draw, const Overlay& overlay, const Stage& stage, ImFont* font, float size, float y,
        std::string_view text, float alpha);
    // The controls along the stage's bottom, on the pop-ups' bar.
    void DrawControls(ImDrawList* draw, const Overlay& overlay, const Stage& stage, std::span<const Hint> hints);
}
