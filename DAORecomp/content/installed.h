#pragma once
// What's installed of the game's downloadable content and title update (see
// install.h for installing them), for the launcher to show.

#include <filesystem>
#include <string>
#include <vector>

namespace dao::content
{
    // Where DLC is installed: userdata/content/454108C0/00000002.
    std::filesystem::path DlcFolder(const std::filesystem::path& userRoot);

    // A DLC package installed in DlcFolder/<content ID>/.
    struct InstalledPackage
    {
        // Its folder (the content ID).
        std::string folder;
        // Its display name (from <content ID>.name), or the folder's if it has none.
        std::string name;
        // The add-ins in it: its folders with a manifest.xml (DAO_PRC_CP_1, ...).
        std::vector<std::string> addIns;
    };

    std::vector<InstalledPackage> ListDlc(const std::filesystem::path& userRoot);

    struct TitleUpdateState
    {
        // title_update/default.xexp is there (as the game looks for it).
        bool present = false;
        // Which one it is: 6, 7, or 0 for anything else (or one that can't be read).
        int number = 0;
    };

    // Hashes title_update/default.xexp: not on the UI thread.
    TitleUpdateState FindTitleUpdate(const std::filesystem::path& gameRoot);
}
