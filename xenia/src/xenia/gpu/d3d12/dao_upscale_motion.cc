// DAO: temporal upscaling's motion vectors (stream output of the scene draws,
// the motion vector pass) - see dao_upscale.h.
#include "xenia/gpu/d3d12/dao_upscale_impl.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <emmintrin.h>
#include <span>

#include "xenia/base/logging.h"
#include "xenia/base/threading.h"
#include "xenia/base/xxhash.h"
#include "xenia/gpu/d3d12/d3d12_command_processor.h"
#include "xenia/gpu/dxbc_shader_translator.h"

namespace xe {
namespace gpu {
namespace d3d12 {

namespace shaders {
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/dao_upscale_mv_depth_ps.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/dao_upscale_mv_ps.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/dao_upscale_mv_vs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/fullscreen_cw_vs.h"
}  // namespace shaders

namespace {

// Stream output of the scene draws' positions (float4 per vertex, each draw in a
// part of its own), followed by the filled-size counters (one per draw, 16
// bytes apart). One for each of the last two frames: a draw's previous
// positions are those its match wrote in the previous frame.
constexpr uint32_t kStreamOutDataSize = 96 << 20;
constexpr uint32_t kStreamOutCounterCount = 16384;
constexpr uint32_t kStreamOutCounterSize = kStreamOutCounterCount * 16;
constexpr uint32_t kStreamOutBufferSize =
    kStreamOutDataSize + kStreamOutCounterSize;

// Sum of the floats, in doubles (8 at a time, in an order of its own).
double SumFloats(const uint32_t* values, size_t count) {
  const float* floats = reinterpret_cast<const float*>(values);
  __m128d sums[4] = {_mm_setzero_pd(), _mm_setzero_pd(), _mm_setzero_pd(),
                     _mm_setzero_pd()};
  size_t i = 0;
  for (; i + 8 <= count; i += 8) {
    const __m128 low = _mm_loadu_ps(floats + i);
    const __m128 high = _mm_loadu_ps(floats + i + 4);
    sums[0] = _mm_add_pd(sums[0], _mm_cvtps_pd(low));
    sums[1] = _mm_add_pd(sums[1], _mm_cvtps_pd(_mm_movehl_ps(low, low)));
    sums[2] = _mm_add_pd(sums[2], _mm_cvtps_pd(high));
    sums[3] = _mm_add_pd(sums[3], _mm_cvtps_pd(_mm_movehl_ps(high, high)));
  }
  const __m128d sum = _mm_add_pd(_mm_add_pd(sums[0], sums[1]),
                                 _mm_add_pd(sums[2], sums[3]));
  double total = _mm_cvtsd_f64(sum) + _mm_cvtsd_f64(_mm_unpackhi_pd(sum, sum));
  for (; i < count; ++i) {
    total += double(floats[i]);
  }
  return total;
}

float HorizontalSum(__m128 v) {
  const __m128 pairs = _mm_add_ps(v, _mm_movehl_ps(v, v));
  return _mm_cvtss_f32(
      _mm_add_ss(pairs, _mm_shuffle_ps(pairs, pairs, _MM_SHUFFLE(1, 1, 1, 1))));
}

// Sum of the absolute differences of the floats; stops (with the sum so far)
// once it's not under `limit`, checked every 16 floats.
float FloatDistance(const uint32_t* a, const uint32_t* b, size_t count,
                    float limit) {
  const float* fa = reinterpret_cast<const float*>(a);
  const float* fb = reinterpret_cast<const float*>(b);
  const __m128 abs_mask = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF));
  auto difference = [&](size_t i) {
    return _mm_and_ps(abs_mask,
                      _mm_sub_ps(_mm_loadu_ps(fa + i), _mm_loadu_ps(fb + i)));
  };
  __m128 sum = _mm_setzero_ps();
  size_t i = 0;
  for (; i + 16 <= count; i += 16) {
    sum = _mm_add_ps(sum, _mm_add_ps(_mm_add_ps(difference(i),
                                                difference(i + 4)),
                                     _mm_add_ps(difference(i + 8),
                                                difference(i + 12))));
    const float total = HorizontalSum(sum);
    if (!(total < limit)) {
      return total;
    }
  }
  for (; i + 4 <= count; i += 4) {
    sum = _mm_add_ps(sum, difference(i));
  }
  float total = HorizontalSum(sum);
  for (; i < count; ++i) {
    total += std::abs(fa[i] - fb[i]);
  }
  return total;
}

}  // namespace

bool DaoUpscalerImpl::InitializeMotion() {
  const ui::d3d12::D3D12Provider& provider = cp_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  // Constants, current and previous positions (raw buffers), scene depth.
  D3D12_DESCRIPTOR_RANGE ranges[3] = {};
  for (uint32_t i = 0; i < 3; ++i) {
    ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[i].NumDescriptors = 1;
    ranges[i].BaseShaderRegister = i;
  }
  D3D12_ROOT_PARAMETER parameters[4] = {};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants.Num32BitValues =
      sizeof(MotionConstants) / sizeof(uint32_t);
  for (uint32_t i = 0; i < 3; ++i) {
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
  desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
  desc.SampleDesc.Count = 1;
  if (FAILED(device->CreateGraphicsPipelineState(
          &desc, IID_PPV_ARGS(
                     &motion_pipelines_[kMotionPipelineNoDepthTest])))) {
    return false;
  }
  // The depth test rejecting what is behind the scene depth, after moving the
  // triangles towards the camera by twice the larger of their x and y slopes
  // (the pixel shader allows for their sum). Not clipped by the moved depth,
  // but clamped to the viewport's range.
  desc.DepthStencilState.DepthEnable = TRUE;
  desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
  desc.RasterizerState.DepthClipEnable = FALSE;
  for (uint32_t i = kMotionPipelineNearerLess; i <= kMotionPipelineNearerGreater;
       ++i) {
    const bool nearer_less = i == kMotionPipelineNearerLess;
    desc.DepthStencilState.DepthFunc = nearer_less
                                           ? D3D12_COMPARISON_FUNC_LESS_EQUAL
                                           : D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    desc.RasterizerState.SlopeScaledDepthBias = nearer_less ? -2.0f : 2.0f;
    if (FAILED(device->CreateGraphicsPipelineState(
            &desc, IID_PPV_ARGS(&motion_pipelines_[i])))) {
      return false;
    }
  }
  desc.VS.pShaderBytecode = shaders::fullscreen_cw_vs;
  desc.VS.BytecodeLength = sizeof(shaders::fullscreen_cw_vs);
  desc.PS.pShaderBytecode = shaders::dao_upscale_mv_depth_ps;
  desc.PS.BytecodeLength = sizeof(shaders::dao_upscale_mv_depth_ps);
  // Writes only depth.
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
  desc.RasterizerState.SlopeScaledDepthBias = 0.0f;
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  if (FAILED(device->CreateGraphicsPipelineState(
          &desc, IID_PPV_ARGS(&motion_depth_pipeline_)))) {
    return false;
  }

  D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  heap_desc.NumDescriptors = 1;
  if (FAILED(device->CreateDescriptorHeap(&heap_desc,
                                          IID_PPV_ARGS(&motion_rtv_heap_)))) {
    return false;
  }
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  if (FAILED(device->CreateDescriptorHeap(&heap_desc,
                                          IID_PPV_ARGS(&motion_dsv_heap_)))) {
    return false;
  }

  return true;
}

bool DaoUpscalerImpl::EnsureStreamOutBuffers() {
  if (stream_out_zeros_) {
    return true;
  }
  if (stream_out_buffers_failed_) {
    return false;
  }
  ID3D12Device* device = cp_.GetD3D12Provider().GetDevice();
  ComPtr<ID3D12Resource> buffers[2], zeros;
  D3D12_RESOURCE_DESC buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(buffer_desc, kStreamOutBufferSize,
                                          D3D12_RESOURCE_FLAG_NONE);
  bool created = true;
  for (ComPtr<ID3D12Resource>& buffer : buffers) {
    created = created && SUCCEEDED(device->CreateCommittedResource(
                             &ui::d3d12::util::kHeapPropertiesDefault,
                             D3D12_HEAP_FLAG_NONE, &buffer_desc,
                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                             IID_PPV_ARGS(&buffer)));
  }
  ui::d3d12::util::FillBufferResourceDesc(buffer_desc, kStreamOutCounterSize,
                                          D3D12_RESOURCE_FLAG_NONE);
  void* mapping;
  D3D12_RANGE read_range = {};
  created = created &&
            SUCCEEDED(device->CreateCommittedResource(
                &ui::d3d12::util::kHeapPropertiesUpload, D3D12_HEAP_FLAG_NONE,
                &buffer_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&zeros))) &&
            SUCCEEDED(zeros->Map(0, &read_range, &mapping));
  if (!created) {
    XELOGE("FSR: failed to create the motion vector stream output buffers");
    stream_out_buffers_failed_ = true;
    return false;
  }
  std::memset(mapping, 0, kStreamOutCounterSize);
  zeros->Unmap(0, nullptr);
  for (uint32_t i = 0; i < 2; ++i) {
    stream_out_buffers_[i] = std::move(buffers[i]);
    stream_out_states_[i] = D3D12_RESOURCE_STATE_COPY_DEST;
  }
  stream_out_zeros_ = std::move(zeros);
  return true;
}

ID3D12PipelineState* DaoUpscalerImpl::GetStreamOutPipeline(
    const PipelineCache::Pipeline& pipeline) {
  const PipelineCache::PipelineRuntimeDescription& runtime =
      pipeline.description;
  std::unique_ptr<StreamOutPipeline>& entry = stream_out_pipelines_[&pipeline];
  if (entry && entry->vertex_shader == runtime.vertex_shader &&
      entry->root_signature == runtime.root_signature) {
    return entry->status.load(std::memory_order_acquire) ==
                   StreamOutPipeline::kReady
               ? entry->state.Get()
               : nullptr;
  }
  if (entry) {
    stream_out_pipelines_old_.push_back(std::move(entry));
  }
  entry = std::make_unique<StreamOutPipeline>();
  entry->vertex_shader = runtime.vertex_shader;
  entry->vertex_shader_hash = runtime.vertex_shader->shader().ucode_data_hash();
  entry->root_signature = runtime.root_signature;
  entry->pipeline = &pipeline;
  if (!stream_out_thread_.joinable()) {
    stream_out_thread_ = std::thread([this] { StreamOutPipelineThread(); });
  }
  {
    std::lock_guard<std::mutex> lock(stream_out_mutex_);
    stream_out_queue_.push_back(entry.get());
  }
  stream_out_cv_.notify_one();
  return nullptr;
}

void DaoUpscalerImpl::StreamOutPipelineThread() {
  xe::threading::set_name("FSR stream output pipelines");
  while (true) {
    StreamOutPipeline* entry;
    {
      std::unique_lock<std::mutex> lock(stream_out_mutex_);
      stream_out_cv_.wait(lock, [this] {
        return stream_out_shutdown_ || !stream_out_queue_.empty();
      });
      if (stream_out_shutdown_) {
        return;
      }
      entry = stream_out_queue_.front();
      stream_out_queue_.pop_front();
    }
    // The draw's pipeline, also writing the positions (SV_Position).
    static const D3D12_SO_DECLARATION_ENTRY kSoEntry = {0, "SV_Position", 0,
                                                        0, 4, 0};
    static const UINT kSoStride = 16;
    D3D12_STREAM_OUTPUT_DESC stream_output = {};
    stream_output.pSODeclaration = &kSoEntry;
    stream_output.NumEntries = 1;
    stream_output.pBufferStrides = &kSoStride;
    stream_output.NumStrides = 1;
    stream_output.RasterizedStream = 0;
    entry->state.Attach(cp_.pipeline_cache_->CreateD3D12Pipeline(
        entry->pipeline->description, &stream_output));
    if (entry->state) {
      entry->status.store(StreamOutPipeline::kReady, std::memory_order_release);
    } else {
      XELOGW("FSR: no stream output pipeline for vertex shader {:016X}",
             entry->vertex_shader_hash);
      entry->state.Reset();
      entry->status.store(StreamOutPipeline::kFailed,
                          std::memory_order_release);
    }
  }
}

void DaoUpscalerImpl::TransitionStreamOut(uint32_t buffer,
                                          D3D12_RESOURCE_STATES state) {
  cp_.PushTransitionBarrier(stream_out_buffers_[buffer].Get(),
                            stream_out_states_[buffer], state);
  stream_out_states_[buffer] = state;
}

bool DaoUpscalerImpl::CaptureMotion(const Shader& vertex_shader, bool indexed,
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
      return false;
  }
  if (!output_vertex_count) {
    return false;
  }
  // Opaque draws with an ordinary vertex shader.
  const auto* pipeline =
      static_cast<const PipelineCache::Pipeline*>(cp_.current_guest_pipeline_);
  ID3D12PipelineState* pipeline_state =
      pipeline ? pipeline->state.load(std::memory_order_acquire) : nullptr;
  if (!pipeline_state) {
    ++motion_skipped_;
    return false;
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
    return false;
  }

  // Requested even if not needed this frame, to be ready for the next.
  ID3D12PipelineState* stream_out_pipeline = GetStreamOutPipeline(*pipeline);

  // The float constants as the vertex shader gets them.
  const RegisterFile& regs = rtc_.register_file();
  std::vector<uint32_t>& constants = motion_constants_;
  const Shader::ConstantRegisterMap& constant_map =
      vertex_shader.constant_register_map();
  size_t register_count = 0;
  for (uint64_t bits : constant_map.float_bitmap) {
    register_count += size_t(std::popcount(bits));
  }
  constants.resize(register_count * 4);
  uint32_t* out = constants.data();
  // Runs of consecutive registers at once.
  for (uint32_t i = 0; i < 4; ++i) {
    uint64_t bits = constant_map.float_bitmap[i];
    while (bits) {
      const uint32_t index = uint32_t(std::countr_zero(bits));
      const uint32_t run = uint32_t(std::countr_one(bits >> index));
      bits &= index + run >= 64 ? 0 : ~uint64_t(0) << (index + run);
      std::memcpy(out,
                  &regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X + (i << 8) +
                               (index << 2)],
                  run * 4 * sizeof(uint32_t));
      out += run * 4;
    }
  }
  // Same shaders, vertex and index data, size and primitive type (and strip
  // cuts): the same object, with its positions written the same way.
  const bool strip_cuts = description.strip_cut_index !=
                          PipelineCache::PipelineStripCutIndex::kNone;
  uint64_t key_data[5] = {
      vertex_shader.ucode_data_hash(), motion_pixel_shader_,
      indexed ? regs.values[XE_GPU_REG_VGT_DMA_BASE] : 0, 0,
      host_vertex_count | (uint64_t(cp_.primitive_topology_) << 32) |
          (uint64_t(strip_cuts) << 40)};
  if (!vertex_shader.vertex_bindings().empty()) {
    key_data[3] =
        regs.GetVertexFetch(vertex_shader.vertex_bindings()[0].fetch_constant)
            .address;
  }
  const uint64_t key = XXH3_64bits(key_data, sizeof(key_data));
  const uint64_t constants_hash =
      XXH3_64bits(constants.data(), constants.size() * sizeof(uint32_t));
  // Drawn again for the second tile.
  if (!motion_seen_.insert(key ^ (constants_hash * 0x9E3779B97F4A7C15ull))) {
    return false;
  }

  // The previous frame's draw of it: the one with the closest constants (sum
  // of absolute differences). Searched from the closest constants_sum
  // outwards, until the difference of the sums alone is too much.
  double constants_sum = SumFloats(constants.data(), constants.size());
  if (!std::isfinite(constants_sum)) {
    constants_sum = INFINITY;
  }
  const MotionDraw* previous = nullptr;
  const auto first = std::lower_bound(
      previous_motion_draws_.begin(), previous_motion_draws_.end(), key,
      [](const MotionCandidate& candidate, uint64_t key) {
        return candidate.key < key;
      });
  const auto last = std::upper_bound(
      first, previous_motion_draws_.end(), key,
      [](uint64_t key, const MotionCandidate& candidate) {
        return key < candidate.key;
      });
  if (first != last) {
    const std::span<const MotionCandidate> candidates(first, last);
    const uint32_t* previous_constants =
        motion_constant_data_[motion_frame_ ^ 1].data();
    float best_distance = INFINITY;
    // Takes the candidate if it's the closest so far (stops summing past the
    // closest), among those that wrote positions.
    auto try_candidate = [&](const MotionDraw& candidate) {
      if (candidate.constants_count != constants.size() ||
          !candidate.output_vertex_count) {
        return;
      }
      if (candidate.constants_hash == constants_hash) {
        previous = &candidate;
        best_distance = 0.0f;
        return;
      }
      const float distance = FloatDistance(
          constants.data(), previous_constants + candidate.constants_offset,
          constants.size(), best_distance);
      if (distance < best_distance) {
        best_distance = distance;
        previous = &candidate;
      }
    };
    if (constants_sum == INFINITY) {
      // Only the same constants (the distance to any others isn't finite).
      for (const MotionCandidate& candidate : candidates) {
        if (candidate.draw->constants_hash == constants_hash) {
          try_candidate(*candidate.draw);
          if (previous) {
            break;
          }
        }
      }
    } else {
      size_t above = size_t(
          std::lower_bound(candidates.begin(), candidates.end(), constants_sum,
                           [](const MotionCandidate& candidate,
                              double sum) {
                             return candidate.constants_sum < sum;
                           }) -
          candidates.begin());
      size_t below = above;
      // With some slack for the rounding of the float distances.
      auto out_of_reach = [&](const MotionCandidate& candidate) {
        return std::abs(candidate.constants_sum - constants_sum) >
               double(best_distance) * 1.001 + 1e-3;
      };
      while (best_distance > 0.0f) {
        const bool up =
            above < candidates.size() && !out_of_reach(candidates[above]);
        const bool down = below > 0 && !out_of_reach(candidates[below - 1]);
        if (!up && !down) {
          break;
        }
        // The nearer sum first.
        if (up && (!down || candidates[above].constants_sum - constants_sum <=
                                constants_sum -
                                    candidates[below - 1].constants_sum)) {
          try_candidate(*candidates[above++].draw);
        } else {
          try_candidate(*candidates[--below].draw);
        }
      }
    }
  }

  MotionDraw& draw = motion_draws_[motion_frame_].emplace_back();
  std::vector<uint32_t>& constant_data = motion_constant_data_[motion_frame_];
  draw.self = this;
  draw.key = key;
  draw.constants_hash = constants_hash;
  draw.constants_offset = uint32_t(constant_data.size());
  draw.constants_count = uint32_t(constants.size());
  constant_data.insert(constant_data.end(), constants.begin(),
                       constants.end());
  draw.constants_sum = constants_sum;
  draw.output_vertex_count = 0;
  draw.has_motion = false;
  // The positions are written for the next frame either way (unless skipped
  // below), but without the previous ones (new this frame, or they weren't
  // written), the motion is unknown and left at zero.

  // Each draw writes to a part of the buffer and a filled-size counter of its
  // own, bound for that draw only (unbound in AfterDraw). Appending draws to
  // one buffer left bound was faster, but on AMD the GPU could hang when
  // another program's GPU work preempted this one's between those draws.
  const uint32_t size = (output_vertex_count * 16 + 255) & ~uint32_t(255);
  const uint32_t offset = (stream_out_used_ + 255) & ~uint32_t(255);
  if (!stream_out_pipeline || !EnsureStreamOutBuffers() ||
      output_vertex_count > (1u << 20) ||
      offset + uint64_t(size) > kStreamOutDataSize ||
      stream_out_counters_used_ >= kStreamOutCounterCount) {
    ++motion_skipped_;
    return false;
  }

  if (!stream_out_reset_) {
    // Zero the filled-size counters, once per frame.
    TransitionStreamOut(motion_frame_, D3D12_RESOURCE_STATE_COPY_DEST);
    cp_.SubmitBarriers();
    cp_.GetDeferredCommandList().D3DCopyBufferRegion(
        stream_out_buffers_[motion_frame_].Get(), kStreamOutDataSize,
        stream_out_zeros_.Get(), 0, kStreamOutCounterSize);
    stream_out_reset_ = true;
  }
  TransitionStreamOut(motion_frame_, D3D12_RESOURCE_STATE_STREAM_OUT);
  cp_.SubmitBarriers();

  draw.stream_out_pipeline = stream_out_pipeline;
  draw.output_vertex_count = output_vertex_count;
  draw.motion = motion_viewport_;
  const D3D12_GPU_VIRTUAL_ADDRESS buffer =
      stream_out_buffers_[motion_frame_]->GetGPUVirtualAddress();
  D3D12_STREAM_OUTPUT_BUFFER_VIEW& view = draw.stream_out_view;
  const uint32_t counter = kStreamOutDataSize + stream_out_counters_used_ * 16;
  view.BufferLocation = buffer + offset;
  view.SizeInBytes = size;
  view.BufferFilledSizeLocation = buffer + counter;
  draw.motion.current_offset = offset;
  draw.motion.current_counter = counter;
  // (Its positions start where its binding does.)
  draw.motion.current_in_buffer = 0;
  stream_out_used_ = offset + size;
  ++stream_out_counters_used_;
  if (previous) {
    // Where the previous frame's draw wrote its positions, in its buffer.
    draw.motion.previous_offset = previous->motion.current_offset;
    draw.motion.previous_counter = previous->motion.current_counter;
    draw.motion.previous_in_buffer = previous->motion.current_in_buffer;
    draw.motion.previous_ndc_jitter[0] = previous->motion.current_ndc_jitter[0];
    draw.motion.previous_ndc_jitter[1] = previous->motion.current_ndc_jitter[1];
    draw.has_motion = true;
    ++motion_matched_;
  } else {
    ++motion_unmatched_;
  }
  stream_out_bound_ = true;
  // Pipeline binds are costly with stream output (Vulkan ends and resumes
  // transform feedback), so the pipeline stays bound for the next draw.
  draw.bind_pipeline = stream_out_bound_pipeline_ != stream_out_pipeline;
  cp_.GetDeferredCommandList().DaoCallback(ExecuteBeginStreamOut, &draw);
  stream_out_bound_pipeline_ = stream_out_pipeline;
  stream_out_guest_pipeline_ = pipeline_state;
  stream_out_binds_ = cp_.pipeline_binds_;
  return true;
}

void DaoUpscalerImpl::ExecuteBeginStreamOut(
    void* context, ID3D12GraphicsCommandList* command_list) {
  const MotionDraw& draw = *static_cast<const MotionDraw*>(context);
  if (draw.bind_pipeline) {
    command_list->SetPipelineState(draw.stream_out_pipeline);
  }
  command_list->SOSetTargets(0, 1, &draw.stream_out_view);
}

void DaoUpscalerImpl::ExecuteRestorePipeline(
    void* context, ID3D12GraphicsCommandList* command_list) {
  command_list->SetPipelineState(static_cast<ID3D12PipelineState*>(context));
}

void DaoUpscalerImpl::ExecuteCloseStreamOut(
    void* context, ID3D12GraphicsCommandList* command_list) {
  command_list->SOSetTargets(0, 1, nullptr);
}

void DaoUpscalerImpl::RecordMotionPass(uint32_t width, uint32_t height) {
  const MotionDrawPool& draws = motion_draws_[motion_frame_];
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
  ui::d3d12::util::DescriptorCpuGpuHandlePair descriptors[3];
  if (stream_out_used_ &&
      EnsureTexture(motion_depth_, DXGI_FORMAT_D32_FLOAT, width, height,
                    "motion vector depth",
                    D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) &&
      cp_.RequestOneUseSingleViewDescriptors(3, descriptors)) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = kStreamOutBufferSize / sizeof(uint32_t);
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    for (uint32_t i = 0; i < 2; ++i) {
      device->CreateShaderResourceView(
          stream_out_buffers_[motion_frame_ ^ i].Get(), &srv,
          descriptors[i].first);
    }
    srv = {};
    srv.Format = depth_.format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(depth_.resource.Get(), &srv,
                                     descriptors[2].first);
    motion_pass_.draws = &draws;
    motion_pass_.positions = descriptors[0].second;
    motion_pass_.previous_positions = descriptors[1].second;
    motion_pass_.depth = descriptors[2].second;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
    dsv.Format = motion_depth_.format;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(
        motion_depth_.resource.Get(), &dsv,
        motion_dsv_heap_->GetCPUDescriptorHandleForHeapStart());
    // Created in this state and never left: only keeps the tracked state.
    Transition(motion_depth_, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    CloseStreamOut();
    for (uint32_t i = 0; i < 2; ++i) {
      TransitionStreamOut(i, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
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
  const D3D12_CPU_DESCRIPTOR_HANDLE dsv =
      self.motion_dsv_heap_->GetCPUDescriptorHandleForHeapStart();
  command_list->OMSetRenderTargets(1, &rtv, TRUE,
                                   pass.draws ? &dsv : nullptr);
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
  command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  command_list->SetGraphicsRootDescriptorTable(1, pass.positions);
  command_list->SetGraphicsRootDescriptorTable(2, pass.previous_positions);
  command_list->SetGraphicsRootDescriptorTable(3, pass.depth);
  command_list->SetPipelineState(self.motion_depth_pipeline_.Get());
  command_list->DrawInstanced(3, 1, 0, 0);
  ID3D12PipelineState* current_pipeline = nullptr;
  for (const MotionDraw& draw : *pass.draws) {
    if (!draw.has_motion) {
      continue;
    }
    const float offset = draw.motion.depth_test_offset;
    ID3D12PipelineState* pipeline =
        self.motion_pipelines_[offset < 0.0f   ? kMotionPipelineNearerLess
                               : offset > 0.0f ? kMotionPipelineNearerGreater
                                               : kMotionPipelineNoDepthTest]
            .Get();
    if (pipeline != current_pipeline) {
      command_list->SetPipelineState(pipeline);
      current_pipeline = pipeline;
    }
    command_list->SetGraphicsRoot32BitConstants(
        0, sizeof(MotionConstants) / sizeof(uint32_t), &draw.motion, 0);
    command_list->DrawInstanced(draw.output_vertex_count, 1, 0, 0);
  }
}

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe
