/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/arm64/a64_address_space.h"

#include "dynarmic/backend/arm64/a64_jitstate.h"
#include "dynarmic/backend/arm64/abi.h"
#include "dynarmic/backend/arm64/devirtualize.h"
#include "dynarmic/backend/arm64/emit_arm64.h"
#include "dynarmic/backend/arm64/stack_layout.h"
#include "dynarmic/common/cast_util.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/frontend/A64/translate/a64_translate.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "dynarmic/ir/opt/passes.h"

namespace Dynarmic::Backend::Arm64 {

template<auto mfp, typename T>
static void* EmitCallTrampoline(oaknut::CodeGenerator& code, T* this_) {
    using namespace oaknut::util;

    const auto info = Devirtualize<mfp>(this_);

    oaknut::Label l_addr, l_this;

    void* target = code.xptr<void*>();
    code.LDR(X0, l_this);
    code.LDR(Xscratch0, l_addr);
    code.BR(Xscratch0);

    code.align(8);
    code.l(l_this);
    code.dx(info.this_ptr);
    code.l(l_addr);
    code.dx(info.fn_ptr);

    return target;
}

template<auto mfp, typename T>
static void* EmitWrappedReadCallTrampoline(oaknut::CodeGenerator& code, T* this_) {
    using namespace oaknut::util;

    const auto info = Devirtualize<mfp>(this_);

    oaknut::Label l_addr, l_this;

    constexpr u64 save_regs = ABI_CALLER_SAVE & ~ToRegList(Xscratch0);

    void* target = code.xptr<void*>();
    ABI_PushRegisters(code, save_regs, 0);
    code.LDR(X0, l_this);
    code.MOV(X1, Xscratch0);
    code.LDR(Xscratch0, l_addr);
    code.BLR(Xscratch0);
    code.MOV(Xscratch0, X0);
    ABI_PopRegisters(code, save_regs, 0);
    code.RET();

    code.align(8);
    code.l(l_this);
    code.dx(info.this_ptr);
    code.l(l_addr);
    code.dx(info.fn_ptr);

    return target;
}

template<auto callback, typename T>
static void* EmitExclusiveReadCallTrampoline(oaknut::CodeGenerator& code, const A64::UserConfig& conf) {
    using namespace oaknut::util;

    oaknut::Label l_addr, l_this;

    auto fn = [](const A64::UserConfig& conf, A64::VAddr vaddr) -> T {
        return conf.global_monitor->ReadAndMark<T>(conf.processor_id, vaddr, [&]() -> T {
            return (conf.callbacks->*callback)(vaddr);
        });
    };

    void* target = code.xptr<void*>();
    code.LDR(X0, l_this);
    code.LDR(Xscratch0, l_addr);
    code.BR(Xscratch0);

    code.align(8);
    code.l(l_this);
    code.dx(mcl::bit_cast<u64>(&conf));
    code.l(l_addr);
    code.dx(mcl::bit_cast<u64>(Common::FptrCast(fn)));

    return target;
}

template<auto mfp, typename T>
static void* EmitWrappedWriteCallTrampoline(oaknut::CodeGenerator& code, T* this_) {
    using namespace oaknut::util;

    const auto info = Devirtualize<mfp>(this_);

    oaknut::Label l_addr, l_this;

    constexpr u64 save_regs = ABI_CALLER_SAVE;

    void* target = code.xptr<void*>();
    ABI_PushRegisters(code, save_regs, 0);
    code.LDR(X0, l_this);
    code.MOV(X1, Xscratch0);
    code.MOV(X2, Xscratch1);
    code.LDR(Xscratch0, l_addr);
    code.BLR(Xscratch0);
    ABI_PopRegisters(code, save_regs, 0);
    code.RET();

    code.align(8);
    code.l(l_this);
    code.dx(info.this_ptr);
    code.l(l_addr);
    code.dx(info.fn_ptr);

    return target;
}

template<auto callback, typename T>
static void* EmitExclusiveWriteCallTrampoline(oaknut::CodeGenerator& code, const A64::UserConfig& conf) {
    using namespace oaknut::util;

    oaknut::Label l_addr, l_this;

    auto fn = [](const A64::UserConfig& conf, A64::VAddr vaddr, T value) -> u32 {
        return conf.global_monitor->DoExclusiveOperation<T>(conf.processor_id, vaddr,
                                                            [&](T expected) -> bool {
                                                                return (conf.callbacks->*callback)(vaddr, value, expected);
                                                            })
                 ? 0
                 : 1;
    };

    void* target = code.xptr<void*>();
    code.LDR(X0, l_this);
    code.LDR(Xscratch0, l_addr);
    code.BR(Xscratch0);

    code.align(8);
    code.l(l_this);
    code.dx(mcl::bit_cast<u64>(&conf));
    code.l(l_addr);
    code.dx(mcl::bit_cast<u64>(Common::FptrCast(fn)));

    return target;
}

static void* EmitRead128CallTrampoline(oaknut::CodeGenerator& code, A64::UserCallbacks* this_) {
    using namespace oaknut::util;

    const auto info = Devirtualize<&A64::UserCallbacks::MemoryRead128>(this_);

    oaknut::Label l_addr, l_this;

    void* target = code.xptr<void*>();
    ABI_PushRegisters(code, (1ull << 29) | (1ull << 30), 0);
    code.LDR(X0, l_this);
    code.LDR(Xscratch0, l_addr);
    code.BLR(Xscratch0);
    code.FMOV(D0, X0);
    code.FMOV(V0.D()[1], X1);
    ABI_PopRegisters(code, (1ull << 29) | (1ull << 30), 0);
    code.RET();

    code.align(8);
    code.l(l_this);
    code.dx(info.this_ptr);
    code.l(l_addr);
    code.dx(info.fn_ptr);

    return target;
}

static void* EmitWrappedRead128CallTrampoline(oaknut::CodeGenerator& code, A64::UserCallbacks* this_) {
    using namespace oaknut::util;

    const auto info = Devirtualize<&A64::UserCallbacks::MemoryRead128>(this_);

    oaknut::Label l_addr, l_this;

    constexpr u64 save_regs = ABI_CALLER_SAVE & ~ToRegList(Q0);

    void* target = code.xptr<void*>();
    ABI_PushRegisters(code, save_regs, 0);
    code.LDR(X0, l_this);
    code.MOV(X1, Xscratch0);
    code.LDR(Xscratch0, l_addr);
    code.BLR(Xscratch0);
    code.FMOV(D0, X0);
    code.FMOV(V0.D()[1], X1);
    ABI_PopRegisters(code, save_regs, 0);
    code.RET();

    code.align(8);
    code.l(l_this);
    code.dx(info.this_ptr);
    code.l(l_addr);
    code.dx(info.fn_ptr);

    return target;
}

static void* EmitExclusiveRead128CallTrampoline(oaknut::CodeGenerator& code, const A64::UserConfig& conf) {
    using namespace oaknut::util;

    oaknut::Label l_addr, l_this;

    auto fn = [](const A64::UserConfig& conf, A64::VAddr vaddr) -> Vector {
        return conf.global_monitor->ReadAndMark<Vector>(conf.processor_id, vaddr, [&]() -> Vector {
            return conf.callbacks->MemoryRead128(vaddr);
        });
    };

    void* target = code.xptr<void*>();
    ABI_PushRegisters(code, (1ull << 29) | (1ull << 30), 0);
    code.LDR(X0, l_this);
    code.LDR(Xscratch0, l_addr);
    code.BLR(Xscratch0);
    code.FMOV(D0, X0);
    code.FMOV(V0.D()[1], X1);
    ABI_PopRegisters(code, (1ull << 29) | (1ull << 30), 0);
    code.RET();

    code.align(8);
    code.l(l_this);
    code.dx(mcl::bit_cast<u64>(&conf));
    code.l(l_addr);
    code.dx(mcl::bit_cast<u64>(Common::FptrCast(fn)));

    return target;
}

static void* EmitWrite128CallTrampoline(oaknut::CodeGenerator& code, A64::UserCallbacks* this_) {
    using namespace oaknut::util;

    const auto info = Devirtualize<&A64::UserCallbacks::MemoryWrite128>(this_);

    oaknut::Label l_addr, l_this;

    void* target = code.xptr<void*>();
    code.LDR(X0, l_this);
    code.FMOV(X2, D0);
    code.FMOV(X3, V0.D()[1]);
    code.LDR(Xscratch0, l_addr);
    code.BR(Xscratch0);

    code.align(8);
    code.l(l_this);
    code.dx(info.this_ptr);
    code.l(l_addr);
    code.dx(info.fn_ptr);

    return target;
}

static void* EmitWrappedWrite128CallTrampoline(oaknut::CodeGenerator& code, A64::UserCallbacks* this_) {
    using namespace oaknut::util;

    const auto info = Devirtualize<&A64::UserCallbacks::MemoryWrite128>(this_);

    oaknut::Label l_addr, l_this;

    constexpr u64 save_regs = ABI_CALLER_SAVE;

    void* target = code.xptr<void*>();
    ABI_PushRegisters(code, save_regs, 0);
    code.LDR(X0, l_this);
    code.MOV(X1, Xscratch0);
    code.FMOV(X2, D0);
    code.FMOV(X3, V0.D()[1]);
    code.LDR(Xscratch0, l_addr);
    code.BLR(Xscratch0);
    ABI_PopRegisters(code, save_regs, 0);
    code.RET();

    code.align(8);
    code.l(l_this);
    code.dx(info.this_ptr);
    code.l(l_addr);
    code.dx(info.fn_ptr);

    return target;
}

static void* EmitExclusiveWrite128CallTrampoline(oaknut::CodeGenerator& code, const A64::UserConfig& conf) {
    using namespace oaknut::util;

    oaknut::Label l_addr, l_this;

    auto fn = [](const A64::UserConfig& conf, A64::VAddr vaddr, Vector value) -> u32 {
        return conf.global_monitor->DoExclusiveOperation<Vector>(conf.processor_id, vaddr,
                                                                 [&](Vector expected) -> bool {
                                                                     return conf.callbacks->MemoryWriteExclusive128(vaddr, value, expected);
                                                                 })
                 ? 0
                 : 1;
    };

    void* target = code.xptr<void*>();
    code.LDR(X0, l_this);
    code.FMOV(X2, D0);
    code.FMOV(X3, V0.D()[1]);
    code.LDR(Xscratch0, l_addr);
    code.BR(Xscratch0);

    code.align(8);
    code.l(l_this);
    code.dx(mcl::bit_cast<u64>(&conf));
    code.l(l_addr);
    code.dx(mcl::bit_cast<u64>(Common::FptrCast(fn)));

    return target;
}

A64AddressSpace::A64AddressSpace(const A64::UserConfig& conf)
        : AddressSpace(conf.code_cache_size)
        , conf(conf) {
}

IR::Block A64AddressSpace::GenerateIR(IR::LocationDescriptor descriptor, u64& pc, u32& inst) const {
    const auto get_code = [this](u64 vaddr) { return conf.callbacks->MemoryReadCode(vaddr); };
    pc = A64::LocationDescriptor{descriptor}.PC();
    inst = get_code(pc).value();
    IR::Block ir_block = A64::Translate(A64::LocationDescriptor{descriptor}, get_code,
                                        {conf.define_unpredictable_behaviour, conf.wall_clock_cntpct});

    Optimization::A64CallbackConfigPass(ir_block, conf);
    Optimization::NamingPass(ir_block);
    if (conf.HasOptimization(OptimizationFlag::GetSetElimination) && !conf.check_halt_on_memory_access) {
        Optimization::A64GetSetElimination(ir_block);
        Optimization::DeadCodeElimination(ir_block);
    }
    if (conf.HasOptimization(OptimizationFlag::ConstProp)) {
        Optimization::ConstantPropagation(ir_block);
        Optimization::DeadCodeElimination(ir_block);
    }
    if (conf.HasOptimization(OptimizationFlag::MiscIROpt)) {
        Optimization::A64MergeInterpretBlocksPass(ir_block, conf.callbacks);
    }
    Optimization::VerificationPass(ir_block);

    return ir_block;
}

CodePtr A64AddressSpace::GetOrEmit(A64JitState& context) {
    auto location = context.GetLocationDescriptor();
    if (location.Value() == 0) {
        printf("GetOrEmit pc == 0\n");
    }
    auto codePtr = AddressSpace::GetOrEmit(location);
    context.last_code_pc = context.code_pc;
    context.code_pc = location.Value();
    context.last_code = context.code;
    context.code = reinterpret_cast<u64>(codePtr);
    return codePtr;
}

void A64AddressSpace::InvalidateCacheRanges(const boost::icl::interval_set<u64>& ranges) {
    InvalidateBasicBlocks(block_ranges.InvalidateRanges(ranges));
}

void A64AddressSpace::GenHaltReasonSet(oaknut::Label& run_code_entry) {
    oaknut::Label _dummy;
    GenHaltReasonSetImpl(false, run_code_entry, _dummy);
}
void A64AddressSpace::GenHaltReasonSet(oaknut::Label& run_code_entry, oaknut::Label& ret_code_entry) {
    GenHaltReasonSetImpl(true, run_code_entry, ret_code_entry);
}
void A64AddressSpace::GenHaltReasonSetImpl(bool isRet, oaknut::Label& run_code_entry, oaknut::Label& ret_code_entry) {
    using namespace oaknut::util;
    oaknut::Label normal_code, halt_reason_set, halt_hr_loop;
    if (halt_reason_on_run) {
        if (isRet) {
            code.SUB(SP, SP, 16);
            code.STP(X0, X19, SP, 0);
            code.MOV(X19, X0);
        }
        code.SUB(SP, SP, 32);
        code.STP(X20, X21, SP, 0);
        code.STP(X22, X23, SP, 16);

        code.LDURH(W20, X19, -4);
        code.LDUR(X21, X19, -16);

        code.MOV(X22, trace_scope_begin);
        code.CMP(X21, X22);
        code.B(LT, normal_code);
        code.MOV(X22, trace_scope_end);
        code.CMP(X21, X22);
        code.B(GT, normal_code);

        code.MOV(W21, W20);
        code.MOV(W22, 0xfc000000);
        code.AND(W22, W22, W21);
        code.MOV(W23, 0x94000000);  // BL
        code.CMP(W22, W23);
        code.B(EQ, halt_reason_set);

        code.MOV(W21, W20);
        code.MOV(W22, 0xfffffc1f);
        code.AND(W22, W22, W21);
        code.MOV(W23, 0xd63f0000);  // BLR
        code.CMP(W22, W23);
        code.B(EQ, halt_reason_set);

        code.MOV(W21, W20);
        code.MOV(W22, 0xfffff800);
        code.AND(W22, W22, W21);
        code.MOV(W23, 0xd63f0800);  // BLRxxx
        code.CMP(W22, W23);
        code.B(EQ, halt_reason_set);

        code.MOV(W21, W20);
        code.MOV(W22, 0xff000010);
        code.AND(W22, W22, W21);
        code.MOV(W23, 0x54000000);  // B.cond
        code.CMP(W22, W23);
        code.B(EQ, halt_reason_set);

        code.MOV(W21, W20);
        code.MOV(W22, 0xff000010);
        code.AND(W22, W22, W21);
        code.MOV(W23, 0x54000010);  // BC.cond
        code.CMP(W22, W23);
        code.B(EQ, halt_reason_set);

        code.MOV(W21, W20);
        code.MOV(W22, 0x7f000000);
        code.AND(W22, W22, W21);
        code.MOV(W23, 0x35000000);  // CBNZ
        code.CMP(W22, W23);
        code.B(EQ, halt_reason_set);

        code.MOV(W21, W20);
        code.MOV(W22, 0x7f000000);
        code.AND(W22, W22, W21);
        code.MOV(W23, 0x34000000);  // CBZ
        code.CMP(W22, W23);
        code.B(EQ, halt_reason_set);

        code.MOV(W21, W20);
        code.MOV(W22, 0x7f000000);
        code.AND(W22, W22, W21);
        code.MOV(W23, 0x37000000);  // TBNZ
        code.CMP(W22, W23);
        code.B(EQ, halt_reason_set);

        code.MOV(W21, W20);
        code.MOV(W22, 0x7f000000);
        code.AND(W22, W22, W21);
        code.MOV(W23, 0x36000000);  // TBZ
        code.CMP(W22, W23);
        code.B(EQ, halt_reason_set);

        code.MOV(W21, W20);
        code.MOV(W22, 0xfc000000);
        code.AND(W22, W22, W21);
        code.MOV(W23, 0x14000000);  // B
        code.CMP(W22, W23);
        code.B(EQ, halt_reason_set);

        code.MOV(W21, W20);
        code.MOV(W22, 0xfffffc1f);
        code.AND(W22, W22, W21);
        code.MOV(W23, 0xd61f0000);  // BR
        code.CMP(W22, W23);
        code.B(EQ, halt_reason_set);

        code.MOV(W21, W20);
        code.MOV(W22, 0xfffff800);
        code.AND(W22, W22, W21);
        code.MOV(W23, 0xd61f0800);  // BRxxx
        code.CMP(W22, W23);
        code.B(EQ, halt_reason_set);

        code.MOV(W21, W20);
        code.MOV(W22, 0xfffffc1f);
        code.AND(W22, W22, W21);
        code.MOV(W23, 0xd65f0000);  // RET
        code.CMP(W22, W23);
        code.B(EQ, halt_reason_set);

        code.MOV(W21, W20);
        code.MOV(W22, 0xfffffbff);
        code.AND(W22, W22, W21);
        code.MOV(W23, 0xd65f0bff);  // RETAA, RETAB
        code.CMP(W22, W23);
        code.B(EQ, halt_reason_set);

        code.MOV(W21, W20);
        code.MOV(W22, 0xffc0001f);
        code.AND(W22, W22, W21);
        code.MOV(W23, 0x5500001f);  // RETAASPPC, RETABSPPC
        code.CMP(W22, W23);
        code.B(EQ, halt_reason_set);

        code.MOV(W21, W20);
        code.MOV(W22, 0xfffffbe0);
        code.AND(W22, W22, W21);
        code.MOV(W23, 0xd65f0be0);  // RETAASPPC, RETABSPPC
        code.CMP(W22, W23);
        code.B(EQ, halt_reason_set);

        code.l(normal_code);
        code.LDP(X20, X21, SP, 0);
        code.LDP(X22, X23, SP, 16);
        code.ADD(SP, SP, 32);
        if (isRet) {
            code.LDP(X0, X19, SP, 0);
            code.ADD(SP, SP, 16);
        }
        code.ADRP(Xscratch0, run_code_entry);
        code.ADR(Xscratch1, run_code_entry);
        code.AND(Xscratch1, Xscratch1, 0xfff);
        code.ADD(Xscratch0, Xscratch0, Xscratch1);
        code.BR(Xscratch0);

        code.l(halt_reason_set);
        code.LDP(X20, X21, SP, 0);
        code.LDP(X22, X23, SP, 16);
        code.ADD(SP, SP, 32);

        code.l(halt_hr_loop);
        code.LDAXR(Wscratch0, Xhalt);
        code.ORR(Wscratch0, Wscratch0, halt_reason_on_run);
        code.STLXR(Wscratch1, Wscratch0, Xhalt);
        code.CBNZ(Wscratch1, halt_hr_loop);

        if (isRet) {
            code.LDP(X0, X19, SP, 0);
            code.ADD(SP, SP, 16);
            code.ADRP(Xscratch0, run_code_entry);
            code.ADR(Xscratch1, run_code_entry);
            code.AND(Xscratch1, Xscratch1, 0xfff);
            code.ADD(Xscratch0, Xscratch0, Xscratch1);
            code.BR(Xscratch0);
        }
    }
}

void A64AddressSpace::EmitPrelude() {
    using namespace oaknut::util;

    UnprotectCodeMemory();

    prelude_info.read_memory_8 = EmitCallTrampoline<&A64::UserCallbacks::MemoryRead8>(code, conf.callbacks);
    prelude_info.read_memory_16 = EmitCallTrampoline<&A64::UserCallbacks::MemoryRead16>(code, conf.callbacks);
    prelude_info.read_memory_32 = EmitCallTrampoline<&A64::UserCallbacks::MemoryRead32>(code, conf.callbacks);
    prelude_info.read_memory_64 = EmitCallTrampoline<&A64::UserCallbacks::MemoryRead64>(code, conf.callbacks);
    prelude_info.read_memory_128 = EmitRead128CallTrampoline(code, conf.callbacks);
    prelude_info.wrapped_read_memory_8 = EmitWrappedReadCallTrampoline<&A64::UserCallbacks::MemoryRead8>(code, conf.callbacks);
    prelude_info.wrapped_read_memory_16 = EmitWrappedReadCallTrampoline<&A64::UserCallbacks::MemoryRead16>(code, conf.callbacks);
    prelude_info.wrapped_read_memory_32 = EmitWrappedReadCallTrampoline<&A64::UserCallbacks::MemoryRead32>(code, conf.callbacks);
    prelude_info.wrapped_read_memory_64 = EmitWrappedReadCallTrampoline<&A64::UserCallbacks::MemoryRead64>(code, conf.callbacks);
    prelude_info.wrapped_read_memory_128 = EmitWrappedRead128CallTrampoline(code, conf.callbacks);
    prelude_info.exclusive_read_memory_8 = EmitExclusiveReadCallTrampoline<&A64::UserCallbacks::MemoryRead8, u8>(code, conf);
    prelude_info.exclusive_read_memory_16 = EmitExclusiveReadCallTrampoline<&A64::UserCallbacks::MemoryRead16, u16>(code, conf);
    prelude_info.exclusive_read_memory_32 = EmitExclusiveReadCallTrampoline<&A64::UserCallbacks::MemoryRead32, u32>(code, conf);
    prelude_info.exclusive_read_memory_64 = EmitExclusiveReadCallTrampoline<&A64::UserCallbacks::MemoryRead64, u64>(code, conf);
    prelude_info.exclusive_read_memory_128 = EmitExclusiveRead128CallTrampoline(code, conf);
    prelude_info.write_memory_8 = EmitCallTrampoline<&A64::UserCallbacks::MemoryWrite8>(code, conf.callbacks);
    prelude_info.write_memory_16 = EmitCallTrampoline<&A64::UserCallbacks::MemoryWrite16>(code, conf.callbacks);
    prelude_info.write_memory_32 = EmitCallTrampoline<&A64::UserCallbacks::MemoryWrite32>(code, conf.callbacks);
    prelude_info.write_memory_64 = EmitCallTrampoline<&A64::UserCallbacks::MemoryWrite64>(code, conf.callbacks);
    prelude_info.write_memory_128 = EmitWrite128CallTrampoline(code, conf.callbacks);
    prelude_info.wrapped_write_memory_8 = EmitWrappedWriteCallTrampoline<&A64::UserCallbacks::MemoryWrite8>(code, conf.callbacks);
    prelude_info.wrapped_write_memory_16 = EmitWrappedWriteCallTrampoline<&A64::UserCallbacks::MemoryWrite16>(code, conf.callbacks);
    prelude_info.wrapped_write_memory_32 = EmitWrappedWriteCallTrampoline<&A64::UserCallbacks::MemoryWrite32>(code, conf.callbacks);
    prelude_info.wrapped_write_memory_64 = EmitWrappedWriteCallTrampoline<&A64::UserCallbacks::MemoryWrite64>(code, conf.callbacks);
    prelude_info.wrapped_write_memory_128 = EmitWrappedWrite128CallTrampoline(code, conf.callbacks);
    prelude_info.exclusive_write_memory_8 = EmitExclusiveWriteCallTrampoline<&A64::UserCallbacks::MemoryWriteExclusive8, u8>(code, conf);
    prelude_info.exclusive_write_memory_16 = EmitExclusiveWriteCallTrampoline<&A64::UserCallbacks::MemoryWriteExclusive16, u16>(code, conf);
    prelude_info.exclusive_write_memory_32 = EmitExclusiveWriteCallTrampoline<&A64::UserCallbacks::MemoryWriteExclusive32, u32>(code, conf);
    prelude_info.exclusive_write_memory_64 = EmitExclusiveWriteCallTrampoline<&A64::UserCallbacks::MemoryWriteExclusive64, u64>(code, conf);
    prelude_info.exclusive_write_memory_128 = EmitExclusiveWrite128CallTrampoline(code, conf);
    prelude_info.call_svc = EmitCallTrampoline<&A64::UserCallbacks::CallSVC>(code, conf.callbacks);
    prelude_info.exception_raised = EmitCallTrampoline<&A64::UserCallbacks::ExceptionRaised>(code, conf.callbacks);
    prelude_info.isb_raised = EmitCallTrampoline<&A64::UserCallbacks::InstructionSynchronizationBarrierRaised>(code, conf.callbacks);
    prelude_info.ic_raised = EmitCallTrampoline<&A64::UserCallbacks::InstructionCacheOperationRaised>(code, conf.callbacks);
    prelude_info.dc_raised = EmitCallTrampoline<&A64::UserCallbacks::DataCacheOperationRaised>(code, conf.callbacks);
    prelude_info.get_cntpct = EmitCallTrampoline<&A64::UserCallbacks::GetCNTPCT>(code, conf.callbacks);
    prelude_info.add_ticks = EmitCallTrampoline<&A64::UserCallbacks::AddTicks>(code, conf.callbacks);
    prelude_info.get_ticks_remaining = EmitCallTrampoline<&A64::UserCallbacks::GetTicksRemaining>(code, conf.callbacks);

    oaknut::Label halt_reason_set, run_code_entry, return_from_run_code, l_return_to_dispatcher;

    prelude_info.run_code = code.xptr<PreludeInfo::RunCodeFuncType>();
    {
        ABI_PushRegisters(code, ABI_CALLEE_SAVE | (1 << 30), sizeof(StackLayout));

        code.MOV(X19, X0);
        code.MOV(Xstate, X1);
        code.MOV(Xhalt, X2);
        if (conf.page_table) {
            code.MOV(Xpagetable, mcl::bit_cast<u64>(conf.page_table));
        }
        if (conf.fastmem_pointer) {
            code.MOV(Xfastmem, *conf.fastmem_pointer);
        }

        if (conf.HasOptimization(OptimizationFlag::ReturnStackBuffer)) {
            code.LDR(Xscratch0, l_return_to_dispatcher);
            for (size_t i = 0; i < RSBCount; i++) {
                code.STR(Xscratch0, SP, offsetof(StackLayout, rsb) + offsetof(RSBEntry, code_ptr) + i * sizeof(RSBEntry));
            }
        }

        if (conf.enable_cycle_counting) {
            code.BL(prelude_info.get_ticks_remaining);
            code.MOV(Xticks, X0);
            code.STR(Xticks, SP, offsetof(StackLayout, cycles_to_run));
        }

        code.MRS(Xscratch1, oaknut::SystemReg::FPCR);
        code.STR(Wscratch1, SP, offsetof(StackLayout, save_host_fpcr));
        code.LDR(Wscratch0, Xstate, offsetof(A64JitState, fpcr));
        code.MSR(oaknut::SystemReg::FPCR, Xscratch0);

        code.LDAR(Wscratch0, Xhalt);
        code.CBNZ(Wscratch0, return_from_run_code);

        GenHaltReasonSet(run_code_entry);

        code.l(run_code_entry);
        code.BR(X19);
    }

    prelude_info.step_code = code.xptr<PreludeInfo::RunCodeFuncType>();
    {
        ABI_PushRegisters(code, ABI_CALLEE_SAVE | (1 << 30), sizeof(StackLayout));

        code.MOV(X19, X0);
        code.MOV(Xstate, X1);
        code.MOV(Xhalt, X2);
        if (conf.page_table) {
            code.MOV(Xpagetable, mcl::bit_cast<u64>(conf.page_table));
        }
        if (conf.fastmem_pointer) {
            code.MOV(Xfastmem, *conf.fastmem_pointer);
        }

        if (conf.HasOptimization(OptimizationFlag::ReturnStackBuffer)) {
            code.LDR(Xscratch0, l_return_to_dispatcher);
            for (size_t i = 0; i < RSBCount; i++) {
                code.STR(Xscratch0, SP, offsetof(StackLayout, rsb) + offsetof(RSBEntry, code_ptr) + i * sizeof(RSBEntry));
            }
        }

        if (conf.enable_cycle_counting) {
            code.MOV(Xticks, 1);
            code.STR(Xticks, SP, offsetof(StackLayout, cycles_to_run));
        }

        code.MRS(Xscratch1, oaknut::SystemReg::FPCR);
        code.STR(Wscratch1, SP, offsetof(StackLayout, save_host_fpcr));
        code.LDR(Wscratch0, Xstate, offsetof(A64JitState, fpcr));
        code.MSR(oaknut::SystemReg::FPCR, Xscratch0);

        oaknut::Label step_hr_loop;
        code.l(step_hr_loop);
        code.LDAXR(Wscratch0, Xhalt);
        code.CBNZ(Wscratch0, return_from_run_code);
        code.ORR(Wscratch0, Wscratch0, static_cast<u32>(HaltReason::Step));
        code.STLXR(Wscratch1, Wscratch0, Xhalt);
        code.CBNZ(Wscratch1, step_hr_loop);

        code.BR(X19);
    }

    prelude_info.return_to_dispatcher = code.xptr<void*>();
    {
        oaknut::Label l_this, l_addr;

        code.LDAR(Wscratch0, Xhalt);
        code.CBNZ(Wscratch0, return_from_run_code);

        if (conf.enable_cycle_counting) {
            code.CMP(Xticks, 0);
            code.B(LE, return_from_run_code);
        }

        code.LDR(X0, l_this);
        code.MOV(X1, Xstate);
        code.LDR(Xscratch0, l_addr);
        code.BLR(Xscratch0);

        oaknut::Label next_code_entry;
        GenHaltReasonSet(next_code_entry, return_from_run_code);
        code.l(next_code_entry);

        code.BR(X0);

        const auto fn = [](A64AddressSpace& self, A64JitState& context) -> CodePtr {
            return self.GetOrEmit(context);
        };

        code.align(8);
        code.l(l_this);
        code.dx(mcl::bit_cast<u64>(this));
        code.l(l_addr);
        code.dx(mcl::bit_cast<u64>(Common::FptrCast(fn)));
    }

    prelude_info.return_from_run_code = code.xptr<void*>();
    {
        code.l(return_from_run_code);

        if (conf.enable_cycle_counting) {
            code.LDR(X1, SP, offsetof(StackLayout, cycles_to_run));
            code.SUB(X1, X1, Xticks);
            code.BL(prelude_info.add_ticks);
        }

        code.LDR(Wscratch0, SP, offsetof(StackLayout, save_host_fpcr));
        code.MSR(oaknut::SystemReg::FPCR, Xscratch0);

        oaknut::Label exit_hr_loop;
        code.l(exit_hr_loop);
        code.LDAXR(W0, Xhalt);
        code.STLXR(Wscratch0, WZR, Xhalt);
        code.CBNZ(Wscratch0, exit_hr_loop);

        ABI_PopRegisters(code, ABI_CALLEE_SAVE | (1 << 30), sizeof(StackLayout));
        code.RET();
    }

    code.align(8);
    code.l(l_return_to_dispatcher);
    code.dx(mcl::bit_cast<u64>(prelude_info.return_to_dispatcher));

    prelude_info.end_of_prelude = code.offset();

    mem.invalidate_all();
    ProtectCodeMemory();
}

EmitConfig A64AddressSpace::GetEmitConfig() {
    return EmitConfig{
        .optimizations = conf.unsafe_optimizations ? conf.optimizations : conf.optimizations & all_safe_optimizations,

        .hook_isb = conf.hook_isb,

        .cntfreq_el0 = conf.cntfrq_el0,
        .ctr_el0 = conf.ctr_el0,
        .dczid_el0 = conf.dczid_el0,
        .tpidrro_el0 = conf.tpidrro_el0,
        .tpidr_el0 = conf.tpidr_el0,

        .check_halt_on_memory_access = conf.check_halt_on_memory_access,

        .page_table_pointer = mcl::bit_cast<u64>(conf.page_table),
        .page_table_address_space_bits = conf.page_table_address_space_bits,
        .page_table_pointer_mask_bits = conf.page_table_pointer_mask_bits,
        .silently_mirror_page_table = conf.silently_mirror_page_table,
        .absolute_offset_page_table = conf.absolute_offset_page_table,
        .detect_misaligned_access_via_page_table = conf.detect_misaligned_access_via_page_table,
        .only_detect_misalignment_via_page_table_on_page_boundary = conf.only_detect_misalignment_via_page_table_on_page_boundary,

        .fastmem_pointer = conf.fastmem_pointer,
        .recompile_on_fastmem_failure = conf.recompile_on_fastmem_failure,
        .fastmem_address_space_bits = conf.fastmem_address_space_bits,
        .silently_mirror_fastmem = conf.silently_mirror_fastmem,

        .wall_clock_cntpct = conf.wall_clock_cntpct,
        .enable_cycle_counting = conf.enable_cycle_counting,

        .always_little_endian = true,

        .descriptor_to_fpcr = [](const IR::LocationDescriptor& location) { return A64::LocationDescriptor{location}.FPCR(); },
        .emit_cond = EmitA64Cond,
        .emit_condition_failed_terminal = EmitA64ConditionFailedTerminal,
        .emit_terminal = EmitA64Terminal,
        .emit_check_memory_abort = EmitA64CheckMemoryAbort,

        .state_nzcv_offset = offsetof(A64JitState, cpsr_nzcv),
        .state_fpsr_offset = offsetof(A64JitState, fpsr),
        .state_exclusive_state_offset = offsetof(A64JitState, exclusive_state),

        .coprocessors{},

        .very_verbose_debugging_output = conf.very_verbose_debugging_output,
    };
}

void A64AddressSpace::RegisterNewBasicBlock(const IR::Block& block, const EmittedBlockInfo&) {
    const A64::LocationDescriptor descriptor{block.Location()};
    const A64::LocationDescriptor end_location{block.EndLocation()};
    const auto range = boost::icl::discrete_interval<u64>::closed(descriptor.PC(), end_location.PC() - 1);
    block_ranges.AddRange(range, descriptor);
}

}  // namespace Dynarmic::Backend::Arm64
