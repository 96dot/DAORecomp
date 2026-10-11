#pragma once
// Host side of the Xenia GPU integration: the game window (its own UI thread),
// Xenia's Direct3D 12 graphics system, and the routing of the Vd* exports and GPU
// register writes into it.

#include "settings.h"

namespace xe::ui
{
    class GraphicsProvider;
    class Presenter;
    class Window;
    class WindowedAppContext;
}

namespace dao::gpu
{
    // The game window's parts, for overlays drawn over the game (ui/keyboard.cpp);
    // all null before InitializeXeniaGraphics succeeds. Used on the UI thread.
    struct XeniaWindow
    {
        xe::ui::Window* window = nullptr;
        xe::ui::WindowedAppContext* appContext = nullptr;
        xe::ui::Presenter* presenter = nullptr;
        xe::ui::GraphicsProvider* provider = nullptr;
    };
    XeniaWindow GetXeniaWindow();

    // Creates the window and the D3D12 graphics system. Returns false if Direct3D 12
    // is unavailable (the runtime then falls back to the null command processor).
    bool InitializeXeniaGraphics(const std::filesystem::path& userRoot);
    bool XeniaActive();

    // Live changes from the in-game Graphics menu, from any thread (applied on the
    // UI thread, asynchronously).
    void XeniaSetFullscreen(bool fullscreen);
    void XeniaSetOutputScaling(Upscaler upscaler, float sharpness);

    void XeniaInitializeRingBuffer(uint32_t physicalAddress, uint32_t sizeLog2);
    void XeniaEnableReadPointerWriteBack(uint32_t address, uint32_t blockSizeLog2);
    void XeniaSetInterruptCallback(uint32_t callback, uint32_t userData);
    void XeniaWriteRegister(uint32_t address, uint32_t value);
    // Whether the game should render its 3D scenes single-sampled in one tile
    // (the temporal upscaler is running and antialiases them).
    bool XeniaWantsSingleSampleScene();

    // VdSwap: writes the swap packet (front buffer fetch constant + XE_SWAP) into
    // the 64 dwords the title reserved at `buffer` in the ring.
    void XeniaWriteSwapPacket(uint32_t buffer, uint32_t fetchConstant, uint32_t frontBufferVirtual,
        uint32_t width, uint32_t height);

    // Saves the current guest output (what the game presented) as a PNG.
    bool CaptureFrame(const std::filesystem::path& path);
    // Hash of the current guest output (0 if unavailable), for frame-rate checks.
    uint64_t HashFrame();
    // The current guest output downscaled 2x (box filter) to tightly packed RGB.
    bool CaptureHalfRes(std::vector<uint8_t>& rgb, uint32_t& width, uint32_t& height);
    bool WritePng(const std::filesystem::path& path, const uint8_t* rgb, uint32_t width, uint32_t height);

    // Runs the guest graphics interrupt callback on the calling (guest-attached) thread.
    void CallInterruptCallback(uint32_t callback, uint32_t userData, uint32_t source, uint32_t cpu);

    // Write watches (xenia_memory.cpp).
    bool HandleWatchedWrite(void* hostAddress, bool isWrite);
    void PrepareGuestWrite(uint32_t address, uint32_t length);
    // Debug (DAO_GPU_INVALIDATE_ALL): drops every GPU copy of guest memory.
    void InvalidateAllGpuCopies();

    // Resolves stay on the GPU: copies what the GPU wrote to [address, +length)
    // (a physical-memory virtual address) into guest memory, for the CPU to
    // read. pixelSizeLog2: of the resolved format. Blocks until the GPU thread
    // has run it and the GPU is done; false if the GPU's data isn't newer than
    // guest memory's, or it couldn't be read back.
    bool ReadBackGpuWrittenMemory(uint32_t address, uint32_t length, uint32_t pixelSizeLog2);
}
