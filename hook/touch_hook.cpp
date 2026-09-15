// touch_hook.cpp — pattern-only native-touch runtime initialization.
#include "touch_hook.h"

#include "layout_platform_override.h"
#include "legacy_touch_restore.h"
#include "logger.h"

#include <windows.h>

namespace zzz::touch {
namespace {

void SignalLauncherReady() {
    const DWORD pid = GetCurrentProcessId();
    wchar_t readyName[128]{};
    swprintf_s(readyName, L"Local\\ZZZTouchHookReady_%lu", pid);
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName);
    if (ready) {
        SetEvent(ready);
        CloseHandle(ready);
        LOG_INFO("launcher ready event signaled (pid=%lu)", pid);
    }
}

}  // namespace

void Run(const std::wstring& unlocker_dir) {
    zzz::log::Init(unlocker_dir.c_str());
    LOG_INFO("=========================================");
    LOG_INFO("zzz-touch-hook starting (pattern-only)");
    LOG_INFO("=========================================");

    wchar_t mode[2]{};
    const DWORD length = GetEnvironmentVariableW(L"ZZZ_TOUCH_MODE", mode, 2);
    if (length != 1 || (mode[0] != L'0' && mode[0] != L'1')) {
        LOG_ERROR("NTR_ABORT missing/invalid launcher mode");
        return;
    }
    const bool mobile = mode[0] == L'1';
    const int layout = mobile ? 1 : 2;  // Game enum: Mobile=1, PC=2.
    LOG_INFO("NTR_MODE mode=%d target=%s liveLayoutPlatform=%d",
             mobile ? 1 : 0, mobile ? "MOBILE" : "DESKTOP", layout);
    const bool inputArmed = !mobile || zzz::legacy::ArmEarlyRestore();
    const bool layoutArmed = zzz::layout_override::ArmEarlyUi(layout);
    if (!inputArmed || !layoutArmed) {
        LOG_ERROR("NTR_ABORT early override arm failed input=%d layout=%d",
                  inputArmed ? 1 : 0, layoutArmed ? 1 : 0);
        return;  // no ready signal: launcher fails closed
    }
    SignalLauncherReady();

    // Establish the selected layout independently of saved preferences.
    if (!zzz::layout_override::FinalizeRuntime(60 * 1000)) {
        LOG_ERROR("NTR_ABORT layout finalize failed");
        return;
    }
    // Legacy manager validation completes after Unity's main thread resumes.
    if (mobile && !zzz::legacy::FinalizeRuntime(60 * 1000)) {
        LOG_ERROR("NTR_ABORT input finalize failed");
        return;
    }

    LOG_INFO("NTR_READY touchInput=%s liveLayoutPlatform=%d "
             "persistPolicy=GAME_DEFAULT locators=PATTERN_ONLY "
             "layoutFieldOffset=0x%X fileMutationByUnlocker=NONE",
             mobile ? "LEGACY_NATIVE" : "GAME_DEFAULT", layout,
             static_cast<unsigned>(zzz::layout_override::LayoutFieldOffset()));
}

}  // namespace zzz::touch
