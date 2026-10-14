// DAO: low latency - see dao_low_latency.h.
#include "xenia/ui/d3d12/dao_low_latency.h"

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/dao_live_settings.h"

#include "ffx_antilag2_dx12.h"

DEFINE_int32(dao_low_latency, 0,
             "Low latency: the GPU's own technology (AMD Radeon Anti-Lag 2) "
             "starts each frame as late as it can still be on time, so the "
             "controller is read closer to when the frame is shown. 0 off, 1 "
             "on, 2 on with boost (Anti-Lag 2 has no boost: the same as 1).",
             "Display");

namespace xe {
namespace ui {
namespace d3d12 {
namespace dao_low_latency {

namespace {

// Set up by Initialize, before the game's thread starts, and taken down by
// Shutdown after it has stopped; in between, FrameStart's (the game thread's).
AMD::AntiLag2DX12::Context s_anti_lag = {};
bool s_anti_lag_available = false;
// Whether it was on at the last FrameStart.
bool s_on = false;
bool s_logged_on = false;

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
}

void Shutdown() {
  if (s_anti_lag_available) {
    AMD::AntiLag2DX12::DeInitialize(&s_anti_lag);
    s_anti_lag_available = false;
  }
  s_on = false;
  s_logged_on = false;
}

void FrameStart() {
  if (!s_anti_lag_available) {
    return;
  }
  const bool on = dao_live::Load(dao_live::values.low_latency) != 0 &&
                  !dao_live::Load(dao_live::values.frame_generation);
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

}  // namespace dao_low_latency
}  // namespace d3d12
}  // namespace ui
}  // namespace xe
