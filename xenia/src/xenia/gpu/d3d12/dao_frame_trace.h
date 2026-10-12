// DAO: one-frame draw trace for finding render passes (e.g. the tonemapper).
// DAO_GPU_FRAME_TRACE=<swap number> logs, for the frame after that swap, every
// draw (shaders, render targets, sampled textures), resolve and the swap itself
// to xenia.log with the "FT" prefix.
// Also keeps per-swap draw counts (always on) and, with DAO_FRAME_HISTORY, the
// draw lists for the flicker detector in DAORecomp/gpu/frame_history.cpp.
#pragma once

#include <array>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/gpu/xenos.h"

namespace xe::gpu::dao_trace {

inline uint32_t& SwapCount() {
  static uint32_t count = 0;
  return count;
}

// Draws of one frame: submitted by the guest (draw packets), skipped by VIZ
// predication, failed in the backend, and issued to the host GPU (the rest were
// silently dropped by the backend, e.g. shaders or pipelines not ready).
struct FrameStats {
  uint32_t swap = 0;
  uint32_t submitted = 0;
  uint32_t viz_skipped = 0;
  uint32_t failed = 0;
  uint32_t issued = 0;
};

inline FrameStats& CurrentStats() {
  static FrameStats stats;
  return stats;
}

constexpr uint32_t kStatsHistory = 256;

inline std::array<FrameStats, kStatsHistory>& StatsHistory() {
  static std::array<FrameStats, kStatsHistory> history;
  return history;
}

inline std::atomic<uint32_t>& StatsWritten() {
  static std::atomic<uint32_t> written{0};
  return written;
}

// Copies the stats of GPU swap `swap` (if still in the history) to `out`.
inline bool GetFrameStats(uint32_t swap, FrameStats& out) {
  const uint32_t written = StatsWritten().load(std::memory_order_acquire);
  if (swap >= written || written - swap > kStatsHistory - 8) {
    return false;
  }
  out = StatsHistory()[swap % kStatsHistory];
  return out.swap == swap;
}

// A guest draw, identified by its index buffer, first vertex fetch constant and
// size.
struct DrawRecord {
  uint32_t index_base;
  uint32_t vertex_fetch0;
  uint32_t count;
  uint32_t prim;
};

inline std::vector<DrawRecord>& CurrentDraws() {
  static std::vector<DrawRecord> draws;
  return draws;
}

inline std::mutex& DrawHistoryMutex() {
  static std::mutex mutex;
  return mutex;
}

inline std::array<std::vector<DrawRecord>, kStatsHistory>& DrawHistory() {
  static std::array<std::vector<DrawRecord>, kStatsHistory> history;
  return history;
}

// Copies the draws of GPU swap `swap` (if still in the history) to `out`.
inline bool GetFrameDraws(uint32_t swap, std::vector<DrawRecord>& out) {
  FrameStats stats;
  if (!GetFrameStats(swap, stats)) {
    return false;
  }
  std::lock_guard lock(DrawHistoryMutex());
  out = DrawHistory()[swap % kStatsHistory];
  return true;
}

inline void CountSubmittedDraw(const RegisterFile& regs, uint32_t index_base,
                               uint32_t count, uint32_t prim) {
  ++CurrentStats().submitted;
  // The draw lists are only for DAO_FRAME_HISTORY's flicker reports (main.cpp
  // starts the detector on the same variable).
  static const bool record_draws = std::getenv("DAO_FRAME_HISTORY") != nullptr;
  if (!record_draws) {
    return;
  }
  CurrentDraws().push_back(
      {index_base,
       regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0] & ~uint32_t(3),
       count, prim});
}
inline void CountVizSkippedDraw() { ++CurrentStats().viz_skipped; }
inline void CountFailedDraw() { ++CurrentStats().failed; }

inline bool Active() {
  static const long target = [] {
    const char* env = std::getenv("DAO_GPU_FRAME_TRACE");
    return env ? std::atol(env) : -1L;
  }();
  return target >= 0 && SwapCount() == uint32_t(target) + 1;
}

inline uint32_t& DrawIndex() {
  static uint32_t index = 0;
  return index;
}

inline void LogDraw(const RegisterFile& regs, uint64_t vs_hash,
                    uint64_t ps_hash, uint32_t color_mask,
                    uint32_t ps_texture_mask, uint32_t vs_texture_mask,
                    xenos::PrimitiveType primitive_type,
                    uint32_t index_count) {
  ++CurrentStats().issued;
  if (!Active()) {
    return;
  }
  std::string line =
      fmt::format("FT draw {:4} VS {:016X} PS {:016X} prim {} count {}",
                  DrawIndex()++, vs_hash, ps_hash, uint32_t(primitive_type),
                  index_count);
  const auto surface = regs.Get<reg::RB_SURFACE_INFO>();
  line += fmt::format(" pitch {} msaa {}", surface.surface_pitch,
                      uint32_t(surface.msaa_samples));
  for (uint32_t i = 0; i < 4; ++i) {
    if (!((color_mask >> (i * 4)) & 0xF)) {
      continue;
    }
    const auto info = regs.Get<reg::RB_COLOR_INFO>(
        reg::RB_COLOR_INFO::rt_register_indices[i]);
    line += fmt::format(
        " | RT{} {} base {} bias {}", i,
        xenos::GetColorRenderTargetFormatName(info.color_format),
        info.color_base | (info.color_base_bit_11 << 11), info.color_exp_bias);
  }
  const auto depth = regs.Get<reg::RB_DEPTH_INFO>();
  const auto depth_control = regs.Get<reg::RB_DEPTHCONTROL>();
  if (depth_control.z_enable || depth_control.stencil_enable) {
    line += fmt::format(
        " | DS {} base {}",
        xenos::GetDepthRenderTargetFormatName(depth.depth_format),
        depth.depth_base | (depth.depth_base_bit_11 << 11));
  }
  for (uint32_t pass = 0; pass < 2; ++pass) {
    uint32_t mask = pass ? vs_texture_mask : ps_texture_mask;
    while (mask) {
      const uint32_t i = uint32_t(std::countr_zero(mask));
      mask &= mask - 1;
      const auto fetch = regs.GetTextureFetch(i);
      const FormatInfo* format = FormatInfo::Get(fetch.format);
      line += fmt::format(
          " | {}tex{} fmt {} ({}bpp{}) {}x{} dim {} @{:08X} pitch {} tiled {} endian {} exp {} sign {}",
          pass ? "VS " : "", i, uint32_t(fetch.format),
          format ? format->bits_per_pixel : 0,
          format && format->type == FormatType::kCompressed ? " bc" : "",
          fetch.size_2d.width + 1, fetch.size_2d.height + 1,
          uint32_t(fetch.dimension), fetch.base_address << 12,
          fetch.pitch << 5, uint32_t(fetch.tiled), uint32_t(fetch.endianness),
          fetch.exp_adjust, uint32_t(fetch.sign_x));
    }
  }
  XELOGI("{}", line);
}

inline void LogResolve(const RegisterFile& regs) {
  if (!Active()) {
    return;
  }
  const auto control = regs.Get<reg::RB_COPY_CONTROL>();
  const auto dest = regs.Get<reg::RB_COPY_DEST_INFO>();
  uint32_t src_base = 0;
  const char* src_format = "depth";
  if (control.copy_src_select < 4) {
    const auto info = regs.Get<reg::RB_COLOR_INFO>(
        reg::RB_COLOR_INFO::rt_register_indices[control.copy_src_select]);
    src_base = info.color_base | (info.color_base_bit_11 << 11);
    src_format = xenos::GetColorRenderTargetFormatName(info.color_format);
  }
  XELOGI(
      "FT resolve {:4} src RT{} {} base {} -> fmt {} @{:08X} exp_bias {} "
      "clear c{} d{}",
      DrawIndex()++, control.copy_src_select, src_format, src_base,
      uint32_t(dest.copy_dest_format),
      regs[XE_GPU_REG_RB_COPY_DEST_BASE], dest.copy_dest_exp_bias,
      uint32_t(control.color_clear_enable),
      uint32_t(control.depth_clear_enable));
}

inline void OnSwap(uint32_t frontbuffer_ptr) {
  if (Active()) {
    XELOGI("FT swap frontbuffer @{:08X}", frontbuffer_ptr);
  }
  FrameStats& stats = CurrentStats();
  stats.swap = SwapCount();
  {
    std::lock_guard lock(DrawHistoryMutex());
    DrawHistory()[stats.swap % kStatsHistory].swap(CurrentDraws());
  }
  CurrentDraws().clear();
  StatsHistory()[stats.swap % kStatsHistory] = stats;
  StatsWritten().store(stats.swap + 1, std::memory_order_release);
  stats = {};
  ++SwapCount();
  DrawIndex() = 0;
}

}  // namespace xe::gpu::dao_trace
