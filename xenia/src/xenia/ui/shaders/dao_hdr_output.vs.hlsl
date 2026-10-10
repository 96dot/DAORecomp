// DAO: full-screen triangle for the HDR output encoding pass.
float4 main(uint vertex_id : SV_VertexID) : SV_Position {
  float2 uv = float2((vertex_id << 1u) & 2u, vertex_id & 2u);
  return float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
}
