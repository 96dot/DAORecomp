#pragma once
// The launcher's parts (see launcher.h), shared by its files: launcher.cpp has
// the command line, the window and its frame loop, widgets.cpp the helpers the
// views draw with, and the views a file each (title_screen.cpp, configure_view.cpp,
// mods_view.cpp, update_view.cpp, install_view.cpp, and add_to_steam.cpp's popup).

// (Every part had these when the launcher was one file.)
#include <future>
#include <ranges>

#include <SDL3/SDL.h>
#include <imgui.h>

#include "launcher/install_runner.h"
#include "launcher/steam.h"
#include "launcher/updater.h"
#include "mods/convert/convert.h"
#include "mods/mod_install.h"
#include "mods/mods.h"
#include "settings.h"
#include "ui/palette.h"

namespace dao::launcher
{
    namespace fs = std::filesystem;
    using ui::palette::kGold;
    using ui::palette::kGoldBright;
    using ui::palette::kShadow;
    using ui::palette::kText;
    using ui::palette::kTextDim;
    using ui::palette::kTextOff;
    using ui::palette::WithAlpha;

    // Messages about something wrong (no game data, a failed download).
    constexpr ImU32 kError = IM_COL32(242, 115, 89, 255);
    // A mod's warnings: what may not work with it.
    constexpr ImU32 kWarning = IM_COL32(236, 186, 92, 255);

    // The game's screens are laid out on a 1280x720 stage.
    constexpr float kStageWidth = 1280.0f, kStageHeight = 720.0f;

    // A sprite where a movie places it: on the stage (the logo, the Options screen)
    // or in the title screen's background clip.
    struct Layer
    {
        const char* sprite;
        float x, y;
        float alpha = 1.0f;
    };

    // The Options screen (optionsmenu.gfx).
    constexpr const char* kPanelBackground = "OptionsMenu_I200";
    constexpr float kPanelBackgroundAlpha = 0.92f;
    // How much the title screen behind it is darkened.
    constexpr float kPanelSceneDim = 0.45f;
    constexpr Layer kHeaderBar = { "OptionsMenu_I1D6", 486.6f, 38.5f };
    constexpr Layer kHeaderFlourish = { "OptionsMenu_I1DE", 466.6f, 23.0f, 0.3f };
    constexpr float kHeaderTitleX = 1000.0f, kHeaderTitleY = 71.5f, kHeaderTitleSize = 34.0f;
    // The tabs: text at kTabTextX; the selected one's highlight the bar mirrored,
    // its curved end at kTabBarRight, its middle at the tab's; LB and RB under them.
    constexpr const char* kTabBar = "OptionsMenu_I1F7";
    constexpr float kTabBarRight = 547.3f, kTabBarHalfHeight = 31.0f, kTabTop = 89.0f, kTabStep = 34.0f,
                    kTabTextX = 258.0f, kTabTextSize = 22.0f;
    constexpr float kBumperGap = 40.0f, kBumperSize = 26.0f, kBumperStep = 62.0f, kBumperOpacity = 0.8f;
    constexpr const char* kSectionBar = "OptionsMenu_I17E";
    // The footer: a bar stretched under the button hints.
    constexpr const char* kFooterBar = "OptionsMenu_I203";
    constexpr float kFooterLeft = 232.0f, kFooterRight = 1049.0f, kFooterTop = 629.0f, kFooterHintY = 632.0f;
    // The contents between the header and the footer.
    constexpr float kContentsTop = 112.0f, kContentsBottom = 612.0f, kContentsRight = 1240.0f;
    constexpr float kConfigureLeft = 575.0f, kModsLeft = 80.0f;

    constexpr const char* kButtonA = "XBoxGamepad_BottomAction";
    constexpr const char* kButtonB = "XBoxGamepad_RightAction";
    constexpr const char* kButtonY = "XBoxGamepad_TopAction";
    constexpr const char* kButtonLB = "XBoxGamepad_LeftBumper";
    constexpr const char* kButtonRB = "XBoxGamepad_RightBumper";
    // Button hints: the icon's height, the gap to the label, the label's size.
    constexpr float kHintIcon = 30.0f, kHintGap = 8.0f, kHintTextSize = 19.0f;

    enum class View { Main, Configure, Mods, Update, Install };
    // Graphics, Display, Patches, Advanced.
    constexpr int kConfigureTabs = 4;

    struct Texture
    {
        SDL_Texture* texture = nullptr;
        float width = 0, height = 0;
    };

    // Adding the game to Steam (from the Advanced tab, or as setup was asked to).
    enum class SteamStep
    {
        None,
        // The artwork is drawn before the next frame.
        Start,
        // Steam is open: asking to close it.
        Confirm,
        // Closing Steam, adding the game, starting Steam again (in the background).
        Working,
        Done,
    };

    struct SteamResult
    {
        // The Steam accounts that got the game.
        int accounts = 0;
        std::string error;
        // Steam has exited but can't start again until Windows lets go of it.
        bool steamStuck = false;
    };

    // Between the popup and the work in the background.
    struct SteamControl
    {
        // Waiting for Steam to close.
        std::atomic<bool> closing = false;
        // Asked to end a Steam stuck while closing.
        std::atomic<bool> end = false;
    };

    // A line of release notes, ready to show.
    struct NoteLine
    {
        enum class Kind
        {
            Blank,
            Heading,
            Text,
            Item,
        } kind;
        // Nesting (indentation steps).
        int level = 0;
        std::string text;
    };

    // What reading the mod list gives.
    struct ModScan
    {
        std::shared_ptr<const mods::GameFiles> gameFiles;
        std::vector<mods::ModInfo> mods;
    };

    // A conversion's outcome, and the mod described again after it.
    struct Converted
    {
        mods::ConvertResult result;
        mods::ModInfo mod;
    };

    // The Install view's paths.
    enum class InstallField
    {
        DiscImage,
        TitleUpdate,
        Folder,
    };

    // What file and folder dialogs chose, from SDL's dialog thread; shared with
    // the dialogs still open, which can outlive the window.
    struct DialogChoices
    {
        std::mutex mutex;
        std::vector<std::pair<InstallField, std::string>> chosen;
        int open = 0;
    };

    struct State
    {
        fs::path gameRoot, userRoot, modsRoot;
        Settings settings;
        std::vector<mods::ModInfo> mods;
        int selectedMod = -1;
        View view = View::Main;
        bool viewChanged = true;
        double viewStart = 0;
        int configureTab = 0;
        // Whether default.xex is in gameRoot (checked once, at startup).
        bool gameFound = false;
        std::unique_ptr<Updater> updater;
        // Mods dropped on the window: the paths of the drop under way, the
        // install running in the background, and how the last one went.
        std::vector<fs::path> dropped;
        std::future<mods::InstallResult> modInstall;
        std::stop_source modStop;
        std::vector<mods::InstallResult::Line> modMessages;
        // A PC mod being converted in the background (its folder), stopped with modStop.
        std::future<Converted> modConvert;
        std::string convertingMod;
        // The mod list being read in the background (at the start, and on Refresh),
        // and whether it has been (until then, the saved list isn't replaced).
        std::future<ModScan> modScan;
        bool modsRead = false;
        // config.toml couldn't be parsed: it isn't saved over (defaults are shown).
        bool settingsUnreadable = false;
        // The game's file names, for the mods' warnings: read once (with the mod
        // list), shared with the background work.
        std::shared_ptr<const mods::GameFiles> gameFiles;
        // Scroll the mod list to the selected mod (just added).
        bool scrollToMod = false;
        // The release the updater found and its notes, taken from it once.
        std::optional<Release> release;
        std::vector<NoteLine> releaseNotes;
        // The menus' serif fonts: the title screen's items, titles, section
        // headings, help.
        ImFont* menuFont = nullptr;
        ImFont* titleFont = nullptr;
        ImFont* headingFont = nullptr;
        ImFont* helpFont = nullptr;
        std::unordered_map<std::string, Texture> art;
        float scale = 1.0f;
        // The opacity of the view's contents while it fades in.
        float fade = 1.0f;
        bool play = false;
        bool quit = false;
        // Setup: the launcher-only build players download, which only installs
        // (from the Install view).
        bool setupOnly = false;
        SDL_Window* window = nullptr;
        // The Install view's choices (UTF-8 paths), and the installer.
        std::string installIso, installUpdate, installDest;
        bool installAccept = false;
        bool installShortcuts = true;
        std::string installStartError;
        InstallRunner installer;
        std::shared_ptr<DialogChoices> dialogs = std::make_shared<DialogChoices>();
        // Whether installDest has the game's files, as of checkedDest.
        std::string checkedDest;
        bool destHasGame = false;
        // The menu art copied out of the disc image being chosen (artIso), while
        // there's no installed game to take it from.
        SDL_Renderer* renderer = nullptr;
        std::string artIso;
        std::future<bool> isoArt;
        // Add to Steam: Steam's folder (empty if there's no Steam to add to), the
        // step, the library artwork, the work in the background and how it went.
        fs::path steamPath;
        SteamStep steamStep = SteamStep::None;
        std::vector<steam::Artwork> steamArt;
        std::future<SteamResult> steamTask;
        std::shared_ptr<SteamControl> steamControl;
        std::chrono::steady_clock::time_point steamWaitStart;
        SteamResult steamResult;
        // Setup: have the installed game's launcher add it to Steam.
        bool installSteam = false;
        // DAO_LAUNCHER_SCREENSHOTS=<dir>: show each view, save it as BMP, quit.
        fs::path screenshotDir;
        int screenshotFrame = 0;
        // Whether Escape and the gamepad's B mean Back this frame: not closing a popup,
        // ending an edit or leaving a nested child window.
        bool backKeysFree = false;
    };

    // Where the game's screens go in the window: the scene covers it; the menus fit
    // in it, anchored to its sides.
    struct Layout
    {
        ImVec2 pos, size;
        float cover = 1, coverX = 0, coverY = 0;
        float fit = 1, fitY = 0;

        explicit Layout(const ImGuiViewport* viewport) : Layout(viewport->Pos, viewport->Size) {}
        Layout(ImVec2 at, ImVec2 extent) : pos(at), size(extent)
        {
            cover = std::max(size.x / kStageWidth, size.y / kStageHeight);
            coverX = (size.x - kStageWidth * cover) * 0.5f;
            coverY = (size.y - kStageHeight * cover) * 0.5f;
            fit = std::min(size.x / kStageWidth, size.y / kStageHeight);
            fitY = (size.y - kStageHeight * fit) * 0.5f;
        }

        ImVec2 Max() const { return ImVec2(pos.x + size.x, pos.y + size.y); }
        ImVec2 Scene(float x, float y) const
        {
            return ImVec2(pos.x + coverX + x * cover, pos.y + coverY + y * cover);
        }
        float Y(float y) const { return pos.y + fitY + y * fit; }
        ImVec2 Left(float x, float y) const { return ImVec2(pos.x + x * fit, Y(y)); }
        ImVec2 Right(float x, float y) const { return ImVec2(pos.x + size.x - (kStageWidth - x) * fit, Y(y)); }
        ImVec2 Center(float x, float y) const
        {
            return ImVec2(pos.x + size.x * 0.5f + (x - kStageWidth * 0.5f) * fit, Y(y));
        }
    };

    // The logo's two parts where the title screen has them (stage pixels), and
    // their bounds.
    struct LogoArt
    {
        const Texture* logo = nullptr;
        const Texture* origins = nullptr;
        ImVec2 logoAt, originsAt, min, max;
    };

    // A view's contents (see ShowContents), and a tab's Restore Defaults.
    using Contents = void (*)(State&);
    using Restore = void (*)(Settings&);

    // The menus' art archive, as the game has it (and LoadGameArt reads it).
    constexpr const char* kArtArchive = "packages/core/data/guiexport.erf";

    // The state, for the helpers that aren't given it (HelpText, Section, RowWidth).
    extern const State* g_state;

    // widgets.cpp: drawing the game's art and text, its menus' buttons and hints,
    // the settings' text, the Options screen's frame; opening folders and files,
    // and switching views.
    const Texture* Art(const State& st, const char* name);
    void DrawTinted(ImDrawList* draw, const Texture& art, ImVec2 at, float scale, ImU32 tint, bool mirrored = false);
    void DrawSprite(ImDrawList* draw, const Texture& art, ImVec2 at, float scale, float alpha,
        bool mirrored = false);
    void DrawStretched(ImDrawList* draw, const Texture& art, ImVec2 min, ImVec2 max, float alpha);
    void ShadowText(ImDrawList* draw, const State& st, ImFont* font, float size, ImVec2 at, ImU32 color,
        const char* text);
    float Ease(ImGuiID id, bool on, float seconds = 0.12f);
    bool MenuButton(const char* id, ImVec2 min, ImVec2 size, bool enabled, bool& hot);
    bool Hint(const State& st, const Layout& layout, const char* button, const char* label, ImVec2 at,
        bool labelFirst, bool clickable);
    void OpenFolder(const fs::path& path);
    void OpenFile(const fs::path& path);
    void HelpText(const char* text);
    void WrappedBullet(const char* text);
    void Section(const char* title);
    float RowWidth();
    void Label(const char* text);
    void SetView(State& st, View view);
    void PanelFrame(State& st, const Layout& layout, const char* title, Restore restore);
    void ShowContents(State& st, const Layout& layout, float left, Contents contents, bool resetScroll = false);

    // title_screen.cpp: the title screen and its menu, and loading the game's art
    // (for every view).
    void DrawScene(ImDrawList* draw, const State& st, const Layout& layout, float dim, double t);
    std::optional<LogoArt> FindLogoArt(const State& st);
    void DrawLogoArt(ImDrawList* draw, const LogoArt& art, ImVec2 center, float width);
    void MainView(State& st, const Layout& layout);
    void LoadArt(State& st, SDL_Renderer* renderer);

    // configure_view.cpp: Configure, its tabs.
    void ConfigureView(State& st, const Layout& layout);

    // mods_view.cpp: Mods, and adding, converting and reading mods in the background.
    bool AddingMods(const State& st);
    bool ConvertingMod(const State& st);
    bool ModsBusy(const State& st);
    bool ReadingMods(const State& st);
    void ReadMods(State& st, std::vector<ModState> saved);
    void FinishReadingMods(State& st);
    void AddDroppedMods(State& st);
    void FinishAddingMods(State& st);
    void FinishConverting(State& st);
    void ModsView(State& st, const Layout& layout);

    // update_view.cpp: Update, a newer release's page.
    const Release* FoundRelease(State& st);
    void UpdateView(State& st, const Layout& layout);

    // install_view.cpp: Install, setup's view.
    void TakeDroppedInstallFiles(State& st);
    bool HasGameFiles(State& st);
    fs::path SetupArtRoot();
    void StartInstall(State& st);
    void InstallView(State& st, const Layout& layout);

    // add_to_steam.cpp: Add to Steam's popup and its work in the background.
    void UpdateSteam(State& st);
    void FinishSteamWork(State& st);
    void SteamPopup(State& st);
}
