#pragma once
// Ending the game when its window is closed. (When the game ends itself, or the Mods
// page ends it from the menu, it isn't saving: those end at once.)

#include <cstdint>

namespace dao::kernel
{
    // Ends the process, once a save the game is writing is closed (waiting 10 s at
    // most): on the console a quit waits for the title, and a save cut short is lost.
    // Not from a game thread, which may be the one to close it.
    [[noreturn]] void ExitGame(uint32_t exitCode);
}
