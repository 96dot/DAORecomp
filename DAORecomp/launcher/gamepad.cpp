#include "stdafx.h"
#include "launcher/launcher_internal.h"

namespace dao::launcher
{
    namespace
    {
        // A stick moves the focus once pushed this far along an axis (0 to 1), and
        // again after coming back under kStickOff.
        constexpr float kStickOn = 0.5f, kStickOff = 0.35f;
        // A stick's axis all the way (SDL's values are -32768 to 32767).
        constexpr float kAxisMax = 32767.0f;
        // How far a stick moves before it counts (SDL_gamepad.h's suggestion), and
        // how far a push sets the hints to that gamepad's buttons (0 to 1).
        constexpr float kDeadZone = 8000.0f / kAxisMax, kUsedAxis = 0.5f;

        PadStyle StyleOf(SDL_GamepadType type)
        {
            switch (type)
            {
            case SDL_GAMEPAD_TYPE_PS3:
            case SDL_GAMEPAD_TYPE_PS4:
            case SDL_GAMEPAD_TYPE_PS5:
                return PadStyle::PlayStation;
            default:
                return PadStyle::Xbox;
            }
        }

        void Open(State& st, SDL_JoystickID id)
        {
            // (A pad script's run is the script's alone.)
            if (st.padScript && id != st.padScript->id)
                return;
            if (std::ranges::any_of(st.gamepads, [&](SDL_Gamepad* pad) { return SDL_GetGamepadID(pad) == id; }))
                return;
            if (SDL_Gamepad* pad = SDL_OpenGamepad(id))
            {
                st.gamepads.push_back(pad);
                // The first one sets the hints until another is used.
                if (st.gamepads.size() == 1)
                    st.padStyle = StyleOf(SDL_GetGamepadType(pad));
            }
            else
                LOGW("Launcher: can't open gamepad {}: {}", id, SDL_GetError());
        }

        // An axis of every gamepad: the one pushed furthest, from -1 to 1.
        float Axis(const State& st, SDL_GamepadAxis axis)
        {
            float value = 0.0f;
            for (SDL_Gamepad* pad : st.gamepads)
            {
                const float v = std::clamp(float(SDL_GetGamepadAxis(pad, axis)) / kAxisMax, -1.0f, 1.0f);
                if (std::abs(v) > std::abs(value))
                    value = v;
            }
            return value;
        }

        bool Button(const State& st, SDL_GamepadButton button)
        {
            return std::ranges::any_of(st.gamepads,
                [&](SDL_Gamepad* pad) { return SDL_GetGamepadButton(pad, button); });
        }

        // Past the dead zone, from -1 to 1 (0 inside it).
        float PastDeadZone(float value)
        {
            const float past = std::clamp((std::abs(value) - kDeadZone) / (1.0f - kDeadZone), 0.0f, 1.0f);
            return value < 0 ? -past : past;
        }

        // The pad script's steps: this many frames each, a button held for the first
        // kHoldFrames (a press) and the right stick for all but the last (scrolling,
        // then let go before the next step).
        constexpr int kStepFrames = 12, kHoldFrames = 3, kScrollFrames = kStepFrames - 1;
        constexpr Sint16 kPushed = Sint16(kAxisMax);

        // A script step's button or stick push.
        struct ScriptInput
        {
            const char* name;
            int button = -1;
            SDL_GamepadAxis axis = SDL_GAMEPAD_AXIS_INVALID;
            Sint16 value = 0;
            int holdFrames = kHoldFrames;
        };
        constexpr ScriptInput kScriptInputs[] = {
            { "a", SDL_GAMEPAD_BUTTON_SOUTH },
            { "b", SDL_GAMEPAD_BUTTON_EAST },
            { "x", SDL_GAMEPAD_BUTTON_WEST },
            { "y", SDL_GAMEPAD_BUTTON_NORTH },
            { "lb", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER },
            { "rb", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER },
            { "start", SDL_GAMEPAD_BUTTON_START },
            { "up", SDL_GAMEPAD_BUTTON_DPAD_UP },
            { "down", SDL_GAMEPAD_BUTTON_DPAD_DOWN },
            { "left", SDL_GAMEPAD_BUTTON_DPAD_LEFT },
            { "right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT },
            { "ls-up", -1, SDL_GAMEPAD_AXIS_LEFTY, -kPushed },
            { "ls-down", -1, SDL_GAMEPAD_AXIS_LEFTY, kPushed },
            { "ls-left", -1, SDL_GAMEPAD_AXIS_LEFTX, -kPushed },
            { "ls-right", -1, SDL_GAMEPAD_AXIS_LEFTX, kPushed },
            { "rs-up", -1, SDL_GAMEPAD_AXIS_RIGHTY, -kPushed, kScrollFrames },
            { "rs-down", -1, SDL_GAMEPAD_AXIS_RIGHTY, kPushed, kScrollFrames },
        };

        const ScriptInput* FindScriptInput(std::string_view name)
        {
            const auto it = std::ranges::find(kScriptInputs, name, [](const ScriptInput& input) {
                return std::string_view(input.name);
            });
            return it != std::end(kScriptInputs) ? &*it : nullptr;
        }

        void SetScriptInput(SDL_Joystick* joystick, const ScriptInput& input, bool on)
        {
            if (input.button >= 0)
                SDL_SetJoystickVirtualButton(joystick, input.button, on);
            else
                SDL_SetJoystickVirtualAxis(joystick, input.axis, on ? input.value : 0);
        }
    }

    void OpenGamepads(State& st)
    {
        int count = 0;
        if (SDL_JoystickID* ids = SDL_GetGamepads(&count))
        {
            for (int i = 0; i < count; i++)
                Open(st, ids[i]);
            SDL_free(ids);
        }
    }

    void HandleGamepadEvent(State& st, const SDL_Event& event)
    {
        switch (event.type)
        {
        case SDL_EVENT_GAMEPAD_ADDED:
            Open(st, event.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            std::erase_if(st.gamepads, [&](SDL_Gamepad* pad) {
                if (SDL_GetGamepadID(pad) != event.gdevice.which)
                    return false;
                SDL_CloseGamepad(pad);
                return true;
            });
            break;
        // The hints show the buttons of the gamepad used last.
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            st.padStyle = StyleOf(SDL_GetGamepadTypeForID(event.gbutton.which));
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if (std::abs(event.gaxis.value) > kUsedAxis * kAxisMax)
                st.padStyle = StyleOf(SDL_GetGamepadTypeForID(event.gaxis.which));
            break;
        default:
            break;
        }
    }

    // The gamepads as ImGui's gamepad keys, all of them as one (between the SDL
    // backend's NewFrame, which feeds none, and ImGui's).
    void FeedGamepads(State& st)
    {
        ImGuiIO& io = ImGui::GetIO();
        if (!st.gamepads.empty())
            io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

        // The left stick moves the focus as the d-pad does: along the axis pushed
        // furthest, held (and so repeating) until it comes back.
        const float x = Axis(st, SDL_GAMEPAD_AXIS_LEFTX), y = Axis(st, SDL_GAMEPAD_AXIS_LEFTY);
        const bool horizontal = std::abs(x) >= std::abs(y);
        const float pushed = horizontal ? std::abs(x) : std::abs(y);
        if (st.stickDir != ImGuiDir_None && pushed < kStickOff)
            st.stickDir = ImGuiDir_None;
        else if (pushed >= kStickOn)
            st.stickDir = horizontal ? (x < 0 ? ImGuiDir_Left : ImGuiDir_Right) : (y < 0 ? ImGuiDir_Up : ImGuiDir_Down);
        const auto dpad = [&](ImGuiKey key, SDL_GamepadButton button, ImGuiDir dir) {
            io.AddKeyEvent(key, Button(st, button) || st.stickDir == dir);
        };
        dpad(ImGuiKey_GamepadDpadLeft, SDL_GAMEPAD_BUTTON_DPAD_LEFT, ImGuiDir_Left);
        dpad(ImGuiKey_GamepadDpadRight, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, ImGuiDir_Right);
        dpad(ImGuiKey_GamepadDpadUp, SDL_GAMEPAD_BUTTON_DPAD_UP, ImGuiDir_Up);
        dpad(ImGuiKey_GamepadDpadDown, SDL_GAMEPAD_BUTTON_DPAD_DOWN, ImGuiDir_Down);

        // By position: A/Cross, B/Circle, Y/Triangle. (Not X/Square, which would open
        // ImGui's window switcher; nor the left stick's keys, which would scroll the
        // launcher's own window.)
        io.AddKeyEvent(ImGuiKey_GamepadFaceDown, Button(st, SDL_GAMEPAD_BUTTON_SOUTH));
        io.AddKeyEvent(ImGuiKey_GamepadFaceRight, Button(st, SDL_GAMEPAD_BUTTON_EAST));
        io.AddKeyEvent(ImGuiKey_GamepadFaceUp, Button(st, SDL_GAMEPAD_BUTTON_NORTH));
        io.AddKeyEvent(ImGuiKey_GamepadL1, Button(st, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER));
        io.AddKeyEvent(ImGuiKey_GamepadR1, Button(st, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER));
        io.AddKeyEvent(ImGuiKey_GamepadStart, Button(st, SDL_GAMEPAD_BUTTON_START));

        // The right stick scrolls the view (ShowContents).
        st.padScroll = PastDeadZone(Axis(st, SDL_GAMEPAD_AXIS_RIGHTY));
    }

    void CloseGamepads(State& st)
    {
        for (SDL_Gamepad* pad : st.gamepads)
            SDL_CloseGamepad(pad);
        st.gamepads.clear();
        if (st.padScript && st.padScript->joystick)
        {
            SDL_CloseJoystick(st.padScript->joystick);
            SDL_DetachVirtualJoystick(st.padScript->id);
            st.padScript->joystick = nullptr;
            SDL_ResetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS);
        }
    }

    // DAO_LAUNCHER_PAD: a virtual DualSense (attached before OpenGamepads; the only
    // gamepad used then), pressing while the window is in the background, and its
    // script's steps, separated by spaces.
    void StartPadScript(State& st, std::string_view script)
    {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.vendor_id = 0x054C;
        desc.product_id = 0x0CE6;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.name = "DAORecomp test pad";
        const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
        SDL_Joystick* joystick = id ? SDL_OpenJoystick(id) : nullptr;
        if (!joystick)
        {
            LOGW("Launcher: can't make the test gamepad: {}", SDL_GetError());
            if (id)
                SDL_DetachVirtualJoystick(id);
            return;
        }
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        PadScript& pad = st.padScript.emplace();
        pad.joystick = joystick;
        pad.id = id;
        for (const auto part : std::views::split(script, ' '))
        {
            if (const std::string_view step(part.begin(), part.end()); !step.empty())
                pad.steps.emplace_back(step);
        }
    }

    // A frame of the script (before the frame's events are read): presses and lets
    // go of its steps' buttons, captures on "shot", quits after the last step.
    void StepPadScript(State& st)
    {
        PadScript& pad = *st.padScript;
        pad.capture = false;
        if (pad.step >= pad.steps.size())
        {
            st.quit = true;
            return;
        }
        const std::string& step = pad.steps[pad.step];
        if (step == "shot")
            pad.capture = pad.frame == 0;
        else if (step == "wait")
        {
            // (Nothing pressed.)
        }
        else if (const ScriptInput* input = FindScriptInput(step))
            SetScriptInput(pad.joystick, *input, pad.frame < input->holdFrames);
        else if (pad.frame == 0)
            LOGW("Launcher: pad script: no such step \"{}\" (passed over)", step);
        if (++pad.frame == kStepFrames)
        {
            pad.frame = 0;
            pad.step++;
        }
    }
}
