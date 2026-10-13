// DAO recomp: the settings the in-game Graphics menu changes while the game runs
// (DAORecomp/ui/graphics_menu.cpp), for the threads that read them every frame
// (GPU, presenter, the game's render thread). Their cvars are plain globals, set
// only before those threads start; these are their copies that can change while
// the threads read them: loaded from the cvars right after the config
// (LoadXeniaLiveSettings in DAORecomp/gpu/xenia_host.cpp, before any of those
// threads starts; the initial values below are only placeholders until then),
// and set by the menu after. Each is read on its own, with relaxed ordering (a
// frame may pair a new value with an old one of another: harmless here).
#ifndef XENIA_DAO_LIVE_SETTINGS_H_
#define XENIA_DAO_LIVE_SETTINGS_H_

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace xe::dao_live {

struct Values {
  std::atomic<bool> present_vsync{true};          // dao_present_vsync
  std::atomic<double> temporal_sharpness{0.3};    // dao_temporal_sharpness
  std::atomic<int32_t> anisotropic_override{-1};  // anisotropic_override
  std::atomic<int32_t> motion_blur{0};            // dao_motion_blur
  std::atomic<double> hdr_paper_white{200.0};     // present_hdr_paper_white
  std::atomic<double> hdr_peak{1000.0};           // present_hdr_peak
};

inline Values values;

template <typename T>
T Load(const std::atomic<T>& value) {
  return value.load(std::memory_order_relaxed);
}

// HDR: SDR white in nits, at least the scRGB unit (80 nits).
inline float HdrPaperWhite() {
  return float(std::max(Load(values.hdr_paper_white), 80.0));
}

// HDR: the peak over SDR white, at least 1.
inline float HdrPeakRatio(float paper_white) {
  return std::max(float(Load(values.hdr_peak)) / paper_white, 1.0f);
}

}  // namespace xe::dao_live

#endif  // XENIA_DAO_LIVE_SETTINGS_H_
