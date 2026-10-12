// Before anything else runs: whether this PC's processor has the instructions the
// program was built for (DAO_MARCH, see ../CMakeLists.txt). Without them it would stop
// at its first such instruction with no word why, e.g. an install built for a newer
// processor and copied to another PC. This file is built for any x86-64 processor (see
// CMakeLists.txt). The feature lists match installer/pipeline.py's V3_MACROS.
#include <intrin.h>
#include <windows.h>

namespace
{
    bool Bit(int value, int bit)
    {
        return (value >> bit) & 1;
    }

    // XCR0: whether Windows saves the AVX registers (bits 1 and 2), so AVX can be used.
    __attribute__((target("xsave"))) bool AvxEnabled()
    {
        return (_xgetbv(0) & 6) == 6;
    }

    bool HasRequiredInstructions()
    {
#if DAO_CPU_LEVEL >= 1
        int r[4];
        __cpuid(r, 1);
        const int ecx1 = r[2];
        // Sandy Bridge's: SSE4.2, POPCNT, OSXSAVE and AVX.
        if (!Bit(ecx1, 20) || !Bit(ecx1, 23) || !Bit(ecx1, 27) || !Bit(ecx1, 28) || !AvxEnabled())
            return false;
#endif
#if DAO_CPU_LEVEL >= 3
        // x86-64-v3's: FMA, MOVBE and F16C (leaf 1), BMI1, AVX2 and BMI2 (leaf 7), LZCNT
        // (leaf 0x80000001).
        if (!Bit(ecx1, 12) || !Bit(ecx1, 22) || !Bit(ecx1, 29))
            return false;
        __cpuid(r, 0);
        if (r[0] < 7)
            return false;
        __cpuidex(r, 7, 0);
        if (!Bit(r[1], 3) || !Bit(r[1], 5) || !Bit(r[1], 8))
            return false;
        __cpuid(r, 0x80000000);
        if (unsigned(r[0]) < 0x80000001u)
            return false;
        __cpuid(r, 0x80000001);
        if (!Bit(r[2], 5))
            return false;
#endif
        return true;
    }

    void NTAPI CheckCpu(void*, DWORD reason, void*)
    {
        if (reason != DLL_PROCESS_ATTACH || HasRequiredInstructions())
            return;
#if DAO_CPU_LEVEL >= 3
        const wchar_t* message =
            L"This copy of Dragon Age: Origins (recompiled) was built for a newer processor than this "
            L"PC's: one with AVX2 (Intel from 2013, AMD from 2015, or later).\n\n"
            L"To build it for this PC, run the installer (DAORecomp.exe from the release page) here "
            L"with your disc image, and choose this install folder.";
#else
        const wchar_t* message =
            L"Dragon Age: Origins (recompiled) needs a processor with AVX (most from 2011 or later).";
#endif
        MessageBoxW(nullptr, message, L"Dragon Age: Origins", MB_OK | MB_ICONERROR);
        // (Not ExitProcess: this runs inside the loader, which that would re-enter.)
        TerminateProcess(GetCurrentProcess(), 1);
    }
}

// A TLS callback in .CRT$XLB: the loader runs these for the main thread before the
// program's entry point, in name order, so ahead of the C runtime's own (.CRT$XLC),
// which runs thread_local initializers built for DAO_MARCH.
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:g_daoCheckCpu")
#pragma section(".CRT$XLB", read)
extern "C" __declspec(allocate(".CRT$XLB")) const PIMAGE_TLS_CALLBACK g_daoCheckCpu = CheckCpu;
