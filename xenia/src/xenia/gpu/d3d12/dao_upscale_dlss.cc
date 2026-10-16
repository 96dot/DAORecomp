// DAO: temporal upscaling's NVIDIA DLSS Super Resolution backend, through
// Streamline (ui/d3d12/dao_streamline.h, started with the device when DLSS is
// the upscaler; its sl.dlss.dll and NVIDIA's nvngx_dlss.dll next to the
// executable) - see dao_upscale.h. DLSS 4.5 (310.9), with the presets its SDK
// recommends for each mode. Experimental: built without an NVIDIA GPU to test
// on.
#include "xenia/gpu/d3d12/dao_upscale_impl.h"

#include <cmath>

#include "xenia/base/logging.h"
#include "xenia/gpu/d3d12/d3d12_command_processor.h"
#include "xenia/ui/d3d12/dao_low_latency.h"
#include "xenia/ui/d3d12/dao_streamline.h"

#include "sl.h"
#include "sl_dlss.h"

namespace xe {
namespace gpu {
namespace d3d12 {

namespace {

namespace dao_streamline = ui::d3d12::dao_streamline;

struct Mode {
  sl::DLSSMode mode;
  const char* name;
  // sl_dlss.h's recommendation: the first generation transformer model (K)
  // up to balanced, DLSS 4.5's second generation ones (M, L) beyond, where
  // they're worth their cost.
  sl::DLSSPreset preset;
  char preset_name;
};

constexpr Mode kModes[] = {
    {sl::DLSSMode::eDLAA, "DLAA", sl::DLSSPreset::ePresetK, 'K'},
    {sl::DLSSMode::eUltraQuality, "ultra quality", sl::DLSSPreset::ePresetK,
     'K'},
    {sl::DLSSMode::eMaxQuality, "quality", sl::DLSSPreset::ePresetK, 'K'},
    {sl::DLSSMode::eBalanced, "balanced", sl::DLSSPreset::ePresetK, 'K'},
    {sl::DLSSMode::eMaxPerformance, "performance", sl::DLSSPreset::ePresetM,
     'M'},
    {sl::DLSSMode::eUltraPerformance, "ultra performance",
     sl::DLSSPreset::ePresetL, 'L'},
};

// The one viewport's number. (A handle is made for each call: Streamline may
// chain the structures it's given.)
constexpr uint32_t kViewport = 0;

// Streamline wants every camera constant; DLSS Super Resolution uses none of
// the matrices, which the game doesn't give: identities.
const sl::float4x4 kIdentity = [] {
  sl::float4x4 matrix;
  for (uint32_t i = 0; i < 4; ++i) {
    matrix.setRow(i, sl::float4(i == 0, i == 1, i == 2, i == 3));
  }
  return matrix;
}();

class DlssBackend : public UpscalerBackend {
 public:
  using UpscalerBackend::UpscalerBackend;
  ~DlssBackend() override;

  const char* name() const override { return "DLSS"; }
  // DLSS takes SDR color as the game encodes it (LDR).
  bool LinearSdrColor() const override { return false; }
  // DLSS doesn't sharpen: the presenter's sharpening applies.
  bool Sharpens() const override { return false; }
  bool EnsureContext(const ContextDesc& desc) override;
  uint32_t JitterPhases() const override { return jitter_phase_count_; }
  void JitterOffset(uint32_t index, float& x, float& y) const override {
    HaltonJitter(index, x, y);
  }
  void RecordDispatch(const Frame& frame) override;

 private:
  bool Load();
  // The mode for desc's upscaling, or nullptr if none takes it.
  const Mode* ChooseMode(const ContextDesc& desc) const;
  // Fails the backend for good, with the reason and Streamline's result
  // logged.
  bool Fail(const char* what, sl::Result result);
  // Releases DLSS's resources, once the GPU is done with them.
  void FreeResources();
  static void ExecuteDispatch(void* context,
                              ID3D12GraphicsCommandList* command_list);

  bool loaded_ = false;
  PFun_slDLSSGetOptimalSettings* get_optimal_settings_ = nullptr;
  PFun_slDLSSSetOptions* set_options_ = nullptr;
  PFun_slSetConstants* set_constants_ = nullptr;
  PFun_slEvaluateFeature* evaluate_feature_ = nullptr;
  PFun_slFreeResources* free_resources_ = nullptr;
  // slDLSSSetOptions succeeded for context_desc_.
  bool initialized_ = false;
  ContextDesc context_desc_ = {};
  uint32_t jitter_phase_count_ = kNativeJitterPhases;
  // What the deferred dispatch needs when the command list is executed.
  Frame frame_ = {};
  uint32_t frame_index_ = 0;
  // The frame index last evaluated: a dispatch left from a swap that wasn't
  // presented runs with the next one's frame, which isn't evaluated twice.
  uint32_t evaluated_index_ = 0;
  // Numbers for upscaled swaps low latency didn't number (no frame started
  // since the last swap); the top bit keeps them apart from its numbers.
  uint32_t own_frame_index_ = 0;
};

DlssBackend::~DlssBackend() { FreeResources(); }

bool DlssBackend::Fail(const char* what, sl::Result result) {
  XELOGE("DLSS: {} failed ({})", what, uint32_t(result));
  failed_ = true;
  return false;
}

void DlssBackend::FreeResources() {
  if (!initialized_) {
    return;
  }
  if (!gpu_done_) {
    upscaler_.AwaitSubmittedFrames();
  }
  initialized_ = false;
  // (Streamline frees them as it stops: after that, they're gone.)
  if (dao_streamline::Active()) {
    free_resources_(sl::kFeatureDLSS, sl::ViewportHandle(kViewport));
  }
}

bool DlssBackend::Load() {
  if (loaded_ || failed_) {
    return !failed_;
  }
  // (DaoUpscalerImpl::CreateBackend chose FSR if it wasn't.)
  if (!dao_streamline::DlssSupported()) {
    XELOGE("DLSS: Streamline doesn't have it");
    failed_ = true;
    return false;
  }
  get_optimal_settings_ = reinterpret_cast<PFun_slDLSSGetOptimalSettings*>(
      dao_streamline::FeatureFunction(sl::kFeatureDLSS,
                                      "slDLSSGetOptimalSettings"));
  set_options_ = reinterpret_cast<PFun_slDLSSSetOptions*>(
      dao_streamline::FeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions"));
  set_constants_ = reinterpret_cast<PFun_slSetConstants*>(
      dao_streamline::CoreFunction("slSetConstants"));
  evaluate_feature_ = reinterpret_cast<PFun_slEvaluateFeature*>(
      dao_streamline::CoreFunction("slEvaluateFeature"));
  free_resources_ = reinterpret_cast<PFun_slFreeResources*>(
      dao_streamline::CoreFunction("slFreeResources"));
  if (!get_optimal_settings_ || !set_options_ || !set_constants_ ||
      !evaluate_feature_ || !free_resources_) {
    XELOGE("DLSS: Streamline's functions aren't there");
    failed_ = true;
    return false;
  }
  loaded_ = true;
  return true;
}

const Mode* DlssBackend::ChooseMode(const ContextDesc& desc) const {
  // The mode whose optimal render size is nearest the render size, of those
  // whose range (the sizes it takes, if it has one) has it: the ranges may
  // overlap.
  sl::DLSSOptions options;
  options.outputWidth = desc.display_size[0];
  options.outputHeight = desc.display_size[1];
  const Mode* chosen = nullptr;
  uint32_t chosen_distance = UINT32_MAX;
  for (const Mode& mode : kModes) {
    options.mode = mode.mode;
    sl::DLSSOptimalSettings settings;
    if (get_optimal_settings_(options, settings) != sl::Result::eOk ||
        !settings.optimalRenderWidth) {
      continue;
    }
    if (settings.renderWidthMax &&
        (desc.render_size[0] < settings.renderWidthMin ||
         desc.render_size[0] > settings.renderWidthMax ||
         desc.render_size[1] < settings.renderHeightMin ||
         desc.render_size[1] > settings.renderHeightMax)) {
      continue;
    }
    const uint32_t distance = uint32_t(std::abs(
        int64_t(settings.optimalRenderWidth) - int64_t(desc.render_size[0])));
    if (distance < chosen_distance) {
      chosen = &mode;
      chosen_distance = distance;
    }
  }
  return chosen;
}

bool DlssBackend::EnsureContext(const ContextDesc& desc) {
  if (failed_) {
    return false;
  }
  if (initialized_ && context_desc_ == desc) {
    return true;
  }
  if (!Load()) {
    return false;
  }
  const Mode* mode = ChooseMode(desc);
  // (Beyond what DLSS's modes take, such as above about 3 times: FSR instead,
  // for the rest of the session.)
  if (!mode) {
    XELOGE("DLSS: can't upscale {}x{} to {}x{}", desc.render_size[0],
           desc.render_size[1], desc.display_size[0], desc.display_size[1]);
    failed_ = true;
    return false;
  }

  // Changing the context needs the GPU done with the old one.
  FreeResources();
  sl::DLSSOptions options;
  options.mode = mode->mode;
  options.outputWidth = desc.display_size[0];
  options.outputHeight = desc.display_size[1];
  options.dlaaPreset = options.ultraQualityPreset = options.qualityPreset =
      options.balancedPreset = options.performancePreset =
          options.ultraPerformancePreset = mode->preset;
  options.colorBuffersHDR = desc.hdr ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  // The color is in the game's units (1 = its white), not pre-exposed; no
  // exposure texture: DLSS's own.
  options.preExposure = 1.0f;
  options.useAutoExposure = sl::Boolean::eTrue;
  const sl::Result result =
      set_options_(sl::ViewportHandle(kViewport), options);
  if (result != sl::Result::eOk) {
    return Fail("setting the options", result);
  }
  initialized_ = true;
  context_desc_ = desc;
  jitter_phase_count_ =
      JitterPhaseCount(desc.render_size[0], desc.display_size[0]);
  XELOGI("DLSS: {}x{} -> {}x{} ({}, preset {}, {}), {} jitter phases",
         desc.render_size[0], desc.render_size[1], desc.display_size[0],
         desc.display_size[1], mode->name, mode->preset_name,
         desc.hdr ? "HDR" : "SDR", jitter_phase_count_);
  return true;
}

void DlssBackend::RecordDispatch(const Frame& frame) {
  frame_ = frame;
  // The frame Streamline knows this swap as (Reflex's), or one of this
  // backend's own.
  frame_index_ = ui::d3d12::dao_low_latency::IssuedFrame();
  if (!frame_index_) {
    frame_index_ = 0x80000000u | ++own_frame_index_;
  }
  upscaler_.command_processor().GetDeferredCommandList().DaoCallback(
      ExecuteDispatch, this);
}

void DlssBackend::ExecuteDispatch(void* context,
                                  ID3D12GraphicsCommandList* command_list) {
  DlssBackend& self = *static_cast<DlssBackend*>(context);
  if (!self.initialized_ || self.failed_ ||
      self.frame_index_ == self.evaluated_index_) {
    return;
  }
  self.evaluated_index_ = self.frame_index_;
  const Frame& frame = self.frame_;
  // Low latency's token of the frame, not another one of the same frame
  // (Streamline keeps only so many), or a token of this backend's own.
  auto* token = static_cast<sl::FrameToken*>(
      ui::d3d12::dao_low_latency::FrameToken(self.frame_index_));
  if (!token) {
    token = static_cast<sl::FrameToken*>(
        dao_streamline::FrameToken(self.frame_index_));
  }
  if (!token) {
    self.Fail("getting the frame's token", sl::Result::eErrorInvalidState);
    return;
  }

  sl::Constants constants;
  // No camera: identities and placeholder values (see kIdentity).
  constants.cameraViewToClip = kIdentity;
  constants.clipToCameraView = kIdentity;
  constants.clipToLensClip = kIdentity;
  constants.clipToPrevClip = kIdentity;
  constants.prevClipToClip = kIdentity;
  constants.cameraPinholeOffset = {0.0f, 0.0f};
  constants.cameraPos = {0.0f, 0.0f, 0.0f};
  constants.cameraUp = {0.0f, 1.0f, 0.0f};
  constants.cameraRight = {1.0f, 0.0f, 0.0f};
  constants.cameraFwd = {0.0f, 0.0f, 1.0f};
  constants.cameraNear = 0.1f;
  constants.cameraFar = 1000.0f;
  constants.cameraFOV = 1.0f;
  constants.cameraAspectRatio =
      float(frame.render_size[0]) / float(frame.render_size[1]);
  constants.motionVectorsInvalidValue = 0.0f;
  // In render pixels, as FSR and XeSS take it (DLSS's convention is theirs).
  constants.jitterOffset = {frame.jitter[0], frame.jitter[1]};
  // The motion vectors are in render pixels; Streamline takes them as
  // fractions of the image.
  constants.mvecScale = {1.0f / float(frame.render_size[0]),
                         1.0f / float(frame.render_size[1])};
  constants.depthInverted = sl::Boolean::eFalse;
  constants.cameraMotionIncluded = sl::Boolean::eTrue;
  constants.motionVectors3D = sl::Boolean::eFalse;
  constants.reset = frame.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  sl::ViewportHandle viewport(kViewport);
  sl::Result result = self.set_constants_(constants, *token, viewport);
  if (result != sl::Result::eOk) {
    self.Fail("setting the frame's constants", result);
    return;
  }

  // The resources for this evaluation only, in the states they're in.
  const sl::Extent render_extent = {0, 0, frame.render_size[0],
                                    frame.render_size[1]};
  const sl::Extent display_extent = {0, 0, frame.display_size[0],
                                     frame.display_size[1]};
  constexpr uint32_t kInputState =
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  sl::Resource color(sl::ResourceType::eTex2d, frame.color, kInputState);
  sl::Resource depth(sl::ResourceType::eTex2d, frame.depth, kInputState);
  sl::Resource motion_vectors(sl::ResourceType::eTex2d, frame.motion_vectors,
                              kInputState);
  sl::Resource output(sl::ResourceType::eTex2d, frame.output,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  sl::ResourceTag tags[] = {
      {&color, sl::kBufferTypeScalingInputColor,
       sl::ResourceLifecycle::eValidUntilEvaluate, &render_extent},
      {&depth, sl::kBufferTypeDepth,
       sl::ResourceLifecycle::eValidUntilEvaluate, &render_extent},
      {&motion_vectors, sl::kBufferTypeMotionVectors,
       sl::ResourceLifecycle::eValidUntilEvaluate, &render_extent},
      {&output, sl::kBufferTypeScalingOutputColor,
       sl::ResourceLifecycle::eValidUntilEvaluate, &display_extent},
  };
  const sl::BaseStructure* inputs[] = {&viewport, &tags[0], &tags[1],
                                       &tags[2], &tags[3]};
  result = self.evaluate_feature_(sl::kFeatureDLSS, *token, inputs,
                                  uint32_t(std::size(inputs)), command_list);
  if (result != sl::Result::eOk) {
    // FSR from the next upscaled frame on (EnsureContext fails).
    self.Fail("upscaling", result);
  }
}

}  // namespace

std::unique_ptr<UpscalerBackend> CreateDlssBackend(DaoUpscalerImpl& upscaler) {
  return std::make_unique<DlssBackend>(upscaler);
}

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe
