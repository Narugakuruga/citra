// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>

#include "common/common_types.h"

#include "core/arm/arm_interface.h"
#include "core/arm/jit_x64/common.h"

namespace JitX64 {

class ARM_Jit final : virtual public ARM_Interface {
public:
    ARM_Jit(PrivilegeMode initial_mode);
    ~ARM_Jit();

    void SetPC(u32 pc) override;
    u32 GetPC() const override;
    u32 GetReg(int index) const override;
    void SetReg(int index, u32 value) override;
    u32 GetVFPReg(int index) const override;
    void SetVFPReg(int index, u32 value) override;
    u32 GetVFPSystemReg(VFPSystemRegister reg) const override;
    void SetVFPSystemReg(VFPSystemRegister reg, u32 value) override;
    u32 GetCPSR() const override;
    void SetCPSR(u32 cpsr) override;
    u32 GetCP15Register(CP15Register reg) override;
    void SetCP15Register(CP15Register reg, u32 value) override;

    void AddTicks(u64 ticks) override;

    void ResetContext(Core::ThreadContext& context, u32 stack_top, u32 entry_point, u32 arg) override;
    void SaveContext(Core::ThreadContext& ctx) override;
    void LoadContext(const Core::ThreadContext& ctx) override;

    void PrepareReschedule() override;
    void ExecuteInstructions(int num_instructions) override;

    void ClearCache() override;
    void FastClearCache();

private:
    struct Impl;

    std::unique_ptr<Impl> impl;
    bool reschedule = false;
    std::unique_ptr<JitState> state;
};

}
