#pragma once
// Force-included into the recompiled code (see CMakeLists.txt). XenonRecomp emits
// PPC_MM_STORE_U32 for stores preceded by eieio; the ones that hit the GPU register
// window (0x7FC80000) are forwarded to the runtime's fake command processor.
#include <cstdint>

void DaoGpuRegisterWrite(uint32_t address, uint32_t value);

#define PPC_MM_STORE_U32(x, y)                                    \
    do                                                            \
    {                                                             \
        const uint32_t daoMmAddress_ = (x);                       \
        const uint32_t daoMmValue_ = (y);                         \
        PPC_STORE_U32(daoMmAddress_, daoMmValue_);                \
        if ((daoMmAddress_ & 0xFFFF0000u) == 0x7FC80000u)         \
            DaoGpuRegisterWrite(daoMmAddress_, daoMmValue_);      \
    } while (0)
