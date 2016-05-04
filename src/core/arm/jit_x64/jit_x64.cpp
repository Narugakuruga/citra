// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "core/arm/dyncom/arm_dyncom_interpreter.h"
#include "core/arm/jit_x64/jit_x64.h"
#include "core/memory.h"

namespace JitX64 {

using namespace Gen;

JitX64::JitX64(XEmitter* code) : code(code) {}

void JitX64::ClearCache() {
    basic_blocks.clear();
    patch_jmp_locations.clear();
    InterpreterClearCache();
}

CodePtr JitX64::GetBB(u32 pc, bool TFlag, bool EFlag) {
    const LocationDescriptor desc = { pc, TFlag, EFlag };

    if (basic_blocks.find(desc) == basic_blocks.end()) {
        return Compile(pc, TFlag, EFlag);
    }

    return basic_blocks[desc];
}

CodePtr JitX64::Compile(u32 pc, bool TFlag, bool EFlag) {
    const CodePtr bb = code->GetWritableCodePtr();
    const LocationDescriptor desc = { pc, TFlag, EFlag };
    ASSERT(basic_blocks.find(desc) == basic_blocks.end());
    basic_blocks[desc] = bb;
    Patch(desc, bb);

    reg_alloc.Init(code);
    cond_manager.Init(this);
    current = desc;
    instructions_compiled = 0;
    stop_compilation = false;

    do {
        instructions_compiled++;

        if (current.TFlag) {
            CompileSingleThumbInstruction();
        } else {
            CompileSingleArmInstruction();
        }

        reg_alloc.AssertNoLocked();
    } while (!stop_compilation && ((current.arm_pc & 0xFFF) != 0));

    if (!stop_compilation) {
        // We're stopping compilation because we've reached a page boundary.
        cond_manager.Always();
        CompileUpdateCycles();
        CompileJumpToBB(current.arm_pc);
    }

    // Insert easily searchable byte sequence for ease of lookup in memory dumps.
    code->NOP();
    code->INT3();
    code->NOP();

    return bb;
}

void JitX64::CompileUpdateCycles(bool reset_cycles) {
    // We're just taking one instruction == one cycle.
    if (instructions_compiled) {
        code->SUB(32, MJitStateCycleCount(), Imm32(instructions_compiled));
    }
    if (reset_cycles) {
        instructions_compiled = 0;
    }
}

void JitX64::CompileReturnToDispatch() {
    if (cond_manager.CurrentCond() == Cond::AL) {
        reg_alloc.FlushEverything();
        CompileUpdateCycles();
        code->JMPptr(MJitStateHostReturnRIP());

        stop_compilation = true;
        return;
    }

    reg_alloc.FlushEverything();
    CompileUpdateCycles(false);
    code->JMPptr(MJitStateHostReturnRIP());

    cond_manager.Always();
    CompileUpdateCycles(true);
    CompileJumpToBB(current.arm_pc);
    code->MOV(32, MJitStateArmPC(), Imm32(current.arm_pc));
    code->JMPptr(MJitStateHostReturnRIP());

    stop_compilation = true;
    return;
}

void JitX64::CompileJumpToBB(u32 new_pc) {
    reg_alloc.FlushEverything();
    code->CMP(32, MJitStateCycleCount(), Imm8(0));

    const LocationDescriptor new_desc = { new_pc, current.TFlag, current.EFlag };
    patch_jmp_locations[new_desc].emplace_back(code->GetWritableCodePtr());
    if (basic_blocks.find(new_desc) == basic_blocks.end()) {
        code->NOP(6); // Leave enough space for a jg instruction.
    } else {
        code->J_CC(CC_G, basic_blocks[new_desc], true);
    }

    code->MOV(32, MJitStateArmPC(), Imm32(new_pc));
    code->JMPptr(MJitStateHostReturnRIP());
}

void JitX64::Patch(LocationDescriptor desc, CodePtr bb) {
    const CodePtr save_code_ptr = code->GetWritableCodePtr();

    for (CodePtr location : patch_jmp_locations[desc]) {
        code->SetCodePtr(location);
        code->J_CC(CC_G, bb, true);
        ASSERT(code->GetCodePtr() - location == 6);
    }

    code->SetCodePtr(save_code_ptr);
}

void JitX64::CompileSingleArmInstruction() {
    u32 inst = Memory::Read32(current.arm_pc & 0xFFFFFFFC);

    auto inst_info = ArmDecoder::DecodeArm(inst);
    if (!inst_info) {
        // TODO: Log message
        CompileInterpretInstruction();
    } else {
        inst_info->Visit(this, inst);
    }
}

void JitX64::CompileSingleThumbInstruction() {
    u32 inst_u32 = Memory::Read32(current.arm_pc & 0xFFFFFFFC);
    if ((current.arm_pc & 0x2) != 0) {
        inst_u32 >>= 16;
    }
    inst_u32 &= 0xFFFFF;
    u16 inst = inst_u32;

    auto inst_info = ArmDecoder::DecodeThumb(inst);
    if (!inst_info) {
        // TODO: Log message
        CompileInterpretInstruction();
    } else {
        inst_info->Visit(this, inst);
    }
}

// Convenience functions:
// We static_assert types because anything that calls these functions makes those assumptions.
// If the types of the variables are changed please update all code that calls these functions.

Gen::OpArg JitX64::MJitStateCycleCount() const {
    static_assert(std::is_same<decltype(JitState::cycles_remaining), s32>::value, "JitState::cycles_remaining must be s32");

    return Gen::MDisp(reg_alloc.JitStateReg(), offsetof(JitState, cycles_remaining));
}

Gen::OpArg JitX64::MJitStateArmPC() const {
    static_assert(std::is_same<decltype(JitState::cpu_state), ARMul_State>::value, "JitState::cpu_state must be ARMul_State");
    static_assert(std::is_same<decltype(ARMul_State::Reg), std::array<u32, 16>>::value, "ARMul_State::Reg must be std::array<u32, 16>");

    return Gen::MDisp(reg_alloc.JitStateReg(), offsetof(JitState, cpu_state) + offsetof(ARMul_State, Reg) + 15 * sizeof(u32));
}

Gen::OpArg JitX64::MJitStateTFlag() const {
    static_assert(std::is_same<decltype(JitState::cpu_state), ARMul_State>::value, "JitState::cpu_state must be ARMul_State");
    static_assert(std::is_same<decltype(ARMul_State::TFlag), u32>::value, "TFlag must be u32");

    return Gen::MDisp(reg_alloc.JitStateReg(), offsetof(JitState, cpu_state) + offsetof(ARMul_State, TFlag));
}

Gen::OpArg JitX64::MJitStateHostReturnRIP() const {
    static_assert(std::is_same<decltype(JitState::return_RIP), u64>::value, "JitState::return_RIP must be u64");

    return Gen::MDisp(reg_alloc.JitStateReg(), offsetof(JitState, return_RIP));
}

Gen::OpArg JitX64::MJitStateHostReturnRSP() const {
    static_assert(std::is_same<decltype(JitState::save_host_RSP), u64>::value, "JitState::save_host_RSP must be u64");

    return Gen::MDisp(reg_alloc.JitStateReg(), offsetof(JitState, save_host_RSP));
}

Gen::OpArg JitX64::MJitStateZFlag() const {
    static_assert(std::is_same<decltype(JitState::cpu_state), ARMul_State>::value, "JitState::cpu_state must be ARMul_State");
    static_assert(std::is_same<decltype(ARMul_State::ZFlag), u32>::value, "ZFlag must be u32");

    return Gen::MDisp(reg_alloc.JitStateReg(), offsetof(JitState, cpu_state) + offsetof(ARMul_State, ZFlag));
}

Gen::OpArg JitX64::MJitStateCFlag() const {
    static_assert(std::is_same<decltype(JitState::cpu_state), ARMul_State>::value, "JitState::cpu_state must be ARMul_State");
    static_assert(std::is_same<decltype(ARMul_State::CFlag), u32>::value, "CFlag must be u32");

    return Gen::MDisp(reg_alloc.JitStateReg(), offsetof(JitState, cpu_state) + offsetof(ARMul_State, CFlag));
}

Gen::OpArg JitX64::MJitStateNFlag() const {
    static_assert(std::is_same<decltype(JitState::cpu_state), ARMul_State>::value, "JitState::cpu_state must be ARMul_State");
    static_assert(std::is_same<decltype(ARMul_State::NFlag), u32>::value, "NFlag must be u32");

    return Gen::MDisp(reg_alloc.JitStateReg(), offsetof(JitState, cpu_state) + offsetof(ARMul_State, NFlag));
}

Gen::OpArg JitX64::MJitStateVFlag() const {
    static_assert(std::is_same<decltype(JitState::cpu_state), ARMul_State>::value, "JitState::cpu_state must be ARMul_State");
    static_assert(std::is_same<decltype(ARMul_State::VFlag), u32>::value, "VFlag must be u32");

    return Gen::MDisp(reg_alloc.JitStateReg(), offsetof(JitState, cpu_state) + offsetof(ARMul_State, VFlag));
}

Gen::OpArg JitX64::MJitStateCpsr() const {
    static_assert(std::is_same<decltype(JitState::cpu_state), ARMul_State>::value, "JitState::cpu_state must be ARMul_State");
    static_assert(std::is_same<decltype(ARMul_State::Cpsr), u32>::value, "Cpsr must be u32");

    return Gen::MDisp(reg_alloc.JitStateReg(), offsetof(JitState, cpu_state) + offsetof(ARMul_State, Cpsr));
}

Gen::OpArg JitX64::MJitStateExclusiveTag() const {
    static_assert(std::is_same<decltype(JitState::cpu_state), ARMul_State>::value, "JitState::cpu_state must be ARMul_State");
    static_assert(std::is_same<decltype(ARMul_State::exclusive_tag), u32>::value, "exclusive_tag must be u32");

    return Gen::MDisp(reg_alloc.JitStateReg(), offsetof(JitState, cpu_state) + offsetof(ARMul_State, exclusive_tag));
}

Gen::OpArg JitX64::MJitStateExclusiveState() const {
    static_assert(std::is_same<decltype(JitState::cpu_state), ARMul_State>::value, "JitState::cpu_state must be ARMul_State");
    static_assert(std::is_same<decltype(ARMul_State::exclusive_state), bool>::value, "exclusive_state must be bool");

    return Gen::MDisp(reg_alloc.JitStateReg(), offsetof(JitState, cpu_state) + offsetof(ARMul_State, exclusive_state));
}

// Common instruction subroutines

void JitX64::CompileCallHost(const void* const fn) {
    // There is no need to setup the stack as the stored RSP has already been properly aligned.

    reg_alloc.FlushABICallerSaved();

    ASSERT(reg_alloc.JitStateReg() != RSP);
    code->MOV(64, R(RSP), MJitStateHostReturnRSP());

    const uintptr_t distance = reinterpret_cast<uintptr_t>(fn) - (reinterpret_cast<uintptr_t>(code->GetCodePtr()) + 5);
    if (distance >= 0x0000000080000000ULL && distance < 0xFFFFFFFF80000000ULL) {
        // Far call
        code->MOV(64, R(RAX), ImmPtr(fn));
        code->CALLptr(R(RAX));
    } else {
        code->CALL(fn);
    }
}

u32 JitX64::PC() const {
    // When executing an ARM instruction, PC reads as the address of that instruction plus 8.
    // When executing an Thumb instruction, PC reads as the address of that instruction plus 4.
    return !current.TFlag ? current.arm_pc + 8 : current.arm_pc + 4;
}

u32 JitX64::PC_WordAligned() const {
    return PC() & 0xFFFFFFFC;
}

u32 JitX64::ExpandArmImmediate(int rotate, ArmImm8 imm8) {
    return CompileExpandArmImmediate_C(rotate, imm8, false);
}

u32 JitX64::CompileExpandArmImmediate_C(int rotate, ArmImm8 imm8, bool update_cflag) {
    u32 immediate = rotr(imm8, rotate * 2);

    if (rotate != 0 && update_cflag) {
        code->MOV(32, MJitStateCFlag(), Gen::Imm32(immediate & 0x80000000 ? 1 : 0));
    }

    return immediate;
}

void JitX64::CompileALUWritePC() {
    reg_alloc.FlushArm(ArmReg::PC);
    code->AND(32, MJitStateArmPC(), Gen::Imm32(!current.TFlag ? 0xFFFFFFFC : 0xFFFFFFFE));
}

}
