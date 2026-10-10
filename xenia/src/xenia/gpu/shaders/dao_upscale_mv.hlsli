// DAO: motion vectors for FSR. Each scene draw's vertex shader was run twice
// with stream output (dao_upscale.cc): with this frame's constants and with
// the previous frame's, both without jitter, giving the clip-space position of
// every vertex now and one frame ago. This pass draws those triangles again
// into the motion vector target, keeping only the pixels where they are the
// visible surface in the scene depth.
ByteAddressBuffer dao_mv_positions : register(t0);
Texture2D<float> dao_mv_scene_depth : register(t1);

cbuffer DaoMvConstants : register(b0) {
  // Byte offsets of this draw's current and previous positions (float4 each)
  // and of their stream output filled-size counters.
  uint dao_mv_current_offset;
  uint dao_mv_previous_offset;
  uint dao_mv_current_counter;
  uint dao_mv_previous_counter;
  // Host viewport of the draw, in pixels relative to the screen (the window
  // offset of the tile removed).
  float2 dao_mv_viewport_offset;
  float2 dao_mv_viewport_extent;
  float2 dao_mv_render_size;
  // FSR jitter of this frame, in pixels: the scene was rasterized shifted by it.
  float2 dao_mv_jitter;
  float dao_mv_z_min;
  float dao_mv_z_max;
};

struct DaoMvInterpolators {
  float4 position : SV_Position;
  // Previous clip-space position (x, y, w).
  float3 previous : TEXCOORD0;
};

// Clip space of the draw's host viewport to screen pixels (y down).
float2 DaoMvToScreen(float2 clip_xy, float w) {
  float2 ndc = clip_xy / w;
  return dao_mv_viewport_offset +
         float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * dao_mv_viewport_extent;
}
