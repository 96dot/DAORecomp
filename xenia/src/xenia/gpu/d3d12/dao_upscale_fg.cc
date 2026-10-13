// DAO: frame generation with AMD FSR 3.1 through the FidelityFX API
// (amd_fidelityfx_framegeneration_dx12.dll next to the executable, loaded on
// demand) - see dao_upscale.h. It runs on the upscaled scene before the UI, at
// the swap, without the FidelityFX swap chain (its pacing threads and UI
// handling assume a game that owns its swap chain): the UI is put over the
// generated frame as over the real one, so it's never warped, and the presenter
// shows the generated frame, then the real one half a frame later
// (ui/dao_frame_pacer.cc), or only real ones if the display can't keep up.
#include "xenia/gpu/d3d12/dao_upscale_impl.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "xenia/base/logging.h"
#include "xenia/gpu/d3d12/d3d12_command_processor.h"

#include "dx12/ffx_api_dx12.h"

DEFINE_bool(dao_frame_generation, false,
            "With temporal upscaling, a frame generated (AMD FSR 3.1) between "
            "each two of the 3D scenes, shown half a frame apart: up to twice "
            "the frames (only real ones while the display can't show that "
            "many), half a frame more latency.",
            "GPU");

namespace xe {
namespace gpu {
namespace d3d12 {

bool DaoUpscalerImpl::FrameGenerationWanted() const {
  // Bindless only: the composites after it reuse descriptors requested before
  // its heaps were bound, which bindful heaps would need again.
  return !fg_failed_ && fg_enabled_ && cp_.bindless_resources_used_;
}

void DaoUpscalerImpl::OnFrameGenerationToggled(bool enabled) {
  if (enabled) {
    if (ui::Presenter* presenter = cp_.graphics_system_->presenter()) {
      presenter->DaoResetFramePacing();
    }
    return;
  }
  ReleaseFrameGeneration();
}

void DaoUpscalerImpl::ReleaseFrameGeneration() {
  DestroyFrameGeneration();
  ReleaseTexture(fg_scene_);
  ReleaseTexture(fg_generated_);
  ReleaseTexture(fg_composite_);
}

bool DaoUpscalerImpl::EnsureFrameGeneration(uint32_t render_width,
                                            uint32_t render_height,
                                            uint32_t display_width,
                                            uint32_t display_height) {
  if (fg_context_ && fg_context_size_[0] == render_width &&
      fg_context_size_[1] == render_height &&
      fg_context_size_[2] == display_width &&
      fg_context_size_[3] == display_height && fg_context_hdr_ == true_hdr_) {
    return true;
  }
  if (fg_failed_ || !ffx_.Load()) {
    return false;
  }
  if (!ffx_.configure) {
    XELOGE(
        "FSR: amd_fidelityfx_loader_dx12.dll has no ffxConfigure, frame "
        "generation is off");
    fg_failed_ = true;
    return false;
  }
  DestroyFrameGeneration();

  // FSR 3.1's frame generation, the version named 3.x: the machine learning
  // one (4.x) also needs the camera's position and orientation, which the
  // game's draws don't give.
  ID3D12Device* device = cp_.GetD3D12Provider().GetDevice();
  ffxQueryDescGetVersions versions = {};
  versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
  versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
  versions.device = device;
  uint64_t count = 0;
  versions.outputCount = &count;
  ffx_.query(nullptr, &versions.header);
  std::vector<uint64_t> ids(count);
  std::vector<const char*> names(count);
  versions.versionIds = ids.data();
  versions.versionNames = names.data();
  ffx_.query(nullptr, &versions.header);
  uint64_t version_id = 0;
  for (uint64_t i = 0; i < count; ++i) {
    if (names[i] && std::strncmp(names[i], "3.", 2) == 0) {
      version_id = ids[i];
      break;
    }
  }
  if (!version_id) {
    XELOGE(
        "FSR: no FSR 3.1 frame generation (amd_fidelityfx_framegeneration_"
        "dx12.dll missing?), frame generation is off");
    fg_failed_ = true;
    return false;
  }

  ffxCreateContextDescFrameGeneration desc = {};
  desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
  // The scene as the upscaler outputs it: gamma-encoded or linear SDR, or
  // linear HDR.
  desc.flags = true_hdr_ ? FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE : 0;
  desc.displaySize = {display_width, display_height};
  desc.maxRenderSize = {render_width, render_height};
  desc.backBufferFormat = FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
  ffxCreateContextDescFrameGenerationVersion version = {};
  version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
  version.version = FFX_FRAMEGENERATION_VERSION;
  ffxCreateBackendDX12Desc backend = {};
  backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
  backend.device = device;
  ffxOverrideVersion override_version = {};
  override_version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
  override_version.versionId = version_id;
  desc.header.pNext = &version.header;
  version.header.pNext = &backend.header;
  backend.header.pNext = &override_version.header;
  const ffxReturnCode_t result = ffx_.create(&fg_context_, &desc.header, nullptr);
  if (result != FFX_API_RETURN_OK) {
    XELOGE("FSR: creating frame generation failed ({}), it's off", result);
    fg_context_ = nullptr;
    fg_failed_ = true;
    return false;
  }
  fg_context_size_[0] = render_width;
  fg_context_size_[1] = render_height;
  fg_context_size_[2] = display_width;
  fg_context_size_[3] = display_height;
  fg_context_hdr_ = true_hdr_;

  ffxQueryGetProviderVersion provider = {};
  provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
  ffx_.query(&fg_context_, &provider.header);
  XELOGI("FSR: frame generation {} - {}x{}{}",
         provider.versionName ? provider.versionName : "?", display_width,
         display_height, true_hdr_ ? ", HDR" : "");
  return true;
}

void DaoUpscalerImpl::DestroyFrameGeneration() {
  if (!fg_context_) {
    return;
  }
  // The submitted frames may still use its resources: kept until the GPU is
  // done with them (a stall here would be a hitch). One retired before is
  // waited for (only if changed again within a frame or two).
  DestroyRetiredFrameGeneration(true);
  fg_retired_context_ = fg_context_;
  fg_retired_submission_ = cp_.GetCurrentSubmission();
  fg_context_ = nullptr;
  fg_previous_ = false;
}

void DaoUpscalerImpl::DestroyRetiredFrameGeneration(bool wait) {
  if (!fg_retired_context_) {
    return;
  }
  if (cp_.GetCompletedSubmission() < fg_retired_submission_) {
    if (!wait) {
      return;
    }
    // (Retired in an earlier submission: this one's isn't submitted yet.)
    cp_.CheckSubmissionCompletion(
        std::min(fg_retired_submission_, cp_.GetCurrentSubmission() - 1));
  }
  ffx_.destroy(&fg_retired_context_, nullptr);
  fg_retired_context_ = nullptr;
}

bool DaoUpscalerImpl::RecordFrameGeneration(const Texture& scene,
                                            uint32_t render_width,
                                            uint32_t render_height,
                                            float frame_ms,
                                            bool previous_given) {
  // Without the previous frame (or across a cut) there's nothing to generate
  // from: it's only taken as the next one's previous.
  const bool reset = reset_history_ || !previous_given;
  ++fg_frame_id_;
  FrameGenerationParams& params = fg_params_;
  params = {};
  params.self = this;

  // Configured for each frame, without a swap chain to notify.
  ffxConfigureDescFrameGeneration& configure = params.configure;
  configure.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
  configure.frameGenerationEnabled = true;
  configure.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
  configure.frameID = fg_frame_id_;

  // The inputs as the upscaler got them (see OnSwap).
  ffxDispatchDescFrameGenerationPrepareV2& prepare = params.prepare;
  prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
  prepare.frameID = fg_frame_id_;
  prepare.renderSize = {render_width, render_height};
  prepare.jitterOffset = {jitter_[0], jitter_[1]};
  // As the upscaler's (OnSwap): motion vectors in pixels, the frame time
  // clamped (a pause or a hitch isn't one), and rough camera values for FSR's
  // depth-based heuristics.
  prepare.motionVectorScale = {1.0f, 1.0f};
  prepare.frameTimeDelta = std::clamp(frame_ms, 1.0f, 100.0f);
  prepare.reset = reset;
  prepare.cameraNear = 0.1f;
  prepare.cameraFar = 1000.0f;
  prepare.cameraFovAngleVertical = 1.0f;
  prepare.viewSpaceToMetersFactor = 1.0f;
  prepare.depth = ffxApiGetResourceDX12(depth_.resource.Get());
  prepare.motionVectors = ffxApiGetResourceDX12(motion_vectors_.resource.Get());

  ffxDispatchDescFrameGeneration& dispatch = params.dispatch;
  dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
  dispatch.presentColor = ffxApiGetResourceDX12(scene.resource.Get());
  dispatch.outputs[0] = ffxApiGetResourceDX12(
      fg_generated_.resource.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
  dispatch.numGeneratedFrames = 1;
  dispatch.reset = reset;
  dispatch.frameID = fg_frame_id_;
  if (true_hdr_) {
    // The scene is linear with 1 = paper white, taken as scRGB (where 1 is
    // 80 nits): the peak is then 80 * peak / paper white.
    dispatch.backbufferTransferFunction =
        FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SCRGB;
    dispatch.minMaxLuminance[0] = 0.0f;
    dispatch.minMaxLuminance[1] =
        80.0f * dao_live::HdrPeakRatio(PaperWhiteNits());
  } else if (backend_->LinearSdrColor()) {
    // SDR linearized for the upscaler (XeSS): scRGB up to SDR white.
    dispatch.backbufferTransferFunction =
        FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SCRGB;
    dispatch.minMaxLuminance[0] = 0.0f;
    dispatch.minMaxLuminance[1] = 80.0f;
  } else {
    dispatch.backbufferTransferFunction =
        FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
  }

  cp_.GetDeferredCommandList().DaoCallback(ExecuteFrameGeneration, &params);
  InvalidateCommandProcessorState();
  fg_previous_ = true;
  return !reset;
}

void DaoUpscalerImpl::ExecuteFrameGeneration(
    void* context, ID3D12GraphicsCommandList* command_list) {
  auto& params = *static_cast<FrameGenerationParams*>(context);
  DaoUpscalerImpl& self = *params.self;
  if (!self.fg_context_) {
    return;
  }
  // Configured, then the dispatches, all with the same frame ID.
  static std::atomic<uint32_t> s_failures{0};
  ffxReturnCode_t result =
      self.ffx_.configure(&self.fg_context_, &params.configure.header);
  if (result == FFX_API_RETURN_OK) {
    params.prepare.commandList = command_list;
    result = self.ffx_.dispatch(&self.fg_context_, &params.prepare.header);
  }
  if (result == FFX_API_RETURN_OK) {
    params.dispatch.commandList = command_list;
    result = self.ffx_.dispatch(&self.fg_context_, &params.dispatch.header);
  }
  if (result != FFX_API_RETURN_OK) {
    // This frame's generated one is shown unwritten; none from now on.
    // (Executed by the GPU thread, which reads fg_failed_.)
    self.fg_failed_ = true;
    if (LogNth(s_failures)) {
      XELOGE("FSR: frame generation failed ({}), it's off", result);
    }
  }
}

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe
