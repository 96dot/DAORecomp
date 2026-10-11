#pragma once
// Which build of the game runs: the disc's executable, or Title Update 6's (the
// disc's default.xex with title_update/default.xexp applied). Awakening and the DLC
// campaigns after it (Leliana's Song, Darkspawn Chronicles, Golems of Amgarrak,
// Witch Hunt) need Title Update 6; the other DLC run on either.
//
// Both are recompiled into this executable when Title Update 6 was generated
// (DAO_HAS_TU6); the player's files pick one at startup.
#include <build_info.h>

namespace dao::kernel
{
    enum class GameVersion
    {
        Disc,
        Tu6,
    };

    struct GameBuild
    {
        GameVersion version = GameVersion::Disc;
        DaoBuildInfo info = DaoBuildDisc;
    };

    // Set once by SelectBuild, before the guest starts.
    inline GameBuild g_build;

    // Where Title Update 6 is installed (by the installer or the launcher):
    // default.xexp and its data files (packages/core/patch/...), next to the game
    // folder.
    std::filesystem::path UpdateRoot(const std::filesystem::path& gameRoot);

    // Whether a default.xexp is Title Update 6's (by its SHA-1): the only one the
    // recompiled code matches, and the patcher trusts what it reads.
    bool IsTu6Patch(const std::filesystem::path& xexp);

    // Chooses Title Update 6 when it's compiled in and installed (or DAO_BUILD=disc
    // forces the disc's). Call before g_memory.Init.
    void SelectBuild(const std::filesystem::path& gameRoot);

    inline bool IsTu6() noexcept
    {
        return g_build.version == GameVersion::Tu6;
    }

    // A guest address (function or data) that moved in Title Update 6.
    struct GameAddress
    {
        uint32_t disc;
        uint32_t tu6;

        uint32_t Get() const noexcept
        {
            return IsTu6() ? tu6 : disc;
        }
    };
}

// Hooks a function of both builds with one body: hook(ctx, base, original), where
// original is the recompiled function (__imp__<name>). The TU6 name carries the
// prefix of its generated code, e.g.
//   GAME_HOOK(sub_825AF710, tu6_sub_825E47B0, SendMessageHook)
#define GAME_HOOK_ONE(name, hook)        \
    extern "C" PPC_FUNC(__imp__##name);  \
    PPC_FUNC(name)                       \
    {                                    \
        hook(ctx, base, __imp__##name);  \
    }
#if DAO_HAS_TU6
#define GAME_HOOK(disc, tu6, hook) GAME_HOOK_ONE(disc, hook) GAME_HOOK_ONE(tu6, hook)
#else
#define GAME_HOOK(disc, tu6, hook) GAME_HOOK_ONE(disc, hook)
#endif
