#include "dao_upscale_mv.hlsli"

// The depth test, if any, doesn't write depth: fine to do before discarding.
[earlydepthstencil]
float2 main(DaoMvInterpolators input) : SV_Target {
  // Only where this surface is what the scene shows (whichever way the depth
  // test goes). The scene depth is MSAA sample 0, not the pixel center, so
  // allow for the depth slope.
  float scene_depth = dao_mv_scene_depth.Load(int3(int2(input.position.xy), 0));
  float slope = abs(ddx(input.depth)) + abs(ddy(input.depth));
  if (abs(input.depth - scene_depth) > kDaoMvDepthTolerance + slope) {
    discard;
  }
  // From the pixel now to the same point one frame ago, in pixels, unjittered.
  // previous.z is the point's w a frame ago. At or behind the camera (w near 0
  // or below) it had no place on screen then, and the divide by w would give
  // infinities FSR spreads over the image: no motion. Anything else not finite
  // (a NaN from the game's own math) is left out the same way.
  if (input.previous.z <= 1.0e-6) {
    return float2(0.0, 0.0);
  }
  float2 current = input.position.xy - dao_mv_jitter;
  float2 previous = DaoMvToScreen(input.previous.xy, input.previous.z);
  float2 motion = previous - current;
  return all(isfinite(motion)) ? motion : float2(0.0, 0.0);
}
