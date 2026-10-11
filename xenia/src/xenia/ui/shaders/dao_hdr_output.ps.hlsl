// DAO: encodes the SDR (gamma-encoded) presentation image into an scRGB (linear,
// 1.0 = 80 nits) back buffer. On HDR displays, scales SDR white to the paper white
// and expands highlights towards the display peak; on SDR displays it only
// linearizes (scale 1, peak ratio 1). A PQ-encoded image (the game's own HDR) is
// decoded instead.
Texture2D<float4> xe_hdr_source : register(t0);

cbuffer XeHdrConstants : register(b0) {
  // Paper white in scRGB units (nits / 80).
  float xe_hdr_scale;
  // Peak luminance / paper white; 1 disables highlight expansion.
  float xe_hdr_peak_ratio;
  // Gamma the game's output is mastered for (2.2 for TVs).
  float xe_hdr_gamma;
  // 1: the image is PQ-encoded (the game's own HDR, its highlights already
  // up to the peak); xe_hdr_scale is then per nit, and the peak ratio and
  // gamma are unused.
  float xe_hdr_pq;
};

float3 PqToNits(float3 pq) {
  float3 p = pow(pq, 1.0f / 78.84375f);
  return 10000.0f * pow(max(p - 0.8359375f, 0.0f) / (18.8515625f - 18.6875f * p),
                        1.0f / 0.1593017578125f);
}

float4 main(float4 position : SV_Position) : SV_Target {
  float3 color = saturate(xe_hdr_source.Load(int3(int2(position.xy), 0)).rgb);
  if (xe_hdr_pq > 0.0f) {
    return float4(PqToNits(color) * xe_hdr_scale, 1.0f);
  }
  float3 linear_color = pow(color, xe_hdr_gamma);
  if (xe_hdr_peak_ratio > 1.0f) {
    // Inverse tonemap on luminance (hue-preserving): identity in the shadows and
    // midtones, rational expansion that maps SDR white to the peak.
    float luminance = dot(linear_color, float3(0.2126f, 0.7152f, 0.0722f));
    float expanded = luminance /
        max(1.0f - luminance * (1.0f - 1.0f / xe_hdr_peak_ratio),
            1.0f / xe_hdr_peak_ratio);
    float weight = smoothstep(0.18f, 1.0f, luminance);
    float target = lerp(luminance, expanded, weight * weight);
    linear_color *= luminance > 0.0f ? target / luminance : 1.0f;
  }
  return float4(linear_color * xe_hdr_scale, 1.0f);
}
