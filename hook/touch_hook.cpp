// touch_hook.cpp — pattern-only native-touch runtime initialization.
#include "touch_hook.h"

#include "general_data_persistence_guard.h"
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

    const bool inputArmed = zzz::legacy::ArmEarlyRestore();
    const bool layoutArmed = zzz::layout_override::ArmEarlyMobileUi();
    if (!inputArmed || !layoutArmed) {
        LOG_ERROR("NTR_ABORT early override arm failed input=%d layout=%d",
                  inputArmed ? 1 : 0, layoutArmed ? 1 : 0);
        return;  // no ready signal: launcher fails closed
    }
    SignalLauncherReady();

    // Establish the live layout and its persistence guard as early as possible.
    if (!zzz::layout_override::FinalizeRuntime(60 * 1000)) {
        LOG_ERROR("NTR_ABORT layout finalize failed");
        return;
    }
    if (!zzz::persistence_guard::Install(60 * 1000)) {
        LOG_ERROR("NTR_ABORT persistence guard install failed");
        return;
    }

    // Legacy manager validation completes after Unity's main thread resumes.
    if (!zzz::legacy::FinalizeRuntime(60 * 1000)) {
        LOG_ERROR("NTR_ABORT input finalize failed");
        return;
    }

    LOG_INFO("NTR_READY touchInput=LEGACY_NATIVE liveLayoutPlatform=1 "
             "persistPolicy=FIRST_OBSERVED locators=PATTERN_ONLY "
             "layoutFieldOffset=0x%X fileMutationByUnlocker=NONE",
             static_cast<unsigned>(zzz::layout_override::LayoutFieldOffset()));
}

}  // namespace zzz::touch
