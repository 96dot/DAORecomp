// The notice (notice.h): a line on the main menu's highlighted plate, where the
// cheats menu shows its messages, fading out at the end. Drawn by the UI thread;
// it takes no input, so the game goes on under it.
#include "stdafx.h"
#include "ui/notice.h"

#include "gpu/xenia_host.h"
#include "ui/overlay.h"

#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/windowed_app_context.h"

namespace dao::ui
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        // On the game's 1280x720 stage, as the cheats menu's messages; seconds shown
        // (long enough to read twice), and the fade at the end.
        constexpr float kNoticeY = 606.0f, kNoticeSize = 19.0f;
        constexpr auto kNoticeTime = std::chrono::seconds(6);
        constexpr double kNoticeFade = 0.5;

        class NoticeDialog;
        NoticeDialog* s_notice = nullptr; // UI thread

        class NoticeDialog final : public xe::ui::ImGuiDialog
        {
        public:
            NoticeDialog(Overlay& overlay, std::string text) : ImGuiDialog(overlay.drawer.get()), overlay_(overlay)
            {
                Replace(std::move(text));
            }

            void Replace(std::string text)
            {
                text_ = std::move(text);
                until_ = Clock::now() + kNoticeTime;
            }

        protected:
            void OnDraw(ImGuiIO& io) override
            {
                const double left = std::chrono::duration<double>(until_ - Clock::now()).count();
                if (left <= 0)
                {
                    Close();
                    return;
                }
                ImGui::SetNextWindowPos(ImVec2(0, 0));
                ImGui::SetNextWindowSize(io.DisplaySize);
                ImGui::Begin("##notice", nullptr,
                    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                        ImGuiWindowFlags_NoScrollWithMouse);
                ImDrawList* draw = ImGui::GetWindowDrawList();
                draw->PushClipRectFullScreen();
                DrawMessage(draw, overlay_, Stage(io), overlay_.italic, kNoticeSize, kNoticeY, text_,
                    float(std::min(1.0, left / kNoticeFade)));
                draw->PopClipRect();
                ImGui::End();
            }

            void OnClose() override
            {
                s_notice = nullptr;
            }

        private:
            Overlay& overlay_;
            std::string text_;
            Clock::time_point until_;
        };
    }

    void ShowNotice(std::string text)
    {
        const gpu::XeniaWindow host = gpu::GetXeniaWindow();
        if (!host.window)
            return;
        host.appContext->CallInUIThread([host, text = std::move(text)]() mutable {
            if (s_notice)
                s_notice->Replace(std::move(text));
            else if (Overlay* overlay = GetOverlay(host))
                s_notice = new NoticeDialog(*overlay, std::move(text)); // deletes itself when closed
        });
    }
}
