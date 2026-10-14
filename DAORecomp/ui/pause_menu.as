// Our rows of the game's START menu (mainmenu.gfx): Photo Mode and Cheats.
//
// scripts/as2c.py compiles this into pause_menu_as.h; at startup the runtime
// inserts it into the game's mainmenu.gfx right after the root's first DoAction
// (which starts the scene class MainMenu), so it runs when the movie loads,
// before the scene starts.
//
// The menu's rows are instances of its exported button (bu_escape_m) in one clip,
// 50 apart: Resume, Save, Load, Options, Achievements (Screenshots_mc), then a
// divider and Exit. Our rows are more of the same button, made below
// Achievements, with Exit and the divider moved down to make room; each runs its
// init function (as the game's rows: <name>_init on _root) as it's made. The
// rows' clip is then scaled so that Exit stays where it was, inside the menu's
// frame. The controller moves through the menu's selectables in index order, so
// ours are numbered after Achievements and Exit's and the login widget's after
// them.
//
// Achievements is the game's own row, greyed: XamShowAchievementsUI shows
// nothing here. Choosing one of our rows closes the menu and asks the host:
//   ExternalCommands.SendMessage("DAORecomp.PhotoMode")  (ui/photo_mode.cpp)
//   ExternalCommands.SendMessage("DAORecomp.Cheats")     (ui/cheats.cpp)
//   ExternalCommands.GetNumber("DAORecomp.Available.Cheats")  0 greys Cheats
// (DAORecomp/ui/graphics_menu.cpp).

var ROW_SYMBOL = "bu_escape_m";
// The rows' spacing, as the menu's.
var ROW_STEP = 50;
// Ours, in order below Achievements.
var ROWS = [
    { name: "PhotoMode_mc", label: "Photo Mode", message: "DAORecomp.PhotoMode" },
    { name: "Cheats_mc", label: "Cheats", message: "DAORecomp.Cheats", available: "DAORecomp.Available.Cheats" }
];
// Depths for our clips in the rows' clip, above its timeline's.
var FIRST_ROW_DEPTH = 100;
var LAYOUT_DEPTH = 99;
// The divider between the rows and Exit: its timeline depth in the rows' clip
// (an AS depth is the timeline's less 16384), and its offset from Achievements.
var DIVIDER_DEPTH = 15 - 16384;
var DIVIDER_X = 66;
var DIVIDER_Y = 38;
// Achievements' offset from the top row and Exit's from Achievements, as the
// movie has them.
var ACHIEVEMENTS_Y = 200;
var EXIT_Y = 100;
// The rows' clip in the menu's clip (Menu_mc), as the movie has it: where it
// ends up, and its opening zoom from ZOOM_FROM to full size about a point of
// the rows (from the menu's frame ZOOM_FIRST_FRAME to ZOOM_LAST_FRAME).
var ROWS_X = 365;
var ROWS_Y = 216;
var ZOOM_FROM = 0.9048;
var ZOOM_PIVOT_X = 147;
var ZOOM_PIVOT_Y = 178.5;
var ZOOM_FIRST_FRAME = 6;
var ZOOM_LAST_FRAME = 13;
// The rows' scale with ours (Exit's old place over its new one, from the top
// row), about the rows' middle (half the button's width).
var ROWS_SCALE = (ACHIEVEMENTS_Y + EXIT_Y) / (ACHIEVEMENTS_Y + EXIT_Y + ROWS.length * ROW_STEP);
var ROWS_MIDDLE_X = 147;
// The menu's own button sounds (as its other rows).
var PRESS_SOUND = 94;
var HOVER_SOUND = 93;

// Selectables after Achievements (SELECTABLE_PROFILE): ours, then Exit and the
// login widget.
var FIRST_ROW_SELECTABLE = MainMenu.SELECTABLE_PROFILE + 1;
MainMenu.SELECTABLE_EXIT = FIRST_ROW_SELECTABLE + ROWS.length;
MainMenu.SELECTABLE_LOGIN = MainMenu.SELECTABLE_EXIT + 1;

function RowInit(row, selectable) {
    return function() {
        this.SetText(row.label);
        MainMenu.ms_GamepadNavigator.SetSelectable(selectable, this);
        this.m_nSoundType = PRESS_SOUND;
        FuncUtils.ChainFunction(this, "onRollOverHitArea", function() {
            ExternalCommands.PlayGUISound(HOVER_SOUND);
        });
        this.SetButtonAction(function() {
            ExternalCommands.SendMessageArgs("GameModeController.HideGUIScreen", "MainMenu");
            ExternalCommands.SendMessage(row.message);
        });
        if (row.available != undefined && ExternalCommands.GetNumber(row.available) == 0) {
            this.Disable();
        } else {
            this.Enable();
        }
    };
}

// Keeps our rows, the divider and Exit where they belong under Achievements,
// which the menu's opening animation moves and fades in, and the rows' clip
// scaled, through its opening zoom: every frame (a few property writes), as the
// timeline no longer moves or fades a clip once a script has moved it.
function Layout(rows) {
    var menu = rows._parent;
    var achievements = rows.Screenshots_mc;
    var divider = rows.getInstanceAtDepth(DIVIDER_DEPTH);
    var extra = ROWS.length * ROW_STEP;
    var place = function() {
        var frame = Math.max(menu._currentframe, ZOOM_FIRST_FRAME);
        var zoom = frame >= ZOOM_LAST_FRAME ? 1 : ZOOM_FROM + (1 - ZOOM_FROM) *
            (frame - ZOOM_FIRST_FRAME) / (ZOOM_LAST_FRAME - ZOOM_FIRST_FRAME);
        rows._x = ROWS_X + (1 - zoom) * ZOOM_PIVOT_X + zoom * ROWS_MIDDLE_X * (1 - ROWS_SCALE);
        rows._y = ROWS_Y + (1 - zoom) * ZOOM_PIVOT_Y;
        rows._xscale = 100 * zoom * ROWS_SCALE;
        rows._yscale = rows._xscale;

        var x = achievements._x;
        var y = achievements._y;
        var alpha = achievements._alpha;
        for (var i = 0; i < ROWS.length; i++) {
            var row = rows[ROWS[i].name];
            row._x = x;
            row._y = y + (i + 1) * ROW_STEP;
            row._alpha = alpha;
        }
        if (divider != undefined) {
            divider._x = x + DIVIDER_X;
            divider._y = y + DIVIDER_Y + extra;
            divider._alpha = alpha;
        }
        rows.Exit_mc._x = x;
        rows.Exit_mc._y = y + EXIT_Y + extra;
        rows.Exit_mc._alpha = alpha;
    };
    rows.createEmptyMovieClip("DAORecomp_Layout", LAYOUT_DEPTH).onEnterFrame = place;
    place();
}

var setup = MainMenu.SetupMainMenuInitFunctions;
MainMenu.SetupMainMenuInitFunctions = function() {
    setup();
    for (var i = 0; i < ROWS.length; i++) {
        _root[ROWS[i].name + "_init"] = RowInit(ROWS[i], FIRST_ROW_SELECTABLE + i);
    }
    var achievementsInit = _root.Screenshots_mc_init;
    _root.Screenshots_mc_init = function() {
        achievementsInit.call(this);
        this.Disable();
        var rows = this._parent;
        for (var i = 0; i < ROWS.length; i++) {
            rows.attachMovie(ROW_SYMBOL, ROWS[i].name, FIRST_ROW_DEPTH + i);
        }
        Layout(rows);
    };
};
