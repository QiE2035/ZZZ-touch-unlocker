#pragma once

#include <windows.h>

namespace zzz::persistence_guard {

// Install the narrow GeneralLocalData serialization guard.
bool Install(DWORD timeout_ms);

}  // namespace zzz::persistence_guard
