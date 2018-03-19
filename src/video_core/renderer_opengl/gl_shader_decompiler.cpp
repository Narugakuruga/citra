// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <map>
#include <set>
#include <string>
#include <boost/optional.hpp>
#include <nihstro/shader_bytecode.h>
#include <queue>
#include "common/assert.h"
#include "common/common_types.h"
#include "video_core/renderer_opengl/gl_shader_decompiler.h"

namespace Pica {
namespace Shader {
namespace Decompiler {

using nihstro::Instruction;
using nihstro::OpCode;
using nihstro::RegisterType;
using nihstro::SourceRegister;
using nihstro::SwizzlePattern;

std::string GetCommonDeclarations() {
    return R"(
struct pica_uniforms {
    bool b[16];
    uvec4 i[4];
    vec4 f[96];
};

bool exec_shader();

)";
}

constexpr u32 PROGRAM_END = MAX_PROGRAM_CODE_LENGTH;

class Impl {
private:
    bool FindEndInstrImpl(u32 begin, u32 end, std::map<u32, bool>& checked_offsets) {
        for (u32 offset = begin; offset < (begin > end ? PROGRAM_END : end); ++offset) {
            const Instruction instr = {program_code[offset]};

            auto [checked_offset_iter, inserted] = checked_offsets.emplace(offset, false);
            if (!inserted) {
                if (checked_offset_iter->second) {
                    return true;
                }
                continue;
            }

            switch (instr.opcode.Value()) {
            case OpCode::Id::END: {
                checked_offset_iter->second = true;
                return true;
            }
            case OpCode::Id::JMPC:
            case OpCode::Id::JMPU: {
                bool opt_end = FindEndInstrImpl(offset + 1, end, checked_offsets);
                bool opt_jmp =
                    FindEndInstrImpl(instr.flow_control.dest_offset, end, checked_offsets);
                if (opt_end && opt_jmp) {
                    checked_offset_iter->second = true;
                    return true;
                }
                return false;
            }
            case OpCode::Id::CALL: {
                bool opt = FindEndInstrImpl(instr.flow_control.dest_offset,
                                            instr.flow_control.dest_offset +
                                                instr.flow_control.num_instructions,
                                            checked_offsets);
                if (opt) {
                    checked_offset_iter->second = true;
                    return true;
                }
                break;
            }
            case OpCode::Id::IFU:
            case OpCode::Id::IFC: {
                if (instr.flow_control.num_instructions != 0) {
                    bool opt_if = FindEndInstrImpl(offset + 1, instr.flow_control.dest_offset,
                                                   checked_offsets);
                    bool opt_else = FindEndInstrImpl(instr.flow_control.dest_offset,
                                                     instr.flow_control.dest_offset +
                                                         instr.flow_control.num_instructions,
                                                     checked_offsets);
                    if (opt_if && opt_else) {
                        checked_offset_iter->second = true;
                        return true;
                    }
                }
                offset = instr.flow_control.dest_offset + instr.flow_control.num_instructions - 1;
                break;
            }
            };
        }
        return false;
    }

    /**
     * Finds if all code paths starting from a given point reach an "end" instruction.
     * @param begin the code starting point.
     * @end the farest point to search for "end" instructions.
     * @return the total number instructions covered. If there is a code path not reaching an "end"
     *     instruction, the return value is boost::none.
     */
    boost::optional<size_t> FindEndInstr(u32 begin, u32 end) {
        // first: offset
        // bool: found END
        std::map<u32, bool> checked_offsets;
        if (FindEndInstrImpl(begin, end, checked_offsets)) {
            return checked_offsets.size();
        } else {
            return boost::none;
        }
    }

    struct Subroutine {
        Subroutine(u32 begin_, u32 end_) : begin(begin_), end(end_) {}

        /// Check if the specified Subroutine calls this Subroutine (directly or indirectly).
        bool IsCalledBy(const Subroutine* caller, std::set<const Subroutine*> stack = {}) const {
            for (auto& pair : callers) {
                if (!stack.emplace(pair.second).second) {
                    continue;
                }
                if (pair.second == caller || pair.second->IsCalledBy(caller, stack)) {
                    return true;
                }
            }
            return false;
        }

        bool HasRecursion() const {
            for (auto& callee : calls) {
                if (IsCalledBy(callee.second) || callee.second->HasRecursion()) {
                    return true;
                }
            }
            return false;
        }

        /// Check if this Subroutine is suitable for inlining.
        bool IsInline() const {
            if (callers.size() > 1 &&
                (end_instr_distance ? static_cast<u32>(*end_instr_distance) : end - begin) > 10) {
                return false;
            }
            return jumps.empty();
        };

        std::string GetName() const {
            return "sub_" + std::to_string(begin) + "_" + std::to_string(end);
        }

        u32 begin;
        u32 end;

        std::set<std::pair<u32, u32>> discovered;
        boost::optional<size_t> end_instr_distance;

        using SubroutineMap = std::map<std::pair<u32 /*begin*/, u32 /*end*/>, const Subroutine*>;
        SubroutineMap branches;
        SubroutineMap calls;
        std::map<u32 /*from*/, const Subroutine*> callers;
        std::map<u32 /*from*/, u32 /*to*/> jumps;
    };

    std::map<std::pair<u32, u32>, Subroutine> subroutines;

    /**
     * Gets the Subroutine object corresponding to the specified address. If it is not in the
     * subroutine list yet, adds it to the list
     */
    Subroutine& GetRoutine(u32 begin, u32 end) {
        auto [iter, inserted] =
            subroutines.emplace(std::make_pair(std::make_pair(begin, end), Subroutine{begin, end}));
        auto& sub = iter->second;
        if (inserted) {
            sub.end_instr_distance = FindEndInstr(sub.begin, sub.end);
        }
        return sub;
    }

    Subroutine& AnalyzeControlFlow() {
        auto& program_main = GetRoutine(main_offset, PROGRAM_END);

        std::queue<std::tuple<u32, u32, Subroutine*>> discover_queue;
        discover_queue.emplace(main_offset, PROGRAM_END, &program_main);

        while (!discover_queue.empty()) {
            u32 begin;
            u32 end;
            Subroutine* routine;
            std::tie(begin, end, routine) = discover_queue.front();
            discover_queue.pop();

            if (!routine->discovered.emplace(begin, end).second) {
                continue;
            }

            for (u32 offset = begin; offset < (begin > end ? PROGRAM_END : end); ++offset) {
                const Instruction instr = {program_code[offset]};
                switch (instr.opcode.Value()) {
                case OpCode::Id::END: {
                    // Breaks the outer for loop if the program ends
                    offset = PROGRAM_END;
                    break;
                }

                case OpCode::Id::JMPC:
                case OpCode::Id::JMPU: {
                    routine->jumps[offset] = instr.flow_control.dest_offset;
                    discover_queue.emplace(instr.flow_control.dest_offset, routine->end, routine);
                    break;
                }

                case OpCode::Id::CALL:
                case OpCode::Id::CALLU:
                case OpCode::Id::CALLC: {
                    std::pair<u32, u32> sub_range{instr.flow_control.dest_offset,
                                                  instr.flow_control.dest_offset +
                                                      instr.flow_control.num_instructions};

                    auto& sub = GetRoutine(sub_range.first, sub_range.second);

                    sub.callers.emplace(offset, routine);
                    routine->calls[sub_range] = &sub;
                    discover_queue.emplace(sub_range.first, sub_range.second, &sub);

                    if (instr.opcode.Value() == OpCode::Id::CALL && sub.end_instr_distance) {
                        // Breaks the outer for loop if the unconditial subroutine ends the program
                        offset = PROGRAM_END;
                    }
                    break;
                }

                case OpCode::Id::IFU:
                case OpCode::Id::IFC: {
                    const u32 if_offset = offset + 1;
                    const u32 else_offset = instr.flow_control.dest_offset;
                    const u32 endif_offset =
                        instr.flow_control.dest_offset + instr.flow_control.num_instructions;
                    ASSERT(else_offset > if_offset);

                    // Both branches are treated as subroutine, so skips the if-else block in this
                    // routine
                    offset = endif_offset - 1;

                    auto& sub_if = GetRoutine(if_offset, else_offset);

                    sub_if.callers.emplace(offset, routine);
                    routine->branches[{if_offset, else_offset}] = &sub_if;
                    discover_queue.emplace(if_offset, else_offset, &sub_if);

                    if (instr.flow_control.num_instructions != 0) {
                        auto& sub_else = GetRoutine(else_offset, endif_offset);

                        sub_else.callers.emplace(offset, routine);
                        routine->branches[{else_offset, endif_offset}] = &sub_else;
                        discover_queue.emplace(else_offset, endif_offset, &sub_else);

                        if (sub_if.end_instr_distance && sub_else.end_instr_distance) {
                            // Breaks the outer for loop if both branches end the program
                            offset = PROGRAM_END;
                        }
                    }
                    break;
                }

                case OpCode::Id::LOOP: {
                    std::pair<u32, u32> sub_range{offset + 1, instr.flow_control.dest_offset + 1};
                    ASSERT(sub_range.second > sub_range.first);

                    auto& sub = GetRoutine(sub_range.first, sub_range.second);

                    sub.callers.emplace(offset, routine);
                    routine->branches[sub_range] = &sub;
                    discover_queue.emplace(sub_range.first, sub_range.second, &sub);

                    // The lopp block is treated as subroutine, so skips the if-else block in this
                    // routine.
                    // Note: the first instruction after the loop block is at dest_offset + 1 (due
                    // to PICA design), and the next instruction to scan is at offset + 1 (due to
                    // for loop increment). The two offset-by-one cancel each other here.
                    offset = instr.flow_control.dest_offset;

                    if (sub.end_instr_distance) {
                        // Breaks the outer for loop if the loop block ends the program
                        offset = PROGRAM_END;
                    }
                    break;
                }
                }
            }
        }
        return program_main;
    }

    bool HasRecursion() {
        for (auto& pair : subroutines) {
            auto& subroutine = pair.second;
            if (subroutine.HasRecursion()) {
                return true;
            }
        }

        return false;
    }

    /// Generates condition evaluation code for the flow control instruction.
    static std::string EvaluateCondition(Instruction::FlowControlType flow_control) {
        using Op = Instruction::FlowControlType::Op;

        std::string result_x =
            flow_control.refx.Value() ? "conditional_code.x" : "!conditional_code.x";
        std::string result_y =
            flow_control.refy.Value() ? "conditional_code.y" : "!conditional_code.y";

        switch (flow_control.op) {
        case Op::JustX:
            return result_x;
        case Op::JustY:
            return result_y;
        case Op::Or:
        case Op::And: {
            std::string and_or = flow_control.op == Op::Or ? "any" : "all";
            std::string bvec;
            if (flow_control.refx.Value() && flow_control.refy.Value()) {
                bvec = "conditional_code";
            } else if (!flow_control.refx.Value() && !flow_control.refy.Value()) {
                bvec = "not(conditional_code)";
            } else {
                bvec = "bvec2(" + result_x + ", " + result_y + ")";
            }
            return and_or + "(" + bvec + ")";
        }
        default:
            UNREACHABLE();
            return "";
        }
    };

    /// Generates code representing the source register.
    std::string GetSourceRegister(const SourceRegister& source_reg,
                                  u32 address_register_index) const {
        u32 index = static_cast<u32>(source_reg.GetIndex());
        std::string index_str = std::to_string(index);

        switch (source_reg.GetRegisterType()) {
        case RegisterType::Input:
            return inputreg_getter(index);
        case RegisterType::Temporary:
            return "reg_tmp" + index_str;
        case RegisterType::FloatUniform:
            if (address_register_index != 0) {
                index_str +=
                    std::string(" + address_registers.") + "xyz"[address_register_index - 1];
            }
            return "uniforms.f[" + index_str + "]";
        default:
            UNREACHABLE();
            return "";
        }
    };

public:
    Impl(const std::array<u32, MAX_PROGRAM_CODE_LENGTH>& program_code,
         const std::array<u32, MAX_SWIZZLE_DATA_LENGTH>& swizzle_data, u32 main_offset,
         const std::function<std::string(u32)>& inputreg_getter,
         const std::function<std::string(u32)>& outputreg_getter, bool sanitize_mul,
         const std::string& emit_cb, const std::string& setemit_cb)
        : program_code(program_code), swizzle_data(swizzle_data), main_offset(main_offset),
          inputreg_getter(inputreg_getter), outputreg_getter(outputreg_getter),
          sanitize_mul(sanitize_mul), emit_cb(emit_cb), setemit_cb(setemit_cb) {}

    std::string Decompile() {
        constexpr bool PRINT_DEBUG = true;

        if (!FindEndInstr(main_offset, PROGRAM_END)) {
            return "";
        }

        auto& program_main = AnalyzeControlFlow();

        if (HasRecursion())
            return "";

        std::string shader_source;
        int scope = 0;
        auto add_line = [&](const std::string& text) {
            ASSERT(scope >= 0);
            if (PRINT_DEBUG && !text.empty()) {
                shader_source += std::string(static_cast<size_t>(scope) * 4, ' ');
            }
            shader_source += text + '\n';
        };

        if (sanitize_mul) {
            add_line("vec4 sanitize_mul(vec4 lhs, vec4 rhs) {");
            ++scope;
            add_line("vec4 product = lhs * rhs;");
            add_line("return mix(product, mix(mix(vec4(0.0), product, isnan(rhs)), product, "
                     "isnan(lhs)), isnan(product));");
            --scope;
            add_line("}\n");
        }

        add_line("bvec2 conditional_code = bvec2(false);");
        add_line("ivec3 address_registers = ivec3(0);");
        for (int i = 0; i < 16; ++i) {
            add_line("vec4 reg_tmp" + std::to_string(i) + " = vec4(0.0, 0.0, 0.0, 1.0);");
        }
        add_line("");

        for (auto& pair : subroutines) {
            auto& subroutine = pair.second;
            if (!subroutine.IsInline()) {
                add_line("bool " + subroutine.GetName() + "();");
            }
        }
        add_line("");

        auto selector_to_string = [](const auto& selector_getter) -> std::string {
            std::string out;
            for (int i = 0; i < 4; ++i) {
                SwizzlePattern::Selector selector = selector_getter(i);
                switch (selector) {
                case SwizzlePattern::Selector::x:
                    out += "x";
                    break;
                case SwizzlePattern::Selector::y:
                    out += "y";
                    break;
                case SwizzlePattern::Selector::z:
                    out += "z";
                    break;
                case SwizzlePattern::Selector::w:
                    out += "w";
                    break;
                default:
                    UNREACHABLE();
                    return "";
                }
            }
            return out;
        };

        auto get_uniform_bool = [&](u32 index) -> std::string {
            if (!emit_cb.empty() && index == 15) {
                // The uniform b15 is set to true after every geometry shader invocation.
                return "((gl_PrimitiveIDIn == 0) || uniforms.b[15])";
            }
            return "uniforms.b[" + std::to_string(index) + "]";
        };

        std::function<u32(const Subroutine&)> call_subroutine;

        auto compile_instr = [&](u32 offset, const auto& jumper_fn) -> u32 {
            const Instruction instr = {program_code[offset]};

            size_t swizzle_offset = instr.opcode.Value().GetInfo().type == OpCode::Type::MultiplyAdd
                                        ? instr.mad.operand_desc_id
                                        : instr.common.operand_desc_id;
            const SwizzlePattern swizzle = {swizzle_data[swizzle_offset]};

            if (PRINT_DEBUG) {
                add_line("// " + std::to_string(offset) + ": " +
                         instr.opcode.Value().GetInfo().name);
            }

            auto set_dest = [&](const std::string& reg, const std::string& value,
                                u32 dest_num_components, u32 value_num_components) {
                u32 dest_mask_num_components = 0;
                std::string dest_mask_swizzle = ".";

                for (u32 i = 0; i < dest_num_components; ++i) {
                    if (swizzle.DestComponentEnabled(static_cast<int>(i))) {
                        dest_mask_swizzle += "xyzw"[i];
                        ++dest_mask_num_components;
                    }
                }

                if (reg.empty() || dest_mask_num_components == 0) {
                    return;
                }
                ASSERT(value_num_components >= dest_num_components || value_num_components == 1);

                std::string dest = reg + (dest_num_components != 1 ? dest_mask_swizzle : "");

                std::string src = value;
                if (value_num_components == 1) {
                    if (dest_mask_num_components != 1) {
                        src = "vec" + std::to_string(dest_mask_num_components) + "(" + value + ")";
                    }
                } else if (value_num_components != dest_mask_num_components) {
                    src = "(" + value + ")" + dest_mask_swizzle;
                }

                add_line(dest + " = " + src + ";");
            };

            switch (instr.opcode.Value().GetInfo().type) {
            case OpCode::Type::Arithmetic: {
                const bool is_inverted =
                    (0 != (instr.opcode.Value().GetInfo().subtype & OpCode::Info::SrcInversed));

                std::string src1 = swizzle.negate_src1 ? "-" : "";
                src1 += GetSourceRegister(instr.common.GetSrc1(is_inverted),
                                          !is_inverted * instr.common.address_register_index);
                src1 += "." +
                        selector_to_string([&](int comp) { return swizzle.GetSelectorSrc1(comp); });

                std::string src2 = swizzle.negate_src2 ? "-" : "";
                src2 += GetSourceRegister(instr.common.GetSrc2(is_inverted),
                                          is_inverted * instr.common.address_register_index);
                src2 += "." +
                        selector_to_string([&](int comp) { return swizzle.GetSelectorSrc2(comp); });

                std::string dest_reg =
                    (instr.common.dest.Value() < 0x10)
                        ? outputreg_getter(static_cast<u32>(instr.common.dest.Value().GetIndex()))
                        : (instr.common.dest.Value() < 0x20)
                              ? "reg_tmp" + std::to_string(instr.common.dest.Value().GetIndex())
                              : "";

                switch (instr.opcode.Value().EffectiveOpCode()) {
                case OpCode::Id::ADD: {
                    set_dest(dest_reg, src1 + " + " + src2, 4, 4);
                    break;
                }

                case OpCode::Id::MUL: {
                    if (sanitize_mul) {
                        set_dest(dest_reg, "sanitize_mul(" + src1 + ", " + src2 + ")", 4, 4);
                    } else {
                        set_dest(dest_reg, src1 + " * " + src2, 4, 4);
                    }
                    break;
                }

                case OpCode::Id::FLR: {
                    set_dest(dest_reg, "floor(" + src1 + ")", 4, 4);
                    break;
                }

                case OpCode::Id::MAX: {
                    set_dest(dest_reg, "max(" + src1 + ", " + src2 + ")", 4, 4);
                    break;
                }

                case OpCode::Id::MIN: {
                    set_dest(dest_reg, "min(" + src1 + ", " + src2 + ")", 4, 4);
                    break;
                }

                case OpCode::Id::DP3:
                case OpCode::Id::DP4:
                case OpCode::Id::DPH:
                case OpCode::Id::DPHI: {
                    OpCode::Id opcode = instr.opcode.Value().EffectiveOpCode();
                    std::string dot;
                    if (opcode == OpCode::Id::DP3) {
                        if (sanitize_mul) {
                            dot = "dot(vec3(sanitize_mul(" + src1 + ", " + src2 + ")), vec3(1.0))";
                        } else {
                            dot = "dot(vec3(" + src1 + "), vec3(" + src2 + "))";
                        }
                    } else {
                        std::string src1_ =
                            (opcode == OpCode::Id::DPH || opcode == OpCode::Id::DPHI)
                                ? "vec4(" + src1 + ".xyz, 1.0)"
                                : src1;
                        if (sanitize_mul) {
                            dot = "dot(sanitize_mul(" + src1_ + ", " + src2 + "), vec4(1.0))";
                        } else {
                            dot = "dot(" + src1 + ", " + src2 + ")";
                        }
                    }

                    set_dest(dest_reg, dot, 4, 1);
                    break;
                }

                case OpCode::Id::RCP: {
                    set_dest(dest_reg, "(1.0 / " + src1 + ".x)", 4, 1);
                    break;
                }

                case OpCode::Id::RSQ: {
                    set_dest(dest_reg, "inversesqrt(" + src1 + ".x)", 4, 1);
                    break;
                }

                case OpCode::Id::MOVA: {
                    set_dest("address_registers", "ivec2(" + src1 + ")", 2, 2);
                    break;
                }

                case OpCode::Id::MOV: {
                    set_dest(dest_reg, src1, 4, 4);
                    break;
                }

                case OpCode::Id::SGE:
                case OpCode::Id::SGEI: {
                    set_dest(dest_reg, "vec4(greaterThanEqual(" + src1 + "," + src2 + "))", 4, 4);
                    break;
                }

                case OpCode::Id::SLT:
                case OpCode::Id::SLTI: {
                    set_dest(dest_reg, "vec4(lessThan(" + src1 + "," + src2 + "))", 4, 4);
                    break;
                }

                case OpCode::Id::CMP: {
                    using CompareOp = Instruction::Common::CompareOpType::Op;
                    const std::map<CompareOp, std::pair<std::string, std::string>> cmp_ops{
                        {CompareOp::Equal, {"==", "equal"}},
                        {CompareOp::NotEqual, {"!=", "notEqual"}},
                        {CompareOp::LessThan, {"<", "lessThan"}},
                        {CompareOp::LessEqual, {"<=", "lessThanEqual"}},
                        {CompareOp::GreaterThan, {">", "greaterThan"}},
                        {CompareOp::GreaterEqual, {">=", "greaterThanEqual"}}};

                    const CompareOp op_x = instr.common.compare_op.x.Value();
                    const CompareOp op_y = instr.common.compare_op.y.Value();

                    if (cmp_ops.find(op_x) == cmp_ops.end()) {
                        LOG_ERROR(HW_GPU, "Unknown compare mode %x", static_cast<int>(op_x));
                    } else if (cmp_ops.find(op_y) == cmp_ops.end()) {
                        LOG_ERROR(HW_GPU, "Unknown compare mode %x", static_cast<int>(op_y));
                    } else if (op_x != op_y) {
                        add_line("conditional_code.x = " + src1 + ".x " +
                                 cmp_ops.find(op_x)->second.first + " " + src2 + ".x;");
                        add_line("conditional_code.y = " + src1 + ".y " +
                                 cmp_ops.find(op_y)->second.first + " " + src2 + ".y;");
                    } else {
                        add_line("conditional_code = " + cmp_ops.find(op_x)->second.second +
                                 "(vec2(" + src1 + "), vec2(" + src2 + "));");
                    }
                    break;
                }

                case OpCode::Id::EX2: {
                    set_dest(dest_reg, "exp2(" + src1 + ".x)", 4, 1);
                    break;
                }

                case OpCode::Id::LG2: {
                    set_dest(dest_reg, "log2(" + src1 + ".x)", 4, 1);
                    break;
                }

                default: {
                    LOG_ERROR(HW_GPU, "Unhandled arithmetic instruction: 0x%02x (%s): 0x%08x",
                              (int)instr.opcode.Value().EffectiveOpCode(),
                              instr.opcode.Value().GetInfo().name, instr.hex);
                    DEBUG_ASSERT(false);
                    break;
                }
                }

                break;
            }

            case OpCode::Type::MultiplyAdd: {
                if ((instr.opcode.Value().EffectiveOpCode() == OpCode::Id::MAD) ||
                    (instr.opcode.Value().EffectiveOpCode() == OpCode::Id::MADI)) {
                    bool is_inverted = (instr.opcode.Value().EffectiveOpCode() == OpCode::Id::MADI);

                    std::string src1 = swizzle.negate_src1 ? "-" : "";
                    src1 += GetSourceRegister(instr.mad.GetSrc1(is_inverted), 0);
                    src1 += "." + selector_to_string(
                                      [&](int comp) { return swizzle.GetSelectorSrc1(comp); });

                    std::string src2 = swizzle.negate_src2 ? "-" : "";
                    src2 += GetSourceRegister(instr.mad.GetSrc2(is_inverted),
                                              !is_inverted * instr.mad.address_register_index);
                    src2 += "." + selector_to_string(
                                      [&](int comp) { return swizzle.GetSelectorSrc2(comp); });

                    std::string src3 = swizzle.negate_src3 ? "-" : "";
                    src3 += GetSourceRegister(instr.mad.GetSrc3(is_inverted),
                                              is_inverted * instr.mad.address_register_index);
                    src3 += "." + selector_to_string(
                                      [&](int comp) { return swizzle.GetSelectorSrc3(comp); });

                    std::string dest_reg =
                        (instr.mad.dest.Value() < 0x10)
                            ? outputreg_getter(static_cast<u32>(instr.mad.dest.Value().GetIndex()))
                            : (instr.mad.dest.Value() < 0x20)
                                  ? "reg_tmp" + std::to_string(instr.mad.dest.Value().GetIndex())
                                  : "";

                    if (sanitize_mul) {
                        set_dest(dest_reg, "sanitize_mul(" + src1 + ", " + src2 + ") + " + src3, 4,
                                 4);
                    } else {
                        set_dest(dest_reg, src1 + " * " + src2 + " + " + src3, 4, 4);
                    }
                } else {
                    LOG_ERROR(HW_GPU, "Unhandled multiply-add instruction: 0x%02x (%s): 0x%08x",
                              (int)instr.opcode.Value().EffectiveOpCode(),
                              instr.opcode.Value().GetInfo().name, instr.hex);
                }
                break;
            }

            default: {
                switch (instr.opcode.Value()) {
                case OpCode::Id::END: {
                    add_line("return true;");
                    offset = PROGRAM_END - 1;
                    break;
                }

                case OpCode::Id::JMPC:
                case OpCode::Id::JMPU: {
                    std::string condition;
                    if (instr.opcode.Value() == OpCode::Id::JMPC) {
                        condition = EvaluateCondition(instr.flow_control);
                    } else {
                        bool invert_test = instr.flow_control.num_instructions & 1;
                        condition = (invert_test ? "!" : "") +
                                    get_uniform_bool(instr.flow_control.bool_uniform_id);
                    }

                    add_line("if (" + condition + ") {");
                    ++scope;
                    jumper_fn(instr.flow_control.dest_offset);

                    --scope;
                    add_line("}");
                    break;
                }

                case OpCode::Id::CALL:
                case OpCode::Id::CALLC:
                case OpCode::Id::CALLU: {
                    std::string condition;
                    if (instr.opcode.Value() == OpCode::Id::CALLC) {
                        condition = EvaluateCondition(instr.flow_control);
                    } else if (instr.opcode.Value() == OpCode::Id::CALLU) {
                        condition = get_uniform_bool(instr.flow_control.bool_uniform_id);
                    }

                    add_line(condition.empty() ? "{" : "if (" + condition + ") {");
                    ++scope;

                    auto& call_sub = GetRoutine(instr.flow_control.dest_offset,
                                                instr.flow_control.dest_offset +
                                                    instr.flow_control.num_instructions);

                    call_subroutine(call_sub);
                    if (instr.opcode.Value() == OpCode::Id::CALL && call_sub.end_instr_distance) {
                        offset = PROGRAM_END - 1;
                    }

                    --scope;
                    add_line("}");
                    break;
                }

                case OpCode::Id::NOP: {
                    break;
                }

                case OpCode::Id::IFC:
                case OpCode::Id::IFU: {
                    std::string condition;
                    if (instr.opcode.Value() == OpCode::Id::IFC) {
                        condition = EvaluateCondition(instr.flow_control);
                    } else {
                        condition = get_uniform_bool(instr.flow_control.bool_uniform_id);
                    }

                    const u32 if_offset = offset + 1;
                    const u32 else_offset = instr.flow_control.dest_offset;
                    const u32 endif_offset =
                        instr.flow_control.dest_offset + instr.flow_control.num_instructions;

                    add_line("if (" + condition + ") {");
                    ++scope;

                    auto& if_sub = GetRoutine(if_offset, else_offset);
                    call_subroutine(if_sub);
                    offset = else_offset - 1;

                    if (instr.flow_control.num_instructions != 0) {
                        --scope;
                        add_line("} else {");
                        ++scope;

                        auto& else_sub = GetRoutine(else_offset, endif_offset);
                        call_subroutine(else_sub);
                        offset = endif_offset - 1;

                        if (if_sub.end_instr_distance && else_sub.end_instr_distance) {
                            offset = PROGRAM_END - 1;
                        }
                    }

                    --scope;
                    add_line("}");

                    if (offset == PROGRAM_END - 1) {
                        add_line("return true;");
                    }
                    break;
                }

                case OpCode::Id::LOOP: {
                    std::string int_uniform =
                        "uniforms.i[" + std::to_string(instr.flow_control.int_uniform_id) + "]";

                    add_line("address_registers.z = int(" + int_uniform + ".y);");

                    std::string loop_var = "loop" + std::to_string(offset);
                    add_line("for (uint " + loop_var + " = 0u; " + loop_var + " <= " + int_uniform +
                             ".x; address_registers.z += int(" + int_uniform + ".z), ++" +
                             loop_var + ") {");
                    ++scope;

                    auto& loop_sub = GetRoutine(offset + 1, instr.flow_control.dest_offset + 1);
                    call_subroutine(loop_sub);
                    offset = instr.flow_control.dest_offset;

                    --scope;
                    add_line("}");

                    if (loop_sub.end_instr_distance) {
                        offset = PROGRAM_END - 1;
                        add_line("return true;");
                    }

                    break;
                }

                case OpCode::Id::EMIT: {
                    if (!emit_cb.empty()) {
                        add_line(emit_cb + "();");
                    }
                    break;
                }

                case OpCode::Id::SETEMIT: {
                    if (!setemit_cb.empty()) {
                        ASSERT(instr.setemit.vertex_id < 3);
                        add_line(setemit_cb + "(" + std::to_string(instr.setemit.vertex_id) +
                                 "u, " + ((instr.setemit.prim_emit != 0) ? "true" : "false") +
                                 ", " + ((instr.setemit.winding != 0) ? "true" : "false") + ");");
                    }
                    break;
                }

                default: {
                    LOG_ERROR(HW_GPU, "Unhandled instruction: 0x%02x (%s): 0x%08x",
                              (int)instr.opcode.Value().EffectiveOpCode(),
                              instr.opcode.Value().GetInfo().name, instr.hex);
                    break;
                }
                }

                break;
            }
            }
            return offset + 1;
        };

        auto compile_range = [&](u32 begin, u32 end, const auto& jmp_cb) -> u32 {
            u32 program_counter;
            for (program_counter = begin; program_counter < (begin > end ? PROGRAM_END : end);) {
                program_counter = compile_instr(program_counter, jmp_cb);
            }
            return program_counter;
        };

        call_subroutine = [&](const Subroutine& subroutine) -> u32 {
            if (subroutine.IsInline()) {
                return compile_range(subroutine.begin, subroutine.end, [](u32) { UNREACHABLE(); });
            }

            std::function<bool(const Subroutine&)> maybe_end_instr =
                [&maybe_end_instr](const Subroutine& subroutine) -> bool {
                for (auto& callee : subroutine.calls) {
                    if (maybe_end_instr(*callee.second)) {
                        return true;
                    }
                }
                for (auto& branch : subroutine.branches) {
                    if (maybe_end_instr(*branch.second)) {
                        return true;
                    }
                }
                return subroutine.end_instr_distance.is_initialized();
            };

            if (subroutine.end_instr_distance) {
                add_line(subroutine.GetName() + "();");
                add_line("return true;");
            } else if (maybe_end_instr(subroutine)) {
                add_line("if (" + subroutine.GetName() + "()) { return true; }");
            } else {
                add_line(subroutine.GetName() + "();");
            }

            return subroutine.end;
        };

        add_line("bool exec_shader() {");
        ++scope;
        call_subroutine(program_main);
        --scope;
        add_line("}\n");

        for (auto& pair : subroutines) {
            auto& subroutine = pair.second;
            if (subroutine.IsInline()) {
                continue;
            }

            std::set<u32> labels;
            for (auto& jump : subroutine.jumps) {
                const auto& opt_end = FindEndInstr(jump.second, subroutine.end);
                const u32 end_dist =
                    opt_end ? static_cast<u32>(*opt_end) : subroutine.end - jump.second;
                if (jump.second < jump.first || end_dist > 10) {
                    labels.insert(jump.second);
                }
            }

            std::function<void(u32)> jmp_callback;
            jmp_callback = [&](u32 offset) {
                if (labels.find(offset) == labels.end()) {
                    if (compile_range(offset, subroutine.end, jmp_callback) != PROGRAM_END) {
                        add_line("return false;");
                    }
                } else {
                    add_line("{ jmp_to = " + std::to_string(offset) + "u; break; }");
                }
            };

            add_line("bool " + subroutine.GetName() + "() {");
            ++scope;

            if (labels.empty()) {
                if (compile_range(subroutine.begin, subroutine.end, jmp_callback) != PROGRAM_END) {
                    add_line("return false;");
                }
            } else {
                labels.insert(subroutine.begin);
                add_line("uint jmp_to = " + std::to_string(subroutine.begin) + "u;");
                add_line("while (true) {");
                ++scope;

                add_line("switch (jmp_to) {");

                for (auto label : labels) {
                    add_line("case " + std::to_string(label) + "u: {");
                    ++scope;

                    auto next_it = labels.lower_bound(label + 1);
                    u32 next_label = next_it == labels.end() ? subroutine.end : *next_it;

                    u32 compile_end = compile_range(label, next_label, jmp_callback);
                    if (compile_end > next_label && compile_end != PROGRAM_END) {
                        add_line("{ jmp_to = " + std::to_string(compile_end) + "u; break; }");
                        labels.emplace(compile_end);
                    }

                    --scope;
                    add_line("}");
                }

                add_line("default: return false;");
                add_line("}");

                --scope;
                add_line("}");

                add_line("return false;");
            }

            --scope;
            add_line("}\n");

            ASSERT(!scope);
        }

        return shader_source;
    }

private:
    const std::array<u32, MAX_PROGRAM_CODE_LENGTH>& program_code;
    const std::array<u32, MAX_SWIZZLE_DATA_LENGTH>& swizzle_data;
    u32 main_offset;
    const std::function<std::string(u32)>& inputreg_getter;
    const std::function<std::string(u32)>& outputreg_getter;
    bool sanitize_mul;
    const std::string& emit_cb;
    const std::string& setemit_cb;
};

std::string DecompileProgram(const std::array<u32, MAX_PROGRAM_CODE_LENGTH>& program_code,
                             const std::array<u32, MAX_SWIZZLE_DATA_LENGTH>& swizzle_data,
                             u32 main_offset,
                             const std::function<std::string(u32)>& inputreg_getter,
                             const std::function<std::string(u32)>& outputreg_getter,
                             bool sanitize_mul, const std::string& emit_cb,
                             const std::string& setemit_cb) {
    Impl impl(program_code, swizzle_data, main_offset, inputreg_getter, outputreg_getter,
              sanitize_mul, emit_cb, setemit_cb);
    return impl.Decompile();
}

} // namespace Decompiler
} // namespace Shader
} // namespace Pica
