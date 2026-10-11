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
//
// Other things in a mod that may not work on this version (PC menus, PC sound
// banks, add-in modules, files the game won't see, ...) are found the same way and
// shown in the launcher as warnings; the mod is still loaded.

#include "settings.h"

#include "mods/game_files.h"

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
        // What may not work with it, as sentences for the player.
        std::vector<std::string> warnings;
        // The files it changes as the game finds them, for FindOverlaps: those under
        // an override folder by name (the game looks there by name), others by path.
        std::vector<std::string> changes;
        // The other loaded mods that change the same files (see FindOverlaps).
        std::string overlap;

        bool IsForPc() const { return pcFiles > 0; }
        // Whether it's layered over the game.
        bool IsLoaded() const { return enabled && !IsForPc(); }
    };

    std::filesystem::path ModsRoot(const std::filesystem::path& gameRoot);

    // Folders under mods/ starting with this aren't mods (the work folder of mods
    // being added).
    constexpr char kHiddenPrefix = '.';
    // Where converting keeps a mod's PC files: mods/.originals/<mod folder>/.
    constexpr char kOriginalsFolder[] = ".originals";

    // A mod folder's description (mod.toml), file count, PC files and warnings.
    // With the game's file names, also warns of scripts and tables it replaces whole.
    ModInfo Describe(const std::filesystem::path& folder, const GameFiles* game = nullptr);

    // The folders under mods/ in the saved load order (see Describe). Folders not
    // in the saved list are appended (enabled); saved entries whose folder is gone
    // are dropped.
    std::vector<ModInfo> Scan(const std::filesystem::path& modsRoot, const std::vector<ModState>& saved,
        const GameFiles* game = nullptr);
    std::vector<ModState> ToState(const std::vector<ModInfo>& mods);

    // Sets each loaded mod's overlap: the other loaded mods changing its files (of
    // which the game uses one each). Again after the load order or a mod changes.
    void FindOverlaps(std::vector<ModInfo>& mods);

    // Moves a mod's folder (and the PC files kept from converting it) to the Recycle
    // Bin; Windows asks first if it would have to delete them for good. False, and
    // why in `error`, if it didn't.
    bool RecycleMod(const std::filesystem::path& modsRoot, const std::string& folder, std::string& error);

    // Whether a path in a mod or package is in an add-in's module (addins/<id>/module/):
    // a new area or campaign, which the Xbox 360 version can't load.
    bool IsInAddinModule(const std::filesystem::path& relative);

    // Layers the enabled mods over the disc device (not those for the PC version).
    void Apply(const std::filesystem::path& modsRoot, const std::vector<ModInfo>& mods);
}
