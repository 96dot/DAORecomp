#include "stdafx.h"

#include <future>

#include "gpu/xenia_host.h"
#include "text.h"
#include "kernel/memory.h"
#include "kernel/module.h"
#include "kernel/thread.h"
#include "settings.h"
#include "gpu/video.h"

#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"
#include "xenia/dao_host.h"
#include "xenia/dao_live_settings.h"
#include "xenia/gpu/d3d12/d3d12_command_processor.h"
#include "xenia/gpu/d3d12/d3d12_graphics_system.h"
#include "xenia/gpu/d3d12/dao_upscale.h"
#include "xenia/gpu/xenos.h"
#include "xenia/memory.h"
#include "xenia/ui/window.h"
#include "xenia/ui/window_listener.h"
#include "xenia/ui/window_win.h"
#include "xenia/ui/windowed_app_context_win.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
// File names are UTF-8.
#define STBIW_WINDOWS_UTF8
#include "third_party/stb/stb_image_write.h"

DECLARE_path(log_file);
DECLARE_bool(log_to_stdout);
DECLARE_bool(dao_present_vsync);
DECLARE_double(dao_temporal_sharpness);
DECLARE_bool(dao_frame_generation);
DECLARE_int32(anisotropic_override);
DECLARE_int32(dao_motion_blur);
DECLARE_double(present_hdr_paper_white);
DECLARE_double(present_hdr_peak);

using namespace dao::kernel;

namespace dao::gpu
{
    namespace
    {
        std::unique_ptr<xe::Memory> s_memory;
        std::unique_ptr<xe::ui::Win32WindowedAppContext> s_appContext;

        std::unique_ptr<xe::ui::Window> s_window;
        std::unique_ptr<xe::gpu::d3d12::D3D12GraphicsSystem> s_graphics;
        std::recursive_mutex s_interruptMutex;

        // Closing the window ends the game.
        class CloseListener final : public xe::ui::WindowListener
        {
        public:
            void OnClosing(xe::ui::UIEvent&) override
            {
                LOGI("Window closed, exiting");
                TerminateProcess(GetCurrentProcess(), 0);
            }
        } s_closeListener;

        // Keeps the window on the monitor it opened on. The configured size is in
        // DPI-scaled units (1280x720 at 150% is 1920x1080 pixels), so it can be
        // bigger than the screen: shrink the game area to the monitor's work area,
        // keeping the configured aspect ratio, and center the window there.
        void FitWindowToMonitor(HWND hwnd, int aspectWidth, int aspectHeight)
        {
            RECT window, client;
            if (!GetWindowRect(hwnd, &window) || !GetClientRect(hwnd, &client))
                return;
            MONITORINFO info{ sizeof(info) };
            if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &info))
                return;
            const RECT& work = info.rcWork;
            const int frameWidth = (window.right - window.left) - client.right;
            const int frameHeight = (window.bottom - window.top) - client.bottom;
            const int availableWidth = (work.right - work.left) - frameWidth;
            const int availableHeight = (work.bottom - work.top) - frameHeight;
            int width = client.right, height = client.bottom;
            if (width > availableWidth || height > availableHeight)
            {
                // Windows may already have cut the height at creation: use the
                // configured shape, not the current one.
                width = availableWidth;
                height = int(int64_t(width) * aspectHeight / aspectWidth);
                if (height > availableHeight)
                {
                    height = availableHeight;
                    width = int(int64_t(height) * aspectWidth / aspectHeight);
                }
                LOGI("Window: {}x{} doesn't fit the display ({}x{} work area), using {}x{}", client.right,
                    client.bottom, work.right - work.left, work.bottom - work.top, width, height);
            }
            width += frameWidth;
            height += frameHeight;
            SetWindowPos(hwnd, nullptr, work.left + ((work.right - work.left) - width) / 2,
                work.top + ((work.bottom - work.top) - height) / 2, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        }

        // How the guest output is scaled to the window: the upscaler and its sharpness.
        xe::ui::Presenter::GuestOutputPaintConfig OutputPaintConfig(Upscaler upscaler, float sharpness)
        {
            using PaintConfig = xe::ui::Presenter::GuestOutputPaintConfig;
            PaintConfig config;
            switch (upscaler)
            {
            case Upscaler::Bilinear:
                config.SetEffect(PaintConfig::Effect::kBilinear);
                break;
            case Upscaler::Cas:
                config.SetEffect(PaintConfig::Effect::kCas);
                config.SetCasAdditionalSharpness(sharpness);
                break;
            case Upscaler::Fsr:
                config.SetEffect(PaintConfig::Effect::kFsr);
                config.SetFsrSharpnessReduction(sharpness);
                break;
            }
            // Dithering targets 8-bit output; the HDR path keeps 10 bits. HDR is
            // chosen at startup (the swap chain format), so this doesn't change.
            config.SetDither(!GetSettings().hdr);
            return config;
        }
    }

    void LoadXeniaLiveSettings()
    {
        xe::dao_live::Values& live = xe::dao_live::values;
        live.present_vsync = cvars::dao_present_vsync;
        live.temporal_sharpness = cvars::dao_temporal_sharpness;
        live.frame_generation = cvars::dao_frame_generation;
        live.anisotropic_override = cvars::anisotropic_override;
        live.motion_blur = cvars::dao_motion_blur;
        live.hdr_paper_white = cvars::present_hdr_paper_white;
        live.hdr_peak = cvars::present_hdr_peak;
    }

    bool InitializeXeniaGraphics(const std::filesystem::path& userRoot)
    {
        cvars::log_file = userRoot / "xenia.log";
        cvars::log_to_stdout = false;
        xe::InitializeLogging("dao-xenia");

        if (!xe::gpu::d3d12::D3D12GraphicsSystem::IsAvailable())
        {
            LOGW("Direct3D 12 is not available; rendering disabled");
            return false;
        }

        // The window and the WindowedAppContext live on their own UI thread, which
        // runs the Win32 message loop for the rest of the process.
        std::promise<bool> ready;
        std::thread([&ready] {
            SetThreadDescription(GetCurrentThread(), L"UI");
            // DAO_BACKGROUND=1 (automated tests): open behind the active window,
            // leaving it focused.
            const char* background = getenv("DAO_BACKGROUND");
            const HWND foregroundWindow = background && atoi(background) ? GetForegroundWindow() : nullptr;
            s_appContext = std::make_unique<xe::ui::Win32WindowedAppContext>(GetModuleHandleW(nullptr), SW_SHOWDEFAULT);
            if (!s_appContext->Initialize())
            {
                ready.set_value(false);
                return;
            }
            const Settings& settings = GetSettings();
            s_window = xe::ui::Window::Create(*s_appContext, "Dragon Age: Origins", settings.windowWidth,
                settings.windowHeight);
            if (!s_window || !s_window->Open())
            {
                ready.set_value(false);
                return;
            }
            if (settings.fullscreen)
                s_window->SetFullscreen(true);
            else
                FitWindowToMonitor(static_cast<xe::ui::Win32Window&>(*s_window).hwnd(), settings.windowWidth,
                    settings.windowHeight);
            s_window->AddListener(&s_closeListener);
            if (foregroundWindow)
            {
                const HWND hwnd = static_cast<xe::ui::Win32Window&>(*s_window).hwnd();
                SetWindowPos(hwnd, foregroundWindow, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                SetForegroundWindow(foregroundWindow);
            }
            ready.set_value(true);
            s_appContext->RunMainMessageLoop();
        }).detach();

        if (!ready.get_future().get())
        {
            LOGE("Failed to create the game window");
            return false;
        }

        s_memory = std::make_unique<xe::Memory>();
        s_graphics = std::make_unique<xe::gpu::d3d12::D3D12GraphicsSystem>();
        if (s_graphics->Setup(s_memory.get(), s_appContext.get(), true) != 0)
        {
            LOGE("Failed to set up the Direct3D 12 graphics system");
            s_graphics.reset();
            return false;
        }
        // The shaders and pipelines of earlier runs, translated and created in the
        // background while the game starts (instead of when first drawn); new ones
        // are added as they're seen.
        s_graphics->InitializeShaderStorage(userRoot / "shader_cache", g_module.titleId, false);
        s_appContext->CallInUIThreadSynchronous([] {
            const Settings& settings = GetSettings();
            s_graphics->presenter()->SetGuestOutputPaintConfigFromUIThread(
                OutputPaintConfig(settings.upscaler, settings.sharpness));
            s_window->SetPresenter(s_graphics->presenter());
        });

        // Registers the game reads directly (the rest are only written).
        *g_memory.Translate<u32>(0x7FC80000 + 0x1951 * 4) = 1; // interrupt status: vblank

        LOGI("Xenia Direct3D 12 graphics initialized");
        return true;
    }

    bool XeniaActive()
    {
        return s_graphics != nullptr;
    }

    XeniaWindow GetXeniaWindow()
    {
        if (!s_graphics || !s_graphics->presenter())
            return {};
        return { s_window.get(), s_appContext.get(), s_graphics->presenter(), s_graphics->provider() };
    }

    void XeniaSetFullscreen(bool fullscreen)
    {
        if (!s_window)
            return;
        s_appContext->CallInUIThread([fullscreen] {
            if (s_window->IsFullscreen() == fullscreen)
                return;
            LOGI("Window: {}", fullscreen ? "fullscreen" : "windowed");
            s_window->SetFullscreen(fullscreen);
            if (!fullscreen)
            {
                const Settings& settings = GetSettings();
                FitWindowToMonitor(static_cast<xe::ui::Win32Window&>(*s_window).hwnd(), settings.windowWidth,
                    settings.windowHeight);
            }
        });
    }

    void XeniaSetOutputScaling(Upscaler upscaler, float sharpness)
    {
        if (!s_graphics)
            return;
        s_appContext->CallInUIThread([upscaler, sharpness] {
            s_graphics->presenter()->SetGuestOutputPaintConfigFromUIThread(OutputPaintConfig(upscaler, sharpness));
        });
    }

    void XeniaInitializeRingBuffer(uint32_t physicalAddress, uint32_t sizeLog2)
    {
        s_graphics->InitializeRingBuffer(physicalAddress, sizeLog2);
    }

    void XeniaEnableReadPointerWriteBack(uint32_t address, uint32_t blockSizeLog2)
    {
        s_graphics->EnableReadPointerWriteBack(address, blockSizeLog2);
    }

    void XeniaSetInterruptCallback(uint32_t callback, uint32_t userData)
    {
        s_graphics->SetInterruptCallback(callback, userData);
    }

    void XeniaWriteRegister(uint32_t address, uint32_t value)
    {
        s_graphics->WriteRegister(address, value);
    }

    bool ReadBackGpuWrittenMemory(uint32_t address, uint32_t length, uint32_t pixelSizeLog2)
    {
        if (!s_graphics || !IsPhysicalAddress(address) || !length)
            return false;
        // Within the 512 MB of physical memory (the 0xE view is offset by a page).
        constexpr uint32_t kPhysicalSize = 0x20000000;
        const uint32_t physical = PhysicalAddressOf(address);
        if (physical >= kPhysicalSize || length > kPhysicalSize - physical)
            return false;
        auto* processor = static_cast<xe::gpu::d3d12::D3D12CommandProcessor*>(s_graphics->command_processor());
        // Shared with the GPU thread, which may finish after this has given up waiting.
        struct Readback
        {
            std::mutex mutex;
            std::condition_variable cv;
            bool done = false;
            bool read = false;
            std::vector<uint8_t> data;
        };
        const auto readback = std::make_shared<Readback>();
        processor->CallInThread([readback, processor, physical, length, pixelSizeLog2] {
            std::vector<uint8_t> data;
            const bool read = processor->DaoReadGpuWrittenMemory(physical, length, pixelSizeLog2, data);
            std::lock_guard lock(readback->mutex);
            readback->read = read;
            readback->data = std::move(data);
            readback->done = true;
            readback->cv.notify_all();
        });
        // A GPU that has stopped leaves the caller, a game thread, without the data
        // rather than stuck. A readback takes milliseconds; 5 s is only reached if
        // the GPU thread is gone or hung. The lambda holds the state it writes, so
        // it may finish after this returns.
        constexpr auto kReadbackTimeout = std::chrono::seconds(5);
        std::unique_lock lock(readback->mutex);
        if (!readback->cv.wait_for(lock, kReadbackTimeout, [&] { return readback->done; }))
        {
            LOGW("GPU: reading back {:08X} (+{}) took too long", address, length);
            return false;
        }
        const std::vector<uint8_t>& data = readback->data;
        // Shorter than length if only whole tiles of a scaled resolve were read.
        if (!readback->read || data.size() > length)
            return false;
        // Through the write watches, so the GPU's copies are reloaded from it.
        PrepareGuestWrite(address, uint32_t(data.size()));
        memcpy(g_memory.Translate<uint8_t>(address), data.data(), data.size());
        return true;
    }

    bool XeniaWantsSingleSampleScene()
    {
        return xe::gpu::d3d12::DaoUpscaler::WantsSingleSampleScene();
    }

    void XeniaWriteSwapPacket(uint32_t buffer, uint32_t fetchConstant, uint32_t frontBufferVirtual,
        uint32_t width, uint32_t height)
    {
        namespace xenos = xe::gpu::xenos;

        // The fetch constant comes from the D3D texture header, which holds a virtual
        // address; the GPU wants the physical one.
        xenos::xe_gpu_texture_fetch_t fetch;
        xe::copy_and_swap_32_unaligned(&fetch, g_memory.Translate<uint32_t>(fetchConstant), 6);
        const uint32_t frontBufferPhysical = PhysicalAddressOf(frontBufferVirtual);
        fetch.base_address = frontBufferPhysical >> 12;

        auto* dwords = g_memory.Translate<xe::be<uint32_t>>(buffer);
        uint32_t offset = 0;
        dwords[offset++] = xenos::MakePacketType0(xe::gpu::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, 6);
        dwords[offset++] = fetch.dword_0;
        dwords[offset++] = fetch.dword_1;
        dwords[offset++] = fetch.dword_2;
        dwords[offset++] = fetch.dword_3;
        dwords[offset++] = fetch.dword_4;
        dwords[offset++] = fetch.dword_5;
        dwords[offset++] = xenos::MakePacketType3(xenos::PM4_XE_SWAP, 4);
        dwords[offset++] = xe::gpu::xenos::kSwapSignature;
        dwords[offset++] = frontBufferPhysical;
        dwords[offset++] = width;
        dwords[offset++] = height;
        while (offset < 64)
            dwords[offset++] = xenos::MakePacketType2();
    }

    bool CaptureFrame(const std::filesystem::path& path, double aspect, bool sdr)
    {
        if (!s_graphics || !s_graphics->presenter())
            return false;
        xe::ui::RawImage image;
        if (!s_graphics->presenter()->CaptureGuestOutput(image, sdr) || !image.width || !image.height)
            return false;
        // The middle of it at `aspect`.
        const auto [frameWidth, frameHeight] = FrameSize(image.width, image.height, aspect);
        const uint32_t width = std::clamp(uint32_t(std::lround(frameWidth)), 1u, image.width);
        const uint32_t height = std::clamp(uint32_t(std::lround(frameHeight)), 1u, image.height);
        const uint32_t left = (image.width - width) / 2, top = (image.height - height) / 2;
        // RGBX -> RGB
        std::vector<uint8_t> rgb(size_t(width) * height * 3);
        for (uint32_t y = 0; y < height; y++)
        {
            const uint8_t* row = image.data.data() + size_t(top + y) * image.stride + size_t(left) * 4;
            for (uint32_t x = 0; x < width; x++)
                memcpy(&rgb[(size_t(y) * width + x) * 3], row + x * 4, 3);
        }
        return stbi_write_png(Utf8(path).c_str(), int(width), int(height), 3, rgb.data(), int(width) * 3) != 0;
    }

    std::pair<double, double> FrameSize(double width, double height, double aspect)
    {
        if (aspect <= 0)
            return { width, height };
        return { std::min(width, height * aspect), std::min(height, width / aspect) };
    }

    uint64_t HashFrame()
    {
        if (!s_graphics || !s_graphics->presenter())
            return 0;
        xe::ui::RawImage image;
        if (!s_graphics->presenter()->CaptureGuestOutput(image) || !image.width)
            return 0;
        return std::hash<std::string_view>{}(
            std::string_view(reinterpret_cast<const char*>(image.data.data()), image.data.size()));
    }

    bool CaptureHalfRes(std::vector<uint8_t>& rgb, uint32_t& width, uint32_t& height)
    {
        if (!s_graphics || !s_graphics->presenter())
            return false;
        xe::ui::RawImage image;
        if (!s_graphics->presenter()->CaptureGuestOutput(image) || image.width < 2 || image.height < 2)
            return false;
        width = image.width / 2;
        height = image.height / 2;
        rgb.resize(size_t(width) * height * 3);
        for (uint32_t y = 0; y < height; y++)
        {
            const uint8_t* row0 = image.data.data() + size_t(y * 2) * image.stride;
            const uint8_t* row1 = row0 + image.stride;
            uint8_t* out = &rgb[size_t(y) * width * 3];
            for (uint32_t x = 0; x < width; x++, out += 3)
            {
                for (int c = 0; c < 3; c++)
                    out[c] = uint8_t((row0[x * 8 + c] + row0[x * 8 + 4 + c] + row1[x * 8 + c] + row1[x * 8 + 4 + c] + 2) / 4);
            }
        }
        return true;
    }

    bool WritePng(const std::filesystem::path& path, const uint8_t* rgb, uint32_t width, uint32_t height)
    {
        return stbi_write_png(Utf8(path).c_str(), int(width), int(height), 3, rgb, int(width) * 3) != 0;
    }

    void CallInterruptCallback(uint32_t callback, uint32_t userData, uint32_t source, uint32_t cpu)
    {
        // Interrupts are serialised (the console delivers them one at a time) and run
        // with the PCR claiming the target hardware thread.
        std::lock_guard lock(s_interruptMutex);
        GuestThread* thread = GuestThread::Current();
        if (!thread)
            thread = GuestThread::AttachHostThread("GPU interrupt");
        if (cpu > 5)
            cpu = 2;
        *g_memory.Translate<uint8_t>(thread->pcr + kPcrCurrentCpu) = uint8_t(cpu);
        CallGuest(callback, { source, userData });
    }
}

namespace xe::dao_host
{
    void AttachGuestThread(const char* name)
    {
        dao::kernel::GuestThread::AttachHostThread(name);
    }

    void DispatchInterrupt(uint32_t callback, uint32_t user_data, uint32_t source, uint32_t cpu)
    {
        dao::gpu::CallInterruptCallback(callback, user_data, source, cpu);
    }

    uint32_t TitleId()
    {
        return dao::kernel::g_module.titleId;
    }

    void ShortSleep(uint32_t microseconds)
    {
        dao::kernel::ShortSleep(microseconds);
    }

    bool PollGpuWaits()
    {
        return dao::FrameRateAbove60(dao::gpu::FrameRate()) ||
            !xe::dao_live::Load(xe::dao_live::values.present_vsync);
    }
}
