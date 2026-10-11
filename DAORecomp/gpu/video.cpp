// xboxkrnl Vd* (video driver) exports. The GPU itself is not emulated: the plan is
// to hook the game's D3D layer onto a host renderer. Until then these report a
// 1280x720 display and record the ring buffer / interrupt setup for later use.
#include "stdafx.h"
#include "kernel/function.h"
#include "gpu/video.h"
#include "gpu/command_processor.h"
#include "gpu/xenia_host.h"
#include "kernel/thread.h"
#include "kernel/objects.h"
#include "settings.h"
#include "ui/graphics_menu.h"

namespace dao::gpu
{
    VideoState g_video;

    // -1: not overridden at runtime, use the setting.
    static std::atomic<int> s_unlockOverride{ -1 };

    bool FrameRateUnlocked()
    {
        const int value = s_unlockOverride.load(std::memory_order_relaxed);
        return value < 0 ? GetSettings().unlockFrameRate : value != 0;
    }

    void SetFrameRateUnlocked(bool unlocked)
    {
        s_unlockOverride = unlocked ? 1 : 0;
        LOGI("Frame rate unlock {}", unlocked ? "on" : "off");
    }
}

namespace dao::imp
{
    using namespace dao::kernel;
    using dao::gpu::g_video;

    struct X_VIDEO_MODE
    {
        u32 displayWidth;
        u32 displayHeight;
        u32 isInterlaced;
        u32 isWidescreen;
        u32 isHiDef;
        be<float> refreshRate;
        u32 videoStandard;
        u32 pixelRate;
        u32 widescreenFlag;
        u32 reserved[3];
    };

    static void FillVideoMode(X_VIDEO_MODE* mode)
    {
        memset(mode, 0, sizeof(*mode));
        mode->displayWidth = g_video.displayWidth;
        mode->displayHeight = g_video.displayHeight;
        mode->isWidescreen = 1;
        mode->isHiDef = 1;
        mode->refreshRate = 60.0f;
        mode->videoStandard = 1;
        mode->pixelRate = 0x8A;
        mode->widescreenFlag = 0x01;
    }

    void VdQueryVideoMode(X_VIDEO_MODE* mode) { FillVideoMode(mode); }

    uint32_t VdQueryVideoFlags()
    {
        return 1 | (g_video.displayWidth >= 1280 ? 2 : 0) | (g_video.displayWidth >= 1920 ? 4 : 0);
    }

    struct X_DISPLAY_INFO
    {
        u16 frontBufferWidth;      // 0x0
        u16 frontBufferHeight;     // 0x2
        uint8_t frontBufferColorFormat;
        uint8_t frontBufferPixelFormat;
        uint8_t pad0[2];
        u32 scalerSourceRect[4];   // 0x8
        u32 scaledOutputWidth;     // 0x18
        u32 scaledOutputHeight;    // 0x1C
        u32 verticalFilterType;    // 0x20
        u32 verticalFilter[3];     // 0x24
        u32 horizontalFilterType;  // 0x30
        u32 horizontalFilter[3];   // 0x34
        u16 overscanLeft;          // 0x40
        u16 overscanTop;
        u16 overscanRight;
        u16 overscanBottom;
        u16 displayWidth;          // 0x48
        u16 displayHeight;         // 0x4A
        be<float> refreshRate;     // 0x4C
        u32 interlaced;            // 0x50
        uint8_t displayColorFormat;// 0x54
        uint8_t pad1;
        u16 actualDisplayWidth;    // 0x56
    };
    static_assert(sizeof(X_DISPLAY_INFO) == 0x58);

    void VdGetCurrentDisplayInformation(X_DISPLAY_INFO* info)
    {
        memset(info, 0, sizeof(*info));
        const uint32_t w = g_video.displayWidth, h = g_video.displayHeight;
        info->frontBufferWidth = uint16_t(w);
        info->frontBufferHeight = uint16_t(h);
        info->scalerSourceRect[2] = w;
        info->scalerSourceRect[3] = h;
        info->scaledOutputWidth = w;
        info->scaledOutputHeight = h;
        info->verticalFilterType = 1;
        info->horizontalFilterType = 1;
        info->overscanLeft = 320;
        info->overscanTop = 180;
        info->overscanRight = 320;
        info->overscanBottom = 180;
        info->displayWidth = uint16_t(w);
        info->displayHeight = uint16_t(h);
        info->refreshRate = 60.0f;
        info->actualDisplayWidth = uint16_t(w);
    }

    void VdGetCurrentDisplayGamma(u32* type, be<float>* power)
    {
        if (type)
            *type = 2; // BT.709
        if (power)
            *power = 2.22222233f;
    }

    uint32_t VdSetDisplayMode(uint32_t flags) { return 0; }

    uint32_t VdInitializeEngines(uint32_t unk0, uint32_t callback, uint32_t arg, uint32_t pfp, uint32_t me)
    {
        LOGI("VdInitializeEngines({:08X}, cb {:08X}, arg {:08X})", unk0, callback, arg);
        return 1;
    }

    void VdShutdownEngines() {}
    uint32_t VdEnableDisableClockGating(uint32_t) { return 0; }

    void VdSetGraphicsInterruptCallback(uint32_t callback, uint32_t userData)
    {
        LOGI("VdSetGraphicsInterruptCallback({:08X}, {:08X})", callback, userData);
        g_video.interruptCallback = callback;
        g_video.interruptUserData = userData;
        if (dao::gpu::XeniaActive())
            dao::gpu::XeniaSetInterruptCallback(callback, userData);
    }

    void VdInitializeRingBuffer(uint32_t physicalAddress, uint32_t sizeLog2)
    {
        LOGI("VdInitializeRingBuffer({:08X}, 2^{})", physicalAddress, sizeLog2);
        g_video.ringBufferBase = physicalAddress;
        g_video.ringBufferSizeLog2 = sizeLog2;
        if (dao::gpu::XeniaActive())
            dao::gpu::XeniaInitializeRingBuffer(physicalAddress, sizeLog2);
        else
            dao::gpu::StartCommandProcessor();
    }

    void VdEnableRingBufferRPtrWriteBack(uint32_t address, uint32_t blockSizeLog2)
    {
        LOGI("VdEnableRingBufferRPtrWriteBack({:08X}, 2^{})", address, blockSizeLog2);
        g_video.readPointerWriteBack = address;
        g_video.writeBackBlockSizeLog2 = blockSizeLog2;
        if (dao::gpu::XeniaActive())
            dao::gpu::XeniaEnableReadPointerWriteBack(address, blockSizeLog2);
    }

    void VdGetSystemCommandBuffer(uint8_t* p0, u32* p1)
    {
        memset(p0, 0, 0x94);
        *reinterpret_cast<u32*>(p0) = 0xBEEF0000;
        *p1 = 0xBEEF0001;
    }

    void VdSetSystemCommandBufferGpuIdentifierAddress(uint32_t address)
    {
        g_video.gpuIdentifierAddress = address;
    }

    uint32_t VdInitializeScalerCommandBuffer(uint32_t sourceXY, uint32_t sourceWH, uint32_t outputXY,
        uint32_t outputWH, uint32_t frontBufferWH, uint32_t verticalFilterType, uint32_t verticalFilter,
        uint32_t horizontalFilterType)
    {
        // Args 9-12 are on the stack: (horizontal params, unk, dest, count).
        PPCContext& ctx = *CurrentContext();
        const uint32_t dest = uint32_t(bridge::IntArg(ctx, 10));
        const uint32_t count = uint32_t(bridge::IntArg(ctx, 11));
        if (dest)
        {
            auto* words = g_memory.Translate<u32>(dest);
            for (uint32_t i = 0; i < count; i++)
                words[i] = 0x80000000; // type-2 packet: nop
        }
        return count;
    }

    uint32_t VdCallGraphicsNotificationRoutines(uint32_t unk0, uint32_t args) { return 0; }
    uint32_t VdIsHSIOTrainingSucceeded() { return 1; }

    uint32_t VdPersistDisplay(uint32_t unk0, u32* out)
    {
        if (out)
            *out = PhysicalAlloc(64, 0x1000, 0x1000, X_PAGE_READWRITE, 0, 0xFFFFFFFF);
        return 1;
    }

    uint32_t VdRetrainEDRAMWorker(uint32_t) { return 0; }
    uint32_t VdRetrainEDRAM(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }

    // Args 9 and 10 (width/height pointers) are on the stack.
    void VdSwap(uint32_t bufferPtr, uint32_t fetchPtr, uint32_t systemWriteBack, uint32_t systemBuffer,
        uint32_t systemBufferId, u32* frontBufferPtr, u32* textureFormatPtr, u32* colorSpacePtr, u32* widthPtr,
        u32* heightPtr)
    {
        g_video.swapCount++;
        if (g_video.swapCount == 1 || (g_video.swapCount % 600) == 0)
            LOGI("VdSwap #{}", g_video.swapCount.load());
        if (!bufferPtr)
            return;

        static const bool invalidateAll = getenv("DAO_GPU_INVALIDATE_ALL") != nullptr;
        if (invalidateAll)
            dao::gpu::InvalidateAllGpuCopies();

        if (dao::gpu::XeniaActive() && fetchPtr && frontBufferPtr && widthPtr && heightPtr)
        {
            dao::gpu::XeniaWriteSwapPacket(bufferPtr, fetchPtr, *frontBufferPtr, *widthPtr, *heightPtr);
            return;
        }

        // Null command processor: one XE_SWAP packet marks the frame boundary.
        auto* words = g_memory.Translate<u32>(bufferPtr);
        memset(words, 0, 64 * 4);
        words[0] = (3u << 30) | ((63u - 1) << 16) | (0x64u << 8);
        words[1] = 0x53574150; // 'SWAP'
        words[2] = frontBufferPtr ? uint32_t(*frontBufferPtr) : 0;
        words[3] = widthPtr ? uint32_t(*widthPtr) : g_video.displayWidth;
        words[4] = heightPtr ? uint32_t(*heightPtr) : g_video.displayHeight;
    }
}

// One step of D3D's wait for a GPU fence (BlockOnFence, sub_82CD7810, e.g. in
// Present for a frame to be done): a few cycles of delay and the GPU hang
// check; returns nonzero to keep waiting. The render thread spent half its time
// spinning in it, keeping a core busy (and on handhelds taking power from the
// GPU): between the checks it now sleeps briefly.
extern "C" PPC_FUNC(__imp__sub_82CC4338);
PPC_FUNC(sub_82CC4338)
{
    __imp__sub_82CC4338(ctx, base);
    if (ctx.r3.u32)
        dao::kernel::ShortSleep(dao::kernel::kSpinSleepMicroseconds);
}

// D3D's swap interrupt handler (runs when the GPU reaches a swap). Its argument
// packs the front buffer address (high bits), the presentation interval (bits
// 8-11) and the immediate-present threshold (low byte); the flip is queued for
// vblank (last flip + interval). The game presents its 3D scenes with interval 2,
// i.e. at 30 fps; clamping the interval to 1 lets them flip on every vblank.
// A late flip (its vblank passed) waits for the next vblank unless less of the
// current one has passed than the threshold (percent): with the threshold at 100
// (set for interval 1 presents too), late frames are shown right away instead of
// dropping to the next vblank, so a frame taking a bit longer than a vblank costs
// that bit rather than a whole vblank. The guest flip only chooses which frame
// the host presents next; tearing is up to the host's own vsync setting.
extern "C" PPC_FUNC(__imp__sub_82CD4218);
PPC_FUNC(sub_82CD4218)
{
    const uint32_t interval = (ctx.r3.u32 >> 8) & 0xF;
    if (interval >= 1 && dao::gpu::FrameRateUnlocked())
        ctx.r3.u64 = (ctx.r3.u64 & ~uint64_t(0xFFF)) | 0x100 | 100;
    __imp__sub_82CD4218(ctx, base);
}

DAO_IMPORT_AS(VdQueryVideoMode, dao::imp::VdQueryVideoMode)
DAO_IMPORT_AS(VdQueryVideoFlags, dao::imp::VdQueryVideoFlags)
DAO_IMPORT_AS(VdGetCurrentDisplayInformation, dao::imp::VdGetCurrentDisplayInformation)
DAO_IMPORT_AS(VdGetCurrentDisplayGamma, dao::imp::VdGetCurrentDisplayGamma)
DAO_IMPORT_AS(VdSetDisplayMode, dao::imp::VdSetDisplayMode)
DAO_IMPORT_AS(VdInitializeEngines, dao::imp::VdInitializeEngines)
DAO_IMPORT_AS(VdShutdownEngines, dao::imp::VdShutdownEngines)
DAO_IMPORT_AS(VdEnableDisableClockGating, dao::imp::VdEnableDisableClockGating)
DAO_IMPORT_AS(VdSetGraphicsInterruptCallback, dao::imp::VdSetGraphicsInterruptCallback)
DAO_IMPORT_AS(VdInitializeRingBuffer, dao::imp::VdInitializeRingBuffer)
DAO_IMPORT_AS(VdEnableRingBufferRPtrWriteBack, dao::imp::VdEnableRingBufferRPtrWriteBack)
DAO_IMPORT_AS(VdGetSystemCommandBuffer, dao::imp::VdGetSystemCommandBuffer)
DAO_IMPORT_AS(VdSetSystemCommandBufferGpuIdentifierAddress, dao::imp::VdSetSystemCommandBufferGpuIdentifierAddress)
DAO_IMPORT_AS(VdInitializeScalerCommandBuffer, dao::imp::VdInitializeScalerCommandBuffer)
DAO_IMPORT_AS(VdCallGraphicsNotificationRoutines, dao::imp::VdCallGraphicsNotificationRoutines)
DAO_IMPORT_AS(VdIsHSIOTrainingSucceeded, dao::imp::VdIsHSIOTrainingSucceeded)
DAO_IMPORT_AS(VdPersistDisplay, dao::imp::VdPersistDisplay)
DAO_IMPORT_AS(VdRetrainEDRAMWorker, dao::imp::VdRetrainEDRAMWorker)
DAO_IMPORT_AS(VdRetrainEDRAM, dao::imp::VdRetrainEDRAM)
DAO_IMPORT_AS(VdSwap, dao::imp::VdSwap)

// While the temporal upscaler antialiases the 3D scene, it's rendered in one
// tile without the game's 2xMSAA: the second predicated tile and the
// multisampling only cost GPU time (a GPU-bound 4K frame 10.4 -> 9.3 ms).
// sub_82661640 gives the scene's tile count, and its tile rectangles at *r4:
// with 2xMSAA, 2 tiles, 1280x512 and 1280x208, at 82144D64. It's called every
// frame, sets the MSAA type global itself, and the engine makes its render
// targets from the result, so the scene can go back to 2xMSAA (the upscaler
// failing) from one frame to the next.
constexpr uint32_t kSceneMsaaType = 0x8316E35C; // D3DMULTISAMPLE_TYPE, 0: none
constexpr uint32_t kSceneTileRects = 0x82144D64; // D3DRECT {x1, y1, x2, y2}[2]
extern "C" PPC_FUNC(__imp__sub_82661640);
PPC_FUNC(sub_82661640)
{
    dao::ui::ApplyEngineGraphicsOptions(base);
    const uint32_t rectsOut = ctx.r4.u32;
    __imp__sub_82661640(ctx, base);
    if (ctx.r3.s32 != 2)
        return;
    if (dao::gpu::XeniaActive() && dao::gpu::XeniaWantsSingleSampleScene())
    {
        PPC_STORE_U32(kSceneMsaaType, 0);
        PPC_STORE_U32(kSceneTileRects + 12, 720); // the first tile covers the screen
        PPC_STORE_U32(rectsOut, kSceneTileRects);
        ctx.r3.u64 = 1;
    }
    else
    {
        PPC_STORE_U32(kSceneTileRects + 12, 512);
    }
}
