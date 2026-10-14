// DAO: low latency - see dao_low_latency.h.
#include "xenia/ui/d3d12/dao_low_latency.h"

#include <atomic>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/dao_live_settings.h"
#include "xenia/ui/d3d12/dao_streamline.h"

#include "ffx_antilag2_dx12.h"
#include "sl.h"
#include "sl_pcl.h"
#include "sl_reflex.h"

DEFINE_int32(dao_low_latency, 0,
             "Low latency: the GPU's own technology (AMD Radeon Anti-Lag 2, "
             "or NVIDIA Reflex through Streamline, experimental) starts each "
             "frame as late as it can still be on time, so the controller is "
             "read closer to when the frame is shown. 0 off, 1 on, 2 on with "
             "boost (Reflex's; Anti-Lag 2 has none: the same as 1).",
             "Display");

namespace xe {
namespace ui {
namespace d3d12 {
namespace dao_low_latency {

namespace {

// dao_low_latency's level with boost.
constexpr int32_t kBoost = 2;

// Set up by Initialize, before the game's thread starts, and taken down by
// Shutdown after it has stopped; in between, FrameStart's (the game thread's).
AMD::AntiLag2DX12::Context s_anti_lag = {};
bool s_anti_lag_available = false;
// Whether it was on at the last FrameStart.
bool s_on = false;
bool s_logged_on = false;

// NVIDIA Reflex, through Streamline (set up by Initialize). Its sleep and the
// frame's start are the game thread's; its markers (PC Latency, sent whether
// or not it's on, as NVIDIA asks) follow the frame through the render
// thread's swap, the GPU thread's submission of that swap and the presenter's
// present. A frame is numbered as it starts; the swap marks the newest frame
// started, the submission the frame its swap had, and the present the newest
// frame submitted (it may skip some), each once.
PFun_slReflexSetOptions* s_reflex_set_options = nullptr;
PFun_slReflexSleep* s_reflex_sleep = nullptr;
PFun_slPCLSetMarker* s_pcl_set_marker = nullptr;
bool s_reflex = false;
sl::ReflexMode s_reflex_mode = sl::ReflexMode::eOff;  // The game thread's.
std::atomic<uint32_t> s_frame_started{0};
std::atomic<uint32_t> s_frame_swapped{0};
std::atomic<uint32_t> s_frame_submitted{0};
std::atomic<uint32_t> s_frame_presented{0};
uint32_t s_frame_presenting = 0;  // The presenter's.
// The started frames' tokens, by frame % sl::MAX_FRAMES_IN_FLIGHT (as many as
// Streamline expects in flight; a frame further behind isn't marked).
std::atomic<sl::FrameToken*> s_tokens[sl::MAX_FRAMES_IN_FLIGHT] = {};
// Each swap's frame (0 for none), by the swap's number % kSwaps, with that
// number in the high half: the render thread counts its swaps, the GPU
// thread the swaps it issues, the same ones in the same order.
constexpr uint32_t kSwaps = 8;
std::atomic<uint64_t> s_swap_frames[kSwaps] = {};
uint32_t s_swaps = 0;         // The render thread's.
uint32_t s_swaps_issued = 0;  // The GPU thread's.

// Low latency's level this frame: the setting's, or off with frame
// generation (its presents are our pacer's; Anti-Lag 2 stalls with it).
int32_t Level() {
  return dao_live::Load(dao_live::values.frame_generation)
             ? 0
             : dao_live::Load(dao_live::values.low_latency);
}

void Mark(sl::PCLMarker marker, uint32_t frame) {
  sl::FrameToken* token =
      s_tokens[frame % sl::MAX_FRAMES_IN_FLIGHT].load(std::memory_order_acquire);
  if (token && uint32_t(*token) == frame) {
    s_pcl_set_marker(marker, *token);
  }
}

// Moves stage on to the newest frame the stage before it reached: that frame,
// or 0 if stage already had it.
uint32_t Advance(const std::atomic<uint32_t>& before,
                 std::atomic<uint32_t>& stage) {
  const uint32_t frame = before.load(std::memory_order_acquire);
  if (!frame || stage.load(std::memory_order_relaxed) == frame) {
    return 0;
  }
  stage.store(frame, std::memory_order_release);
  return frame;
}

void InitializeReflex() {
  if (!dao_streamline::Active()) {
    return;
  }
  s_reflex_set_options = reinterpret_cast<PFun_slReflexSetOptions*>(
      dao_streamline::FeatureFunction(sl::kFeatureReflex,
                                      "slReflexSetOptions"));
  s_reflex_sleep = reinterpret_cast<PFun_slReflexSleep*>(
      dao_streamline::FeatureFunction(sl::kFeatureReflex, "slReflexSleep"));
  auto* get_state = reinterpret_cast<PFun_slReflexGetState*>(
      dao_streamline::FeatureFunction(sl::kFeatureReflex, "slReflexGetState"));
  s_pcl_set_marker = reinterpret_cast<PFun_slPCLSetMarker*>(
      dao_streamline::FeatureFunction(sl::kFeaturePCL, "slPCLSetMarker"));
  sl::ReflexOptions options;
  options.mode = sl::ReflexMode::eOff;
  // Its options set once before the first frame sets the setting's mode.
  if (!s_reflex_set_options || !s_reflex_sleep || !get_state ||
      !s_pcl_set_marker || s_reflex_set_options(options) != sl::Result::eOk) {
    XELOGW("Low latency: NVIDIA Reflex couldn't be set up");
    // Nothing else uses Streamline: not its swap chain for nothing.
    dao_streamline::Shutdown();
    return;
  }
  s_reflex_mode = sl::ReflexMode::eOff;
  sl::ReflexState state;
  if (get_state(state) == sl::Result::eOk && !state.lowLatencyAvailable) {
    XELOGW("Low latency: NVIDIA Reflex isn't available on this GPU (only its "
           "latency markers are sent)");
  } else {
    XELOGI("Low latency: NVIDIA Reflex available (experimental)");
  }
  s_reflex = true;
}

void ReflexFrameStart() {
  const int32_t level = Level();
  const sl::ReflexMode mode = level == 0 ? sl::ReflexMode::eOff
                              : level >= kBoost
                                  ? sl::ReflexMode::eLowLatencyWithBoost
                                  : sl::ReflexMode::eLowLatency;
  if (mode != s_reflex_mode) {
    sl::ReflexOptions options;
    options.mode = mode;
    const sl::Result result = s_reflex_set_options(options);
    // (Tried again next frame if it failed.)
    if (result == sl::Result::eOk) {
      s_reflex_mode = mode;
    }
    if (mode != sl::ReflexMode::eOff && !s_logged_on) {
      s_logged_on = true;
      if (result == sl::Result::eOk) {
        XELOGI("Low latency: NVIDIA Reflex on{}",
               mode == sl::ReflexMode::eLowLatencyWithBoost ? " with boost"
                                                            : "");
      } else {
        XELOGW("Low latency: NVIDIA Reflex failed ({})", uint32_t(result));
      }
    }
  }
  // Sleeps (whether or not it's on, as NVIDIA asks), then the frame starts.
  uint32_t frame = s_frame_started.load(std::memory_order_relaxed) + 1;
  // (0 is no frame.)
  frame += !frame;
  sl::FrameToken* token =
      static_cast<sl::FrameToken*>(dao_streamline::FrameToken(frame));
  if (!token) {
    return;
  }
  s_tokens[frame % sl::MAX_FRAMES_IN_FLIGHT].store(token,
                                                   std::memory_order_release);
  s_reflex_sleep(*token);
  s_pcl_set_marker(sl::PCLMarker::eSimulationStart, *token);
  s_frame_started.store(frame, std::memory_order_release);
}

}  // namespace

void Initialize(ID3D12Device* device) {
  // (S_OK only with an AMD driver that has it: E_HANDLE without one.)
  const HRESULT result = AMD::AntiLag2DX12::Initialize(&s_anti_lag, device);
  s_anti_lag_available = result == S_OK;
  if (s_anti_lag_available) {
    XELOGI("Low latency: AMD Radeon Anti-Lag 2 available");
  } else if (result != E_HANDLE) {
    XELOGW("Low latency: AMD Radeon Anti-Lag 2 isn't available ({:08X})",
           uint32_t(result));
  }
  InitializeReflex();
}

void Shutdown() {
  if (s_anti_lag_available) {
    AMD::AntiLag2DX12::DeInitialize(&s_anti_lag);
    s_anti_lag_available = false;
  }
  s_on = false;
  s_logged_on = false;
  s_reflex = false;
}

bool NeedsRestart() { return dao_streamline::StartsAfterRestart(); }

void FrameStart() {
  if (s_reflex) {
    ReflexFrameStart();
    return;
  }
  if (!s_anti_lag_available) {
    return;
  }
  const bool on = Level() != 0;
  // Off and never on: not a call into the driver every frame.
  if (!on && !s_on) {
    return;
  }
  // (No frame rate limit of Anti-Lag 2's own: the game paces itself.)
  constexpr unsigned int kNoFrameLimit = 0;
  const HRESULT result =
      AMD::AntiLag2DX12::Update(&s_anti_lag, on, kNoFrameLimit);
  s_on = on;
  if (on && !s_logged_on) {
    s_logged_on = true;
    if (result == S_OK) {
      XELOGI("Low latency: AMD Radeon Anti-Lag 2 on");
    } else {
      XELOGW("Low latency: AMD Radeon Anti-Lag 2 failed ({:08X})",
             uint32_t(result));
    }
  }
}

void FrameSwapped() {
  if (!s_reflex) {
    return;
  }
  const uint32_t frame = Advance(s_frame_started, s_frame_swapped);
  if (frame) {
    Mark(sl::PCLMarker::eSimulationEnd, frame);
    Mark(sl::PCLMarker::eRenderSubmitStart, frame);
  }
  ++s_swaps;
  s_swap_frames[s_swaps % kSwaps].store(uint64_t(s_swaps) << 32 | frame,
                                        std::memory_order_release);
}

uint32_t SwapIssued() {
  if (!s_reflex) {
    return 0;
  }
  ++s_swaps_issued;
  const uint64_t swap = s_swap_frames[s_swaps_issued % kSwaps].load(
      std::memory_order_acquire);
  return uint32_t(swap >> 32) == s_swaps_issued ? uint32_t(swap) : 0;
}

void RenderingSubmitted(uint32_t frame) {
  if (!frame) {
    return;
  }
  Mark(sl::PCLMarker::eRenderSubmitEnd, frame);
  s_frame_submitted.store(frame, std::memory_order_release);
}

void BeforePresent() {
  s_frame_presenting = 0;
  if (!s_reflex) {
    return;
  }
  s_frame_presenting = Advance(s_frame_submitted, s_frame_presented);
  if (s_frame_presenting) {
    Mark(sl::PCLMarker::ePresentStart, s_frame_presenting);
  }
}

void AfterPresent() {
  if (s_frame_presenting) {
    Mark(sl::PCLMarker::ePresentEnd, s_frame_presenting);
  }
}

}  // namespace dao_low_latency
}  // namespace d3d12
}  // namespace ui
}  // namespace xe
