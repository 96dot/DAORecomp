#pragma once
// Ending the game from the host (closing its window, the Mods page) or for the game.

#include <cstdint>

namespace dao::kernel
{
    // Ends the process, once a save the game is writing is closed (waiting a few seconds
    // at most): on the console a quit waits for the title, and a save cut short is lost.
    [[noreturn]] void ExitGame(uint32_t exitCode);
}
