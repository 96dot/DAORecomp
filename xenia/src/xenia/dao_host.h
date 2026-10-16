// DAO recomp: the few services Xenia's GPU code needs from the host runtime
// (in the emulator these came from the kernel and CPU modules). Implemented in
// DAORecomp/gpu/xenia_host.cpp.
#ifndef XENIA_DAO_HOST_H_
#define XENIA_DAO_HOST_H_

#include <cstdint>

namespace xe::dao_host {

// Gives the calling host thread a guest identity so it can run guest code
// (interrupt callbacks). Idempotent.
void AttachGuestThread(const char* name);

// Runs the title's graphics interrupt callback (source 0 = vblank, 1 = command
// processor) on the calling thread, as if on hardware thread `cpu`.
void DispatchInterrupt(uint32_t callback, uint32_t user_data, uint32_t source,
                       uint32_t cpu);

uint32_t TitleId();

// Sleeps about `microseconds` (high resolution, unlike Sleep / NtDelayExecution
// on Windows).
void ShortSleep(uint32_t microseconds);

}  // namespace xe::dao_host

#endif  // XENIA_DAO_HOST_H_
