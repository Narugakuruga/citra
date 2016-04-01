// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>

#include "common/common_types.h"

#include "core/arm/arm_interface.h"
#include "core/arm/skyeye_common/armstate.h"

#ifdef _WIN32
#include "core/arm/unicorn/unicorn_dynload.h"
#else
#include <unicorn/unicorn.h>
#endif

namespace Core {
struct ThreadContext;
}

class ARM_Unicorn final : virtual public ARM_Interface {
public:
    ARM_Unicorn(PrivilegeMode initial_mode);
    ~ARM_Unicorn();

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

    // public because memory needs this in order to map the pointer to the engine.
    // todo design that better
    uc_engine* engine;
private:
    uc_hook service;
    u32 NumInstrsToExecute;
};
