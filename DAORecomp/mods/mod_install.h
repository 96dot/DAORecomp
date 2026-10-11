#pragma once
// Adding mods by dropping them on the launcher: archives (.zip, .7z, .rar, read by
// Windows' tar.exe, which Windows 10 1803 and later have), folders or loose files
// become mod folders under mods/, their files sorted into the place the game
// reads them from:
//   - a single folder wrapping everything is unwrapped (it names the mod);
//   - the shallowest packages/, core/ and override/ folders are put at packages/,
//     packages/core/ and packages/core/override/;
//   - the rest goes into packages/core/override/<mod name>/ (the game reads that
//     folder's subfolders too), or straight into override/ for a DAModder mod
//     made for it;
//   - readmes and pictures at the top stay at the mod folder's top.
// The dropped files themselves are only read. Archives holding links (which could
// reach outside the mod) are refused.

#include <stop_token>

#include "mods/mods.h"

namespace dao::mods
{
    struct InstallResult
    {
        // The mods added (each a new folder under mods/), in the order dropped.
        std::vector<ModInfo> mods;
        // What happened, for the player, and whether each line is a problem (a
        // drop failed, or a mod can't be loaded or may need a choice made).
        struct Line
        {
            std::string text;
            bool problem = false;
        };
        std::vector<Line> lines;
    };

    // Each archive and folder becomes a mod; loose files together become one.
    // Stops early (what's done stays) when `stop` is requested.
    InstallResult Install(const std::filesystem::path& modsRoot, const std::vector<std::filesystem::path>& dropped,
        std::stop_token stop);

    // Whether archives can be opened here (not under Wine, which has no tar.exe).
    bool CanOpenArchives();

    // Removes what interrupted installs left in mods/ (not those still running).
    void CleanUpInstalls(const std::filesystem::path& modsRoot);
}
