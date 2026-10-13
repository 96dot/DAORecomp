// DAO: motion blur, second pass: for each tile, the longest blur vector of the
// tile and its eight neighbours (from dao_motion_blur_tile_max.cs), so a pixel
// knows how far blur from outside its tile can reach it. None at all when
// enough tiles had a camera cut: then the whole frame is one (a conversation's
// shot changing), and parts of it moving less than the cut would streak.
#include "dao_motion_blur.hlsli"

Texture2D<float2> dao_tiles : register(t0);
RWTexture2D<float2> dao_dest : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (all(id.xy == 0)) {
    dao_cuts[uint2(dao_counter ^ 1, 0)] = 0;
  }
  if (any(id.xy >= dao_size)) {
    return;
  }
  float2 longest = float2(0.0, 0.0);
  if (dao_cuts[uint2(dao_counter, 0)] < dao_cut_tiles) {
    float longest_squared = 0.0;
    for (int y = -1; y <= 1; ++y) {
      for (int x = -1; x <= 1; ++x) {
        int2 p = clamp(int2(id.xy) + int2(x, y), int2(0, 0), int2(dao_size) - 1);
        float2 blur = dao_tiles[p];
        float blur_squared = dot(blur, blur);
        if (blur_squared > longest_squared) {
          longest = blur;
          longest_squared = blur_squared;
        }
      }
    }
  }
  dao_dest[id.xy] = longest;
}
