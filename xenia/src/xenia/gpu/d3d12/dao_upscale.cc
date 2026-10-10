// DAO: temporal upscaling with AMD FSR 3.1 / FSR 4 - see dao_upscale.h.
#include "xenia/gpu/d3d12/dao_upscale.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform_win.h"
#include "xenia/base/xxhash.h"
#include "xenia/gpu/d3d12/d3d12_command_processor.h"
#include "xenia/gpu/d3d12/d3d12_render_target_cache.h"
#include "xenia/gpu/d3d12/pipeline_cache.h"
#include "xenia/gpu/dxbc_shader_translator.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/ui/d3d12/d3d12_presenter.h"
#include "xenia/ui/d3d12/d3d12_util.h"

#include "dx12/ffx_api_dx12.h"
#include "ffx_api.h"
#include "ffx_upscale.h"

DEFINE_string(dao_temporal_upscaler, "off",
              "Temporal upscaling and anti-aliasing of 3D scenes: off, auto "
              "(FSR 4 where supported, otherwise FSR 3.1), fsr4, fsr3.",
              "GPU");
DEFINE_double(dao_temporal_sharpness, 0.3,
              "Sharpening after temporal upscaling, 0 (off) to 1.", "GPU");
DEFINE_bool(dao_motion_vectors, true,
            "Motion vectors for temporal upscaling (false: zero motion, for "
            "comparison).",
            "GPU");
DEFINE_bool(dao_show_motion_vectors, false,
            "Shows the motion vectors instead of the game (red: horizontal, "
            "green: vertical motion, gray: none), for debugging.",
            "GPU");

namespace xe {
namespace gpu {
namespace d3d12 {

namespace shaders {
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/dao_upscale_composite_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/dao_upscale_copy_color_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/dao_upscale_copy_depth_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/dao_upscale_copy_depth_ms_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/dao_upscale_mv_ps.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/dao_upscale_mv_vs.h"
}  // namespace shaders

namespace {

using Microsoft::WRL::ComPtr;

// Stream output of the scene draws' positions (float4 per vertex, two passes
// per draw), followed by the filled-size counters (one per pass, 16 bytes
// apart).
constexpr uint32_t kStreamOutDataSize = 96 << 20;
constexpr uint32_t kStreamOutCounterCount = 4096;
constexpr uint32_t kStreamOutCounterSize = kStreamOutCounterCount * 16;
constexpr uint32_t kStreamOutBufferSize =
    kStreamOutDataSize + kStreamOutCounterSize;

// A texture owned by the upscaler, with its tracked state.
struct Texture {
  ComPtr<ID3D12Resource> resource;
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  uint32_t width = 0;
  uint32_t height = 0;
};

void FfxMessage(uint32_t type, const wchar_t* message) {
  std::string text;
  for (const wchar_t* c = message; c && *c; ++c) {
    text += char(*c < 128 ? *c : '?');
  }
  if (type == FFX_API_MESSAGE_TYPE_ERROR) {
    XELOGE("FSR: {}", text);
  } else {
    XELOGW("FSR: {}", text);
  }
}

}  // namespace

class DaoUpscalerImpl {
 public:
  DaoUpscalerImpl(D3D12CommandProcessor& command_processor,
                  D3D12RenderTargetCache& render_target_cache)
      : cp_(command_processor), rtc_(render_target_cache) {}
  ~DaoUpscalerImpl();

  bool Initialize();
  void OnDraw(const RegisterFile& regs, draw_util::ViewportInfo& viewport_info,
              reg::RB_DEPTHCONTROL depth_control, uint32_t color_mask,
              uint64_t pixel_shader_hash, uint32_t pixel_shader_texture_mask,
              bool window_offset_in_edram);
  void AfterDraw(const Shader& vertex_shader, bool indexed,
                 uint32_t host_vertex_count, bool skip_motion);
  void OnResolve(const draw_util::ResolveInfo& resolve_info);
  ID3D12Resource* OnSwap(ID3D12Resource* frontbuffer,
                         const D3D12_SHADER_RESOURCE_VIEW_DESC& frontbuffer_srv,
                         uint32_t render_width, uint32_t render_height,
                         D3D12_SHADER_RESOURCE_VIEW_DESC& srv_out);

 private:
  struct CopyConstants {
    int32_t source_offset[2];
    int32_t dest_offset[2];
    uint32_t size[2];
  };
  struct CompositeConstants {
    uint32_t dest_size[2];
    float render_size[2];
    uint32_t show_motion;
  };
  // What the deferred FSR dispatch needs when the command list is executed.
  struct DispatchParams {
    DaoUpscalerImpl* self;
    ffxDispatchDescUpscale desc;
  };
  // dao_upscale_mv.hlsli.
  struct MotionConstants {
    uint32_t current_offset;
    uint32_t previous_offset;
    uint32_t current_counter;
    uint32_t previous_counter;
    float viewport_offset[2];
    float viewport_extent[2];
    float render_size[2];
    float jitter[2];
    float z_min;
    float z_max;
  };
  // A scene draw given motion vectors.
  struct MotionDraw {
    DaoUpscalerImpl* self;
    // For finding the same draw in the next frame.
    uint64_t key;
    uint64_t constants_hash;
    std::vector<uint32_t> constants;
    // Stream output, right after the draw.
    ID3D12PipelineState* stream_out_pipeline;
    ID3D12PipelineState* pipeline;
    UINT float_parameter;
    UINT system_parameter;
    D3D12_GPU_VIRTUAL_ADDRESS float_constants;
    D3D12_GPU_VIRTUAL_ADDRESS previous_float_constants;
    D3D12_GPU_VIRTUAL_ADDRESS system_constants;
    D3D12_GPU_VIRTUAL_ADDRESS unjittered_system_constants;
    bool indexed;
    uint32_t host_vertex_count;
    // Triangle list vertices written by each pass (0 if no motion vectors),
    // and the space for them.
    uint32_t output_vertex_count;
    uint32_t output_size;
    MotionConstants motion;
  };
  // The motion vector pass at the swap.
  struct MotionPass {
    DaoUpscalerImpl* self;
    const std::deque<MotionDraw>* draws;
    D3D12_GPU_DESCRIPTOR_HANDLE positions;
    D3D12_GPU_DESCRIPTOR_HANDLE depth;
    uint32_t width;
    uint32_t height;
  };
  struct StreamOutPipeline {
    const void* vertex_shader;
    ID3D12RootSignature* root_signature;
    ComPtr<ID3D12PipelineState> state;
  };

  bool LoadFfx();
  bool EnsureContext(uint32_t render_width, uint32_t render_height,
                     uint32_t display_width, uint32_t display_height);
  void DestroyContext();
  bool EnsureTexture(Texture& texture, DXGI_FORMAT format, uint32_t width,
                     uint32_t height, const char* name,
                     D3D12_RESOURCE_FLAGS extra_flags = D3D12_RESOURCE_FLAG_NONE);
  void Transition(Texture& texture, D3D12_RESOURCE_STATES state);
  // Copies a rectangle of a render target into one of the FSR input textures.
  void CopyFromRenderTarget(ID3D12Resource* source,
                            const D3D12_SHADER_RESOURCE_VIEW_DESC& source_srv,
                            D3D12RenderTargetCache::D3D12RenderTarget& target,
                            D3D12_RESOURCE_STATES read_state, Texture& dest,
                            ID3D12PipelineState* pipeline,
                            const CopyConstants& constants);
  void CaptureHudless();
  bool InitializeMotion();
  ID3D12PipelineState* GetStreamOutPipeline(
      const PipelineCache::Pipeline& pipeline);
  void TransitionStreamOut(D3D12_RESOURCE_STATES state);
  void CaptureMotion(const Shader& vertex_shader, bool indexed,
                     uint32_t host_vertex_count);
  static void ExecuteStreamOut(void* context,
                               ID3D12GraphicsCommandList* command_list);
  void RecordMotionPass(uint32_t width, uint32_t height);
  static void ExecuteMotionPass(void* context,
                                ID3D12GraphicsCommandList* command_list);
  void GetDisplaySize(uint32_t render_width, uint32_t render_height,
                      uint32_t& width_out, uint32_t& height_out) const;
  // Re-records the command processor's bindings after external commands.
  void InvalidateCommandProcessorState();
  static void ExecuteDispatch(void* context,
                              ID3D12GraphicsCommandList* command_list);
  void EndFrame(bool upscaled, const char* reason);

  D3D12CommandProcessor& cp_;
  D3D12RenderTargetCache& rtc_;

  HMODULE ffx_module_ = nullptr;
  PfnFfxCreateContext ffx_create_ = nullptr;
  PfnFfxDestroyContext ffx_destroy_ = nullptr;
  PfnFfxQuery ffx_query_ = nullptr;
  PfnFfxDispatch ffx_dispatch_ = nullptr;
  bool ffx_failed_ = false;
  ffxContext context_ = nullptr;
  uint32_t context_size_[4] = {};
  std::string version_name_;
  int32_t jitter_phase_count_ = 8;
  uint32_t jitter_index_ = 0;

  ComPtr<ID3D12RootSignature> copy_root_signature_;
  ComPtr<ID3D12PipelineState> copy_depth_ms_pipeline_;
  ComPtr<ID3D12PipelineState> copy_depth_pipeline_;
  ComPtr<ID3D12PipelineState> copy_color_pipeline_;
  ComPtr<ID3D12RootSignature> composite_root_signature_;
  ComPtr<ID3D12PipelineState> composite_pipeline_;

  // Render resolution inputs.
  Texture color_;
  Texture depth_;
  Texture motion_vectors_;
  // Display resolution.
  Texture upscaled_;
  Texture composite_;
  // Size of the render resolution textures (the front buffer of the last
  // frame, the scaled 1280x720 normally).
  uint32_t render_size_[2] = {};

  // Jitter of the current frame in render pixels (FSR's jitterOffset).
  bool jitter_active_ = false;
  float jitter_[2] = {};
  // Frames left before jittering again after FSR could not run.
  uint32_t jitter_cooldown_ = 0;
  uint32_t fail_streak_ = 0;

  // Current frame.
  uint32_t scene_draws_ = 0;
  uint32_t depth_tiles_ = 0;
  uint32_t scene_texture_ = 0;
  bool scene_depth_pending_ = false;
  D3D12RenderTargetCache::D3D12RenderTarget* scene_depth_target_ = nullptr;
  int32_t scene_depth_window_offset_[2] = {};
  bool hudless_pending_ = false;
  bool hudless_captured_ = false;
  uint64_t hudless_pixel_shader_ = 0;
  bool reset_history_ = true;
  bool was_upscaling_ = false;
  std::chrono::steady_clock::time_point last_swap_;

  DispatchParams dispatch_params_;

  // Motion vectors.
  bool motion_available_ = false;
  ComPtr<ID3D12RootSignature> motion_root_signature_;
  ComPtr<ID3D12PipelineState> motion_pipeline_;
  ComPtr<ID3D12DescriptorHeap> motion_rtv_heap_;
  ComPtr<ID3D12Resource> stream_out_buffer_;
  ComPtr<ID3D12Resource> stream_out_zeros_;
  D3D12_RESOURCE_STATES stream_out_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
  std::unordered_map<const void*, StreamOutPipeline> stream_out_pipelines_;
  // This frame's draws and the previous frame's, alternating (a frame's draws
  // are read by its commands until they're executed after the swap).
  std::deque<MotionDraw> motion_draws_[2];
  uint32_t motion_frame_ = 0;
  std::unordered_multimap<uint64_t, const MotionDraw*> previous_motion_draws_;
  std::unordered_set<uint64_t> motion_seen_;
  uint32_t stream_out_used_ = 0;
  uint32_t stream_out_counters_used_ = 0;
  bool stream_out_reset_ = false;
  uint32_t motion_unmatched_ = 0;
  uint32_t motion_skipped_ = 0;
  bool motion_logged_ = false;
  // From OnDraw to AfterDraw.
  bool motion_pending_ = false;
  float motion_ndc_jitter_[2] = {};
  MotionConstants motion_viewport_ = {};
  uint64_t motion_pixel_shader_ = 0;
  MotionPass motion_pass_ = {};
};

DaoUpscalerImpl* DaoUpscaler::impl_ = nullptr;

DaoUpscalerImpl::~DaoUpscalerImpl() {
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

  // Composite: constants, 4 SRVs (separate tables, the one-use descriptors
  // aren't contiguous), destination UAV, linear sampler.
  D3D12_DESCRIPTOR_RANGE composite_ranges[5] = {};
  for (uint32_t i = 0; i < 4; ++i) {
    composite_ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    composite_ranges[i].NumDescriptors = 1;
    composite_ranges[i].BaseShaderRegister = i;
  }
  composite_ranges[4].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  composite_ranges[4].NumDescriptors = 1;
  D3D12_ROOT_PARAMETER composite_parameters[6] = {};
  composite_parameters[0].ParameterType =
      D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  composite_parameters[0].Constants.Num32BitValues =
      sizeof(CompositeConstants) / sizeof(uint32_t);
  for (uint32_t i = 0; i < 5; ++i) {
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
  if (!copy_root_signature_ || !composite_root_signature_) {
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
  if (!copy_depth_ms_pipeline_ || !copy_depth_pipeline_ ||
      !copy_color_pipeline_ || !composite_pipeline_) {
    XELOGE("FSR: failed to create the pipelines");
    return false;
  }
  motion_available_ = InitializeMotion();
  if (!motion_available_) {
    XELOGW("FSR: motion vectors unavailable");
  }
  jitter_active_ = cvars::dao_temporal_upscaler != "off";
  last_swap_ = std::chrono::steady_clock::now();
  return true;
}

bool DaoUpscalerImpl::InitializeMotion() {
  const ui::d3d12::D3D12Provider& provider = cp_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  // Constants, positions (raw buffer), scene depth.
  D3D12_DESCRIPTOR_RANGE ranges[2] = {};
  for (uint32_t i = 0; i < 2; ++i) {
    ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[i].NumDescriptors = 1;
    ranges[i].BaseShaderRegister = i;
  }
  D3D12_ROOT_PARAMETER parameters[3] = {};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants.Num32BitValues =
      sizeof(MotionConstants) / sizeof(uint32_t);
  for (uint32_t i = 0; i < 2; ++i) {
    parameters[1 + i].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1 + i].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
  }
  D3D12_ROOT_SIGNATURE_DESC root_desc = {};
  root_desc.NumParameters = UINT(std::size(parameters));
  root_desc.pParameters = parameters;
  motion_root_signature_.Attach(
      ui::d3d12::util::CreateRootSignature(provider, root_desc));
  if (!motion_root_signature_) {
    return false;
  }

  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
  desc.pRootSignature = motion_root_signature_.Get();
  desc.VS.pShaderBytecode = shaders::dao_upscale_mv_vs;
  desc.VS.BytecodeLength = sizeof(shaders::dao_upscale_mv_vs);
  desc.PS.pShaderBytecode = shaders::dao_upscale_mv_ps;
  desc.PS.BytecodeLength = sizeof(shaders::dao_upscale_mv_ps);
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
      D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = UINT_MAX;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = DXGI_FORMAT_R16G16_FLOAT;
  desc.SampleDesc.Count = 1;
  if (FAILED(device->CreateGraphicsPipelineState(
          &desc, IID_PPV_ARGS(&motion_pipeline_)))) {
    return false;
  }

  D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  heap_desc.NumDescriptors = 1;
  if (FAILED(device->CreateDescriptorHeap(&heap_desc,
                                          IID_PPV_ARGS(&motion_rtv_heap_)))) {
    return false;
  }

  D3D12_RESOURCE_DESC buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(buffer_desc, kStreamOutBufferSize,
                                          D3D12_RESOURCE_FLAG_NONE);
  if (FAILED(device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE,
          &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
          IID_PPV_ARGS(&stream_out_buffer_)))) {
    return false;
  }
  stream_out_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
  ui::d3d12::util::FillBufferResourceDesc(buffer_desc, kStreamOutCounterSize,
                                          D3D12_RESOURCE_FLAG_NONE);
  if (FAILED(device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesUpload, D3D12_HEAP_FLAG_NONE,
          &buffer_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
          IID_PPV_ARGS(&stream_out_zeros_)))) {
    return false;
  }
  void* mapping;
  D3D12_RANGE read_range = {};
  if (FAILED(stream_out_zeros_->Map(0, &read_range, &mapping))) {
    return false;
  }
  std::memset(mapping, 0, kStreamOutCounterSize);
  stream_out_zeros_->Unmap(0, nullptr);
  return true;
}

bool DaoUpscalerImpl::LoadFfx() {
  if (ffx_module_ || ffx_failed_) {
    return !ffx_failed_;
  }
  ffx_module_ = LoadLibraryW(L"amd_fidelityfx_loader_dx12.dll");
  if (ffx_module_) {
    ffx_create_ = reinterpret_cast<PfnFfxCreateContext>(
        GetProcAddress(ffx_module_, "ffxCreateContext"));
    ffx_destroy_ = reinterpret_cast<PfnFfxDestroyContext>(
        GetProcAddress(ffx_module_, "ffxDestroyContext"));
    ffx_query_ =
        reinterpret_cast<PfnFfxQuery>(GetProcAddress(ffx_module_, "ffxQuery"));
    ffx_dispatch_ = reinterpret_cast<PfnFfxDispatch>(
        GetProcAddress(ffx_module_, "ffxDispatch"));
  }
  if (!ffx_create_ || !ffx_destroy_ || !ffx_query_ || !ffx_dispatch_) {
    XELOGE(
        "FSR: amd_fidelityfx_loader_dx12.dll is missing or incomplete, "
        "temporal upscaling is off");
    ffx_failed_ = true;
    return false;
  }

  // List the available upscaler versions for the log.
  ID3D12Device* device = cp_.GetD3D12Provider().GetDevice();
  ffxQueryDescGetVersions versions = {};
  versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
  versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  versions.device = device;
  uint64_t count = 0;
  versions.outputCount = &count;
  ffx_query_(nullptr, &versions.header);
  std::vector<uint64_t> ids(count);
  std::vector<const char*> names(count);
  versions.versionIds = ids.data();
  versions.versionNames = names.data();
  ffx_query_(nullptr, &versions.header);
  for (uint64_t i = 0; i < count; ++i) {
    XELOGI("FSR: available upscaler {}", names[i] ? names[i] : "?");
  }
  return true;
}

bool DaoUpscalerImpl::EnsureContext(uint32_t render_width,
                                    uint32_t render_height,
                                    uint32_t display_width,
                                    uint32_t display_height) {
  if (context_ && context_size_[0] == render_width &&
      context_size_[1] == render_height && context_size_[2] == display_width &&
      context_size_[3] == display_height) {
    return true;
  }
  if (!LoadFfx()) {
    return false;
  }
  DestroyContext();

  ID3D12Device* device = cp_.GetD3D12Provider().GetDevice();

  // Pick the version: the newest by default (FSR 4 on RDNA 4), or the one
  // asked for.
  uint64_t version_id = 0;
  const std::string& wanted = cvars::dao_temporal_upscaler;
  if (wanted == "fsr3" || wanted == "fsr4") {
    ffxQueryDescGetVersions versions = {};
    versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    versions.device = device;
    uint64_t count = 0;
    versions.outputCount = &count;
    ffx_query_(nullptr, &versions.header);
    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count);
    versions.versionIds = ids.data();
    versions.versionNames = names.data();
    ffx_query_(nullptr, &versions.header);
    const char prefix = wanted == "fsr3" ? '3' : '4';
    for (uint64_t i = 0; i < count; ++i) {
      if (names[i] && names[i][0] == prefix) {
        version_id = ids[i];
        break;
      }
    }
    if (!version_id) {
      XELOGW("FSR: no {} upscaler available, using the default", wanted);
    }
  }

  ffxCreateContextDescUpscale desc = {};
  desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  // The input is the tonemapped (gamma-encoded) image.
  desc.flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
  desc.maxRenderSize = {render_width, render_height};
  desc.maxUpscaleSize = {display_width, display_height};
  desc.fpMessage = FfxMessage;
  ffxCreateContextDescUpscaleVersion version = {};
  version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
  version.version = FFX_UPSCALER_VERSION;
  ffxCreateBackendDX12Desc backend = {};
  backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
  backend.device = device;
  ffxOverrideVersion override_version = {};
  override_version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
  override_version.versionId = version_id;
  desc.header.pNext = &version.header;
  version.header.pNext = &backend.header;
  if (version_id) {
    backend.header.pNext = &override_version.header;
  }
  ffxReturnCode_t result = ffx_create_(&context_, &desc.header, nullptr);
  if (result != FFX_API_RETURN_OK) {
    XELOGE("FSR: creating the upscaler failed ({})", result);
    context_ = nullptr;
    ffx_failed_ = true;
    return false;
  }
  context_size_[0] = render_width;
  context_size_[1] = render_height;
  context_size_[2] = display_width;
  context_size_[3] = display_height;

  ffxQueryGetProviderVersion provider = {};
  provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
  ffx_query_(&context_, &provider.header);
  version_name_ = provider.versionName ? provider.versionName : "?";

  ffxQueryDescUpscaleGetJitterPhaseCount phases = {};
  phases.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
  phases.renderWidth = render_width;
  phases.displayWidth = display_width;
  phases.pOutPhaseCount = &jitter_phase_count_;
  if (ffx_query_(&context_, &phases.header) != FFX_API_RETURN_OK ||
      jitter_phase_count_ <= 0) {
    jitter_phase_count_ = 8;
  }
  XELOGI("FSR: upscaler {} - {}x{} -> {}x{}, {} jitter phases", version_name_,
         render_width, render_height, display_width, display_height,
         jitter_phase_count_);
  reset_history_ = true;
  return true;
}

void DaoUpscalerImpl::DestroyContext() {
  if (!context_) {
    return;
  }
  // The context's resources may still be in use by submitted frames.
  cp_.AwaitAllQueueOperationsCompletion();
  ffx_destroy_(&context_, nullptr);
  context_ = nullptr;
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
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | extra_flags;
  // Zeroed (motion vectors are zero until something writes them).
  if (FAILED(provider.GetDevice()->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE,
          &desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
          IID_PPV_ARGS(&texture.resource)))) {
    XELOGE("FSR: failed to create the {} texture ({}x{})", name, width,
           height);
    texture = {};
    return false;
  }
  texture.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
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
  motion_pending_ = false;
  if (!jitter_active_) {
    return;
  }
  const auto surface = regs.Get<reg::RB_SURFACE_INFO>();
  // The scene: depth-tested multisampled draws (the depth prepass and both
  // tiles of the main pass; shadow maps and the UI are single-sampled).
  if (depth_control.z_enable &&
      surface.msaa_samples != xenos::MsaaSamples::k1X) {
    ++scene_draws_;
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
    motion_ndc_jitter_[0] =
        2.0f * jitter_[0] / float(viewport_info.xy_extent[0]);
    motion_ndc_jitter_[1] =
        -2.0f * jitter_[1] / float(viewport_info.xy_extent[1]);
    viewport_info.ndc_offset[0] += motion_ndc_jitter_[0];
    viewport_info.ndc_offset[1] += motion_ndc_jitter_[1];
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
      }
      motion.z_min = viewport_info.z_min;
      motion.z_max = viewport_info.z_max;
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
      return;
    }
  }
}

void DaoUpscalerImpl::AfterDraw(const Shader& vertex_shader, bool indexed,
                                uint32_t host_vertex_count, bool skip_motion) {
  if (hudless_pending_) {
    hudless_pending_ = false;
    CaptureHudless();
  }
  if (motion_pending_) {
    motion_pending_ = false;
    if (skip_motion) {
      ++motion_skipped_;
    } else {
      CaptureMotion(vertex_shader, indexed, host_vertex_count);
    }
  }
}

ID3D12PipelineState* DaoUpscalerImpl::GetStreamOutPipeline(
    const PipelineCache::Pipeline& pipeline) {
  const PipelineCache::PipelineRuntimeDescription& runtime =
      pipeline.description;
  auto it = stream_out_pipelines_.find(&pipeline);
  if (it != stream_out_pipelines_.end() &&
      it->second.vertex_shader == runtime.vertex_shader &&
      it->second.root_signature == runtime.root_signature) {
    return it->second.state.Get();
  }
  const PipelineCache::PipelineDescription& description = runtime.description;
  // The draw's vertex shader and root signature, writing the positions
  // instead of rasterizing. Render target formats and sample count as bound.
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
  desc.pRootSignature = runtime.root_signature;
  const std::vector<uint8_t>& vertex_shader =
      runtime.vertex_shader->translated_binary();
  desc.VS.pShaderBytecode = vertex_shader.data();
  desc.VS.BytecodeLength = vertex_shader.size();
  D3D12_SO_DECLARATION_ENTRY so_entry = {0, "SV_Position", 0, 0, 4, 0};
  UINT so_stride = 16;
  desc.StreamOutput.pSODeclaration = &so_entry;
  desc.StreamOutput.NumEntries = 1;
  desc.StreamOutput.pBufferStrides = &so_stride;
  desc.StreamOutput.NumStrides = 1;
  desc.StreamOutput.RasterizedStream = D3D12_SO_NO_RASTERIZED_STREAM;
  desc.SampleMask = UINT_MAX;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.DepthClipEnable = TRUE;
  switch (description.strip_cut_index) {
    case PipelineCache::PipelineStripCutIndex::kFFFF:
      desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
      break;
    case PipelineCache::PipelineStripCutIndex::kFFFFFFFF:
      desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
      break;
    default:
      desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
      break;
  }
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    const PipelineCache::PipelineRenderTarget& rt =
        description.render_targets[i];
    if (rt.used) {
      desc.NumRenderTargets = i + 1;
      desc.RTVFormats[i] = rtc_.GetColorDrawDXGIFormat(rt.format);
    }
  }
  desc.DSVFormat =
      D3D12RenderTargetCache::GetDepthDSVDXGIFormat(description.depth_format);
  const uint32_t samples = uint32_t(1)
                           << uint32_t(description.host_msaa_samples);
  if (samples == 2 && !rtc_.msaa_2x_supported()) {
    desc.SampleMask = 0b1001;
    desc.SampleDesc.Count = 4;
  } else {
    desc.SampleDesc.Count = samples;
  }
  StreamOutPipeline& entry = stream_out_pipelines_[&pipeline];
  entry.vertex_shader = runtime.vertex_shader;
  entry.root_signature = runtime.root_signature;
  entry.state.Reset();
  if (FAILED(cp_.GetD3D12Provider().GetDevice()->CreateGraphicsPipelineState(
          &desc, IID_PPV_ARGS(&entry.state)))) {
    XELOGW("FSR: no stream output pipeline for vertex shader {:016X}",
           runtime.vertex_shader->shader().ucode_data_hash());
    entry.state.Reset();
  }
  return entry.state.Get();
}

void DaoUpscalerImpl::TransitionStreamOut(D3D12_RESOURCE_STATES state) {
  cp_.PushTransitionBarrier(stream_out_buffer_.Get(), stream_out_state_,
                            state);
  stream_out_state_ = state;
}

void DaoUpscalerImpl::CaptureMotion(const Shader& vertex_shader, bool indexed,
                                    uint32_t host_vertex_count) {
  // Stream output writes triangle lists.
  uint32_t output_vertex_count;
  switch (cp_.primitive_topology_) {
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
      output_vertex_count = host_vertex_count / 3 * 3;
      break;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
      output_vertex_count =
          host_vertex_count >= 3 ? (host_vertex_count - 2) * 3 : 0;
      break;
    default:
      ++motion_skipped_;
      return;
  }
  if (!output_vertex_count) {
    return;
  }
  // Opaque draws with an ordinary vertex shader.
  const auto* pipeline =
      static_cast<const PipelineCache::Pipeline*>(cp_.current_guest_pipeline_);
  ID3D12PipelineState* pipeline_state =
      pipeline ? pipeline->state.load(std::memory_order_acquire) : nullptr;
  if (!pipeline_state) {
    ++motion_skipped_;
    return;
  }
  const PipelineCache::PipelineRuntimeDescription& runtime =
      pipeline->description;
  const PipelineCache::PipelineDescription& description = runtime.description;
  const PipelineCache::PipelineRenderTarget& rt0 =
      description.render_targets[0];
  const bool blended =
      rt0.src_blend != PipelineCache::PipelineBlendFactor::kOne ||
      rt0.dest_blend != PipelineCache::PipelineBlendFactor::kZero ||
      rt0.blend_op != xenos::BlendOp::kAdd;
  if (runtime.geometry_shader ||
      DxbcShaderTranslator::Modification(runtime.vertex_shader->modification())
              .vertex.host_vertex_shader_type !=
          Shader::HostVertexShaderType::kVertex ||
      description.zpd_total || description.viz_survey ||
      description.resolution_scale_native || !rt0.used || !rt0.write_mask ||
      (blended && !description.depth_write)) {
    ++motion_skipped_;
    return;
  }

  // The float constants as the vertex shader gets them.
  const RegisterFile& regs = rtc_.register_file();
  std::vector<uint32_t> constants;
  const Shader::ConstantRegisterMap& constant_map =
      vertex_shader.constant_register_map();
  for (uint32_t i = 0; i < 4; ++i) {
    uint64_t bits = constant_map.float_bitmap[i];
    while (bits) {
      const uint32_t index = uint32_t(std::countr_zero(bits));
      bits &= bits - 1;
      const uint32_t* source =
          &regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X + (i << 8) +
                       (index << 2)];
      constants.insert(constants.end(), source, source + 4);
    }
  }
  // Same shaders, vertex and index data and size: the same object.
  uint64_t key_data[5] = {vertex_shader.ucode_data_hash(), motion_pixel_shader_,
                          indexed ? regs.values[XE_GPU_REG_VGT_DMA_BASE] : 0,
                          0, host_vertex_count};
  if (!vertex_shader.vertex_bindings().empty()) {
    key_data[3] =
        regs.GetVertexFetch(vertex_shader.vertex_bindings()[0].fetch_constant)
            .address;
  }
  const uint64_t key = XXH3_64bits(key_data, sizeof(key_data));
  const uint64_t constants_hash =
      XXH3_64bits(constants.data(), constants.size() * sizeof(uint32_t));
  // Drawn again for the second tile.
  if (!motion_seen_.insert(key ^ (constants_hash * 0x9E3779B97F4A7C15ull))
           .second) {
    return;
  }

  // The previous frame's draw of it: the one with the closest constants.
  const MotionDraw* previous = nullptr;
  float best_distance = INFINITY;
  auto range = previous_motion_draws_.equal_range(key);
  for (auto it = range.first; it != range.second; ++it) {
    const MotionDraw& candidate = *it->second;
    if (candidate.constants.size() != constants.size()) {
      continue;
    }
    if (candidate.constants_hash == constants_hash) {
      previous = &candidate;
      break;
    }
    float distance = 0.0f;
    for (size_t i = 0; i < constants.size(); ++i) {
      float a, b;
      std::memcpy(&a, &constants[i], sizeof(float));
      std::memcpy(&b, &candidate.constants[i], sizeof(float));
      distance += std::abs(a - b);
    }
    if (distance < best_distance) {
      best_distance = distance;
      previous = &candidate;
    }
  }

  std::deque<MotionDraw>& draws = motion_draws_[motion_frame_];
  MotionDraw& draw = draws.emplace_back();
  draw.self = this;
  draw.key = key;
  draw.constants_hash = constants_hash;
  draw.constants = std::move(constants);
  draw.output_vertex_count = 0;
  if (!previous) {
    // New this frame: motion unknown, left at zero (kept for the next frame).
    ++motion_unmatched_;
    return;
  }

  const uint32_t output_size =
      (output_vertex_count * 16 + 255) & ~uint32_t(255);
  ID3D12PipelineState* stream_out_pipeline = GetStreamOutPipeline(*pipeline);
  if (!stream_out_pipeline || output_vertex_count > (1u << 20) ||
      stream_out_used_ + uint64_t(output_size) * 2 > kStreamOutDataSize ||
      stream_out_counters_used_ + 2 > kStreamOutCounterCount) {
    ++motion_skipped_;
    return;
  }

  // Constants: the previous frame's floats, and the system constants without
  // the jitter (for both passes).
  D3D12_GPU_VIRTUAL_ADDRESS previous_float_constants;
  D3D12_GPU_VIRTUAL_ADDRESS unjittered_system_constants;
  ui::d3d12::D3D12UploadBufferPool& pool = *cp_.constant_buffer_pool_;
  uint8_t* mapping = pool.Request(
      cp_.frame_current_,
      std::max(previous->constants.size() * sizeof(uint32_t), size_t(16)),
      D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
      &previous_float_constants);
  if (!mapping) {
    return;
  }
  std::memcpy(mapping, previous->constants.data(),
              previous->constants.size() * sizeof(uint32_t));
  DxbcShaderTranslator::SystemConstants system_constants =
      cp_.system_constants_;
  system_constants.ndc_offset[0] -= motion_ndc_jitter_[0];
  system_constants.ndc_offset[1] -= motion_ndc_jitter_[1];
  mapping = pool.Request(cp_.frame_current_, sizeof(system_constants),
                         D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
                         nullptr, nullptr, &unjittered_system_constants);
  if (!mapping) {
    return;
  }
  std::memcpy(mapping, &system_constants, sizeof(system_constants));

  if (!stream_out_reset_) {
    // Zero the filled-size counters, once per frame.
    TransitionStreamOut(D3D12_RESOURCE_STATE_COPY_DEST);
    cp_.SubmitBarriers();
    cp_.GetDeferredCommandList().D3DCopyBufferRegion(
        stream_out_buffer_.Get(), kStreamOutDataSize, stream_out_zeros_.Get(),
        0, kStreamOutCounterSize);
    stream_out_reset_ = true;
  }
  TransitionStreamOut(D3D12_RESOURCE_STATE_STREAM_OUT);
  cp_.SubmitBarriers();

  const bool bindless = cp_.bindless_resources_used_;
  draw.stream_out_pipeline = stream_out_pipeline;
  draw.pipeline = pipeline_state;
  draw.float_parameter =
      bindless
          ? D3D12CommandProcessor::kRootParameter_Bindless_FloatConstantsVertex
          : D3D12CommandProcessor::kRootParameter_Bindful_FloatConstantsVertex;
  draw.system_parameter =
      bindless ? D3D12CommandProcessor::kRootParameter_Bindless_SystemConstants
               : D3D12CommandProcessor::kRootParameter_Bindful_SystemConstants;
  draw.float_constants = cp_.cbuffer_binding_float_vertex_.address;
  draw.previous_float_constants = previous_float_constants;
  draw.system_constants = cp_.cbuffer_binding_system_.address;
  draw.unjittered_system_constants = unjittered_system_constants;
  draw.indexed = indexed;
  draw.host_vertex_count = host_vertex_count;
  draw.output_vertex_count = output_vertex_count;
  draw.output_size = output_size;
  draw.motion = motion_viewport_;
  draw.motion.current_offset = stream_out_used_;
  draw.motion.previous_offset = stream_out_used_ + output_size;
  draw.motion.current_counter =
      kStreamOutDataSize + stream_out_counters_used_ * 16;
  draw.motion.previous_counter = draw.motion.current_counter + 16;
  stream_out_used_ += output_size * 2;
  stream_out_counters_used_ += 2;
  cp_.GetDeferredCommandList().DaoCallback(ExecuteStreamOut, &draw);
}

void DaoUpscalerImpl::ExecuteStreamOut(
    void* context, ID3D12GraphicsCommandList* command_list) {
  const MotionDraw& draw = *static_cast<const MotionDraw*>(context);
  const D3D12_GPU_VIRTUAL_ADDRESS buffer =
      draw.self->stream_out_buffer_->GetGPUVirtualAddress();
  command_list->SetPipelineState(draw.stream_out_pipeline);
  command_list->SetGraphicsRootConstantBufferView(
      draw.system_parameter, draw.unjittered_system_constants);
  for (uint32_t pass = 0; pass < 2; ++pass) {
    command_list->SetGraphicsRootConstantBufferView(
        draw.float_parameter,
        pass ? draw.previous_float_constants : draw.float_constants);
    D3D12_STREAM_OUTPUT_BUFFER_VIEW view;
    view.BufferLocation = buffer + (pass ? draw.motion.previous_offset
                                         : draw.motion.current_offset);
    view.SizeInBytes = draw.output_size;
    view.BufferFilledSizeLocation =
        buffer + (pass ? draw.motion.previous_counter
                       : draw.motion.current_counter);
    command_list->SOSetTargets(0, 1, &view);
    if (draw.indexed) {
      command_list->DrawIndexedInstanced(draw.host_vertex_count, 1, 0, 0, 0);
    } else {
      command_list->DrawInstanced(draw.host_vertex_count, 1, 0, 0);
    }
  }
  command_list->SOSetTargets(0, 1, nullptr);
  // Back to the draw's state.
  command_list->SetGraphicsRootConstantBufferView(draw.float_parameter,
                                                  draw.float_constants);
  command_list->SetGraphicsRootConstantBufferView(draw.system_parameter,
                                                  draw.system_constants);
  command_list->SetPipelineState(draw.pipeline);
}

void DaoUpscalerImpl::RecordMotionPass(uint32_t width, uint32_t height) {
  const std::deque<MotionDraw>& draws = motion_draws_[motion_frame_];
  ID3D12Device* device = cp_.GetD3D12Provider().GetDevice();
  D3D12_RENDER_TARGET_VIEW_DESC rtv = {};
  rtv.Format = motion_vectors_.format;
  rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
  device->CreateRenderTargetView(
      motion_vectors_.resource.Get(), &rtv,
      motion_rtv_heap_->GetCPUDescriptorHandleForHeapStart());

  // Without positions, only cleared to zero.
  motion_pass_ = {};
  motion_pass_.self = this;
  motion_pass_.width = width;
  motion_pass_.height = height;
  ui::d3d12::util::DescriptorCpuGpuHandlePair descriptors[2];
  if (stream_out_used_ &&
      cp_.RequestOneUseSingleViewDescriptors(2, descriptors)) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = kStreamOutBufferSize / sizeof(uint32_t);
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device->CreateShaderResourceView(stream_out_buffer_.Get(), &srv,
                                     descriptors[0].first);
    srv = {};
    srv.Format = depth_.format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(depth_.resource.Get(), &srv,
                                     descriptors[1].first);
    motion_pass_.draws = &draws;
    motion_pass_.positions = descriptors[0].second;
    motion_pass_.depth = descriptors[1].second;
    TransitionStreamOut(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(depth_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  Transition(motion_vectors_, D3D12_RESOURCE_STATE_RENDER_TARGET);
  cp_.SubmitBarriers();
  cp_.GetDeferredCommandList().DaoCallback(ExecuteMotionPass, &motion_pass_);
  InvalidateCommandProcessorState();
}

void DaoUpscalerImpl::ExecuteMotionPass(
    void* context, ID3D12GraphicsCommandList* command_list) {
  const MotionPass& pass = *static_cast<const MotionPass*>(context);
  DaoUpscalerImpl& self = *pass.self;
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv =
      self.motion_rtv_heap_->GetCPUDescriptorHandleForHeapStart();
  command_list->OMSetRenderTargets(1, &rtv, TRUE, nullptr);
  const float zero[4] = {};
  command_list->ClearRenderTargetView(rtv, zero, 0, nullptr);
  if (!pass.draws) {
    return;
  }
  const D3D12_VIEWPORT viewport = {
      0.0f, 0.0f, float(pass.width), float(pass.height), 0.0f, 1.0f};
  const D3D12_RECT scissor = {0, 0, LONG(pass.width), LONG(pass.height)};
  command_list->RSSetViewports(1, &viewport);
  command_list->RSSetScissorRects(1, &scissor);
  command_list->SetGraphicsRootSignature(self.motion_root_signature_.Get());
  command_list->SetPipelineState(self.motion_pipeline_.Get());
  command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  command_list->SetGraphicsRootDescriptorTable(1, pass.positions);
  command_list->SetGraphicsRootDescriptorTable(2, pass.depth);
  for (const MotionDraw& draw : *pass.draws) {
    if (!draw.output_vertex_count) {
      continue;
    }
    command_list->SetGraphicsRoot32BitConstants(
        0, sizeof(MotionConstants) / sizeof(uint32_t), &draw.motion, 0);
    command_list->DrawInstanced(draw.output_vertex_count, 1, 0, 0);
  }
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
  if (!jitter_active_ || !resolve_info.copy_dest_extent_length ||
      !render_size_[0]) {
    return;
  }
  const RegisterFile& regs = rtc_.register_file();
  if (regs.Get<reg::RB_SURFACE_INFO>().msaa_samples ==
      xenos::MsaaSamples::k1X) {
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

void DaoUpscalerImpl::ExecuteDispatch(void* context,
                                      ID3D12GraphicsCommandList* command_list) {
  auto& params = *static_cast<DispatchParams*>(context);
  DaoUpscalerImpl& self = *params.self;
  if (!self.context_) {
    return;
  }
  params.desc.commandList = command_list;
  ffxReturnCode_t result = self.ffx_dispatch_(&self.context_, &params.desc.header);
  if (result != FFX_API_RETURN_OK) {
    XELOGE("FSR: dispatch failed ({})", result);
  }
}

ID3D12Resource* DaoUpscalerImpl::OnSwap(
    ID3D12Resource* frontbuffer,
    const D3D12_SHADER_RESOURCE_VIEW_DESC& frontbuffer_srv,
    uint32_t render_width, uint32_t render_height,
    D3D12_SHADER_RESOURCE_VIEW_DESC& srv_out) {
  const auto now = std::chrono::steady_clock::now();
  const float frame_ms =
      std::chrono::duration<float, std::milli>(now - last_swap_).count();
  last_swap_ = now;

  const bool size_changed =
      render_size_[0] != render_width || render_size_[1] != render_height;
  render_size_[0] = render_width;
  render_size_[1] = render_height;

  const char* reason = nullptr;
  if (!jitter_active_) {
    reason = "off";
  } else if (size_changed) {
    reason = "render size changed";
  } else if (!scene_draws_) {
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
        !EnsureTexture(composite_, DXGI_FORMAT_R8G8B8A8_UNORM, display_width,
                       display_height, "composite")) {
      reason = "upscaler unavailable";
    }
  }
  if (reason) {
    EndFrame(false, reason);
    return nullptr;
  }

  // Motion vectors, then FSR, recorded into the command list when it's
  // executed.
  if (motion_available_) {
    RecordMotionPass(render_width, render_height);
  }
  Transition(color_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(depth_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(motion_vectors_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(upscaled_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  cp_.SubmitBarriers();
  ffxDispatchDescUpscale& desc = dispatch_params_.desc;
  desc = {};
  desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
  desc.color = ffxApiGetResourceDX12(color_.resource.Get());
  desc.depth = ffxApiGetResourceDX12(depth_.resource.Get());
  desc.motionVectors = ffxApiGetResourceDX12(motion_vectors_.resource.Get());
  desc.output = ffxApiGetResourceDX12(upscaled_.resource.Get(),
                                      FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
  desc.jitterOffset = {jitter_[0], jitter_[1]};
  desc.motionVectorScale = {1.0f, 1.0f};
  desc.renderSize = {render_width, render_height};
  desc.upscaleSize = {display_width, display_height};
  desc.enableSharpening = cvars::dao_temporal_sharpness > 0.0;
  desc.sharpness =
      float(std::clamp(cvars::dao_temporal_sharpness, 0.0, 1.0));
  desc.frameTimeDelta = std::clamp(frame_ms, 1.0f, 100.0f);
  desc.preExposure = 1.0f;
  desc.reset = reset_history_;
  // Rough camera values; FSR uses them for its depth-based heuristics.
  desc.cameraNear = 0.1f;
  desc.cameraFar = 1000.0f;
  desc.cameraFovAngleVertical = 1.0f;
  desc.viewSpaceToMetersFactor = 1.0f;
  desc.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
  dispatch_params_.self = this;
  cp_.GetDeferredCommandList().DaoCallback(ExecuteDispatch, &dispatch_params_);
  InvalidateCommandProcessorState();

  // Composite the UI.
  ID3D12Device* device = cp_.GetD3D12Provider().GetDevice();
  ui::d3d12::util::DescriptorCpuGpuHandlePair descriptors[5];
  if (!cp_.RequestOneUseSingleViewDescriptors(5, descriptors)) {
    EndFrame(false, "no descriptors");
    return nullptr;
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
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
  uav.Format = composite_.format;
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  device->CreateUnorderedAccessView(composite_.resource.Get(), nullptr, &uav,
                                    descriptors[4].first);
  Transition(upscaled_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(composite_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  cp_.SubmitBarriers();
  DeferredCommandList& command_list = cp_.GetDeferredCommandList();
  command_list.D3DSetComputeRootSignature(composite_root_signature_.Get());
  CompositeConstants constants;
  constants.dest_size[0] = display_width;
  constants.dest_size[1] = display_height;
  constants.render_size[0] = float(render_width);
  constants.render_size[1] = float(render_height);
  constants.show_motion = cvars::dao_show_motion_vectors ? 1 : 0;
  command_list.D3DSetComputeRoot32BitConstants(
      0, sizeof(constants) / sizeof(uint32_t), &constants, 0);
  for (uint32_t i = 0; i < 5; ++i) {
    command_list.D3DSetComputeRootDescriptorTable(1 + i,
                                                  descriptors[i].second);
  }
  cp_.SetExternalPipeline(composite_pipeline_.Get());
  command_list.D3DDispatch((display_width + 7) / 8, (display_height + 7) / 8,
                           1);
  Transition(composite_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  srv_out = {};
  srv_out.Format = composite_.format;
  srv_out.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv_out.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv_out.Texture2D.MipLevels = 1;
  EndFrame(true, nullptr);
  return composite_.resource.Get();
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
  // If a 3D scene couldn't be upscaled, stop jittering it for a while (a few
  // jittered frames shown as they are look like a small shake).
  const bool enabled = cvars::dao_temporal_upscaler != "off";
  if (scene_draws_ && !upscaled && jitter_active_) {
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
  jitter_active_ = enabled && !jitter_cooldown_;

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
  std::deque<MotionDraw>& motion_draws = motion_draws_[motion_frame_];
  if (upscaled && !motion_logged_ && !previous_motion_draws_.empty()) {
    motion_logged_ = true;
    XELOGI(
        "FSR: motion vectors for {} draws ({} new, {} skipped), {} KB of "
        "positions",
        motion_draws.size() - motion_unmatched_, motion_unmatched_,
        motion_skipped_, stream_out_used_ >> 10);
  }
  // This frame's draws are the previous ones for the next frame (the commands
  // reading them are executed at this swap).
  previous_motion_draws_.clear();
  for (const MotionDraw& draw : motion_draws) {
    previous_motion_draws_.emplace(draw.key, &draw);
  }
  motion_frame_ ^= 1;
  motion_draws_[motion_frame_].clear();
  motion_seen_.clear();
  stream_out_used_ = 0;
  stream_out_counters_used_ = 0;
  stream_out_reset_ = false;
  motion_unmatched_ = 0;
  motion_skipped_ = 0;
  motion_pending_ = false;

  scene_draws_ = 0;
  depth_tiles_ = 0;
  scene_texture_ = 0;
  scene_depth_pending_ = false;
  scene_depth_target_ = nullptr;
  hudless_pending_ = false;
  hudless_captured_ = false;
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
}

void DaoUpscaler::Shutdown() {
  delete impl_;
  impl_ = nullptr;
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

void DaoUpscaler::AfterDraw(const Shader& vertex_shader, bool indexed,
                            uint32_t host_vertex_count, bool skip_motion) {
  if (impl_) {
    impl_->AfterDraw(vertex_shader, indexed, host_vertex_count, skip_motion);
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
    D3D12_SHADER_RESOURCE_VIEW_DESC& srv_out) {
  return impl_ ? impl_->OnSwap(frontbuffer, frontbuffer_srv, frontbuffer_width,
                               frontbuffer_height, srv_out)
               : nullptr;
}

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe
