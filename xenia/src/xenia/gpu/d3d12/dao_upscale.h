// DAO: temporal upscaling of 3D scenes with AMD FSR 3.1 / FSR 4, through the
// FidelityFX API (amd_fidelityfx_loader_dx12.dll and
// amd_fidelityfx_upscaler_dx12.dll next to the executable, loaded on demand;
// amd_fidelityfx_framegeneration_dx12.dll for frame generation).
//
// A 3D frame of the game (Eclipse engine, Xbox 360 Direct3D 9):
//   depth prepass, shadow maps -> scene draws (2xMSAA, two predicated tiles,
//   each resolved: color to the scene texture, depth to a depth texture; while
//   upscaling, one single-sampled tile, see WantsSingleSampleScene) ->
//   full-screen post-processing passes reading the scene texture (fog, bloom,
//   the tonemapper) -> UI on the same render target -> resolve to the front
//   buffer -> swap.
// What FSR gets:
//   - jitter: added to the viewport offset of the scene draws (depth-tested
//     multisampled draws, or with the scene single-sampled, depth-tested
//     screen-wide draws before the scene's color resolve), so everything up to
//     the UI is jittered;
//   - depth: the scene depth, copied tile by tile at the first resolve after
//     the tile's draws, from the depth buffer the draws used;
//   - color: the render target right after the last full-screen pass that reads
//     the scene texture (tonemapped, no UI yet);
//   - motion vectors: each opaque scene draw's vertex shader is run again with
//     stream output, unjittered, giving the positions of its vertices; their
//     previous positions are those written by the matching draw of the
//     previous frame (same shaders and vertex data, the closest constants).
//     At the swap, the captured triangles are drawn into the motion vector
//     texture where they match the scene depth. Covers camera, object and
//     skinned motion.
// At the swap, the FSR output is composited with the UI (where the front buffer
// differs from the image FSR got, the front buffer is scaled up instead) and
// replaces the front buffer as the guest output, at the size it is shown at.
// With frame generation, a frame between the previous one and this one is
// generated from the FSR output (dao_upscale_fg.cc) and composited with the UI
// the same way; the presenter shows it before this one.
#pragma once

#include "xenia/ui/d3d12/d3d12_api.h"

#include <atomic>
#include <cstdint>

#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/shader.h"

namespace xe {
namespace gpu {
namespace d3d12 {

class D3D12CommandProcessor;
class D3D12RenderTargetCache;
class DaoUpscalerImpl;

class DaoUpscaler {
 public:
  // Created with the command processor (does nothing unless enabled by the
  // dao_temporal_upscaler cvar).
  static void Initialize(D3D12CommandProcessor& command_processor,
                         D3D12RenderTargetCache& render_target_cache);
  static void Shutdown();

  // Whether the game should render its 3D scenes in one tile without its
  // 2xMSAA: the upscaler antialiases them (from the jitter), so the second
  // tile and the multisampling would only cost GPU time. False again if FSR
  // fails (for good, or for a while). Any thread.
  static bool WantsSingleSampleScene();

  // Before a draw's system constants are computed: jitters scene draws by
  // offsetting viewport_info.ndc_offset.
  static void OnDraw(const RegisterFile& regs,
                     draw_util::ViewportInfo& viewport_info,
                     reg::RB_DEPTHCONTROL normalized_depth_control,
                     uint32_t normalized_color_mask, uint64_t pixel_shader_hash,
                     uint32_t pixel_shader_texture_mask,
                     bool window_offset_in_edram);
  // Right before the draw is recorded, with its pipeline set (skip_motion:
  // memexport or predicated).
  static void BeforeDraw(const Shader& vertex_shader, bool indexed,
                         uint32_t host_vertex_count, bool skip_motion);
  // After the draw has been recorded.
  static void AfterDraw();
  // In a resolve, before the render targets are read.
  static void OnResolve(const draw_util::ResolveInfo& resolve_info);
  // At the swap, after the front buffer texture has been obtained. Returns the
  // image to present instead (at the size of the window area it's shown in)
  // and its view, or nullptr to present the front buffer. hdr_out: the image is
  // the game's HDR (dao_true_hdr), PQ-encoded or scRGB (the presenter's
  // IsHdrScrgb); upscaled_out: by FSR (else the front buffer, only encoded);
  // generated_out: the generated frame to show before it (same size, format
  // and srv_out; recorded, not yet made: see ExecuteFrameGeneration), or
  // nullptr.
  static ID3D12Resource* OnSwap(
      ID3D12Resource* frontbuffer,
      const D3D12_SHADER_RESOURCE_VIEW_DESC& frontbuffer_srv,
      uint32_t frontbuffer_width, uint32_t frontbuffer_height,
      D3D12_SHADER_RESOURCE_VIEW_DESC& srv_out, bool& hdr_out,
      bool& upscaled_out, ID3D12Resource*& generated_out);

 private:
  friend class DaoUpscalerImpl;
  static DaoUpscalerImpl* impl_;
  static std::atomic<bool> single_sample_scene_;
};

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe
