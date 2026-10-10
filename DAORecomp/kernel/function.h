#pragma once
// Bridge between recompiled guest calls and host C++ import implementations.
//
//   uint32_t NtClose(uint32_t handle);
//   DAO_IMPORT(NtClose)            -> defines __imp__NtClose
//
// Integer-like parameters come from r3..r10, float/double from f1..f13 (separate
// counters, as on the 360 ABI). Pointer parameters are guest addresses translated
// to host pointers (0 -> nullptr). Return values go to r3 (pointers converted back
// to guest addresses) or f1.

#include "types.h"
#include "memory.h"

namespace dao::kernel
{
    void NoteImport(const char* name, const PPCContext& ctx);
}

namespace dao::bridge
{
    struct ArgState
    {
        int gpr = 0;
        int fpr = 0;
    };

    inline PPCRegister& Gpr(PPCContext& ctx, int index)
    {
        PPCRegister* regs[] = { &ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10 };
        return *regs[index];
    }

    inline PPCRegister& Fpr(PPCContext& ctx, int index)
    {
        PPCRegister* regs[] = { &ctx.f1, &ctx.f2, &ctx.f3, &ctx.f4, &ctx.f5, &ctx.f6, &ctx.f7,
            &ctx.f8, &ctx.f9, &ctx.f10, &ctx.f11, &ctx.f12, &ctx.f13 };
        return *regs[index];
    }

    // Reads the index'th (0-based) integer argument; arguments past the eighth
    // live in 8-byte slots of the caller's parameter area starting at r1+0x50
    // (a 32-bit argument is stored with stw at the low word, r1+0x54).
    inline uint64_t IntArg(PPCContext& ctx, int index)
    {
        if (index < 8)
            return Gpr(ctx, index).u64;
        return *kernel::g_memory.Translate<be<uint64_t>>(ctx.r1.u32 + 0x50 + (index - 8) * 8);
    }

    template<typename T>
    T GetArg(PPCContext& ctx, ArgState& state)
    {
        if constexpr (std::is_same_v<T, PPCContext&>)
        {
            return ctx;
        }
        else if constexpr (std::is_floating_point_v<T>)
        {
            const double value = Fpr(ctx, state.fpr++).f64;
            return T(value);
        }
        else
        {
            const int index = state.gpr++;
            const uint64_t raw = IntArg(ctx, index);

            if constexpr (std::is_pointer_v<T>)
            {
                const uint32_t address = uint32_t(raw);
                return address ? reinterpret_cast<T>(kernel::g_memory.Translate(address)) : nullptr;
            }
            else if constexpr (std::is_same_v<T, bool>)
            {
                return uint32_t(raw) != 0;
            }
            else if constexpr (std::is_enum_v<T>)
            {
                return T(uint32_t(raw));
            }
            else if constexpr (sizeof(T) == 8)
            {
                return T(raw);
            }
            else
            {
                return T(uint32_t(raw));
            }
        }
    }

    template<typename T>
    void SetResult(PPCContext& ctx, T value)
    {
        if constexpr (std::is_floating_point_v<T>)
            ctx.f1.f64 = double(value);
        else if constexpr (std::is_pointer_v<T>)
            ctx.r3.u64 = kernel::g_memory.ToGuest(value);
        else if constexpr (std::is_same_v<T, bool>)
            ctx.r3.u64 = value ? 1 : 0;
        else if constexpr (std::is_signed_v<T> && sizeof(T) < 8)
            ctx.r3.s64 = int64_t(value);
        else
            ctx.r3.u64 = uint64_t(value);
    }
}

namespace dao::bridge::detail
{
    template<auto Func, typename R, typename... Args>
    inline void InvokeImpl(PPCContext& ctx, R (*)(Args...))
    {
        ArgState state;
        std::tuple<Args...> args{ GetArg<Args>(ctx, state)... };
        if constexpr (std::is_void_v<R>)
            std::apply(Func, args);
        else
            SetResult(ctx, std::apply(Func, args));
    }
}

#define DAO_IMPORT_AS(importName, func) \
    PPC_FUNC(__imp__##importName)     {         (void)base;         ::dao::kernel::NoteImport(#importName, ctx); \
        LOGT(#importName "({:08X}, {:08X}, {:08X}, {:08X}) lr {:08X}", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, uint32_t(ctx.lr));         ::dao::bridge::detail::InvokeImpl<&func>(ctx, &func);     }

#define DAO_IMPORT(name) DAO_IMPORT_AS(name, name)

// Logs once per import name and returns 0.
namespace dao
{
    void ReportStub(const char* name, PPCContext& ctx);
}

#define DAO_STUB(name) \
    PPC_FUNC(__imp__##name) { (void)base; ::dao::ReportStub(#name, ctx); ctx.r3.u64 = 0; }
#define DAO_STUB_RETURN(name, value) \
    PPC_FUNC(__imp__##name) { (void)base; ::dao::ReportStub(#name, ctx); ctx.r3.u64 = (value); }
