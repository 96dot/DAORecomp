"""Adds the PPC/VMX128 instructions DAO needs to XenonRecomp's recompiler.cpp.

Idempotent: run against a pristine recompiler.cpp (keeps a .orig backup).
usage: patch_recompiler.py <path to XenonRecomp/XenonRecomp/recompiler.cpp>
"""
import os
import shutil
import sys

NEW_CASES = r'''
    // ---- DAO additions: instructions XenonRecomp lacked ----

    // D-form loads/stores with update: operands { rT, D, rA }
    case PPC_INST_LHZU:
    case PPC_INST_LHAU:
    case PPC_INST_STHU:
    case PPC_INST_LFSU:
    case PPC_INST_LFDU:
    case PPC_INST_STFSU:
    case PPC_INST_STFDU:
        println("\t{} = {} + {}.u32;", ea(), int32_t(insn.operands[1]), r(insn.operands[2]));
        switch (insn.opcode->id)
        {
        case PPC_INST_LHZU: println("\t{}.u64 = PPC_LOAD_U16({});", r(insn.operands[0]), ea()); break;
        case PPC_INST_LHAU: println("\t{}.s64 = int16_t(PPC_LOAD_U16({}));", r(insn.operands[0]), ea()); break;
        case PPC_INST_STHU: println("\tPPC_STORE_U16({}, {}.u16);", ea(), r(insn.operands[0])); break;
        case PPC_INST_LFSU:
            printSetFlushMode(false);
            println("\t{}.u32 = PPC_LOAD_U32({});", temp(), ea());
            println("\t{}.f64 = double({}.f32);", f(insn.operands[0]), temp());
            break;
        case PPC_INST_LFDU:
            printSetFlushMode(false);
            println("\t{}.u64 = PPC_LOAD_U64({});", f(insn.operands[0]), ea());
            break;
        case PPC_INST_STFSU:
            printSetFlushMode(false);
            println("\t{}.f32 = float({}.f64);", temp(), f(insn.operands[0]));
            println("\tPPC_STORE_U32({}, {}.u32);", ea(), temp());
            break;
        case PPC_INST_STFDU:
            printSetFlushMode(false);
            println("\tPPC_STORE_U64({}, {}.u64);", ea(), f(insn.operands[0]));
            break;
        }
        println("\t{}.u32 = {};", r(insn.operands[2]), ea());
        break;

    // X-form loads/stores with update: operands { rT, rA, rB }
    case PPC_INST_LBZUX:
    case PPC_INST_LHZUX:
    case PPC_INST_LWZUX:
    case PPC_INST_LWAUX:
    case PPC_INST_LFSUX:
    case PPC_INST_LFDUX:
    case PPC_INST_STBUX:
    case PPC_INST_STHUX:
    case PPC_INST_STDUX:
    case PPC_INST_STFSUX:
        println("\t{} = {}.u32 + {}.u32;", ea(), r(insn.operands[1]), r(insn.operands[2]));
        switch (insn.opcode->id)
        {
        case PPC_INST_LBZUX: println("\t{}.u64 = PPC_LOAD_U8({});", r(insn.operands[0]), ea()); break;
        case PPC_INST_LHZUX: println("\t{}.u64 = PPC_LOAD_U16({});", r(insn.operands[0]), ea()); break;
        case PPC_INST_LWZUX: println("\t{}.u64 = PPC_LOAD_U32({});", r(insn.operands[0]), ea()); break;
        case PPC_INST_LWAUX: println("\t{}.s64 = int32_t(PPC_LOAD_U32({}));", r(insn.operands[0]), ea()); break;
        case PPC_INST_LFSUX:
            printSetFlushMode(false);
            println("\t{}.u32 = PPC_LOAD_U32({});", temp(), ea());
            println("\t{}.f64 = double({}.f32);", f(insn.operands[0]), temp());
            break;
        case PPC_INST_LFDUX:
            printSetFlushMode(false);
            println("\t{}.u64 = PPC_LOAD_U64({});", f(insn.operands[0]), ea());
            break;
        case PPC_INST_STBUX: println("\tPPC_STORE_U8({}, {}.u8);", ea(), r(insn.operands[0])); break;
        case PPC_INST_STHUX: println("\tPPC_STORE_U16({}, {}.u16);", ea(), r(insn.operands[0])); break;
        case PPC_INST_STDUX: println("\tPPC_STORE_U64({}, {}.u64);", ea(), r(insn.operands[0])); break;
        case PPC_INST_STFSUX:
            printSetFlushMode(false);
            println("\t{}.f32 = float({}.f64);", temp(), f(insn.operands[0]));
            println("\tPPC_STORE_U32({}, {}.u32);", ea(), temp());
            break;
        }
        println("\t{}.u32 = {};", r(insn.operands[1]), ea());
        break;

    case PPC_INST_EQV:
        println("\t{}.u64 = ~({}.u64 ^ {}.u64);", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;

    case PPC_INST_ADDC:
        println("\t{}.ca = {}.u32 + {}.u32 < {}.u32;", xer(), r(insn.operands[1]), r(insn.operands[2]), r(insn.operands[1]));
        println("\t{}.u64 = {}.u64 + {}.u64;", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;

    case PPC_INST_ADDME:
        // rD = rA + CA - 1; carry out unless rA == 0 && CA == 0
        println("\t{}.u8 = ({}.u32 != 0) | {}.ca;", temp(), r(insn.operands[1]), xer());
        println("\t{}.u64 = {}.u64 + {}.ca - 1;", r(insn.operands[0]), r(insn.operands[1]), xer());
        println("\t{}.ca = {}.u8;", xer(), temp());
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;

    case PPC_INST_SUBFZE:
        // rD = ~rA + CA; carry out only when ~rA == 0xFFFFFFFF and CA == 1
        println("\t{}.u8 = ({}.u32 == 0) & {}.ca;", temp(), r(insn.operands[1]), xer());
        println("\t{}.u64 = ~{}.u64 + {}.ca;", r(insn.operands[0]), r(insn.operands[1]), xer());
        println("\t{}.ca = {}.u8;", xer(), temp());
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;

    case PPC_INST_BDZF:
    case PPC_INST_BDNZT:
    {
        // operands { BI, target }; BI selects lt/gt/eq/so of cr[BI / 4]
        static constexpr const char* bits[] = { "lt", "gt", "eq", "so" };
        const bool zero = insn.opcode->id == PPC_INST_BDZF;
        const bool want = insn.opcode->id == PPC_INST_BDNZT;
        println("\t--{}.u64;", ctr());
        const auto cond = fmt::format("{}.u32 {} 0 && {}{}.{}", ctr(), zero ? "==" : "!=", want ? "" : "!", cr(insn.operands[0] / 4), bits[insn.operands[0] % 4]);
        if (insn.operands[1] < fn.base || insn.operands[1] >= fn.base + fn.size)
        {
            println("\tif ({}) {{", cond);
            print("\t");
            printFunctionCall(insn.operands[1]);
            println("\t\treturn;");
            println("\t}}");
        }
        else
        {
            println("\tif ({}) goto loc_{:X};", cond, insn.operands[1]);
        }
        break;
    }

    case PPC_INST_BSO:
        printConditionalBranch(false, "so");
        break;

    case PPC_INST_BNS:
        printConditionalBranch(true, "so");
        break;

    case PPC_INST_BSOLR:
        println("\tif ({}.so) return;", cr(insn.operands[0]));
        break;

    case PPC_INST_FRSQRTE:
        printSetFlushMode(false);
        println("\t{}.f64 = 1.0 / sqrt({}.f64);", f(insn.operands[0]), f(insn.operands[1]));
        break;

    case PPC_INST_DCBST:
        // no op
        break;

    case PPC_INST_LVEHX:
        // like lvewx: load the containing aligned vector (element position is preserved)
        print("\tsimde_mm_store_si128((simde__m128i*){}.u8, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)(base + ((", v(insn.operands[0]));
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32) & ~0xF))), simde_mm_load_si128((simde__m128i*)VectorMaskL)));", r(insn.operands[2]));
        break;

    case PPC_INST_VNOR128:
        println("\tsimde_mm_store_si128((simde__m128i*){}.u8, simde_mm_xor_si128(simde_mm_or_si128(simde_mm_load_si128((simde__m128i*){}.u8), simde_mm_load_si128((simde__m128i*){}.u8)), simde_mm_set1_epi32(-1)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;

    case PPC_INST_VSEL128:
        // operands { vD, vA, vB, vD }: vD = (vA & ~vD) | (vB & vD)
        println("\tsimde_mm_store_si128((simde__m128i*){}.u8, simde_mm_or_si128(simde_mm_andnot_si128(simde_mm_load_si128((simde__m128i*){}.u8), simde_mm_load_si128((simde__m128i*){}.u8)), simde_mm_and_si128(simde_mm_load_si128((simde__m128i*){}.u8), simde_mm_load_si128((simde__m128i*){}.u8))));", v(insn.operands[0]), v(insn.operands[3]), v(insn.operands[1]), v(insn.operands[3]), v(insn.operands[2]));
        break;

    case PPC_INST_VPKSWSS:
    case PPC_INST_VPKSWSS128:
        // vectors are stored reversed, so the high (vA) half goes second
        println("\tsimde_mm_store_si128((simde__m128i*){}.s16, simde_mm_packs_epi32(simde_mm_load_si128((simde__m128i*){}.s32), simde_mm_load_si128((simde__m128i*){}.s32)));", v(insn.operands[0]), v(insn.operands[2]), v(insn.operands[1]));
        break;

    case PPC_INST_VSUBSHS:
        println("\tsimde_mm_store_si128((simde__m128i*){}.s16, simde_mm_subs_epi16(simde_mm_load_si128((simde__m128i*){}.s16), simde_mm_load_si128((simde__m128i*){}.s16)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;

    case PPC_INST_VADDSWS:
        for (size_t i = 0; i < 4; i++)
            println("\t{{ int64_t s = int64_t({}.s32[{}]) + {}.s32[{}]; {}.s32[{}] = int32_t(s > 0x7FFFFFFFll ? 0x7FFFFFFFll : (s < -0x80000000ll ? -0x80000000ll : s)); }}", v(insn.operands[1]), i, v(insn.operands[2]), i, vTemp(), i);
        println("\t{} = {};", v(insn.operands[0]), vTemp());
        break;

    case PPC_INST_VCFPUXWS128:
    {
        // float -> u32 with saturation, scaled by 2^uimm; NaN and negatives become 0
        printSetFlushMode(true);
        const float scale = ldexp(1.0f, int32_t(insn.operands[2]));
        for (size_t i = 0; i < 4; i++)
        {
            println("\t{{ float x = {}.f32[{}] * float({});{}.u32[{}] = !(x > 0.0f) ? 0u : (x >= 4294967296.0f ? 0xFFFFFFFFu : uint32_t(x)); }}",
                v(insn.operands[1]), i, scale, vTemp(), i);
        }
        println("\t{} = {};", v(insn.operands[0]), vTemp());
        break;
    }

    case PPC_INST_VCMPBFP:
    case PPC_INST_VCMPBFP128:
        // bounds compare: bit31 = !(a <= b), bit30 = !(a >= -b); NaN sets both
        printSetFlushMode(true);
        println("\tsimde_mm_store_si128((simde__m128i*){}.u8, simde_mm_or_si128("
            "simde_mm_andnot_si128(simde_mm_castps_si128(simde_mm_cmple_ps(simde_mm_load_ps({}.f32), simde_mm_load_ps({}.f32))), simde_mm_set1_epi32(int(0x80000000))), "
            "simde_mm_andnot_si128(simde_mm_castps_si128(simde_mm_cmpge_ps(simde_mm_load_ps({}.f32), simde_mm_xor_ps(simde_mm_load_ps({}.f32), simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x80000000)))))), simde_mm_set1_epi32(0x40000000))));",
            vTemp(), v(insn.operands[1]), v(insn.operands[2]), v(insn.operands[1]), v(insn.operands[2]));
        println("\t{} = {};", v(insn.operands[0]), vTemp());
        if (strchr(insn.opcode->name, '.'))
        {
            println("\t{}.lt = 0; {}.gt = 0; {}.so = 0;", cr(6), cr(6), cr(6));
            println("\t{}.eq = simde_mm_movemask_epi8(simde_mm_cmpeq_epi32(simde_mm_load_si128((simde__m128i*){}.u8), simde_mm_setzero_si128())) == 0xFFFF;", cr(6), vTemp());
        }
        break;

'''

OLD_VCMPBFP = '''    case PPC_INST_VCMPBFP:
    case PPC_INST_VCMPBFP128:
        println("\\t__builtin_debugtrap();");
        break;

'''

OLD_BDNZF = '''        // NOTE: assuming eq here as a shortcut because all the instructions in the game do that
        println("\\t--{}.u64;", ctr());
        println("\\tif ({}.u32 != 0 && !{}.eq) goto loc_{:X};", ctr(), cr(insn.operands[0] / 4), insn.operands[1]);'''

NEW_BDNZF = '''    {
        // BI selects lt/gt/eq/so of cr[BI / 4] (upstream assumed eq)
        static constexpr const char* bits[] = { "lt", "gt", "eq", "so" };
        println("\\t--{}.u64;", ctr());
        println("\\tif ({}.u32 != 0 && !{}.{}) goto loc_{:X};", ctr(), cr(insn.operands[0] / 4), bits[insn.operands[0] % 4], insn.operands[1]);
    }'''

ANCHOR = '    default:\n        return false;\n    }'

# mftb must tick at the 360's 49.875 MHz, not the host TSC rate.
OLD_MFTB = '''        println("\\t{}.u64 = __rdtsc();", r(insn.operands[0]));'''
NEW_MFTB = '''        println("\\t{}.u64 = PPC_QUERY_TIMEBASE();", r(insn.operands[0]));'''

# ...which the runtime provides through this hook in the generated ppc_context.h.
CONTEXT_ANCHOR = '#define PPC_MEMORY_SIZE 0x100000000ull'
CONTEXT_ADDITION = '''#define PPC_MEMORY_SIZE 0x100000000ull

#ifndef PPC_QUERY_TIMEBASE
uint64_t PPCQueryTimebase() noexcept;
#define PPC_QUERY_TIMEBASE() PPCQueryTimebase()
#endif'''


def write_if_changed(path, text):
    """Writes text unless the file already has it (keeps the timestamp, so builds stay incremental)."""
    if os.path.exists(path) and open(path, encoding='utf-8', newline='').read() == text:
        return False
    open(path, 'w', encoding='utf-8', newline='').write(text)
    return True


def patch_context(path):
    orig = path + '.orig'
    if not os.path.exists(orig):
        shutil.copyfile(path, orig)
    s = open(orig, encoding='utf-8').read()
    assert s.count(CONTEXT_ANCHOR) == 1, 'ppc_context anchor'
    if write_if_changed(path, s.replace(CONTEXT_ANCHOR, CONTEXT_ADDITION)):
        print('patched', path)


def patch(path):
    """Patches recompiler.cpp at path (and XenonUtils/ppc_context.h next to it)."""
    orig = path + '.orig'
    if not os.path.exists(orig):
        shutil.copyfile(path, orig)
    s = open(orig, encoding='utf-8').read()
    for needle in (ANCHOR, OLD_VCMPBFP, OLD_BDNZF, OLD_MFTB):
        assert s.count(needle) == 1, needle[:60]
    s = s.replace(OLD_VCMPBFP, '').replace(OLD_BDNZF, NEW_BDNZF).replace(OLD_MFTB, NEW_MFTB)
    s = s.replace(ANCHOR, NEW_CASES + ANCHOR)
    if write_if_changed(path, s):
        print('patched', path)
    patch_context(os.path.join(os.path.dirname(path), '..', 'XenonUtils', 'ppc_context.h'))


def main():
    patch(sys.argv[1])


if __name__ == '__main__':
    main()
