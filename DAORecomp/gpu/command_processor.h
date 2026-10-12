#pragma once
// A minimal Xenos command processor, used only when the Xenia GPU isn't running. It
// consumes the PM4 ring buffer the game's D3D writes, executes the packets that have
// CPU-visible side effects (fences, memory writes, waits, interrupts, register
// writes) and publishes the read pointer, so the game's D3D layer sees a live GPU.
// Draw packets are not executed (Xenia's D3D12 backend renders them otherwise).

namespace dao::gpu
{
    // Xenos register indices (dword offsets into the 0x7FC80000 MMIO window).
    namespace reg
    {
        constexpr uint32_t CP_RB_RPTR = 0x01C4;
        constexpr uint32_t CP_RB_WPTR = 0x01C5;
        constexpr uint32_t SCRATCH_UMSK = 0x01DC;
        constexpr uint32_t SCRATCH_ADDR = 0x01DD;
        constexpr uint32_t SCRATCH_REG0 = 0x0578;
        constexpr uint32_t COHER_STATUS_HOST = 0x0A31;
        constexpr uint32_t D1MODE_VBLANK_STATUS = 0x1951;
    }

    constexpr uint32_t kRegisterWindow = 0x7FC80000;
    constexpr uint32_t kRegisterCount = 0x10000;

    // Starts the command processor and vblank threads (after VdInitializeRingBuffer).
    void StartCommandProcessor();

    // MMIO register write from guest code.
    void OnRegisterWrite(uint32_t index, uint32_t value);

    uint32_t ReadRegister(uint32_t index);

    // Runs the guest graphics interrupt callback (source 0 = vblank, 1 = command processor).
    void DispatchInterrupt(uint32_t source, uint32_t cpu);

    // Host-visible frame boundary (one per XE_SWAP packet the CP executes).
    uint64_t FramesPresented();
}
