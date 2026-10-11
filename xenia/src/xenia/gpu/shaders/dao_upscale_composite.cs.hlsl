// DAO: puts the game's UI over the FSR-upscaled scene. The UI is drawn after
// the image FSR receives, so it is found by comparing the final frame with that
// image: where they differ, the final frame is scaled up bilinearly instead.
// For HDR output the upscaled scene is linear (1 = the game's white) and the
// result is PQ-encoded (Rec. 709 primaries, decoded by the presenter).
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
  // DAO_MODE_* (CompositeMode in dao_upscale.cc).
  uint dao_mode;
  float dao_paper_white;  // Nits.
  float dao_gamma;        // Of the game's output.
};

#define DAO_MODE_SDR 0
#define DAO_MODE_HDR 1
// One source (dao_final) to the destination at its size: PQ-encoded (true HDR
// frames not upscaled), or linear (FSR's HDR input without the game's
// tonemapping pass).
#define DAO_MODE_ENCODE 2
#define DAO_MODE_LINEARIZE 3

// PQ (SMPTE ST 2084) from linear (1 = paper white); the inverse of PqToNits in
// dao_hdr_output.ps.
float3 Pq(float3 linear_color) {
  float3 y = pow(saturate(linear_color * (dao_paper_white / 10000.0)),
                 0.1593017578125);
  return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375);
}

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
  if (dao_mode >= DAO_MODE_ENCODE) {
    float3 color = pow(saturate(dao_final.Load(int3(id.xy, 0)).rgb), dao_gamma);
    dao_dest[id.xy] =
        float4(dao_mode == DAO_MODE_ENCODE ? Pq(color) : color, 1.0);
    return;
  }
  // The four render pixels that bilinear filtering of this pixel reads.
  int2 p = int2(floor(uv * dao_render_size - 0.5));
  float difference = max(max(UiDifference(p), UiDifference(p + int2(1, 0))),
                         max(UiDifference(p + int2(0, 1)), UiDifference(p + 1)));
  // Both images come from the same render target, so outside the UI they are
  // identical; small differences are faint UI (fades, shadows).
  float ui = saturate(difference * 128.0);
  float3 scene = dao_upscaled[id.xy].rgb;
  float3 color = scene;
  // Most of the screen has no UI.
  if (ui > 0.0) {
    float3 final = dao_final.SampleLevel(dao_linear, uv, 0.0).rgb;
    if (dao_mode == DAO_MODE_HDR) {
      final = pow(saturate(final), dao_gamma);
    }
    color = lerp(scene, final, ui);
  }
  if (dao_show_motion) {
    float2 motion = dao_motion.Load(int3(int2(uv * dao_render_size), 0));
    float gray = dot(scene, float3(0.299, 0.587, 0.114));
    color = any(abs(motion) > 0.05)
                ? float3(saturate(0.5 + motion / 32.0), gray * 0.5)
                : gray.xxx * 0.5;
  }
  dao_dest[id.xy] = float4(dao_mode == DAO_MODE_HDR ? Pq(color) : color, 1.0);
}
