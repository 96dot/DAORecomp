// DAO: temporal upscaling with AMD FSR 3.1 / FSR 4 - see dao_upscale.h. This
// file: the frame (draws, resolves, the swap, the composite).
#include "xenia/gpu/d3d12/dao_upscale_impl.h"

#include <algorithm>
#include <bit>

#include "xenia/base/logging.h"
#include "xenia/dao_live_settings.h"
#include "xenia/gpu/d3d12/d3d12_command_processor.h"
#include "xenia/gpu/d3d12/dao_gpu_timing.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/ui/d3d12/d3d12_presenter.h"

DEFINE_string(dao_temporal_upscaler, "off",
              "Temporal upscaling and anti-aliasing of 3D scenes: off, auto "
              "(FSR 4 where supported, otherwise FSR 3.1), fsr4, fsr3.",
              "GPU");
DEFINE_double(dao_temporal_sharpness, 0.3,
              "Sharpening after temporal upscaling, 0 (off) to 1.", "GPU");
DEFINE_bool(dao_single_sample_scene, true,
            "With temporal upscaling, render 3D scenes in one tile without "
            "2xMSAA (the upscaler antialiases them).",
            "GPU");
DEFINE_bool(dao_motion_vectors, true,
            "Motion vectors for temporal upscaling (false: zero motion, for "
            "comparison).",
            "GPU");
DEFINE_bool(dao_true_hdr, true,
            "With HDR output on an HDR display, the game's 3D scenes in HDR: "
            "its tonemapping redone with the highlights it compresses (and "
            "upscaled by FSR in HDR with temporal upscaling). Otherwise the "
            "SDR image is expanded by the presenter.",
            "GPU");
DEFINE_int32(dao_motion_blur, 0,
             "Motion blur of 3D scenes with temporal upscaling, from its "
             "motion vectors (camera, objects, characters): 0 off, 1 low (the "
             "shutter open a quarter of the frame time, 8 samples per pixel), "
             "2 high (half of it, 12 samples). Costs GPU time where the scene "
             "moves.",
             "GPU");
DEFINE_bool(dao_show_motion_vectors, false,
            "Shows the motion vectors instead of the game (red: horizontal, "
            "green: vertical motion, gray: none), for debugging.",
            "GPU");

namespace xe {
namespace gpu {
namespace d3d12 {

namespace shaders {
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/dao_hdr_tonemap_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/dao_motion_blur_neighbor_max_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/dao_motion_blur_tile_max_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/dao_upscale_composite_blur_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/dao_upscale_composite_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/dao_upscale_copy_color_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/dao_upscale_copy_depth_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/dao_upscale_copy_depth_ms_cs.h"
}  // namespace shaders

namespace {

// The depth difference from the scene within which a motion vector pass pixel
// is the visible surface (kDaoMvDepthTolerance in dao_upscale_mv.hlsli).
constexpr float kMotionDepthTolerance = 2e-5f;
// How far the motion vector pass's depth test moves the triangles towards the
// camera: the tolerance, with a margin for the rounding of the depth tested
// (it's interpolated differently from the pixel shader's).
constexpr float kMotionDepthTestOffset = kMotionDepthTolerance * 1.25f;

}  // namespace

DaoUpscalerImpl* DaoUpscaler::impl_ = nullptr;
std::atomic<bool> DaoUpscaler::single_sample_scene_{false};

DaoUpscalerImpl::~DaoUpscalerImpl() {
  if (stream_out_thread_.joinable()) {
    {
      std::lock_guard<std::mutex> lock(stream_out_mutex_);
      stream_out_shutdown_ = true;
    }
    stream_out_cv_.notify_one();
    stream_out_thread_.join();
  }
  DestroyContext();
  if (ffx_module_) {
    FreeLibrary(ffx_module_);
  }
}

bool DaoUpscalerImpl::Initialize() {
  const ui::d3d12::D3D12Provider& provider = cp_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  // Copy: constants, source SRV, destination UAV.
  D3D12_DESCRIPTOR_RANGE copy_ranges[2] = {};
  copy_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  copy_ranges[0].NumDescriptors = 1;
  copy_ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  copy_ranges[1].NumDescriptors = 1;
  D3D12_ROOT_PARAMETER copy_parameters[3] = {};
  copy_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  copy_parameters[0].Constants.Num32BitValues =
      sizeof(CopyConstants) / sizeof(uint32_t);
  for (uint32_t i = 0; i < 2; ++i) {
    copy_parameters[1 + i].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    copy_parameters[1 + i].DescriptorTable.NumDescriptorRanges = 1;
    copy_parameters[1 + i].DescriptorTable.pDescriptorRanges = &copy_ranges[i];
  }
  D3D12_ROOT_SIGNATURE_DESC copy_desc = {};
  copy_desc.NumParameters = UINT(std::size(copy_parameters));
  copy_desc.pParameters = copy_parameters;
  copy_root_signature_.Attach(
      ui::d3d12::util::CreateRootSignature(provider, copy_desc));

  // Motion blur's tile passes: constants, t0 to t2, u0 to u3.
  D3D12_DESCRIPTOR_RANGE blur_ranges[kBlurTables] = {};
  for (uint32_t i = 0; i < kBlurTables; ++i) {
    blur_ranges[i].RangeType = i < 3 ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV
                                     : D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    blur_ranges[i].NumDescriptors = 1;
    blur_ranges[i].BaseShaderRegister = i < 3 ? i : i - 3;
  }
  D3D12_ROOT_PARAMETER blur_parameters[1 + kBlurTables] = {};
  blur_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  blur_parameters[0].Constants.Num32BitValues =
      sizeof(BlurConstants) / sizeof(uint32_t);
  for (uint32_t i = 0; i < kBlurTables; ++i) {
    blur_parameters[1 + i].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    blur_parameters[1 + i].DescriptorTable.NumDescriptorRanges = 1;
    blur_parameters[1 + i].DescriptorTable.pDescriptorRanges = &blur_ranges[i];
  }
  D3D12_ROOT_SIGNATURE_DESC blur_desc = {};
  blur_desc.NumParameters = UINT(std::size(blur_parameters));
  blur_desc.pParameters = blur_parameters;
  blur_root_signature_.Attach(
      ui::d3d12::util::CreateRootSignature(provider, blur_desc));

  // Composite: constants, 6 SRVs (separate tables, the one-use descriptors
  // aren't contiguous), destination UAV, linear sampler.
  D3D12_DESCRIPTOR_RANGE composite_ranges[kCompositeTables] = {};
  for (uint32_t i = 0; i < kCompositeTables - 1; ++i) {
    composite_ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    composite_ranges[i].NumDescriptors = 1;
    composite_ranges[i].BaseShaderRegister = i;
  }
  composite_ranges[kCompositeTables - 1].RangeType =
      D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  composite_ranges[kCompositeTables - 1].NumDescriptors = 1;
  D3D12_ROOT_PARAMETER composite_parameters[1 + kCompositeTables] = {};
  composite_parameters[0].ParameterType =
      D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  composite_parameters[0].Constants.Num32BitValues =
      sizeof(CompositeConstants) / sizeof(uint32_t);
  for (uint32_t i = 0; i < kCompositeTables; ++i) {
    composite_parameters[1 + i].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    composite_parameters[1 + i].DescriptorTable.NumDescriptorRanges = 1;
    composite_parameters[1 + i].DescriptorTable.pDescriptorRanges =
        &composite_ranges[i];
  }
  D3D12_STATIC_SAMPLER_DESC linear_sampler = {};
  linear_sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  linear_sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  linear_sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  linear_sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  linear_sampler.MaxLOD = D3D12_FLOAT32_MAX;
  linear_sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC composite_desc = {};
  composite_desc.NumParameters = UINT(std::size(composite_parameters));
  composite_desc.pParameters = composite_parameters;
  composite_desc.NumStaticSamplers = 1;
  composite_desc.pStaticSamplers = &linear_sampler;
  composite_root_signature_.Attach(
      ui::d3d12::util::CreateRootSignature(provider, composite_desc));
  // HDR tonemapping: constants, 3 SRVs (the game's textures, wherever they
  // are in the heap), destination UAV, linear sampler.
  D3D12_DESCRIPTOR_RANGE tonemap_ranges[4] = {};
  for (uint32_t i = 0; i < 3; ++i) {
    tonemap_ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    tonemap_ranges[i].NumDescriptors = 1;
    tonemap_ranges[i].BaseShaderRegister = i;
  }
  tonemap_ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  tonemap_ranges[3].NumDescriptors = 1;
  D3D12_ROOT_PARAMETER tonemap_parameters[5] = {};
  tonemap_parameters[0].ParameterType =
      D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  tonemap_parameters[0].Constants.Num32BitValues =
      sizeof(HdrTonemapConstants) / sizeof(uint32_t);
  for (uint32_t i = 0; i < 4; ++i) {
    tonemap_parameters[1 + i].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    tonemap_parameters[1 + i].DescriptorTable.NumDescriptorRanges = 1;
    tonemap_parameters[1 + i].DescriptorTable.pDescriptorRanges =
        &tonemap_ranges[i];
  }
  D3D12_ROOT_SIGNATURE_DESC tonemap_desc = {};
  tonemap_desc.NumParameters = UINT(std::size(tonemap_parameters));
  tonemap_desc.pParameters = tonemap_parameters;
  tonemap_desc.NumStaticSamplers = 1;
  tonemap_desc.pStaticSamplers = &linear_sampler;
  hdr_tonemap_root_signature_.Attach(
      ui::d3d12::util::CreateRootSignature(provider, tonemap_desc));
  if (!copy_root_signature_ || !composite_root_signature_ ||
      !blur_root_signature_ || !hdr_tonemap_root_signature_) {
    XELOGE("FSR: failed to create the root signatures");
    return false;
  }

  copy_depth_ms_pipeline_.Attach(ui::d3d12::util::CreateComputePipeline(
      device, shaders::dao_upscale_copy_depth_ms_cs,
      sizeof(shaders::dao_upscale_copy_depth_ms_cs),
      copy_root_signature_.Get()));
  copy_depth_pipeline_.Attach(ui::d3d12::util::CreateComputePipeline(
      device, shaders::dao_upscale_copy_depth_cs,
      sizeof(shaders::dao_upscale_copy_depth_cs), copy_root_signature_.Get()));
  copy_color_pipeline_.Attach(ui::d3d12::util::CreateComputePipeline(
      device, shaders::dao_upscale_copy_color_cs,
      sizeof(shaders::dao_upscale_copy_color_cs), copy_root_signature_.Get()));
  composite_pipeline_.Attach(ui::d3d12::util::CreateComputePipeline(
      device, shaders::dao_upscale_composite_cs,
      sizeof(shaders::dao_upscale_composite_cs),
      composite_root_signature_.Get()));
  hdr_tonemap_pipeline_.Attach(ui::d3d12::util::CreateComputePipeline(
      device, shaders::dao_hdr_tonemap_cs,
      sizeof(shaders::dao_hdr_tonemap_cs), hdr_tonemap_root_signature_.Get()));
  composite_blur_pipeline_.Attach(ui::d3d12::util::CreateComputePipeline(
      device, shaders::dao_upscale_composite_blur_cs,
      sizeof(shaders::dao_upscale_composite_blur_cs),
      composite_root_signature_.Get()));
  blur_tile_max_pipeline_.Attach(ui::d3d12::util::CreateComputePipeline(
      device, shaders::dao_motion_blur_tile_max_cs,
      sizeof(shaders::dao_motion_blur_tile_max_cs),
      blur_root_signature_.Get()));
  blur_neighbor_max_pipeline_.Attach(ui::d3d12::util::CreateComputePipeline(
      device, shaders::dao_motion_blur_neighbor_max_cs,
      sizeof(shaders::dao_motion_blur_neighbor_max_cs),
      blur_root_signature_.Get()));
  if (!copy_depth_ms_pipeline_ || !copy_depth_pipeline_ ||
      !copy_color_pipeline_ || !composite_pipeline_ ||
      !hdr_tonemap_pipeline_ || !composite_blur_pipeline_ ||
      !blur_tile_max_pipeline_ || !blur_neighbor_max_pipeline_) {
    XELOGE("FSR: failed to create the pipelines");
    return false;
  }
  motion_available_ = InitializeMotion();
  if (!motion_available_) {
    XELOGW("FSR: motion vectors unavailable");
  }
  jitter_active_ = cvars::dao_temporal_upscaler != "off";
  true_hdr_ = WantsTrueHdr();
  single_sample_scene_ = jitter_active_ && cvars::dao_single_sample_scene;
  last_swap_ = std::chrono::steady_clock::now();
  return true;
}

bool DaoUpscalerImpl::EnsureTexture(Texture& texture, DXGI_FORMAT format,
                                    uint32_t width, uint32_t height,
                                    const char* name,
                                    D3D12_RESOURCE_FLAGS extra_flags) {
  if (texture.resource && texture.format == format && texture.width == width &&
      texture.height == height) {
    return true;
  }
  if (texture.resource) {
    // Release once the GPU is done with it.
    texture.resource->AddRef();
    cp_.resources_for_deletion_.emplace_back(cp_.GetCurrentSubmission(),
                                             texture.resource.Get());
    texture.resource.Reset();
  }
  const ui::d3d12::D3D12Provider& provider = cp_.GetD3D12Provider();
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  // Depth buffers can't have unordered access.
  const bool depth =
      (extra_flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
  desc.Flags =
      depth ? extra_flags
            : D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | extra_flags;
  const D3D12_RESOURCE_STATES state =
      depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE
            : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  // Zeroed (motion vectors are zero until something writes them).
  if (FAILED(provider.GetDevice()->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE,
          &desc, state, nullptr, IID_PPV_ARGS(&texture.resource)))) {
    XELOGE("FSR: failed to create the {} texture ({}x{})", name, width,
           height);
    texture = {};
    return false;
  }
  texture.state = state;
  texture.format = format;
  texture.width = width;
  texture.height = height;
  return true;
}

void DaoUpscalerImpl::Transition(Texture& texture,
                                 D3D12_RESOURCE_STATES state) {
  cp_.PushTransitionBarrier(texture.resource.Get(), texture.state, state);
  texture.state = state;
}

void DaoUpscalerImpl::CopyFromRenderTarget(
    ID3D12Resource* source, const D3D12_SHADER_RESOURCE_VIEW_DESC& source_srv,
    D3D12RenderTargetCache::D3D12RenderTarget& target,
    D3D12_RESOURCE_STATES read_state, Texture& dest,
    ID3D12PipelineState* pipeline, const CopyConstants& constants) {
  if (!constants.size[0] || !constants.size[1]) {
    return;
  }
  ID3D12Device* device = cp_.GetD3D12Provider().GetDevice();
  ui::d3d12::util::DescriptorCpuGpuHandlePair descriptors[2];
  if (!cp_.RequestOneUseSingleViewDescriptors(2, descriptors)) {
    return;
  }
  device->CreateShaderResourceView(source, &source_srv, descriptors[0].first);
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
  uav.Format = dest.format;
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  device->CreateUnorderedAccessView(dest.resource.Get(), nullptr, &uav,
                                    descriptors[1].first);

  // The render target goes back to the state the render target cache expects.
  D3D12_RESOURCE_STATES target_state = target.SetResourceState(read_state);
  cp_.PushTransitionBarrier(source, target_state, read_state);
  Transition(dest, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  cp_.SubmitBarriers();

  DeferredCommandList& command_list = cp_.GetDeferredCommandList();
  command_list.D3DSetComputeRootSignature(copy_root_signature_.Get());
  command_list.D3DSetComputeRoot32BitConstants(
      0, sizeof(constants) / sizeof(uint32_t), &constants, 0);
  command_list.D3DSetComputeRootDescriptorTable(1, descriptors[0].second);
  command_list.D3DSetComputeRootDescriptorTable(2, descriptors[1].second);
  cp_.SetExternalPipeline(pipeline);
  command_list.D3DDispatch((constants.size[0] + 7) / 8,
                           (constants.size[1] + 7) / 8, 1);

  cp_.PushTransitionBarrier(source, read_state, target_state);
  target.SetResourceState(target_state);
  // The next copy into the same texture must wait for this one.
  cp_.PushUAVBarrier(dest.resource.Get());
}

void DaoUpscalerImpl::OnDraw(const RegisterFile& regs,
                             draw_util::ViewportInfo& viewport_info,
                             reg::RB_DEPTHCONTROL depth_control,
                             uint32_t color_mask, uint64_t pixel_shader_hash,
                             uint32_t pixel_shader_texture_mask,
                             bool window_offset_in_edram) {
  hudless_pending_ = false;
  hdr_pending_ = false;
  motion_pending_ = false;
  // Without temporal upscaling, true HDR needs only the image before the UI.
  if (!jitter_active_ && !true_hdr_) {
    return;
  }
  const auto surface = regs.Get<reg::RB_SURFACE_INFO>();
  // The scene: depth-tested multisampled draws (the depth prepass and both
  // tiles of the main pass; shadow maps and the UI are single-sampled). Single-
  // sampled, the depth-tested draws as wide as the screen (not the shadow
  // maps) until the scene's color is resolved (after it, effects drawn over
  // it).
  const bool single_sample_scene_draw =
      single_sample_scene_ && !scene_texture_ &&
      surface.msaa_samples == xenos::MsaaSamples::k1X &&
      surface.surface_pitch * rtc_.draw_resolution_scale_x() ==
          render_size_[0];
  if (jitter_active_ && depth_control.z_enable &&
      (surface.msaa_samples != xenos::MsaaSamples::k1X ||
       single_sample_scene_draw)) {
    ++scene_draws_;
    single_sample_scene_drawn_ |= single_sample_scene_draw;
    // Where the draw is on the screen: its render target coordinates minus
    // the window offset of the tile, unless that's in the EDRAM base.
    int32_t window_offset[2] = {};
    if (!window_offset_in_edram &&
        regs.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
      const auto pa_sc_window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
      window_offset[0] = pa_sc_window_offset.window_x_offset;
      window_offset[1] = pa_sc_window_offset.window_y_offset;
    }
    auto* depth_target = static_cast<D3D12RenderTargetCache::D3D12RenderTarget*>(
        rtc_.last_update_accumulated_render_targets()[0]);
    if (depth_target && depth_target->key().is_depth) {
      scene_depth_target_ = depth_target;
      scene_depth_window_offset_[0] = window_offset[0];
      scene_depth_window_offset_[1] = window_offset[1];
      scene_depth_pending_ = true;
    }
    if (!viewport_info.xy_extent[0] || !viewport_info.xy_extent[1]) {
      return;
    }
    const float ndc_jitter[2] = {
        2.0f * jitter_[0] / float(viewport_info.xy_extent[0]),
        -2.0f * jitter_[1] / float(viewport_info.xy_extent[1])};
    viewport_info.ndc_offset[0] += ndc_jitter[0];
    viewport_info.ndc_offset[1] += ndc_jitter[1];
    // Motion vectors for the draws writing color (not the depth prepass).
    if (motion_available_ && cvars::dao_motion_vectors && (color_mask & 0xF) &&
        render_size_[0]) {
      motion_pending_ = true;
      motion_pixel_shader_ = pixel_shader_hash;
      const int32_t scale[2] = {int32_t(rtc_.draw_resolution_scale_x()),
                                int32_t(rtc_.draw_resolution_scale_y())};
      MotionConstants& motion = motion_viewport_;
      for (uint32_t i = 0; i < 2; ++i) {
        motion.viewport_offset[i] =
            float(viewport_info.xy_offset[i] - window_offset[i] * scale[i]);
        motion.viewport_extent[i] = float(viewport_info.xy_extent[i]);
        motion.render_size[i] = float(render_size_[i]);
        motion.jitter[i] = jitter_[i];
        motion.current_ndc_jitter[i] = ndc_jitter[i];
      }
      motion.z_min = viewport_info.z_min;
      motion.z_max = viewport_info.z_max;
      // Towards the camera, for the motion vector pass's depth test (none if
      // the draw's isn't less or greater).
      switch (depth_control.zfunc) {
        case xenos::CompareFunction::kLess:
        case xenos::CompareFunction::kLessEqual:
          motion.depth_test_offset = -kMotionDepthTestOffset;
          ++depth_less_draws_;
          break;
        case xenos::CompareFunction::kGreater:
        case xenos::CompareFunction::kGreaterEqual:
          motion.depth_test_offset = kMotionDepthTestOffset;
          ++depth_greater_draws_;
          break;
        default:
          motion.depth_test_offset = 0.0f;
      }
    }
    return;
  }
  // A full-screen pass reading the resolved scene: the last one before the
  // swap is the image without the UI.
  if (!scene_texture_ || !(color_mask & 0xF) ||
      surface.msaa_samples != xenos::MsaaSamples::k1X) {
    return;
  }
  uint32_t mask = pixel_shader_texture_mask;
  while (mask) {
    const uint32_t i = uint32_t(std::countr_zero(mask));
    mask &= mask - 1;
    if ((regs.GetTextureFetch(i).base_address << 12) == scene_texture_) {
      hudless_pending_ = true;
      hudless_pixel_shader_ = pixel_shader_hash;
      if (true_hdr_ && pixel_shader_hash == kTonemapPixelShader) {
        BeginHdrTonemap(regs);
      }
      return;
    }
  }
}

void DaoUpscalerImpl::BeforeDraw(const Shader& vertex_shader, bool indexed,
                                 uint32_t host_vertex_count,
                                 bool skip_motion) {
  if (stream_out_bound_pipeline_ && cp_.pipeline_binds_ != stream_out_binds_) {
    stream_out_bound_pipeline_ = nullptr;
  }
  bool streamed = false;
  if (motion_pending_) {
    motion_pending_ = false;
    if (skip_motion) {
      ++motion_skipped_;
    } else {
      streamed = CaptureMotion(vertex_shader, indexed, host_vertex_count);
    }
  }
  if (stream_out_bound_pipeline_ && !streamed) {
    cp_.GetDeferredCommandList().DaoCallback(ExecuteRestorePipeline,
                                             stream_out_guest_pipeline_);
    stream_out_bound_pipeline_ = nullptr;
  }
}

void DaoUpscalerImpl::AfterDraw() {
  // A draw's stream output buffer is bound for that draw only (see
  // CaptureMotion).
  CloseStreamOut();
  if (hudless_pending_) {
    hudless_pending_ = false;
    CaptureHudless();
  }
  if (hdr_pending_) {
    hdr_pending_ = false;
    RunHdrTonemap();
  }
}

void DaoUpscalerImpl::CloseStreamOut() {
  if (!stream_out_bound_) {
    return;
  }
  stream_out_bound_ = false;
  cp_.GetDeferredCommandList().DaoCallback(ExecuteCloseStreamOut, nullptr);
}

void DaoUpscalerImpl::CaptureHudless() {
  if (!render_size_[0]) {
    return;
  }
  D3D12RenderTargetCache::RenderTarget* const* targets =
      rtc_.last_update_accumulated_render_targets();
  auto* target =
      static_cast<D3D12RenderTargetCache::D3D12RenderTarget*>(targets[1]);
  if (!target) {
    return;
  }
  const D3D12RenderTargetCache::RenderTargetKey key = target->key();
  if (key.is_depth || key.msaa_samples != xenos::MsaaSamples::k1X) {
    return;
  }
  if (!EnsureTexture(color_, DXGI_FORMAT_R8G8B8A8_UNORM, render_size_[0],
                     render_size_[1], "color")) {
    return;
  }
  ID3D12Resource* source = target->resource();
  const D3D12_RESOURCE_DESC source_desc = source->GetDesc();
  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = rtc_.GetColorDrawDXGIFormat(
      xenos::ColorRenderTargetFormat(key.resource_format));
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  CopyConstants constants = {};
  constants.size[0] = std::min(render_size_[0], uint32_t(source_desc.Width));
  constants.size[1] = std::min(render_size_[1], uint32_t(source_desc.Height));
  CopyFromRenderTarget(source, srv, *target,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, color_,
                       copy_color_pipeline_.Get(), constants);
  hudless_captured_ = true;
}

void DaoUpscalerImpl::OnResolve(const draw_util::ResolveInfo& resolve_info) {
  if ((!jitter_active_ && !true_hdr_) ||
      !resolve_info.copy_dest_extent_length || !render_size_[0]) {
    return;
  }
  const RegisterFile& regs = rtc_.register_file();
  // Single-sampled, the scene's resolves are the first ones after its draws.
  if (regs.Get<reg::RB_SURFACE_INFO>().msaa_samples ==
          xenos::MsaaSamples::k1X &&
      !(single_sample_scene_drawn_ && !scene_texture_)) {
    return;
  }
  // A scene resolve (color, or depth for the game's own use).
  if (!scene_texture_ && !resolve_info.IsCopyingDepth()) {
    scene_texture_ = regs.values[XE_GPU_REG_RB_COPY_DEST_BASE];
  }
  // The bound depth buffer holds this tile's depth until a resolve clears it
  // (the last tile's depth resolve does, before the color resolve), so it's
  // taken at the first resolve after the tile's draws.
  if (!scene_depth_pending_) {
    return;
  }
  scene_depth_pending_ = false;
  // The depth buffer of the tile's draws (not necessarily what the resolve
  // reads: with the window offset in the EDRAM base, the draws use a render
  // target at another base, laid out like the screen).
  D3D12RenderTargetCache::D3D12RenderTarget* target = scene_depth_target_;
  if (!target) {
    return;
  }
  if (!EnsureTexture(depth_, DXGI_FORMAT_R32_FLOAT, render_size_[0],
                     render_size_[1], "depth")) {
    return;
  }
  const D3D12RenderTargetCache::RenderTargetKey key = target->key();
  const int32_t scale[2] = {
      int32_t(key.scale_native ? 1 : rtc_.draw_resolution_scale_x()),
      int32_t(key.scale_native ? 1 : rtc_.draw_resolution_scale_y())};
  ID3D12Resource* source = target->resource();
  const D3D12_RESOURCE_DESC source_desc = source->GetDesc();
  const int32_t source_size[2] = {int32_t(source_desc.Width),
                                  int32_t(source_desc.Height)};
  // The resolved area on the screen, and where the draws put it.
  int32_t screen[4], source_offset[2];
  for (uint32_t i = 0; i < 2; ++i) {
    screen[i] = (resolve_info.dao_rect[i] - resolve_info.dao_window_offset[i]) *
                scale[i];
    screen[2 + i] =
        (resolve_info.dao_rect[2 + i] - resolve_info.dao_window_offset[i]) *
        scale[i];
    source_offset[i] = scene_depth_window_offset_[i] * scale[i];
    // Clip to the screen and to the render target.
    screen[i] = std::max({screen[i], 0, -source_offset[i]});
    screen[2 + i] = std::min({screen[2 + i], int32_t(render_size_[i]),
                              source_size[i] - source_offset[i]});
    if (screen[2 + i] <= screen[i]) {
      return;
    }
  }
  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = D3D12RenderTargetCache::GetDepthSRVDepthDXGIFormat(
      xenos::DepthRenderTargetFormat(key.resource_format));
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  const bool msaa = key.msaa_samples != xenos::MsaaSamples::k1X;
  if (msaa) {
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
  } else {
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
  }
  CopyConstants constants;
  for (uint32_t i = 0; i < 2; ++i) {
    constants.source_offset[i] = screen[i] + source_offset[i];
    constants.dest_offset[i] = screen[i];
    constants.size[i] = uint32_t(screen[2 + i] - screen[i]);
  }
  CopyFromRenderTarget(
      source, srv, *target,
      D3D12_RESOURCE_STATE_DEPTH_READ |
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
      depth_,
      msaa ? copy_depth_ms_pipeline_.Get() : copy_depth_pipeline_.Get(),
      constants);
  ++depth_tiles_;
}

void DaoUpscalerImpl::GetDisplaySize(uint32_t render_width,
                                     uint32_t render_height,
                                     uint32_t& width_out,
                                     uint32_t& height_out) const {
  width_out = render_width;
  height_out = render_height;
  ui::Presenter* presenter = cp_.graphics_system_->presenter();
  if (!presenter) {
    return;
  }
  uint32_t surface_width, surface_height;
  presenter->GetPaintSurfaceSize(surface_width, surface_height);
  if (!surface_width || !surface_height) {
    return;
  }
  // The largest area with the game's aspect ratio, as the presenter shows it.
  const auto aspect = cp_.graphics_system_->GetScaledAspectRatio();
  uint32_t width = surface_width;
  uint32_t height = uint32_t(uint64_t(width) * aspect.second / aspect.first);
  if (height > surface_height) {
    height = surface_height;
    width = uint32_t(uint64_t(height) * aspect.first / aspect.second);
  }
  // FSR doesn't downscale: below the render size, it's anti-aliasing only and
  // the presenter scales down.
  if (width > render_width && height > render_height) {
    width_out = width & ~uint32_t(1);
    height_out = height & ~uint32_t(1);
  }
}

void DaoUpscalerImpl::InvalidateCommandProcessorState() {
  D3D12CommandProcessor& cp = cp_;
  ++cp.pipeline_binds_;
  cp.current_guest_pipeline_ = nullptr;
  cp.current_external_pipeline_ = nullptr;
  cp.current_graphics_root_signature_ = nullptr;
  cp.current_graphics_root_up_to_date_ = 0;
  if (cp.bindless_resources_used_) {
    cp.deferred_command_list_.SetDescriptorHeaps(
        cp.view_bindless_heap_, cp.sampler_bindless_heap_current_);
  } else {
    cp.view_bindful_heap_current_ = nullptr;
    cp.sampler_bindful_heap_current_ = nullptr;
  }
  cp.primitive_topology_ = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
  cp.ff_viewport_update_needed_ = true;
  cp.ff_scissor_update_needed_ = true;
  rtc_.InvalidateCommandListRenderTargets();
}

ID3D12Resource* DaoUpscalerImpl::OnSwap(
    ID3D12Resource* frontbuffer,
    const D3D12_SHADER_RESOURCE_VIEW_DESC& frontbuffer_srv,
    uint32_t render_width, uint32_t render_height,
    D3D12_SHADER_RESOURCE_VIEW_DESC& srv_out, bool& hdr_out,
    bool& upscaled_out) {
  hdr_out = false;
  upscaled_out = false;
  const auto now = std::chrono::steady_clock::now();
  const float frame_ms =
      std::chrono::duration<float, std::milli>(now - last_swap_).count();
  last_swap_ = now;

  const bool size_changed =
      render_size_[0] != render_width || render_size_[1] != render_height;
  render_size_[0] = render_width;
  render_size_[1] = render_height;

  overlay_shown_ = true_hdr_ && OverlayShown();
  const char* reason = nullptr;
  if (!jitter_active_) {
    if (true_hdr_ && !size_changed && hudless_captured_ && !overlay_shown_) {
      return CompositeHdr(frontbuffer, frontbuffer_srv, srv_out, hdr_out);
    }
    reason = "off";
  } else if (size_changed) {
    reason = "render size changed";
  } else if (!HasScene()) {
    reason = "no 3D scene";
  } else if (!depth_tiles_) {
    reason = "no scene depth";
  } else if (!hudless_captured_) {
    reason = "no image before the UI";
  }
  uint32_t display_width = 0, display_height = 0;
  if (!reason) {
    GetDisplaySize(render_width, render_height, display_width,
                   display_height);
    if (!EnsureContext(render_width, render_height, display_width,
                       display_height) ||
        !EnsureTexture(motion_vectors_, DXGI_FORMAT_R16G16_FLOAT,
                       render_width, render_height, "motion vector",
                       D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ||
        !EnsureTexture(upscaled_, DXGI_FORMAT_R16G16B16A16_FLOAT,
                       display_width, display_height, "upscaled") ||
        !EnsureTexture(composite_,
                       true_hdr_ ? HdrFormat() : DXGI_FORMAT_R8G8B8A8_UNORM,
                       display_width, display_height, "composite")) {
      reason = "upscaler unavailable";
    }
  }
  if (reason) {
    return NotUpscaled(reason, frontbuffer, frontbuffer_srv, srv_out, hdr_out);
  }

  // Motion vectors, then FSR, recorded into the command list when it's
  // executed.
  if (motion_available_) {
    RecordMotionPass(render_width, render_height);
    if (dao_timing::GpuTiming::Enabled()) {
      dao_timing::GpuTiming::Get().Mark(cp_, "motion vectors");
    }
  }
  if (true_hdr_ && !EnsureHdrScene()) {
    return NotUpscaled("no HDR scene", frontbuffer, frontbuffer_srv, srv_out,
                       hdr_out);
  }
  Texture& fsr_color = true_hdr_ ? hdr_ : color_;
  Transition(fsr_color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(color_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(depth_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(motion_vectors_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(upscaled_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  cp_.SubmitBarriers();
  ffxDispatchDescUpscale& desc = dispatch_params_.desc;
  desc = {};
  desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
  desc.color = ffxApiGetResourceDX12(fsr_color.resource.Get());
  desc.depth = ffxApiGetResourceDX12(depth_.resource.Get());
  desc.motionVectors = ffxApiGetResourceDX12(motion_vectors_.resource.Get());
  desc.output = ffxApiGetResourceDX12(upscaled_.resource.Get(),
                                      FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
  desc.jitterOffset = {jitter_[0], jitter_[1]};
  desc.motionVectorScale = {1.0f, 1.0f};
  desc.renderSize = {render_width, render_height};
  desc.upscaleSize = {display_width, display_height};
  const double sharpness =
      dao_live::Load(dao_live::values.temporal_sharpness);
  desc.enableSharpening = sharpness > 0.0;
  desc.sharpness = float(std::clamp(sharpness, 0.0, 1.0));
  desc.frameTimeDelta = std::clamp(frame_ms, 1.0f, 100.0f);
  desc.preExposure = 1.0f;
  desc.reset = reset_history_;
  // Rough camera values; FSR uses them for its depth-based heuristics.
  desc.cameraNear = 0.1f;
  desc.cameraFar = 1000.0f;
  desc.cameraFovAngleVertical = 1.0f;
  desc.viewSpaceToMetersFactor = 1.0f;
  desc.flags = true_hdr_ ? 0 : FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
  dispatch_params_.self = this;
  cp_.GetDeferredCommandList().DaoCallback(ExecuteDispatch, &dispatch_params_);
  InvalidateCommandProcessorState();
  if (dao_timing::GpuTiming::Enabled()) {
    dao_timing::GpuTiming::Get().Mark(cp_, "FSR");
  }

  // Motion blur's tiles, then the composite of the UI (blurring the scene).
  // All the views at once: requesting more could switch descriptor heaps.
  ID3D12Device* device = cp_.GetD3D12Provider().GetDevice();
  MotionBlur blur = GetMotionBlur(render_width, render_height);
  if (blur.samples &&
      (!EnsureTexture(blur_pixels_, DXGI_FORMAT_R32G32_FLOAT, render_width,
                      render_height, "blur pixel") ||
       !EnsureTexture(blur_tiles_, DXGI_FORMAT_R16G16_FLOAT,
                      blur.tile_count[0], blur.tile_count[1], "blur tile") ||
       !EnsureTexture(blur_around_, DXGI_FORMAT_R16G16_FLOAT,
                      blur.tile_count[0], blur.tile_count[1],
                      "blur neighborhood") ||
       !EnsureTexture(blur_cuts_, DXGI_FORMAT_R32_UINT, 2, 1, "blur cut") ||
       !EnsureTexture(blur_motion_[0], DXGI_FORMAT_R16G16_FLOAT, render_width,
                      render_height, "blur motion") ||
       !EnsureTexture(blur_motion_[1], DXGI_FORMAT_R16G16_FLOAT, render_width,
                      render_height, "blur motion"))) {
    // Not trying again every frame: until the render size changes.
    blur_failed_size_[0] = render_width;
    blur_failed_size_[1] = render_height;
    blur = {};
  }
  ui::d3d12::util::DescriptorCpuGpuHandlePair
      descriptors[kCompositeTables + kBlurViews];
  if (!cp_.RequestOneUseSingleViewDescriptors(
          kCompositeTables + (blur.samples ? kBlurViews : 0), descriptors)) {
    return NotUpscaled("no descriptors", frontbuffer, frontbuffer_srv, srv_out,
                       hdr_out);
  }
  if (blur.samples) {
    RecordMotionBlurTiles(blur, render_width, render_height,
                          descriptors + kCompositeTables);
    if (dao_timing::GpuTiming::Enabled()) {
      dao_timing::GpuTiming::Get().Mark(cp_, "motion blur tiles");
    }
  } else {
    blur_motion_valid_ = false;
  }
  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  srv.Format = upscaled_.format;
  device->CreateShaderResourceView(upscaled_.resource.Get(), &srv,
                                   descriptors[0].first);
  device->CreateShaderResourceView(frontbuffer, &frontbuffer_srv,
                                   descriptors[1].first);
  srv.Format = color_.format;
  device->CreateShaderResourceView(color_.resource.Get(), &srv,
                                   descriptors[2].first);
  srv.Format = motion_vectors_.format;
  device->CreateShaderResourceView(motion_vectors_.resource.Get(), &srv,
                                   descriptors[3].first);
  // Motion blur's pixels and neighbourhoods (without blur, unread: any view).
  if (blur.samples) {
    srv.Format = blur_pixels_.format;
    device->CreateShaderResourceView(blur_pixels_.resource.Get(), &srv,
                                     descriptors[4].first);
    srv.Format = blur_around_.format;
    device->CreateShaderResourceView(blur_around_.resource.Get(), &srv,
                                     descriptors[5].first);
  } else {
    descriptors[4] = descriptors[5] = descriptors[3];
  }
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
  uav.Format = composite_.format;
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  device->CreateUnorderedAccessView(composite_.resource.Get(), nullptr, &uav,
                                    descriptors[6].first);
  Transition(upscaled_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(composite_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  CompositeConstants constants = {};
  constants.dest_size[0] = display_width;
  constants.dest_size[1] = display_height;
  constants.render_size[0] = float(render_width);
  constants.render_size[1] = float(render_height);
  constants.show_motion = cvars::dao_show_motion_vectors ? 1 : 0;
  constants.mode = !true_hdr_       ? kCompositeSdr
                   : overlay_shown_ ? kCompositeHdrToSdr
                                    : kCompositeHdr;
  constants.blur_samples = blur.samples;
  constants.blur_tile = blur.tile;
  DispatchComposite(descriptors, constants);
  Transition(composite_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  srv_out = {};
  srv_out.Format = composite_.format;
  srv_out.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv_out.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv_out.Texture2D.MipLevels = 1;
  hdr_out = true_hdr_ && !overlay_shown_;
  upscaled_out = true;
  EndFrame(true, nullptr);
  return composite_.resource.Get();
}

void DaoUpscalerImpl::DispatchComposite(
    const ui::d3d12::util::DescriptorCpuGpuHandlePair* descriptors,
    CompositeConstants constants) {
  cp_.SubmitBarriers();
  constants.paper_white = PaperWhiteNits();
  constants.gamma = OutputGamma();
  constants.scrgb = HdrScrgb() ? 1 : 0;
  DeferredCommandList& command_list = cp_.GetDeferredCommandList();
  command_list.D3DSetComputeRootSignature(composite_root_signature_.Get());
  command_list.D3DSetComputeRoot32BitConstants(
      0, sizeof(constants) / sizeof(uint32_t), &constants, 0);
  for (uint32_t i = 0; i < kCompositeTables; ++i) {
    command_list.D3DSetComputeRootDescriptorTable(1 + i,
                                                  descriptors[i].second);
  }
  cp_.SetExternalPipeline(constants.blur_samples
                              ? composite_blur_pipeline_.Get()
                              : composite_pipeline_.Get());
  command_list.D3DDispatch((constants.dest_size[0] + 7) / 8,
                           (constants.dest_size[1] + 7) / 8, 1);
}

bool DaoUpscalerImpl::ConvertImage(
    ID3D12Resource* source, const D3D12_SHADER_RESOURCE_VIEW_DESC& source_srv,
    Texture& dest, CompositeMode mode) {
  // The source and destination views; the source is read as t1 only, the
  // other tables share its view.
  ui::d3d12::util::DescriptorCpuGpuHandlePair descriptors[2];
  if (!cp_.RequestOneUseSingleViewDescriptors(2, descriptors)) {
    return false;
  }
  ID3D12Device* device = cp_.GetD3D12Provider().GetDevice();
  device->CreateShaderResourceView(source, &source_srv, descriptors[0].first);
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
  uav.Format = dest.format;
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  device->CreateUnorderedAccessView(dest.resource.Get(), nullptr, &uav,
                                    descriptors[1].first);
  const ui::d3d12::util::DescriptorCpuGpuHandlePair tables[kCompositeTables] = {
      descriptors[0], descriptors[0], descriptors[0], descriptors[0],
      descriptors[0], descriptors[0], descriptors[1]};
  Transition(dest, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  CompositeConstants constants = {};
  constants.dest_size[0] = dest.width;
  constants.dest_size[1] = dest.height;
  constants.render_size[0] = float(dest.width);
  constants.render_size[1] = float(dest.height);
  constants.mode = mode;
  DispatchComposite(tables, constants);
  Transition(dest, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  return true;
}

ID3D12Resource* DaoUpscalerImpl::NotUpscaled(
    const char* reason, ID3D12Resource* frontbuffer,
    const D3D12_SHADER_RESOURCE_VIEW_DESC& frontbuffer_srv,
    D3D12_SHADER_RESOURCE_VIEW_DESC& srv_out, bool& hdr_out) {
  // With an overlay, the presenter's SDR path.
  const bool encode = true_hdr_ && !overlay_shown_ &&
                      EnsureTexture(encoded_, HdrFormat(), render_size_[0],
                                    render_size_[1], "encoded") &&
                      ConvertImage(frontbuffer, frontbuffer_srv, encoded_,
                                   kCompositeEncode);
  EndFrame(false, reason);
  if (!encode) {
    return nullptr;
  }
  hdr_out = true;
  srv_out = {};
  srv_out.Format = encoded_.format;
  srv_out.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv_out.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv_out.Texture2D.MipLevels = 1;
  return encoded_.resource.Get();
}

void DaoUpscalerImpl::EndFrame(bool upscaled, const char* reason) {
  if (upscaled != was_upscaling_) {
    if (upscaled) {
      XELOGI("FSR: upscaling ({} draws, {} depth tiles, image before UI from "
             "pixel shader {:016X})",
             scene_draws_, depth_tiles_, hudless_pixel_shader_);
      motion_logged_ = false;
    } else {
      XELOGI("FSR: not upscaling - {}", reason);
    }
    was_upscaling_ = upscaled;
  }
  // History is valid only across consecutive upscaled frames.
  reset_history_ = !upscaled;
  // Counted per frame.
  depth_less_draws_ = depth_greater_draws_ = 0;
  // If a 3D scene couldn't be upscaled, stop jittering it for a while (a few
  // jittered frames shown as they are look like a small shake).
  const bool enabled = cvars::dao_temporal_upscaler != "off";
  if (HasScene() && !upscaled && jitter_active_) {
    if (++fail_streak_ >= 3) {
      jitter_cooldown_ = 300;
      fail_streak_ = 0;
    }
  } else if (upscaled) {
    fail_streak_ = 0;
  }
  if (jitter_cooldown_) {
    --jitter_cooldown_;
  }
  // No retries once FSR couldn't be loaded or created.
  jitter_active_ = enabled && !jitter_cooldown_ && !ffx_failed_;
  true_hdr_ = WantsTrueHdr();
  // The game's 2xMSAA while the scene isn't antialiased here.
  DaoUpscaler::single_sample_scene_.store(
      single_sample_scene_ && jitter_active_, std::memory_order_relaxed);

  // Next frame's jitter.
  jitter_[0] = jitter_[1] = 0.0f;
  if (jitter_active_) {
    jitter_index_ = (jitter_index_ + 1) % uint32_t(jitter_phase_count_);
    float x = 0.0f, y = 0.0f;
    bool queried = false;
    if (context_) {
      ffxQueryDescUpscaleGetJitterOffset offset = {};
      offset.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
      offset.index = int32_t(jitter_index_);
      offset.phaseCount = jitter_phase_count_;
      offset.pOutX = &x;
      offset.pOutY = &y;
      queried = ffx_query_(&context_, &offset.header) == FFX_API_RETURN_OK;
    }
    if (!queried) {
      // Halton (2, 3), as FSR uses.
      auto halton = [](uint32_t index, uint32_t base) {
        float f = 1.0f, r = 0.0f;
        for (uint32_t i = index + 1; i; i /= base) {
          f /= float(base);
          r += f * float(i % base);
        }
        return r;
      };
      x = halton(jitter_index_, 2) - 0.5f;
      y = halton(jitter_index_, 3) - 0.5f;
    }
    // FSR's jitter offset is the negated sample offset.
    jitter_[0] = -x;
    jitter_[1] = -y;
  }

  // Motion: how the draws were covered, once after upscaling starts.
  const MotionDrawPool& motion_draws = motion_draws_[motion_frame_];
  if (upscaled && !motion_logged_ && !previous_motion_draws_.empty()) {
    motion_logged_ = true;
    XELOGI(
        "FSR: motion vectors for {} draws ({} new, {} skipped), {} KB of "
        "positions",
        motion_matched_, motion_unmatched_,
        motion_skipped_, stream_out_used_ >> 10);
  }
  // This frame's draws are the previous ones for the next frame (the commands
  // reading them are executed at this swap).
  previous_motion_draws_.clear();
  for (const MotionDraw& draw : motion_draws) {
    previous_motion_draws_.push_back({draw.key, draw.constants_sum, &draw});
  }
  std::sort(previous_motion_draws_.begin(), previous_motion_draws_.end(),
            [](const MotionCandidate& a, const MotionCandidate& b) {
              return a.key != b.key ? a.key < b.key
                                    : a.constants_sum < b.constants_sum;
            });
  motion_frame_ ^= 1;
  motion_draws_[motion_frame_].clear();
  motion_constant_data_[motion_frame_].clear();
  motion_seen_.clear();
  CloseStreamOut();
  stream_out_used_ = 0;
  stream_out_counters_used_ = 0;
  stream_out_reset_ = false;
  motion_matched_ = 0;
  motion_unmatched_ = 0;
  motion_skipped_ = 0;
  motion_pending_ = false;

  scene_draws_ = 0;
  depth_tiles_ = 0;
  scene_texture_ = 0;
  single_sample_scene_drawn_ = false;
  scene_depth_pending_ = false;
  scene_depth_target_ = nullptr;
  hudless_pending_ = false;
  hudless_captured_ = false;
  hdr_pending_ = false;
  hdr_captured_ = false;
}

void DaoUpscaler::Initialize(D3D12CommandProcessor& command_processor,
                             D3D12RenderTargetCache& render_target_cache) {
  Shutdown();
  auto* impl = new DaoUpscalerImpl(command_processor, render_target_cache);
  if (!impl->Initialize()) {
    delete impl;
    return;
  }
  impl_ = impl;
  single_sample_scene_.store(impl->single_sample_scene(),
                             std::memory_order_relaxed);
}

void DaoUpscaler::Shutdown() {
  single_sample_scene_.store(false, std::memory_order_relaxed);
  delete impl_;
  impl_ = nullptr;
}

bool DaoUpscaler::WantsSingleSampleScene() {
  return single_sample_scene_.load(std::memory_order_relaxed);
}

void DaoUpscaler::OnDraw(const RegisterFile& regs,
                         draw_util::ViewportInfo& viewport_info,
                         reg::RB_DEPTHCONTROL normalized_depth_control,
                         uint32_t normalized_color_mask,
                         uint64_t pixel_shader_hash,
                         uint32_t pixel_shader_texture_mask,
                         bool window_offset_in_edram) {
  if (impl_) {
    impl_->OnDraw(regs, viewport_info, normalized_depth_control,
                  normalized_color_mask, pixel_shader_hash,
                  pixel_shader_texture_mask, window_offset_in_edram);
  }
}

void DaoUpscaler::BeforeDraw(const Shader& vertex_shader, bool indexed,
                             uint32_t host_vertex_count, bool skip_motion) {
  if (impl_) {
    impl_->BeforeDraw(vertex_shader, indexed, host_vertex_count, skip_motion);
  }
}

void DaoUpscaler::AfterDraw() {
  if (impl_) {
    impl_->AfterDraw();
  }
}

void DaoUpscaler::OnResolve(const draw_util::ResolveInfo& resolve_info) {
  if (impl_) {
    impl_->OnResolve(resolve_info);
  }
}

ID3D12Resource* DaoUpscaler::OnSwap(
    ID3D12Resource* frontbuffer,
    const D3D12_SHADER_RESOURCE_VIEW_DESC& frontbuffer_srv,
    uint32_t frontbuffer_width, uint32_t frontbuffer_height,
    D3D12_SHADER_RESOURCE_VIEW_DESC& srv_out, bool& hdr_out,
    bool& upscaled_out) {
  hdr_out = upscaled_out = false;
  return impl_ ? impl_->OnSwap(frontbuffer, frontbuffer_srv, frontbuffer_width,
                               frontbuffer_height, srv_out, hdr_out,
                               upscaled_out)
               : nullptr;
}

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe
