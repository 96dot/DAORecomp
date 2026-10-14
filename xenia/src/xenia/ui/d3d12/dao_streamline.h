// DAO: NVIDIA Streamline (tools/Streamline-SDK, its DLLs next to the
// executable): the way to NVIDIA's Reflex and DLSS. Loaded only on an NVIDIA
// GPU and only when low latency is on as the game starts, from the
// executable's folder, its sl.interposer.dll exactly the installer's (its
// SHA-256 pinned). Manual hooking: the device's direct queue and the swap
// chain are created through Streamline's proxies (it hooks Present through
// the swap chain's), everything else uses the native interfaces.
// Experimental: built without an NVIDIA GPU to test on.
#ifndef XENIA_UI_D3D12_DAO_STREAMLINE_H_
#define XENIA_UI_D3D12_DAO_STREAMLINE_H_

#include <cstdint>

#include "xenia/ui/d3d12/d3d12_api.h"

namespace xe {
namespace ui {
namespace d3d12 {
namespace dao_streamline {

// D3D12Provider, once the adapter is chosen and before the device is created
// on it: starts Streamline if it's wanted and can be (slInit).
void Initialize(const DXGI_ADAPTER_DESC& adapter);
// The device was created, before anything else is created with it or the
// factory: gives it to Streamline (slSetD3DDevice) and gets its proxies of
// both; stops it if it can't.
void SetDevice(IDXGIFactory2* factory, ID3D12Device* device);
// Creates the direct queue, through Streamline's device proxy when it's on;
// *queue is the native queue all the same (its proxy's GetDevice would give
// the proxy device, which others such as neural rendering don't know).
HRESULT CreateDirectQueue(ID3D12Device* device,
                          const D3D12_COMMAND_QUEUE_DESC& desc,
                          ID3D12CommandQueue** queue);
// The factory and queue to create the swap chain with when Streamline is on
// (its proxies, owned here), else nullptr.
IDXGIFactory2* SwapChainFactory();
ID3D12CommandQueue* SwapChainQueue();
// Before the device is released (slShutdown), or before the swap chain is
// created if what it's for can't be had (the factory and queue are then the
// native ones).
void Shutdown();

// Streamline is running on the device.
bool Active();
// Streamline wasn't started only because low latency was off: turned on, it
// starts with the next start (an NVIDIA GPU, sl.interposer.dll checked).
bool StartsAfterRestart();

// A feature's function (slGetFeatureFunction), or nullptr; feature is an
// sl::Feature.
void* FeatureFunction(uint32_t feature, const char* name);
// The token of frame index (slGetNewFrameToken; an sl::FrameToken*), or
// nullptr.
void* FrameToken(uint32_t index);

}  // namespace dao_streamline
}  // namespace d3d12
}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_D3D12_DAO_STREAMLINE_H_
