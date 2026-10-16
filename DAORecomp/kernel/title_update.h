#pragma once
// The game's executable and the title update's files, apart from the recompiled code
// (so the launcher-only build can check them too).

namespace dao::kernel
{
    // Where the title update is installed (by the installer or the launcher):
    // default.xexp and its data files (packages/core/patch/...), next to the game
    // folder.
    std::filesystem::path UpdateRoot(const std::filesystem::path& gameRoot);

    // Which title update a default.xexp is (by its SHA-1): 6 or 7, the ones the
    // recompiled code matches (and the patcher trusts what it reads), or 0 for
    // anything else. Both are for the disc with media ID 186A1115.
    int TitleUpdateNumber(const std::filesystem::path& xexp);
    // The same for a default.xexp read into memory.
    int TitleUpdateNumber(std::span<const uint8_t> xexp);

    // Whether a default.xex read into memory is the disc's (by its SHA-1): the one
    // the code was recompiled from, and the only one the loader patches and parses.
    bool IsGameXex(std::span<const uint8_t> xex);
}
