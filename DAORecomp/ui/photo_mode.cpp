// Photo mode. The game's mode controller runs one game mode at a time; the 360
// build still has the development fly camera as mode 6 (GameModeFlyCam, vtable
// 8217B840), which hides the HUD and moves the camera with the sticks. Photo mode
// asks the controller for it the way the engine's SetGameModeMessage does
// (sub_82317FA8: RequestGameMode(controller, mode, 0, 0)), from the controller's
// own update, pauses the world, and asks for the mode it left to come back.
//
// On top of the fly camera (left stick moves, right stick turns, the bumpers
// slow and speed it up): the triggers lower and raise it, and the overlay
// (photo_overlay.cpp) sets its field of view, roll and speed. The d-pad, the face
// buttons, START and BACK are the overlay's: the game doesn't see them (the fly
// camera's own d-pad pans across the level).
#include "stdafx.h"
#include "ui/photo_mode.h"
#include "ui/menu_movies.h"
#include "gpu/camera_fov.h"
#include "input/input.h"
#include "kernel/function.h"
#include "kernel/game_build.h"
#include "kernel/game_modes.h"
#include "kernel/thread.h"

#include <cmath>
#include <numbers>

namespace
{
    using namespace dao::input;
    using namespace dao::kernel;

    // RequestGameMode(controller, mode, argument, data): queues the switch (a mode
    // that switches at once, as the fly camera, replaces any queued one).
    constexpr dao::kernel::GameAddress kRequestGameMode{ 0x82316BB0, 0x82348010 };

    // (Photo mode starts from exploring and combat: kernel/game_modes.h.)

    // The game (a pointer): its "Paused" attribute's value (a byte) at +232. SetPaused(game, paused, byUser) pauses the world
    // (the TogglePause message's handler, sub_823180C8, passes byUser = 1).
    constexpr dao::kernel::GameAddress kGame{ 0x83211BB0, 0x832E20E8 };
    constexpr uint32_t kPausedOffset = 232;
    constexpr dao::kernel::GameAddress kSetPaused{ 0x822659D0, 0x82285470 };

    // The fly camera mode's camera object (what it moves) is at +92: its position
    // (x, y, z, w; z is up) at +112, its orientation (a quaternion x, y, z, w; +y
    // ahead) at +144. SetPosition(object, position, blend seconds, 0) and
    // SetOrientation(object, quaternion) move it as the fly camera does (both read
    // their vector with a 16-byte aligned load).
    constexpr uint32_t kFlyCamObjectOffset = 92;
    constexpr uint32_t kPositionOffset = 112;
    constexpr uint32_t kOrientationOffset = 144;
    constexpr dao::kernel::GameAddress kSetPosition{ 0x82340118, 0x82370198 };
    constexpr dao::kernel::GameAddress kSetOrientation{ 0x82340430, 0x823704B0 };

    // The fly camera's input actions (the input map's): the left stick's two axes.
    constexpr uint32_t kActionMoveX = 43;
    constexpr uint32_t kActionMoveY = 44;

    // Height from the triggers as fast as the fly camera moves on the stick, in
    // units (metres) a second: its 6, 0.18 with LB, 132 with RB.
    constexpr double kHeightSpeed = 6.0, kHeightSpeedSlow = 0.18, kHeightSpeedFast = 132.0;
    constexpr int kTriggerDeadZone = 30; // XINPUT_GAMEPAD_TRIGGER_THRESHOLD

    // How long a toggle asked for in a menu waits for it to close (the START
    // menu's row closes its menu, then asks).
    constexpr auto kMenuWait = std::chrono::seconds(1);
    // How long a pending exit has before another exit asks again.
    constexpr auto kRetryLeave = std::chrono::milliseconds(500);
    // The longest frame time the triggers' height change takes (a hitch doesn't
    // jump it).
    constexpr double kMaxStep = 0.1;

    // The buttons the game doesn't see in photo mode (the overlay's).
    constexpr uint16_t kPhotoButtons = kButtonA | kButtonB | kButtonX | kButtonY | kButtonStart | kButtonBack |
        kButtonDpadUp | kButtonDpadDown | kButtonDpadLeft | kButtonDpadRight;

    // Asked for (any thread): the time, or 0.
    std::atomic<int64_t> s_toggleAt{ 0 };
    // The game's buttons are photo mode's (any thread); those held as it ended.
    std::atomic<bool> s_takesButtons{ false };
    std::atomic<uint16_t> s_heldOver{ 0 };

    // Mode update thread only.
    enum class State
    {
        Off,
        Entering, // the fly camera asked for
        On,
        Leaving,  // the previous mode asked for
    };
    State s_state = State::Off;
    int32_t s_returnMode = kModeExplore; // the mode photo mode came from
    bool s_pausedHere = false;           // photo mode paused the world
    int64_t s_leftAt = 0;                // when Leaving was asked for

    // The camera where photo mode started (to go back to), and the roll given it
    // so far (game thread).
    struct Pose
    {
        uint32_t position[4];
        uint32_t orientation[4];
    };
    std::optional<Pose> s_start;
    double s_appliedRoll = 0;

    // In the fly camera's update (this thread).
    thread_local bool t_inFlyCam = false;
    std::atomic<double> s_speed{ 1.0 };

    // Nanoseconds (never 0).
    int64_t Now()
    {
        using namespace std::chrono;
        return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count() | 1;
    }

    int32_t CurrentMode(uint8_t* base, uint32_t controller)
    {
        const uint32_t current = PPC_LOAD_U32(controller + kCurrentModeOffset);
        return current ? int32_t(PPC_LOAD_U32(current + kModeNumberOffset)) : -1;
    }

    void SetPaused(uint32_t game, bool paused)
    {
        dao::kernel::CallGuest(kSetPaused.Get(), { game, paused ? 1u : 0u, 0 });
    }

    // Back to the game's state: the world unpaused if photo mode paused it, the
    // buttons and the field of view the game's again.
    void Finish(uint8_t* base, uint32_t game)
    {
        if (s_pausedHere && game && PPC_LOAD_U8(game + kPausedOffset))
            SetPaused(game, false);
        s_pausedHere = false;
        s_takesButtons = false;
        dao::gpu::SetPhotoFov(0);
        dao::ui::photo::CloseOverlay();
        s_state = State::Off;
    }

    void Enter(uint8_t* base, uint32_t controller, uint32_t game, int32_t mode)
    {
        s_returnMode = mode;
        s_pausedHere = !PPC_LOAD_U8(game + kPausedOffset);
        LOGI("Photo mode: on (from mode {}{})", mode, s_pausedHere ? "" : ", already paused");
        dao::kernel::CallGuest(kRequestGameMode.Get(), { controller, uint32_t(kModeFlyCam), 0, 0 });
        s_takesButtons = true;
        s_start.reset();
        s_appliedRoll = 0;
        s_state = State::Entering;
    }

    void Leave(uint32_t controller)
    {
        LOGI("Photo mode: off (back to mode {})", s_returnMode);
        dao::kernel::CallGuest(kRequestGameMode.Get(), { controller, uint32_t(s_returnMode), 0, 0 });
        s_state = State::Leaving;
        s_leftAt = Now();
    }

    // The mode controller's update (applies the queued switches): r3 is the
    // controller.
    void ModeUpdateHook(PPCContext& ctx, uint8_t* base, PPCFunc* original)
    {
        const uint32_t controller = ctx.r3.u32;
        const uint32_t game = PPC_LOAD_U32(kGame.Get());
        const int64_t toggleAt = s_toggleAt.load(std::memory_order_relaxed);
        if (toggleAt && game)
        {
            const int32_t mode = CurrentMode(base, controller);
            const bool stale = Now() - toggleAt > std::chrono::nanoseconds(kMenuWait).count();
            if (stale)
            {
                // Asked for in a menu that didn't close, or long ago (the mode
                // controller didn't update meanwhile: loading): not taken.
                s_toggleAt = 0;
                if (s_state == State::Off)
                    LOGI("Photo mode: not available in mode {}", mode);
            }
            else if (s_state == State::On || s_state == State::Leaving)
            {
                // Asked again only if the last ask hasn't taken for a while (another
                // exit pressed meanwhile is the same exit).
                s_toggleAt = 0;
                if (s_state == State::On || Now() - s_leftAt > std::chrono::nanoseconds(kRetryLeave).count())
                    Leave(controller);
            }
            else if (s_state == State::Off && (mode == kModeExplore || mode == kModeCombat))
            {
                s_toggleAt = 0;
                Enter(base, controller, game, mode);
            }
            else if (s_state != State::Off || mode != kModeMenu)
            {
                // Mid-switch, or in a conversation, a cutscene, loading, ...
                s_toggleAt = 0;
                if (s_state == State::Off)
                    LOGI("Photo mode: not available in mode {}", mode);
            }
        }

        original(ctx, base);

        // Where the switches got to.
        const int32_t mode = CurrentMode(base, controller);
        switch (s_state)
        {
        case State::Entering:
            if (mode == kModeFlyCam)
            {
                // Paused once in the fly camera (a mode switch can unpause).
                s_state = State::On;
                if (s_pausedHere)
                    SetPaused(game, true);
                dao::ui::photo::ShowOverlay(dao::gpu::GameplayFovDegrees());
            }
            else
            {
                LOGW("Photo mode: the game stayed in mode {}", mode);
                Finish(base, game);
            }
            break;
        case State::On:
        case State::Leaving:
            // Left (by photo mode, or the game: a cutscene or a load starting).
            if (mode != kModeFlyCam)
            {
                if (s_state == State::On)
                    LOGI("Photo mode: ended by the game (mode {})", mode);
                Finish(base, game);
            }
            break;
        case State::Off:
            break;
        }
    }

    // A float of guest memory.
    double LoadFloat(uint8_t* base, uint32_t address)
    {
        return std::bit_cast<float>(PPC_LOAD_U32(address));
    }

    // q * r (Hamilton; x, y, z, w).
    std::array<double, 4> Multiply(const std::array<double, 4>& q, const std::array<double, 4>& r)
    {
        return { q[3] * r[0] + q[0] * r[3] + q[1] * r[2] - q[2] * r[1],
            q[3] * r[1] - q[0] * r[2] + q[1] * r[3] + q[2] * r[0],
            q[3] * r[2] + q[0] * r[1] - q[1] * r[0] + q[2] * r[3],
            q[3] * r[3] - q[0] * r[0] - q[1] * r[1] - q[2] * r[2] };
    }

    // A vector (big-endian floats' bits) in guest memory for SetPosition or
    // SetOrientation: below the hooked function's stack pointer (nothing lives
    // there at its entry or exit, and CallGuest's frames start 0x100 below it).
    uint32_t StoreVector(PPCContext& ctx, uint8_t* base, const uint32_t (&words)[4])
    {
        const uint32_t vector = (ctx.r1.u32 - 32) & ~0xFu;
        for (uint32_t i = 0; i < 4; ++i)
            PPC_STORE_U32(vector + i * 4, words[i]);
        return vector;
    }

    void SetPosition(PPCContext& ctx, uint8_t* base, uint32_t object, const uint32_t (&position)[4])
    {
        // At once (no blend).
        dao::kernel::CallGuest(kSetPosition.Get(), { object, StoreVector(ctx, base, position), 0, 0 }, { 0.0 });
    }

    void SetOrientation(PPCContext& ctx, uint8_t* base, uint32_t object, const uint32_t (&orientation)[4])
    {
        dao::kernel::CallGuest(kSetOrientation.Get(), { object, StoreVector(ctx, base, orientation) });
    }

    // The fly camera's update: r3 is the mode, f1 the frame's time in seconds.
    void FlyCamUpdateHook(PPCContext& ctx, uint8_t* base, PPCFunc* original)
    {
        const uint32_t mode = ctx.r3.u32;
        const double seconds = std::clamp(ctx.f1.f64, 0.0, kMaxStep);
        const bool on = s_state == State::On;
        const dao::ui::photo::CameraSettings settings = dao::ui::photo::GetCameraSettings();
        if (on)
            s_speed.store(settings.speed, std::memory_order_relaxed);
        t_inFlyCam = on;
        original(ctx, base);
        t_inFlyCam = false;
        const uint32_t object = on ? PPC_LOAD_U32(mode + kFlyCamObjectOffset) : 0;
        if (!object)
            return;

        if (!s_start)
        {
            Pose pose;
            for (uint32_t i = 0; i < 4; ++i)
            {
                pose.position[i] = PPC_LOAD_U32(object + kPositionOffset + i * 4);
                pose.orientation[i] = PPC_LOAD_U32(object + kOrientationOffset + i * 4);
            }
            s_start = pose;
        }
        if (dao::ui::photo::TakeResetRequest())
        {
            SetPosition(ctx, base, object, s_start->position);
            SetOrientation(ctx, base, object, s_start->orientation);
            s_appliedRoll = 0;
        }

        // Up and down with the triggers.
        PadState pad;
        if (GetState(0, pad))
        {
            const auto amount = [](uint8_t trigger) {
                return trigger > kTriggerDeadZone ? (trigger - kTriggerDeadZone) / (255.0 - kTriggerDeadZone) : 0.0;
            };
            const double rise = amount(pad.rightTrigger) - amount(pad.leftTrigger);
            if (rise != 0)
            {
                const double speed = (pad.buttons & kButtonRB) ? kHeightSpeedFast
                                     : (pad.buttons & kButtonLB) ? kHeightSpeedSlow
                                                                 : kHeightSpeed;
                uint32_t position[4];
                for (uint32_t i = 0; i < 4; ++i)
                    position[i] = PPC_LOAD_U32(object + kPositionOffset + i * 4);
                const double z = LoadFloat(base, object + kPositionOffset + 8) +
                    rise * speed * settings.speed * seconds;
                position[2] = std::bit_cast<uint32_t>(float(z));
                SetPosition(ctx, base, object, position);
            }
        }

        // Roll: turned about its own forward axis (+y) by how much the setting has
        // changed since it was last applied.
        const double roll = settings.roll - s_appliedRoll;
        if (std::abs(roll) > 1e-4)
        {
            std::array<double, 4> q;
            for (uint32_t i = 0; i < 4; ++i)
                q[i] = LoadFloat(base, object + kOrientationOffset + i * 4);
            const double half = roll * std::numbers::pi / 360.0;
            const std::array<double, 4> turned = Multiply(q, { 0.0, std::sin(half), 0.0, std::cos(half) });
            const double norm =
                std::sqrt(turned[0] * turned[0] + turned[1] * turned[1] + turned[2] * turned[2] + turned[3] * turned[3]);
            uint32_t orientation[4];
            for (uint32_t i = 0; i < 4; ++i)
                orientation[i] = std::bit_cast<uint32_t>(float(turned[i] / norm));
            SetOrientation(ctx, base, object, orientation);
            s_appliedRoll = settings.roll;
        }

        // The field of view: set here (the fly camera doesn't), and kept by the
        // camera update's SetFov if it sets one in this mode.
        const double fov = dao::gpu::VerticalFov(settings.fov);
        dao::gpu::SetPhotoFov(fov);
        if (const uint32_t camera = PPC_LOAD_U32(kMainCamera.Get()))
        {
            const uint32_t setFov = PPC_LOAD_U32(PPC_LOAD_U32(camera) + kSetFovSlot * 4);
            dao::kernel::CallGuest(setFov, { camera }, { double(float(fov)) });
        }
    }

    // The input map's analog actions (r4 points to the action's number; the value in
    // f1): the left stick scaled by the speed setting in the fly camera.
    void AnalogActionHook(PPCContext& ctx, uint8_t* base, PPCFunc* original)
    {
        const uint32_t action = t_inFlyCam ? PPC_LOAD_U32(ctx.r4.u32) : 0;
        original(ctx, base);
        if (action == kActionMoveX || action == kActionMoveY)
            ctx.f1.f64 *= s_speed.load(std::memory_order_relaxed);
    }
}

namespace dao::ui
{
    void InitializePhotoMode()
    {
        InstallPauseMenuMovie();
        input::SetThumbComboHandler(TogglePhotoMode);
    }

    void TogglePhotoMode()
    {
        s_toggleAt = Now();
    }

    uint16_t PhotoModeFilterButtons(uint16_t buttons)
    {
        if (s_takesButtons.load(std::memory_order_relaxed))
        {
            s_heldOver.store(buttons & kPhotoButtons, std::memory_order_relaxed);
            return buttons & ~kPhotoButtons;
        }
        // Each button still down from photo mode stays hidden until let go.
        const uint16_t held = s_heldOver.fetch_and(buttons, std::memory_order_relaxed) & buttons;
        return buttons & ~held;
    }
}

GAME_HOOK(sub_82317138, tu6_sub_82348590, ModeUpdateHook)
GAME_HOOK(sub_824640B8, tu6_sub_824A48E8, FlyCamUpdateHook)
GAME_HOOK(sub_8266CA00, tu6_sub_826A09F8, AnalogActionHook)
