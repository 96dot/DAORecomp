// DAO: in place of the gamma ramp for a PQ-encoded (HDR) frame, which the ramp
// is not made for: copies it to the presenter's guest output. The bindings of
// apply_gamma_table.cs (the ramp, t0, is unused).
Texture2D<float4> dao_source : register(t1);
RWTexture2D<float4> dao_dest : register(u0);

cbuffer DaoPqCopyConstants : register(b0) {
  uint2 dao_size;
};

[numthreads(16, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= dao_size)) {
    return;
  }
  dao_dest[id.xy] = float4(dao_source[id.xy].rgb, 1.0);
}
