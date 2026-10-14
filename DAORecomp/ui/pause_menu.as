// The Photo Mode row of the game's START menu (mainmenu.gfx).
//
// scripts/as2c.py compiles this into pause_menu_as.h; at startup the runtime
// inserts it into the game's mainmenu.gfx right after the root's first DoAction
// (which starts the scene class MainMenu), so it runs when the movie loads,
// before the scene starts. The menu's fifth row (Screenshots_mc) shows
// Achievements on the 360, which XamShowAchievementsUI answers with nothing
// here; the row becomes Photo Mode instead, keeping its place, its sounds and
// the menu's controller navigation. Choosing it closes the menu and asks the
// host for photo mode:
//   ExternalCommands.SendMessage("DAORecomp.PhotoMode")
// (DAORecomp/ui/photo_mode.cpp).

var LABEL = "Photo Mode";
// The menu's own button sounds (as its other rows).
var PRESS_SOUND = 94;
var HOVER_SOUND = 93;

var setup = MainMenu.SetupMainMenuInitFunctions;
MainMenu.SetupMainMenuInitFunctions = function() {
    setup();
    _root.Screenshots_mc_init = function() {
        this.SetText(LABEL);
        MainMenu.ms_GamepadNavigator.SetSelectable(MainMenu.SELECTABLE_PROFILE, this);
        this.m_nSoundType = PRESS_SOUND;
        FuncUtils.ChainFunction(this, "onRollOverHitArea", function() {
            ExternalCommands.PlayGUISound(HOVER_SOUND);
        });
        this.SetButtonAction(function() {
            ExternalCommands.SendMessageArgs("GameModeController.HideGUIScreen", "MainMenu");
            ExternalCommands.SendMessage("DAORecomp.PhotoMode");
        });
        this.Enable();
    };
};
