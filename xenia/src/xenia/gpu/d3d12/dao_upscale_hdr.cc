// DAO: true HDR of the game's 3D scenes (its tonemapping redone, the HDR
// composite) - see dao_upscale.h.
#include "xenia/gpu/d3d12/dao_upscale_impl.h"

#include <algorithm>
#include <cmath>

#include "xenia/dao_live_settings.h"
#include "xenia/gpu/d3d12/d3d12_command_processor.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/ui/d3d12/d3d12_presenter.h"

DECLARE_bool(dao_true_hdr);
DECLARE_bool(present_hdr);

namespace xe {
namespace gpu {
namespace d3d12 {

namespace {

// The tonemapping pass's color matrix (rows, offset in w), which its vertex
// shader (microcode hash AF062EA1E2A3052F) computes from constants alone
// (brightness, contrast, saturation, a hue rotation): that shader's microcode,
// translated by hand, in order; operations paired in one instruction read the
// values from before it.
void TonemapRows(const RegisterFile& regs, float rows[3][4]) {
  struct V {
    float x, y, z, w;
  };
  auto c = [&regs](uint32_t index) {
    const uint32_t r = XE_GPU_REG_SHADER_CONSTANT_000_X + index * 4;
    return V{regs.Get<float>(r), regs.Get<float>(r + 1),
             regs.Get<float>(r + 2), regs.Get<float>(r + 3)};
  };
  const V c0 = c(0), c1 = c(1), c2 = c(2), c3 = c(3), c252 = c(252),
          c253 = c(253), c254 = c(254), c255 = c(255);
  V r0 = {}, r1 = {}, r2 = {}, r3 = {}, r4 = {}, r5 = {}, r6 = {};
  r0.z = c1.x * c252.w;
  r1.x = c253.y - c2.x;
  r0.x = c3.x * c252.z + c254.w;
  r4.x = r1.x * c254.x;
  r4.y = r1.x * c254.y;
  r4.z = r1.x * c254.z;
  r6.y = c1.x * c0.x;
  r3.x = r4.x + c2.x;
  r3.y = r4.z + c2.x;
  r3.z = r4.y + c2.x;
  r0.y = r0.x - std::floor(r0.x);
  r2.y = r4.x * r6.y;
  r2.z = r4.y * r6.y;
  r2.w = r4.z * r6.y;
  r0.x = c255.x * r0.y;
  r1.y = c253.x == 0.0f ? r3.z : r4.z;
  r1.z = c253.y == 0.0f ? r3.z : r4.z;
  r1.w = c253.y == 0.0f ? r3.z : r4.x;
  r6.x = r0.z + c253.w;
  r6.z = r0.x + c253.z;
  r4.w = r3.x;
  r0.x = std::cos(r6.z);
  r0.y = r4.y * r6.x;
  r0.w = r4.x * r6.x;
  r0.z = c253.y - r0.x;
  r1.x = (r4.z + r4.y + r4.w) * r6.x;
  r0.w += r0.y;
  r5.x = r6.y * r3.x;
  r5.y = r6.x * r3.y;
  r5.z = r6.y * r3.y;
  r0.y = std::sin(r6.z);
  r1.y = (r1.z + r1.w + r1.y) * r6.x;
  r1.z = r5.y + r0.w;
  r0.y *= c252.x;
  r0.z *= c252.y;
  r4.x = r0.z + r0.y;
  r4.y = r0.z + r0.x;
  r4.z = r0.z - r0.y;
  rows[0][3] = r1.z * r4.z + r1.x * r4.y + r1.y * r4.x;
  rows[1][3] = r1.z * r4.x + r1.x * r4.z + r1.y * r4.y;
  rows[2][3] = r1.z * r4.y + r1.x * r4.x + r1.y * r4.z;
  r1 = {r2.z * r4.y, r2.y * r4.z, r2.y * r4.y, r2.y * r4.x};
  r0 = {r2.z * r4.x, r2.z * r4.z, r2.w * r4.x, r2.w * r4.y};
  r2.x = r3.z * r6.y;
  r2 = {r2.x * r4.z, r2.x * r4.y, r2.w * r4.z, r2.x * r4.x};
  r3.y = r0.w + r0.z;
  r3.x = r2.z + r0.w;
  r6.x = r5.x * r4.x + r1.y;
  r6.y = r5.x * r4.y + r1.w;
  r6.z = r5.x * r4.z + r1.z;
  r2 = {r0.x + r2.x, r0.z + r2.z, r0.y + r2.y, r2.w + r1.x};
  rows[2][2] = r5.z * r4.y + r2.y;
  rows[0][0] = r6.y + r1.y;
  rows[1][0] = r6.z + r1.w;
  r1 = {r1.x + r2.x, r6.x + r1.z, r6.y + r1.y, r6.z + r1.w};
  r0.z = r5.z * r4.x + r3.x;
  r0.w = r5.z * r4.z + r3.y;
  r0.x += r2.z;
  r0.y += r2.w;
  rows[2][0] = r1.y;
  rows[2][1] = r1.x;
  rows[0][1] = r0.y;
  rows[0][2] = r0.w;
  rows[1][1] = r0.x;
  rows[1][2] = r0.z;
}

}  // namespace

void DaoUpscalerImpl::BeginHdrTonemap(const RegisterFile& regs) {
  // Its constants: the matrix from the vertex shader's, the rest the pixel
  // shader's (c0.x, c255.xy).
  hdr_pending_ = true;
  HdrTonemapConstants& constants = hdr_constants_;
  TonemapRows(regs, constants.rows);
  const uint32_t ps = XE_GPU_REG_SHADER_CONSTANT_000_X + 256 * 4;
  constants.bloom_scale = regs.Get<float>(ps);
  constants.offset = regs.Get<float>(ps + 255 * 4);
  constants.curve_v = regs.Get<float>(ps + 255 * 4 + 1);
}

bool DaoUpscalerImpl::EnsureHdrScene() {
  if (hdr_captured_ && hudless_pixel_shader_ == kTonemapPixelShader) {
    return true;
  }
  D3D12_SHADER_RESOURCE_VIEW_DESC color_srv = {};
  color_srv.Format = color_.format;
  color_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  color_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  color_srv.Texture2D.MipLevels = 1;
  Transition(color_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  return EnsureTexture(hdr_, DXGI_FORMAT_R16G16B16A16_FLOAT, render_size_[0],
                       render_size_[1], "HDR") &&
         ConvertImage(color_.resource.Get(), color_srv, hdr_,
                      kCompositeLinearize);
}

bool DaoUpscalerImpl::WantsTrueHdr() const {
  if (!cvars::present_hdr || !cvars::dao_true_hdr) {
    return false;
  }
  // On SDR displays the presenter only linearizes; HDR here would be clipped.
  const ui::Presenter* presenter = cp_.graphics_system_->presenter();
  return presenter && presenter->IsDisplayHdr();
}

bool DaoUpscalerImpl::HdrScrgb() const {
  const ui::Presenter* presenter = cp_.graphics_system_->presenter();
  return presenter && presenter->IsHdrScrgb();
}

void DaoUpscalerImpl::RunHdrTonemap() {
  const uint32_t width = render_size_[0], height = render_size_[1];
  if (!width ||
      !EnsureTexture(hdr_, DXGI_FORMAT_R16G16B16A16_FLOAT, width, height,
                     "HDR")) {
    return;
  }
  // Bindless, only the destination needs one.
  const bool bindless = cp_.bindless_resources_used_;
  ui::d3d12::util::DescriptorCpuGpuHandlePair descriptors[4];
  const uint32_t descriptor_count = bindless ? 1 : 4;
  if (!cp_.RequestOneUseSingleViewDescriptors(descriptor_count, descriptors)) {
    return;
  }
  const ui::d3d12::util::DescriptorCpuGpuHandlePair& dest_descriptor =
      descriptors[descriptor_count - 1];
  // The pass's textures, bound for it by the texture cache: 0 the bloom, 1 the
  // scene, 2 the curve.
  const ui::d3d12::D3D12Provider& provider = cp_.GetD3D12Provider();
  D3D12TextureCache& texture_cache = *cp_.texture_cache_;
  D3D12_GPU_DESCRIPTOR_HANDLE textures[3];
  for (uint32_t i = 0; i < 3; ++i) {
    D3D12Shader::TextureBinding binding = {};
    binding.fetch_constant = i;
    binding.dimension = xenos::FetchOpDimension::k2D;
    if (bindless) {
      textures[i] = provider.OffsetViewDescriptor(
          cp_.GetViewBindlessHeapGPUStart(),
          texture_cache.GetActiveTextureBindlessSRVIndex(binding));
    } else {
      texture_cache.WriteActiveTextureBindfulSRV(binding,
                                                 descriptors[i].first);
      textures[i] = descriptors[i].second;
    }
  }
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
  uav.Format = hdr_.format;
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  provider.GetDevice()->CreateUnorderedAccessView(
      hdr_.resource.Get(), nullptr, &uav, dest_descriptor.first);
  Transition(hdr_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  cp_.SubmitBarriers();

  HdrTonemapConstants& constants = hdr_constants_;
  constants.size[0] = width;
  constants.size[1] = height;
  constants.gamma = OutputGamma();
  constants.peak_ratio = dao_live::HdrPeakRatio(PaperWhiteNits());
  // In the curves' linear part: their shoulders start at about 0.45.
  constants.knee = 0.4f;
  DeferredCommandList& command_list = cp_.GetDeferredCommandList();
  command_list.D3DSetComputeRootSignature(hdr_tonemap_root_signature_.Get());
  command_list.D3DSetComputeRoot32BitConstants(
      0, sizeof(constants) / sizeof(uint32_t), &constants, 0);
  for (uint32_t i = 0; i < 3; ++i) {
    command_list.D3DSetComputeRootDescriptorTable(1 + i, textures[i]);
  }
  command_list.D3DSetComputeRootDescriptorTable(4, dest_descriptor.second);
  cp_.SetExternalPipeline(hdr_tonemap_pipeline_.Get());
  command_list.D3DDispatch((width + 7) / 8, (height + 7) / 8, 1);
  // If the pass is drawn again this frame, the next write must wait.
  cp_.PushUAVBarrier(hdr_.resource.Get());
  hdr_captured_ = true;
}

ID3D12Resource* DaoUpscalerImpl::CompositeHdr(
    ID3D12Resource* frontbuffer,
    const D3D12_SHADER_RESOURCE_VIEW_DESC& frontbuffer_srv,
    D3D12_SHADER_RESOURCE_VIEW_DESC& srv_out, bool& hdr_out) {
  const uint32_t width = render_size_[0], height = render_size_[1];
  if (!EnsureHdrScene()) {
    return NotUpscaled("no HDR scene", frontbuffer, frontbuffer_srv, srv_out,
                       hdr_out);
  }
  ui::d3d12::util::DescriptorCpuGpuHandlePair views[4];
  if (!EnsureTexture(encoded_, HdrFormat(), width, height, "encoded") ||
      !cp_.RequestOneUseSingleViewDescriptors(4, views)) {
    return NotUpscaled("no descriptors", frontbuffer, frontbuffer_srv, srv_out,
                       hdr_out);
  }
  // The scene (t0) and the image before the UI (t2) at the front buffer's
  // size.
  ID3D12Device* device = cp_.GetD3D12Provider().GetDevice();
  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  srv.Format = hdr_.format;
  device->CreateShaderResourceView(hdr_.resource.Get(), &srv, views[0].first);
  device->CreateShaderResourceView(frontbuffer, &frontbuffer_srv,
                                   views[1].first);
  srv.Format = color_.format;
  device->CreateShaderResourceView(color_.resource.Get(), &srv,
                                   views[2].first);
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
  uav.Format = encoded_.format;
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  device->CreateUnorderedAccessView(encoded_.resource.Get(), nullptr, &uav,
                                    views[3].first);
  // No motion (t3) or blur (t4, t5): unread, given the image's view.
  const ui::d3d12::util::DescriptorCpuGpuHandlePair tables[kCompositeTables] = {
      views[0], views[1], views[2], views[2], views[2], views[2], views[3]};
  Transition(hdr_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(color_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(encoded_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  CompositeConstants constants = {};
  constants.dest_size[0] = width;
  constants.dest_size[1] = height;
  constants.render_size[0] = float(width);
  constants.render_size[1] = float(height);
  constants.mode = kCompositeHdr;
  DispatchComposite(tables, constants);
  Transition(encoded_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  EndFrame(false, "off or paused (true HDR at the render size)");
  hdr_out = true;
  srv_out = {};
  srv_out.Format = encoded_.format;
  srv_out.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv_out.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv_out.Texture2D.MipLevels = 1;
  return encoded_.resource.Get();
}

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe
