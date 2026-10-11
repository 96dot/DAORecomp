// Turns off the game's occlusion-query culling (one-frame missing geometry at 60 fps).
//
// The renderer culls objects with D3D occlusion queries: a render worker
// (Eclipse::Update, guest thread 4) draws each candidate's bounding box inside a
// query and reads the sample count back a frame later. The count is the
// difference of two ZPD counter snapshots summed over the predicated tiles, and
// Xenia's ZPD emulation does not reproduce those snapshots consistently: the
// game reads counts that jump between 0, small values and wrapped negatives
// (~4.26e9). At 30 fps the reads happened late enough to see settled values;
// at 60 fps an object regularly reads 0 for a frame and is skipped.
//
// The engine has its own switch for running without queries (byte 83211EC3, set
// at renderer init from a config value): query commands are then skipped and
// every query reports 1000 samples, i.e. visible. Setting it after that init
// keeps the game on its own supported path. The cost is drawing what occlusion
// culling would have skipped, which is negligible on PC hardware.
// DAO_OCCLUSION_QUERIES=1 keeps the queries (for testing).
#include "stdafx.h"
#include "kernel/function.h"

namespace
{
    constexpr dao::kernel::GameAddress kQueriesDisabledFlag{ 0x83211EC3, 0x832E2404 };

    // Renderer initialisation (runs once; computes the flag among other setup).
    void RendererInitHook(PPCContext& ctx, uint8_t* base, PPCFunc* original)
    {
        original(ctx, base);
        static const bool keepQueries = getenv("DAO_OCCLUSION_QUERIES") != nullptr;
        const uint32_t flag = kQueriesDisabledFlag.Get();
        if (!keepQueries && !base[flag])
        {
            base[flag] = 1;
            LOGI("Occlusion-query culling disabled (Xenia's ZPD counts are unreliable for it)");
        }
    }
}

GAME_HOOK(sub_8263BBF0, tu6_sub_8266F388, RendererInitHook)
