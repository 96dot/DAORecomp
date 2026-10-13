#include "stdafx.h"
#include "launcher/launcher_internal.h"

#include <shellapi.h>

#include <imgui_internal.h>

namespace dao::launcher
{
    const State* g_state = nullptr;

    const Texture* Art(const State& st, const char* name)
    {
        const auto it = st.art.find(name);
        return it != st.art.end() ? &it->second : nullptr;
    }

    // A sprite with its top left at `at`, `scale` window pixels per sprite pixel;
    // mirrored, it extends left of `at` instead.
    void DrawTinted(ImDrawList* draw, const Texture& art, ImVec2 at, float scale, ImU32 tint, bool mirrored)
    {
        const float w = art.width * scale, h = art.height * scale;
        const ImVec2 min(mirrored ? at.x - w : at.x, at.y), max(min.x + w, min.y + h);
        draw->AddImage(ImTextureID(uintptr_t(art.texture)), min, max, ImVec2(mirrored ? 1.0f : 0.0f, 0),
            ImVec2(mirrored ? 0.0f : 1.0f, 1), tint);
    }

    void DrawSprite(ImDrawList* draw, const Texture& art, ImVec2 at, float scale, float alpha,
        bool mirrored)
    {
        DrawTinted(draw, art, at, scale, WithAlpha(IM_COL32_WHITE, alpha), mirrored);
    }

    // A sprite stretched over a rectangle.
    void DrawStretched(ImDrawList* draw, const Texture& art, ImVec2 min, ImVec2 max, float alpha)
    {
        draw->AddImage(ImTextureID(uintptr_t(art.texture)), min, max, ImVec2(0, 0), ImVec2(1, 1),
            WithAlpha(IM_COL32_WHITE, alpha));
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

    // An animated 0..1 for an item: towards 1 while `on`.
    float Ease(ImGuiID id, bool on, float seconds)
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

    namespace
    {
        // The width of a button hint, in stage pixels.
        float HintWidth(const State& st, const char* label)
        {
            return kHintIcon + kHintGap + st.headingFont->CalcTextSizeA(kHintTextSize, FLT_MAX, 0, label).x;
        }

        const char* XboxArt(PadButton button)
        {
            switch (button)
            {
            case PadButton::A:
                return kButtonA;
            case PadButton::B:
                return kButtonB;
            case PadButton::Y:
                return kButtonY;
            case PadButton::LB:
                return kButtonLB;
            case PadButton::RB:
                return kButtonRB;
            }
            return kButtonA;
        }

        // A PlayStation's face buttons' symbols, in their colours; L1 and R1 as pills.
        // (Their shapes' sizes are fractions of the button's height or radius.)
        constexpr ImU32 kPsCross = IM_COL32(124, 178, 232, 255), kPsCircle = IM_COL32(255, 102, 102, 255),
                        kPsTriangle = IM_COL32(64, 226, 160, 255);
        constexpr ImU32 kPsButton = IM_COL32(24, 22, 20, 235);
        // A pill's width, in heights.
        constexpr float kPsPillWidth = 1.7f;

        void DrawPlayStationButton(ImDrawList* draw, const State& st, PadButton button, ImVec2 at, float height,
            float alpha)
        {
            const float thickness = std::max(1.5f, height * 0.08f);
            const ImU32 rim = WithAlpha(kText, 0.6f * alpha);
            if (button == PadButton::LB || button == PadButton::RB)
            {
                const ImVec2 max(at.x + height * kPsPillWidth, at.y + height);
                draw->AddRectFilled(at, max, WithAlpha(kPsButton, alpha), height * 0.5f);
                draw->AddRect(at, max, rim, height * 0.5f, 0, thickness * 0.75f);
                const char* label = button == PadButton::LB ? "L1" : "R1";
                const float size = height * 0.62f;
                const ImVec2 text = st.headingFont->CalcTextSizeA(size, FLT_MAX, 0, label);
                draw->AddText(st.headingFont, size,
                    ImVec2((at.x + max.x - text.x) * 0.5f, (at.y + max.y - text.y) * 0.5f), WithAlpha(kText, alpha),
                    label);
                return;
            }
            const float r = height * 0.5f, s = r * 0.42f;
            const ImVec2 c(at.x + r, at.y + r);
            draw->AddCircleFilled(c, r * 0.95f, WithAlpha(kPsButton, alpha));
            draw->AddCircle(c, r * 0.95f, rim, 0, thickness * 0.75f);
            switch (button)
            {
            case PadButton::A:
                draw->AddLine(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), WithAlpha(kPsCross, alpha), thickness);
                draw->AddLine(ImVec2(c.x - s, c.y + s), ImVec2(c.x + s, c.y - s), WithAlpha(kPsCross, alpha), thickness);
                break;
            case PadButton::B:
                draw->AddCircle(c, s, WithAlpha(kPsCircle, alpha), 0, thickness);
                break;
            case PadButton::Y:
            default:
                draw->AddTriangle(ImVec2(c.x, c.y - s * 1.05f), ImVec2(c.x + s, c.y + s * 0.7f),
                    ImVec2(c.x - s, c.y + s * 0.7f), WithAlpha(kPsTriangle, alpha), thickness);
                break;
            }
        }
    }

    // A gamepad button's icon, `height` high with its top left at `at`, as the
    // gamepad used last has it.
    void DrawPadButton(ImDrawList* draw, const State& st, PadButton button, ImVec2 at, float height, float alpha)
    {
        if (st.padStyle == PadStyle::PlayStation)
            DrawPlayStationButton(draw, st, button, at, height, alpha);
        else if (const Texture* art = Art(st, XboxArt(button)))
            DrawSprite(draw, *art, at, height / art->height, alpha);
        else
            draw->AddCircleFilled(ImVec2(at.x + height * 0.5f, at.y + height * 0.5f), height * 0.45f,
                WithAlpha(kGold, alpha));
    }

    // A button hint of the game's footers at `at` (its top left; its top right when
    // the label comes first): the button's icon and the label. Clickable when
    // `clickable`; returns whether it was clicked.
    bool Hint(const State& st, const Layout& layout, PadButton button, const char* label, ImVec2 at,
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
        DrawPadButton(draw, st, button, ImVec2(iconX, min.y), icon, st.fade);
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

    // A popup asking something, centred over the window; if it's open, EndPopup after.
    bool BeginDialog(const State& st, const char* title)
    {
        const float width = 560.0f * st.scale;
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, FLT_MAX));
        return ImGui::BeginPopupModal(title, nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    }

    // Escape or the gamepad's B: a dialog's Cancel.
    bool CancelPressed()
    {
        return ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
    }

    // BeginTable for a table that scrolls (in a child window of its own), its rows
    // navigated as part of the view (NavFlattened, as ShowContents' contents): the
    // d-pad moves into, through and out of it, and B means Back. (ImGui has no call
    // for a table's child flags: they're set as BeginChild's would be.)
    bool BeginNavTable(const char* id, int columns, ImGuiTableFlags flags, ImVec2 size)
    {
        ImGuiContext& g = *GImGui;
        g.NextWindowData.HasFlags |= ImGuiNextWindowDataFlags_HasChildFlags;
        g.NextWindowData.ChildFlags = ImGuiChildFlags_NavFlattened;
        if (ImGui::BeginTable(id, columns, flags, size))
            return true;
        // (Not begun: not left for the next window.)
        g.NextWindowData.ClearFlags();
        return false;
    }

    // Wrapped text in `color` (kWarning, kError).
    void ColoredText(ImU32 color, const char* text)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextWrapped("%s", text);
        ImGui::PopStyleColor();
    }

    // How something went, a line each: problems in the error colour.
    void Messages(const std::vector<mods::InstallResult::Line>& lines)
    {
        ImGui::PushTextWrapPos(0.0f);
        for (const auto& line : lines)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, line.problem ? kError : kText);
            ImGui::TextUnformatted(line.text.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::PopTextWrapPos();
    }

    void SetView(State& st, View view)
    {
        st.view = view;
        st.viewChanged = true;
        st.viewStart = ImGui::GetTime();
    }

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
        Hint(st, layout, PadButton::A, "Select", layout.Center(kFooterLeft + 1.0f, kFooterHintY), false, false);
        if (restore)
        {
            const char* label = "Restore Defaults";
            const ImVec2 hint = layout.Center(kStageWidth * 0.5f - HintWidth(st, label) * 0.5f, kFooterHintY);
            // ImGui's navigation also takes the gamepad's Y (to type into the focused
            // slider); owning it keeps Y to this.
            const ImGuiID owner = ImGui::GetID("restore defaults");
            ImGui::SetKeyOwner(ImGuiKey_GamepadFaceUp, owner);
            if (Hint(st, layout, PadButton::Y, label, hint, false, true) ||
                (st.backKeysFree && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, ImGuiInputFlags_None, owner)))
                restore(st.settings);
        }
        if (st.installer.GetState() == InstallRunner::State::Running)
            return;
        if (Hint(st, layout, PadButton::B, st.setupOnly ? "Close" : "Back",
                layout.Center(kFooterRight, kFooterHintY), true, true) ||
            (st.backKeysFree && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                                    ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))))
        {
            if (st.setupOnly)
                st.quit = true;
            else
                SetView(st, View::Main);
        }
    }

    // A view's contents, scrolling between the header and the footer, from `left`
    // (stage pixels from the left) to the right side; scrolled to the top when the
    // view is new or `resetScroll`. A new view's first item has the focus, unless
    // something took it this frame (Configure's tab): the gamepad's focus is never
    // left on the last view's items. The gamepad's right stick scrolls the list the
    // focus is in (a scrolling table), or else the contents.
    void ShowContents(State& st, const Layout& layout, float left, Contents contents, bool resetScroll)
    {
        // Scrolled this far a second with the stick all the way, in stage pixels.
        constexpr float kPadScrollSpeed = 900.0f;
        const ImVec2 min = layout.Left(left, kContentsTop), max = layout.Right(kContentsRight, kContentsBottom);
        ImGui::SetCursorScreenPos(min);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, st.fade);
        if (resetScroll || st.viewChanged)
            ImGui::SetNextWindowScroll(ImVec2(0, 0));
        ImGuiContext& g = *GImGui;
        // The focus is in this (the launcher's) window, or one navigated as part of it
        // (NavFlattened: the contents below, their tables), as NavInitWindow wants.
        ImGuiWindow* const root = ImGui::GetCurrentWindow();
        if (st.viewChanged && !g.NavMoveSubmitted && g.NavWindow && g.NavWindow->RootWindowForNav == root)
            ImGui::NavInitWindow(g.NavWindow, true);
        ImGui::BeginChild("contents", ImVec2(max.x - min.x, max.y - min.y), ImGuiChildFlags_NavFlattened,
            ImGuiWindowFlags_NoBackground);
        // (Whole pixels: ImGui rounds the scroll, so the rest waits for the next frame.)
        if (st.padScroll == 0.0f || st.viewChanged)
            st.padScrollCarry = 0.0f;
        else
        {
            ImGuiWindow* window = ImGui::GetCurrentWindow();
            if (g.NavWindow && g.NavWindow != window && g.NavWindow->RootWindowForNav == root &&
                g.NavWindow->ScrollMax.y > 0.0f)
                window = g.NavWindow;
            st.padScrollCarry += st.padScroll * kPadScrollSpeed * layout.fit * ImGui::GetIO().DeltaTime;
            const float pixels = std::trunc(st.padScrollCarry);
            st.padScrollCarry -= pixels;
            ImGui::SetScrollY(window, window->Scroll.y + pixels);
        }
        contents(st);
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }
}
