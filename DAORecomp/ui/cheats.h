#pragma once
// Cheats (cheats.cpp, its overlay cheats_overlay.cpp): the developers' own debug
// scripts, still in the game (the PC version's console ran them with "runscript"),
// run through the game's script engine from a menu over the paused game. Opened
// from the Options menu's Graphics tab (graphics_menu.cpp) or with F7.

#include <cstdint>
#include <string_view>

namespace dao::ui
{
    // Opens the cheats menu, or closes it, at the game's next frame (if it can open
    // there: in exploration or combat). Any thread.
    void ToggleCheats();

    // Whether the cheats menu is open (photo mode doesn't start meanwhile). Any thread.
    bool CheatsOpen();

    // Whether the cheats menu can open (the game was last in exploration or combat:
    // the Graphics tab's button). Any thread.
    bool CheatsAvailable();

    // XamInputGetState: `buttons` without those the cheats menu takes while it's open
    // (and after it, until they're released). Any thread.
    uint16_t CheatsFilterButtons(uint16_t buttons);

    // From the game's mode controller's update (photo_mode.cpp), before it: opens and
    // closes the menu, and runs the scripts asked for. `mode` is the current game
    // mode, `game` the game (a pointer).
    void UpdateCheats(uint8_t* base, int32_t mode, uint32_t game);

    // Between the cheats and their overlay.
    namespace cheats
    {
        // Who a script runs as (its OBJECT_SELF) and with what value.
        enum class Scope
        {
            Hero,          // the player's character, with the value given
            EachTag,       // the player's character, once for each party member with
                           // the member's tag as the value
            EachMortal,    // each party member that isn't immortal (remembered)
            MadeImmortal,  // each party member EachMortal ran for that still is
                           // (all who are, if none: from an earlier session)
        };

        // Runs the game's script `name` (lower case), at the game's next frame
        // while the menu is open, with `value` (up to 40 characters) as what it
        // reads for its value (RUNSCRIPT_VAR). Any thread.
        void Run(std::string_view name, std::string_view value = {}, Scope scope = Scope::Hero);

        // The game's state the menu shows, as of its last frame with the menu open.
        struct State
        {
            bool immortal = false;    // the player's character
            bool skillChecks = false; // every skill check succeeds
        };
        State GetState();

        // Shown when the menu opens, closed when it closes (from the game thread).
        void ShowOverlay();
        void CloseOverlay();
    }
}
