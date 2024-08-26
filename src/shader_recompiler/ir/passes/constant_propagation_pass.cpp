// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <bit>
#include <optional>
#include <type_traits>
#include "common/func_traits.h"
#include "shader_recompiler/ir/basic_block.h"

namespace Shader::Optimization {

template <typename T>
[[nodiscard]] T Arg(const IR::Value& value) {
    if constexpr (std::is_same_v<T, bool>) {
        return value.U1();
    } else if constexpr (std::is_same_v<T, u32>) {
        return value.U32();
    } else if constexpr (std::is_same_v<T, s32>) {
        return static_cast<s32>(value.U32());
    } else if constexpr (std::is_same_v<T, f32>) {
        return value.F32();
    } else if constexpr (std::is_same_v<T, u64>) {
        return value.U64();
    } else if constexpr (std::is_same_v<T, s64>) {
        return static_cast<s64>(value.U64());
    }
}

template <typename Func, size_t... I>
IR::Value EvalImmediates(const IR::Inst& inst, Func&& func, std::index_sequence<I...>) {
    using Traits = Common::LambdaTraits<decltype(func)>;
    return IR::Value{func(Arg<typename Traits::template ArgType<I>>(inst.Arg(I))...)};
}

template <typename T, typename ImmFn>
bool FoldCommutative(IR::Inst& inst, ImmFn&& imm_fn) {
    const IR::Value lhs{inst.Arg(0)};
    const IR::Value rhs{inst.Arg(1)};

    const bool is_lhs_immediate{lhs.IsImmediate()};
    const bool is_rhs_immediate{rhs.IsImmediate()};

    if (is_lhs_immediate && is_rhs_immediate) {
        const auto result{imm_fn(Arg<T>(lhs), Arg<T>(rhs))};
        inst.ReplaceUsesWith(IR::Value{result});
        return false;
    }
    if (is_lhs_immediate && !is_rhs_immediate) {
        IR::Inst* const rhs_inst{rhs.InstRecursive()};
        if (rhs_inst->GetOpcode() == inst.GetOpcode() && rhs_inst->Arg(1).IsImmediate()) {
            const auto combined{imm_fn(Arg<T>(lhs), Arg<T>(rhs_inst->Arg(1)))};
            inst.SetArg(0, rhs_inst->Arg(0));
            inst.SetArg(1, IR::Value{combined});
        } else {
            // Normalize
            inst.SetArg(0, rhs);
            inst.SetArg(1, lhs);
        }
    }
    if (!is_lhs_immediate && is_rhs_immediate) {
        const IR::Inst* const lhs_inst{lhs.InstRecursive()};
        if (lhs_inst->GetOpcode() == inst.GetOpcode() && lhs_inst->Arg(1).IsImmediate()) {
            const auto combined{imm_fn(Arg<T>(rhs), Arg<T>(lhs_inst->Arg(1)))};
            inst.SetArg(0, lhs_inst->Arg(0));
            inst.SetArg(1, IR::Value{combined});
        }
    }
    return true;
}

template <typename Func>
bool FoldWhenAllImmediates(IR::Inst& inst, Func&& func) {
    if (!inst.AreAllArgsImmediates() /*|| inst.HasAssociatedPseudoOperation()*/) {
        return false;
    }
    using Indices = std::make_index_sequence<Common::LambdaTraits<decltype(func)>::NUM_ARGS>;
    inst.ReplaceUsesWith(EvalImmediates(inst, func, Indices{}));
    return true;
}

template <IR::Opcode op, typename Dest, typename Source>
void FoldBitCast(IR::Inst& inst, IR::Opcode reverse) {
    const IR::Value value{inst.Arg(0)};
    if (value.IsImmediate()) {
        inst.ReplaceUsesWith(IR::Value{std::bit_cast<Dest>(Arg<Source>(value))});
        return;
    }
    IR::Inst* const arg_inst{value.InstRecursive()};
    if (arg_inst->GetOpcode() == reverse) {
        inst.ReplaceUsesWith(arg_inst->Arg(0));
        return;
    }
}

std::optional<IR::Value> FoldCompositeExtractImpl(IR::Value inst_value, IR::Opcode insert,
                                                  IR::Opcode construct, u32 first_index) {
    IR::Inst* const inst{inst_value.InstRecursive()};
    if (inst->GetOpcode() == construct) {
        return inst->Arg(first_index);
    }
    if (inst->GetOpcode() != insert) {
        return std::nullopt;
    }
    IR::Value value_index{inst->Arg(2)};
    if (!value_index.IsImmediate()) {
        return std::nullopt;
    }
    const u32 second_index{value_index.U32()};
    if (first_index != second_index) {
        IR::Value value_composite{inst->Arg(0)};
        if (value_composite.IsImmediate()) {
            return std::nullopt;
        }
        return FoldCompositeExtractImpl(value_composite, insert, construct, first_index);
    }
    return inst->Arg(1);
}

void FoldCompositeExtract(IR::Inst& inst, IR::Opcode construct, IR::Opcode insert) {
    const IR::Value value_1{inst.Arg(0)};
    const IR::Value value_2{inst.Arg(1)};
    if (value_1.IsImmediate()) {
        return;
    }
    if (!value_2.IsImmediate()) {
        return;
    }
    const u32 first_index{value_2.U32()};
    const std::optional result{FoldCompositeExtractImpl(value_1, insert, construct, first_index)};
    if (!result) {
        return;
    }
    inst.ReplaceUsesWith(*result);
}

void FoldConvert(IR::Inst& inst, IR::Opcode opposite) {
    const IR::Value value{inst.Arg(0)};
    if (value.IsImmediate()) {
        return;
    }
    IR::Inst* const producer{value.InstRecursive()};
    if (producer->GetOpcode() == opposite) {
        inst.ReplaceUsesWith(producer->Arg(0));
    }
}

void FoldLogicalAnd(IR::Inst& inst) {
    if (!FoldCommutative<bool>(inst, [](bool a, bool b) { return a && b; })) {
        return;
    }
    const IR::Value rhs{inst.Arg(1)};
    if (rhs.IsImmediate()) {
        if (rhs.U1()) {
            inst.ReplaceUsesWith(inst.Arg(0));
        } else {
            inst.ReplaceUsesWith(IR::Value{false});
        }
    }
}

void FoldSelect(IR::Inst& inst) {
    const IR::Value cond{inst.Arg(0)};
    if (cond.IsImmediate()) {
        inst.ReplaceUsesWith(cond.U1() ? inst.Arg(1) : inst.Arg(2));
    }
}

void FoldLogicalOr(IR::Inst& inst) {
    if (!FoldCommutative<bool>(inst, [](bool a, bool b) { return a || b; })) {
        return;
    }
    const IR::Value rhs{inst.Arg(1)};
    if (rhs.IsImmediate()) {
        if (rhs.U1()) {
            inst.ReplaceUsesWith(IR::Value{true});
        } else {
            inst.ReplaceUsesWith(inst.Arg(0));
        }
    }
}

void FoldLogicalNot(IR::Inst& inst) {
    const IR::U1 value{inst.Arg(0)};
    if (value.IsImmediate()) {
        inst.ReplaceUsesWith(IR::Value{!value.U1()});
        return;
    }
    IR::Inst* const arg{value.InstRecursive()};
    if (arg->GetOpcode() == IR::Opcode::LogicalNot) {
        inst.ReplaceUsesWith(arg->Arg(0));
    }
}

void FoldInverseFunc(IR::Inst& inst, IR::Opcode reverse) {
    const IR::Value value{inst.Arg(0)};
    if (value.IsImmediate()) {
        return;
    }
    IR::Inst* const arg_inst{value.InstRecursive()};
    if (arg_inst->GetOpcode() == reverse) {
        inst.ReplaceUsesWith(arg_inst->Arg(0));
        return;
    }
}

template <typename T>
void FoldAdd(IR::Block& block, IR::Inst& inst) {
    if (!FoldCommutative<T>(inst, [](T a, T b) { return a + b; })) {
        return;
    }
    const IR::Value rhs{inst.Arg(1)};
    if (rhs.IsImmediate() && Arg<T>(rhs) == 0) {
        inst.ReplaceUsesWith(inst.Arg(0));
        return;
    }
}

template <u32 idx>
bool IsArgImm(const IR::Inst& inst, u32 imm) {
    const IR::Value& arg = inst.Arg(idx);
    return arg.IsImmediate() && arg.U32() == imm;
};

void FoldBooleanConvert(IR::Inst& inst) {
    // Eliminate pattern
    // %4 = <some bool>
    // %5 = SelectU32 %4, #1, #0 (uses: 2)
    // %8 = INotEqual %5, #0 (uses: 1)
    if (!IsArgImm<1>(inst, 0)) {
        return;
    }
    IR::Inst* prod = inst.Arg(0).TryInstRecursive();
    if (!prod || prod->GetOpcode() != IR::Opcode::SelectU32) {
        return;
    }
    if (IsArgImm<1>(*prod, 1) && IsArgImm<2>(*prod, 0)) {
        inst.ReplaceUsesWith(prod->Arg(0));
    }
}

void FoldCmpClass(IR::Inst& inst) {
    ASSERT_MSG(inst.Arg(1).IsImmediate(), "Unable to resolve compare operation");
    const auto class_mask = static_cast<IR::FloatClassFunc>(inst.Arg(1).U32());
    if ((class_mask & IR::FloatClassFunc::NaN) == IR::FloatClassFunc::NaN) {
        inst.ReplaceOpcode(IR::Opcode::FPIsNan32);
    } else if ((class_mask & IR::FloatClassFunc::Infinity) == IR::FloatClassFunc::Infinity) {
        inst.ReplaceOpcode(IR::Opcode::FPIsInf32);
    } else {
        UNREACHABLE();
    }
}

void FoldReadLane(IR::Inst& inst) {
    const u32 lane = inst.Arg(1).U32();
    IR::Inst* prod = inst.Arg(0).InstRecursive();
    while (prod->GetOpcode() == IR::Opcode::WriteLane) {
        if (prod->Arg(2).U32() == lane) {
            inst.ReplaceUsesWith(prod->Arg(1));
            return;
        }
        prod = prod->Arg(0).InstRecursive();
    }
}

void ConstantPropagation(IR::Block& block, IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::IAdd32:
        return FoldAdd<u32>(block, inst);
    case IR::Opcode::ISub32:
        FoldWhenAllImmediates(inst, [](u32 a, u32 b) { return a - b; });
        return;
    case IR::Opcode::ConvertF32U32:
        FoldWhenAllImmediates(inst, [](u32 a) { return static_cast<float>(a); });
        return;
    case IR::Opcode::IMul32:
        FoldWhenAllImmediates(inst, [](u32 a, u32 b) { return a * b; });
        return;
    case IR::Opcode::FPCmpClass32:
        FoldCmpClass(inst);
        return;
    case IR::Opcode::ShiftRightArithmetic32:
        FoldWhenAllImmediates(inst, [](s32 a, s32 b) { return static_cast<u32>(a >> b); });
        return;
    case IR::Opcode::BitCastF32U32:
        return FoldBitCast<IR::Opcode::BitCastF32U32, f32, u32>(inst, IR::Opcode::BitCastU32F32);
    case IR::Opcode::BitCastU32F32:
        return FoldBitCast<IR::Opcode::BitCastU32F32, u32, f32>(inst, IR::Opcode::BitCastF32U32);
    case IR::Opcode::PackHalf2x16:
        return FoldInverseFunc(inst, IR::Opcode::UnpackHalf2x16);
    case IR::Opcode::UnpackHalf2x16:
        return FoldInverseFunc(inst, IR::Opcode::PackHalf2x16);
    case IR::Opcode::PackFloat2x16:
        return FoldInverseFunc(inst, IR::Opcode::UnpackFloat2x16);
    case IR::Opcode::UnpackFloat2x16:
        return FoldInverseFunc(inst, IR::Opcode::PackFloat2x16);
    case IR::Opcode::SelectU1:
    case IR::Opcode::SelectU8:
    case IR::Opcode::SelectU16:
    case IR::Opcode::SelectU32:
    case IR::Opcode::SelectU64:
    case IR::Opcode::SelectF32:
    case IR::Opcode::SelectF64:
        return FoldSelect(inst);
    case IR::Opcode::ReadLane:
        return FoldReadLane(inst);
    case IR::Opcode::FPNeg32:
        FoldWhenAllImmediates(inst, [](f32 a) { return -a; });
        return;
    case IR::Opcode::LogicalAnd:
        return FoldLogicalAnd(inst);
    case IR::Opcode::LogicalOr:
        return FoldLogicalOr(inst);
    case IR::Opcode::LogicalNot:
        return FoldLogicalNot(inst);
    case IR::Opcode::SLessThan32:
        FoldWhenAllImmediates(inst, [](s32 a, s32 b) { return a < b; });
        return;
    case IR::Opcode::SLessThan64:
        FoldWhenAllImmediates(inst, [](s64 a, s64 b) { return a < b; });
        return;
    case IR::Opcode::ULessThan32:
        FoldWhenAllImmediates(inst, [](u32 a, u32 b) { return a < b; });
        return;
    case IR::Opcode::ULessThan64:
        FoldWhenAllImmediates(inst, [](u64 a, u64 b) { return a < b; });
        return;
    case IR::Opcode::SLessThanEqual:
        FoldWhenAllImmediates(inst, [](s32 a, s32 b) { return a <= b; });
        return;
    case IR::Opcode::ULessThanEqual:
        FoldWhenAllImmediates(inst, [](u32 a, u32 b) { return a <= b; });
        return;
    case IR::Opcode::SGreaterThan:
        FoldWhenAllImmediates(inst, [](s32 a, s32 b) { return a > b; });
        return;
    case IR::Opcode::UGreaterThan:
        FoldWhenAllImmediates(inst, [](u32 a, u32 b) { return a > b; });
        return;
    case IR::Opcode::SGreaterThanEqual:
        FoldWhenAllImmediates(inst, [](s32 a, s32 b) { return a >= b; });
        return;
    case IR::Opcode::UGreaterThanEqual:
        FoldWhenAllImmediates(inst, [](u32 a, u32 b) { return a >= b; });
        return;
    case IR::Opcode::IEqual:
        FoldWhenAllImmediates(inst, [](u32 a, u32 b) { return a == b; });
        return;
    case IR::Opcode::INotEqual:
        FoldWhenAllImmediates(inst, [](u32 a, u32 b) { return a != b; });
        FoldBooleanConvert(inst);
        return;
    case IR::Opcode::BitwiseAnd32:
        FoldWhenAllImmediates(inst, [](u32 a, u32 b) { return a & b; });
        return;
    case IR::Opcode::BitwiseAnd64:
	FoldWhenAllImmediates(inst, [](u64 a, u64 b) { return a & b; });
	return;
    case IR::Opcode::BitwiseOr32:
        FoldWhenAllImmediates(inst, [](u32 a, u32 b) { return a | b; });
        return;
    case IR::Opcode::BitwiseOr64:
	FoldWhenAllImmediates(inst, [](u64 a, u64 b) { return a | b; });
	return;
    case IR::Opcode::BitwiseXor32:
        FoldWhenAllImmediates(inst, [](u32 a, u32 b) { return a ^ b; });
        return;
    case IR::Opcode::BitFieldUExtract:
        FoldWhenAllImmediates(inst, [](u32 base, u32 shift, u32 count) {
            if (static_cast<size_t>(shift) + static_cast<size_t>(count) > 32) {
                UNREACHABLE_MSG("Undefined result in {}({}, {}, {})", IR::Opcode::BitFieldUExtract,
                                base, shift, count);
            }
            return (base >> shift) & ((1U << count) - 1);
        });
        return;
    case IR::Opcode::BitFieldSExtract:
        FoldWhenAllImmediates(inst, [](s32 base, u32 shift, u32 count) {
            const size_t back_shift{static_cast<size_t>(shift) + static_cast<size_t>(count)};
            const size_t left_shift{32 - back_shift};
            const size_t right_shift{static_cast<size_t>(32 - count)};
            if (back_shift > 32 || left_shift >= 32 || right_shift >= 32) {
                UNREACHABLE_MSG("Undefined result in {}({}, {}, {})", IR::Opcode::BitFieldSExtract,
                                base, shift, count);
            }
            return static_cast<u32>((base << left_shift) >> right_shift);
        });
        return;
    case IR::Opcode::BitFieldInsert:
        FoldWhenAllImmediates(inst, [](u32 base, u32 insert, u32 offset, u32 bits) {
            if (bits >= 32 || offset >= 32) {
                UNREACHABLE_MSG("Undefined result in {}({}, {}, {}, {})",
                                IR::Opcode::BitFieldInsert, base, insert, offset, bits);
            }
            return (base & ~(~(~0u << bits) << offset)) | (insert << offset);
        });
        return;
    case IR::Opcode::CompositeExtractU32x2:
        return FoldCompositeExtract(inst, IR::Opcode::CompositeConstructU32x2,
                                    IR::Opcode::CompositeInsertU32x2);
    case IR::Opcode::CompositeExtractU32x3:
        return FoldCompositeExtract(inst, IR::Opcode::CompositeConstructU32x3,
                                    IR::Opcode::CompositeInsertU32x3);
    case IR::Opcode::CompositeExtractU32x4:
        return FoldCompositeExtract(inst, IR::Opcode::CompositeConstructU32x4,
                                    IR::Opcode::CompositeInsertU32x4);
    case IR::Opcode::CompositeExtractF32x2:
        return FoldCompositeExtract(inst, IR::Opcode::CompositeConstructF32x2,
                                    IR::Opcode::CompositeInsertF32x2);
    case IR::Opcode::CompositeExtractF32x3:
        return FoldCompositeExtract(inst, IR::Opcode::CompositeConstructF32x3,
                                    IR::Opcode::CompositeInsertF32x3);
    case IR::Opcode::CompositeExtractF32x4:
        return FoldCompositeExtract(inst, IR::Opcode::CompositeConstructF32x4,
                                    IR::Opcode::CompositeInsertF32x4);
    case IR::Opcode::CompositeExtractF16x2:
        return FoldCompositeExtract(inst, IR::Opcode::CompositeConstructF16x2,
                                    IR::Opcode::CompositeInsertF16x2);
    case IR::Opcode::CompositeExtractF16x3:
        return FoldCompositeExtract(inst, IR::Opcode::CompositeConstructF16x3,
                                    IR::Opcode::CompositeInsertF16x3);
    case IR::Opcode::CompositeExtractF16x4:
        return FoldCompositeExtract(inst, IR::Opcode::CompositeConstructF16x4,
                                    IR::Opcode::CompositeInsertF16x4);
    case IR::Opcode::ConvertF32F16:
        return FoldConvert(inst, IR::Opcode::ConvertF16F32);
    case IR::Opcode::ConvertF16F32:
        return FoldConvert(inst, IR::Opcode::ConvertF32F16);
    default:
        break;
    }
}

void ConstantPropagationPass(IR::BlockList& program) {
    const auto end{program.rend()};
    for (auto it = program.rbegin(); it != end; ++it) {
        IR::Block* const block{*it};
        for (IR::Inst& inst : block->Instructions()) {
            ConstantPropagation(*block, inst);
        }
    }
}

} // namespace Shader::Optimization
