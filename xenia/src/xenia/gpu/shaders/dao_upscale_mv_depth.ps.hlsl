// DAO: the scene depth into the motion vector pass's depth buffer (drawn with
// fullscreen_cw.vs).
Texture2D<float> dao_mv_scene_depth : register(t1);

float main(float4 position : SV_Position) : SV_Depth {
  return dao_mv_scene_depth.Load(int3(int2(position.xy), 0));
}
