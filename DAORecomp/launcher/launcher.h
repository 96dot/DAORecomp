#pragma once
// The launcher window shown before the game: Play, Configure (graphics, display,
// patches, advanced), Mods, and Update when a newer release is out.
// It edits userdata/config.toml; the game reads that file once the launcher closes.

namespace dao::launcher
{
    // Whether to show the launcher: --launcher or --mods forces it, --play or
    // DAO_SKIP_LAUNCHER skips it, otherwise [launcher] show in config.toml (holding
    // Shift shows it anyway).
    bool ShouldShow(int argc, char** argv, const std::filesystem::path& userRoot);

    // Whether --mods asks for the launcher to open on its Mods page.
    bool ShowsMods(int argc, char** argv);

    // Runs the launcher, on the Mods page with `showMods`; false if the player closed
    // it instead of pressing Play.
    bool Run(const std::filesystem::path& gameRoot, const std::filesystem::path& userRoot, bool showMods = false);
}
