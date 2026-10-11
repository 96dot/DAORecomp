#include "stdafx.h"
#include "ui/keyboard.h"

#include <future>

#include "gpu/xenia_host.h"
#include "input/input.h"
#include "launcher/game_art.h"
#include "text.h"

#include "third_party/imgui/imgui.h"
#include "xenia/ui/graphics_provider.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/immediate_drawer.h"
#include "xenia/ui/presenter.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app_context.h"

namespace fs = std::filesystem;

namespace dao::ui
{
    namespace
    {
        // The game's menus: cream serif text, gold edges, dark warm panels (as the launcher's).
        constexpr ImU32 kText = IM_COL32(236, 226, 204, 255);
        constexpr ImU32 kTextDim = IM_COL32(190, 178, 154, 255);
        constexpr ImU32 kGold = IM_COL32(176, 146, 92, 255);
        constexpr ImU32 kGoldBright = IM_COL32(232, 204, 140, 255);
        constexpr ImU32 kShadow = IM_COL32(0, 0, 0, 200);

        // The game's art: the Options screen's panel, section and footer bars, and the
        // controller's buttons.
        constexpr std::string_view kAtlases[] = { "atl_optionsmen_dxt5", "atl_shared_dxt5" };
        constexpr const char* kPanel = "OptionsMenu_I200";
        constexpr const char* kSectionBar = "OptionsMenu_I17E";
        constexpr const char* kFooterBar = "OptionsMenu_I203";
        constexpr const char* kButtonA = "XBoxGamepad_BottomAction";
        constexpr const char* kButtonB = "XBoxGamepad_RightAction";
        constexpr const char* kButtonX = "XBoxGamepad_LeftAction";
        constexpr const char* kButtonY = "XBoxGamepad_TopAction";
        constexpr const char* kButtonLT = "XBoxGamepad_LeftTrigger";
        constexpr const char* kButtonRT = "XBoxGamepad_RightTrigger";
        constexpr const char* kButtonStart = "XBoxGamepad_Start";
        constexpr std::string_view kArtNames[] = { kPanel, kSectionBar, kFooterBar, kButtonA, kButtonB, kButtonX,
            kButtonY, kButtonLT, kButtonRT, kButtonStart };

        // Laid out on the game's 1280x720 stage, fitted to the window.
        constexpr float kStageWidth = 1280.0f, kStageHeight = 720.0f;
        constexpr float kPanelLeft = 250.0f, kPanelRight = 1030.0f, kPanelTop = 110.0f, kPanelBottom = 640.0f;
        constexpr float kInset = 30.0f;
        constexpr float kTitleY = 128.0f, kTitleSize = 26.0f, kSectionBarHeight = 40.0f;
        constexpr float kDescriptionY = 176.0f, kDescriptionSize = 19.0f;
        constexpr float kFieldTop = 210.0f, kFieldHeight = 50.0f, kFieldTextSize = 28.0f, kCountSize = 16.0f;
        constexpr float kKeysTop = 278.0f, kKeyWidth = 66.0f, kKeyHeight = 50.0f, kKeyGap = 6.0f;
        constexpr float kKeyTextSize = 24.0f, kWideKeyTextSize = 19.0f;
        constexpr float kFooterTop = 580.0f, kHintY = 590.0f, kHintIcon = 28.0f, kHintGap = 6.0f, kHintSpace = 18.0f,
                        kHintTextSize = 17.0f;
        constexpr float kSceneDim = 0.6f, kPanelAlpha = 0.96f;

        // The keys: four rows of ten characters per page, then the wide keys.
        constexpr int kColumns = 10, kCharacterRows = 4, kRows = kCharacterRows + 1;
        constexpr std::u16string_view kLetters[kCharacterRows] = { u"1234567890", u"qwertyuiop", u"asdfghjkl'",
            u"zxcvbnm-.," };
        constexpr std::u16string_view kAccents[kCharacterRows] = { u"àáâäãåæçèé",
            u"êëìíîïñòóô",
            u"öõøœùúûüýÿ", u"ß!?&()/:;_" };
        enum class Wide
        {
            Caps,
            Accents,
            Space,
            Backspace,
            Done,
        };
        constexpr int kWideKeys = 5, kWideSpan = kColumns / kWideKeys;

        // The characters the fonts have (and the game's, likely): Latin.
        constexpr ImWchar kGlyphRanges[] = { 0x0020, 0x00FF, 0x0100, 0x017F, 0x2010, 0x2027, 0 };
        bool Typeable(char16_t c)
        {
            for (size_t i = 0; kGlyphRanges[i]; i += 2)
                if (c >= kGlyphRanges[i] && c <= kGlyphRanges[i + 1])
                    return c != 0x7F;
            return false;
        }

        // XINPUT_GAMEPAD_* bits, and the triggers as two more.
        enum Button : uint32_t
        {
            kDpadUp = 0x0001,
            kDpadDown = 0x0002,
            kDpadLeft = 0x0004,
            kDpadRight = 0x0008,
            kStart = 0x0010,
            kLeftShoulder = 0x0100,
            kRightShoulder = 0x0200,
            kA = 0x1000,
            kB = 0x2000,
            kX = 0x4000,
            kY = 0x8000,
            kLeftTrigger = 0x10000,
            kRightTrigger = 0x20000,
        };
        constexpr uint32_t kDirections = kDpadUp | kDpadDown | kDpadLeft | kDpadRight;
        constexpr int kStickThreshold = 16384, kTriggerThreshold = 128;
        // A held direction moves again after this long, then this often.
        constexpr double kRepeatDelay = 0.35, kRepeatEvery = 0.07;

        uint32_t PadButtons()
        {
            input::PadState pad;
            if (!input::GetState(0, pad))
                return 0;
            uint32_t buttons = pad.buttons;
            if (pad.leftTrigger >= kTriggerThreshold)
                buttons |= kLeftTrigger;
            if (pad.rightTrigger >= kTriggerThreshold)
                buttons |= kRightTrigger;
            if (pad.thumbLY > kStickThreshold)
                buttons |= kDpadUp;
            if (pad.thumbLY < -kStickThreshold)
                buttons |= kDpadDown;
            if (pad.thumbLX < -kStickThreshold)
                buttons |= kDpadLeft;
            if (pad.thumbLX > kStickThreshold)
                buttons |= kDpadRight;
            return buttons;
        }

        std::string ToUtf8(std::u16string_view text)
        {
            return WideToUtf8(std::wstring_view(reinterpret_cast<const wchar_t*>(text.data()), text.size()));
        }

        char16_t Upper(char16_t c)
        {
            wchar_t w = wchar_t(c);
            CharUpperBuffW(&w, 1);
            return char16_t(w);
        }

        ImU32 WithAlpha(ImU32 color, float alpha)
        {
            const float a = float(color >> IM_COL32_A_SHIFT & 0xFF) * std::clamp(alpha, 0.0f, 1.0f);
            return (color & ~IM_COL32_A_MASK) | ImU32(a + 0.5f) << IM_COL32_A_SHIFT;
        }

        struct Sprite
        {
            std::unique_ptr<xe::ui::ImmediateTexture> texture;
            float width = 0, height = 0;
        };

        // What draws over the game window: Xenia's ImGui drawer with the fonts and
        // the game's art, made once on the UI thread.
        struct Overlay
        {
            std::unique_ptr<xe::ui::ImmediateDrawer> immediate;
            std::unique_ptr<xe::ui::ImGuiDrawer> drawer;
            ImFont* regular = nullptr;
            ImFont* bold = nullptr;
            ImFont* italic = nullptr;
            std::unordered_map<std::string, Sprite> art;

            const Sprite* Art(const char* name) const
            {
                const auto it = art.find(name);
                return it != art.end() && it->second.texture ? &it->second : nullptr;
            }
        };

        fs::path s_gameRoot;
        Overlay* s_overlay = nullptr;   // kept for the process's life
        std::atomic<bool> s_open{ false };
        std::atomic<bool> s_holdUntilReleased{ false };

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
            // Keys are picked by this, not ImGui's navigation.
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

            launcher::LoadGameArt(s_gameRoot, kAtlases, kArtNames,
                [&](const std::string& name, launcher::ArtImage& image) {
                    Sprite sprite;
                    sprite.texture = overlay->immediate->CreateTexture(uint32_t(image.width), uint32_t(image.height),
                        xe::ui::ImmediateTextureFilter::kLinear, false, image.rgba.data());
                    sprite.width = float(image.width);
                    sprite.height = float(image.height);
                    overlay->art[name] = std::move(sprite);
                });
            if (overlay->art.size() < std::size(kArtNames))
                LOGW("Keyboard: {} of {} images found in the game's guiexport.erf", overlay->art.size(),
                    std::size(kArtNames));
            s_overlay = overlay.release();
            return s_overlay;
        }

        class KeyboardDialog final : public xe::ui::ImGuiDialog
        {
        public:
            KeyboardDialog(Overlay& overlay, const KeyboardRequest& request,
                std::promise<std::optional<std::u16string>> done)
                : ImGuiDialog(overlay.drawer.get()), overlay_(overlay), title_(ToUtf8(request.title)),
                  description_(ToUtf8(request.description)), text_(request.text), max_(request.maxLength),
                  done_(std::move(done))
            {
                if (text_.size() > max_)
                    text_.resize(max_);
                cursor_ = text_.size();
                // Buttons already down (the one that opened it) act once let go.
                previous_ = PadButtons();
            }

        protected:
            void OnClose() override
            {
                if (!finished_)
                    done_.set_value(std::nullopt);
            }

            void OnDraw(ImGuiIO& io) override
            {
                fit_ = std::min(io.DisplaySize.x / kStageWidth, io.DisplaySize.y / kStageHeight);
                originX_ = (io.DisplaySize.x - kStageWidth * fit_) * 0.5f;
                originY_ = (io.DisplaySize.y - kStageHeight * fit_) * 0.5f;
                HandlePad();
                HandleKeyboard(io);
                if (finished_)
                    return;

                ImGui::SetNextWindowPos(ImVec2(0, 0));
                ImGui::SetNextWindowSize(io.DisplaySize);
                ImGui::Begin("##keyboard", nullptr,
                    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                        ImGuiWindowFlags_NoScrollWithMouse);
                ImDrawList* draw = ImGui::GetWindowDrawList();
                draw->AddRectFilled(ImVec2(0, 0), io.DisplaySize, WithAlpha(IM_COL32_BLACK, kSceneDim));
                DrawPanel(draw);
                DrawField(draw);
                DrawKeys(draw);
                DrawHints(draw);
                ImGui::End();
            }

        private:
            ImVec2 At(float x, float y) const { return ImVec2(originX_ + x * fit_, originY_ + y * fit_); }

            void ShadowText(ImDrawList* draw, ImFont* font, float size, ImVec2 at, ImU32 color, std::string_view text)
            {
                const float offset = std::max(1.0f, size / 16.0f);
                draw->AddText(font, size, ImVec2(at.x + offset, at.y + offset), kShadow, text.data(),
                    text.data() + text.size());
                draw->AddText(font, size, at, color, text.data(), text.data() + text.size());
            }

            void DrawStretched(ImDrawList* draw, const char* name, ImVec2 min, ImVec2 max, float alpha)
            {
                if (const Sprite* sprite = overlay_.Art(name))
                    draw->AddImage(ImTextureID(uintptr_t(sprite->texture.get())), min, max, ImVec2(0, 0),
                        ImVec2(1, 1), WithAlpha(IM_COL32_WHITE, alpha));
            }

            // The Options screen's background in a gold frame, the title on its section bar.
            void DrawPanel(ImDrawList* draw)
            {
                const ImVec2 min = At(kPanelLeft, kPanelTop), max = At(kPanelRight, kPanelBottom);
                draw->AddRectFilled(min, max, IM_COL32(18, 12, 8, 235));
                DrawStretched(draw, kPanel, min, max, kPanelAlpha);
                draw->AddRect(min, max, WithAlpha(kGold, 0.8f), 0, 0, std::max(1.0f, 1.5f * fit_));

                const float barHeight = kSectionBarHeight * fit_;
                const ImVec2 bar = At(kPanelLeft + kInset * 0.5f, kTitleY - 6.0f);
                if (const Sprite* sprite = overlay_.Art(kSectionBar))
                    draw->AddImage(ImTextureID(uintptr_t(sprite->texture.get())), bar,
                        ImVec2(bar.x + sprite->width * barHeight / sprite->height, bar.y + barHeight));
                const float size = kTitleSize * fit_;
                ShadowText(draw, overlay_.bold, size, At(kPanelLeft + kInset, kTitleY),
                    kText, title_.empty() ? "Enter text" : title_);
                if (!description_.empty())
                {
                    const float descriptionSize = kDescriptionSize * fit_;
                    const ImVec2 at = At(kPanelLeft + kInset, kDescriptionY);
                    const float wrap = (kPanelRight - kPanelLeft - 2 * kInset) * fit_;
                    draw->AddText(overlay_.italic, descriptionSize, at, kTextDim, description_.c_str(), nullptr, wrap);
                }
            }

            // The text so far, with its cursor and how much room is left.
            void DrawField(ImDrawList* draw)
            {
                const ImVec2 min = At(kPanelLeft + kInset, kFieldTop);
                const ImVec2 max = At(kPanelRight - kInset, kFieldTop + kFieldHeight);
                draw->AddRectFilled(min, max, IM_COL32(0, 0, 0, 160));
                draw->AddRect(min, max, WithAlpha(kGold, 0.9f), 0, 0, std::max(1.0f, fit_));
                const float size = kFieldTextSize * fit_, pad = 12.0f * fit_;
                const std::string text = ToUtf8(text_);
                const std::string before = ToUtf8(std::u16string_view(text_).substr(0, cursor_));
                const float caretX = overlay_.regular->CalcTextSizeA(size, FLT_MAX, 0, before.c_str()).x;
                const float room = max.x - min.x - 2 * pad;
                const float scroll = std::max(0.0f, caretX - room);
                const float textY = min.y + (max.y - min.y - size) * 0.5f;
                draw->PushClipRect(ImVec2(min.x + pad * 0.5f, min.y), ImVec2(max.x - pad * 0.5f, max.y), true);
                draw->AddText(overlay_.regular, size, ImVec2(min.x + pad - scroll, textY), kText, text.c_str());
                // Blinking, as the game's text fields.
                if (std::fmod(ImGui::GetTime(), 1.0) < 0.6)
                {
                    const float x = min.x + pad - scroll + caretX;
                    draw->AddLine(ImVec2(x, textY + size * 0.1f), ImVec2(x, textY + size * 0.95f), kGoldBright,
                        std::max(1.0f, 2.0f * fit_));
                }
                draw->PopClipRect();
                const std::string count = fmt::format("{}/{}", text_.size(), max_);
                const float countSize = kCountSize * fit_;
                const ImVec2 extent = overlay_.regular->CalcTextSizeA(countSize, FLT_MAX, 0, count.c_str());
                draw->AddText(overlay_.regular, countSize, ImVec2(max.x - extent.x, max.y + 3.0f * fit_), kTextDim,
                    count.c_str());
            }

            char16_t Character(int row, int column) const
            {
                const char16_t c = (accents_ ? kAccents : kLetters)[row][column];
                return caps_ ? Upper(c) : c;
            }

            const char* WideLabel(Wide key) const
            {
                switch (key)
                {
                case Wide::Caps:
                    return "Caps";
                case Wide::Accents:
                    return accents_ ? "Letters" : "Accents";
                case Wide::Space:
                    return "Space";
                case Wide::Backspace:
                    return "Backspace";
                case Wide::Done:
                    return "Done";
                }
                return "";
            }

            // One key: dark, gold-edged; the selected one lit gold.
            void DrawKey(ImDrawList* draw, int row, int column, int span, std::string_view label, bool lit, float textSize)
            {
                const float left = kPanelLeft + (kPanelRight - kPanelLeft - (kColumns * kKeyWidth +
                                                     (kColumns - 1) * kKeyGap)) * 0.5f;
                const float x = left + column * (kKeyWidth + kKeyGap);
                const float y = kKeysTop + row * (kKeyHeight + kKeyGap);
                const ImVec2 min = At(x, y), max = At(x + span * kKeyWidth + (span - 1) * kKeyGap, y + kKeyHeight);
                ImGui::SetCursorScreenPos(min);
                ImGui::PushID(row * kColumns + column);
                const bool clicked = ImGui::InvisibleButton("##key", ImVec2(max.x - min.x, max.y - min.y));
                const bool hovered = ImGui::IsItemHovered();
                ImGui::PopID();
                const bool selected = row == row_ && column == SelectedColumn();
                if (selected)
                    draw->AddRectFilledMultiColor(min, max, WithAlpha(kGold, 0.6f), WithAlpha(kGold, 0.6f),
                        WithAlpha(kGold, 0.2f), WithAlpha(kGold, 0.2f));
                else
                    draw->AddRectFilled(min, max, IM_COL32(20, 14, 10, hovered ? 220 : 170));
                draw->AddRect(min, max, selected ? kGoldBright : WithAlpha(kGold, hovered ? 0.8f : 0.35f), 0, 0,
                    std::max(1.0f, (selected ? 2.0f : 1.0f) * fit_));
                const float size = textSize * fit_;
                const ImVec2 extent =
                    overlay_.bold->CalcTextSizeA(size, FLT_MAX, 0, label.data(), label.data() + label.size());
                ShadowText(draw, overlay_.bold, size,
                    ImVec2((min.x + max.x - extent.x) * 0.5f, (min.y + max.y - extent.y) * 0.5f),
                    selected || lit ? kGoldBright : kText, label);
                if (clicked)
                {
                    row_ = row;
                    column_ = row == kCharacterRows ? column / kWideSpan : column;
                    remembered_ = column;
                    Press();
                }
            }

            void DrawKeys(ImDrawList* draw)
            {
                for (int row = 0; row < kCharacterRows; ++row)
                    for (int column = 0; column < kColumns; ++column)
                    {
                        const char16_t c = Character(row, column);
                        DrawKey(draw, row, column, 1, ToUtf8(std::u16string_view(&c, 1)), false, kKeyTextSize);
                    }
                for (int key = 0; key < kWideKeys; ++key)
                {
                    const Wide wide = Wide(key);
                    const bool lit = (wide == Wide::Caps && caps_) || (wide == Wide::Accents && accents_);
                    DrawKey(draw, kCharacterRows, key * kWideSpan, kWideSpan, WideLabel(wide), lit, kWideKeyTextSize);
                }
            }

            // The footer: the game's bar and its button icons with what they do.
            void DrawHints(ImDrawList* draw)
            {
                DrawStretched(draw, kFooterBar, At(kPanelLeft + kInset * 0.5f, kFooterTop),
                    At(kPanelRight - kInset * 0.5f, kFooterTop + kHintIcon + 2 * (kHintY - kFooterTop)), 1.0f);
                const std::pair<const char*, const char*> hints[] = { { kButtonA, "Select" }, { kButtonB, "Cancel" },
                    { kButtonX, "Backspace" }, { kButtonY, "Space" }, { kButtonLT, "Caps" },
                    { kButtonRT, accents_ ? "Letters" : "Accents" }, { kButtonStart, "Done" } };
                const float icon = kHintIcon * fit_, gap = kHintGap * fit_, space = kHintSpace * fit_;
                const float size = kHintTextSize * fit_;
                float width = -space;
                for (const auto& [button, label] : hints)
                    width += icon + gap + overlay_.bold->CalcTextSizeA(size, FLT_MAX, 0, label).x + space;
                float x = At((kPanelLeft + kPanelRight) * 0.5f, 0).x - width * 0.5f;
                const float y = At(0, kHintY).y;
                for (const auto& [button, label] : hints)
                {
                    if (const Sprite* sprite = overlay_.Art(button))
                    {
                        const float w = sprite->width * icon / sprite->height;
                        draw->AddImage(ImTextureID(uintptr_t(sprite->texture.get())), ImVec2(x, y),
                            ImVec2(x + w, y + icon));
                        x += w + gap;
                    }
                    const ImVec2 extent = overlay_.bold->CalcTextSizeA(size, FLT_MAX, 0, label);
                    ShadowText(draw, overlay_.bold, size, ImVec2(x, y + (icon - extent.y) * 0.5f), kText, label);
                    x += extent.x + space;
                }
            }

            int SelectedColumn() const { return row_ == kCharacterRows ? column_ * kWideSpan : column_; }

            void Move(uint32_t direction)
            {
                // `remembered_` keeps the column across the wide keys' row.
                if (direction & (kDpadUp | kDpadDown))
                {
                    row_ = (row_ + ((direction & kDpadUp) ? kRows - 1 : 1)) % kRows;
                    column_ = row_ == kCharacterRows ? remembered_ / kWideSpan : remembered_;
                    return;
                }
                const int count = row_ == kCharacterRows ? kWideKeys : kColumns;
                column_ = (column_ + ((direction & kDpadLeft) ? count - 1 : 1)) % count;
                remembered_ = row_ == kCharacterRows ? column_ * kWideSpan : column_;
            }

            void HandlePad()
            {
                const uint32_t buttons = PadButtons();
                const uint32_t pressed = buttons & ~previous_;
                previous_ = buttons;
                const double now = ImGui::GetTime();
                const uint32_t direction = buttons & kDirections;
                if (pressed & kDirections)
                {
                    Move(pressed & kDirections);
                    nextRepeat_ = now + kRepeatDelay;
                }
                else if (direction && now >= nextRepeat_)
                {
                    Move(direction);
                    nextRepeat_ = now + kRepeatEvery;
                }
                if (pressed & kA)
                    Press();
                if (pressed & kX)
                    Erase();
                if (pressed & kY)
                    Insert(u' ');
                if (pressed & kLeftTrigger)
                    caps_ = !caps_;
                if (pressed & kRightTrigger)
                    accents_ = !accents_;
                if ((pressed & kLeftShoulder) && cursor_ > 0)
                    --cursor_;
                if ((pressed & kRightShoulder) && cursor_ < text_.size())
                    ++cursor_;
                if (pressed & kStart)
                    Finish(text_);
                else if (pressed & kB)
                    Finish(std::nullopt);
            }

            void HandleKeyboard(ImGuiIO& io)
            {
                if (finished_)
                    return;
                for (const ImWchar c : io.InputQueueCharacters)
                {
                    if (c < 0x10000 && Typeable(char16_t(c)))
                        Insert(char16_t(c));
                }
                if (ImGui::IsKeyPressed(ImGuiKey_Backspace))
                    Erase();
                if (ImGui::IsKeyPressed(ImGuiKey_Delete) && cursor_ < text_.size())
                    text_.erase(cursor_, 1);
                if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && cursor_ > 0)
                    --cursor_;
                if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && cursor_ < text_.size())
                    ++cursor_;
                if (ImGui::IsKeyPressed(ImGuiKey_Home))
                    cursor_ = 0;
                if (ImGui::IsKeyPressed(ImGuiKey_End))
                    cursor_ = text_.size();
                if (ImGui::IsKeyPressed(ImGuiKey_Enter, false))
                    Finish(text_);
                else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                    Finish(std::nullopt);
            }

            // The selected key.
            void Press()
            {
                if (finished_)
                    return;
                if (row_ < kCharacterRows)
                {
                    Insert(Character(row_, column_));
                    return;
                }
                switch (Wide(column_))
                {
                case Wide::Caps:
                    caps_ = !caps_;
                    break;
                case Wide::Accents:
                    accents_ = !accents_;
                    break;
                case Wide::Space:
                    Insert(u' ');
                    break;
                case Wide::Backspace:
                    Erase();
                    break;
                case Wide::Done:
                    Finish(text_);
                    break;
                }
            }

            void Insert(char16_t c)
            {
                if (text_.size() >= max_)
                    return;
                text_.insert(text_.begin() + cursor_, c);
                ++cursor_;
            }

            void Erase()
            {
                if (cursor_ == 0)
                    return;
                text_.erase(--cursor_, 1);
            }

            void Finish(std::optional<std::u16string> result)
            {
                if (finished_)
                    return;
                finished_ = true;
                done_.set_value(std::move(result));
                Close();
            }

            Overlay& overlay_;
            const std::string title_, description_;
            std::u16string text_;
            size_t cursor_ = 0;
            const size_t max_;
            std::promise<std::optional<std::u16string>> done_;
            bool finished_ = false;
            // The selected key: a row, and a column (in the wide keys' row, which of them).
            int row_ = 1, column_ = 0, remembered_ = 0;
            bool caps_ = false, accents_ = false;
            uint32_t previous_ = 0;
            double nextRepeat_ = 0;
            float fit_ = 1, originX_ = 0, originY_ = 0;
        };
    }

    void SetKeyboardGameRoot(const fs::path& gameRoot)
    {
        s_gameRoot = gameRoot;
    }

    std::optional<std::u16string> RunKeyboard(const KeyboardRequest& request)
    {
        const gpu::XeniaWindow host = gpu::GetXeniaWindow();
        if (!host.window || s_open.exchange(true))
            return request.text;
        LOGI("Keyboard: shown for \"{}\" (up to {} characters)", ToUtf8(request.title), request.maxLength);
        input::SetTextEntry(true);
        std::promise<std::optional<std::u16string>> done;
        auto result = done.get_future();
        bool shown = false;
        host.appContext->CallInUIThreadSynchronous([&] {
            if (Overlay* overlay = GetOverlay(host))
            {
                // It deletes itself when closed.
                new KeyboardDialog(*overlay, request, std::move(done));
                shown = true;
            }
        });
        std::optional<std::u16string> text = shown ? result.get() : std::optional(request.text);
        input::SetTextEntry(false);
        s_holdUntilReleased = true;
        s_open = false;
        LOGI("Keyboard: {}", text ? fmt::format("entered {} characters", text->size()) : std::string("cancelled"));
        return text;
    }

    bool KeyboardHoldsInput()
    {
        if (s_open)
            return true;
        if (!s_holdUntilReleased)
            return false;
        if (PadButtons() != 0)
            return true;
        s_holdUntilReleased = false;
        return false;
    }
}
