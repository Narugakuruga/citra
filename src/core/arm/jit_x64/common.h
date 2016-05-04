// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "common/common_types.h"

#include "core/arm/decoder/decoder.h"
#include "core/arm/skyeye_common/armstate.h"

namespace JitX64 {

using ArmReg = ArmDecoder::Register;
using ArmRegList = ArmDecoder::RegisterList;
using ArmImm4 = ArmDecoder::Imm4;
using ArmImm5 = ArmDecoder::Imm5;
using ArmImm8 = ArmDecoder::Imm8;
using ArmImm11 = ArmDecoder::Imm11;
using ArmImm12 = ArmDecoder::Imm12;
using ArmImm24 = ArmDecoder::Imm24;
using Cond = ArmDecoder::Cond;
using ShiftType = ArmDecoder::ShiftType;
using SignExtendRotation = ArmDecoder::SignExtendRotation;

struct JitState final {
    JitState() : cpu_state(PrivilegeMode::USER32MODE) {}
    void Reset() {
        cpu_state.Reset();
    }

    ARMul_State cpu_state;

    /// This value should always be appropriately aligned for a CALL instruction to be made.
    u64 save_host_RSP = 0;
    /// Jitted code will JMP to this value when done. Should contain the an address which returns to the dispatcher.
    u64 return_RIP = 0;
    /// If this value becomes <= 0, jitted code will jump to return_RIP.
    s32 cycles_remaining = 0;
};

constexpr bool IsValidArmReg(ArmReg arm_reg) {
    return static_cast<unsigned>(arm_reg) <= 15;
}

inline bool IsEvenArmReg(ArmReg arm_reg) {
    ASSERT(IsValidArmReg(arm_reg));
    return static_cast<unsigned>(arm_reg) % 2 == 0;
}

/// Turns a ArmReg into an ArmRegList bitmap.
constexpr ArmRegList MakeRegList(ArmReg arm_reg) {
    return 1 << static_cast<unsigned>(arm_reg);
}

}
