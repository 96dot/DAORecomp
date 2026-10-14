#pragma once
// Host input for the guest's XamInput*: SDL3 gamepads (XInput, DualSense, Switch,
// ... through SDL's drivers), the keyboard while the game window has focus, and
// scripted presses for automated testing (DAO_INPUT_SCRIPT). All of it is merged
// into one virtual controller for user 0.

namespace dao::input
{
    // XINPUT_GAMEPAD_* bits.
    enum Button : uint16_t
    {
        kButtonDpadUp = 0x0001,
        kButtonDpadDown = 0x0002,
        kButtonDpadLeft = 0x0004,
        kButtonDpadRight = 0x0008,
        kButtonStart = 0x0010,
        kButtonBack = 0x0020,
        kButtonLeftThumb = 0x0040,
        kButtonRightThumb = 0x0080,
        kButtonLB = 0x0100,
        kButtonRB = 0x0200,
        kButtonGuide = 0x0400,
        kButtonA = 0x1000,
        kButtonB = 0x2000,
        kButtonX = 0x4000,
        kButtonY = 0x8000,
    };

    // XInput-layout state (button bits as XINPUT_GAMEPAD_*, sticks up = positive).
    struct PadState
    {
        uint32_t packetNumber = 0;
        uint16_t buttons = 0;
        uint8_t leftTrigger = 0, rightTrigger = 0;
        int16_t thumbLX = 0, thumbLY = 0, thumbRX = 0, thumbRY = 0;
    };

    void Initialize();

    // False when no controller is connected for `user`.
    bool GetState(uint32_t user, PadState& out);

    // While on, the keyboard types text (into the on-screen keyboard) instead of
    // pressing the virtual controller's buttons.
    void SetTextEntry(bool on);

    // XInput semantics: the motors keep these speeds until changed.
    void SetRumble(uint32_t user, uint16_t leftMotor, uint16_t rightMotor);

    // Called on the input thread when L3 and R3 are pressed together; the game
    // sees neither click. With a handler set, a click on its own reaches the game
    // kThumbComboWindow late (the other may still come).
    void SetThumbComboHandler(void (*handler)());
    inline constexpr std::chrono::milliseconds kThumbComboWindow{ 80 };

}
