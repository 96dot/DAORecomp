#pragma once
// Adding the game to Steam as a non-Steam game: a shortcut in each of this PC's Steam
// accounts (userdata/<account>/config/shortcuts.vdf), with library artwork and launch
// options. Steam rewrites that file while it runs, so it has to be closed first.

namespace dao::launcher::steam
{
    // An image for Steam's library, as RGBA rows.
    struct Artwork
    {
        // The grid file's name after the app id: "p" (cover, 600x900), "" (wide
        // capsule, 920x430), "_hero" (1920x620), "_logo" (transparent).
        std::string suffix;
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;
    };

    struct Shortcut
    {
        std::string name;
        std::filesystem::path exe;
        std::string launchOptions;
    };

    // Steam's folder (from the registry); empty if Steam isn't installed, or under
    // Proton or Wine (where adding the game is done in Steam itself).
    std::filesystem::path FindSteam();

    // Whether the Steam in that folder is running.
    bool IsRunning(const std::filesystem::path& steam);

    // Asks Steam to exit and waits up to `timeout` for it; true once it's closed.
    bool Shutdown(const std::filesystem::path& steam, std::chrono::seconds timeout);

    // Starts Steam again (after Shutdown).
    bool Start(const std::filesystem::path& steam);

    // Adds the shortcut (or updates the one for the same exe) to every Steam account
    // on this PC, with the artwork. Steam must not be running. Returns how many
    // accounts got it; 0 with `error` if none did.
    int AddShortcut(const std::filesystem::path& steam, const Shortcut& shortcut, std::span<const Artwork> artwork,
        std::string& error);
}
