// DAO: copies a rectangle of a host render target into an FSR input texture
// (the scene depth, tile by tile, and the image before the UI is drawn).
// DAO_COPY_TYPE is the texel type, DAO_COPY_MSAA reads sample 0 of an MSAA
// render target.
#ifdef DAO_COPY_MSAA
Texture2DMS<DAO_COPY_TYPE> dao_copy_source : register(t0);
#else
Texture2D<DAO_COPY_TYPE> dao_copy_source : register(t0);
#endif
RWTexture2D<DAO_COPY_TYPE> dao_copy_dest : register(u0);

cbuffer DaoCopyConstants : register(b0) {
  int2 dao_copy_source_offset;
  int2 dao_copy_dest_offset;
  uint2 dao_copy_size;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= dao_copy_size)) {
    return;
  }
  int2 source = int2(id.xy) + dao_copy_source_offset;
#ifdef DAO_COPY_MSAA
  DAO_COPY_TYPE value = dao_copy_source.Load(source, 0);
#else
  DAO_COPY_TYPE value = dao_copy_source.Load(int3(source, 0));
#endif
  dao_copy_dest[int2(id.xy) + dao_copy_dest_offset] = value;
}
