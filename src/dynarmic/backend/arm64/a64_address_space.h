/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include "dynarmic/backend/arm64/address_space.h"
#include "dynarmic/backend/block_range_information.h"
#include "dynarmic/interface/A64/config.h"

namespace Dynarmic::Backend::Arm64 {

struct EmittedBlockInfo;
struct A64JitState;
class A64AddressSpace final : public AddressSpace {
public:
    explicit A64AddressSpace(const A64::UserConfig& conf);

    IR::Block GenerateIR(IR::LocationDescriptor, u64& pc, u32& inst) const override;
    CodePtr GetOrEmit(A64JitState& context);
    CodePtr GetOrEmit(IR::LocationDescriptor descriptor) { return AddressSpace::GetOrEmit(descriptor); }

    void InvalidateCacheRanges(const boost::icl::interval_set<u64>& ranges);

    void Initialize(u32 _halt_reason_on_run, u64 _trace_scope_begin, u64 _trace_scope_end) {
        halt_reason_on_run = _halt_reason_on_run;
        trace_scope_begin = _trace_scope_begin;
        trace_scope_end = _trace_scope_end;
        EmitPrelude();
    }
protected:
    friend class A64Core;

    void EmitPrelude();
    EmitConfig GetEmitConfig() override;
    void RegisterNewBasicBlock(const IR::Block& block, const EmittedBlockInfo& block_info) override;

    void GenHaltReasonSet(oaknut::Label& run_code_entry);
    void GenHaltReasonSet(oaknut::Label& run_code_entry, oaknut::Label& ret_code_entry);
    void GenHaltReasonSetImpl(bool isRet, oaknut::Label& run_code_entry, oaknut::Label& ret_code_entry);

    const A64::UserConfig conf;
    BlockRangeInformation<u64> block_ranges;

    u32 halt_reason_on_run;
    u64 trace_scope_begin;
    u64 trace_scope_end;
};

}  // namespace Dynarmic::Backend::Arm64
