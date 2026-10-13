// DAO: motion blur's tile passes (dao_motion_blur_tile_max.cs, then
// dao_motion_blur_neighbor_max.cs), before the blur itself in
// dao_upscale_composite_blur.cs. Blur vectors are half the motion over the
// shutter time, in render pixels, at most a tile long.

cbuffer DaoBlurConstants : register(b0) {
  // The tile pass: the motion vectors' size; the neighbourhood pass: the
  // tile counts.
  uint2 dao_size;
  uint dao_tile;    // Render pixels per tile.
  float dao_scale;  // From motion to blur vectors.
  // Motion longer than this in a frame (render pixels) is a camera cut, not
  // motion: its pixels and tiles get no blur.
  float dao_cut;
  // The scene's depth is greater nearer the camera.
  uint dao_depth_reversed;
  // This frame's counter of tiles with a cut in dao_cuts (0 or 1), and how
  // many such tiles make the whole frame a cut.
  uint dao_counter;
  uint dao_cut_tiles;
};

// Tiles with a cut, counted by the tile pass in [dao_counter], read by the
// neighbourhood pass, which clears the other counter for the next frame.
RWTexture2D<uint> dao_cuts : register(u2);
