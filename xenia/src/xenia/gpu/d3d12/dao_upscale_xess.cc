// DAO: temporal upscaling's Intel XeSS Super Resolution backend (libxess.dll
// next to the executable, loaded on demand) - see dao_upscale.h. On GPUs
// without Intel's XMX units, XeSS runs its DP4a model.
#include "xenia/gpu/d3d12/dao_upscale_impl.h"

#include <chrono>
#include <optional>
#include <type_traits>

#include "xenia/base/logging.h"
#include "xenia/base/platform_win.h"
#include "xenia/gpu/d3d12/d3d12_command_processor.h"

// Only the types and declarations: the functions come from the library loaded
// at run time.
#include "xess/xess_d3d12.h"

namespace xe {
namespace gpu {
namespace d3d12 {

namespace {

// The presets from the least upscaling to the most, which XeSS tunes its
// model for. Their input resolution ranges reach up to the output size, so
// the first with the render size in its range is the one meant for the
// nearest ratio at or above the actual one.
constexpr xess_quality_settings_t kQualities[] = {
    XESS_QUALITY_SETTING_AA,
    XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS,
    XESS_QUALITY_SETTING_ULTRA_QUALITY,
    XESS_QUALITY_SETTING_QUALITY,
    XESS_QUALITY_SETTING_BALANCED,
    XESS_QUALITY_SETTING_PERFORMANCE,
    XESS_QUALITY_SETTING_ULTRA_PERFORMANCE,
};

const char* QualityName(xess_quality_settings_t quality) {
  switch (quality) {
    case XESS_QUALITY_SETTING_AA:
      return "native anti-aliasing";
    case XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS:
      return "ultra quality plus";
    case XESS_QUALITY_SETTING_ULTRA_QUALITY:
      return "ultra quality";
    case XESS_QUALITY_SETTING_QUALITY:
      return "quality";
    case XESS_QUALITY_SETTING_BALANCED:
      return "balanced";
    case XESS_QUALITY_SETTING_PERFORMANCE:
      return "performance";
    case XESS_QUALITY_SETTING_ULTRA_PERFORMANCE:
      return "ultra performance";
    default:
      return "?";
  }
}

// May be called from XeSS's threads.
void XessMessage(const char* message, xess_logging_level_t level) {
  static std::atomic<uint32_t> s_errors{0}, s_warnings{0};
  const bool error = level == XESS_LOGGING_LEVEL_ERROR;
  if (!LogNth(error ? s_errors : s_warnings)) {
    return;
  }
  if (error) {
    XELOGE("XeSS: {}", message ? message : "");
  } else {
    XELOGW("XeSS: {}", message ? message : "");
  }
}

class XessBackend : public UpscalerBackend {
 public:
  using UpscalerBackend::UpscalerBackend;
  ~XessBackend() override;

  const char* name() const override { return "XeSS"; }
  // XeSS takes linear color only.
  bool LinearSdrColor() const override { return true; }
  bool Sharpens() const override { return false; }
  void Prepare(bool hdr) override;
  bool EnsureContext(const ContextDesc& desc) override;
  uint32_t JitterPhases() const override { return jitter_phase_count_; }
  void JitterOffset(uint32_t index, float& x, float& y) const override {
    HaltonJitter(index, x, y);
  }
  void RecordDispatch(const Frame& frame) override;

 private:
  bool Load();
  // The flags of a context for HDR or SDR color.
  static uint32_t InitFlags(bool hdr) {
    // XeSS's defaults (no flag set for them): motion vectors at the render
    // size, in pixels, without the jitter; depth nearer smaller; the color
    // linear. LDR_INPUT_COLOR without true HDR: SDR, 0 to 1, no tonemapping
    // inside XeSS.
    return hdr ? XESS_INIT_FLAG_NONE : XESS_INIT_FLAG_LDR_INPUT_COLOR;
  }
  // Fails the backend for good, with the reason and XeSS's result logged.
  bool Fail(const char* what, xess_result_t result);
  static void ExecuteDispatch(void* context,
                              ID3D12GraphicsCommandList* command_list);

  HMODULE module_ = nullptr;
  decltype(&xessGetVersion) get_version_ = nullptr;
  decltype(&xessGetIntelXeFXVersion) get_xefx_version_ = nullptr;
  decltype(&xessD3D12CreateContext) create_context_ = nullptr;
  decltype(&xessDestroyContext) destroy_context_ = nullptr;
  decltype(&xessIsOptimalDriver) is_optimal_driver_ = nullptr;
  decltype(&xessSetLoggingCallback) set_logging_callback_ = nullptr;
  decltype(&xessGetOptimalInputResolution) get_optimal_input_resolution_ =
      nullptr;
  decltype(&xessD3D12Init) init_ = nullptr;
  decltype(&xessD3D12Execute) execute_ = nullptr;
  // Optional (Prepare only).
  decltype(&xessD3D12BuildPipelines) build_pipelines_ = nullptr;
  xess_context_handle_t context_ = nullptr;
  // The flags of the pipelines Prepare started building, until the first
  // xessD3D12Init.
  std::optional<uint32_t> prepared_flags_;
  // xessD3D12Init succeeded for context_desc_.
  bool initialized_ = false;
  ContextDesc context_desc_ = {};
  uint32_t jitter_phase_count_ = 8;
  // What the deferred dispatch needs when the command list is executed.
  xess_d3d12_execute_params_t execute_params_ = {};
};

XessBackend::~XessBackend() {
  if (context_) {
    if (initialized_ && !gpu_done_) {
      upscaler_.AwaitSubmittedFrames();
    }
    destroy_context_(context_);
  }
  if (module_) {
    FreeLibrary(module_);
  }
}

bool XessBackend::Fail(const char* what, xess_result_t result) {
  XELOGE("XeSS: {} failed ({})", what, int32_t(result));
  failed_ = true;
  return false;
}

bool XessBackend::Load() {
  if (context_ || failed_) {
    return !failed_;
  }
  module_ = LoadLibraryW(L"libxess.dll");
  if (module_) {
    auto get = [this](auto& function, const char* name) {
      function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(
          GetProcAddress(module_, name));
      return function != nullptr;
    };
    bool complete = get(get_version_, "xessGetVersion");
    complete &= get(get_xefx_version_, "xessGetIntelXeFXVersion");
    complete &= get(create_context_, "xessD3D12CreateContext");
    complete &= get(destroy_context_, "xessDestroyContext");
    complete &= get(is_optimal_driver_, "xessIsOptimalDriver");
    complete &= get(set_logging_callback_, "xessSetLoggingCallback");
    complete &= get(get_optimal_input_resolution_,
                    "xessGetOptimalInputResolution");
    complete &= get(init_, "xessD3D12Init");
    complete &= get(execute_, "xessD3D12Execute");
    get(build_pipelines_, "xessD3D12BuildPipelines");
    if (!complete) {
      FreeLibrary(module_);
      module_ = nullptr;
    }
  }
  if (!module_) {
    XELOGE("XeSS: libxess.dll is missing or incomplete");
    failed_ = true;
    return false;
  }
  xess_version_t version = {};
  get_version_(&version);
  const xess_result_t result =
      create_context_(upscaler_.device(), &context_);
  if (result != XESS_RESULT_SUCCESS) {
    context_ = nullptr;
    return Fail("creating the context", result);
  }
  set_logging_callback_(context_, XESS_LOGGING_LEVEL_WARNING, XessMessage);
  // 0.0.0 on other GPUs than Intel's.
  xess_version_t xefx_version = {};
  get_xefx_version_(context_, &xefx_version);
  XELOGI("XeSS: version {}.{}.{}, Intel XeFX {}.{}.{}", version.major,
         version.minor, version.patch, xefx_version.major, xefx_version.minor,
         xefx_version.patch);
  if (is_optimal_driver_(context_) == XESS_RESULT_WARNING_OLD_DRIVER) {
    XELOGW("XeSS: the GPU driver is old, a newer one may run XeSS better");
  }
  return true;
}

void XessBackend::Prepare(bool hdr) {
  if (!Load() || !build_pipelines_) {
    return;
  }
  // xessD3D12Init then waits for the build if it's still going.
  const xess_result_t result =
      build_pipelines_(context_, nullptr, false, InitFlags(hdr));
  if (result == XESS_RESULT_SUCCESS) {
    prepared_flags_ = InitFlags(hdr);
  } else {
    XELOGW("XeSS: building the pipelines ahead failed ({})", int32_t(result));
  }
}

bool XessBackend::EnsureContext(const ContextDesc& desc) {
  if (failed_) {
    return false;
  }
  if (initialized_ && context_desc_ == desc) {
    return true;
  }
  if (!Load()) {
    return false;
  }

  // The preset for the upscaling.
  const xess_2d_t output = {desc.display_size[0], desc.display_size[1]};
  xess_quality_settings_t quality = xess_quality_settings_t(0);
  for (xess_quality_settings_t candidate : kQualities) {
    xess_2d_t optimal, smallest, largest;
    if (get_optimal_input_resolution_(context_, &output, candidate, &optimal,
                                      &smallest,
                                      &largest) == XESS_RESULT_SUCCESS &&
        desc.render_size[0] >= smallest.x &&
        desc.render_size[0] <= largest.x &&
        desc.render_size[1] >= smallest.y &&
        desc.render_size[1] <= largest.y) {
      quality = candidate;
      break;
    }
  }
  // (Above 3 times, the most a preset covers, as from 1280x720 to above
  // 3840x2160: FSR instead, for the rest of the session.)
  if (!quality) {
    XELOGE("XeSS: can't upscale {}x{} to {}x{}", desc.render_size[0],
           desc.render_size[1], desc.display_size[0], desc.display_size[1]);
    failed_ = true;
    return false;
  }

  // Re-initializing needs the GPU done with the context.
  if (initialized_) {
    upscaler_.AwaitSubmittedFrames();
    initialized_ = false;
  }
  xess_d3d12_init_params_t init = {};
  init.outputResolution = output;
  init.qualitySetting = quality;
  init.initFlags = InitFlags(desc.hdr);
  const auto start = std::chrono::steady_clock::now();
  // xessD3D12Init must have the flags of a build still going: with others
  // (HDR toggled since Prepare), that build is finished first.
  if (prepared_flags_ && *prepared_flags_ != init.initFlags) {
    build_pipelines_(context_, nullptr, true, *prepared_flags_);
  }
  prepared_flags_.reset();
  const xess_result_t result = init_(context_, &init);
  if (result != XESS_RESULT_SUCCESS) {
    return Fail("initializing", result);
  }
  initialized_ = true;
  context_desc_ = desc;
  jitter_phase_count_ =
      JitterPhaseCount(desc.render_size[0], desc.display_size[0]);
  XELOGI("XeSS: {}x{} -> {}x{} ({}, {}), {} jitter phases, initialized in {} "
         "ms",
         desc.render_size[0], desc.render_size[1], desc.display_size[0],
         desc.display_size[1], QualityName(quality), desc.hdr ? "HDR" : "SDR",
         jitter_phase_count_,
         std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - start)
             .count());
  return true;
}

void XessBackend::RecordDispatch(const Frame& frame) {
  xess_d3d12_execute_params_t& params = execute_params_;
  params = {};
  params.pColorTexture = frame.color;
  params.pVelocityTexture = frame.motion_vectors;
  params.pDepthTexture = frame.depth;
  params.pOutputTexture = frame.output;
  params.jitterOffsetX = frame.jitter[0];
  params.jitterOffsetY = frame.jitter[1];
  // The color is in the game's units (1 = its white), not pre-exposed.
  params.exposureScale = 1.0f;
  params.resetHistory = frame.reset ? 1 : 0;
  params.inputWidth = frame.render_size[0];
  params.inputHeight = frame.render_size[1];
  upscaler_.command_processor().GetDeferredCommandList().DaoCallback(
      ExecuteDispatch, this);
}

void XessBackend::ExecuteDispatch(void* context,
                                  ID3D12GraphicsCommandList* command_list) {
  XessBackend& self = *static_cast<XessBackend*>(context);
  if (!self.initialized_ || self.failed_) {
    return;
  }
  const xess_result_t result =
      self.execute_(self.context_, command_list, &self.execute_params_);
  if (result != XESS_RESULT_SUCCESS) {
    // FSR from the next upscaled frame on (EnsureContext fails).
    self.Fail("upscaling", result);
  }
}

}  // namespace

std::unique_ptr<UpscalerBackend> CreateXessBackend(DaoUpscalerImpl& upscaler) {
  return std::make_unique<XessBackend>(upscaler);
}

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe
