// DAO: temporal upscaling of 3D scenes with AMD FSR 3.1 / FSR 4, through the
// FidelityFX API (amd_fidelityfx_loader_dx12.dll and
// amd_fidelityfx_upscaler_dx12.dll next to the executable, loaded on demand).
//
// A 3D frame of the game (Eclipse engine, Xbox 360 Direct3D 9):
//   depth prepass, shadow maps -> scene draws (2xMSAA, two predicated tiles,
//   each resolved: color to the scene texture, depth to a depth texture) ->
//   full-screen post-processing passes reading the scene texture (fog, bloom,
//   the tonemapper) -> UI on the same render target -> resolve to the front
//   buffer -> swap.
// What FSR gets:
//   - jitter: added to the viewport offset of the scene draws (depth-tested
//     multisampled draws), so everything up to the UI is jittered;
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
#pragma once

#include "xenia/ui/d3d12/d3d12_api.h"

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
  // and its view, or nullptr to present the front buffer.
  static ID3D12Resource* OnSwap(
      ID3D12Resource* frontbuffer,
      const D3D12_SHADER_RESOURCE_VIEW_DESC& frontbuffer_srv,
      uint32_t frontbuffer_width, uint32_t frontbuffer_height,
      D3D12_SHADER_RESOURCE_VIEW_DESC& srv_out);

 private:
  static DaoUpscalerImpl* impl_;
};

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe
