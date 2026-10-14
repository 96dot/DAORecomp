// DAO: low latency (dao_low_latency): the GPU vendor's technology that starts
// each frame as late as it can still be on time, so the controller is read
// closer to when the frame is shown. AMD Radeon Anti-Lag 2 (its SDK's header,
// thirdparty/antilag2, calling into the AMD driver), or NVIDIA Reflex through
// Streamline (dao_streamline.h; experimental, untested). Off while frame
// generation is on: Anti-Lag 2 knows FSR 3's own frame generation swap chain,
// not our pacer's presents, and costs frames with them (a wait of 190 ms a
// frame with its frame generation calls, about a tenth of the frame rate
// without them); Reflex likewise, untried. Reflex also has markers for each
// frame's stages (PC Latency), from the threads they're on.
#ifndef XENIA_UI_D3D12_DAO_LOW_LATENCY_H_
#define XENIA_UI_D3D12_DAO_LOW_LATENCY_H_

#include <cstdint>

#include "xenia/ui/d3d12/d3d12_api.h"

namespace xe {
namespace ui {
namespace d3d12 {
namespace dao_low_latency {

// The device was created (D3D12Provider), before the game's thread starts:
// finds what the GPU's driver has.
void Initialize(ID3D12Device* device);
// Before the device is released, after the game's thread has stopped.
void Shutdown();
// Low latency was off as the game started on an NVIDIA GPU that would have
// Reflex: turning it on takes effect at the next start (Streamline starts
// before the device).
bool NeedsRestart();

// The game's thread, once a frame, just before it reads the controller: waits
// as long as the technology has it wait.
void FrameStart();
// The render thread, at each swap (VdSwap), before the game's thread can start
// the next frame: the frame's commands are written.
void FrameSwapped();
// The GPU thread, as it issues each swap: the swap's frame (0 for none), for
// RenderingSubmitted.
uint32_t SwapIssued();
// The GPU thread, until the next swap is issued: the frame of the swap
// SwapIssued last returned (0 for none), for Streamline's other features.
uint32_t IssuedFrame();
// The token (sl::FrameToken*) Reflex has for a frame, while it's still that
// frame's, or nullptr: Streamline keeps only so many, so its other features
// take this one rather than another of the same frame.
void* FrameToken(uint32_t frame);
// The GPU thread, once a swap's frame's commands are submitted (before the
// presenter has it).
void RenderingSubmitted(uint32_t frame);
// The presenter, just before and after each present.
void BeforePresent();
void AfterPresent();

}  // namespace dao_low_latency
}  // namespace d3d12
}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_D3D12_DAO_LOW_LATENCY_H_
