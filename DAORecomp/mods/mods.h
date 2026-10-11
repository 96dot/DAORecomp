#pragma once
// File-override mods: each folder under mods/ (next to game/ and userdata/)
// mirrors the game folder, e.g. mods/MyMod/packages/core/override/foo.gda.
// Enabled mods are layered over the disc in load order, later mods winning.
// The engine itself also scans packages/core/override for loose files.
//
// An optional mod.toml in the folder describes the mod:
//   name = "My Mod"   version = "1.0"   author = "..."   description = "..."
//
// Mods made for the PC version are found by their files (PC models and other GFF
// files, .dds textures, PC archives), which the Xbox 360 engine can't read, and
// aren't loaded: a mod's other files usually refer to them.

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
        // Files in the PC version's formats, and one of them (its path in the mod).
        size_t pcFiles = 0;
        std::string pcExample;

        bool IsForPc() const { return pcFiles > 0; }
        // Whether it's layered over the game.
        bool IsLoaded() const { return enabled && !IsForPc(); }
    };

    std::filesystem::path ModsRoot(const std::filesystem::path& gameRoot);

    // Folders under mods/ starting with this aren't mods (the work folder of mods
    // being added).
    constexpr char kHiddenPrefix = '.';

    // A mod folder's description (mod.toml), file count and PC files.
    ModInfo Describe(const std::filesystem::path& folder);

    // The folders under mods/ in the saved load order. Folders not in the saved
    // list are appended (enabled); saved entries whose folder is gone are dropped.
    std::vector<ModInfo> Scan(const std::filesystem::path& modsRoot, const std::vector<ModState>& saved);
    std::vector<ModState> ToState(const std::vector<ModInfo>& mods);

    // Layers the enabled mods over the disc device (not those for the PC version).
    void Apply(const std::filesystem::path& modsRoot, const std::vector<ModInfo>& mods);
}
