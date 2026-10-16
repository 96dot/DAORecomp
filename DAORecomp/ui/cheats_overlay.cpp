// The cheats menu's overlay: the cheats on a panel (as photo mode's settings,
// overlay.h), the selected one's description under it, the controls along the
// bottom, and a message for each cheat used. Drawn by the UI thread; the
// controller is the menu's while it's open (the game sees none of it, cheats.cpp),
// read here. Each cheat is one of the game's own debug scripts.
#include "stdafx.h"
#include "ui/cheats.h"

#include "gpu/xenia_host.h"
#include "input/input.h"
#include "ui/overlay.h"
#include "ui/palette.h"

#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/windowed_app_context.h"

namespace dao::ui
{
    namespace
    {
        using namespace palette;
        using namespace input;
        using cheats::Scope;

        enum class Row
        {
            Immortal,
            SkillChecks,
            KillEnemies,
            Gold,
            Experience,
            Strengthen,
            Count,
        };
        constexpr const char* kRowNames[] = { "Immortal Party", "Win Every Skill Check", "Kill Nearby Enemies",
            "Add Gold", "Add Experience", "Strengthen Party" };
        constexpr const char* kDescriptions[] = {
            "The party still takes damage, but no one in it can die.",
            "Persuasion, intimidation and the other skill checks in conversations always succeed.",
            "Kills up to 30 enemies the party leader can see, even those the story keeps alive: some fights may "
            "not end as they should.",
            "Adds gold to the party's purse.",
            "Experience for the whole party, as the game rewards it.",
            "Health and mana 1,000, strength and dexterity 50, for everyone in the party. Permanent.",
        };
        static_assert(std::size(kRowNames) == size_t(Row::Count) && std::size(kDescriptions) == size_t(Row::Count));

        // The amounts left and right choose between, and the one chosen at first.
        constexpr int kGold[] = { 1, 10, 100, 1000 };
        constexpr int kExperience[] = { 1000, 5000, 20000, 100000 };
        constexpr size_t kFirstGold = 2, kFirstExperience = 1;
        // Coppers in a gold coin (zz_money's value is in coppers).
        constexpr int kCoppersPerGold = 10000;

        // Laid out on the game's 1280x720 stage (the panel and the controls as
        // overlay.cpp's): the description under the panel, as wide as it (room for
        // kDescriptionLines lines kDescriptionLineHeight apart, for its size), and
        // the messages over the controls.
        constexpr float kDescriptionGap = 12.0f, kDescriptionLeft = 52.0f, kDescriptionWidth = 376.0f;
        constexpr float kDescriptionSize = 17.0f, kDescriptionLines = 3.0f, kDescriptionLineHeight = 1.3f;
        // The paused game dimmed behind the menu, as behind the game's own menus.
        constexpr ImU32 kBackdrop = IM_COL32(0, 0, 0, 90);
        // Seconds: how long a message lasts, and how long it takes to fade out.
        constexpr double kToastTime = 2.5;
        // Held d-pad: the first repeat, then every.
        constexpr double kRepeatDelay = 0.35, kRepeatEvery = 0.07;

        constexpr uint16_t kDirections = kButtonDpadUp | kButtonDpadDown | kButtonDpadLeft | kButtonDpadRight;

        // A number with thousands separators.
        std::string Grouped(int value)
        {
            std::string digits = std::to_string(value);
            for (int at = int(digits.size()) - 3; at > 0; at -= 3)
                digits.insert(size_t(at), ",");
            return digits;
        }

        // The rows left and right change the amount of, and the rows A switches.
        bool HasAmount(Row row)
        {
            return row == Row::Gold || row == Row::Experience;
        }
        bool IsSwitch(Row row)
        {
            return row == Row::Immortal || row == Row::SkillChecks;
        }

        class CheatsDialog final : public xe::ui::ImGuiDialog
        {
        public:
            explicit CheatsDialog(Overlay& overlay) : ImGuiDialog(overlay.drawer.get()), overlay_(overlay)
            {
                // Buttons already down (the Graphics tab's A) act once let go.
                PadState pad;
                if (GetState(0, pad))
                    previous_ = pad.buttons;
            }

            void Dismiss() { Close(); }

        protected:
            void OnDraw(ImGuiIO& io) override
            {
                stage_ = Stage(io);
                now_ = ImGui::GetTime();
                if (!leaving_)
                    HandlePad();

                ImGui::SetNextWindowPos(ImVec2(0, 0));
                ImGui::SetNextWindowSize(io.DisplaySize);
                ImGui::Begin("##cheats", nullptr,
                    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                        ImGuiWindowFlags_NoScrollWithMouse);
                // The whole screen (a window clips to a few pixels inside its edges).
                ImDrawList* draw = ImGui::GetWindowDrawList();
                draw->PushClipRectFullScreen();
                draw->AddRectFilled(ImVec2(0, 0), io.DisplaySize, kBackdrop);
                DrawPanel(draw);
                DrawControls(draw);
                DrawToast(draw);
                draw->PopClipRect();
                ImGui::End();
            }

        private:
            std::string Value(Row row) const
            {
                const cheats::State state = cheats::GetState();
                switch (row)
                {
                case Row::Immortal:
                    return state.immortal ? "On" : "Off";
                case Row::SkillChecks:
                    return state.skillChecks ? "On" : "Off";
                case Row::Gold:
                    return Grouped(kGold[gold_]) + " gold";
                case Row::Experience:
                    return Grouped(kExperience[experience_]) + " XP";
                default:
                    return {};
                }
            }

            // The cheats down the left, the selected one's description under them.
            void DrawPanel(ImDrawList* draw)
            {
                const float size = kDescriptionSize * stage_.fit;
                const float descriptionHeight =
                    kDescriptionGap + kDescriptionSize * kDescriptionLineHeight * kDescriptionLines;
                PanelRow rows[int(Row::Count)];
                for (int i = 0; i < int(Row::Count); ++i)
                {
                    const Row row = Row(i);
                    rows[i] = { kRowNames[i], Value(row), HasAmount(row) };
                }
                const float bottom = ui::DrawPanel(draw, overlay_, stage_, "Cheats", rows, row_, descriptionHeight);
                ShadowText(draw, overlay_.italic, size, stage_.At(kDescriptionLeft, bottom + kDescriptionGap),
                    kTextDim, kDescriptions[row_], kDescriptionWidth * stage_.fit);
            }

            void DrawControls(ImDrawList* draw)
            {
                const Row row = Row(row_);
                const bool amount = HasAmount(row);
                const bool toggle = IsSwitch(row);
                std::vector<Hint> hints = { { "Select", art::kDpadUp, art::kDpadDown } };
                if (amount)
                    hints.push_back({ "Amount", art::kDpadLeft, art::kDpadRight });
                hints.push_back({ toggle ? "Switch" : amount ? "Add" : "Use", art::kButtonA });
                hints.push_back({ "Close", art::kButtonB });
                ui::DrawControls(draw, overlay_, stage_, hints);
            }

            void DrawToast(ImDrawList* draw)
            {
                if (now_ >= toastUntil_)
                    return;
                const float alpha = FadeOut(toastUntil_ - now_, kToastFade);
                DrawMessage(draw, overlay_, stage_, overlay_.italic, kToastSize, kToastY, toast_, alpha);
            }

            void ShowToast(std::string text)
            {
                toast_ = std::move(text);
                toastUntil_ = now_ + kToastTime;
            }

            // A on the selected row.
            void Use()
            {
                const cheats::State state = cheats::GetState();
                switch (Row(row_))
                {
                case Row::Immortal:
                    // (The script switches its creature's: it runs for those that aren't yet,
                    // or back for those it made immortal.)
                    cheats::Run("pc_immortal", {}, state.immortal ? Scope::MadeImmortal : Scope::EachMortal);
                    ShowToast(state.immortal ? "The party can die again" : "The party can't die");
                    break;
                case Row::SkillChecks:
                    cheats::Run("zz_override_skills", state.skillChecks ? "0" : "1");
                    ShowToast(state.skillChecks ? "Skill checks as the game makes them" : "Every skill check succeeds");
                    break;
                case Row::KillEnemies:
                    cheats::Run("killallhostiles");
                    ShowToast("Enemies in sight killed");
                    break;
                case Row::Gold:
                    cheats::Run("zz_money", std::to_string(kGold[gold_] * kCoppersPerGold));
                    ShowToast("Added " + Value(Row::Gold));
                    break;
                case Row::Experience:
                    cheats::Run("addxp", std::to_string(kExperience[experience_]));
                    ShowToast("Added " + Value(Row::Experience));
                    break;
                case Row::Strengthen:
                    // (The script finds the creature it strengthens by its tag.)
                    cheats::Run("zz_supercrit", {}, Scope::EachTag);
                    ShowToast("The party is stronger");
                    break;
                case Row::Count:
                    break;
                }
            }

            // Left (-1) or right (1) on the selected row.
            void Change(int step)
            {
                const auto move = [step](size_t& index, size_t count) {
                    index = size_t(std::clamp(int(index) + step, 0, int(count) - 1));
                };
                if (Row(row_) == Row::Gold)
                    move(gold_, std::size(kGold));
                else if (Row(row_) == Row::Experience)
                    move(experience_, std::size(kExperience));
            }

            void HandlePad()
            {
                PadState pad;
                if (!GetState(0, pad))
                    return;
                const uint16_t buttons = pad.buttons;
                const uint16_t pressed = buttons & ~previous_;
                previous_ = buttons;

                if (pressed & (kButtonB | kButtonStart))
                {
                    // Once: another press before the overlay closes would open it again.
                    leaving_ = true;
                    ToggleCheats();
                    return;
                }
                if (pressed & kButtonA)
                    Use();

                // A step a press, repeating while held.
                const uint16_t direction = buttons & kDirections;
                uint16_t step = pressed & kDirections;
                if (step)
                    nextRepeat_ = now_ + kRepeatDelay;
                else if (direction && now_ >= nextRepeat_)
                {
                    step = direction;
                    nextRepeat_ = now_ + kRepeatEvery;
                }
                const int rows = int(Row::Count);
                if (step & kButtonDpadUp)
                    row_ = (row_ + rows - 1) % rows;
                if (step & kButtonDpadDown)
                    row_ = (row_ + 1) % rows;
                if (step & kButtonDpadLeft)
                    Change(-1);
                if (step & kButtonDpadRight)
                    Change(1);
            }

            Overlay& overlay_;
            Stage stage_;
            double now_ = 0;
            uint16_t previous_ = 0;
            double nextRepeat_ = 0;
            bool leaving_ = false; // closing asked for
            int row_ = 0;
            size_t gold_ = kFirstGold;
            size_t experience_ = kFirstExperience;
            std::string toast_;
            double toastUntil_ = 0;
        };

        CheatsDialog* s_dialog = nullptr; // UI thread
    }

    namespace cheats
    {
        void ShowOverlay()
        {
            const gpu::XeniaWindow host = gpu::GetXeniaWindow();
            if (!host.window)
            {
                LOGW("Cheats: no window, closing");
                ToggleCheats();
                return;
            }
            host.appContext->CallInUIThread([host] {
                if (s_dialog)
                    return;
                if (Overlay* overlay = GetOverlay(host))
                    s_dialog = new CheatsDialog(*overlay); // deletes itself when closed
                else
                {
                    // Nothing to show: closed rather than the controller taken.
                    LOGW("Cheats: no overlay, closing");
                    ToggleCheats();
                }
            });
        }

        void CloseOverlay()
        {
            const gpu::XeniaWindow host = gpu::GetXeniaWindow();
            if (!host.window)
                return;
            host.appContext->CallInUIThread([] {
                if (s_dialog)
                    s_dialog->Dismiss();
                s_dialog = nullptr;
            });
        }
    }
}
