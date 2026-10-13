// DAO: motion blur of 3D scenes, from temporal upscaling's motion vectors -
// see dao_upscale.h.
#include "xenia/gpu/d3d12/dao_upscale_impl.h"

#include <algorithm>

#include "xenia/gpu/d3d12/d3d12_command_processor.h"

DECLARE_int32(dao_motion_blur);
DECLARE_bool(dao_motion_vectors);
DECLARE_bool(dao_show_motion_vectors);

namespace xe {
namespace gpu {
namespace d3d12 {

// Motion blur's Low and High: the shutter open for this much of the frame
// time, and samples per pixel.
constexpr float kBlurShutter[2] = {0.25f, 0.5f};
constexpr uint32_t kBlurSamples[2] = {8, 12};
// Tiles (the longest blur): 1/24 of the render height, rounded (30 pixels at
// 720p), at least a tile pass thread group's 8.
constexpr uint32_t kBlurTilesPerHeight = 24;
constexpr uint32_t kBlurMinTile = 8;
// Motion of more than this much of the render height in a frame is a camera
// cut, not motion; with a cut in more than 1/kBlurCutTileFraction of the
// tiles, the whole frame is one.
constexpr float kBlurCut = 0.25f;
constexpr uint32_t kBlurCutTileFraction = 8;

DaoUpscalerImpl::MotionBlur DaoUpscalerImpl::GetMotionBlur(
    uint32_t render_width, uint32_t render_height) const {
  MotionBlur blur;
  // Not on the first upscaled frame (the motion is from frames not upscaled),
  // nor with motion vectors off or shown instead of the scene.
  const int32_t strength = cvars::dao_motion_blur;
  if (strength <= 0 || !motion_available_ || !cvars::dao_motion_vectors ||
      cvars::dao_show_motion_vectors || reset_history_ ||
      (render_width == blur_failed_size_[0] &&
       render_height == blur_failed_size_[1])) {
    return blur;
  }
  // Blur vectors are half the motion over the shutter time. (Levels as
  // DAORecomp's kMaxMotionBlur: 1 low, 2 high; more is high.)
  const size_t level = strength >= 2 ? 1 : 0;
  blur.samples = kBlurSamples[level];
  blur.scale = kBlurShutter[level] * 0.5f;
  blur.tile = std::max(
      (render_height + kBlurTilesPerHeight / 2) / kBlurTilesPerHeight,
      kBlurMinTile);
  blur.cut = float(render_height) * kBlurCut;
  blur.tile_count[0] = (render_width + blur.tile - 1) / blur.tile;
  blur.tile_count[1] = (render_height + blur.tile - 1) / blur.tile;
  blur.cut_tiles = std::max(
      blur.tile_count[0] * blur.tile_count[1] / kBlurCutTileFraction,
      uint32_t(1));
  return blur;
}

void DaoUpscalerImpl::RecordMotionBlurTiles(
    const MotionBlur& blur, uint32_t render_width, uint32_t render_height,
    const ui::d3d12::util::DescriptorCpuGpuHandlePair* descriptors) {
  // descriptors: motion, depth, tiles (unordered), pixels (unordered), cuts
  // (unordered), tiles, neighbourhoods (unordered), the previous motion,
  // this motion's copy (unordered).
  ID3D12Device* device = cp_.GetD3D12Provider().GetDevice();
  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  srv.Format = motion_vectors_.format;
  device->CreateShaderResourceView(motion_vectors_.resource.Get(), &srv,
                                   descriptors[0].first);
  srv.Format = depth_.format;
  device->CreateShaderResourceView(depth_.resource.Get(), &srv,
                                   descriptors[1].first);
  uav.Format = blur_tiles_.format;
  device->CreateUnorderedAccessView(blur_tiles_.resource.Get(), nullptr, &uav,
                                    descriptors[2].first);
  uav.Format = blur_pixels_.format;
  device->CreateUnorderedAccessView(blur_pixels_.resource.Get(), nullptr, &uav,
                                    descriptors[3].first);
  uav.Format = blur_cuts_.format;
  device->CreateUnorderedAccessView(blur_cuts_.resource.Get(), nullptr, &uav,
                                    descriptors[4].first);
  srv.Format = blur_tiles_.format;
  device->CreateShaderResourceView(blur_tiles_.resource.Get(), &srv,
                                   descriptors[5].first);
  uav.Format = blur_around_.format;
  device->CreateUnorderedAccessView(blur_around_.resource.Get(), nullptr,
                                    &uav, descriptors[6].first);
  Texture& previous_motion = blur_motion_[blur_counter_ ^ 1];
  Texture& motion_copy = blur_motion_[blur_counter_];
  srv.Format = previous_motion.format;
  device->CreateShaderResourceView(previous_motion.resource.Get(), &srv,
                                   descriptors[7].first);
  uav.Format = motion_copy.format;
  device->CreateUnorderedAccessView(motion_copy.resource.Get(), nullptr, &uav,
                                    descriptors[8].first);
  BlurConstants constants = {};
  constants.size[0] = render_width;
  constants.size[1] = render_height;
  constants.tile = blur.tile;
  constants.scale = blur.scale;
  constants.cut = blur.cut;
  // Whichever depth function most of the scene drew with.
  constants.depth_reversed = depth_greater_draws_ > depth_less_draws_ ? 1 : 0;
  constants.counter = blur_counter_;
  constants.cut_tiles = blur.cut_tiles;
  constants.previous_valid = blur_motion_valid_ ? 1 : 0;
  blur_counter_ ^= 1;
  blur_motion_valid_ = true;
  // The tile pass, after the last neighbourhood pass cleared its counter.
  Transition(blur_pixels_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  Transition(blur_tiles_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  Transition(blur_cuts_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  Transition(previous_motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(motion_copy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  cp_.PushUAVBarrier(blur_cuts_.resource.Get());
  const ui::d3d12::util::DescriptorCpuGpuHandlePair* tile_tables[kBlurTables] = {
      &descriptors[0], &descriptors[1], &descriptors[7], &descriptors[2],
      &descriptors[3], &descriptors[4], &descriptors[8]};
  DispatchBlurTiles(blur_tile_max_pipeline_.Get(), constants, tile_tables,
                    blur.tile_count[0], blur.tile_count[1]);
  // The neighbourhood pass, after the tile pass's counting: it uses only t0,
  // u0 and u2 (the other tables are placeholders).
  Transition(blur_pixels_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(blur_tiles_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(blur_around_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  cp_.PushUAVBarrier(blur_cuts_.resource.Get());
  constants.size[0] = blur.tile_count[0];
  constants.size[1] = blur.tile_count[1];
  const ui::d3d12::util::DescriptorCpuGpuHandlePair* around_tables[kBlurTables] = {
      &descriptors[5], &descriptors[5], &descriptors[5], &descriptors[6],
      &descriptors[6], &descriptors[4], &descriptors[6]};
  DispatchBlurTiles(blur_neighbor_max_pipeline_.Get(), constants,
                    around_tables, (blur.tile_count[0] + 7) / 8,
                    (blur.tile_count[1] + 7) / 8);
  Transition(blur_around_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

void DaoUpscalerImpl::DispatchBlurTiles(
    ID3D12PipelineState* pipeline, const BlurConstants& constants,
    const ui::d3d12::util::DescriptorCpuGpuHandlePair* const* tables,
    uint32_t groups_x, uint32_t groups_y) {
  cp_.SubmitBarriers();
  DeferredCommandList& command_list = cp_.GetDeferredCommandList();
  command_list.D3DSetComputeRootSignature(blur_root_signature_.Get());
  command_list.D3DSetComputeRoot32BitConstants(
      0, sizeof(constants) / sizeof(uint32_t), &constants, 0);
  for (uint32_t i = 0; i < kBlurTables; ++i) {
    command_list.D3DSetComputeRootDescriptorTable(1 + i, tables[i]->second);
  }
  cp_.SetExternalPipeline(pipeline);
  command_list.D3DDispatch(groups_x, groups_y, 1);
}

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe
