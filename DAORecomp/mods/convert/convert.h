#pragma once
// Converting a mod made for the PC version to the Xbox 360 version's formats:
// GFF files (meshes, models, tables, ...) to the 360's byte order and struct
// layouts, meshes' vertices to its vertex formats, .dds textures to .xds (PC
// normal maps to DXN, as the 360's are). The struct layouts come from the game's
// own files of the same type.
//
// PC archives (.erf and .rim, ERF V2.0 and V2.2) become loose files next to them
// (in override/, which the game reads loose files from), and PC packages (.dazip)
// are unpacked into override/<package name>/ first. Add-in modules (new areas or
// campaigns) in them are left out: the 360 version can't load them.
//
// Where a mod replaces a game model, material or texture that the 360 version has
// smaller copies of for distant models (_2/_3 models, _l2/_l3 materials, l2/l3
// textures), those are made from the mod's too, so the game's don't show instead.
//
// Files that can't be converted are left out, and said so. The mod's PC files are
// moved to mods/.originals/<mod folder>/ (which keeps their paths), so converting
// can be undone by moving them back. The work is done in mods/.converting/ and
// the files put in place at the end: an interrupted conversion changes nothing.
// It stops when the drive gets nearly full or the mod unpacks to more than any
// mod could hold.

#include <stop_token>

namespace dao::mods
{
    class GameFiles;

    struct ConvertResult
    {
        bool converted = false;   // files were changed
        size_t files = 0;         // files written
        std::vector<std::string> problems;   // why nothing was converted
        std::vector<std::string> leftOut;    // files that couldn't be (when others were)
        std::vector<std::string> notes;      // other things worth knowing
    };

    // With the game's files (GameFiles::Read), or they're read.
    ConvertResult ConvertPcMod(const std::filesystem::path& modFolder, const std::filesystem::path& gameRoot,
        std::stop_token stop = {}, const GameFiles* game = nullptr);

    // Removes what interrupted conversions left in mods/.
    void CleanUpConversions(const std::filesystem::path& modsRoot);
}
