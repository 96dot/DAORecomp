// DAO: temporal upscaling with AMD FSR 3.1 / FSR 4 - see dao_upscale.h.
#include "xenia/gpu/d3d12/dao_upscale.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <emmintrin.h>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform_win.h"
#include "xenia/base/threading.h"
#include "xenia/base/xxhash.h"
#include "xenia/gpu/d3d12/d3d12_command_processor.h"
#include "xenia/gpu/d3d12/dao_gpu_timing.h"
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
DECLARE_bool(present_hdr);
DECLARE_double(present_hdr_paper_white);
DECLARE_double(present_hdr_peak);
DECLARE_double(present_hdr_gamma);
DEFINE_bool(dao_show_motion_vectors, false,
            "Shows the motion vectors instead of the game (red: horizontal, "
            "green: vertical motion, gray: none), for debugging.",
            "GPU");

namespace xe {
namespace gpu {
namespace d3d12 {

namespace shaders {
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/dao_hdr_tonemap_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/dao_upscale_composite_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/dao_upscale_copy_color_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/dao_upscale_copy_depth_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/dao_upscale_copy_depth_ms_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/dao_upscale_mv_depth_ps.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/dao_upscale_mv_ps.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/dao_upscale_mv_vs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/fullscreen_cw_vs.h"
}  // namespace shaders

namespace {

using Microsoft::WRL::ComPtr;

// Stream output of the scene draws' positions (float4 per vertex, each draw in a
// part of its own), followed by the filled-size counters (one per draw, 16
// bytes apart). One for each of the last two frames: a draw's previous
// positions are those its match wrote in the previous frame.
constexpr uint32_t kStreamOutDataSize = 96 << 20;
constexpr uint32_t kStreamOutCounterCount = 16384;
constexpr uint32_t kStreamOutCounterSize = kStreamOutCounterCount * 16;
constexpr uint32_t kStreamOutBufferSize =
    kStreamOutDataSize + kStreamOutCounterSize;

// The depth difference from the scene within which a motion vector pass pixel
// is the visible surface (kDaoMvDepthTolerance in dao_upscale_mv.hlsli).
constexpr float kMotionDepthTolerance = 2e-5f;
// How far the motion vector pass's depth test moves the triangles towards the
// camera: the tolerance, with a margin for the rounding of the depth tested
// (it's interpolated differently from the pixel shader's).
constexpr float kMotionDepthTestOffset = kMotionDepthTolerance * 1.25f;

// The motion vector pass's pipelines.
enum MotionPipeline : uint32_t {
  kMotionPipelineNoDepthTest,
  kMotionPipelineNearerLess,
  kMotionPipelineNearerGreater,
  kMotionPipelineCount,
};

// A texture owned by the upscaler, with its tracked state.
struct Texture {
  ComPtr<ID3D12Resource> resource;
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  uint32_t width = 0;
  uint32_t height = 0;
};

// Whether to log the n-th (from 0) of a message that may come every frame: the
// first 10, then every 1000th.
bool LogNth(std::atomic<uint32_t>& count) {
  const uint32_t n = count.fetch_add(1, std::memory_order_relaxed);
  return n < 10 || n % 1000 == 0;
}

void FfxMessage(uint32_t type, const wchar_t* message) {
  static std::atomic<uint32_t> s_errors{0}, s_warnings{0};
  if (!LogNth(type == FFX_API_MESSAGE_TYPE_ERROR ? s_errors : s_warnings)) {
    return;
  }
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

// A set of 64-bit hashes, cleared without freeing (open addressing).
class HashSet {
 public:
  // Whether `hash` wasn't in the set yet.
  bool insert(uint64_t hash) {
    // 0 marks free slots.
    hash |= uint64_t(hash == 0);
    if ((size_ + 1) * 2 > slots_.size()) {
      Grow();
    }
    const size_t mask = slots_.size() - 1;
    for (size_t i = size_t(hash) & mask;; i = (i + 1) & mask) {
      if (slots_[i] == hash) {
        return false;
      }
      if (!slots_[i]) {
        slots_[i] = hash;
        ++size_;
        return true;
      }
    }
  }
  void clear() {
    if (size_) {
      std::fill(slots_.begin(), slots_.end(), 0);
      size_ = 0;
    }
  }

 private:
  void Grow() {
    std::vector<uint64_t> old = std::move(slots_);
    slots_.assign(std::max<size_t>(old.size() * 2, 1024), 0);
    size_ = 0;
    for (uint64_t hash : old) {
      if (hash) {
        insert(hash);
      }
    }
  }

  std::vector<uint64_t> slots_;
  size_t size_ = 0;
};

// The game's tonemapping pass: the scene and its bloom through a color matrix
// and a curve (see dao_hdr_tonemap.cs). Its pixel shader's microcode hash, the
// same in the disc's executable and Title Update 6's.
constexpr uint64_t kTonemapPixelShader = 0x20BA772900FB8AA3;

// dao_upscale_composite.cs's dao_mode values.
enum CompositeMode : uint32_t {
  kCompositeSdr,
  kCompositeHdr,
  // The HDR scene with the UI, output in SDR (while presenter UI is shown).
  kCompositeHdrToSdr,
  // One source (t1) to the destination at its size: PQ-encoded, or linear.
  kCompositeEncode,
  kCompositeLinearize,
};

// SDR white in nits; at least the scRGB unit, as in the presenter.
float PaperWhiteNits() {
  return float(std::max(cvars::present_hdr_paper_white, 80.0));
}

// The gamma the game's output is encoded with, within reason.
float OutputGamma() {
  return float(std::clamp(cvars::present_hdr_gamma, 1.0, 3.0));
}

// The tonemapping pass's color matrix (rows, offset in w), which its vertex
// shader (microcode hash AF062EA1E2A3052F) computes from constants alone
// (brightness, contrast, saturation, a hue rotation): that shader's microcode,
// translated by hand, in order; operations paired in one instruction read the
// values from before it.
void TonemapRows(const RegisterFile& regs, float rows[3][4]) {
  struct V {
    float x, y, z, w;
  };
  auto c = [&regs](uint32_t index) {
    const uint32_t r = XE_GPU_REG_SHADER_CONSTANT_000_X + index * 4;
    return V{regs.Get<float>(r), regs.Get<float>(r + 1),
             regs.Get<float>(r + 2), regs.Get<float>(r + 3)};
  };
  const V c0 = c(0), c1 = c(1), c2 = c(2), c3 = c(3), c252 = c(252),
          c253 = c(253), c254 = c(254), c255 = c(255);
  V r0 = {}, r1 = {}, r2 = {}, r3 = {}, r4 = {}, r5 = {}, r6 = {};
  r0.z = c1.x * c252.w;
  r1.x = c253.y - c2.x;
  r0.x = c3.x * c252.z + c254.w;
  r4.x = r1.x * c254.x;
  r4.y = r1.x * c254.y;
  r4.z = r1.x * c254.z;
  r6.y = c1.x * c0.x;
  r3.x = r4.x + c2.x;
  r3.y = r4.z + c2.x;
  r3.z = r4.y + c2.x;
  r0.y = r0.x - std::floor(r0.x);
  r2.y = r4.x * r6.y;
  r2.z = r4.y * r6.y;
  r2.w = r4.z * r6.y;
  r0.x = c255.x * r0.y;
  r1.y = c253.x == 0.0f ? r3.z : r4.z;
  r1.z = c253.y == 0.0f ? r3.z : r4.z;
  r1.w = c253.y == 0.0f ? r3.z : r4.x;
  r6.x = r0.z + c253.w;
  r6.z = r0.x + c253.z;
  r4.w = r3.x;
  r0.x = std::cos(r6.z);
  r0.y = r4.y * r6.x;
  r0.w = r4.x * r6.x;
  r0.z = c253.y - r0.x;
  r1.x = (r4.z + r4.y + r4.w) * r6.x;
  r0.w += r0.y;
  r5.x = r6.y * r3.x;
  r5.y = r6.x * r3.y;
  r5.z = r6.y * r3.y;
  r0.y = std::sin(r6.z);
  r1.y = (r1.z + r1.w + r1.y) * r6.x;
  r1.z = r5.y + r0.w;
  r0.y *= c252.x;
  r0.z *= c252.y;
  r4.x = r0.z + r0.y;
  r4.y = r0.z + r0.x;
  r4.z = r0.z - r0.y;
  rows[0][3] = r1.z * r4.z + r1.x * r4.y + r1.y * r4.x;
  rows[1][3] = r1.z * r4.x + r1.x * r4.z + r1.y * r4.y;
  rows[2][3] = r1.z * r4.y + r1.x * r4.x + r1.y * r4.z;
  r1 = {r2.z * r4.y, r2.y * r4.z, r2.y * r4.y, r2.y * r4.x};
  r0 = {r2.z * r4.x, r2.z * r4.z, r2.w * r4.x, r2.w * r4.y};
  r2.x = r3.z * r6.y;
  r2 = {r2.x * r4.z, r2.x * r4.y, r2.w * r4.z, r2.x * r4.x};
  r3.y = r0.w + r0.z;
  r3.x = r2.z + r0.w;
  r6.x = r5.x * r4.x + r1.y;
  r6.y = r5.x * r4.y + r1.w;
  r6.z = r5.x * r4.z + r1.z;
  r2 = {r0.x + r2.x, r0.z + r2.z, r0.y + r2.y, r2.w + r1.x};
  rows[2][2] = r5.z * r4.y + r2.y;
  rows[0][0] = r6.y + r1.y;
  rows[1][0] = r6.z + r1.w;
  r1 = {r1.x + r2.x, r6.x + r1.z, r6.y + r1.y, r6.z + r1.w};
  r0.z = r5.z * r4.x + r3.x;
  r0.w = r5.z * r4.z + r3.y;
  r0.x += r2.z;
  r0.y += r2.w;
  rows[2][0] = r1.y;
  rows[2][1] = r1.x;
  rows[0][1] = r0.y;
  rows[0][2] = r0.w;
  rows[1][1] = r0.x;
  rows[1][2] = r0.z;
}

}  // namespace

class DaoUpscalerImpl {
 public:
  DaoUpscalerImpl(D3D12CommandProcessor& command_processor,
                  D3D12RenderTargetCache& render_target_cache)
      : cp_(command_processor), rtc_(render_target_cache) {}
  ~DaoUpscalerImpl();

  bool Initialize();
  bool single_sample_scene() const { return single_sample_scene_; }
  // Whether the frame has a 3D scene. Single-sampled, the multisampled draws
  // alone (the game's clears) aren't one unless a depth tile was taken.
  bool HasScene() const {
    return scene_draws_ && (!single_sample_scene_ ||
                            single_sample_scene_drawn_ || depth_tiles_);
  }
  void OnDraw(const RegisterFile& regs, draw_util::ViewportInfo& viewport_info,
              reg::RB_DEPTHCONTROL depth_control, uint32_t color_mask,
              uint64_t pixel_shader_hash, uint32_t pixel_shader_texture_mask,
              bool window_offset_in_edram);
  void BeforeDraw(const Shader& vertex_shader, bool indexed,
                  uint32_t host_vertex_count, bool skip_motion);
  void AfterDraw();
  void OnResolve(const draw_util::ResolveInfo& resolve_info);
  ID3D12Resource* OnSwap(ID3D12Resource* frontbuffer,
                         const D3D12_SHADER_RESOURCE_VIEW_DESC& frontbuffer_srv,
                         uint32_t render_width, uint32_t render_height,
                         D3D12_SHADER_RESOURCE_VIEW_DESC& srv_out, bool& pq_out,
                         bool& upscaled_out);

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
    uint32_t mode;
    float paper_white;
    float gamma;
  };
  // dao_hdr_tonemap.cs.
  struct HdrTonemapConstants {
    float rows[3][4];
    uint32_t size[2];
    float bloom_scale;
    float offset;
    float curve_v;
    float gamma;
    float peak_ratio;
    float knee;
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
    uint32_t current_in_buffer;
    uint32_t previous_in_buffer;
    float viewport_offset[2];
    float viewport_extent[2];
    float render_size[2];
    float jitter[2];
    float z_min;
    float z_max;
    float depth_test_offset;
    float padding;
    // The jitter in the positions written (NDC offset per W).
    float current_ndc_jitter[2];
    float previous_ndc_jitter[2];
  };
  // A scene draw given motion vectors.
  struct MotionDraw {
    DaoUpscalerImpl* self;
    // For finding the same draw in the next frame.
    uint64_t key;
    uint64_t constants_hash;
    // Where the constants are in motion_constant_data_ of the frame.
    uint32_t constants_offset;
    uint32_t constants_count;
    // Sum of the constants (infinity if any isn't finite): the difference of
    // two is at most the distance between their constants.
    double constants_sum;
    // The draw's pipeline with stream output.
    ID3D12PipelineState* stream_out_pipeline;
    // Whether stream_out_pipeline is bound before the draw (otherwise the
    // previous draw left it bound).
    bool bind_pipeline;
    // Triangle list vertices of the positions written (0 if none).
    uint32_t output_vertex_count;
    // Bound before the draw, unbound after it.
    D3D12_STREAM_OUTPUT_BUFFER_VIEW stream_out_view;
    // Drawn in the motion vector pass: the previous frame's draw of it wrote
    // positions.
    bool has_motion;
    MotionConstants motion;
  };
  // MotionDraws in chunks kept when cleared: stable addresses (commands keep
  // pointers to them) without an allocation for every draw.
  class MotionDrawPool {
   public:
    MotionDraw& emplace_back() {
      if (size_ == chunks_.size() * kChunkSize) {
        chunks_.push_back(std::make_unique<MotionDraw[]>(kChunkSize));
      }
      MotionDraw& draw = (*this)[size_++];
      draw = {};
      return draw;
    }
    void clear() { size_ = 0; }
    MotionDraw& operator[](size_t i) const {
      return chunks_[i / kChunkSize][i % kChunkSize];
    }
    struct Iterator {
      const MotionDrawPool* pool;
      size_t i;
      MotionDraw& operator*() const { return (*pool)[i]; }
      Iterator& operator++() {
        ++i;
        return *this;
      }
      bool operator!=(const Iterator& other) const { return i != other.i; }
    };
    Iterator begin() const { return {this, 0}; }
    Iterator end() const { return {this, size_}; }

   private:
    static constexpr size_t kChunkSize = 256;
    std::vector<std::unique_ptr<MotionDraw[]>> chunks_;
    size_t size_ = 0;
  };
  // The motion vector pass at the swap.
  struct MotionPass {
    DaoUpscalerImpl* self;
    const MotionDrawPool* draws;
    D3D12_GPU_DESCRIPTOR_HANDLE positions;
    D3D12_GPU_DESCRIPTOR_HANDLE previous_positions;
    D3D12_GPU_DESCRIPTOR_HANDLE depth;
    uint32_t width;
    uint32_t height;
  };
  // A draw's vertex shader writing the positions with stream output, created
  // in the background (it can take long, and the draw can do without motion
  // vectors meanwhile).
  struct StreamOutPipeline {
    enum Status : uint32_t { kPending, kReady, kFailed };
    const void* vertex_shader;
    uint64_t vertex_shader_hash;
    ID3D12RootSignature* root_signature;
    const PipelineCache::Pipeline* pipeline;
    ComPtr<ID3D12PipelineState> state;
    std::atomic<uint32_t> status{kPending};
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
  // At the tonemapping pass's draw: its constants, and RunHdrTonemap after it.
  void BeginHdrTonemap(const RegisterFile& regs);
  void RunHdrTonemap();
  // True HDR without temporal upscaling: the tonemapping pass's image with the
  // UI, at the render size, PQ-encoded into encoded_.
  ID3D12Resource* CompositeHdr(
      ID3D12Resource* frontbuffer,
      const D3D12_SHADER_RESOURCE_VIEW_DESC& frontbuffer_srv,
      D3D12_SHADER_RESOURCE_VIEW_DESC& srv_out, bool& pq_out);
  // Whether this frame is true HDR (see dao_true_hdr).
  bool WantsTrueHdr() const;
  // Presenter UI (the on-screen keyboard, notifications) is drawn over the
  // guest output, in SDR: true HDR frames are output in SDR meanwhile.
  bool OverlayShown() const;
  // FSR's HDR input (or the scene for CompositeHdr) in hdr_: the tonemapping
  // pass redone, or if it isn't the image before the UI (a pass after it, or
  // none), that image linearized.
  bool EnsureHdrScene();
  // A frame not upscaled: with true HDR, the front buffer PQ-encoded into
  // encoded_ (all true HDR frames are PQ), else nullptr (the front buffer).
  ID3D12Resource* NotUpscaled(
      const char* reason, ID3D12Resource* frontbuffer,
      const D3D12_SHADER_RESOURCE_VIEW_DESC& frontbuffer_srv,
      D3D12_SHADER_RESOURCE_VIEW_DESC& srv_out, bool& pq_out);
  // The composite shader with one source, in kCompositeEncode or
  // kCompositeLinearize mode, into dest (left readable).
  bool ConvertImage(ID3D12Resource* source,
                    const D3D12_SHADER_RESOURCE_VIEW_DESC& source_srv,
                    Texture& dest, CompositeMode mode);
  void DispatchComposite(const ui::d3d12::util::DescriptorCpuGpuHandlePair*
                             descriptors,
                         CompositeConstants constants);
  bool InitializeMotion();
  // The stream output buffers (kStreamOutBufferSize each), created at the
  // first motion capture rather than with the rest, so they take no memory with
  // the temporal upscaler off. False if they couldn't be created.
  bool EnsureStreamOutBuffers();
  // Null until created (or if that failed).
  ID3D12PipelineState* GetStreamOutPipeline(
      const PipelineCache::Pipeline& pipeline);
  void StreamOutPipelineThread();
  // Of stream_out_buffers_[buffer].
  void TransitionStreamOut(uint32_t buffer, D3D12_RESOURCE_STATES state);
  // Whether the draw writes its positions.
  bool CaptureMotion(const Shader& vertex_shader, bool indexed,
                     uint32_t host_vertex_count);
  // Before the draw: the pipeline with stream output and the buffer.
  static void ExecuteBeginStreamOut(void* context,
                                    ID3D12GraphicsCommandList* command_list);
  // Binds the pipeline (the context).
  static void ExecuteRestorePipeline(void* context,
                                     ID3D12GraphicsCommandList* command_list);
  // Unbinds the stream output buffers, if bound.
  void CloseStreamOut();
  static void ExecuteCloseStreamOut(void* context,
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
  ComPtr<ID3D12RootSignature> hdr_tonemap_root_signature_;
  ComPtr<ID3D12PipelineState> hdr_tonemap_pipeline_;

  // Render resolution inputs.
  Texture color_;
  Texture depth_;
  Texture motion_vectors_;
  // The scene depth for the motion vector pass's depth test.
  Texture motion_depth_;
  // Display resolution.
  Texture upscaled_;
  Texture composite_;
  // True HDR frames not upscaled: the front buffer PQ-encoded.
  Texture encoded_;
  // True HDR: FSR's input, linear: the game's tonemapping pass redone, or
  // the image before the UI linearized when there's no such pass.
  Texture hdr_;
  // Size of the render resolution textures (the front buffer of the last
  // frame, the scaled 1280x720 normally).
  uint32_t render_size_[2] = {};

  // Jitter of the current frame in render pixels (FSR's jitterOffset).
  bool jitter_active_ = false;
  // The game renders the scene single-sampled (DaoUpscaler::
  // WantsSingleSampleScene).
  bool single_sample_scene_ = false;
  float jitter_[2] = {};
  // Frames left before jittering again after FSR could not run.
  uint32_t jitter_cooldown_ = 0;
  uint32_t fail_streak_ = 0;

  // Current frame.
  uint32_t scene_draws_ = 0;
  uint32_t depth_tiles_ = 0;
  uint32_t scene_texture_ = 0;
  // Single-sampled scene draws were drawn (their resolves are the scene's).
  bool single_sample_scene_drawn_ = false;
  bool scene_depth_pending_ = false;
  D3D12RenderTargetCache::D3D12RenderTarget* scene_depth_target_ = nullptr;
  int32_t scene_depth_window_offset_[2] = {};
  bool hudless_pending_ = false;
  bool hudless_captured_ = false;
  // True HDR this frame (WantsTrueHdr), and OverlayShown at its swap.
  bool true_hdr_ = false;
  bool overlay_shown_ = false;
  // The FSR context was created for true HDR (linear input).
  bool context_hdr_ = false;
  // The tonemapping pass, from OnDraw to AfterDraw; ran this frame.
  bool hdr_pending_ = false;
  bool hdr_captured_ = false;
  HdrTonemapConstants hdr_constants_ = {};
  uint64_t hudless_pixel_shader_ = 0;
  bool reset_history_ = true;
  bool was_upscaling_ = false;
  std::chrono::steady_clock::time_point last_swap_;

  DispatchParams dispatch_params_;

  // Motion vectors.
  bool motion_available_ = false;
  ComPtr<ID3D12RootSignature> motion_root_signature_;
  ComPtr<ID3D12PipelineState> motion_pipelines_[kMotionPipelineCount];
  // Copies the scene depth into the depth buffer.
  ComPtr<ID3D12PipelineState> motion_depth_pipeline_;
  ComPtr<ID3D12DescriptorHeap> motion_rtv_heap_;
  ComPtr<ID3D12DescriptorHeap> motion_dsv_heap_;
  // Indexed by motion_frame_.
  ComPtr<ID3D12Resource> stream_out_buffers_[2];
  D3D12_RESOURCE_STATES stream_out_states_[2] = {
      D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_DEST};
  ComPtr<ID3D12Resource> stream_out_zeros_;
  // Not retried after failing.
  bool stream_out_buffers_failed_ = false;
  std::unordered_map<const void*, std::unique_ptr<StreamOutPipeline>>
      stream_out_pipelines_;
  // Replaced ones (rare), kept: they may still be queued for creation or used
  // by commands not executed yet.
  std::vector<std::unique_ptr<StreamOutPipeline>> stream_out_pipelines_old_;
  std::thread stream_out_thread_;
  std::mutex stream_out_mutex_;
  std::condition_variable stream_out_cv_;
  std::deque<StreamOutPipeline*> stream_out_queue_;
  bool stream_out_shutdown_ = false;
  // This frame's draws and the previous frame's, alternating (a frame's draws
  // are read by its commands until they're executed after the swap).
  MotionDrawPool motion_draws_[2];
  // The constants of the frame's draws.
  std::vector<uint32_t> motion_constant_data_[2];
  uint32_t motion_frame_ = 0;
  // The previous frame's draws, in order of key, then constants_sum.
  struct MotionCandidate {
    uint64_t key;
    double constants_sum;
    const MotionDraw* draw;
  };
  std::vector<MotionCandidate> previous_motion_draws_;
  HashSet motion_seen_;
  // The constants of the current draw (kept to reuse the allocation).
  std::vector<uint32_t> motion_constants_;
  uint32_t stream_out_used_ = 0;
  uint32_t stream_out_counters_used_ = 0;
  bool stream_out_reset_ = false;
  // Whether a draw since the last CloseStreamOut bound a stream output buffer.
  bool stream_out_bound_ = false;
  uint32_t motion_matched_ = 0;
  uint32_t motion_unmatched_ = 0;
  uint32_t motion_skipped_ = 0;
  bool motion_logged_ = false;
  // From OnDraw to BeforeDraw.
  bool motion_pending_ = false;
  // The pipeline with stream output a draw left bound, while Xenia hasn't
  // bound another (cp_.pipeline_binds_ is still stream_out_binds_): the next
  // draw restores the guest one, unless it writes its positions with it too.
  ID3D12PipelineState* stream_out_bound_pipeline_ = nullptr;
  ID3D12PipelineState* stream_out_guest_pipeline_ = nullptr;
  uint64_t stream_out_binds_ = 0;
  MotionConstants motion_viewport_ = {};
  uint64_t motion_pixel_shader_ = 0;
  MotionPass motion_pass_ = {};
};

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
      !hdr_tonemap_root_signature_) {
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
  if (!copy_depth_ms_pipeline_ || !copy_depth_pipeline_ ||
      !copy_color_pipeline_ || !composite_pipeline_ ||
      !hdr_tonemap_pipeline_) {
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
      context_size_[3] == display_height && context_hdr_ == true_hdr_) {
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
  // The input is the tonemapped (gamma-encoded) image, or the HDR one
  // (linear).
  desc.flags = true_hdr_ ? FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE
                         : FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
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
  context_hdr_ = true_hdr_;

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
  // The context's resources may still be in use by submitted frames: awaited.
  // Not the open submission, which hasn't used it (its dispatches are recorded
  // at swaps, which end their submissions) and which this may be called in the
  // middle of: ending it would drop the rest of the frame's commands.
  const uint64_t current_submission = cp_.GetCurrentSubmission();
  if (current_submission > 1) {  // Otherwise nothing was submitted yet.
    cp_.CheckSubmissionCompletion(current_submission - 1);
  }
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
          break;
        case xenos::CompareFunction::kGreater:
        case xenos::CompareFunction::kGreaterEqual:
          motion.depth_test_offset = kMotionDepthTestOffset;
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

void DaoUpscalerImpl::BeginHdrTonemap(const RegisterFile& regs) {
  // Its constants: the matrix from the vertex shader's, the rest the pixel
  // shader's (c0.x, c255.xy).
  hdr_pending_ = true;
  HdrTonemapConstants& constants = hdr_constants_;
  TonemapRows(regs, constants.rows);
  const uint32_t ps = XE_GPU_REG_SHADER_CONSTANT_000_X + 256 * 4;
  constants.bloom_scale = regs.Get<float>(ps);
  constants.offset = regs.Get<float>(ps + 255 * 4);
  constants.curve_v = regs.Get<float>(ps + 255 * 4 + 1);
}

bool DaoUpscalerImpl::OverlayShown() const {
  const ui::Presenter* presenter = cp_.graphics_system_->presenter();
  return presenter && presenter->HasUIDrawers();
}

bool DaoUpscalerImpl::EnsureHdrScene() {
  if (hdr_captured_ && hudless_pixel_shader_ == kTonemapPixelShader) {
    return true;
  }
  D3D12_SHADER_RESOURCE_VIEW_DESC color_srv = {};
  color_srv.Format = color_.format;
  color_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  color_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  color_srv.Texture2D.MipLevels = 1;
  Transition(color_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  return EnsureTexture(hdr_, DXGI_FORMAT_R16G16B16A16_FLOAT, render_size_[0],
                       render_size_[1], "HDR") &&
         ConvertImage(color_.resource.Get(), color_srv, hdr_,
                      kCompositeLinearize);
}

bool DaoUpscalerImpl::WantsTrueHdr() const {
  if (!cvars::present_hdr || !cvars::dao_true_hdr) {
    return false;
  }
  // On SDR displays the presenter only linearizes; HDR here would be clipped.
  const ui::Presenter* presenter = cp_.graphics_system_->presenter();
  return presenter && presenter->IsDisplayHdr();
}

void DaoUpscalerImpl::RunHdrTonemap() {
  const uint32_t width = render_size_[0], height = render_size_[1];
  if (!width ||
      !EnsureTexture(hdr_, DXGI_FORMAT_R16G16B16A16_FLOAT, width, height,
                     "HDR")) {
    return;
  }
  // Bindless, only the destination needs one.
  const bool bindless = cp_.bindless_resources_used_;
  ui::d3d12::util::DescriptorCpuGpuHandlePair descriptors[4];
  const uint32_t descriptor_count = bindless ? 1 : 4;
  if (!cp_.RequestOneUseSingleViewDescriptors(descriptor_count, descriptors)) {
    return;
  }
  const ui::d3d12::util::DescriptorCpuGpuHandlePair& dest_descriptor =
      descriptors[descriptor_count - 1];
  // The pass's textures, bound for it by the texture cache: 0 the bloom, 1 the
  // scene, 2 the curve.
  const ui::d3d12::D3D12Provider& provider = cp_.GetD3D12Provider();
  D3D12TextureCache& texture_cache = *cp_.texture_cache_;
  D3D12_GPU_DESCRIPTOR_HANDLE textures[3];
  for (uint32_t i = 0; i < 3; ++i) {
    D3D12Shader::TextureBinding binding = {};
    binding.fetch_constant = i;
    binding.dimension = xenos::FetchOpDimension::k2D;
    if (bindless) {
      textures[i] = provider.OffsetViewDescriptor(
          cp_.GetViewBindlessHeapGPUStart(),
          texture_cache.GetActiveTextureBindlessSRVIndex(binding));
    } else {
      texture_cache.WriteActiveTextureBindfulSRV(binding,
                                                 descriptors[i].first);
      textures[i] = descriptors[i].second;
    }
  }
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
  uav.Format = hdr_.format;
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  provider.GetDevice()->CreateUnorderedAccessView(
      hdr_.resource.Get(), nullptr, &uav, dest_descriptor.first);
  Transition(hdr_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  cp_.SubmitBarriers();

  HdrTonemapConstants& constants = hdr_constants_;
  constants.size[0] = width;
  constants.size[1] = height;
  constants.gamma = OutputGamma();
  constants.peak_ratio =
      std::max(float(cvars::present_hdr_peak) / PaperWhiteNits(), 1.0f);
  // In the curves' linear part: their shoulders start at about 0.45.
  constants.knee = 0.4f;
  DeferredCommandList& command_list = cp_.GetDeferredCommandList();
  command_list.D3DSetComputeRootSignature(hdr_tonemap_root_signature_.Get());
  command_list.D3DSetComputeRoot32BitConstants(
      0, sizeof(constants) / sizeof(uint32_t), &constants, 0);
  for (uint32_t i = 0; i < 3; ++i) {
    command_list.D3DSetComputeRootDescriptorTable(1 + i, textures[i]);
  }
  command_list.D3DSetComputeRootDescriptorTable(4, dest_descriptor.second);
  cp_.SetExternalPipeline(hdr_tonemap_pipeline_.Get());
  command_list.D3DDispatch((width + 7) / 8, (height + 7) / 8, 1);
  // If the pass is drawn again this frame, the next write must wait.
  cp_.PushUAVBarrier(hdr_.resource.Get());
  hdr_captured_ = true;
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

void DaoUpscalerImpl::CloseStreamOut() {
  if (!stream_out_bound_) {
    return;
  }
  stream_out_bound_ = false;
  cp_.GetDeferredCommandList().DaoCallback(ExecuteCloseStreamOut, nullptr);
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

void DaoUpscalerImpl::ExecuteDispatch(void* context,
                                      ID3D12GraphicsCommandList* command_list) {
  auto& params = *static_cast<DispatchParams*>(context);
  DaoUpscalerImpl& self = *params.self;
  if (!self.context_) {
    return;
  }
  params.desc.commandList = command_list;
  ffxReturnCode_t result = self.ffx_dispatch_(&self.context_, &params.desc.header);
  static std::atomic<uint32_t> s_failures{0};
  if (result != FFX_API_RETURN_OK && LogNth(s_failures)) {
    XELOGE("FSR: dispatch failed ({})", result);
  }
}

ID3D12Resource* DaoUpscalerImpl::OnSwap(
    ID3D12Resource* frontbuffer,
    const D3D12_SHADER_RESOURCE_VIEW_DESC& frontbuffer_srv,
    uint32_t render_width, uint32_t render_height,
    D3D12_SHADER_RESOURCE_VIEW_DESC& srv_out, bool& pq_out,
    bool& upscaled_out) {
  pq_out = false;
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
      return CompositeHdr(frontbuffer, frontbuffer_srv, srv_out, pq_out);
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
                       true_hdr_ ? DXGI_FORMAT_R10G10B10A2_UNORM
                                 : DXGI_FORMAT_R8G8B8A8_UNORM,
                       display_width, display_height, "composite")) {
      reason = "upscaler unavailable";
    }
  }
  if (reason) {
    return NotUpscaled(reason, frontbuffer, frontbuffer_srv, srv_out, pq_out);
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
                       pq_out);
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
  desc.flags = true_hdr_ ? 0 : FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
  dispatch_params_.self = this;
  cp_.GetDeferredCommandList().DaoCallback(ExecuteDispatch, &dispatch_params_);
  InvalidateCommandProcessorState();
  if (dao_timing::GpuTiming::Enabled()) {
    dao_timing::GpuTiming::Get().Mark(cp_, "FSR");
  }

  // Composite the UI.
  ID3D12Device* device = cp_.GetD3D12Provider().GetDevice();
  ui::d3d12::util::DescriptorCpuGpuHandlePair descriptors[5];
  if (!cp_.RequestOneUseSingleViewDescriptors(5, descriptors)) {
    return NotUpscaled("no descriptors", frontbuffer, frontbuffer_srv, srv_out,
                       pq_out);
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
  CompositeConstants constants = {};
  constants.dest_size[0] = display_width;
  constants.dest_size[1] = display_height;
  constants.render_size[0] = float(render_width);
  constants.render_size[1] = float(render_height);
  constants.show_motion = cvars::dao_show_motion_vectors ? 1 : 0;
  constants.mode = !true_hdr_       ? kCompositeSdr
                   : overlay_shown_ ? kCompositeHdrToSdr
                                    : kCompositeHdr;
  DispatchComposite(descriptors, constants);
  Transition(composite_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  srv_out = {};
  srv_out.Format = composite_.format;
  srv_out.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv_out.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv_out.Texture2D.MipLevels = 1;
  pq_out = true_hdr_ && !overlay_shown_;
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
  DeferredCommandList& command_list = cp_.GetDeferredCommandList();
  command_list.D3DSetComputeRootSignature(composite_root_signature_.Get());
  command_list.D3DSetComputeRoot32BitConstants(
      0, sizeof(constants) / sizeof(uint32_t), &constants, 0);
  for (uint32_t i = 0; i < 5; ++i) {
    command_list.D3DSetComputeRootDescriptorTable(1 + i,
                                                  descriptors[i].second);
  }
  cp_.SetExternalPipeline(composite_pipeline_.Get());
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
  const ui::d3d12::util::DescriptorCpuGpuHandlePair tables[5] = {
      descriptors[0], descriptors[0], descriptors[0], descriptors[0],
      descriptors[1]};
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

ID3D12Resource* DaoUpscalerImpl::CompositeHdr(
    ID3D12Resource* frontbuffer,
    const D3D12_SHADER_RESOURCE_VIEW_DESC& frontbuffer_srv,
    D3D12_SHADER_RESOURCE_VIEW_DESC& srv_out, bool& pq_out) {
  const uint32_t width = render_size_[0], height = render_size_[1];
  if (!EnsureHdrScene()) {
    return NotUpscaled("no HDR scene", frontbuffer, frontbuffer_srv, srv_out,
                       pq_out);
  }
  ui::d3d12::util::DescriptorCpuGpuHandlePair descriptors[5];
  if (!EnsureTexture(encoded_, DXGI_FORMAT_R10G10B10A2_UNORM, width, height,
                     "encoded") ||
      !cp_.RequestOneUseSingleViewDescriptors(5, descriptors)) {
    return NotUpscaled("no descriptors", frontbuffer, frontbuffer_srv, srv_out,
                       pq_out);
  }
  // The scene (t0) and the image before the UI (t2) at the front buffer's
  // size; no motion vectors (t3, unread, given the image's view).
  ID3D12Device* device = cp_.GetD3D12Provider().GetDevice();
  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  srv.Format = hdr_.format;
  device->CreateShaderResourceView(hdr_.resource.Get(), &srv,
                                   descriptors[0].first);
  device->CreateShaderResourceView(frontbuffer, &frontbuffer_srv,
                                   descriptors[1].first);
  srv.Format = color_.format;
  device->CreateShaderResourceView(color_.resource.Get(), &srv,
                                   descriptors[2].first);
  descriptors[3] = descriptors[2];
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
  uav.Format = encoded_.format;
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  device->CreateUnorderedAccessView(encoded_.resource.Get(), nullptr, &uav,
                                    descriptors[4].first);
  Transition(hdr_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(color_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(encoded_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  CompositeConstants constants = {};
  constants.dest_size[0] = width;
  constants.dest_size[1] = height;
  constants.render_size[0] = float(width);
  constants.render_size[1] = float(height);
  constants.mode = kCompositeHdr;
  DispatchComposite(descriptors, constants);
  Transition(encoded_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  EndFrame(false, "off or paused (true HDR at the render size)");
  pq_out = true;
  srv_out = {};
  srv_out.Format = encoded_.format;
  srv_out.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv_out.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv_out.Texture2D.MipLevels = 1;
  return encoded_.resource.Get();
}

ID3D12Resource* DaoUpscalerImpl::NotUpscaled(
    const char* reason, ID3D12Resource* frontbuffer,
    const D3D12_SHADER_RESOURCE_VIEW_DESC& frontbuffer_srv,
    D3D12_SHADER_RESOURCE_VIEW_DESC& srv_out, bool& pq_out) {
  // With an overlay, the presenter's SDR path.
  const bool encode = true_hdr_ && !overlay_shown_ &&
                      EnsureTexture(encoded_, DXGI_FORMAT_R10G10B10A2_UNORM,
                                    render_size_[0], render_size_[1],
                                    "encoded") &&
                      ConvertImage(frontbuffer, frontbuffer_srv, encoded_,
                                   kCompositeEncode);
  EndFrame(false, reason);
  if (!encode) {
    return nullptr;
  }
  pq_out = true;
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
    D3D12_SHADER_RESOURCE_VIEW_DESC& srv_out, bool& pq_out,
    bool& upscaled_out) {
  pq_out = upscaled_out = false;
  return impl_ ? impl_->OnSwap(frontbuffer, frontbuffer_srv, frontbuffer_width,
                               frontbuffer_height, srv_out, pq_out,
                               upscaled_out)
               : nullptr;
}

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe
