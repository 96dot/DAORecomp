#pragma once
// The game's menu movies with our additions (menu_movies.cpp): each is written to
// userdata/generated and served at the movie's override path. They return false
// (the menu stays the game's) if the movie can't be patched.

namespace dao::ui
{
    // optionsmenu.gfx with the Graphics tab (graphics_tab.as).
    bool InstallOptionsMovie();

    // mainmenu.gfx (the START menu) with the Achievements row made the Photo Mode
    // row (pause_menu.as).
    bool InstallPauseMenuMovie();
}
