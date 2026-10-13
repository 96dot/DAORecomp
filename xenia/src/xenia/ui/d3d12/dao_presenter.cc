// DAO: D3D12Presenter's additions: presenting with vsync (with a frame
// latency of two), HDR output through an scRGB swap chain, and captures of
// an scRGB guest output.
#include "xenia/ui/d3d12/d3d12_presenter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/ui/d3d12/d3d12_util.h"

DEFINE_bool(dao_present_vsync, true,
            "Wait for the display's refresh when presenting (no tearing).",
            "Display");
DEFINE_bool(present_hdr, false,
            "Present through an scRGB (HDR) swap chain, expanding the SDR image "
            "on HDR displays.",
            "Display");
DEFINE_double(present_hdr_paper_white, 200.0,
              "HDR: brightness of SDR white in nits.", "Display");
DEFINE_double(present_hdr_peak, 1000.0,
              "HDR: peak brightness highlights are expanded to, in nits.",
              "Display");
DEFINE_double(present_hdr_gamma, 2.2,
              "HDR: gamma the guest output is decoded with.", "Display");
DEFINE_bool(dao_hdr_scrgb, false,
            "HDR quality High: the game's HDR frames stay scRGB (16-bit float) "
            "from the upscaler to the display, not PQ in 10 bits (smoother "
            "gradients, more memory traffic). Read at startup.",
            "Display");

namespace xe {
namespace ui {
namespace d3d12 {

namespace {
// DAO: 8-bit values for the non-negative half floats (bits 0 to 0x7FFF) of an
// scRGB guest output (HDR quality High), as its 10-bit counterpart would give:
// SDR clamped to 1, the game's linear HDR (1 = 80 nits) PQ-encoded as in
// dao_upscale_composite.cs. Captures (screenshots, tests) only.
const std::array<uint8_t, 0x8000>& HalfTo8bpc(bool game_hdr) {
  static const auto make = [](bool pq) {
    std::array<uint8_t, 0x8000> table;
    for (uint32_t half = 0; half < table.size(); ++half) {
      // IEEE half: exponent 31 is infinity or NaN, 0 subnormal.
      const uint32_t exponent = half >> 10, mantissa = half & 0x3FF;
      const float value =
          exponent == 31 ? 65504.0f
          : exponent
              ? std::ldexp(float(mantissa | 0x400), int(exponent) - 25)
              : std::ldexp(float(mantissa), -24);
      float encoded = std::min(value, 1.0f);
      if (pq) {
        const float y = std::pow(std::min(value * 80.0f / 10000.0f, 1.0f),
                                 0.1593017578125f);
        encoded = std::pow(
            (0.8359375f + 18.8515625f * y) / (1.0f + 18.6875f * y), 78.84375f);
      }
      table[half] = uint8_t(encoded * 255.0f + 0.5f);
    }
    return table;
  };
  static const std::array<uint8_t, 0x8000> sdr = make(false);
  static const std::array<uint8_t, 0x8000> hdr = make(true);
  return game_hdr ? hdr : sdr;
}
}  // namespace

namespace shaders {
#include "xenia/ui/shaders/bytecode/d3d12_5_1/dao_hdr_output_ps.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/dao_hdr_output_vs.h"
}  // namespace shaders

bool D3D12Presenter::DaoInitializeHdrOutput(
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC&
        guest_output_paint_pipeline_desc) {
  ID3D12Device* device = provider_.GetDevice();
  D3D12_DESCRIPTOR_RANGE hdr_source_range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                                             1, 0, 0, 0};
  D3D12_ROOT_PARAMETER hdr_parameters[2] = {};
  hdr_parameters[0].ParameterType =
      D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  hdr_parameters[0].DescriptorTable.NumDescriptorRanges = 1;
  hdr_parameters[0].DescriptorTable.pDescriptorRanges = &hdr_source_range;
  hdr_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  hdr_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  hdr_parameters[1].Constants.Num32BitValues = 4;
  hdr_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC hdr_root_signature_desc = {};
  hdr_root_signature_desc.NumParameters = 2;
  hdr_root_signature_desc.pParameters = hdr_parameters;
  *(hdr_output_root_signature_.ReleaseAndGetAddressOf()) =
      util::CreateRootSignature(provider_, hdr_root_signature_desc);
  if (!hdr_output_root_signature_) {
    XELOGE("D3D12Presenter: Failed to create the HDR output root signature");
    return false;
  }
  D3D12_GRAPHICS_PIPELINE_STATE_DESC hdr_pipeline_desc =
      guest_output_paint_pipeline_desc;
  hdr_pipeline_desc.pRootSignature = hdr_output_root_signature_.Get();
  hdr_pipeline_desc.VS.pShaderBytecode = shaders::dao_hdr_output_vs;
  hdr_pipeline_desc.VS.BytecodeLength = sizeof(shaders::dao_hdr_output_vs);
  hdr_pipeline_desc.PS.pShaderBytecode = shaders::dao_hdr_output_ps;
  hdr_pipeline_desc.PS.BytecodeLength = sizeof(shaders::dao_hdr_output_ps);
  hdr_pipeline_desc.RTVFormats[0] = kSwapChainHdrFormat;
  if (FAILED(device->CreateGraphicsPipelineState(
          &hdr_pipeline_desc, IID_PPV_ARGS(&hdr_output_pipeline_)))) {
    XELOGE("D3D12Presenter: Failed to create the HDR output pipeline");
    return false;
  }
  return true;
}

bool D3D12Presenter::DaoConnectHdrOutput() {
  ID3D12Device* device = provider_.GetDevice();
  paint_context_.swap_chain_hdr = cvars::present_hdr;
  paint_context_.hdr_source.Reset();
  if (paint_context_.swap_chain_hdr) {
    paint_context_.swap_chain->SetColorSpace1(
        DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
    paint_context_.display_hdr = false;
    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    Microsoft::WRL::ComPtr<IDXGIOutput6> output6;
    DXGI_OUTPUT_DESC1 output_desc;
    if (SUCCEEDED(paint_context_.swap_chain->GetContainingOutput(&output)) &&
        SUCCEEDED(output.As(&output6)) &&
        SUCCEEDED(output6->GetDesc1(&output_desc))) {
      paint_context_.display_hdr =
          output_desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    }
    XELOGI("D3D12Presenter: scRGB output on an {} display{}",
           paint_context_.display_hdr ? "HDR" : "SDR",
           hdr_scrgb_ ? ", HDR quality High (scRGB throughout)" : "");
    display_hdr_.store(paint_context_.display_hdr, std::memory_order_relaxed);
    D3D12_RESOURCE_DESC hdr_source_desc = {};
    hdr_source_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    hdr_source_desc.Width = paint_context_.swap_chain_width;
    hdr_source_desc.Height = paint_context_.swap_chain_height;
    hdr_source_desc.DepthOrArraySize = 1;
    hdr_source_desc.MipLevels = 1;
    hdr_source_desc.Format = HdrSourceFormat();
    hdr_source_desc.SampleDesc.Count = 1;
    hdr_source_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (FAILED(device->CreateCommittedResource(
            &util::kHeapPropertiesDefault, provider_.GetHeapFlagCreateNotZeroed(),
            &hdr_source_desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&paint_context_.hdr_source)))) {
      XELOGE("D3D12Presenter: Failed to create the HDR source texture");
      return false;
    }
    D3D12_RENDER_TARGET_VIEW_DESC hdr_source_rtv_desc = {};
    hdr_source_rtv_desc.Format = HdrSourceFormat();
    hdr_source_rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(
        paint_context_.hdr_source.Get(), &hdr_source_rtv_desc,
        provider_.OffsetRTVDescriptor(
            paint_context_.rtv_heap->GetCPUDescriptorHandleForHeapStart(),
            PaintContext::kRTVIndexHdrSource));
    D3D12_SHADER_RESOURCE_VIEW_DESC hdr_source_srv_desc = {};
    hdr_source_srv_desc.Format = HdrSourceFormat();
    hdr_source_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    hdr_source_srv_desc.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    hdr_source_srv_desc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(
        paint_context_.hdr_source.Get(), &hdr_source_srv_desc,
        provider_.OffsetViewDescriptor(
            paint_context_.view_heap->GetCPUDescriptorHandleForHeapStart(),
            PaintContext::kViewIndexHdrSourceSrv));
  }
  return true;
}

bool D3D12Presenter::DaoWaitForFrameLatency() {
  // With vsync, at most two frames wait to be shown: with DXGI's default
  // three, a 60.00 Hz guest on a 59.94 Hz display queued them up over a
  // minute, ~50 ms of latency. Two, not one, so a frame finished a little late
  // against the display's refresh doesn't hold the next one's GPU work back
  // (frame rate before latency). Waiting for room, not skipping the paint when
  // there's none: a guest frame landing just before the refresh would be
  // skipped and that refresh shown nothing new (below 60 fps on a 60 Hz
  // display). The timeout only guards against a swap chain that stopped
  // presenting (a display asleep): then nothing is presented, as a present
  // not waited for would let one more frame queue from then on.
  const UINT frame_latency = cvars::dao_present_vsync ? 2 : 3;
  if (paint_context_.frame_latency != frame_latency &&
      SUCCEEDED(paint_context_.swap_chain->SetMaximumFrameLatency(
          frame_latency))) {
    paint_context_.frame_latency = frame_latency;
  }
  if (paint_context_.frame_latency_waitable &&
      WaitForSingleObjectEx(paint_context_.frame_latency_waitable, 100,
                            FALSE) == WAIT_TIMEOUT) {
    static bool timeout_logged = false;
    if (!timeout_logged) {
      timeout_logged = true;
      XELOGW("D3D12Presenter: the swap chain had no room for 100 ms");
    }
    return false;
  }
  return true;
}

void D3D12Presenter::DaoEncodeHdrSource(
    ID3D12GraphicsCommandList* command_list, ID3D12Resource* back_buffer,
    ID3D12Resource* swap_chain_back_buffer,
    const D3D12_CPU_DESCRIPTOR_HANDLE& swap_chain_back_buffer_rtv,
    bool game_hdr) {
  D3D12_RESOURCE_BARRIER hdr_barriers[2] = {};
  hdr_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  hdr_barriers[0].Transition.pResource = back_buffer;
  hdr_barriers[0].Transition.Subresource =
      D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  hdr_barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  hdr_barriers[0].Transition.StateAfter =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  hdr_barriers[1] = hdr_barriers[0];
  hdr_barriers[1].Transition.pResource = swap_chain_back_buffer;
  hdr_barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
  hdr_barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  command_list->ResourceBarrier(2, hdr_barriers);
  command_list->OMSetRenderTargets(1, &swap_chain_back_buffer_rtv, true,
                                   nullptr);
  D3D12_VIEWPORT hdr_viewport = {
      0.0f, 0.0f, float(paint_context_.swap_chain_width),
      float(paint_context_.swap_chain_height), 0.0f, 1.0f};
  command_list->RSSetViewports(1, &hdr_viewport);
  D3D12_RECT hdr_scissor = {0, 0, LONG(paint_context_.swap_chain_width),
                            LONG(paint_context_.swap_chain_height)};
  command_list->RSSetScissorRects(1, &hdr_scissor);
  ID3D12DescriptorHeap* hdr_view_heap = paint_context_.view_heap.Get();
  command_list->SetDescriptorHeaps(1, &hdr_view_heap);
  command_list->SetPipelineState(hdr_output_pipeline_.Get());
  command_list->SetGraphicsRootSignature(hdr_output_root_signature_.Get());
  command_list->SetGraphicsRootDescriptorTable(
      0, provider_.OffsetViewDescriptor(
             hdr_view_heap->GetGPUDescriptorHandleForHeapStart(),
             PaintContext::kViewIndexHdrSourceSrv));
  const bool display_hdr = paint_context_.display_hdr;
  const float paper_white =
      float(std::max(cvars::present_hdr_paper_white, 80.0));
  // The scale to scRGB (1 = 80 nits): an SDR image's white is paper_white
  // on HDR displays and 1 on SDR ones; a PQ image is in nits, scaled by
  // 1/80 (HDR) or 1/paper_white (SDR); an scRGB one is copied.
  const bool pq_encoded = game_hdr && !hdr_scrgb_;
  float scale = display_hdr ? paper_white / 80.0f : 1.0f;
  if (pq_encoded) {
    scale = 1.0f / (display_hdr ? 80.0f : paper_white);
  }
  const float hdr_constants[4] = {
      scale,
      display_hdr
          ? std::max(float(cvars::present_hdr_peak) / paper_white, 1.0f)
          : 1.0f,
      // DAO: a gamma from the config the shader can use.
      float(std::clamp(cvars::present_hdr_gamma, 1.0, 3.0)),
      game_hdr ? (hdr_scrgb_ ? 2.0f : 1.0f) : 0.0f};
  command_list->SetGraphicsRoot32BitConstants(1, 4, hdr_constants, 0);
  command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  command_list->DrawInstanced(3, 1, 0, 0);
  hdr_barriers[0].Transition.StateBefore =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  hdr_barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
  command_list->ResourceBarrier(1, hdr_barriers);
}

void D3D12Presenter::DaoConvertScRgbCapture(const uint8_t* source,
                                            uint32_t row_pitch, bool game_hdr,
                                            RawImage& image_out) {
  const std::array<uint8_t, 0x8000>& to_8bpc = HalfTo8bpc(game_hdr);
  for (uint32_t y = 0; y < image_out.height; ++y) {
    uint8_t* dest_row = image_out.data.data() + size_t(image_out.stride) * y;
    const uint16_t* source_row = reinterpret_cast<const uint16_t*>(
        source + size_t(row_pitch) * y);
    for (uint32_t x = 0; x < image_out.width; ++x) {
      for (uint32_t c = 0; c < 3; ++c) {
        const uint16_t half = source_row[x * 4 + c];
        dest_row[x * 4 + c] = (half & 0x8000) ? 0 : to_8bpc[half];
      }
      dest_row[x * 4 + 3] = 0xFF;
    }
  }
}

}  // namespace d3d12
}  // namespace ui
}  // namespace xe
