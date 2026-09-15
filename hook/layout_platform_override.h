#pragma once

#include <windows.h>
#include <cstdint>

namespace zzz::layout_override {

bool ArmEarlyUi(int layout);
bool FinalizeRuntime(DWORD timeout_ms);
uint32_t LayoutFieldOffset();

}  // namespace zzz::layout_override
