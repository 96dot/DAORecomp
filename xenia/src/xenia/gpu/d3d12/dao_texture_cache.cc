// DAO: resolves writing their areas straight into the textures of the
// resolved memory: D3D12TextureCache's part (the render target cache's is
// D3D12RenderTargetCache::GetResolvedArea).
#include "xenia/gpu/d3d12/d3d12_texture_cache.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/gpu/d3d12/d3d12_command_processor.h"
#include "xenia/ui/d3d12/d3d12_util.h"

DECLARE_bool(dao_resolve_to_textures);

namespace xe {
namespace gpu {
namespace d3d12 {

namespace shaders {
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/dao_resolve_texture_cs.h"
}  // namespace shaders

void D3D12TextureCache::DaoInitializeResolveTextures() {
  const ui::d3d12::D3D12Provider& provider =
      command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  // Writing resolved areas into textures: constants, the source SRV, the
  // destination UAV.
  if (cvars::dao_resolve_to_textures) {
    D3D12_DESCRIPTOR_RANGE resolve_texture_ranges[2] = {};
    resolve_texture_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    resolve_texture_ranges[0].NumDescriptors = 1;
    resolve_texture_ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    resolve_texture_ranges[1].NumDescriptors = 1;
    D3D12_ROOT_PARAMETER resolve_texture_parameters[3] = {};
    resolve_texture_parameters[0].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    resolve_texture_parameters[0].Constants.Num32BitValues =
        sizeof(ResolveTextureConstants) / sizeof(uint32_t);
    for (uint32_t i = 0; i < 2; ++i) {
      resolve_texture_parameters[1 + i].ParameterType =
          D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      resolve_texture_parameters[1 + i].DescriptorTable.NumDescriptorRanges = 1;
      resolve_texture_parameters[1 + i].DescriptorTable.pDescriptorRanges =
          &resolve_texture_ranges[i];
    }
    D3D12_ROOT_SIGNATURE_DESC resolve_texture_desc = {};
    resolve_texture_desc.NumParameters =
        UINT(xe::countof(resolve_texture_parameters));
    resolve_texture_desc.pParameters = resolve_texture_parameters;
    *(resolve_texture_root_signature_.ReleaseAndGetAddressOf()) =
        ui::d3d12::util::CreateRootSignature(provider, resolve_texture_desc);
    if (resolve_texture_root_signature_) {
      *(resolve_texture_pipeline_.ReleaseAndGetAddressOf()) =
          ui::d3d12::util::CreateComputePipeline(
              device, shaders::dao_resolve_texture_cs,
              sizeof(shaders::dao_resolve_texture_cs),
              resolve_texture_root_signature_.Get());
    }
    if (!resolve_texture_pipeline_) {
      XELOGE(
          "D3D12TextureCache: Failed to create the pipeline for writing "
          "resolved areas into textures");
    }
  }
}

bool D3D12TextureCache::IsResolveWritable(const TextureKey& key) const {
  return resolve_texture_pipeline_ &&
         key.dimension == xenos::DataDimension::k2DOrStacked && key.tiled &&
         !key.depth_or_array_size_minus_1 && !key.mip_max_level &&
         key.format == xenos::TextureFormat::k_8_8_8_8 &&
         !key.signed_separate &&
         (!key.packed_mips || std::min(key.GetWidth(), key.GetHeight()) > 16);
}

bool D3D12TextureCache::PrepareResolvedTextures(const ResolvedArea& area) {
  std::vector<Texture*>& textures = resolve_invalidated_textures_;
  const uint32_t pitch = area.dest_pitch_div_32 << 5;
  if (!resolve_texture_pipeline_ || area.dest_base & 0xFFF ||
      area.dest_x + area.width > pitch) {
    textures.clear();
    return false;
  }
  // The same texels the untiling would load, into a texture created as a UAV.
  // Textures not used lately (other views of the memory) are left outdated
  // rather than kept alive and written by every resolve: lately is in time
  // rather than submissions, of which a frame may have several.
  constexpr uint64_t kUsedLatelyMilliseconds = 250;
  const uint64_t now = xe::Clock::QueryHostUptimeMillis();
  auto described = [&](const Texture* texture) {
    const TextureKey& key = texture->key();
    if (!IsResolveWritable(key) ||
        key.base_page != ((area.dest_base >> 12) & 0x1FFFF) ||
        key.pitch != area.dest_pitch_div_32 ||
        bool(key.scaled_resolve) != area.scaled ||
        area.dest_x >= key.GetWidth() || area.dest_y >= key.GetHeight() ||
        texture->last_usage_time() + kUsedLatelyMilliseconds < now) {
      return false;
    }
    if (!(static_cast<const D3D12Texture*>(texture)->resource()->GetDesc().Flags &
          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
      // Created before a resolve first wrote its memory: untiled after every
      // resolve while it stays cached.
      if (!resolve_missed_uav_logged_) {
        resolve_missed_uav_logged_ = true;
        XELOGW(
            "D3D12TextureCache: a texture at {:08X} written by resolves isn't "
            "a UAV, reloaded after each of them instead",
            key.base_page << 12);
      }
      return false;
    }
    return true;
  };
  textures.erase(std::remove_if(textures.begin(), textures.end(),
                                [&](const Texture* texture) {
                                  return !described(texture);
                                }),
                 textures.end());
  return !textures.empty();
}

namespace {
// Byte positions in a 32-bit word after an endian swap.
void SwapBytes32(uint32_t bytes[4], xenos::Endian endian) {
  switch (endian) {
    case xenos::Endian::k8in16:
      std::swap(bytes[0], bytes[1]);
      std::swap(bytes[2], bytes[3]);
      break;
    case xenos::Endian::k8in32:
      std::swap(bytes[0], bytes[3]);
      std::swap(bytes[1], bytes[2]);
      break;
    case xenos::Endian::k16in32:
      std::swap(bytes[0], bytes[2]);
      std::swap(bytes[1], bytes[3]);
      break;
    default:
      break;
  }
}
}  // namespace

void D3D12TextureCache::WriteResolvedTextures(const ResolvedArea& area) {
  const uint32_t scale_x = area.scaled ? draw_resolution_scale_x() : 1;
  const uint32_t scale_y = area.scaled ? draw_resolution_scale_y() : 1;
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  DeferredCommandList& command_list =
      command_processor_.GetDeferredCommandList();
  // The resolve writes the render target's RGBA bytes (red / blue swapped if
  // requested) with its endian swap, the untiling reads them with the
  // texture's.
  uint32_t written[4] = {0, 1, 2, 3};
  if (area.swap_red_blue) {
    std::swap(written[0], written[2]);
  }
  SwapBytes32(written, area.endian);
  ResolveTextureConstants constants;
  constants.source_offset[0] = area.source_x * scale_x;
  constants.source_offset[1] = area.source_y * scale_y;
  constants.dest_offset[0] = area.dest_x * scale_x;
  constants.dest_offset[1] = area.dest_y * scale_y;
  constants.fill[0] = area.fill_x;
  constants.fill[1] = area.fill_y;
  D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
  srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv_desc.Texture2D.MipLevels = 1;
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
  uav_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  for (Texture* texture : resolve_invalidated_textures_) {
    auto& d3d12_texture = *static_cast<D3D12Texture*>(texture);
    const TextureKey& key = texture->key();
    uint32_t loaded[4] = {written[0], written[1], written[2], written[3]};
    SwapBytes32(loaded, key.endianness);
    constants.channels =
        loaded[0] | loaded[1] << 2 | loaded[2] << 4 | loaded[3] << 6;
    constants.size[0] = std::min(area.width * scale_x,
                                 (key.GetWidth() - area.dest_x) * scale_x);
    constants.size[1] = std::min(area.height * scale_y,
                                 (key.GetHeight() - area.dest_y) * scale_y);
    ui::d3d12::util::DescriptorCpuGpuHandlePair descriptors[2];
    if (!command_processor_.RequestOneUseSingleViewDescriptors(2,
                                                               descriptors)) {
      // Loaded from the resolved memory instead.
      break;
    }
    ID3D12Resource* resource = d3d12_texture.resource();
    device->CreateShaderResourceView(area.source, &srv_desc,
                                     descriptors[0].first);
    device->CreateUnorderedAccessView(resource, nullptr, &uav_desc,
                                      descriptors[1].first);
    command_processor_.PushTransitionBarrier(
        resource,
        d3d12_texture.SetResourceState(D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    command_processor_.SubmitBarriers();
    command_list.D3DSetComputeRootSignature(
        resolve_texture_root_signature_.Get());
    command_list.D3DSetComputeRoot32BitConstants(
        0, sizeof(constants) / sizeof(uint32_t), &constants, 0);
    command_list.D3DSetComputeRootDescriptorTable(1, descriptors[0].second);
    command_list.D3DSetComputeRootDescriptorTable(2, descriptors[1].second);
    command_processor_.SetExternalPipeline(resolve_texture_pipeline_.Get());
    command_list.D3DDispatch((constants.size[0] + 7) / 8,
                             (constants.size[1] + 7) / 8, 1);
    // A following write to the same texture waits for this one.
    command_processor_.PushUAVBarrier(resource);
    texture->MarkAsUsed();
    MakeUpToDateAndWatch(*texture);
  }
  resolve_invalidated_textures_.clear();
}

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe
