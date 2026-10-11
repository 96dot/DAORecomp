// DAO: writes a resolved area of a host render target into a texture of the
// resolved memory, the same texels the resolve and the texture's untiling would
// give (see D3D12TextureCache::WriteResolvedTextures).
Texture2D<float4> dao_resolve_source : register(t0);
RWTexture2D<float4> dao_resolve_dest : register(u0);

cbuffer DaoResolveTextureConstants : register(b0) {
  uint2 dao_resolve_source_offset;
  uint2 dao_resolve_dest_offset;
  uint2 dao_resolve_size;
  // The first columns / rows of the area copy the next one (the half-pixel
  // offset fill of scaled resolves).
  uint2 dao_resolve_fill;
  // The source channel of each destination channel, 2 bits each (red / blue
  // swap and byte swaps of the resolve and the texture).
  uint dao_resolve_channels;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= dao_resolve_size)) {
    return;
  }
  float4 source = dao_resolve_source.Load(
      int3(dao_resolve_source_offset + max(id.xy, dao_resolve_fill), 0));
  float4 dest;
  dest.r = source[dao_resolve_channels & 3u];
  dest.g = source[(dao_resolve_channels >> 2u) & 3u];
  dest.b = source[(dao_resolve_channels >> 4u) & 3u];
  dest.a = source[(dao_resolve_channels >> 6u) & 3u];
  dao_resolve_dest[dao_resolve_dest_offset + id.xy] = dest;
}
