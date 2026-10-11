"""Patches the vendored Xenia GPU (xenia/src/xenia) to run inside the DAO runtime
instead of the Xenia emulator: no kernel/CPU modules, interrupts and guest
threads go through xenia/dao_host.h, memory is DAORecomp's (see xenia/memory.h).

Idempotent: each replacement is skipped if its result is already present.

Historical: the vendored tree has since been edited in place (the upscaler,
HDR, GPU timing, the frame limiter...), so this no longer reproduces it or
applies cleanly to it, and isn't part of the build. Don't run it on the tree.
"""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent / "xenia" / "src" / "xenia"
failed = False


def patch(rel, old, new, count=1):
    global failed
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    if new in text and old not in text:
        return
    # Insertions keep `old` inside `new`: applied once `new` is present.
    if new and old in new and new in text:
        return
    if old not in text:
        print(f"PATCH FAILED: {rel}: {old[:70]!r}")
        failed = True
        return
    text = text.replace(old, new, count)
    path.write_text(text, encoding="utf-8")


# ---------------------------------------------------------------- graphics_system.h
patch("gpu/graphics_system.h", '#include "xenia/cpu/processor.h"\n', "")
patch("gpu/graphics_system.h", '#include "xenia/kernel/xthread.h"\n',
      '#include "xenia/base/threading.h"\n')
patch("gpu/graphics_system.h", """  cpu::Processor* processor() const { return processor_; }
  kernel::KernelState* kernel_state() const { return kernel_state_; }
""", "")
patch("gpu/graphics_system.h", """  virtual X_STATUS Setup(cpu::Processor* processor,
                         kernel::KernelState* kernel_state,
                         ui::WindowedAppContext* app_context,
                         bool with_presentation);""", """  virtual X_STATUS Setup(Memory* memory, ui::WindowedAppContext* app_context,
                         bool with_presentation);""")
patch("gpu/graphics_system.h", """ protected:
  GraphicsSystem();

  virtual std::unique_ptr<CommandProcessor> CreateCommandProcessor() = 0;

  static uint32_t ReadRegisterThunk(void* ppc_context, GraphicsSystem* gs,
                                    uint32_t addr);
  static void WriteRegisterThunk(void* ppc_context, GraphicsSystem* gs,
                                 uint32_t addr, uint32_t value);
  uint32_t ReadRegister(uint32_t addr);
  void WriteRegister(uint32_t addr, uint32_t value);
""", """  // MMIO register access (0x7FC80000 window), called by the DAO runtime.
  uint32_t ReadRegister(uint32_t addr);
  void WriteRegister(uint32_t addr, uint32_t value);

 protected:
  GraphicsSystem();

  virtual std::unique_ptr<CommandProcessor> CreateCommandProcessor() = 0;
""")
patch("gpu/graphics_system.h", """  Memory* memory_ = nullptr;
  cpu::Processor* processor_ = nullptr;
  kernel::KernelState* kernel_state_ = nullptr;
""", """  Memory* memory_ = nullptr;
""")
patch("gpu/graphics_system.h", """  kernel::object_ref<kernel::XHostThread> frame_limiter_worker_thread_;""",
      """  std::thread frame_limiter_worker_thread_;""")

# ---------------------------------------------------------------- graphics_system.cc
patch("gpu/graphics_system.cc", '#include "xenia/config.h"\n', '#include "xenia/dao_host.h"\n')
patch("gpu/graphics_system.cc", '#include "xenia/kernel/kernel_state.h"\n', "")
patch("gpu/graphics_system.cc", """X_STATUS GraphicsSystem::Setup(cpu::Processor* processor,
                               kernel::KernelState* kernel_state,
                               ui::WindowedAppContext* app_context,
                               bool with_presentation) {
  memory_ = processor->memory();
  processor_ = processor;
  kernel_state_ = kernel_state;
  app_context_ = app_context;""", """X_STATUS GraphicsSystem::Setup(Memory* memory,
                               ui::WindowedAppContext* app_context,
                               bool with_presentation) {
  memory_ = memory;
  app_context_ = app_context;""")
# Register access comes from the runtime's MMIO store hook instead.
patch("gpu/graphics_system.cc", """  // Let the processor know we want register access callbacks.
  memory_->AddVirtualMappedRange(
      0x7FC80000, 0xFFFF0000, 0x0000FFFF, this,
      reinterpret_cast<cpu::MMIOReadCallback>(ReadRegisterThunk),
      reinterpret_cast<cpu::MMIOWriteCallback>(WriteRegisterThunk));

""", "")
# Frame limiter / vblank thread: a plain host thread with a guest identity
# (it runs the vblank interrupt callback).
patch("gpu/graphics_system.cc", """  frame_limiter_worker_thread_ =
      kernel::object_ref<kernel::XHostThread>(new kernel::XHostThread(
          kernel_state_, 128 * 1024, 0,
          [this]() {""", """  frame_limiter_worker_thread_ = std::thread(
          [this]() {
            xe::dao_host::AttachGuestThread("GPU Frame limiter");
            xe::threading::set_name("GPU Frame limiter");""")
patch("gpu/graphics_system.cc", """              // If there is no title running then there is no need for guest
              // frame limiter thread.
              if (!kernel_state_->is_title_open()) {
                xe::threading::Sleep(std::chrono::milliseconds(100));
                continue;
              }

""", """              if (paused_) {
                xe::threading::Sleep(std::chrono::milliseconds(10));
                continue;
              }

""")
patch("gpu/graphics_system.cc", """            return 0;
          },
          kernel_state->GetIdleProcess()));
  // As we run vblank interrupts the debugger must be able to suspend us.
  frame_limiter_worker_thread_->set_can_debugger_suspend(true);
  frame_limiter_worker_thread_->set_name("GPU Frame limiter");
  frame_limiter_worker_thread_->Create();
  // The Windows branch spins for part of each period, which is what the
  // lowest priority is for. The Linux branch only sleeps, so there it buys
  // nothing and costs wake-up latency: kLowest maps to nice 15 against nice 0
  // for every other thread in the process, and any other busy program then
  // delays the guest's vblanks.
#if XE_PLATFORM_WIN32
  frame_limiter_worker_thread_->thread()->set_priority(
      threading::ThreadPriority::kLowest);
#else
  frame_limiter_worker_thread_->thread()->set_priority(
      threading::ThreadPriority::kNormal);
#endif
""", """            return 0;
          });
""")
patch("gpu/graphics_system.cc", """  if (frame_limiter_worker_thread_) {
    frame_limiter_worker_running_ = false;
    frame_limiter_worker_thread_->Wait(0, 0, 0, nullptr);
    frame_limiter_worker_thread_.reset();
  }""", """  if (frame_limiter_worker_thread_.joinable()) {
    frame_limiter_worker_running_ = false;
    frame_limiter_worker_thread_.join();
  }""")
patch("gpu/graphics_system.cc", """  config::SaveConfig();

  xe::FatalError""", """  xe::FatalError""")
patch("gpu/graphics_system.cc", """uint32_t GraphicsSystem::ReadRegisterThunk(void* ppc_context,
                                           GraphicsSystem* gs, uint32_t addr) {
  return gs->ReadRegister(addr);
}

void GraphicsSystem::WriteRegisterThunk(void* ppc_context, GraphicsSystem* gs,
                                        uint32_t addr, uint32_t value) {
  gs->WriteRegister(addr, value);
}

""", "")
patch("gpu/graphics_system.cc", """  kernel_state()->EmulateCPInterruptDPC(interrupt_callback_,
                                        interrupt_callback_data_, source, cpu);""",
      """  if (interrupt_callback_) {
    xe::dao_host::DispatchInterrupt(interrupt_callback_,
                                    interrupt_callback_data_, source, cpu);
  }""")
patch("gpu/graphics_system.cc", """  if (!kernel_state_) {
    return {1280, 720};
  }

  if (cvars::custom_internal_display_resolution_x != 0 &&
      cvars::custom_internal_display_resolution_y != 0) {
    return {cvars::custom_internal_display_resolution_x,
            cvars::custom_internal_display_resolution_y};
  }

  const auto resolution =
      kernel::Resolution(kernel_state()->xconfig()->ReadSetting<uint32_t>(
          kernel::XCONFIG_USER_CATEGORY,
          kernel::XCONFIG_USER_AV_COMPOSITE_SCREENSZ));

  return {resolution.width_, resolution.height_};""", """  if (cvars::custom_internal_display_resolution_x != 0 &&
      cvars::custom_internal_display_resolution_y != 0) {
    return {cvars::custom_internal_display_resolution_x,
            cvars::custom_internal_display_resolution_y};
  }
  return {1280, 720};""")

# ---------------------------------------------------------------- command_processor.h
patch("gpu/command_processor.h", '#include "xenia/kernel/xthread.h"\n', '#include "xenia/base/threading.h"\n')
patch("gpu/command_processor.h", """  CommandProcessor(GraphicsSystem* graphics_system,
                   kernel::KernelState* kernel_state);""", """  explicit CommandProcessor(GraphicsSystem* graphics_system);""")
patch("gpu/command_processor.h", """  kernel::KernelState* kernel_state_ = nullptr;\n""", "")
patch("gpu/command_processor.h", """  kernel::object_ref<kernel::XHostThread> worker_thread_;""",
      """  std::unique_ptr<xe::threading::Thread> worker_thread_;""")

# ---------------------------------------------------------------- command_processor.cc
patch("gpu/command_processor.cc", '#include "xenia/kernel/kernel_state.h"\n', '#include "xenia/dao_host.h"\n')
patch("gpu/command_processor.cc", '#include "xenia/kernel/user_module.h"\n', "")
patch("gpu/command_processor.cc", """CommandProcessor::CommandProcessor(GraphicsSystem* graphics_system,
                                   kernel::KernelState* kernel_state)
    : reader_(nullptr, 0),
      memory_(graphics_system->memory()),
      kernel_state_(kernel_state),""", """CommandProcessor::CommandProcessor(GraphicsSystem* graphics_system)
    : reader_(nullptr, 0),
      memory_(graphics_system->memory()),""")
patch("gpu/command_processor.cc", """  worker_thread_ =
      kernel::object_ref<kernel::XHostThread>(new kernel::XHostThread(
          kernel_state_, 128 * 1024, 0,
          [this]() {
            WorkerThreadMain();
            return 0;
          },
          kernel_state_->GetIdleProcess()));
  worker_thread_->set_name("GPU Commands");
  worker_thread_->Create();""", """  xe::threading::Thread::CreationParameters params;
  params.stack_size = 16 * 1024 * 1024;
  worker_thread_ = xe::threading::Thread::Create(params, [this]() {
    xe::dao_host::AttachGuestThread("GPU Commands");
    WorkerThreadMain();
  });
  worker_thread_->set_name("GPU Commands");""")
patch("gpu/command_processor.cc", """  worker_thread_->Wait(0, 0, 0, nullptr);
  worker_thread_.reset();""", """  xe::threading::Wait(worker_thread_.get(), false);
  worker_thread_.reset();""")
patch("gpu/command_processor.cc", """      kernel::XThread::IsInThread(worker_thread_.get())) {""",
      """      xe::threading::Thread::GetCurrentThread() == worker_thread_.get()) {""")
patch("gpu/command_processor.cc", """  worker_thread_->thread()->Resume();""", """  worker_thread_->Resume();""")
patch("gpu/command_processor.cc", """  std::memset(kernel_state_->memory()->TranslatePhysical(primary_buffer_ptr_),""",
      """  std::memset(memory_->TranslatePhysical(primary_buffer_ptr_),""")
patch("gpu/command_processor.cc", """XE_NOINLINE XE_COLD void CommandProcessor::LogKickoffInitator(uint32_t value) {
  cpu::backend::GuestPseudoStackTrace st;

  if (logging::ShouldLog(LogLevel::Debug) &&
      kernel_state_->processor()->backend()->PopulatePseudoStacktrace(&st)) {
    logging::LoggerBatch<LogLevel::Debug> log_initiator{};

    log_initiator("Updating read ptr to {}, initiator stacktrace below\\n",
                  value);

    for (uint32_t i = 0; i < st.count; ++i) {
      log_initiator("\\t{:08X}\\n", st.return_addrs[i]);
    }

    if (st.truncated_flag) {
      log_initiator("\\t(Truncated stacktrace to {} entries)\\n",
                    cpu::backend::MAX_GUEST_PSEUDO_STACKTRACE_ENTRIES);
    }
    log_initiator.submit('d');
  }
}""", """XE_NOINLINE XE_COLD void CommandProcessor::LogKickoffInitator(uint32_t value) {
  XELOGD("Updating write ptr to {}", value);
}""")

# ---------------------------------------------------------------- pm4 implement
patch("gpu/pm4_command_processor_implement.h",
      """        uint32_t title_id = kernel_state_->GetExecutableModule()->title_id();""",
      """        uint32_t title_id = xe::dao_host::TitleId();""")
patch("gpu/pm4_command_processor_implement.h",
      """    uint32_t title_id = kernel_state_->GetExecutableModule()
                            ? kernel_state_->GetExecutableModule()->title_id()
                            : 0;""", """    uint32_t title_id = xe::dao_host::TitleId();""")

# ---------------------------------------------------------------- d3d12
patch("gpu/d3d12/d3d12_command_processor.h", """                                 kernel::KernelState* kernel_state);""",
      """                                 void* unused = nullptr);""")
patch("gpu/d3d12/d3d12_command_processor.cc", """    D3D12GraphicsSystem* graphics_system, kernel::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state),""", """    D3D12GraphicsSystem* graphics_system, void* unused)
    : CommandProcessor(graphics_system),""")
patch("gpu/d3d12/d3d12_command_processor.cc", '#include "xenia/kernel/kernel_state.h"\n', "")
patch("gpu/d3d12/d3d12_command_processor.cc", """  auto* audio_system = kernel_state_->emulator()->audio_system();
  if (audio_system) {
    title << " - " << audio_system->name();
  }
""", "")
patch("gpu/d3d12/d3d12_graphics_system.h", """  X_STATUS Setup(cpu::Processor* processor, kernel::KernelState* kernel_state,
                 ui::WindowedAppContext* app_context,
                 bool with_presentation) override;""", """  X_STATUS Setup(Memory* memory, ui::WindowedAppContext* app_context,
                 bool with_presentation) override;""")
patch("gpu/d3d12/d3d12_graphics_system.cc", """X_STATUS D3D12GraphicsSystem::Setup(cpu::Processor* processor,
                                    kernel::KernelState* kernel_state,
                                    ui::WindowedAppContext* app_context,
                                    bool with_presentation) {
  provider_ = xe::ui::d3d12::D3D12Provider::Create();
  return GraphicsSystem::Setup(processor, kernel_state, app_context,
                               with_presentation);""", """X_STATUS D3D12GraphicsSystem::Setup(Memory* memory,
                                    ui::WindowedAppContext* app_context,
                                    bool with_presentation) {
  provider_ = xe::ui::d3d12::D3D12Provider::Create();
  return GraphicsSystem::Setup(memory, app_context, with_presentation);""")
patch("gpu/d3d12/d3d12_graphics_system.cc", """      new D3D12CommandProcessor(this, kernel_state_));""",
      """      new D3D12CommandProcessor(this));""")


def patch_function_body(rel, signature, new_body):
    """Replaces the body of the function starting at `signature` (up to the first
    line that is exactly "}")."""
    global failed
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    start = text.find(signature)
    if start < 0:
        print(f"PATCH FAILED: {rel}: {signature!r}")
        failed = True
        return
    end = text.find("\n}\n", start)
    replacement = signature + "\n" + new_body + "\n}"
    if text[start:end + 2] == replacement:
        return
    text = text[:start] + replacement + text[end + 2:]
    path.write_text(text, encoding="utf-8")


# ---------------------------------------------------------------- ui: no hid module
patch("ui/imgui_drawer.h", '#include "xenia/hid/input_system.h"\n',
      "namespace xe::hid {\nclass InputSystem;\n}  // namespace xe::hid\n")
patch_function_body("ui/imgui_drawer.cc", "void ImGuiDrawer::UpdateGamepads() {",
                    "  // DAO: gamepad navigation of Xenia's ImGui dialogs is not used.")

# ---------------------------------------------------------------- d3d12: DXC headers
# (DxbcConverter is only loaded at runtime when dxilconv.dll is present.)

# ---------------------------------------------------------------- clang-cl fixes
patch("gpu/draw_util.cc", """#if XE_PLATFORM_WIN32
  __m128i xyoffsetadd = _mm_cvtsi64x_si128(""", """#if XE_PLATFORM_WIN32 && !defined(__clang__)
  __m128i xyoffsetadd = _mm_cvtsi64x_si128(""")
patch("base/mapped_memory_win.cc",
      "  static inline constexpr HANDLE kFileHandleInvalid = INVALID_HANDLE_VALUE;",
      "  static inline const HANDLE kFileHandleInvalid = INVALID_HANDLE_VALUE;")

patch("gpu/d3d12/d3d12_command_processor.h", """#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
""", "")
patch("gpu/d3d12/d3d12_command_processor.cc", '#include "xenia/apu/audio_system.h"\n', "")
patch("gpu/d3d12/d3d12_command_processor.cc", '#include "xenia/emulator.h"\n', "")

# ---------------------------------------------------------------- DAO: HDR output
# present_hdr: an R16G16B16A16_FLOAT scRGB swap chain. Everything is painted to an
# R10G10B10A2 "HDR source" (idle in COMMON, so the paint loop's PRESENT<->RT
# barriers apply unchanged), then shaders/dao_hdr_output.* encode it into the back
# buffer: linearize, and on HDR displays scale to paper white + expand highlights.
PH = "ui/d3d12/d3d12_presenter.h"
PC = "ui/d3d12/d3d12_presenter.cc"
patch(PH, "  static constexpr DXGI_FORMAT kSwapChainFormat = DXGI_FORMAT_B8G8R8A8_UNORM;\n",
      "  static constexpr DXGI_FORMAT kSwapChainFormat = DXGI_FORMAT_B8G8R8A8_UNORM;\n"
      "  static constexpr DXGI_FORMAT kSwapChainHdrFormat =\n"
      "      DXGI_FORMAT_R16G16B16A16_FLOAT;\n"
      "  static constexpr DXGI_FORMAT kHdrSourceFormat = DXGI_FORMAT_R10G10B10A2_UNORM;\n")
patch(PH, """      kRTVCount =
          kRTVIndexGuestOutputIntermediate0 + kGuestOutputMailboxSize - 1,
""", """      kRTVIndexHdrSource =
          kRTVIndexGuestOutputIntermediate0 + kMaxGuestOutputPaintEffects - 1,

      kRTVCount,
""")
patch(PH, """      kViewCount = kViewIndexGuestOutputIntermediate0Srv +
                   kMaxGuestOutputPaintEffects - 1,
""", """      kViewIndexHdrSourceSrv = kViewIndexGuestOutputIntermediate0Srv +
                               kMaxGuestOutputPaintEffects - 1,

      kViewCount,
""")
patch(PH, """    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kSwapChainBufferCount>
        swap_chain_buffers;
  };
""", """    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kSwapChainBufferCount>
        swap_chain_buffers;
    // DAO HDR output.
    bool swap_chain_hdr = false;
    bool display_hdr = false;
    Microsoft::WRL::ComPtr<ID3D12Resource> hdr_source;
  };
""")
patch(PH, """             size_t(GuestOutputPaintEffect::kCount)>
      guest_output_paint_final_pipelines_;
""", """             size_t(GuestOutputPaintEffect::kCount)>
      guest_output_paint_final_pipelines_;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> hdr_output_root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> hdr_output_pipeline_;
""")

patch(PC, """    "D3D12");

namespace xe {""", """    "D3D12");

DEFINE_bool(present_hdr, false,
            "Present through an scRGB (HDR) swap chain, expanding the SDR image "
            "on HDR displays.",
            "Display");
DEFINE_double(present_hdr_paper_white, 200.0,
              "HDR: brightness of SDR white in nits.", "Display");
DEFINE_double(present_hdr_peak, 1000.0,
              "HDR: peak brightness highlights are expanded to, in nits.",
              "Display");
DEFINE_double(present_hdr_gamma, 2.2,
              "HDR: gamma the guest output is decoded with.", "Display");

namespace xe {""")
patch(PC, '#include "xenia/ui/shaders/bytecode/d3d12_5_1/guest_output_triangle_strip_rect_vs.h"\n',
      '#include "xenia/ui/shaders/bytecode/d3d12_5_1/guest_output_triangle_strip_rect_vs.h"\n'
      '#include "xenia/ui/shaders/bytecode/d3d12_5_1/dao_hdr_output_ps.h"\n'
      '#include "xenia/ui/shaders/bytecode/d3d12_5_1/dao_hdr_output_vs.h"\n')
patch(PC, "    swap_chain_desc.Format = kSwapChainFormat;\n",
      "    swap_chain_desc.Format =\n"
      "        cvars::present_hdr ? kSwapChainHdrFormat : kSwapChainFormat;\n")
patch(PC, """  ID3D12Device* device = provider_.GetDevice();

  // Create the RTV descriptors.
""", """  ID3D12Device* device = provider_.GetDevice();

  // DAO: HDR output - color space, display capability and the HDR source.
  paint_context_.swap_chain_hdr = cvars::present_hdr;
  paint_context_.hdr_source.Reset();
  if (paint_context_.swap_chain_hdr) {
    paint_context_.swap_chain->SetColorSpace1(
        DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
    paint_context_.display_hdr = false;
    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    Microsoft::WRL::ComPtr<IDXGIOutput6> output6;
    DXGI_OUTPUT_DESC1 output_desc;
    if (SUCCEEDED(paint_context_.swap_chain->GetContainingOutput(&output)) &&
        SUCCEEDED(output.As(&output6)) &&
        SUCCEEDED(output6->GetDesc1(&output_desc))) {
      paint_context_.display_hdr =
          output_desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    }
    XELOGI("D3D12Presenter: scRGB output on an {} display",
           paint_context_.display_hdr ? "HDR" : "SDR");
    D3D12_RESOURCE_DESC hdr_source_desc = {};
    hdr_source_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    hdr_source_desc.Width = paint_context_.swap_chain_width;
    hdr_source_desc.Height = paint_context_.swap_chain_height;
    hdr_source_desc.DepthOrArraySize = 1;
    hdr_source_desc.MipLevels = 1;
    hdr_source_desc.Format = kHdrSourceFormat;
    hdr_source_desc.SampleDesc.Count = 1;
    hdr_source_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (FAILED(device->CreateCommittedResource(
            &util::kHeapPropertiesDefault, provider_.GetHeapFlagCreateNotZeroed(),
            &hdr_source_desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&paint_context_.hdr_source)))) {
      XELOGE("D3D12Presenter: Failed to create the HDR source texture");
      paint_context_.DestroySwapChain();
      return SurfacePaintConnectResult::kFailure;
    }
    D3D12_RENDER_TARGET_VIEW_DESC hdr_source_rtv_desc = {};
    hdr_source_rtv_desc.Format = kHdrSourceFormat;
    hdr_source_rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(
        paint_context_.hdr_source.Get(), &hdr_source_rtv_desc,
        provider_.OffsetRTVDescriptor(
            paint_context_.rtv_heap->GetCPUDescriptorHandleForHeapStart(),
            PaintContext::kRTVIndexHdrSource));
    D3D12_SHADER_RESOURCE_VIEW_DESC hdr_source_srv_desc = {};
    hdr_source_srv_desc.Format = kHdrSourceFormat;
    hdr_source_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    hdr_source_srv_desc.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    hdr_source_srv_desc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(
        paint_context_.hdr_source.Get(), &hdr_source_srv_desc,
        provider_.OffsetViewDescriptor(
            paint_context_.view_heap->GetCPUDescriptorHandleForHeapStart(),
            PaintContext::kViewIndexHdrSourceSrv));
  }

  // Create the RTV descriptors.
""")
patch(PC, "    rtv_desc.Format = kSwapChainFormat;\n",
      "    rtv_desc.Format = paint_context_.swap_chain_hdr ? kSwapChainHdrFormat\n"
      "                                                    : kSwapChainFormat;\n")
patch(PC, """  swap_chain.Reset();
  swap_chain_allows_tearing = false;
""", """  swap_chain.Reset();
  hdr_source.Reset();
  swap_chain_hdr = false;
  swap_chain_allows_tearing = false;
""")
patch(PC, "      guest_output_paint_pipeline_desc.RTVFormats[0] = kSwapChainFormat;\n",
      "      guest_output_paint_pipeline_desc.RTVFormats[0] =\n"
      "          cvars::present_hdr ? kHdrSourceFormat : kSwapChainFormat;\n")
patch(PC, """  // Initialize connection-independent parts of the painting context.
""", """  // DAO: HDR output encoding pass.
  if (cvars::present_hdr) {
    D3D12_DESCRIPTOR_RANGE hdr_source_range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                                               1, 0, 0, 0};
    D3D12_ROOT_PARAMETER hdr_parameters[2] = {};
    hdr_parameters[0].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    hdr_parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    hdr_parameters[0].DescriptorTable.pDescriptorRanges = &hdr_source_range;
    hdr_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    hdr_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    hdr_parameters[1].Constants.Num32BitValues = 4;
    hdr_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC hdr_root_signature_desc = {};
    hdr_root_signature_desc.NumParameters = 2;
    hdr_root_signature_desc.pParameters = hdr_parameters;
    *(hdr_output_root_signature_.ReleaseAndGetAddressOf()) =
        util::CreateRootSignature(provider_, hdr_root_signature_desc);
    if (!hdr_output_root_signature_) {
      XELOGE("D3D12Presenter: Failed to create the HDR output root signature");
      return false;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC hdr_pipeline_desc =
        guest_output_paint_pipeline_desc;
    hdr_pipeline_desc.pRootSignature = hdr_output_root_signature_.Get();
    hdr_pipeline_desc.VS.pShaderBytecode = shaders::dao_hdr_output_vs;
    hdr_pipeline_desc.VS.BytecodeLength = sizeof(shaders::dao_hdr_output_vs);
    hdr_pipeline_desc.PS.pShaderBytecode = shaders::dao_hdr_output_ps;
    hdr_pipeline_desc.PS.BytecodeLength = sizeof(shaders::dao_hdr_output_ps);
    hdr_pipeline_desc.RTVFormats[0] = kSwapChainHdrFormat;
    if (FAILED(device->CreateGraphicsPipelineState(
            &hdr_pipeline_desc, IID_PPV_ARGS(&hdr_output_pipeline_)))) {
      XELOGE("D3D12Presenter: Failed to create the HDR output pipeline");
      return false;
    }
  }

  // Initialize connection-independent parts of the painting context.
""")
patch(PC, """  bool back_buffer_acquired = false;
  bool back_buffer_bound = false;
""", """  // DAO: with HDR output, everything is painted to the HDR source first.
  ID3D12Resource* const swap_chain_back_buffer = back_buffer;
  const D3D12_CPU_DESCRIPTOR_HANDLE swap_chain_back_buffer_rtv = back_buffer_rtv;
  if (paint_context_.swap_chain_hdr) {
    back_buffer = paint_context_.hdr_source.Get();
    back_buffer_rtv = provider_.OffsetRTVDescriptor(
        rtv_heap_start, PaintContext::kRTVIndexHdrSource);
  }
  bool back_buffer_acquired = false;
  bool back_buffer_bound = false;
""")
patch(PC, """  // End drawing to the back buffer.
""", """  // DAO: encode the HDR source into the scRGB back buffer.
  if (paint_context_.swap_chain_hdr) {
    D3D12_RESOURCE_BARRIER hdr_barriers[2] = {};
    hdr_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    hdr_barriers[0].Transition.pResource = back_buffer;
    hdr_barriers[0].Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    hdr_barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    hdr_barriers[0].Transition.StateAfter =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    hdr_barriers[1] = hdr_barriers[0];
    hdr_barriers[1].Transition.pResource = swap_chain_back_buffer;
    hdr_barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    hdr_barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    command_list->ResourceBarrier(2, hdr_barriers);
    command_list->OMSetRenderTargets(1, &swap_chain_back_buffer_rtv, true,
                                     nullptr);
    D3D12_VIEWPORT hdr_viewport = {
        0.0f, 0.0f, float(paint_context_.swap_chain_width),
        float(paint_context_.swap_chain_height), 0.0f, 1.0f};
    command_list->RSSetViewports(1, &hdr_viewport);
    D3D12_RECT hdr_scissor = {0, 0, LONG(paint_context_.swap_chain_width),
                              LONG(paint_context_.swap_chain_height)};
    command_list->RSSetScissorRects(1, &hdr_scissor);
    ID3D12DescriptorHeap* hdr_view_heap = paint_context_.view_heap.Get();
    command_list->SetDescriptorHeaps(1, &hdr_view_heap);
    command_list->SetPipelineState(hdr_output_pipeline_.Get());
    command_list->SetGraphicsRootSignature(hdr_output_root_signature_.Get());
    command_list->SetGraphicsRootDescriptorTable(
        0, provider_.OffsetViewDescriptor(
               hdr_view_heap->GetGPUDescriptorHandleForHeapStart(),
               PaintContext::kViewIndexHdrSourceSrv));
    const bool display_hdr = paint_context_.display_hdr;
    const float paper_white =
        float(std::max(cvars::present_hdr_paper_white, 80.0));
    const float hdr_constants[4] = {
        display_hdr ? paper_white / 80.0f : 1.0f,
        display_hdr
            ? std::max(float(cvars::present_hdr_peak) / paper_white, 1.0f)
            : 1.0f,
        float(cvars::present_hdr_gamma), 0.0f};
    command_list->SetGraphicsRoot32BitConstants(1, 4, hdr_constants, 0);
    command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    command_list->DrawInstanced(3, 1, 0, 0);
    hdr_barriers[0].Transition.StateBefore =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    hdr_barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    command_list->ResourceBarrier(1, hdr_barriers);
    back_buffer = swap_chain_back_buffer;
  }

  // End drawing to the back buffer.
""")

# ---------------------------------------------------------------- DAO: frame trace
CP = "gpu/d3d12/d3d12_command_processor.cc"
patch(CP, '#include "xenia/gpu/d3d12/d3d12_command_processor.h"\n',
      '#include "xenia/gpu/d3d12/d3d12_command_processor.h"\n'
      '#include "xenia/gpu/d3d12/dao_frame_trace.h"\n')
patch(CP, "  texture_cache_->RequestTextures(used_texture_mask);\n",
      "  texture_cache_->RequestTextures(used_texture_mask);\n"
      "  dao_trace::LogDraw(\n"
      "      regs, vertex_shader->ucode_data_hash(),\n"
      "      pixel_shader ? pixel_shader->ucode_data_hash() : 0,\n"
      "      normalized_color_mask,\n"
      "      pixel_shader ? pixel_shader->GetUsedTextureMaskAfterTranslation() : 0,\n"
      "      vertex_shader->GetUsedTextureMaskAfterTranslation(), primitive_type,\n"
      "      index_count);\n")
patch(CP, """bool D3D12CommandProcessor::IssueCopy() {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES
""", """bool D3D12CommandProcessor::IssueCopy() {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES
  dao_trace::LogResolve(*register_file_);
""")
patch(CP, """                                      uint32_t frontbuffer_height) {
  SCOPE_profile_cpu_f("gpu");
  EndZPDFrame();
""", """                                      uint32_t frontbuffer_height) {
  SCOPE_profile_cpu_f("gpu");
  dao_trace::OnSwap(frontbuffer_ptr);
  EndZPDFrame();
""")

# Per-frame draw statistics for the flicker detector.
PM4 = "gpu/pm4_command_processor_implement.h"
patch("gpu/command_processor.cc", '#include "xenia/gpu/command_processor.h"\n',
      '#include "xenia/gpu/command_processor.h"\n'
      '#include "xenia/gpu/d3d12/dao_frame_trace.h"\n')
patch(PM4, """  if (draw_succeeded) {
    // A consumer draw whose survey is still outstanding runs under the
    // backend's predicate instead of blocking. Surveys themselves are
    // ordinary draws here.
    if (COMMAND_PROCESSOR::PrepareVIZDraw(viz_query_condition)) {""", """  if (draw_succeeded) {
    dao_trace::CountSubmittedDraw(
        *register_file_, is_indexed ? index_buffer_info.guest_base : 0,
        vgt_draw_initiator.num_indices,
        uint32_t(vgt_draw_initiator.prim_type));
    // A consumer draw whose survey is still outstanding runs under the
    // backend's predicate instead of blocking. Surveys themselves are
    // ordinary draws here.
    if (!COMMAND_PROCESSOR::PrepareVIZDraw(viz_query_condition)) {
      dao_trace::CountVizSkippedDraw();
    } else {""")
patch(PM4, """      viz_draw_predicate_ = {};
      if (!draw_succeeded) {
        XELOGE("{}({}, {}, {}): Failed in backend", opcode_name,""", """      viz_draw_predicate_ = {};
      if (!draw_succeeded) {
        dao_trace::CountFailedDraw();
        XELOGE("{}({}, {}, {}): Failed in backend", opcode_name,""")

# ---------------------------------------------------------------- DAO: temporal upscaling
# gpu/d3d12/dao_upscale.{h,cc}: FSR 3.1 / FSR 4 on the scene, UI composited after.
patch("gpu/draw_util.h", """  uint32_t rb_copy_mask;
""", """  uint32_t rb_copy_mask;

  // DAO: the resolve rectangle in render target pixels (x0, y0, x1, y1) and the
  // window offset that was added to it, for copying the scene depth tile by
  // tile (dao_upscale).
  int32_t dao_rect[4];
  int32_t dao_window_offset[2];
""")
patch("gpu/draw_util.cc", """  x1 = xe::align(x1, int32_t(xenos::kResolveAlignmentPixels));
  y1 = xe::align(y1, int32_t(xenos::kResolveAlignmentPixels));
""", """  x1 = xe::align(x1, int32_t(xenos::kResolveAlignmentPixels));
  y1 = xe::align(y1, int32_t(xenos::kResolveAlignmentPixels));

  info_out.dao_rect[0] = x0;
  info_out.dao_rect[1] = y0;
  info_out.dao_rect[2] = x1;
  info_out.dao_rect[3] = y1;
  const bool dao_window_offset_enabled =
      regs.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable;
  info_out.dao_window_offset[0] =
      dao_window_offset_enabled ? pa_sc_window_offset.window_x_offset : 0;
  info_out.dao_window_offset[1] =
      dao_window_offset_enabled ? pa_sc_window_offset.window_y_offset : 0;
""")

# A command that calls back into the executor with the real command list (for
# AMD FidelityFX, which records its own commands).
DCL = "gpu/d3d12/deferred_command_list.h"
patch(DCL, """  void D3DWriteBufferImmediate(D3D12_GPU_VIRTUAL_ADDRESS dest, UINT value) {""",
      """  // DAO: calls callback(context, command_list) when executed, for external
  // libraries that record into the command list themselves. The caller must
  // re-record any state the callback changes.
  using DaoCallbackFunction = void (*)(void* context,
                                       ID3D12GraphicsCommandList* command_list);
  void DaoCallback(DaoCallbackFunction callback, void* context) {
    auto& args = *reinterpret_cast<DaoCallbackArguments*>(
        WriteCommand(Command::kDaoCallback, sizeof(DaoCallbackArguments)));
    args.callback = callback;
    args.context = context;
  }

  void D3DWriteBufferImmediate(D3D12_GPU_VIRTUAL_ADDRESS dest, UINT value) {""")
patch(DCL, """    kD3DWriteBufferImmediate,
  };""", """    kD3DWriteBufferImmediate,
    kDaoCallback,
  };""")
patch(DCL, """  struct D3DWriteBufferImmediateArguments {
    D3D12_GPU_VIRTUAL_ADDRESS dest;
    UINT value;
  };
""", """  struct D3DWriteBufferImmediateArguments {
    D3D12_GPU_VIRTUAL_ADDRESS dest;
    UINT value;
  };

  struct DaoCallbackArguments {
    DaoCallbackFunction callback;
    void* context;
  };
""")
patch("gpu/d3d12/deferred_command_list.cc", """          command_list_2->WriteBufferImmediate(1, &param, &mode);
        }
      } break;
""", """          command_list_2->WriteBufferImmediate(1, &param, &mode);
        }
      } break;
      case Command::kDaoCallback: {
        auto& args = *reinterpret_cast<const DaoCallbackArguments*>(stream);
        args.callback(args.context, command_list);
      } break;
""")

patch("ui/presenter.h", """  const GuestOutputPaintConfig& GetGuestOutputPaintConfigFromUIThread() const {""",
      """  // DAO: the size of the surface being painted to (0 if none), read by the
  // GPU thread to size the temporal upscaler output (a stale value only delays
  // a resize by a frame).
  void GetPaintSurfaceSize(uint32_t& width_out, uint32_t& height_out) const {
    width_out = surface_width_in_paint_connection_;
    height_out = surface_height_in_paint_connection_;
  }

  const GuestOutputPaintConfig& GetGuestOutputPaintConfigFromUIThread() const {""")

patch("gpu/d3d12/d3d12_command_processor.h",
      "class D3D12CommandProcessor final : public CommandProcessor {\n",
      "class D3D12CommandProcessor final : public CommandProcessor {\n"
      "  friend class DaoUpscalerImpl;\n\n")
patch("gpu/d3d12/d3d12_render_target_cache.h",
      "class D3D12RenderTargetCache final : public RenderTargetCache {\n",
      "class D3D12RenderTargetCache final : public RenderTargetCache {\n"
      "  friend class DaoUpscalerImpl;\n\n")

patch("gpu/d3d12/pipeline_cache.h", "class PipelineCache {\n",
      "class PipelineCache {\n  friend class DaoUpscalerImpl;\n\n")

RTC = "gpu/d3d12/d3d12_render_target_cache.cc"
patch(RTC, '#include "xenia/gpu/d3d12/d3d12_command_processor.h"\n',
      '#include "xenia/gpu/d3d12/d3d12_command_processor.h"\n'
      '#include "xenia/gpu/d3d12/dao_upscale.h"\n')
patch(RTC, """  if (copy_dest_info_out) {
    // The destination format in it is normalized""", """  DaoUpscaler::OnResolve(resolve_info);

  if (copy_dest_info_out) {
    // The destination format in it is normalized""")

patch(CP, '#include "xenia/gpu/d3d12/dao_frame_trace.h"\n',
      '#include "xenia/gpu/d3d12/dao_frame_trace.h"\n'
      '#include "xenia/gpu/d3d12/dao_upscale.h"\n')
patch(CP, """  std::memset(&system_constants_, 0, sizeof(system_constants_));

  return true;
}
""", """  std::memset(&system_constants_, 0, sizeof(system_constants_));

  DaoUpscaler::Initialize(*this, *render_target_cache_);

  return true;
}
""")
patch(CP, """void D3D12CommandProcessor::ShutdownContext() {
  AwaitAllQueueOperationsCompletion();
""", """void D3D12CommandProcessor::ShutdownContext() {
  AwaitAllQueueOperationsCompletion();

  DaoUpscaler::Shutdown();
""")
patch(CP, """    previous_viewport_info_args_ = gviargs;
    previous_viewport_info_ = viewport_info;
  }
""", """    previous_viewport_info_args_ = gviargs;
    previous_viewport_info_ = viewport_info;
  }
  // DAO: jitter for temporal upscaling (the cached copy stays unjittered).
  DaoUpscaler::OnDraw(
      regs, viewport_info, normalized_depth_control, normalized_color_mask,
      pixel_shader ? pixel_shader->ucode_data_hash() : 0,
      pixel_shader ? pixel_shader->GetUsedTextureMaskAfterTranslation() : 0,
      window_offset_tiles != 0);
""")
patch(CP, """  if (memexport_used) {
    // Make sure this memexporting draw is ordered with other work using shared
    // memory as a UAV.""", """  DaoUpscaler::AfterDraw(
      *vertex_shader,
      primitive_processing_result.index_buffer_type !=
          PrimitiveProcessor::ProcessedIndexBufferType::kNone,
      primitive_processing_result.host_draw_vertex_count,
      memexport_used || predicate_buffer);

  if (memexport_used) {
    // Make sure this memexporting draw is ordered with other work using shared
    // memory as a UAV.""")
# Motion vectors run the guest vertex shaders with stream output, which needs
# the flag in their root signatures.
patch(CP, """  desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

  // Base parameters.""", """  desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT;

  // Base parameters.""")
patch(CP, """    root_signature_bindless_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
""", """    root_signature_bindless_desc.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT;
""")
patch(CP, """  D3D12_RESOURCE_DESC swap_texture_desc = swap_texture_resource->GetDesc();
""", """  D3D12_RESOURCE_DESC swap_texture_desc = swap_texture_resource->GetDesc();

  // DAO: with temporal upscaling, the upscaled image (with the UI composited)
  // is presented instead of the front buffer.
  D3D12_SHADER_RESOURCE_VIEW_DESC dao_upscaled_srv_desc;
  if (ID3D12Resource* dao_upscaled = DaoUpscaler::OnSwap(
          swap_texture_resource, swap_texture_srv_desc,
          uint32_t(swap_texture_desc.Width),
          uint32_t(swap_texture_desc.Height), dao_upscaled_srv_desc)) {
    swap_texture_resource = dao_upscaled;
    swap_texture_srv_desc = dao_upscaled_srv_desc;
    swap_texture_desc = dao_upscaled->GetDesc();
  }
""")

if failed:
    sys.exit(1)
print("xenia patched")
