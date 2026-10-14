// The Graphics tab of the game's Options menu.
//
// scripts/as2c.py compiles this into graphics_tab_as.h; at startup the runtime
// inserts it into the game's optionsmenu.gfx right after the root's first
// DoAction (which defines OptionsMenu), so it runs when the movie loads, before
// the scene starts. The tab is made of the menu's own widgets (tab button,
// section headers, pickers, checkboxes, sliders). Values go through the host:
//   ExternalCommands.GetNumber("DAORecomp.<Key>")      the current value
//   ExternalCommands.GetNumber("DAORecomp.Pending.<Key>")  1 if it needs a restart
//   ExternalCommands.GetNumber("DAORecomp.Available.<Key>")  0 if it's hidden
//   ExternalCommands.SendMessage("DAORecomp.Set <Key> <value>")
//   ExternalCommands.SendMessage("DAORecomp.RestoreDefaults")
//   ExternalCommands.SendMessage("DAORecomp.OpenMods")  ends the game for the
//       launcher's Mods page (returns only if the launcher can't be started)
//   ExternalCommands.SendMessage("DAORecomp.Cheats")  opens the cheats menu over
//       the game once the menus have closed (DAORecomp/ui/cheats.cpp)
// The keys and their values are those of DAORecomp/ui/graphics_menu.cpp.

// OptionsMenu.ms_nCurrentTab on this tab (the engine's tabs are 0 to 4).
var GRAPHICS_TAB = 64;
// Sprite 387 of the movie (a section header bar with its label), exported under
// this name by the runtime's patch.
var HEADER_SYMBOL = "DAORecomp_SectionHeader";
// The menu's push button (View EULA, Change Storage Device on the Online tab).
var BUTTON_SYMBOL = "Btn_StorageDev";

// Positions in the tab, as in the game's own tabs.
var HEADER_X = 32;
var CHECKBOX_X = 53;
var PICKER_X = 55.9;
var PICKER_LABEL_X = 81;
var SLIDER_X = 88.65;
var BUTTON_X = 53;
// Row heights: a header, a checkbox, a slider, a picker (its label above it), a button.
var HEADER_STEP = 52;
var TOGGLE_STEP = 33;
var SLIDER_STEP = 64;
var PICKER_STEP = 70;
var BUTTON_STEP = 46;
var PICKER_BELOW_LABEL = 19.45;
// The game's click sounds (m_nSoundType) of checkboxes and tabs.
var CHECKBOX_SOUND = 96;
var TAB_SOUND = 126;
// The sliders' steps (0 to 10), as graphics_menu.cpp's kSliderSteps.
var SLIDER_STEPS = 10;
// How long the engine gets to write the options committed before Manage mods
// closes the game, in milliseconds.
var COMMIT_WAIT = 500;

function Nits(values) {
    var labels = new Array();
    for (var i = 0; i < values.length; i++) {
        labels.push(values[i] + " nits");
    }
    return labels;
}

// Fields of view, in degrees across at 16:9 (91: the game's own), as settings.h's
// kFovSteps.
var FOV = [80, 85, 91, 95, 100, 105, 110, 115, 120];
function FovLabels() {
    var labels = new Array();
    for (var i = 0; i < FOV.length; i++) {
        labels.push(FOV[i] == 91 ? "91° (original)" : FOV[i] + "°");
    }
    return labels;
}

var HDR_PAPER_WHITE = [80, 100, 120, 160, 200, 250, 300, 400];
var HDR_PEAK = [400, 600, 800, 1000, 1500, 2000, 3000, 4000];

// Manage mods: the game closes for the launcher's Mods page, once the player
// agrees. The menu's options (the other tabs' too) are committed first, as
// leaving the menu does. (Defined before ROWS: as2c assigns functions in order.)
function OpenMods() {
    var text = "Close the game and open the launcher's Mods page?\n\n"
        + "Any unsaved progress will be lost.";
    Popups.ShowDialogBox(text, Localization.GetString(325483), function() {
        ExternalCommands.SendMessageArgs("GUI.DialogBoxClosed");
        ExternalCommands.SendMessage("ClientOptions.CommitOptions");
        var timer = setInterval(function() {
            clearInterval(timer);
            ExternalCommands.SendMessage("DAORecomp.OpenMods");
            // Still here: the launcher couldn't be started.
            Popups.ShowDialogBox("The launcher couldn't be started (see userdata/dao.log).",
                Localization.GetString(325483), function() {
                    ExternalCommands.SendMessageArgs("GUI.DialogBoxClosed");
                });
        }, COMMIT_WAIT);
    }, Localization.GetString(325484), function() {
        ExternalCommands.SendMessageArgs("GUI.DialogBoxClosed");
    });
}

// Cheats: the menus close (the options committed, as leaving the menu does: after
// the restart notice, if any) and the cheats menu opens over the game.
function OpenCheats() {
    CommitOptionsThen(new Array(), function() {
        ExternalCommands.SendMessageArgs("GameModeController.HideGUIScreen", "MainMenu");
        ExternalCommands.SendMessage("DAORecomp.Cheats");
    });
}

// The rows in order: a header, a button (its action), or a setting shown as a
// picker (values and their labels), a checkbox (toggle) or a slider. A value not
// in a picker's list (from config.toml, such as a resolution scale above 4) shows
// as the closest one. An optional row is shown only where the host has it
// (DAORecomp.Available.<key>: the cheats paused from exploration or combat). A
// restart row asks the host whether its change waits for the next start
// (DAORecomp.Pending.<key>: low latency's only on an NVIDIA GPU).
var ROWS = [
    { header: "Display" },
    { key: "Fullscreen", label: "Display mode", values: [0, 1], labels: ["Windowed", "Fullscreen"] },
    { key: "ResolutionScale", label: "Rendering resolution", restart: true, values: [1, 2, 3, 4],
      labels: ["1280x720", "2560x1440", "3840x2160 (4K)", "5120x2880"] },
    { key: "FrameRate", label: "Frame rate", values: [30, 60, 90, 120, 144, 165, 240, 0],
      labels: ["30 fps (original)", "60 fps", "90 fps", "120 fps", "144 fps", "165 fps", "240 fps", "Unlimited"] },
    { key: "VSync", label: "Vertical sync", toggle: true },
    { key: "LowLatency", label: "Low latency (Anti-Lag 2 / Reflex)", restart: true, values: [0, 1, 2],
      labels: ["Off", "On", "On + Boost"] },
    { header: "Upscaling" },
    { key: "TemporalUpscaler", label: "Temporal upscaling", restart: true, values: [0, 1, 2, 3, 4, 5],
      labels: ["Off", "Auto", "FidelityFX FSR 4", "FidelityFX FSR 3.1", "Intel XeSS-SR",
               "NVIDIA DLSS 4.5 (experimental)"] },
    { key: "TemporalSharpness", label: "Temporal sharpening (FSR)", slider: SLIDER_STEPS },
    { key: "FrameGeneration", label: "Frame generation (with temporal upscaling)", toggle: true },
    { key: "NeuralRendering", label: "Neural rendering (with temporal upscaling)", restart: true, toggle: true,
      optional: true },
    { key: "NeuralStrength", label: "Neural rendering strength", slider: SLIDER_STEPS, optional: true },
    { key: "NeuralNetwork", label: "Neural rendering quality", restart: true, optional: true,
      values: [0, 1080, 900, 720], labels: ["Auto", "Quality (1080)", "Balanced (900)", "Performance (720)"] },
    { key: "NeuralPasses", label: "Neural rendering passes", restart: true, optional: true, values: [1, 2, 3],
      labels: ["1x", "2x (stronger)", "3x (strongest)"] },
    { key: "NeuralSkinProtect", label: "Protect skin from the extra passes", restart: true, toggle: true,
      optional: true },
    { key: "NeuralStyle", label: "Neural rendering style", restart: true, optional: true, values: [0, 1, 2],
      labels: ["0 (NVIDIA's default)", "1 (lmxxf's default)", "2 (NVIDIA's third)"] },
    { key: "Upscaler", label: "Output upscaler", values: [0, 1, 2],
      labels: ["Bilinear", "FidelityFX CAS", "FidelityFX FSR 1"] },
    { key: "Sharpness", label: "Output sharpening", slider: SLIDER_STEPS },
    { header: "Quality" },
    { key: "AnisotropicFiltering", label: "Anisotropic filtering", values: [0, 2, 4, 8, 16],
      labels: ["Game default", "2x", "4x", "8x", "16x"] },
    { key: "Shadows", label: "Dynamic shadows", toggle: true },
    { key: "PostProcessing", label: "Post-processing effects", toggle: true },
    { key: "MotionBlur", label: "Motion blur (with temporal upscaling)", values: [0, 1, 2],
      labels: ["Off", "Low", "High"] },
    { header: "Camera" },
    { key: "Fov", label: "Field of view", values: FOV, labels: FovLabels() },
    { key: "ConversationFov", label: "Field of view in conversations", values: FOV, labels: FovLabels() },
    { header: "HDR" },
    { key: "Hdr", label: "HDR output", restart: true, toggle: true },
    { key: "HdrPaperWhite", label: "Paper white", values: HDR_PAPER_WHITE, labels: Nits(HDR_PAPER_WHITE) },
    { key: "HdrPeak", label: "Peak brightness", values: HDR_PEAK, labels: Nits(HDR_PEAK) },
    { key: "HdrQuality", label: "HDR quality", restart: true, values: [0, 1],
      labels: ["Standard", "High (16-bit scRGB)"] },
    { header: "Mods" },
    { button: "Manage mods", action: OpenMods },
    { header: "Cheats", key: "Cheats", optional: true },
    { button: "Open cheats", action: OpenCheats, key: "Cheats", optional: true }
];

function Get(key) {
    return ExternalCommands.GetNumber("DAORecomp." + key);
}

function Set(key, value) {
    ExternalCommands.SendMessage("DAORecomp.Set " + key + " " + value);
    // The host keeps the peak at least the paper white.
    if (key == "HdrPaperWhite") {
        RefreshRows();
    }
}

// The index of the value closest to v.
function Closest(values, v) {
    var best = 0;
    for (var i = 1; i < values.length; i++) {
        if (Math.abs(values[i] - v) < Math.abs(values[best] - v)) {
            best = i;
        }
    }
    return best;
}

function IndexOf(items, item) {
    for (var i = 0; i < items.length; i++) {
        if (items[i] == item) {
            return i;
        }
    }
    return -1;
}

// Attaches a widget whose GUI.Element init function is `init` (found through
// <name>_init on the clip itself).
function Attach(parent, symbol, name, x, y, init) {
    var props = { _x: x, _y: y };
    props[name + "_init"] = init;
    return MovieUtils.CreateMovie(parent, symbol, name, props);
}

function AddHeader(group, index, text, y) {
    var props = { _x: HEADER_X, _y: y };
    props.LabelGameplay_mc_init = function() {
        this.SetText(text);
    };
    MovieUtils.CreateMovie(group, HEADER_SYMBOL, "Header" + index + "_mc", props);
}

function AddButton(group, index, row, y) {
    return Attach(group, BUTTON_SYMBOL, "Button" + index + "_mc", BUTTON_X, y, function() {
        this.RollOver_mc.ButtonText_MC.SetText(row.button);
        this.Disable_mc.ButtonText_MC.SetText(row.button);
        this.OnButtonAction = row.action;
        // Nothing to show again, but RefreshRows refreshes every row.
        this.Refresh = function() {};
    });
}

function AddToggle(group, row, y) {
    return Attach(group, "checkbox_mc", "Opt" + row.key + "_mc", CHECKBOX_X, y, function() {
        this.m_nSoundType = CHECKBOX_SOUND;
        this.Label_mc_init = function() {
            this.SetText(row.label);
        };
        this.SetButtonAction(function() {
            Set(row.key, 1);
        });
        this.SetSecondaryButtonAction(function() {
            Set(row.key, 0);
        });
        this.Refresh = function() {
            if (Get(row.key) != 0) {
                this.Select(false);
            } else {
                this.Unselect(false);
            }
        };
        this.Refresh();
        this.AddBehaviour(Behaviour.RollOver);
    });
}

function AddPicker(group, row, y) {
    Attach(group, "txt_PotionOption", "Label" + row.key + "_mc", PICKER_LABEL_X, y, function() {
        this.SetText(row.label);
    });
    return Attach(group, "Picker", "Opt" + row.key + "_mc", PICKER_X, y + PICKER_BELOW_LABEL, function() {
        var picker = this;
        this.PickedEntry = Closest(row.values, Get(row.key));
        this.NumEntries = row.values.length;
        this.InitPickerButton = function(button) {
            this.PickerButton_mc.SetText(row.labels[this.PickedEntry]);
        };
        this.InitPickerList = function(list) {
            for (var i = 0; i < row.labels.length; i++) {
                var entry = list.AddChild("PickerEntry");
                entry.SetDirection(SharedLibrary.PickerEntry.NODIR);
                entry.Name = row.labels[i];
                entry.OnButtonAction = FuncUtils.Bind(this, picker.PickEntry, i);
                entry.nIndex = i;
            }
        };
        this.PickEntry = function(index) {
            this.PickedEntry = index;
            Set(row.key, row.values[index]);
            this.PickerButton_mc.SetText(row.labels[index]);
            this.ClosePicker();
        };
        this.Refresh = function() {
            this.PickedEntry = Closest(row.values, Get(row.key));
            this.PickerButton_mc.SetText(row.labels[this.PickedEntry]);
        };
    });
}

function AddSlider(group, row, y) {
    return Attach(group, "slider_m", "Opt" + row.key + "_mc", SLIDER_X, y, function() {
        this.m_bIntegral = true;
        this.SliderValue = new DataType.ConstrainedInteger(Get(row.key), 0, row.slider);
        this.Label_mc_init = function() {
            this.SetText(row.label);
        };
        OptionsUtils.InitSliderArrows(this, true);
        DataType.Observable.RegisterListener(this, "SliderValue", new DataType.Callback(null, function(oldValue, newValue) {
            Set(row.key, Math.round(newValue.GetValue()));
        }));
        this.Refresh = function() {
            this.SliderValue.SetIntValue(Get(row.key));
        };
        this.AddBehaviour(Behaviour.RollOver);
    });
}

var graphicsTab = null;      // the tab button
var graphicsGroup = null;    // the clip holding the rows while the tab is shown
var graphicsOptions = new Array();  // the rows, for the menu's navigation

function BuildRows(scrollable) {
    var group = scrollable.createEmptyMovieClip("Graphics_grp_mc", scrollable.getNextHighestDepth());
    group.m_nUniqueID = MovieUtils.cm_nNextUniqueID;
    MovieUtils.cm_nNextUniqueID = MovieUtils.cm_nNextUniqueID + 1;
    var options = new Array();
    var y = 0;
    for (var i = 0; i < ROWS.length; i++) {
        var row = ROWS[i];
        if (row.optional && Get("Available." + row.key) == 0) {
            // Not shown.
        } else if (row.header != undefined) {
            AddHeader(group, i, row.header, y);
            y += HEADER_STEP;
        } else if (row.button != undefined) {
            options.push(AddButton(group, i, row, y));
            y += BUTTON_STEP;
        } else if (row.toggle) {
            options.push(AddToggle(group, row, y + 3));
            y += TOGGLE_STEP;
        } else if (row.slider) {
            options.push(AddSlider(group, row, y + 1));
            y += SLIDER_STEP;
        } else {
            options.push(AddPicker(group, row, y));
            y += PICKER_STEP;
        }
    }
    graphicsGroup = group;
    graphicsOptions = options;
    // The rows hide their parts (picker lists) once initialized, over the next
    // frames: measure now and again a few frames later.
    scrollable.UpdateScrollValues();
    var timer = setInterval(function() {
        clearInterval(timer);
        scrollable.UpdateScrollValues();
    }, 50);
}

function ShowGraphicsTab() {
    var scrollable = OptionsMenu.m_mcScrollingWindow.Scrollable_mc;
    MovieUtils.RemoveAllChildren(scrollable);
    OptionsMenu.ms_nCurrentTab = GRAPHICS_TAB;
    BuildRows(scrollable);
}

function RefreshRows() {
    for (var i = 0; i < graphicsOptions.length; i++) {
        graphicsOptions[i].Refresh();
    }
}

// Adds the tab button below Audio/Video, moving the tabs after it down a step.
function AddGraphicsTab(tabs) {
    var audio = tabs.AudioTab_mc;
    var controls = tabs.ControlsTab_mc;
    if (audio == undefined || controls == undefined) {
        return;
    }
    var step = audio._y - controls._y;
    graphicsTab = MovieUtils.CreateMovie(tabs, "OptionTab", "GraphicsTab_mc", {
        _x: audio._x,
        _y: audio._y + step,
        ButtonText_MC_init: function() {
            this.SetText("Graphics");
        }
    });
    if (tabs.OnlineTab_mc != undefined) {
        tabs.OnlineTab_mc._y += step;
    }
    var groups = OptionsMenu.OptionGroups;
    var at = IndexOf(groups, audio);
    groups.splice(at < 0 ? groups.length : at + 1, 0, graphicsTab);
}

// The restart-only settings changed while the menu was open: those pending a
// restart now that weren't when it opened.
var pendingAtStart = new Object();

function RecordPending() {
    for (var i = 0; i < ROWS.length; i++) {
        if (ROWS[i].restart) {
            pendingAtStart[ROWS[i].key] = Get("Pending." + ROWS[i].key);
        }
    }
}

function RestartChanges() {
    var changes = new Array();
    for (var i = 0; i < ROWS.length; i++) {
        var row = ROWS[i];
        if (row.restart && Get("Pending." + row.key) != 0 && pendingAtStart[row.key] == 0) {
            changes.push(row.label);
        }
    }
    return changes;
}

var startScene = OptionsMenu.StartScene;
OptionsMenu.StartScene = function() {
    startScene.apply(this, arguments);
    RecordPending();
    var tabsInit = _root.OptionTabsGroup_mc_init;
    _root.OptionTabsGroup_mc_init = function() {
        tabsInit.apply(this, arguments);
        AddGraphicsTab(this);
    };
    _root.GraphicsTab_mc_init = function() {
        this.m_nSoundType = TAB_SOUND;
        this.OnButtonAction = ShowGraphicsTab;
    };
};

// The menu's navigation knows the engine's tabs by position (0 to 3); the ones
// after this tab are a step further down. The game's functions are static (they
// name OptionsMenu), but are called on it in case a mod's are not.
var setCurrentSubOptions = OptionsMenu.SetCurrentSubOptions;
OptionsMenu.SetCurrentSubOptions = function() {
    var selected = OptionsMenu.SelectedGroup;
    var at = IndexOf(OptionsMenu.OptionGroups, graphicsTab);
    if (graphicsTab == null || at < 0 || selected < at) {
        setCurrentSubOptions.apply(OptionsMenu, arguments);
        return;
    }
    if (selected == at) {
        OptionsMenu.m_aCurrentSubOptions = graphicsOptions;
        OptionsMenu.m_mcCurrentGroupContainer = graphicsGroup;
        return;
    }
    OptionsMenu.SelectedGroup = selected - 1;
    setCurrentSubOptions.apply(OptionsMenu, arguments);
    OptionsMenu.SelectedGroup = selected;
};

// Leaving the menu: say which changes need a restart, as the game does for its own,
// then commit the options (the game's CommitOptions, with `args`) and call `after`
// (if not null).
var commitOptions = OptionsMenu.CommitOptions;
function CommitOptionsThen(args, after) {
    var changes = RestartChanges();
    if (changes.length == 0) {
        commitOptions.apply(OptionsMenu, args);
        if (after != null) {
            after();
        }
        return;
    }
    RecordPending();
    var text = Localization.GetString(325619) + "\n";
    for (var i = 0; i < changes.length; i++) {
        text += "\n   - " + changes[i];
    }
    Popups.ShowDialogBox(text, Localization.GetString(325483), function() {
        ExternalCommands.SendMessageArgs("GUI.DialogBoxClosed");
        commitOptions.apply(OptionsMenu);
        if (after != null) {
            after();
        }
    });
}
OptionsMenu.CommitOptions = function() {
    CommitOptionsThen(arguments, null);
};

// Restore Defaults on this tab resets its settings (the engine doesn't know it).
var sendMessageArgs = ExternalCommands.SendMessageArgs;
ExternalCommands.SendMessageArgs = function(name) {
    if (name == "ClientOptions.RestoreDefaults" && OptionsMenu.ms_nCurrentTab == GRAPHICS_TAB) {
        ExternalCommands.SendMessage("DAORecomp.RestoreDefaults");
        RefreshRows();
        return;
    }
    sendMessageArgs.apply(ExternalCommands, arguments);
};
