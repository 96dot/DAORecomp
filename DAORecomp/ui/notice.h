#pragma once
// A message over the game for a few seconds (notice_overlay.cpp), for what the game
// itself doesn't tell the player: a save that couldn't be written.

#include <string>

namespace dao::ui
{
    // Shown from the window's next frame, in place of one still shown. Any thread.
    void ShowNotice(std::string text);
}
