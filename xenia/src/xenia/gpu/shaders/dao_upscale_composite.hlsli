// DAO: puts the game's UI over the FSR-upscaled scene. The UI is drawn after
// the image FSR receives, so it is found by comparing the final frame with that
// image: where they differ, the final frame is scaled up bilinearly instead.
// For HDR output the upscaled scene is linear (1 = the game's white) and the
// result is PQ-encoded (Rec. 709 primaries, decoded by the presenter), or with
// HDR quality High scRGB (linear, 1 = 80 nits, as the presenter's back buffer).
// With motion blur (dao_upscale_composite_blur.cs), the scene is blurred along
// its motion first, so the UI stays sharp.
Texture2D<float4> dao_upscaled : register(t0);  // Display resolution.
Texture2D<float4> dao_final : register(t1);     // Render resolution, with UI.
Texture2D<float4> dao_hudless : register(t2);   // Render resolution, no UI.
Texture2D<float2> dao_motion : register(t3);    // Render resolution.
#ifdef DAO_MOTION_BLUR
// Each pixel's blur length and log depth (render resolution), and the longest
// blur vector around each tile, from dao_motion_blur_tile_max.cs and
// dao_motion_blur_neighbor_max.cs.
Texture2D<float2> dao_blur_pixels : register(t4);
Texture2D<float2> dao_blur_around : register(t5);
#endif
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
  // HDR results are scRGB, not PQ (HDR quality High).
  uint dao_scrgb;
  // Motion blur (DAO_MOTION_BLUR only): samples per pixel, render pixels per
  // tile.
  uint dao_blur_samples;
  uint dao_blur_tile;
};

#define DAO_MODE_SDR 0
#define DAO_MODE_HDR 1
// The HDR scene with the UI, output in SDR.
#define DAO_MODE_HDR_TO_SDR 2
// One source (dao_final) to the destination at its size: HDR-encoded (true HDR
// frames not upscaled), or linear (FSR's HDR input without the game's
// tonemapping pass).
#define DAO_MODE_ENCODE 3
#define DAO_MODE_LINEARIZE 4

// PQ (SMPTE ST 2084) from linear (1 = paper white); the inverse of PqToNits in
// dao_hdr_output.ps.
float3 Pq(float3 linear_color) {
  float3 y = pow(saturate(linear_color * (dao_paper_white / 10000.0)),
                 0.1593017578125);
  return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375);
}

// An HDR result from linear (1 = paper white): PQ, or scRGB.
float3 EncodeHdr(float3 linear_color) {
  return dao_scrgb ? max(linear_color, 0.0) * (dao_paper_white / 80.0)
                   : Pq(linear_color);
}

#ifdef DAO_MOTION_BLUR
// Motion blur: the reconstruction filter of McGuire et al., "A Reconstruction
// Filter for Plausible Motion Blur" (I3D 2012). Each pixel gathers samples
// along the longest blur vector around its tile; a sample counts where it is in
// front and its own blur reaches this pixel, or behind and this pixel's blur
// reaches it, or both blur together. Cost: dao_blur_samples samples (a load
// and a bilinear fetch each) per display pixel, where its tile's neighbourhood
// moves.

// Blur shorter than this (render pixels) is none: all the reach a pixel has.
static const float kBlurMinimum = 0.5;
// Where depths are compared, how much nearer (in log view depth) counts as in
// front.
static const float kBlurSoftDepth = 0.02;
// Where a blur reaches, fading over this fraction of its length either side.
static const float kBlurEdge = 0.05;
// The weight of samples where both blurs reach (the paper's).
static const float kBlurBothWeight = 2.0;

// 1 where a is in front of b, fading out over kBlurSoftDepth.
float InFront(float log_depth_a, float log_depth_b) {
  return saturate(1.0 - (log_depth_a - log_depth_b) / kBlurSoftDepth);
}

// Whether blur reaching distance pixels covers a point at distance: fading
// with the distance (cone), or up to its end (cylinder).
float Cone(float distance, float reach) {
  return saturate(1.0 - distance / reach);
}
float Cylinder(float distance, float reach) {
  return 1.0 - smoothstep((1.0 - kBlurEdge) * reach,
                          (1.0 + kBlurEdge) * reach, distance);
}

// Jimenez's interleaved gradient noise, 0 to 1: offsets the samples per pixel
// so few samples band less.
float Noise(uint2 p) {
  return frac(52.9829189 * frac(dot(float2(p), float2(0.06711056, 0.00583715))));
}

float3 MotionBlur(uint2 id, float2 uv, float3 center) {
  float2 x = uv * dao_render_size;
  int2 last = int2(dao_render_size) - 1;
  int2 px = min(int2(x), last);
  float2 around = dao_blur_around[uint2(px) / dao_blur_tile];
  // Most of a frame, still or slow, isn't blurred.
  if (dot(around, around) < kBlurMinimum * kBlurMinimum) {
    return center;
  }
  // Its blur's length and log view depth.
  float2 pixel_x = dao_blur_pixels[px];
  float reach_x = max(pixel_x.x, kBlurMinimum);
  float depth_x = pixel_x.y;
  float weight = 1.0 / reach_x;
  float3 sum = center * weight;
  float offset = Noise(id) - 0.5;
  for (uint i = 0; i < dao_blur_samples; ++i) {
    float t = lerp(-1.0, 1.0, (float(i) + 1.0 + offset) /
                                  float(dao_blur_samples + 1));
    float2 y = x + around * t;
    float2 pixel_y = dao_blur_pixels[clamp(int2(y), int2(0, 0), last)];
    float reach_y = max(pixel_y.x, kBlurMinimum);
    float depth_y = pixel_y.y;
    float distance = length(y - x);
    float sample_weight =
        InFront(depth_y, depth_x) * Cone(distance, reach_y) +
        InFront(depth_x, depth_y) * Cone(distance, reach_x) +
        Cylinder(distance, reach_y) * Cylinder(distance, reach_x) *
            kBlurBothWeight;
    sum += sample_weight *
           dao_upscaled.SampleLevel(dao_linear, y / dao_render_size, 0.0).rgb;
    weight += sample_weight;
  }
  return sum / weight;
}
#endif  // DAO_MOTION_BLUR

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
        float4(dao_mode == DAO_MODE_ENCODE ? EncodeHdr(color) : color, 1.0);
    return;
  }
  // The four render pixels that bilinear filtering of this pixel reads (only
  // this one at the render size).
  float difference;
  if (all(float2(dao_dest_size) == dao_render_size)) {
    difference = UiDifference(int2(id.xy));
  } else {
    int2 p = int2(floor(uv * dao_render_size - 0.5));
    difference = max(max(UiDifference(p), UiDifference(p + int2(1, 0))),
                     max(UiDifference(p + int2(0, 1)), UiDifference(p + 1)));
  }
  // Both images come from the same render target, so outside the UI they are
  // identical; small differences are faint UI (fades, shadows).
  float ui = saturate(difference * 128.0);
  float3 scene = dao_upscaled[id.xy].rgb;
#ifdef DAO_MOTION_BLUR
  // Not under opaque UI.
  if (ui < 1.0) {
    scene = MotionBlur(id.xy, uv, scene);
  }
#endif
  if (dao_mode == DAO_MODE_HDR_TO_SDR) {
    scene = pow(saturate(scene), 1.0 / dao_gamma);
  }
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
  // (SDR clamped: a 16-bit float destination wouldn't, and the gamma ramp
  // after this reads its table by the value.)
  dao_dest[id.xy] = float4(
      dao_mode == DAO_MODE_HDR ? EncodeHdr(color) : saturate(color), 1.0);
}
