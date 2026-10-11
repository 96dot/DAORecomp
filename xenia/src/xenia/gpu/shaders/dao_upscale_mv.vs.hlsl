#include "dao_upscale_mv.hlsli"

DaoMvInterpolators main(uint vertex_id : SV_VertexID) {
  DaoMvInterpolators output;
  uint byte_index = vertex_id * 16;
  // Vertices past what stream output wrote (strip cuts) are culled.
  if (dao_mv_current_in_buffer + byte_index >=
          dao_mv_positions.Load(dao_mv_current_counter) ||
      dao_mv_previous_in_buffer + byte_index >=
          dao_mv_previous_positions.Load(dao_mv_previous_counter)) {
    output.position = asfloat(0x7FC00000).xxxx;
    output.previous = float3(0.0, 0.0, 1.0);
    output.depth = 0.0;
    return output;
  }
  float4 current =
      asfloat(dao_mv_positions.Load4(dao_mv_current_offset + byte_index));
  float4 previous = asfloat(
      dao_mv_previous_positions.Load4(dao_mv_previous_offset + byte_index));
  // Written as drawn, jittered: without the jitter.
  current.xy -= dao_mv_current_ndc_jitter * current.w;
  previous.xy -= dao_mv_previous_ndc_jitter * previous.w;
  // Rasterize where the scene was: the current position plus the jitter, on
  // the full-frame motion vector target, with the draw's depth range.
  float2 screen = DaoMvToScreen(current.xy, current.w) + dao_mv_jitter;
  float2 ndc = float2(screen.x / dao_mv_render_size.x * 2.0 - 1.0,
                      1.0 - screen.y / dao_mv_render_size.y * 2.0);
  float depth = dao_mv_z_min + current.z / current.w * (dao_mv_z_max - dao_mv_z_min);
  output.position =
      float4(ndc, depth + dao_mv_depth_test_offset, 1.0) * current.w;
  output.previous = previous.xyw;
  output.depth = depth;
  return output;
}
