#pragma once
// The game's own menu art for the launcher: sprites of the UI texture atlases in the
// player's guiexport.erf (Xbox 360 textures: tiled, big-endian DXT1/DXT5), decoded
// at startup. Nothing of the game is shipped.

namespace dao::launcher
{
    struct ArtImage
    {
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;
    };

    // The named sprites (their names in the atlases' descriptions, without ".dds",
    // e.g. "startmenu2_I15F") of the given atlases (e.g. "atl_startmenu2_dxt5"). A
    // sprite that can't be found or decoded is left out.
    std::unordered_map<std::string, ArtImage> LoadGameArt(const std::filesystem::path& gameRoot,
        std::span<const std::string_view> atlases, std::span<const std::string_view> names);
}
