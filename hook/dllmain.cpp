// dllmain.cpp — minimal DLL entry point. Real initialization runs on a Win32
// worker thread after DllMain returns, outside the loader lock.
#include "touch_hook.h"

#include <windows.h>

#include <string>

namespace {

std::wstring GetDllDir(HMODULE mod) {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(mod, path, MAX_PATH);
    std::wstring p = path;
    const auto pos = p.find_last_of(L'\\');
    return pos == std::wstring::npos ? std::wstring() : p.substr(0, pos);
}

DWORD WINAPI InitThread(void* param) {
    const HMODULE mod = static_cast<HMODULE>(param);
    zzz::touch::Run(GetDllDir(mod));
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*reserved*/) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;

    DisableThreadLibraryCalls(hModule);
    HANDLE worker = CreateThread(nullptr, 0, &InitThread, hModule, 0, nullptr);
    if (!worker) return FALSE;
    CloseHandle(worker);
    return TRUE;
}
