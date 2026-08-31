#pragma once

#include <windows.h>

namespace zzz::legacy {

// Arm the Old-Input-only patch before the game's primary thread resumes.
bool ArmEarlyRestore();

// Wait for Unity's input initialization and validate the resulting runtime state.
bool FinalizeRuntime(DWORD timeout_ms);

}  // namespace zzz::legacy
