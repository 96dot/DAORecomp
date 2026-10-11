// DAO: the game's tonemapping pass redone for HDR, for FSR's HDR input. The
// game adds the bloom to the scene, applies a color matrix, then a curve per
// channel (255 entries of 8 bits, tinting the image) that is about linear in
// gamma space up to a shoulder compressing the highlights to white. Here the
// image is the game's, brightened where the shoulder darkens it: by how much
// the curve's linear part, continued, exceeds the curve at the brightest
// channel (one factor for all three, keeping the game's hues and tint), rolled
// off towards the display's peak. Output: linear, 1 = the game's white.
Texture2DArray<float4> dao_bloom : register(t0);  // Alpha: bloom strength.
Texture2DArray<float4> dao_scene : register(t1);
Texture2DArray<float4> dao_curve : register(t2);
SamplerState dao_linear : register(s0);
RWTexture2D<float4> dao_dest : register(u0);

cbuffer DaoHdrTonemapConstants : register(b0) {
  // The color matrix (from the game's vertex shader); w multiplies dao_offset.
  float4 dao_rows[3];
  uint2 dao_size;
  float dao_bloom_scale;
  float dao_offset;
  float dao_curve_v;
  // The gamma the curve's output is encoded with.
  float dao_gamma;
  // Display peak / paper white; 1 gives the game's image (linear).
  float dao_peak_ratio;
  // Curve input where its linear part is measured and continued from.
  float dao_knee;
};

float4 Curve(float x) {
  return dao_curve.SampleLevel(dao_linear, float3(x, dao_curve_v, 0.0), 0.0);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= dao_size)) {
    return;
  }
  float3 uv = float3((float2(id.xy) + 0.5) / float2(dao_size), 0.0);
  float4 bloom = dao_bloom.SampleLevel(dao_linear, uv, 0.0);
  float4 v = float4(
      bloom.rgb * (bloom.a * dao_bloom_scale) +
          dao_scene.SampleLevel(dao_linear, uv, 0.0).rgb,
      dao_offset);
  float3 x = float3(dot(dao_rows[0], v), dot(dao_rows[1], v),
                    dot(dao_rows[2], v));
  float3 color = pow(saturate(float3(Curve(x.r).r, Curve(x.g).g,
                                     Curve(x.b).b)), dao_gamma);
  float m = max(x.r, max(x.g, x.b));
  if (dao_peak_ratio > 1.0 && m > dao_knee) {
    // On the green curve (the least tinted): its linear part continued, from
    // the slope just below the knee (the same for the whole screen, so cached
    // by the texture unit), against the curve itself.
    // About 4 of the curve's 255 entries.
    const float step = 1.0 / 64.0;
    float knee = Curve(dao_knee).g;
    float slope = max(knee - Curve(dao_knee - step).g, 0.0) / step;
    float target = pow(knee + slope * (m - dao_knee), dao_gamma);
    float headroom = dao_peak_ratio - 1.0;
    target = min(target, 1.0) +
             headroom * (1.0 - exp(-max(target - 1.0, 0.0) / headroom));
    float curve = pow(saturate(Curve(m).g), dao_gamma);
    // (The curve above the knee is far from 0; this only avoids dividing by
    // it.)
    color *= max(target / max(curve, 1.0 / 1024.0), 1.0);
    color *= min(dao_peak_ratio / max(color.r, max(color.g, color.b)), 1.0);
  }
  dao_dest[id.xy] = float4(color, 1.0);
}
