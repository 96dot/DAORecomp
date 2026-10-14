// The field of view settings. Every frame, the game's camera update (82265108;
// 82284BD8 in Title Update 6) gives the main camera its field of view (vertical,
// in radians) through the camera's SetFov (vtable slot 3): 60 degrees in
// gameplay, and each shot's own in conversations (narrower). SetFov on the main
// camera within that update gets it scaled for the game mode: by the gameplay
// setting in exploration and combat, by the conversation one in conversations and
// cutscenes (the modes the update itself checks after SetFov), not at all in the
// others (the title screen, loading, character creation). The scale keeps the
// image's proportions: the tangent of half the angle is multiplied, so a setting
// of 110 degrees widens the 60-degree gameplay view to 110 degrees across at 16:9,
// and a conversation shot by as much. In the fly camera (photo mode) the field of
// view is photo mode's own, when it sets one.
#include "stdafx.h"
#include "gpu/camera_fov.h"
#include "kernel/function.h"
#include "kernel/game_build.h"
#include "kernel/game_modes.h"
#include "settings.h"

#include <cmath>
#include <numbers>

namespace
{
    // The settings, in horizontal degrees at 16:9.
    std::atomic<int> s_gameplayFov{ dao::kOriginalFov };
    std::atomic<int> s_conversationFov{ dao::kOriginalFov };
    // Photo mode's, in vertical radians (0: none).
    std::atomic<double> s_photoFov{ 0 };

    using namespace dao::kernel;

    // The game mode, as the camera update reads it from its object: the mode
    // controller at +280 (kernel/game_modes.h has the rest).
    constexpr uint32_t kModeControllerOffset = 280;
    // The modes in the world's own camera (exploring, combat, dead, and the menus
    // over the world), and those with shots of their own (conversations; cutscenes,
    // unverified).
    constexpr int32_t kLastGameplayMode = kModeMenu;

    // The field of view SetFov passes on, in radians: what the scale can give (at
    // most 88 degrees vertical at kMaxFov), with room for a wider shot of the game's.
    constexpr double kMinScaledFov = 0.01;
    constexpr double kMaxScaledFov = 3.0;

    // The camera update's object while this thread runs it (0: not in it).
    thread_local uint32_t t_cameraUpdate = 0;

    // tan(fov / 2) of the game's own horizontal field of view at 16:9: its 60
    // degrees vertical (91.5 across).
    constexpr double kOriginalHalfTan = std::numbers::inv_sqrt3 * 16.0 / 9.0;

    // The scale of tan(fov / 2) that turns the game's horizontal field of view
    // into `degrees`.
    double FovScale(int degrees)
    {
        if (degrees == dao::kOriginalFov)
            return 1.0;
        return std::tan(degrees * std::numbers::pi / 360.0) / kOriginalHalfTan;
    }

    // The camera update's current game mode (-1: none).
    int32_t Mode(uint8_t* base, uint32_t cameraUpdate)
    {
        const uint32_t controller = PPC_LOAD_U32(cameraUpdate + kModeControllerOffset);
        const uint32_t current = controller ? PPC_LOAD_U32(controller + kCurrentModeOffset) : 0;
        return current ? int32_t(PPC_LOAD_U32(current + kModeNumberOffset)) : -1;
    }

    // The setting for a game mode (kOriginalFov: none).
    int ModeFov(int32_t mode)
    {
        if (mode >= 0 && mode <= kLastGameplayMode)
            return s_gameplayFov.load(std::memory_order_relaxed);
        if (mode == kModeConversation || mode == kModeCutscene)
            return s_conversationFov.load(std::memory_order_relaxed);
        return dao::kOriginalFov;
    }

    // The camera update: r3 is its object.
    void CameraUpdateHook(PPCContext& ctx, uint8_t* base, PPCFunc* original)
    {
        const uint32_t before = t_cameraUpdate;
        t_cameraUpdate = ctx.r3.u32;
        original(ctx, base);
        t_cameraUpdate = before;
    }

    // The camera's SetFov: r3 is the camera, f1 the field of view. (The mode is read
    // here, not as the update starts: the update may change it first.)
    void SetFovHook(PPCContext& ctx, uint8_t* base, PPCFunc* original)
    {
        if (t_cameraUpdate && ctx.r3.u32 == PPC_LOAD_U32(kMainCamera.Get()))
        {
            const int32_t mode = Mode(base, t_cameraUpdate);
            const int degrees = ModeFov(mode);
            const double photo = s_photoFov.load(std::memory_order_relaxed);
            if (mode == kModeFlyCam && photo > 0)
                ctx.f1.f64 = double(float(photo));
            else if (degrees != dao::kOriginalFov)
            {
                const double scaled = 2.0 * std::atan(std::tan(ctx.f1.f64 / 2.0) * FovScale(degrees));
                // (Rounded to single precision: the game stores it as a float.)
                ctx.f1.f64 = double(float(std::clamp(scaled, kMinScaledFov, kMaxScaledFov)));
            }
        }
        original(ctx, base);
    }
}

namespace dao::gpu
{
    void SetCameraFov(int gameplay, int conversation)
    {
        s_gameplayFov = std::clamp(gameplay, kMinFov, kMaxFov);
        s_conversationFov = std::clamp(conversation, kMinFov, kMaxFov);
    }

    double GameplayFovDegrees()
    {
        const int degrees = s_gameplayFov.load(std::memory_order_relaxed);
        if (degrees == dao::kOriginalFov)
            return 2.0 * std::atan(kOriginalHalfTan) * 180.0 / std::numbers::pi;
        return degrees;
    }

    double VerticalFov(double degrees)
    {
        return 2.0 * std::atan(std::tan(degrees * std::numbers::pi / 360.0) * 9.0 / 16.0);
    }

    void SetPhotoFov(double radians)
    {
        s_photoFov = radians > 0 ? std::clamp(radians, kMinScaledFov, kMaxScaledFov) : 0.0;
    }
}

GAME_HOOK(sub_82265108, tu6_sub_82284BD8, CameraUpdateHook)
GAME_HOOK(sub_82650038, tu6_sub_82683E60, SetFovHook)
