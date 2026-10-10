#include "dao_upscale_mv.hlsli"

float2 main(DaoMvInterpolators input) : SV_Target {
  // Only where this surface is what the scene shows (whichever way the depth
  // test goes). The scene depth is MSAA sample 0, not the pixel center, so
  // allow for the depth slope.
  float scene_depth = dao_mv_scene_depth.Load(int3(int2(input.position.xy), 0));
  float slope = abs(ddx(input.position.z)) + abs(ddy(input.position.z));
  if (abs(input.position.z - scene_depth) > 2e-5 + slope) {
    discard;
  }
  // From the pixel now to the same point one frame ago, in pixels, unjittered.
  float2 current = input.position.xy - dao_mv_jitter;
  float2 previous = DaoMvToScreen(input.previous.xy, input.previous.z);
  return previous - current;
}
