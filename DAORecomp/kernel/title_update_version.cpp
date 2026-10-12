// Title Updates 6 and 7 on one recompiled build. Their code differs in six
// instructions (docs/porting-playbook.md, "Later title updates"): five address a
// global four bytes apart (and nothing else uses either address), and one is the
// version number this constructor stores in a 4-byte object it allocates (0x304D
// in 6, 0x32FF in 7); the rest of 7 is data files. So the build, generated from
// either update's patch, runs both, with the number set here for the update
// installed (kernel/game_build.h).
#include "stdafx.h"
#include "kernel/function.h"
#include "kernel/game_build.h"

#if DAO_HAS_TU6
namespace
{
    constexpr uint32_t kTu6Version = 0x304D;
    constexpr uint32_t kTu7Version = 0x32FF;

    // r3: the object constructed; its first word points to the version number
    // (null if that allocation failed). The constructor doesn't read the number
    // after storing it, so it's set when the constructor returns.
    void VersionHook(PPCContext& ctx, uint8_t* base, PPCFunc* original)
    {
        const uint32_t self = ctx.r3.u32;
        original(ctx, base);
        if (!self)
            return;
        if (const uint32_t version = PPC_LOAD_U32(self))
            PPC_STORE_U32(version, dao::kernel::IsTu7() ? kTu7Version : kTu6Version);
    }
}

GAME_HOOK_ONE(tu6_sub_82620A10, VersionHook)
#endif
