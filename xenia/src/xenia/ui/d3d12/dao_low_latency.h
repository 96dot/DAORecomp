// DAO: low latency (dao_low_latency): the GPU vendor's technology that starts
// each frame as late as it can still be on time, so the controller is read
// closer to when the frame is shown. AMD Radeon Anti-Lag 2 (its SDK's header,
// thirdparty/antilag2, calling into the AMD driver). Off while frame generation
// is on: Anti-Lag 2 knows FSR 3's own frame generation swap chain, not our
// pacer's presents, and costs frames with them (a wait of 190 ms a frame with
// its frame generation calls, about a tenth of the frame rate without them).
#ifndef XENIA_UI_D3D12_DAO_LOW_LATENCY_H_
#define XENIA_UI_D3D12_DAO_LOW_LATENCY_H_

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

// The game's thread, once a frame, just before it reads the controller: waits
// as long as the technology has it wait.
void FrameStart();

}  // namespace dao_low_latency
}  // namespace d3d12
}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_D3D12_DAO_LOW_LATENCY_H_
