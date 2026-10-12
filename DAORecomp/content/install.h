#pragma once
// Installing the player's Xbox 360 packages dropped on the launcher (one by one,
// or a folder holding them, as copied from a console's drive):
//   - downloadable content goes to userdata/content/454108C0/00000002/<content ID>/,
//     where the game finds it (with its name in <content ID>.name);
//   - Title Update 6 or 7 (needed by Awakening and the DLC campaigns after it) goes
//     to title_update/, next to the game folder.
// The packages themselves are only read.

#include <stop_token>

#include "mods/mod_install.h"

namespace dao::content
{
    struct DroppedPackage
    {
        std::filesystem::path path;
        // Found in a folder dropped (where other games' packages are passed over).
        bool inFolder = false;
    };

    // Takes the packages out of a drop: package files, and folders with packages in
    // them (each such folder is all packages: none of it becomes a mod). The rest
    // stays in `dropped`.
    std::vector<DroppedPackage> TakePackages(std::vector<std::filesystem::path>& dropped);

    // Installs them; what happened goes to `lines`.
    void InstallPackages(const std::filesystem::path& gameRoot, const std::filesystem::path& userRoot,
        const std::vector<DroppedPackage>& packages, std::stop_token stop,
        std::vector<mods::InstallResult::Line>& lines);
}
