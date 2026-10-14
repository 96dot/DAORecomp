// DAO: encodes the SDR (gamma-encoded) presentation image into an scRGB (linear,
// 1.0 = 80 nits) back buffer. On HDR displays, scales SDR white to the paper white
// and expands highlights towards the display peak; on SDR displays it only
// linearizes (scale 1, peak ratio 1). The game's own HDR is decoded from PQ
// instead, or passed through when it's already scRGB (HDR quality High, with
// presenter UI drawn over it; otherwise the presenter paints it to the back
// buffer itself). Presenter UI over the game's own HDR comes in a layer of its
// own (SDR, premultiplied), put over the image at an SDR image's white.
Texture2D<float4> xe_hdr_source : register(t0);
Texture2D<float4> xe_hdr_ui : register(t1);

cbuffer XeHdrConstants : register(b0) {
  // Paper white in scRGB units (nits / 80).
  float xe_hdr_scale;
  // Peak luminance / paper white; 1 disables highlight expansion.
  float xe_hdr_peak_ratio;
  // Gamma the game's output is mastered for (2.2 for TVs).
  float xe_hdr_gamma;
  // The image: 0 gamma-encoded (SDR); 1 PQ-encoded (the game's own HDR, its
  // highlights already up to the peak), xe_hdr_scale then per nit; 2 scRGB
  // (the game's own HDR, HDR quality High), copied. The peak ratio and gamma
  // are only for SDR.
  float xe_hdr_encoding;
  // The UI layer's white in scRGB units; 0: no UI layer.
  float xe_hdr_ui_scale;
};

// SMPTE ST 2084 (PQ) to nits; as PqToNits in dao_presenter.cc (keep in step).
float3 PqToNits(float3 pq) {
  float3 p = pow(pq, 1.0f / 78.84375f);
  return 10000.0f * pow(max(p - 0.8359375f, 0.0f) / (18.8515625f - 18.6875f * p),
                        1.0f / 0.1593017578125f);
}

// The image at a pixel, in scRGB (linear, 1 = 80 nits), by xe_hdr_encoding.
float3 Image(int2 position) {
  float3 color = xe_hdr_source.Load(int3(position, 0)).rgb;
  if (xe_hdr_encoding > 1.5f) {
    return max(color, 0.0f);
  }
  color = saturate(color);
  if (xe_hdr_encoding > 0.5f) {
    return PqToNits(color) * xe_hdr_scale;
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
  return linear_color * xe_hdr_scale;
}

float4 main(float4 position : SV_Position) : SV_Target {
  int2 pixel = int2(position.xy);
  float3 color = Image(pixel);
  if (xe_hdr_ui_scale > 0.0f) {
    // Over: the UI's colour (gamma-encoded, so un-premultiplied first)
    // linearized at the UI's white.
    float4 ui = xe_hdr_ui.Load(int3(pixel, 0));
    if (ui.a > 0.0f) {
      float3 ui_color = pow(saturate(ui.rgb / ui.a), xe_hdr_gamma);
      color = color * (1.0f - ui.a) + ui_color * xe_hdr_ui_scale * ui.a;
    }
  }
  return float4(color, 1.0f);
}
