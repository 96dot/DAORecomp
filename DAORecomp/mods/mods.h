#pragma once
// File-override mods: each folder under mods/ (next to game/ and userdata/)
// mirrors the game folder, e.g. mods/MyMod/packages/core/override/foo.gda.
// Enabled mods are layered over the disc in load order, later mods winning.
// The engine itself also scans packages/core/override for loose files.
//
// An optional mod.toml in the folder describes the mod:
//   name = "My Mod"   version = "1.0"   author = "..."   description = "..."

#include "settings.h"

namespace dao::mods
{
    struct ModInfo
    {
        std::string folder;
        std::string name;
        std::string version;
        std::string author;
        std::string description;
        bool enabled = true;
        size_t fileCount = 0;
    };

    std::filesystem::path ModsRoot(const std::filesystem::path& gameRoot);

    // The folders under mods/ in the saved load order. Folders not in the saved
    // list are appended (enabled); saved entries whose folder is gone are dropped.
    std::vector<ModInfo> Scan(const std::filesystem::path& modsRoot, const std::vector<ModState>& saved);
    std::vector<ModState> ToState(const std::vector<ModInfo>& mods);

    // Layers the enabled mods over the disc device.
    void Apply(const std::filesystem::path& modsRoot, const std::vector<ModState>& mods);
}
