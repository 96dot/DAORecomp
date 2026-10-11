// DAO: in place of the gamma ramp pass for a PQ-encoded (HDR) frame: copies it
// to the presenter's guest output, with the game's 256-entry gamma ramp (its
// brightness) applied where the image is within SDR white, as on the SDR
// output; brighter values are left as they are (the ramp ends at white).
// The bindings of apply_gamma_table.cs, with more constants.
Buffer<float4> dao_ramp : register(t0);  // BGR in RGB.
Texture2D<float4> dao_source : register(t1);
RWTexture2D<float4> dao_dest : register(u0);

cbuffer DaoPqCopyConstants : register(b0) {
  uint2 dao_size;
  float dao_paper_white;  // Nits.
  float dao_gamma;        // Of the game's output.
  // 0: the ramp isn't a 256-entry table; copied unchanged.
  uint dao_use_ramp;
};

float3 PqToNits(float3 pq) {
  float3 p = pow(saturate(pq), 1.0 / 78.84375);
  return 10000.0 * pow(max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p),
                       1.0 / 0.1593017578125);
}

float3 NitsToPq(float3 nits) {
  float3 y = pow(saturate(nits / 10000.0), 0.1593017578125);
  return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375);
}

// The ramp between its entries.
float3 Ramp(float3 encoded) {
  float3 index = saturate(encoded) * 255.0;
  uint3 i0 = uint3(index);
  uint3 i1 = min(i0 + 1, 255);
  float3 t = index - float3(i0);
  return float3(lerp(dao_ramp[i0.r].b, dao_ramp[i1.r].b, t.r),
                lerp(dao_ramp[i0.g].g, dao_ramp[i1.g].g, t.g),
                lerp(dao_ramp[i0.b].r, dao_ramp[i1.b].r, t.b));
}

[numthreads(16, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= dao_size)) {
    return;
  }
  float3 color = dao_source[id.xy].rgb;
  if (dao_use_ramp) {
    float3 relative = PqToNits(color) / dao_paper_white;
    float3 ramped = pow(Ramp(pow(relative, 1.0 / dao_gamma)), dao_gamma);
    color = NitsToPq(
        (relative <= 1.0 ? ramped : relative) * dao_paper_white);
  }
  dao_dest[id.xy] = float4(color, 1.0);
}
