#include "stdafx.h"
#include "input/input.h"

#include <cmath>
#include <fstream>
#include <sstream>

#include <SDL3/SDL.h>

namespace dao::input
{
    namespace
    {
        // XINPUT_GAMEPAD_* bits.
        enum : uint16_t
        {
            kDpadUp = 0x0001, kDpadDown = 0x0002, kDpadLeft = 0x0004, kDpadRight = 0x0008,
            kStart = 0x0010, kBack = 0x0020, kLeftThumb = 0x0040, kRightThumb = 0x0080,
            kLeftShoulder = 0x0100, kRightShoulder = 0x0200, kGuide = 0x0400,
            kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000,
        };

        // Pseudo-buttons the script understands besides the real ones.
        enum : uint32_t
        {
            kScriptLT = 1u << 16, kScriptRT = 1u << 17,
            kScriptLUp = 1u << 18, kScriptLDown = 1u << 19, kScriptLLeft = 1u << 20, kScriptLRight = 1u << 21,
            kScriptRUp = 1u << 22, kScriptRDown = 1u << 23, kScriptRLeft = 1u << 24, kScriptRRight = 1u << 25,
        };

        constexpr std::pair<std::string_view, uint32_t> kButtonNames[] = {
            { "UP", kDpadUp }, { "DOWN", kDpadDown }, { "LEFT", kDpadLeft }, { "RIGHT", kDpadRight },
            { "START", kStart }, { "BACK", kBack }, { "LS", kLeftThumb }, { "RS", kRightThumb },
            { "LB", kLeftShoulder }, { "RB", kRightShoulder }, { "GUIDE", kGuide },
            { "A", kA }, { "B", kB }, { "X", kX }, { "Y", kY },
            { "LT", kScriptLT }, { "RT", kScriptRT },
            { "LUP", kScriptLUp }, { "LDOWN", kScriptLDown }, { "LLEFT", kScriptLLeft }, { "LRIGHT", kScriptLRight },
            { "RUP", kScriptRUp }, { "RDOWN", kScriptRDown }, { "RLEFT", kScriptRLeft }, { "RRIGHT", kScriptRRight },
        };

        struct ScriptEvent
        {
            double time = 0;    // first press, seconds since Initialize
            double hold = 0.15; // seconds held
            double period = 0;  // > 0: repeats
            double until = 1e30;
            uint32_t buttons = 0;
        };

        std::mutex s_mutex;
        PadState s_state;       // merged, guarded by s_mutex
        std::atomic<uint32_t> s_rumble{ 0 }; // left << 16 | right
        // While text is typed (the on-screen keyboard), keys aren't buttons.
        std::atomic<bool> s_textEntry{ false };
        std::vector<ScriptEvent> s_script;
        std::chrono::steady_clock::time_point s_start;

        uint32_t ParseButtons(std::string_view text)
        {
            uint32_t result = 0;
            while (!text.empty())
            {
                const size_t plus = text.find('+');
                const std::string_view name = text.substr(0, plus);
                bool found = false;
                for (const auto& [buttonName, bit] : kButtonNames)
                {
                    if (_strnicmp(buttonName.data(), name.data(), name.size()) == 0 && buttonName.size() == name.size())
                    {
                        result |= bit;
                        found = true;
                    }
                }
                if (!found)
                    LOGW("Input script: unknown button '{}'", name);
                if (plus == std::string_view::npos)
                    break;
                text.remove_prefix(plus + 1);
            }
            return result;
        }

        // DAO_INPUT_SCRIPT is a file path or an inline script; statements are
        // separated by newlines or ';':
        //   <time> <BUTTONS> [hold]                         e.g. "12 START", "20 A+LB 0.5"
        //   every <period> <BUTTONS> [hold] [from <t>] [until <t>]
        // BUTTONS joins names with '+': A B X Y START BACK LB RB LT RT LS RS GUIDE
        // UP DOWN LEFT RIGHT (d-pad), LUP/LDOWN/LLEFT/LRIGHT and RUP/... (full stick).
        void LoadScript(const char* spec)
        {
            std::string text;
            if (std::ifstream file(spec); file)
                text.assign(std::istreambuf_iterator<char>(file), {});
            else
                text = spec;
            std::replace(text.begin(), text.end(), ';', '\n');

            std::istringstream lines(text);
            for (std::string line; std::getline(lines, line);)
            {
                if (const size_t hash = line.find('#'); hash != std::string::npos)
                    line.resize(hash);
                std::istringstream words(line);
                std::vector<std::string> tokens;
                for (std::string word; words >> word;)
                    tokens.push_back(word);
                if (tokens.empty())
                    continue;

                ScriptEvent event;
                size_t i = 0;
                try
                {
                    if (tokens[0] == "every")
                    {
                        event.period = std::stod(tokens.at(1));
                        event.buttons = ParseButtons(tokens.at(2));
                        i = 3;
                    }
                    else
                    {
                        event.time = std::stod(tokens.at(0));
                        event.buttons = ParseButtons(tokens.at(1));
                        i = 2;
                    }
                    if (i < tokens.size() && tokens[i] != "from" && tokens[i] != "until")
                        event.hold = std::stod(tokens[i++]);
                    for (; i + 1 < tokens.size(); i += 2)
                    {
                        if (tokens[i] == "from")
                            event.time = std::stod(tokens[i + 1]);
                        else if (tokens[i] == "until")
                            event.until = std::stod(tokens[i + 1]);
                    }
                }
                catch (const std::exception&)
                {
                    LOGW("Input script: can't parse '{}'", line);
                    continue;
                }
                s_script.push_back(event);
            }
            LOGI("Input script: {} statements", s_script.size());
        }

        uint32_t ScriptButtons(double now)
        {
            uint32_t result = 0;
            for (const auto& event : s_script)
            {
                if (now < event.time || now > event.until)
                    continue;
                double phase = now - event.time;
                if (event.period > 0)
                    phase = std::fmod(phase, event.period);
                if (phase < event.hold)
                    result |= event.buttons;
            }
            return result;
        }

        bool GameWindowFocused()
        {
            DWORD process = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &process);
            return process == GetCurrentProcessId();
        }

        uint32_t KeyboardButtons()
        {
            if (s_textEntry.load(std::memory_order_relaxed) || !GameWindowFocused())
                return 0;
            static constexpr std::pair<int, uint32_t> kKeys[] = {
                { VK_RETURN, kStart }, { VK_TAB, kBack }, { VK_SPACE, kA }, { VK_ESCAPE, kB },
                { 'R', kX }, { 'F', kY }, { 'Q', kLeftShoulder }, { 'E', kRightShoulder },
                { 'Z', kScriptLT }, { 'C', kScriptRT }, { VK_LSHIFT, kLeftThumb }, { 'V', kRightThumb },
                { VK_UP, kDpadUp }, { VK_DOWN, kDpadDown }, { VK_LEFT, kDpadLeft }, { VK_RIGHT, kDpadRight },
                { 'W', kScriptLUp }, { 'S', kScriptLDown }, { 'A', kScriptLLeft }, { 'D', kScriptLRight },
                { 'I', kScriptRUp }, { 'K', kScriptRDown }, { 'J', kScriptRLeft }, { 'L', kScriptRRight },
            };
            uint32_t result = 0;
            for (const auto& [key, bit] : kKeys)
            {
                if (GetAsyncKeyState(key) & 0x8000)
                    result |= bit;
            }
            return result;
        }

        int16_t ClampAxis(int value)
        {
            return int16_t(std::clamp(value, -32768, 32767));
        }

        // Applies keyboard/script pseudo-buttons on top of a real pad's state.
        void MergeDigital(PadState& state, uint32_t buttons)
        {
            state.buttons |= uint16_t(buttons);
            if (buttons & kScriptLT)
                state.leftTrigger = 0xFF;
            if (buttons & kScriptRT)
                state.rightTrigger = 0xFF;
            auto stick = [](int16_t& x, int16_t& y, uint32_t b, uint32_t up, uint32_t down, uint32_t left, uint32_t right) {
                if (b & up)
                    y = 32767;
                if (b & down)
                    y = -32768;
                if (b & left)
                    x = -32768;
                if (b & right)
                    x = 32767;
            };
            stick(state.thumbLX, state.thumbLY, buttons, kScriptLUp, kScriptLDown, kScriptLLeft, kScriptLRight);
            stick(state.thumbRX, state.thumbRY, buttons, kScriptRUp, kScriptRDown, kScriptRLeft, kScriptRRight);
        }

        PadState ReadGamepad(SDL_Gamepad* pad)
        {
            static constexpr std::pair<SDL_GamepadButton, uint16_t> kMap[] = {
                { SDL_GAMEPAD_BUTTON_SOUTH, kA }, { SDL_GAMEPAD_BUTTON_EAST, kB },
                { SDL_GAMEPAD_BUTTON_WEST, kX }, { SDL_GAMEPAD_BUTTON_NORTH, kY },
                { SDL_GAMEPAD_BUTTON_BACK, kBack }, { SDL_GAMEPAD_BUTTON_GUIDE, kGuide },
                { SDL_GAMEPAD_BUTTON_START, kStart }, { SDL_GAMEPAD_BUTTON_TOUCHPAD, kBack },
                { SDL_GAMEPAD_BUTTON_LEFT_STICK, kLeftThumb }, { SDL_GAMEPAD_BUTTON_RIGHT_STICK, kRightThumb },
                { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kLeftShoulder }, { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kRightShoulder },
                { SDL_GAMEPAD_BUTTON_DPAD_UP, kDpadUp }, { SDL_GAMEPAD_BUTTON_DPAD_DOWN, kDpadDown },
                { SDL_GAMEPAD_BUTTON_DPAD_LEFT, kDpadLeft }, { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kDpadRight },
            };
            PadState state;
            for (const auto& [button, bit] : kMap)
            {
                if (SDL_GetGamepadButton(pad, button))
                    state.buttons |= bit;
            }
            // SDL triggers are 0..32767, sticks have +Y down.
            state.leftTrigger = uint8_t(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >> 7);
            state.rightTrigger = uint8_t(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7);
            state.thumbLX = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
            state.thumbLY = ClampAxis(-int(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY)));
            state.thumbRX = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
            state.thumbRY = ClampAxis(-int(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY)));
            return state;
        }

        void OnGamepadOpened(SDL_Gamepad* pad)
        {
            LOGI("Gamepad connected: {} ({})", SDL_GetGamepadName(pad) ? SDL_GetGamepadName(pad) : "?",
                int(SDL_GetGamepadType(pad)));
            // DualSense/DualShock light bar: Grey Warden red.
            SDL_SetGamepadLED(pad, 140, 0, 0);
        }

        void InputThread()
        {
            SetThreadDescription(GetCurrentThread(), L"Input");
            SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
            if (!SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC))
                LOGE("SDL_Init failed: {}; only keyboard input is available", SDL_GetError());

            SDL_Gamepad* pad = nullptr;
            uint32_t appliedRumble = 0;
            auto lastRumble = std::chrono::steady_clock::time_point{};
            PadState previous;
            uint32_t packet = 0;

            for (auto next = std::chrono::steady_clock::now();; next += std::chrono::milliseconds(4))
            {
                std::this_thread::sleep_until(next);

                SDL_Event event;
                while (SDL_PollEvent(&event))
                {
                    if (event.type == SDL_EVENT_GAMEPAD_ADDED && !pad)
                    {
                        pad = SDL_OpenGamepad(event.gdevice.which);
                        if (pad)
                            OnGamepadOpened(pad);
                    }
                    else if (event.type == SDL_EVENT_GAMEPAD_REMOVED && pad &&
                        event.gdevice.which == SDL_GetGamepadID(pad))
                    {
                        LOGI("Gamepad disconnected");
                        SDL_CloseGamepad(pad);
                        pad = nullptr;
                        appliedRumble = 0;
                        // Fall back to another connected pad.
                        int count = 0;
                        if (SDL_JoystickID* ids = SDL_GetGamepads(&count))
                        {
                            if (count > 0 && (pad = SDL_OpenGamepad(ids[0])))
                                OnGamepadOpened(pad);
                            SDL_free(ids);
                        }
                    }
                }

                PadState state = pad ? ReadGamepad(pad) : PadState{};
                const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_start).count();
                MergeDigital(state, KeyboardButtons() | ScriptButtons(now));
                state.packetNumber = previous.packetNumber;
                if (memcmp(&state, &previous, sizeof(state)) != 0)
                    state.packetNumber = ++packet;
                previous = state;
                {
                    std::lock_guard lock(s_mutex);
                    s_state = state;
                }

                // Rumble: SDL effects have a duration, XInput's last until changed.
                const uint32_t rumble = s_rumble.load(std::memory_order_relaxed);
                const auto clock = std::chrono::steady_clock::now();
                if (pad && (rumble != appliedRumble || (rumble && clock - lastRumble > std::chrono::seconds(1))))
                {
                    SDL_RumbleGamepad(pad, uint16_t(rumble >> 16), uint16_t(rumble), rumble ? 2000 : 0);
                    appliedRumble = rumble;
                    lastRumble = clock;
                }
            }
        }
    }

    void Initialize()
    {
        s_start = std::chrono::steady_clock::now();
        if (const char* script = getenv("DAO_INPUT_SCRIPT"))
            LoadScript(script);
        std::thread(InputThread).detach();
    }

    bool GetState(uint32_t user, PadState& out)
    {
        // A single virtual controller: pad, keyboard and script all drive user 0,
        // which is always connected (so keyboard-only play works).
        if (user != 0)
            return false;
        std::lock_guard lock(s_mutex);
        out = s_state;
        return true;
    }

    void SetTextEntry(bool on)
    {
        s_textEntry.store(on, std::memory_order_relaxed);
    }

    void SetRumble(uint32_t user, uint16_t leftMotor, uint16_t rightMotor)
    {
        if (user == 0)
            s_rumble.store(uint32_t(leftMotor) << 16 | rightMotor, std::memory_order_relaxed);
    }
}
