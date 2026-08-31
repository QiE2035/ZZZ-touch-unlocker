#include "legacy_touch_restore.h"

#include "logger.h"
#include "patterns.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace zzz::legacy {
namespace {

constexpr const char* kInputLocatorPattern =
    "45 33 FF 44 38 B8 ?? ?? ?? ?? 0F 84 ?? ?? ?? ?? "
    "B9 D8 00 00 00 E8 ?? ?? ?? ?? 48 85 C0";
constexpr size_t kTargetOffsetFromPattern = 3;

struct LocatedInput {
    uintptr_t target = 0;
    uintptr_t legacyManagerGlobal = 0;
    uintptr_t newManagerGlobal = 0;
    uintptr_t playerSettingsGlobal = 0;
    uint32_t playerSettingsField = 0;
    uintptr_t legacyBranchTarget = 0;
};

enum class EarlyState : int {
    Idle = 0,
    Armed,
    Patching,
    Patched,
    TooLate,
    SignatureMismatch,
    PatternAmbiguous,
    ProtectFailed,
    NotificationUnavailable,
    NotificationFailed,
};

std::atomic<EarlyState> g_earlyState{EarlyState::Idle};
std::atomic<uintptr_t> g_legacyManagerGlobal{0};
std::atomic<uintptr_t> g_newManagerGlobal{0};
std::atomic<uintptr_t> g_playerSettingsGlobal{0};
std::atomic<uint32_t> g_playerSettingsField{0};
void* g_dllNotifyCookie = nullptr;

using NtStatus = LONG;
struct ZzzUnicodeString {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};
struct ZzzLdrDllLoadedData {
    ULONG Flags;
    const ZzzUnicodeString* FullDllName;
    const ZzzUnicodeString* BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
};
union ZzzLdrDllNotificationData {
    ZzzLdrDllLoadedData Loaded;
    ZzzLdrDllLoadedData Unloaded;
};
using ZzzLdrDllNotificationFn = void (NTAPI*)(
    ULONG reason, const ZzzLdrDllNotificationData* data, void* context);
using LdrRegisterDllNotificationFn = NtStatus (NTAPI*)(
    ULONG flags, ZzzLdrDllNotificationFn callback, void* context, void** cookie);
using LdrUnregisterDllNotificationFn = NtStatus (NTAPI*)(void* cookie);
LdrUnregisterDllNotificationFn g_ldrUnregister = nullptr;

bool WriteBytes(uintptr_t address, const uint8_t* bytes, size_t size) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(address), size,
                        PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    std::memcpy(reinterpret_cast<void*>(address), bytes, size);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), size);
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(address), size, oldProtect, &ignored);
    return true;
}

bool LooksLikeOriginal(uintptr_t target, uint32_t& field, uintptr_t& branchTarget) {
    if (!target) return false;
    const auto* p = reinterpret_cast<const uint8_t*>(target);
    if (p[0] != 0x44 || p[1] != 0x38 || p[2] != 0xB8 ||
        p[7] != 0x0F || p[8] != 0x84) return false;
    field = *reinterpret_cast<const uint32_t*>(p + 3);
    branchTarget = zzz::patterns::DecodeRel32Target(target + 7, 2, 6);
    return field >= 0x100 && field < 0x1000 && branchTarget > target;
}

uintptr_t FindSingleRipStore(uintptr_t begin, uintptr_t end) {
    uintptr_t found = 0;
    size_t count = 0;
    for (uintptr_t p = begin; p + 7 <= end; ++p) {
        const auto* b = reinterpret_cast<const uint8_t*>(p);
        if (b[0] == 0x48 && b[1] == 0x89 && b[2] == 0x05) {
            found = zzz::patterns::DecodeRipRelativeAddress(p, 3, 7);
            if (++count > 1) return 0;
        }
    }
    return count == 1 ? found : 0;
}

uintptr_t FindLegacyGlobalBefore(uintptr_t target) {
    uintptr_t found = 0;
    size_t count = 0;
    const uintptr_t begin = target > 0x60 ? target - 0x60 : target;
    for (uintptr_t p = begin; p + 8 <= target; ++p) {
        const auto* b = reinterpret_cast<const uint8_t*>(p);
        if (b[0] == 0x48 && b[1] == 0x83 && b[2] == 0x3D && b[7] == 0x00) {
            found = zzz::patterns::DecodeRipRelativeAddress(p, 3, 8);
            if (++count > 1) return 0;
        }
    }
    return count == 1 ? found : 0;
}

uintptr_t ResolvePlayerSettingsGlobal(uintptr_t target, HMODULE unity) {
    const uintptr_t callsite = target - 8;
    if (*reinterpret_cast<const uint8_t*>(callsite) != 0xE8) return 0;
    const uintptr_t getter = zzz::patterns::DecodeRel32Target(callsite, 1, 5);
    if (!zzz::patterns::AddressInSection(unity, ".text", getter)) return 0;

    const auto* g = reinterpret_cast<const uint8_t*>(getter);
    if (g[0] != 0x33 || g[1] != 0xC9 || g[2] != 0xE9) return 0;
    const uintptr_t inner = zzz::patterns::DecodeRel32Target(getter + 2, 1, 5);
    if (!zzz::patterns::AddressInSection(unity, ".text", inner)) return 0;

    const auto* in = reinterpret_cast<const uint8_t*>(inner);
    if (in[0] != 0x48 || in[1] != 0x63 || in[2] != 0xC1 ||
        in[3] != 0x48 || in[4] != 0x8D || in[5] != 0x0D) return 0;
    return zzz::patterns::DecodeRipRelativeAddress(inner + 3, 3, 7);
}

LocatedInput Locate(HMODULE unity, bool& ambiguous) {
    ambiguous = false;
    LocatedInput out{};
    if (!unity) return out;

    size_t matches = 0;
    const uintptr_t hit = zzz::patterns::FindUniquePattern(
        unity, ".text", kInputLocatorPattern, &matches);
    if (!hit) {
        ambiguous = matches >= 2;
        return out;
    }

    const uintptr_t target = hit + kTargetOffsetFromPattern;
    uint32_t field = 0;
    uintptr_t branchTarget = 0;
    if (!LooksLikeOriginal(target, field, branchTarget)) return out;
    if (!zzz::patterns::AddressInSection(unity, ".text", branchTarget)) return out;

    const auto* legacyDecision = reinterpret_cast<const uint8_t*>(branchTarget);
    if (legacyDecision[0] != 0xE8 ||
        legacyDecision[5] != 0x44 || legacyDecision[6] != 0x38 ||
        legacyDecision[7] != 0xB8 ||
        *reinterpret_cast<const uint32_t*>(legacyDecision + 8) != field + 1) {
        return out;
    }

    const uintptr_t legacyGlobal = FindLegacyGlobalBefore(target);
    const uintptr_t newGlobal = FindSingleRipStore(target + 13, branchTarget);
    const uintptr_t settingsGlobal = ResolvePlayerSettingsGlobal(target, unity);
    if (!legacyGlobal || !newGlobal || !settingsGlobal ||
        !zzz::patterns::AddressInModule(unity, legacyGlobal) ||
        !zzz::patterns::AddressInModule(unity, newGlobal) ||
        !zzz::patterns::AddressInModule(unity, settingsGlobal)) {
        return out;
    }

    out.target = target;
    out.legacyManagerGlobal = legacyGlobal;
    out.newManagerGlobal = newGlobal;
    out.playerSettingsGlobal = settingsGlobal;
    out.playerSettingsField = field;
    out.legacyBranchTarget = branchTarget;
    return out;
}

void MakePatch(const LocatedInput& loc, uint8_t out[13]) {
    out[0] = 0x66; out[1] = 0x44; out[2] = 0x89; out[3] = 0xB8;
    std::memcpy(out + 4, &loc.playerSettingsField, 4);
    out[8] = 0xE9;
    const int32_t rel = static_cast<int32_t>(
        static_cast<intptr_t>(loc.legacyBranchTarget) -
        static_cast<intptr_t>(loc.target + 13));
    std::memcpy(out + 9, &rel, 4);
}

wchar_t FoldAscii(wchar_t c) {
    return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + 32) : c;
}

bool IsUnityPlayerName(const ZzzUnicodeString* name) {
    static constexpr wchar_t kName[] = L"UnityPlayer.dll";
    constexpr size_t kChars = (sizeof(kName) / sizeof(kName[0])) - 1;
    if (!name || !name->Buffer || name->Length != kChars * sizeof(wchar_t)) return false;
    for (size_t i = 0; i < kChars; ++i)
        if (FoldAscii(name->Buffer[i]) != FoldAscii(kName[i])) return false;
    return true;
}

const char* EarlyStateName(EarlyState s) {
    switch (s) {
        case EarlyState::Idle: return "IDLE";
        case EarlyState::Armed: return "ARMED";
        case EarlyState::Patching: return "PATCHING";
        case EarlyState::Patched: return "PATCHED";
        case EarlyState::TooLate: return "TOO_LATE";
        case EarlyState::SignatureMismatch: return "SIGNATURE_MISMATCH";
        case EarlyState::PatternAmbiguous: return "PATTERN_AMBIGUOUS";
        case EarlyState::ProtectFailed: return "PROTECT_FAILED";
        case EarlyState::NotificationUnavailable: return "NOTIFICATION_UNAVAILABLE";
        case EarlyState::NotificationFailed: return "NOTIFICATION_FAILED";
        default: return "UNKNOWN";
    }
}

EarlyState TryPatchInputMode(HMODULE unity) {
    bool ambiguous = false;
    const LocatedInput loc = Locate(unity, ambiguous);
    if (!loc.target) return ambiguous ? EarlyState::PatternAmbiguous
                                      : EarlyState::SignatureMismatch;

    const uintptr_t base = reinterpret_cast<uintptr_t>(unity);
    g_legacyManagerGlobal.store(loc.legacyManagerGlobal, std::memory_order_release);
    g_newManagerGlobal.store(loc.newManagerGlobal, std::memory_order_release);
    g_playerSettingsGlobal.store(loc.playerSettingsGlobal, std::memory_order_release);
    g_playerSettingsField.store(loc.playerSettingsField, std::memory_order_release);

    LOG_INFO("LTR_INPUT_LOCATED source=PATTERN patchRva=0x%llX field=0x%X legacyGlobalRva=0x%llX newGlobalRva=0x%llX settingsGlobalRva=0x%llX",
        static_cast<unsigned long long>(loc.target - base),
        static_cast<unsigned>(loc.playerSettingsField),
        static_cast<unsigned long long>(loc.legacyManagerGlobal - base),
        static_cast<unsigned long long>(loc.newManagerGlobal - base),
        static_cast<unsigned long long>(loc.playerSettingsGlobal - base));

    const uintptr_t legacyManager = *reinterpret_cast<const uintptr_t*>(loc.legacyManagerGlobal);
    const uintptr_t newManager = *reinterpret_cast<const uintptr_t*>(loc.newManagerGlobal);
    if (legacyManager || newManager) return EarlyState::TooLate;

    uint32_t field = 0;
    uintptr_t branch = 0;
    if (!LooksLikeOriginal(loc.target, field, branch) ||
        field != loc.playerSettingsField || branch != loc.legacyBranchTarget) {
        return EarlyState::SignatureMismatch;
    }

    uint8_t patch[13]{};
    MakePatch(loc, patch);
    if (!WriteBytes(loc.target, patch, sizeof(patch))) return EarlyState::ProtectFailed;
    return EarlyState::Patched;
}

EarlyState TryPatchInputModeOnce(HMODULE unity) {
    EarlyState expected = EarlyState::Armed;
    if (!g_earlyState.compare_exchange_strong(
            expected, EarlyState::Patching, std::memory_order_acq_rel)) {
        return expected;
    }

    const EarlyState result = TryPatchInputMode(unity);
    g_earlyState.store(result, std::memory_order_release);
    return result;
}

void NTAPI DllNotification(ULONG reason,
                           const ZzzLdrDllNotificationData* data,
                           void*) {
    if (reason != 1 || !data || !IsUnityPlayerName(data->Loaded.BaseDllName)) return;
    TryPatchInputModeOnce(reinterpret_cast<HMODULE>(data->Loaded.DllBase));
}

HMODULE WaitForModule(const wchar_t* moduleName, DWORD timeoutMs) {
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        if (HMODULE mod = GetModuleHandleW(moduleName)) return mod;
        if (GetTickCount64() - start >= timeoutMs) return nullptr;
        Sleep(10);
    }
}

void UnregisterNotification() {
    if (g_dllNotifyCookie && g_ldrUnregister) {
        g_ldrUnregister(g_dllNotifyCookie);
        g_dllNotifyCookie = nullptr;
    }
}

}  // namespace

bool ArmEarlyRestore() {
    EarlyState expected = EarlyState::Idle;
    if (!g_earlyState.compare_exchange_strong(expected, EarlyState::Armed,
            std::memory_order_acq_rel)) {
        const EarlyState current = g_earlyState.load(std::memory_order_acquire);
        return current == EarlyState::Armed || current == EarlyState::Patching ||
               current == EarlyState::Patched;
    }

    LOG_INFO("LTR_INPUT_MODE_SWITCH_ARMING target=OLD_ONLY locator=PATTERN_ONLY");
    if (HMODULE unity = GetModuleHandleW(L"UnityPlayer.dll")) {
        const EarlyState state = TryPatchInputModeOnce(unity);
        return state == EarlyState::Patching || state == EarlyState::Patched;
    }

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) {
        g_earlyState.store(EarlyState::NotificationUnavailable, std::memory_order_release);
        return false;
    }
    auto reg = reinterpret_cast<LdrRegisterDllNotificationFn>(
        GetProcAddress(ntdll, "LdrRegisterDllNotification"));
    g_ldrUnregister = reinterpret_cast<LdrUnregisterDllNotificationFn>(
        GetProcAddress(ntdll, "LdrUnregisterDllNotification"));
    if (!reg || !g_ldrUnregister) {
        g_earlyState.store(EarlyState::NotificationUnavailable, std::memory_order_release);
        return false;
    }

    const NtStatus status = reg(0, &DllNotification, nullptr, &g_dllNotifyCookie);
    if (status < 0 || !g_dllNotifyCookie) {
        g_earlyState.store(EarlyState::NotificationFailed, std::memory_order_release);
        return false;
    }

    if (HMODULE unity = GetModuleHandleW(L"UnityPlayer.dll")) {
        const EarlyState state = TryPatchInputModeOnce(unity);
        if (state != EarlyState::Armed && state != EarlyState::Patching &&
            state != EarlyState::Patched) {
            return false;
        }
    }
    return true;
}

bool FinalizeRuntime(DWORD timeout_ms) {
    HMODULE unity = WaitForModule(L"UnityPlayer.dll", timeout_ms);
    if (!unity) return false;

    EarlyState state = g_earlyState.load(std::memory_order_acquire);
    if (state == EarlyState::Armed)
        state = TryPatchInputModeOnce(unity);

    const ULONGLONG start = GetTickCount64();
    while (state == EarlyState::Patching && GetTickCount64() - start < timeout_ms) {
        Sleep(1);
        state = g_earlyState.load(std::memory_order_acquire);
    }

    LOG_INFO("LTR_INPUT_MODE_RESULT UnityPlayer=%p state=%s",
        reinterpret_cast<void*>(unity), EarlyStateName(state));
    if (state != EarlyState::Patched) return false;

    const uintptr_t legacyGlobal = g_legacyManagerGlobal.load(std::memory_order_acquire);
    const uintptr_t newGlobal = g_newManagerGlobal.load(std::memory_order_acquire);
    const uintptr_t settingsGlobal = g_playerSettingsGlobal.load(std::memory_order_acquire);
    const uint32_t fieldOffset = g_playerSettingsField.load(std::memory_order_acquire);
    if (!legacyGlobal || !newGlobal || !settingsGlobal || !fieldOffset) return false;

    uintptr_t legacyManager = 0;
    const ULONGLONG managerStart = GetTickCount64();
    while (GetTickCount64() - managerStart < timeout_ms) {
        legacyManager = *reinterpret_cast<const uintptr_t*>(legacyGlobal);
        if (legacyManager) break;
        Sleep(1);
    }
    if (!legacyManager) return false;

    const uintptr_t newManager = *reinterpret_cast<const uintptr_t*>(newGlobal);
    const uintptr_t settings = *reinterpret_cast<const uintptr_t*>(settingsGlobal);
    if (!settings) return false;
    const uint8_t newBackends = *reinterpret_cast<const uint8_t*>(settings + fieldOffset);
    const uint8_t disableOld = *reinterpret_cast<const uint8_t*>(settings + fieldOffset + 1);

    LOG_INFO("LTR_ACTIVE_INPUT_MODE settings=%p fields=0x%X/0x%X newBackends=%u disableOld=%u newManager=%p",
        reinterpret_cast<void*>(settings), static_cast<unsigned>(fieldOffset),
        static_cast<unsigned>(fieldOffset + 1), static_cast<unsigned>(newBackends),
        static_cast<unsigned>(disableOld), reinterpret_cast<void*>(newManager));
    if (newBackends != 0 || disableOld != 0 || newManager != 0) return false;

    const uintptr_t vtable = *reinterpret_cast<const uintptr_t*>(legacyManager);
    if (!vtable || !zzz::patterns::AddressInModule(unity, vtable)) return false;

    LOG_INFO("LTR_MANAGER_READY manager=%p vtable=%p validation=SEMANTIC",
        reinterpret_cast<void*>(legacyManager), reinterpret_cast<void*>(vtable));

    UnregisterNotification();
    LOG_INFO("LTR_NATIVE_READY activeInput=OLD_ONLY locator=PATTERN_ONLY newManager=OFF legacyManager=ON");
    return true;
}

}  // namespace zzz::legacy
