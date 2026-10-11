#pragma once
// The on-screen keyboard (XamShowKeyboardUI), drawn over the game in the game's
// own menu art: a grid of keys picked with the controller as on the Xbox 360
// (A types, X erases, Y is a space, LT capitals, RT accented letters, LB/RB move
// the cursor, Start is done, B cancels), typing on the keyboard, or clicking.

namespace dao::ui
{
    struct KeyboardRequest
    {
        std::u16string title;
        std::u16string description;
        std::u16string text;       // the starting text
        uint32_t maxLength = 0;    // characters
    };

    // Shows the keyboard and waits for the player (from any thread but the window's):
    // the text entered, or nothing if they cancelled. Without a game window to show
    // it in, the starting text.
    std::optional<std::u16string> RunKeyboard(const KeyboardRequest& request);

    // Whether the game's controller input is held back: while the keyboard is open,
    // and after it closes until the buttons are let go (so the button that closed it
    // doesn't also act in the game).
    bool KeyboardHoldsInput();
}
