// DAO: puts the game's UI over the FSR-upscaled scene. The UI is drawn after
// the image FSR receives, so it is found by comparing the final frame with that
// image: where they differ, the final frame is scaled up bilinearly instead.
Texture2D<float4> dao_upscaled : register(t0);  // Display resolution.
Texture2D<float4> dao_final : register(t1);     // Render resolution, with UI.
Texture2D<float4> dao_hudless : register(t2);   // Render resolution, no UI.
Texture2D<float2> dao_motion : register(t3);    // Render resolution.
SamplerState dao_linear : register(s0);
RWTexture2D<float4> dao_dest : register(u0);

cbuffer DaoCompositeConstants : register(b0) {
  uint2 dao_dest_size;
  float2 dao_render_size;
  // Shows the motion vectors instead (red: x, green: y, 32 pixels from the
  // middle to full; gray: none).
  uint dao_show_motion;
};

float UiDifference(int2 p) {
  p = clamp(p, int2(0, 0), int2(dao_render_size) - 1);
  float3 d = abs(dao_final.Load(int3(p, 0)).rgb - dao_hudless.Load(int3(p, 0)).rgb);
  return max(d.r, max(d.g, d.b));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= dao_dest_size)) {
    return;
  }
  float2 uv = (float2(id.xy) + 0.5) / float2(dao_dest_size);
  // The four render pixels that bilinear filtering of this pixel reads.
  int2 p = int2(floor(uv * dao_render_size - 0.5));
  float difference = max(max(UiDifference(p), UiDifference(p + int2(1, 0))),
                         max(UiDifference(p + int2(0, 1)), UiDifference(p + 1)));
  // Both images come from the same render target, so outside the UI they are
  // identical; small differences are faint UI (fades, shadows).
  float ui = saturate(difference * 128.0);
  float3 scene = dao_upscaled[id.xy].rgb;
  float3 final = dao_final.SampleLevel(dao_linear, uv, 0.0).rgb;
  float3 color = lerp(scene, final, ui);
  if (dao_show_motion) {
    float2 motion = dao_motion.Load(int3(int2(uv * dao_render_size), 0));
    float gray = dot(scene, float3(0.299, 0.587, 0.114));
    color = any(abs(motion) > 0.05)
                ? float3(saturate(0.5 + motion / 32.0), gray * 0.5)
                : gray.xxx * 0.5;
  }
  dao_dest[id.xy] = float4(color, 1.0);
}
