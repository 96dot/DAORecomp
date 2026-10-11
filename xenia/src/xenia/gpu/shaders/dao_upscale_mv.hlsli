// DAO: motion vectors for FSR. Each scene draw's vertex shader was run again
// with stream output and without jitter (dao_upscale.cc), giving the
// clip-space position of every vertex, now and, from the matching draw of the
// previous frame, one frame ago. This pass draws those triangles again into
// the motion vector target, keeping only the pixels where they are the visible
// surface in the scene depth. The scene depth is also in the depth buffer, so
// the depth test can reject most of the hidden pixels before the pixel shader
// (conservatively: the vertex shader moves the triangles towards the camera by
// a bit more than the tolerance, and the slope-scaled depth bias by twice the
// larger of the x and y slopes, while the pixel shader allows their sum).
ByteAddressBuffer dao_mv_positions : register(t0);
ByteAddressBuffer dao_mv_previous_positions : register(t1);
Texture2D<float> dao_mv_scene_depth : register(t2);

cbuffer DaoMvConstants : register(b0) {
  // Byte offsets of this draw's current and previous positions (float4 each,
  // in dao_mv_positions and dao_mv_previous_positions) and of the filled-size
  // counters of the stream output buffers they were written into, and where
  // in those buffers they are (the counters count from the buffer start;
  // draws are appended).
  uint dao_mv_current_offset;
  uint dao_mv_previous_offset;
  uint dao_mv_current_counter;
  uint dao_mv_previous_counter;
  uint dao_mv_current_in_buffer;
  uint dao_mv_previous_in_buffer;
  // Host viewport of the draw, in pixels relative to the screen (the window
  // offset of the tile removed).
  float2 dao_mv_viewport_offset;
  float2 dao_mv_viewport_extent;
  float2 dao_mv_render_size;
  // FSR jitter of this frame, in pixels: the scene was rasterized shifted by it.
  float2 dao_mv_jitter;
  float dao_mv_z_min;
  float dao_mv_z_max;
  // Added to the depth for the depth test: towards the camera, negative if
  // nearer is less, positive if nearer is greater, 0 if the draw's depth test
  // is neither (then the pass has none).
  float dao_mv_depth_test_offset;
};

// Larger depth differences from the scene are other surfaces
// (kMotionDepthTolerance in dao_upscale.cc must be the same).
static const float kDaoMvDepthTolerance = 2e-5;

struct DaoMvInterpolators {
  float4 position : SV_Position;
  // Previous clip-space position (x, y, w).
  float3 previous : TEXCOORD0;
  // The depth without the depth test's tolerance (interpolated like
  // SV_Position.z).
  noperspective float depth : TEXCOORD1;
};

// Clip space of the draw's host viewport to screen pixels (y down).
float2 DaoMvToScreen(float2 clip_xy, float w) {
  float2 ndc = clip_xy / w;
  return dao_mv_viewport_offset +
         float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * dao_mv_viewport_extent;
}
