#pragma once
// Which build of the game runs: the disc's executable, or Title Update 6's or 7's
// (the disc's default.xex with title_update/default.xexp applied). Awakening and the
// DLC campaigns after it (Leliana's Song, Darkspawn Chronicles, Golems of Amgarrak,
// Witch Hunt) need a title update; the other DLC run on either.
//
// The disc's and the title update's code are recompiled into this executable when
// the title update's was generated (DAO_HAS_TU6); the player's files pick one at
// startup. Title Update 7's code is Title Update 6's but for a version number and
// a global moved four bytes (used only by the code that moved it), so one
// recompiled build (the "tu6" one, from either's patch) runs both
// (kernel/title_update_version.cpp; docs/porting-playbook.md, "Later title updates").
#include <build_info.h>

#include "kernel/title_update.h"

namespace dao::kernel
{
    enum class GameVersion
    {
        Disc,
        Tu6,
        Tu7,
    };

    struct GameBuild
    {
        GameVersion version = GameVersion::Disc;
        DaoBuildInfo info = DaoBuildDisc;
    };

    // Set once by SelectBuild, before the guest starts.
    inline GameBuild g_build;

    // Chooses the title update's build when it's compiled in and Title Update 6 or 7
    // is installed (or DAO_BUILD=disc forces the disc's). Call before g_memory.Init.
    void SelectBuild(const std::filesystem::path& gameRoot);

    // Whether the title update's code runs (Title Update 6's or 7's: the "tu6"
    // recompiled build).
    inline bool IsTitleUpdate() noexcept
    {
        return g_build.version != GameVersion::Disc;
    }

    inline bool IsTu7() noexcept
    {
        return g_build.version == GameVersion::Tu7;
    }

    // A guest address (function or data) that moved in Title Update 6 (and 7).
    struct GameAddress
    {
        uint32_t disc;
        uint32_t tu6;

        uint32_t Get() const noexcept
        {
            return IsTitleUpdate() ? tu6 : disc;
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
