#pragma once

#include <windows.h>
#include <cstdint>

namespace zzz::layout_override {

bool ArmEarlyMobileUi();
bool FinalizeRuntime(DWORD timeout_ms);
uint32_t LayoutFieldOffset();

}  // namespace zzz::layout_override
