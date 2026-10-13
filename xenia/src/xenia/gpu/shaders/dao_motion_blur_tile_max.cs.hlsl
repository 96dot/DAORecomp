// DAO: motion blur, first pass, one thread group per tile of the motion
// vectors: each pixel's blur length and log depth, together for the blur
// (dao_upscale_composite_blur.cs reads them once each), and the tile's longest
// blur vector. A tile with a camera cut in it gets none, and is counted.
// A pixel's blur is no longer than the motion where it was a frame ago: only
// motion that lasted is blurred, not one frame's wrong motion vectors (a draw
// matched to the wrong one of the frame before) or motion every other frame
// (animation at half the frame rate), which would flicker.
#include "dao_motion_blur.hlsli"

Texture2D<float2> dao_motion : register(t0);
Texture2D<float> dao_depth : register(t1);
// The motion vectors of the frame before (as dao_motion then), and this
// frame's copy of them for the next.
Texture2D<float2> dao_previous_motion : register(t2);
RWTexture2D<float2> dao_tiles : register(u0);
// x: the blur vector's length, y: the log view depth.
RWTexture2D<float2> dao_pixels : register(u1);
// (u2: dao_cuts, in dao_motion_blur.hlsli.)
RWTexture2D<float2> dao_motion_copy : register(u3);

// The longest blur vector (xy, its squared length in z), and 1 in w with a
// cut, per thread.
groupshared float4 dao_longest[64];

[numthreads(8, 8, 1)]
void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID,
          uint index : SV_GroupIndex) {
  uint2 origin = group.xy * dao_tile;
  float tile_size = float(dao_tile);
  float4 longest = 0.0;
  for (uint y = thread.y; y < dao_tile; y += 8) {
    for (uint x = thread.x; x < dao_tile; x += 8) {
      uint2 p = origin + uint2(x, y);
      if (any(p >= dao_size)) {
        continue;
      }
      float2 motion = dao_motion[p];
      dao_motion_copy[p] = motion;
      float2 blur = 0.0;
      float motion_squared = dot(motion, motion);
      if (motion_squared > dao_cut * dao_cut) {
        longest.w = 1.0;
      } else {
        blur = motion * dao_scale;
        // Where it was a frame ago (motion is the previous position minus
        // this one); outside the image (Load gives 0) or with no previous
        // motion, none, so no blur.
        float2 previous = 0.0;
        if (dao_previous_valid) {
          previous = dao_previous_motion.Load(
              int3(int2(floor(float2(p) + 0.5 + motion)), 0));
        }
        float previous_squared = dot(previous, previous);
        if (previous_squared < motion_squared) {
          blur *= sqrt(previous_squared / motion_squared);
        }
        float length_squared = dot(blur, blur);
        if (length_squared > tile_size * tile_size) {
          blur *= tile_size * rsqrt(length_squared);
        }
      }
      float blur_squared = dot(blur, blur);
      if (blur_squared > longest.z) {
        longest.xyz = float3(blur, blur_squared);
      }
      // Log of the view depth, up to a constant (a far plane far away).
      float depth = dao_depth[p];
      float log_depth =
          -log(max(dao_depth_reversed ? depth : 1.0 - depth, 1.0e-7));
      dao_pixels[p] = float2(sqrt(blur_squared), log_depth);
    }
  }
  dao_longest[index] = longest;
  GroupMemoryBarrierWithGroupSync();
  for (uint stride = 32; stride; stride >>= 1) {
    if (index < stride) {
      float4 other = dao_longest[index + stride];
      float4 mine = dao_longest[index];
      if (other.z > mine.z) {
        mine.xyz = other.xyz;
      }
      mine.w = max(mine.w, other.w);
      dao_longest[index] = mine;
    }
    GroupMemoryBarrierWithGroupSync();
  }
  if (index == 0) {
    float4 tile = dao_longest[0];
    if (tile.w != 0.0) {
      InterlockedAdd(dao_cuts[uint2(dao_counter, 0)], 1);
    }
    dao_tiles[group.xy] = tile.w != 0.0 ? float2(0.0, 0.0) : tile.xy;
  }
}
