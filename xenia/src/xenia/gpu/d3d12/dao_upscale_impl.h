// DAO: the temporal upscaler's internals (see dao_upscale.h), shared by its
// files: dao_upscale.cc (the frame: draws, resolves, the swap and the
// composite), the backends dao_upscale_fsr.cc (FidelityFX) and
// dao_upscale_xess.cc (XeSS), dao_upscale_fg.cc (frame generation),
// dao_upscale_motion.cc (motion vectors), dao_upscale_hdr.cc (true HDR) and
// dao_motion_blur.cc.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/dao_live_settings.h"
#include "xenia/gpu/d3d12/d3d12_render_target_cache.h"
#include "xenia/gpu/d3d12/dao_upscale.h"
#include "xenia/gpu/d3d12/pipeline_cache.h"
#include "xenia/ui/d3d12/d3d12_util.h"

#include "LmxxfNrApi.h"
#include "ffx_api.h"
#include "ffx_framegeneration.h"

DECLARE_double(present_hdr_gamma);
DECLARE_bool(dao_neural_rendering);

namespace xe {
namespace gpu {
namespace d3d12 {

// Helpers of the upscaler's files (only they include this header), used
// unqualified through the using-directive after the namespace.
namespace dao_upscale {

using Microsoft::WRL::ComPtr;

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
inline bool LogNth(std::atomic<uint32_t>& count) {
  const uint32_t n = count.fetch_add(1, std::memory_order_relaxed);
  return n < 10 || n % 1000 == 0;
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
  // A linear scene with the UI, output in SDR: the linearized SDR image's
  // (XeSS).
  kCompositeHdrToSdr,
  // One source (t1) to the destination at its size: HDR-encoded (HdrFormat),
  // or linear.
  kCompositeEncode,
  kCompositeLinearize,
  // The scene alone, motion-blurred, as the upscaler output it (frame
  // generation's input).
  kCompositeScene,
  // One source (t1, linear) to the destination at its size, gamma-encoded as
  // the game's output.
  kCompositeGamma,
};

// SDR white in nits; at least the scRGB unit, as in the presenter.
inline float PaperWhiteNits() { return dao_live::HdrPaperWhite(); }

// The gamma the game's output is encoded with, within reason.
inline float OutputGamma() {
  return float(std::clamp(cvars::present_hdr_gamma, 1.0, 3.0));
}

// The phases of a jitter sequence for upscaling render_width pixels to
// display_width: 8 per render pixel's area in display pixels (at least 8), so
// that every display pixel gets samples.
uint32_t JitterPhaseCount(uint32_t render_width, uint32_t display_width);
// A jitter sample offset, -0.5 to 0.5 render pixels: Halton (2, 3).
void HaltonJitter(uint32_t index, float& x, float& y);

}  // namespace dao_upscale

using namespace dao_upscale;

class DaoUpscalerImpl;

// The FidelityFX API (amd_fidelityfx_loader_dx12.dll next to the executable),
// loaded when first needed: FSR's upscaler backend and frame generation call
// it (dao_upscale_fsr.cc).
struct FfxApi {
  ~FfxApi();
  // False if the loader is missing or incomplete (logged once): not tried
  // again. configure may be null (only frame generation needs it).
  bool Load();
  // " - see the log of <proxy>", or nothing: for FidelityFX's error messages.
  std::string ProxyNote() const {
    return proxy.empty() ? std::string() : " - see the log of " + proxy;
  }

  HMODULE module = nullptr;
  bool failed = false;
  PfnFfxCreateContext create = nullptr;
  PfnFfxDestroyContext destroy = nullptr;
  PfnFfxQuery query = nullptr;
  PfnFfxDispatch dispatch = nullptr;
  PfnFfxConfigure configure = nullptr;
  // A DLL in the program folder in place of a system one (OptiScaler, which
  // can take over FidelityFX's calls), for the log; empty if none.
  std::string proxy;
};

// A temporal upscaler: AMD FSR 3.1 / 4 (dao_upscale_fsr.cc) or Intel XeSS
// (dao_upscale_xess.cc), its library loaded from next to the executable when
// first needed. Used on the command processor's thread only.
class UpscalerBackend {
 public:
  // What the context is for.
  struct ContextDesc {
    uint32_t render_size[2];
    uint32_t display_size[2];
    // The color is linear HDR (true HDR), else SDR.
    bool hdr;
    bool operator==(const ContextDesc& other) const = default;
  };
  // A frame to upscale. The inputs are at the render size, in
  // NON_PIXEL_SHADER_RESOURCE; the output (RGBA16F, in the color's encoding)
  // at the display size, in UNORDERED_ACCESS.
  struct Frame {
    // SDR: the game's image (gamma-encoded RGBA8), or linearized (RGBA16F)
    // if LinearSdrColor; HDR: linear (RGBA16F, 1 = the game's white).
    ID3D12Resource* color;
    // R32_FLOAT, nearer is smaller.
    ID3D12Resource* depth;
    // R16G16_FLOAT, in render pixels, to where each pixel was in the previous
    // frame, without the jitter.
    ID3D12Resource* motion_vectors;
    ID3D12Resource* output;
    // In render pixels, as the scene draws are offset (FSR's and XeSS's
    // jitter offset: the negated sample offset).
    float jitter[2];
    uint32_t render_size[2];
    uint32_t display_size[2];
    // 0 (none) to 1, if the backend Sharpens.
    float sharpness;
    float frame_ms;
    // No history (the previous frame wasn't upscaled).
    bool reset;
  };

  explicit UpscalerBackend(DaoUpscalerImpl& upscaler) : upscaler_(upscaler) {}
  virtual ~UpscalerBackend() = default;

  // For the log.
  virtual const char* name() const = 0;
  // Whether it takes SDR color linear (the game's image linearized) rather
  // than as the game encodes it.
  virtual bool LinearSdrColor() const = 0;
  // Whether it sharpens (Frame::sharpness); otherwise the presenter's
  // sharpening applies.
  virtual bool Sharpens() const = 0;
  // Starts, before the first frame, what can be done ahead: XeSS loads and
  // builds its pipelines in the background (not to stall the first 3D scene).
  virtual void Prepare(bool hdr) {}
  // Whether it can't be used: its library is missing, the GPU or the sizes
  // unsupported, or creating its context or upscaling failed. Not tried
  // again.
  bool failed() const { return failed_; }
  // Whether the last EnsureContext started it again after a failed dispatch
  // (its history gone), cleared by reading it.
  bool TakeRestarted() { return std::exchange(restarted_, false); }
  // Creates the context for desc unless it's for it already (re-creating it
  // waits for the GPU to be done with the old one). False if it can't be.
  virtual bool EnsureContext(const ContextDesc& desc) = 0;
  // The jitter sequence's length for the context (8 without one).
  virtual uint32_t JitterPhases() const = 0;
  // A jitter phase's sample offset, -0.5 to 0.5 render pixels.
  virtual void JitterOffset(uint32_t index, float& x, float& y) const = 0;
  // Records the upscale into the deferred command list, run when it's
  // executed (the frame is copied), with the context of the last
  // EnsureContext.
  virtual void RecordDispatch(const Frame& frame) = 0;
  // The GPU is done with everything recorded with it: destroying it needn't
  // wait.
  void SetGpuDone() { gpu_done_ = true; }

 protected:
  DaoUpscalerImpl& upscaler_;
  bool failed_ = false;
  bool restarted_ = false;
  bool gpu_done_ = false;
};

std::unique_ptr<UpscalerBackend> CreateFsrBackend(DaoUpscalerImpl& upscaler);
std::unique_ptr<UpscalerBackend> CreateXessBackend(DaoUpscalerImpl& upscaler);

// Neural rendering of the 3D scene before the upscaler (dao_upscale_neural.cc):
// lmxxf's HIP port of DLSS 5's neural rendering (AMD RDNA 4), its runtime
// loaded when first used. Used on the command processor's thread only.
class NeuralRendering {
 public:
  explicit NeuralRendering(DaoUpscalerImpl& upscaler) : upscaler_(upscaler) {}
  ~NeuralRendering();
  // Whether it can't be used: its runtime, weights or kernels are missing, or
  // it failed. Not tried again.
  bool failed() const { return failed_; }
  // Runs the network on `color` (the scene at the render size, SDR, in
  // NON_PIXEL_SHADER_RESOURCE): records what's before it, ends the open
  // submission, has the queue wait for the network, and begins the next.
  // Returns its output at the render size (in NON_PIXEL_SHADER_RESOURCE when
  // read), or null if it doesn't run this frame (the open submission as it
  // was, unless it failed).
  // `strength` is in (0, 1].
  ID3D12Resource* Run(ID3D12Resource* color, uint32_t width, uint32_t height,
                      float strength, bool reset);
  // Loads it and builds the network for frames of `color`'s size (as Run's,
  // in NON_PIXEL_SHADER_RESOURCE), unless it's loaded or has failed: a second
  // or two, done on the first frame rather than the first 3D scene's.
  void Prepare(ID3D12Resource* color, uint32_t width, uint32_t height);

 private:
  bool Load();
  // The runtime's description of a frame of `color`.
  LmxxfNrFrameInfo FrameInfo(ID3D12Resource* color, uint32_t width,
                             uint32_t height, float strength);
  // Logs the runtime's error and turns it off; false.
  bool Fail(const char* what);
  static void ExecuteInputs(void* context,
                            ID3D12GraphicsCommandList* command_list);
  static void ExecuteOutputs(void* context,
                             ID3D12GraphicsCommandList* command_list);

  DaoUpscalerImpl& upscaler_;
  HMODULE module_ = nullptr;
  LmxxfNrApi api_ = {};
  void* session_ = nullptr;
  LmxxfNrJob job_ = {};
  // The job's outputs are recorded: retired before the next frame's.
  bool job_pending_ = false;
  // Set while the submission with its inputs is executed.
  bool inputs_recorded_ = false;
  // A history reset to do on the next frame it runs on.
  bool reset_pending_ = false;
  bool failed_ = false;
  uint64_t frame_ = 0;
};

class DaoUpscalerImpl {
  friend class NeuralRendering;

 public:
  DaoUpscalerImpl(D3D12CommandProcessor& command_processor,
                  D3D12RenderTargetCache& render_target_cache)
      : cp_(command_processor), rtc_(render_target_cache) {}
  ~DaoUpscalerImpl();

  bool Initialize();
  bool single_sample_scene() const { return single_sample_scene_; }
  // For the backends.
  D3D12CommandProcessor& command_processor() const { return cp_; }
  FfxApi& ffx() { return ffx_; }
  ID3D12Device* device() const;
  // Waits for the GPU to finish the submitted frames (before a backend's
  // context is destroyed). Not the open submission, which hasn't used it (the
  // dispatches are recorded at swaps, which end their submissions) and which
  // this may be called in the middle of: ending it would drop the rest of the
  // frame's commands.
  void AwaitSubmittedFrames();
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
                         D3D12_SHADER_RESOURCE_VIEW_DESC& srv_out, bool& hdr_out,
                         bool& sharpened_out, ID3D12Resource*& generated_out);

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
    uint32_t scrgb;
    // dao_upscale_composite_blur.cs only.
    uint32_t blur_samples;
    uint32_t blur_tile;
  };
  // The composite's descriptor tables: t0 to t5, then u0.
  static constexpr uint32_t kCompositeTables = 7;
  // dao_motion_blur.hlsli (the tile passes).
  struct BlurConstants {
    // The tile pass: the motion vectors' size; the neighbourhood pass: the
    // tile counts.
    uint32_t size[2];
    uint32_t tile;
    float scale;
    float cut;
    uint32_t depth_reversed;
    uint32_t counter;
    uint32_t cut_tiles;
    uint32_t previous_valid;
  };
  // The tile passes' descriptor tables (t0 to t2, u0 to u3), and their views
  // (RecordMotionBlurTiles).
  static constexpr uint32_t kBlurTables = 7;
  static constexpr uint32_t kBlurViews = kBlurTables + 2;
  // dao_motion_blur this frame (samples 0: off).
  struct MotionBlur {
    uint32_t samples = 0;
    uint32_t tile = 0;  // Render pixels.
    float scale = 0.0f;
    float cut = 0.0f;  // Render pixels.
    uint32_t tile_count[2] = {};
    // Tiles with a cut that make the whole frame one.
    uint32_t cut_tiles = 0;
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
  // What frame generation's deferred calls need when the command list is
  // executed.
  struct FrameGenerationParams {
    DaoUpscalerImpl* self;
    ffxConfigureDescFrameGeneration configure;
    ffxDispatchDescFrameGenerationPrepareV2 prepare;
    ffxDispatchDescFrameGeneration dispatch;
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

  // The context of the backend chosen with dao_temporal_upscaler, or of FSR
  // if XeSS can't be used; false if no backend can upscale the frame.
  bool EnsureBackendContext(uint32_t render_width, uint32_t render_height,
                            uint32_t display_width, uint32_t display_height);
  bool EnsureTexture(Texture& texture, DXGI_FORMAT format, uint32_t width,
                     uint32_t height, const char* name,
                     D3D12_RESOURCE_FLAGS extra_flags = D3D12_RESOURCE_FLAG_NONE);
  // Released once the GPU is done with it.
  void ReleaseTexture(Texture& texture);
  void Transition(Texture& texture, D3D12_RESOURCE_STATES state);
  // Copies a rectangle of a render target into one of the upscaler's input
  // textures.
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
  // UI, at the render size, HDR-encoded into encoded_.
  ID3D12Resource* CompositeHdr(
      ID3D12Resource* frontbuffer,
      const D3D12_SHADER_RESOURCE_VIEW_DESC& frontbuffer_srv,
      D3D12_SHADER_RESOURCE_VIEW_DESC& srv_out, bool& hdr_out);
  // Whether this frame is true HDR (see dao_true_hdr).
  bool WantsTrueHdr() const;
  // True HDR frames are scRGB (HDR quality High, the presenter's IsHdrScrgb),
  // else PQ-encoded; HdrFormat is their format.
  bool HdrScrgb() const;
  DXGI_FORMAT HdrFormat() const {
    return HdrScrgb() ? DXGI_FORMAT_R16G16B16A16_FLOAT
                      : DXGI_FORMAT_R10G10B10A2_UNORM;
  }
  // The linear scene in hdr_, for the upscaler (true HDR, or a backend's
  // LinearSdrColor), neural rendering or CompositeHdr: with true HDR the
  // tonemapping pass redone (unless it isn't the image before the UI: a pass
  // after it, or none), else the image before the UI linearized.
  bool EnsureHdrScene();
  // hdr_ at the render size (as EnsureHdrScene and neural rendering's
  // warm-up take it).
  bool EnsureHdrTexture();
  // A frame not upscaled: with true HDR, the front buffer HDR-encoded into
  // encoded_ (all true HDR frames are), else nullptr (the front buffer).
  ID3D12Resource* NotUpscaled(
      const char* reason, ID3D12Resource* frontbuffer,
      const D3D12_SHADER_RESOURCE_VIEW_DESC& frontbuffer_srv,
      D3D12_SHADER_RESOURCE_VIEW_DESC& srv_out, bool& hdr_out);
  // The composite shader with one source, in kCompositeEncode,
  // kCompositeLinearize or kCompositeGamma mode, into dest (left readable).
  bool ConvertImage(ID3D12Resource* source,
                    const D3D12_SHADER_RESOURCE_VIEW_DESC& source_srv,
                    Texture& dest, CompositeMode mode);
  // Motion blur's settings for a frame upscaled now, off if there's none.
  MotionBlur GetMotionBlur(uint32_t render_width,
                           uint32_t render_height) const;
  // Motion blur's tile passes, into blur_pixels_, blur_tiles_ and then
  // blur_around_ (left readable), and a copy of the motion vectors for the
  // next; descriptors: kBlurViews.
  void RecordMotionBlurTiles(
      const MotionBlur& blur, uint32_t render_width, uint32_t render_height,
      const ui::d3d12::util::DescriptorCpuGpuHandlePair* descriptors);
  // A tile pass, with its kBlurTables descriptor tables.
  void DispatchBlurTiles(
      ID3D12PipelineState* pipeline, const BlurConstants& constants,
      const ui::d3d12::util::DescriptorCpuGpuHandlePair* const* tables,
      uint32_t groups_x, uint32_t groups_y);
  // With kCompositeTables descriptors; with motion blur if
  // constants.blur_samples.
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
  // Frame generation (dao_upscale_fg.cc): the live setting, unless it failed.
  bool FrameGenerationWanted() const;
  // When the live setting changes: off releases its context and images, on
  // has the presenter try its frames right away.
  void OnFrameGenerationToggled(bool enabled);
  bool EnsureFrameGeneration(uint32_t render_width, uint32_t render_height,
                             uint32_t display_width, uint32_t display_height);
  // Retires the context: destroyed once the GPU is done with the frames
  // submitted with it (DestroyRetiredFrameGeneration), not waited for here.
  void DestroyFrameGeneration();
  // The retired context destroyed, waiting for the GPU if wait (else only if
  // it's done with it).
  void DestroyRetiredFrameGeneration(bool wait);
  // The context and images released (off, failed, or no upscaler left).
  void ReleaseFrameGeneration();
  // The frame between the previous one given and this one's scene (display
  // size, readable) into fg_generated_ (left in unordered access). False if
  // it has none to show (the first after a reset, or not previous_given).
  bool RecordFrameGeneration(const Texture& scene, uint32_t render_width,
                             uint32_t render_height, float frame_ms,
                             bool previous_given);
  static void ExecuteFrameGeneration(void* context,
                                     ID3D12GraphicsCommandList* command_list);
  void EndFrame(bool upscaled, const char* reason);

  D3D12CommandProcessor& cp_;
  D3D12RenderTargetCache& rtc_;

  // (Before the backends: FSR's and frame generation's calls use it, so it's
  // destroyed after them.)
  FfxApi ffx_;
  std::unique_ptr<UpscalerBackend> backend_;
  // With dao_neural_rendering (dao_upscale_neural.cc), created when first used.
  std::unique_ptr<NeuralRendering> neural_;
  NeuralRendering& Neural() {
    if (!neural_) {
      neural_ = std::make_unique<NeuralRendering>(*this);
    }
    return *neural_;
  }
  // Paused (F6, or strength 0) since it last ran: its history restarts when it
  // resumes.
  bool neural_paused_ = false;
  // The strength of the network's output in the last upscaled frame (0
  // without it), and the change in it that resets the upscaler's history.
  float neural_shown_ = 0.0f;
  static constexpr float kNeuralStrengthJump = 0.25f;
  // Whether NeuralRendering::Prepare was done (at the first swap).
  bool neural_prepared_ = false;
  // Whether backend_ is FSR's (which the others fall back to).
  bool backend_fsr_ = false;
  // A backend replaced by FSR, kept until the GPU is done with the
  // submission it was replaced in (a dispatch of it may be queued there if
  // that submission couldn't be ended).
  std::unique_ptr<UpscalerBackend> retired_backend_;
  uint64_t retired_submission_ = 0;
  // The context the backend was last asked for (a change resets the
  // history).
  UpscalerBackend::ContextDesc context_desc_ = {};
  uint32_t jitter_index_ = 0;

  ComPtr<ID3D12RootSignature> copy_root_signature_;
  ComPtr<ID3D12PipelineState> copy_depth_ms_pipeline_;
  ComPtr<ID3D12PipelineState> copy_depth_pipeline_;
  ComPtr<ID3D12PipelineState> copy_color_pipeline_;
  ComPtr<ID3D12RootSignature> composite_root_signature_;
  ComPtr<ID3D12PipelineState> composite_pipeline_;
  ComPtr<ID3D12PipelineState> composite_blur_pipeline_;
  ComPtr<ID3D12RootSignature> blur_root_signature_;
  ComPtr<ID3D12PipelineState> blur_tile_max_pipeline_;
  ComPtr<ID3D12PipelineState> blur_neighbor_max_pipeline_;
  ComPtr<ID3D12RootSignature> hdr_tonemap_root_signature_;
  ComPtr<ID3D12PipelineState> hdr_tonemap_pipeline_;

  // Render resolution inputs.
  Texture color_;
  Texture depth_;
  Texture motion_vectors_;
  // The scene depth for the motion vector pass's depth test.
  Texture motion_depth_;
  // Motion blur (render resolution): each pixel's blur length and log depth;
  // per tile, the longest blur vector in it, and around it; 2 counters of
  // tiles with a camera cut (kept in unordered access), used in turn,
  // blur_counter_ this frame's.
  Texture blur_pixels_;
  Texture blur_tiles_;
  Texture blur_around_;
  Texture blur_cuts_;
  uint32_t blur_counter_ = 0;
  // Copies of the motion vectors, of this blur frame ([blur_counter_]) and
  // the last blur frame's; that one is the frame before's if
  // blur_motion_valid_.
  Texture blur_motion_[2];
  bool blur_motion_valid_ = false;
  // The render size its textures couldn't be created at.
  uint32_t blur_failed_size_[2] = {};
  // How many of this frame's scene draws have a Less(Equal) depth function,
  // and how many Greater(Equal): whichever is more tells motion blur which
  // way the depth grows.
  uint32_t depth_less_draws_ = 0;
  uint32_t depth_greater_draws_ = 0;
  // Display resolution.
  Texture upscaled_;
  Texture composite_;
  // True HDR frames not upscaled: the front buffer HDR-encoded (HdrFormat).
  Texture encoded_;
  // Frame generation: the motion-blurred scene (its input with motion blur),
  // the generated frame, and it with the UI (composite_'s format).
  Texture fg_scene_;
  Texture fg_generated_;
  Texture fg_composite_;
  // The upscaler's input when it's linear (EnsureHdrScene): with true HDR
  // the game's tonemapping pass redone, or the image before the UI
  // linearized. Neural rendering's input too.
  Texture hdr_;
  // Neural rendering's output (linear) gamma-encoded, for a backend taking
  // SDR (color_'s format).
  Texture neural_sdr_;
  // Size of the render resolution textures (the front buffer of the last
  // frame, the scaled 1280x720 normally).
  uint32_t render_size_[2] = {};

  // Jitter of the current frame in render pixels (UpscalerBackend::Frame).
  bool jitter_active_ = false;
  // The game renders the scene single-sampled (DaoUpscaler::
  // WantsSingleSampleScene).
  bool single_sample_scene_ = false;
  float jitter_[2] = {};
  // Frames left before jittering again after the upscaler could not run.
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
  // True HDR this frame (WantsTrueHdr).
  bool true_hdr_ = false;
  // The tonemapping pass, from OnDraw to AfterDraw; ran this frame.
  bool hdr_pending_ = false;
  bool hdr_captured_ = false;
  HdrTonemapConstants hdr_constants_ = {};
  uint64_t hudless_pixel_shader_ = 0;
  bool reset_history_ = true;
  bool was_upscaling_ = false;
  std::chrono::steady_clock::time_point last_swap_;

  // Frame generation's context, not retried after failing; the display size
  // its images couldn't be created at; the live setting last seen.
  ffxContext fg_context_ = nullptr;
  ffxContext fg_retired_context_ = nullptr;
  uint64_t fg_retired_submission_ = 0;
  uint32_t fg_context_size_[4] = {};
  bool fg_context_hdr_ = false;
  bool fg_failed_ = false;
  uint32_t fg_failed_size_[2] = {};
  bool fg_enabled_ = false;
  // Its frame IDs (one more for each frame given), and whether the last frame
  // was given.
  uint64_t fg_frame_id_ = 0;
  bool fg_previous_ = false;
  FrameGenerationParams fg_params_;

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

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe
