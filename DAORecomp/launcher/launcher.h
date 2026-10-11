#pragma once
// The launcher window shown before the game: Play, Configure (graphics, display,
// patches, advanced), Mods, and Update when a newer release is out.
// It edits userdata/config.toml; the game reads that file once the launcher closes.

namespace dao::launcher
{
    // Whether to show the launcher: --launcher forces it, --play or DAO_SKIP_LAUNCHER
    // skips it, otherwise [launcher] show in config.toml (holding Shift shows it anyway).
    bool ShouldShow(int argc, char** argv, const std::filesystem::path& userRoot);

    // Runs the launcher; false if the player closed it instead of pressing Play.
    bool Run(const std::filesystem::path& gameRoot, const std::filesystem::path& userRoot);
}
