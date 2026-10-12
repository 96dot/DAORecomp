#pragma once
// Title Update 6's files, apart from the recompiled code (so the launcher-only build
// can check them too).

namespace dao::kernel
{
    // Where Title Update 6 is installed (by the installer or the launcher):
    // default.xexp and its data files (packages/core/patch/...), next to the game
    // folder.
    std::filesystem::path UpdateRoot(const std::filesystem::path& gameRoot);

    // Whether a default.xexp is Title Update 6's (by its SHA-1): the only one the
    // recompiled code matches, and the patcher trusts what it reads.
    bool IsTu6Patch(const std::filesystem::path& xexp);
}
