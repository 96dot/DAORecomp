#pragma once
// The launcher window shown before the game (and again, on its Mods page, when
// the game's Manage mods button restarts it): Play, Configure (graphics, display,
// patches, advanced), Mods, and Update when a newer release is out.
// It edits userdata/config.toml; the game reads that file once the launcher closes.

namespace dao::launcher
{
    // Whether to show the launcher: --launcher or --mods forces it, --play or
    // DAO_SKIP_LAUNCHER skips it, otherwise [launcher] show in config.toml (holding
    // Shift shows it anyway).
    bool ShouldShow(int argc, char** argv, const std::filesystem::path& userRoot);

    // Whether --mods asks for the launcher to open on its Mods page (as the game's
    // Options > Graphics > Manage mods does).
    bool ShowsMods(int argc, char** argv);

    // Whether --add-to-steam asks the launcher to add the game to Steam once it's
    // open (as setup has it do after installing, when chosen).
    bool AddsToSteam(int argc, char** argv);

    // Runs the launcher, on the Mods page with `showMods`, adding the game to Steam
    // with `addToSteam`; false if the player closed it instead of pressing Play.
    bool Run(const std::filesystem::path& gameRoot, const std::filesystem::path& userRoot, bool showMods = false,
        bool addToSteam = false);

    struct SetupResult
    {
        // The DAORecomp.exe setup installed; empty if it was closed first.
        std::filesystem::path installed;
        // Whether that one's launcher is to add the game to Steam.
        bool addToSteam = false;
    };

    // Setup, the launcher-only build players download: the Install view for `dest`,
    // with `iso` as the disc image if given (started at once with `update` when the
    // folder has the game's files).
    SetupResult RunSetup(const std::filesystem::path& dest, const std::filesystem::path& iso, bool update);
}
