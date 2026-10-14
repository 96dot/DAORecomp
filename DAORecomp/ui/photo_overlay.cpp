// Photo mode's overlay: the controls along the bottom, a settings panel (field of
// view, roll, camera speed, frame, grid), the frame's bars and grid over the
// scene, and taking the photo (the game's image without the overlay, cropped to
// the frame, saved as a PNG in Pictures\DAORecomp). Drawn by the UI thread; the
// controller's d-pad, face buttons, START and BACK are read here (the game doesn't
// see them in photo mode, photo_mode.cpp), the triggers and sticks by the camera.
//
// The game's HDR stays HDR on screen (the presenter puts the overlay over it, its
// white at paper white on HDR displays); photos of it are tonemapped to SDR.
#include "stdafx.h"
#include "ui/photo_mode.h"

#include <shlobj.h>
#include <cmath>
#include <numbers>

#include "gpu/xenia_host.h"
#include "input/input.h"
#include "text.h"
#include "ui/overlay.h"
#include "ui/palette.h"

#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app_context.h"

namespace fs = std::filesystem;

namespace dao::ui
{
    namespace
    {
        using namespace palette;
        using namespace input;

        // The settings' ranges and steps (a step per press of left or right); the
        // field of view in degrees across a 16:9 image.
        constexpr double kPhotoMinFov = 5.0, kPhotoMaxFov = 130.0, kFovStep = 1.0;
        constexpr double kMaxRoll = 90.0, kRollStep = 1.0;
        constexpr double kSpeeds[] = { 0.1, 0.25, 0.5, 1.0, 2.0, 4.0 };
        constexpr size_t kNormalSpeed = 3;
        // Held d-pad: zoom (tan of half the field of view times e^rate a second) and
        // roll (degrees a second).
        constexpr double kZoomRate = 0.9, kRollRate = 30.0;

        struct Frame
        {
            const char* name;
            double aspect; // width / height; 0: the whole image
        };
        constexpr Frame kFrames[] = { { "Full Screen", 0 }, { "16:9", 16.0 / 9.0 }, { "21:9", 21.0 / 9.0 },
            { "32:9", 32.0 / 9.0 }, { "3:2", 3.0 / 2.0 }, { "4:3", 4.0 / 3.0 }, { "5:4", 5.0 / 4.0 },
            { "1:1", 1.0 }, { "4:5", 4.0 / 5.0 }, { "2:3", 2.0 / 3.0 }, { "9:16", 9.0 / 16.0 } };
        enum class Grid
        {
            Off,
            Thirds,
            GoldenRatio,
            CentreCross,
            Count,
        };
        constexpr const char* kGridNames[] = { "Off", "Rule of Thirds", "Golden Ratio", "Centre Cross" };
        static_assert(std::size(kGridNames) == size_t(Grid::Count));

        enum class Row
        {
            Fov,
            Roll,
            Speed,
            Frame,
            Grid,
            Count,
        };
        constexpr const char* kRowNames[] = { "Field of View", "Roll", "Camera Speed", "Frame", "Grid" };

        // Laid out on the game's 1280x720 stage. The panel: on shading fading to
        // the right, the save screen's title plate, then a main-menu plate a row (the
        // highlighted one for the selected row), the pop-ups' divider under them.
        constexpr float kPanelLeft = 40.0f, kPanelWidth = 400.0f, kPanelTop = 40.0f, kShadeWidth = 560.0f;
        constexpr float kTitleWidth = 360.0f, kTitleMinHeight = 60.0f, kTitleSize = 26.0f;
        constexpr float kRowGap = 6.0f, kRowHeight = 44.0f, kRowTextSize = 20.0f, kInset = 26.0f;
        constexpr float kPlateEnds = 0.09f;
        constexpr float kArrowHeight = 22.0f, kValueWidth = 150.0f;
        constexpr float kDividerWidth = 380.0f;
        // The controls along the bottom, on the pop-ups' bar.
        constexpr float kBarLeft = 30.0f, kBarTop = 664.0f, kBarBottom = 708.0f, kHintTop = 673.0f;
        constexpr float kBarEnds = 0.06f;
        constexpr HintStyle kHintStyle{ 26.0f, 5.0f, 16.0f, 16.0f };
        // Messages (on a highlighted plate): what was saved, over the bar, and the
        // setting the d-pad changes, at the top.
        constexpr float kToastY = 606.0f, kToastSize = 19.0f, kReadoutY = 22.0f, kReadoutSize = 22.0f;
        constexpr float kMessagePad = 34.0f;
        constexpr float kCrossArm = 18.0f;
        // Seconds: how long messages and the flash last, and how long the toast and
        // the readout take to fade out at the end.
        constexpr double kToastTime = 3.0, kReadoutTime = 1.2, kFlashTime = 0.25;
        constexpr double kToastFade = 0.5, kReadoutFade = 0.3;
        // Held d-pad on the panel: the first repeat, then every.
        constexpr double kRepeatDelay = 0.35, kRepeatEvery = 0.07;
        // The longest frame time the d-pad's zoom and roll take (a hitch doesn't
        // jump them).
        constexpr double kMaxStep = 0.1;

        constexpr uint16_t kDirections = kButtonDpadUp | kButtonDpadDown | kButtonDpadLeft | kButtonDpadRight;

        // The settings (the overlay writes them on the UI thread, the camera reads
        // them on the game thread).
        std::atomic<double> s_fov{ 0 };
        std::atomic<double> s_roll{ 0 };
        std::atomic<double> s_speed{ 1 };
        std::atomic<bool> s_reset{ false };
        double s_startFov = 0; // UI thread, as photo mode started

        // A photo being saved (its thread), and how it went.
        std::atomic<bool> s_saving{ false };
        std::mutex s_toastMutex;
        std::string s_toast; // set by the saving thread, shown by the overlay
        bool s_toastNew = false;

        // The folder photos go to: DAO_PHOTO_DIR (tests), or Pictures\DAORecomp.
        fs::path PhotoFolder()
        {
            if (const wchar_t* dir = _wgetenv(L"DAO_PHOTO_DIR"))
                return fs::path(dir);
            PWSTR pictures = nullptr;
            fs::path folder;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pictures)))
                folder = fs::path(pictures) / L"DAORecomp";
            CoTaskMemFree(pictures);
            return folder;
        }

        void ShowToast(std::string text)
        {
            std::lock_guard lock(s_toastMutex);
            s_toast = std::move(text);
            s_toastNew = true;
        }

        // On a thread of its own: reading the image back and the PNG take a while.
        // False if one is still being saved.
        bool SavePhoto(double aspect)
        {
            if (s_saving.exchange(true))
                return false;
            std::thread([aspect] {
                const fs::path folder = PhotoFolder();
                std::error_code ec;
                bool saved = false;
                fs::path path;
                if (!folder.empty())
                    fs::create_directories(folder, ec);
                if (!folder.empty() && fs::is_directory(folder, ec))
                {
                    SYSTEMTIME time;
                    GetLocalTime(&time);
                    const std::wstring stem = std::format(L"DAO {:04}-{:02}-{:02} {:02}-{:02}-{:02}", time.wYear,
                        time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
                    path = folder / (stem + L".png");
                    for (int i = 2; fs::exists(path, ec) && i < 100; ++i)
                        path = folder / std::format(L"{} ({}).png", stem, i);
                    saved = gpu::CaptureFrame(path, aspect, true);
                }
                // (Not the whole path in the log: it has the Windows user's name.)
                const std::string shown = Utf8(folder.filename()) + "\\" + Utf8(path.filename());
                if (saved)
                {
                    LOGI("Photo mode: saved {}", shown);
                    ShowToast("Photo saved to " + shown);
                }
                else
                {
                    LOGW("Photo mode: couldn't save the photo in {}", Utf8(folder.filename()));
                    ShowToast("The photo couldn't be saved");
                }
                s_saving = false;
            }).detach();
            return true;
        }

        // tan(fov / 2) scaled by `factor`, as a field of view in the range.
        double ScaleFov(double degrees, double factor)
        {
            const double half = std::atan(std::tan(degrees * std::numbers::pi / 360.0) * factor);
            return std::clamp(half * 360.0 / std::numbers::pi, kPhotoMinFov, kPhotoMaxFov);
        }

        class PhotoDialog final : public xe::ui::ImGuiDialog
        {
        public:
            explicit PhotoDialog(Overlay& overlay) : ImGuiDialog(overlay.drawer.get()), overlay_(overlay)
            {
                // Buttons already down (the menu's A) act once let go.
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
                    HandlePad(std::clamp(double(io.DeltaTime), 0.0, kMaxStep));
                {
                    std::lock_guard lock(s_toastMutex);
                    if (std::exchange(s_toastNew, false))
                    {
                        toast_ = s_toast;
                        toastUntil_ = now_ + kToastTime;
                    }
                }

                ImGui::SetNextWindowPos(ImVec2(0, 0));
                ImGui::SetNextWindowSize(io.DisplaySize);
                ImGui::Begin("##photo", nullptr,
                    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                        ImGuiWindowFlags_NoScrollWithMouse);
                // The whole screen: a window clips to its inside (a few pixels in from
                // its edges), which the bars would leave the game's image showing past.
                ImDrawList* draw = ImGui::GetWindowDrawList();
                draw->PushClipRectFullScreen();
                DrawFrame(draw);
                if (!hidden_)
                {
                    if (panelOpen_)
                        DrawPanel(draw);
                    else
                        DrawReadout(draw);
                    DrawControls(draw);
                    DrawToast(draw);
                }
                DrawFlash(draw, io);
                draw->PopClipRect();
                ImGui::End();
            }

        private:
            ImVec2 At(float x, float y) const { return stage_.At(x, y); }
            float Px(float size) const { return size * stage_.fit; }

            // The frame's corners on the screen: the game's image (16:9, as the
            // stage) or the middle of it at the frame's aspect, as the photo is cut.
            std::pair<ImVec2, ImVec2> FrameRect() const
            {
                const ImVec2 min = At(0, 0), max = At(Stage::kWidth, Stage::kHeight);
                const auto [w, h] = gpu::FrameSize(max.x - min.x, max.y - min.y, kFrames[frame_].aspect);
                const ImVec2 middle((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
                return { ImVec2(middle.x - float(w) * 0.5f, middle.y - float(h) * 0.5f),
                    ImVec2(middle.x + float(w) * 0.5f, middle.y + float(h) * 0.5f) };
            }

            // Black bars outside the frame (what the photo leaves out), its edge,
            // and the grid in it.
            void DrawFrame(ImDrawList* draw)
            {
                const auto [min, max] = FrameRect();
                const ImVec2 screen = ImGui::GetIO().DisplaySize;
                const ImU32 bar = IM_COL32(0, 0, 0, 255);
                draw->AddRectFilled(ImVec2(0, 0), ImVec2(screen.x, min.y), bar);
                draw->AddRectFilled(ImVec2(0, max.y), screen, bar);
                draw->AddRectFilled(ImVec2(0, min.y), ImVec2(min.x, max.y), bar);
                draw->AddRectFilled(ImVec2(max.x, min.y), ImVec2(screen.x, max.y), bar);
                if (hidden_)
                    return;
                const float line = std::max(1.0f, stage_.fit);
                if (kFrames[frame_].aspect > 0)
                    draw->AddRect(min, max, WithAlpha(kGold, 0.7f), 0, 0, line);
                const ImU32 color = IM_COL32(255, 255, 255, 90);
                const auto lines = [&](float a, float b) {
                    for (const float f : { a, b })
                    {
                        const float x = min.x + (max.x - min.x) * f, y = min.y + (max.y - min.y) * f;
                        draw->AddLine(ImVec2(x, min.y), ImVec2(x, max.y), color, line);
                        draw->AddLine(ImVec2(min.x, y), ImVec2(max.x, y), color, line);
                    }
                };
                switch (Grid(grid_))
                {
                case Grid::Thirds:
                    lines(1.0f / 3.0f, 2.0f / 3.0f);
                    break;
                case Grid::GoldenRatio:
                    lines(0.382f, 0.618f);
                    break;
                case Grid::CentreCross:
                {
                    const ImVec2 middle((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
                    const float arm = Px(kCrossArm);
                    draw->AddLine(ImVec2(middle.x - arm, middle.y), ImVec2(middle.x + arm, middle.y), color, line);
                    draw->AddLine(ImVec2(middle.x, middle.y - arm), ImVec2(middle.x, middle.y + arm), color, line);
                    break;
                }
                default:
                    break;
                }
            }

            std::string Value(Row row) const
            {
                switch (row)
                {
                case Row::Fov:
                    return fmt::format("{:.0f}\xC2\xB0", s_fov.load());
                case Row::Roll:
                    return fmt::format("{:.0f}\xC2\xB0", s_roll.load());
                case Row::Speed:
                    return fmt::format("{:g}x", kSpeeds[speed_]);
                case Row::Frame:
                    return kFrames[frame_].name;
                case Row::Grid:
                    return kGridNames[grid_];
                case Row::Count:
                    break;
                }
                return {};
            }

            // The settings, down the left.
            void DrawPanel(ImDrawList* draw)
            {
                const float titleHeight = std::max(ArtHeight(overlay_, art::kTitlePlate, kTitleWidth), kTitleMinHeight);
                const float rowsTop = kPanelTop + titleHeight + kRowGap;
                const float rowsBottom = rowsTop + int(Row::Count) * (kRowHeight + kRowGap);
                const float dividerHeight = ArtHeight(overlay_, art::kDivider, kDividerWidth);

                // Shading behind it, fading out to the right.
                const ImVec2 shadeMin = At(0, kPanelTop - 20.0f);
                const ImVec2 shadeMax = At(kPanelLeft + kShadeWidth, rowsBottom + dividerHeight + 10.0f);
                draw->AddRectFilledMultiColor(ImVec2(0, shadeMin.y), shadeMax, IM_COL32(0, 0, 0, 200),
                    IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 200));

                // The title on its plate.
                const float middle = kPanelLeft + kPanelWidth * 0.5f;
                DrawArt(draw, overlay_, art::kTitlePlate, At(middle - kTitleWidth * 0.5f, kPanelTop),
                    At(middle + kTitleWidth * 0.5f, kPanelTop + titleHeight));
                const float titleSize = Px(kTitleSize);
                constexpr const char* kTitle = "Photo Mode";
                const ImVec2 titleExtent = overlay_.bold->CalcTextSizeA(titleSize, FLT_MAX, 0, kTitle);
                const ImVec2 titleMiddle = At(middle, kPanelTop + titleHeight * 0.5f);
                ShadowText(draw, overlay_.bold, titleSize,
                    ImVec2(titleMiddle.x - titleExtent.x * 0.5f, titleMiddle.y - titleExtent.y * 0.5f), kGoldBright,
                    kTitle);

                for (int i = 0; i < int(Row::Count); ++i)
                {
                    const float top = rowsTop + i * (kRowHeight + kRowGap);
                    const ImVec2 rowMin = At(kPanelLeft, top), rowMax = At(kPanelLeft + kPanelWidth, top + kRowHeight);
                    const bool selected = i == row_;
                    DrawArtSliced(draw, overlay_, selected ? art::kPlateLit : art::kPlate, rowMin, rowMax, kPlateEnds,
                        selected ? 1.0f : 0.85f);
                    const float size = Px(kRowTextSize);
                    const float textY = (rowMin.y + rowMax.y - size) * 0.5f;
                    ShadowText(draw, overlay_.bold, size, ImVec2(At(kPanelLeft + kInset, 0).x, textY),
                        selected ? kGoldBright : kText, kRowNames[i]);

                    // The value, between the Options screen's arrows when selected.
                    const std::string value = Value(Row(i));
                    const float right = At(kPanelLeft + kPanelWidth - kInset, 0).x;
                    const float left = right - Px(kValueWidth);
                    const ImVec2 extent = overlay_.regular->CalcTextSizeA(size, FLT_MAX, 0, value.c_str());
                    ShadowText(draw, overlay_.regular, size, ImVec2((left + right - extent.x) * 0.5f, textY),
                        selected ? kText : kTextDim, value);
                    const float arrowHeightPerWidth = ArtHeight(overlay_, art::kArrowLeft, 1.0f);
                    if (selected && arrowHeightPerWidth > 0)
                    {
                        const float h = Px(kArrowHeight), w = h / arrowHeightPerWidth;
                        const float y = (rowMin.y + rowMax.y - h) * 0.5f;
                        DrawArt(draw, overlay_, art::kArrowLeft, ImVec2(left, y), ImVec2(left + w, y + h));
                        DrawArt(draw, overlay_, art::kArrowLeft, ImVec2(right - w, y), ImVec2(right, y + h), 1.0f, true);
                    }
                }

                DrawArt(draw, overlay_, art::kDivider, At(middle - kDividerWidth * 0.5f, rowsBottom),
                    At(middle + kDividerWidth * 0.5f, rowsBottom + dividerHeight));
            }

            // A line of text centred on a highlighted plate, faded by `alpha`.
            void DrawMessage(ImDrawList* draw, ImFont* font, float size, float y, std::string_view text, float alpha)
            {
                const float pixels = Px(size);
                const ImVec2 extent = font->CalcTextSizeA(pixels, FLT_MAX, 0, text.data(), text.data() + text.size());
                const float middle = At(Stage::kWidth * 0.5f, 0).x, top = At(0, y).y, pad = Px(kMessagePad);
                const float height = extent.y * 1.9f;
                DrawArtSliced(draw, overlay_, art::kPlateLit, ImVec2(middle - extent.x * 0.5f - pad, top),
                    ImVec2(middle + extent.x * 0.5f + pad, top + height), kPlateEnds, alpha);
                ShadowText(draw, font, pixels, ImVec2(middle - extent.x * 0.5f, top + (height - extent.y) * 0.5f),
                    WithAlpha(kText, alpha), text);
            }

            // With the panel closed: the field of view or roll for a moment after
            // the d-pad changes them.
            void DrawReadout(ImDrawList* draw)
            {
                if (now_ >= readoutUntil_)
                    return;
                const float alpha = float(std::min(1.0, (readoutUntil_ - now_) / kReadoutFade));
                DrawMessage(draw, overlay_.bold, kReadoutSize, kReadoutY,
                    fmt::format("{}  {}", kRowNames[int(readout_)], Value(readout_)), alpha);
            }

            // The controls along the bottom, on the pop-ups' bar.
            void DrawControls(ImDrawList* draw)
            {
                DrawArtSliced(draw, overlay_, art::kBar, At(kBarLeft, kBarTop), At(Stage::kWidth - kBarLeft, kBarBottom),
                    kBarEnds, 0.95f);
                std::vector<Hint> hints = { { "Move", art::kLeftStick }, { "Look", art::kRightStick },
                    { "Down / Up", art::kButtonLT, art::kButtonRT } };
                if (panelOpen_)
                    hints.insert(hints.end(), { { "Select", art::kDpadUp, art::kDpadDown },
                                                  { "Change", art::kDpadLeft, art::kDpadRight },
                                                  { "Take Photo", art::kButtonA }, { "Close", art::kButtonX } });
                else
                    hints.insert(hints.end(), { { "Zoom", art::kDpadUp, art::kDpadDown },
                                                  { "Roll", art::kDpadLeft, art::kDpadRight },
                                                  { "Take Photo", art::kButtonA }, { "Settings", art::kButtonX } });
                hints.insert(hints.end(), { { "Hide UI", art::kButtonY }, { "Reset", art::kButtonBack },
                                              { panelOpen_ ? "Back" : "Exit", art::kButtonB } });
                const float width = HintsWidth(overlay_, stage_, hints, kHintStyle);
                ui::DrawHints(draw, overlay_, stage_, hints, kHintStyle, At(Stage::kWidth * 0.5f, 0).x - width * 0.5f,
                    At(0, kHintTop).y);
            }

            void DrawToast(ImDrawList* draw)
            {
                if (now_ >= toastUntil_)
                    return;
                const float alpha = float(std::min(1.0, (toastUntil_ - now_) / kToastFade));
                DrawMessage(draw, overlay_.italic, kToastSize, kToastY, toast_, alpha);
            }

            // The shutter: a white flash fading out.
            void DrawFlash(ImDrawList* draw, const ImGuiIO& io)
            {
                if (now_ >= flashUntil_)
                    return;
                const float alpha = float((flashUntil_ - now_) / kFlashTime) * 0.6f;
                draw->AddRectFilled(ImVec2(0, 0), io.DisplaySize, WithAlpha(IM_COL32_WHITE, alpha));
            }

            void ShowReadout(Row row)
            {
                readout_ = row;
                readoutUntil_ = now_ + kReadoutTime;
            }

            // Left (-1) or right (1) on the panel's selected row.
            void Change(int step)
            {
                switch (Row(row_))
                {
                case Row::Fov:
                    s_fov = std::clamp(std::round(s_fov.load()) + step * kFovStep, kPhotoMinFov, kPhotoMaxFov);
                    break;
                case Row::Roll:
                    s_roll = std::clamp(std::round(s_roll.load()) + step * kRollStep, -kMaxRoll, kMaxRoll);
                    break;
                case Row::Speed:
                    speed_ = size_t(std::clamp(int(speed_) + step, 0, int(std::size(kSpeeds)) - 1));
                    s_speed = kSpeeds[speed_];
                    break;
                case Row::Frame:
                    frame_ = (frame_ + std::size(kFrames) + step) % std::size(kFrames);
                    break;
                case Row::Grid:
                    grid_ = (grid_ + size_t(Grid::Count) + step) % size_t(Grid::Count);
                    break;
                case Row::Count:
                    break;
                }
            }

            void Reset()
            {
                s_fov = s_startFov;
                s_roll = 0.0;
                s_reset = true;
            }

            // Once: another press before the overlay closes would enter again.
            void Exit()
            {
                leaving_ = true;
                TogglePhotoMode();
            }

            void HandlePad(double seconds)
            {
                PadState pad;
                if (!GetState(0, pad))
                    return;
                const uint16_t buttons = pad.buttons;
                const uint16_t pressed = buttons & ~previous_;
                previous_ = buttons;

                if (hidden_)
                {
                    // Anything of the overlay's shows it again (A still takes a photo).
                    if (pressed & (kButtonY | kButtonB | kButtonX | kButtonStart))
                    {
                        hidden_ = false;
                        return;
                    }
                }
                else if (pressed & kButtonY)
                    hidden_ = true;

                if ((pressed & kButtonA) && SavePhoto(kFrames[frame_].aspect))
                    flashUntil_ = now_ + kFlashTime;
                if (pressed & kButtonStart)
                {
                    Exit();
                    return;
                }
                if (pressed & kButtonB)
                {
                    if (panelOpen_)
                        panelOpen_ = false;
                    else
                        Exit();
                    return;
                }
                if (pressed & kButtonX)
                    panelOpen_ = !panelOpen_;
                if (pressed & kButtonBack)
                    Reset();

                const uint16_t direction = buttons & kDirections;
                if (panelOpen_ && !hidden_)
                {
                    // A step a press, repeating while held.
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
                    return;
                }
                // Zoom and roll while held.
                const int zoom = ((direction & kButtonDpadDown) ? 1 : 0) - ((direction & kButtonDpadUp) ? 1 : 0);
                if (zoom)
                {
                    s_fov = ScaleFov(s_fov.load(), std::exp(zoom * kZoomRate * seconds));
                    ShowReadout(Row::Fov);
                }
                const int roll = ((direction & kButtonDpadRight) ? 1 : 0) - ((direction & kButtonDpadLeft) ? 1 : 0);
                if (roll)
                {
                    s_roll = std::clamp(s_roll.load() + roll * kRollRate * seconds, -kMaxRoll, kMaxRoll);
                    ShowReadout(Row::Roll);
                }
            }

            Overlay& overlay_;
            Stage stage_;
            double now_ = 0;
            uint16_t previous_ = 0;
            double nextRepeat_ = 0;
            bool panelOpen_ = false;
            bool hidden_ = false;
            bool leaving_ = false; // exit asked for
            int row_ = 0;
            size_t speed_ = kNormalSpeed;
            size_t frame_ = 0;
            size_t grid_ = 0;
            Row readout_ = Row::Fov;
            double readoutUntil_ = 0;
            std::string toast_;
            double toastUntil_ = 0;
            double flashUntil_ = 0;
        };

        PhotoDialog* s_dialog = nullptr; // UI thread
    }

    namespace photo
    {
        CameraSettings GetCameraSettings()
        {
            return { s_fov.load(std::memory_order_relaxed), s_roll.load(std::memory_order_relaxed),
                s_speed.load(std::memory_order_relaxed) };
        }

        bool TakeResetRequest()
        {
            return s_reset.exchange(false, std::memory_order_relaxed);
        }

        void ShowOverlay(double fov)
        {
            s_fov = fov;
            s_roll = 0.0;
            s_speed = kSpeeds[kNormalSpeed];
            s_reset = false;
            const gpu::XeniaWindow host = gpu::GetXeniaWindow();
            if (!host.window)
            {
                LOGW("Photo mode: no window, leaving");
                TogglePhotoMode();
                return;
            }
            host.appContext->CallInUIThread([host, fov] {
                s_startFov = fov;
                if (s_dialog)
                    return;
                if (Overlay* overlay = GetOverlay(host))
                    s_dialog = new PhotoDialog(*overlay); // deletes itself when closed
                else
                {
                    // No controls to show: out again rather than the buttons taken.
                    LOGW("Photo mode: no overlay, leaving");
                    TogglePhotoMode();
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
