// DAO: D3D12CommandProcessor's additions: the temporal upscaler's image and
// the HDR copy at the swap, submitting before waits, and reading back GPU-
// written memory.
#include "xenia/gpu/d3d12/d3d12_command_processor.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/gpu/d3d12/dao_gpu_timing.h"
#include "xenia/gpu/d3d12/dao_upscale.h"
#include "xenia/ui/d3d12/d3d12_util.h"

DECLARE_double(present_hdr_paper_white);
DECLARE_double(present_hdr_gamma);

namespace xe {
namespace gpu {
namespace d3d12 {

namespace shaders {
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/dao_hdr_copy_cs.h"
}  // namespace shaders

bool D3D12CommandProcessor::DaoInitializeHdrCopy(
    D3D12_ROOT_PARAMETER* apply_gamma_root_parameters,
    const D3D12_ROOT_SIGNATURE_DESC& apply_gamma_root_signature_desc) {
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  // The same layout with more constants (the gamma ramp's root signature,
  // created from the description before, is done with it).
  apply_gamma_root_parameters[UINT(ApplyGammaRootParameter::kConstants)]
      .Constants.Num32BitValues = sizeof(DaoHdrCopyConstants) / sizeof(uint32_t);
  *(dao_hdr_copy_root_signature_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateRootSignature(provider,
                                           apply_gamma_root_signature_desc);
  if (!dao_hdr_copy_root_signature_) {
    XELOGE("Failed to create the HDR copy root signature");
    return false;
  }
  *(dao_hdr_copy_pipeline_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateComputePipeline(
          device, shaders::dao_hdr_copy_cs, sizeof(shaders::dao_hdr_copy_cs),
          dao_hdr_copy_root_signature_.Get());
  if (!dao_hdr_copy_pipeline_) {
    XELOGE("Failed to create the HDR copy compute pipeline");
    return false;
  }
  return true;
}

void D3D12CommandProcessor::DaoUpdateGammaRampIdentity() {
  const reg::DC_LUT_30_COLOR* table = gamma_ramp_256_entry_table();
  dao_gamma_ramp_identity_ = true;
  for (uint32_t i = 0; i < 256; ++i) {
    const uint32_t identity = (i * 1023 + 127) / 255;
    for (uint32_t value :
         {table[i].color_10_red, table[i].color_10_green,
          table[i].color_10_blue}) {
      dao_gamma_ramp_identity_ &=
          value + 1 >= identity && value <= identity + 1;
    }
  }
}

void D3D12CommandProcessor::DaoReplaceSwapTexture(
    ID3D12Resource*& swap_texture_resource,
    D3D12_SHADER_RESOURCE_VIEW_DESC& swap_texture_srv_desc,
    D3D12_RESOURCE_DESC& swap_texture_desc, bool dao_timed,
    bool& dao_sharpened, bool& dao_hdr) {
  if (dao_timed) {
    dao_timing::GpuTiming::Get().Mark(*this, "swap start");
  }
  D3D12_SHADER_RESOURCE_VIEW_DESC dao_upscaled_srv_desc;
  bool dao_upscaled_by_fsr = false;
  if (ID3D12Resource* dao_upscaled = DaoUpscaler::OnSwap(
          swap_texture_resource, swap_texture_srv_desc,
          uint32_t(swap_texture_desc.Width),
          uint32_t(swap_texture_desc.Height), dao_upscaled_srv_desc,
          dao_hdr, dao_upscaled_by_fsr)) {
    swap_texture_resource = dao_upscaled;
    swap_texture_srv_desc = dao_upscaled_srv_desc;
    const D3D12_RESOURCE_DESC dao_upscaled_desc = dao_upscaled->GetDesc();
    dao_sharpened = dao_upscaled_by_fsr &&
                    dao_upscaled_desc.Width <= swap_texture_desc.Width &&
                    dao_upscaled_desc.Height <= swap_texture_desc.Height;
    swap_texture_desc = dao_upscaled_desc;
    if (dao_timed) {
      dao_timing::GpuTiming::Get().Mark(*this, "UI composite, motion blur");
    }
  }
}

void D3D12CommandProcessor::DaoSetHdrCopyRoot(
    const D3D12_RESOURCE_DESC& swap_texture_desc, bool use_pwl_gamma_ramp,
    bool dao_scrgb) {
  deferred_command_list_.D3DSetComputeRootSignature(
      dao_hdr_copy_root_signature_.Get());
  DaoHdrCopyConstants dao_hdr_copy_constants;
  dao_hdr_copy_constants.size[0] = uint32_t(swap_texture_desc.Width);
  dao_hdr_copy_constants.size[1] = uint32_t(swap_texture_desc.Height);
  dao_hdr_copy_constants.paper_white =
      float(std::max(cvars::present_hdr_paper_white, 80.0));
  dao_hdr_copy_constants.gamma =
      float(std::clamp(cvars::present_hdr_gamma, 1.0, 3.0));
  dao_hdr_copy_constants.use_ramp =
      !use_pwl_gamma_ramp && !dao_gamma_ramp_identity_;
  dao_hdr_copy_constants.scrgb = dao_scrgb ? 1 : 0;
  deferred_command_list_.D3DSetComputeRoot32BitConstants(
      UINT(ApplyGammaRootParameter::kConstants),
      sizeof(dao_hdr_copy_constants) / sizeof(uint32_t),
      &dao_hdr_copy_constants, 0);
}

void D3D12CommandProcessor::PrepareForWait() {
  CommandProcessor::PrepareForWait();
  // The commands before the wait run on the GPU while the command processor
  // waits (on the guest CPU, often for most of a frame): submitted now rather
  // than at the swap.
  if (submission_open_ && CanEndSubmissionImmediately()) {
    EndSubmission(false);
  }
}

bool D3D12CommandProcessor::DaoReadGpuWrittenMemory(
    uint32_t address, uint32_t length, uint32_t pixel_size_log2,
    std::vector<uint8_t>& out) {
  out.clear();
  if (!length || !shared_memory_->IsRangeWrittenByGpu(address, length) ||
      !BeginSubmission(false)) {
    return false;
  }
  const bool scaled = texture_cache_->IsDrawResolutionScaled() &&
                      texture_cache_->DaoIsRangeAllScaledResolved(address, length);
  uint32_t read_length = length;
  uint32_t tile_count = 0;
  ID3D12Resource* scaled_source = nullptr;
  uint64_t scaled_offset = 0;
  if (scaled) {
    if (!texture_cache_->MakeScaledResolveRangeCurrent(address, length)) {
      return false;
    }
    if (!GetScaledReadbackSource(address, length, pixel_size_log2, read_length,
                                 tile_count, scaled_source, scaled_offset)) {
      return false;
    }
  }

  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  D3D12_RESOURCE_DESC buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(
      buffer_desc, AlignReadbackBufferSize(read_length),
      D3D12_RESOURCE_FLAG_NONE);
  Microsoft::WRL::ComPtr<ID3D12Resource> readback;
  if (FAILED(provider.GetDevice()->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesReadback,
          provider.GetHeapFlagCreateNotZeroed(), &buffer_desc,
          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
          IID_PPV_ARGS(&readback)))) {
    XELOGE("Failed to create a {} KB readback buffer", read_length >> 10);
    return false;
  }
  if (scaled) {
    if (!RecordScaledReadback(scaled_source, scaled_offset, read_length,
                              pixel_size_log2, tile_count, readback.Get())) {
      return false;
    }
  } else {
    shared_memory_->UseAsCopySource();
    SubmitBarriers();
    deferred_command_list_.D3DCopyBufferRegion(
        readback.Get(), 0, shared_memory_->GetBuffer(), address, read_length);
  }
  if (!AwaitAllQueueOperationsCompletion()) {
    // The copy may still run: released once its submission is done.
    resources_for_deletion_.emplace_back(GetCurrentSubmission(),
                                         readback.Detach());
    return false;
  }
  D3D12_RANGE read_range = {0, read_length};
  void* mapping;
  if (FAILED(readback->Map(0, &read_range, &mapping))) {
    return false;
  }
  out.assign(static_cast<const uint8_t*>(mapping),
             static_cast<const uint8_t*>(mapping) + read_length);
  D3D12_RANGE written_range = {};
  readback->Unmap(0, &written_range);
  return true;
}

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe
