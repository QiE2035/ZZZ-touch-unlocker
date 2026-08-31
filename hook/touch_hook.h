#pragma once

#include <string>

namespace zzz::touch {

// Initialize the runtime patches. The DLL directory is used only for logging.
void Run(const std::wstring& unlocker_dir);

}  // namespace zzz::touch
