#include "layout_platform_override.h"

#include "logger.h"
#include "patterns.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace zzz::layout_override {
namespace {

constexpr const char* kLayoutLocatorPattern =
    "E8 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ?? "
    "8B 40 ?? 85 C0 0F 4F F0 89 F0 48 83 C4 20 5B 5F 5E C3";
constexpr size_t kLayoutTargetOffsetFromPattern = 14;

struct LocatedLayout {
    uintptr_t target = 0;
    uint8_t fieldOffset = 0;
};

enum class PatchState : int {
    Idle = 0,
    Armed,
    Patching,
    Patched,
    SignatureMismatch,
    PatternAmbiguous,
    ProtectFailed,
    NotificationUnavailable,
    NotificationFailed,
};

std::atomic<PatchState> g_state{PatchState::Idle};
std::atomic<uintptr_t> g_applyAddress{0};
std::atomic<uint32_t> g_fieldOffset{0};
uint8_t g_layout = 1;  // Set before the notification is registered.
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

bool BytesEqual(uintptr_t address, const uint8_t* expected, size_t size) {
    return address && expected &&
        std::memcmp(reinterpret_cast<const void*>(address), expected, size) == 0;
}

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

void MakeOriginal(uint8_t field, uint8_t out[10]) {
    const uint8_t bytes[10] = {
        0x8B, 0x40, field, 0x85, 0xC0, 0x0F, 0x4F, 0xF0, 0x89, 0xF0
    };
    std::memcpy(out, bytes, sizeof(bytes));
}

void MakeLayout(uint8_t field, uint8_t out[10]) {
    const uint8_t bytes[10] = {
        0xC7, 0x40, field, g_layout, 0x00, 0x00, 0x00, 0x6A, g_layout, 0x58
    };
    std::memcpy(out, bytes, sizeof(bytes));
}

bool LooksLikeOriginal(uintptr_t target, uint8_t& field) {
    if (!target) return false;
    const auto* p = reinterpret_cast<const uint8_t*>(target);
    if (p[0] != 0x8B || p[1] != 0x40 || p[3] != 0x85 || p[4] != 0xC0 ||
        p[5] != 0x0F || p[6] != 0x4F || p[7] != 0xF0 ||
        p[8] != 0x89 || p[9] != 0xF0) return false;
    field = p[2];
    return field >= 0x20 && field <= 0x7C && (field % 4) == 0;
}

LocatedLayout Locate(HMODULE ga, bool& ambiguous) {
    ambiguous = false;
    LocatedLayout out{};
    if (!ga) return out;

    size_t matches = 0;
    const uintptr_t hit = zzz::patterns::FindUniquePattern(
        ga, "il2cpp", kLayoutLocatorPattern, &matches);
    if (!hit) {
        ambiguous = matches >= 2;
        return out;
    }

    const uintptr_t target = hit + kLayoutTargetOffsetFromPattern;
    uint8_t field = 0;
    if (!LooksLikeOriginal(target, field)) return out;

    out.target = target;
    out.fieldOffset = field;
    return out;
}

wchar_t FoldAscii(wchar_t c) {
    return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + 32) : c;
}

bool IsGameAssemblyName(const ZzzUnicodeString* name) {
    static constexpr wchar_t kName[] = L"GameAssembly.dll";
    constexpr size_t kChars = (sizeof(kName) / sizeof(kName[0])) - 1;
    if (!name || !name->Buffer || name->Length != kChars * sizeof(wchar_t)) return false;
    for (size_t i = 0; i < kChars; ++i)
        if (FoldAscii(name->Buffer[i]) != FoldAscii(kName[i])) return false;
    return true;
}

const char* StateName(PatchState state) {
    switch (state) {
        case PatchState::Idle: return "IDLE";
        case PatchState::Armed: return "ARMED";
        case PatchState::Patching: return "PATCHING";
        case PatchState::Patched: return "PATCHED";
        case PatchState::SignatureMismatch: return "SIGNATURE_MISMATCH";
        case PatchState::PatternAmbiguous: return "PATTERN_AMBIGUOUS";
        case PatchState::ProtectFailed: return "PROTECT_FAILED";
        case PatchState::NotificationUnavailable: return "NOTIFICATION_UNAVAILABLE";
        case PatchState::NotificationFailed: return "NOTIFICATION_FAILED";
        default: return "UNKNOWN";
    }
}

PatchState TryPatch(HMODULE ga) {
    if (!ga) return PatchState::SignatureMismatch;
    const uintptr_t base = reinterpret_cast<uintptr_t>(ga);

    bool ambiguous = false;
    const LocatedLayout loc = Locate(ga, ambiguous);
    if (!loc.target) return ambiguous ? PatchState::PatternAmbiguous
                                      : PatchState::SignatureMismatch;

    uint8_t original[10]{};
    uint8_t selected[10]{};
    MakeOriginal(loc.fieldOffset, original);
    MakeLayout(loc.fieldOffset, selected);

    g_applyAddress.store(loc.target, std::memory_order_release);
    g_fieldOffset.store(loc.fieldOffset, std::memory_order_release);

    LOG_INFO("UIR_LAYOUT_LOCATED source=PATTERN rva=0x%llX fieldOffset=0x%X",
        static_cast<unsigned long long>(loc.target - base),
        static_cast<unsigned>(loc.fieldOffset));

    if (!BytesEqual(loc.target, original, sizeof(original)))
        return PatchState::SignatureMismatch;
    if (!WriteBytes(loc.target, selected, sizeof(selected)))
        return PatchState::ProtectFailed;
    return PatchState::Patched;
}

PatchState TryPatchOnce(HMODULE ga) {
    PatchState expected = PatchState::Armed;
    if (!g_state.compare_exchange_strong(
            expected, PatchState::Patching, std::memory_order_acq_rel)) {
        return expected;
    }

    const PatchState result = TryPatch(ga);
    g_state.store(result, std::memory_order_release);
    return result;
}

void NTAPI DllNotification(ULONG reason,
                           const ZzzLdrDllNotificationData* data,
                           void*) {
    if (reason != 1 || !data || !IsGameAssemblyName(data->Loaded.BaseDllName)) return;
    TryPatchOnce(reinterpret_cast<HMODULE>(data->Loaded.DllBase));
}

HMODULE WaitForModule(const wchar_t* name, DWORD timeoutMs) {
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        if (HMODULE mod = GetModuleHandleW(name)) return mod;
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

bool ArmEarlyUi(int layout) {
    if (layout != 1 && layout != 2) return false;
    PatchState expected = PatchState::Idle;
    if (!g_state.compare_exchange_strong(expected, PatchState::Armed,
            std::memory_order_acq_rel)) {
        const PatchState current = g_state.load(std::memory_order_acquire);
        return current == PatchState::Armed || current == PatchState::Patching ||
               current == PatchState::Patched;
    }

    g_layout = static_cast<uint8_t>(layout);
    LOG_INFO("UIR_LAYOUT_OVERRIDE_ARMING target=%s locator=PATTERN_ONLY",
             layout == 1 ? "MOBILE" : "DESKTOP");
    if (HMODULE ga = GetModuleHandleW(L"GameAssembly.dll")) {
        const PatchState state = TryPatchOnce(ga);
        return state == PatchState::Patching || state == PatchState::Patched;
    }

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) {
        g_state.store(PatchState::NotificationUnavailable, std::memory_order_release);
        return false;
    }
    auto reg = reinterpret_cast<LdrRegisterDllNotificationFn>(
        GetProcAddress(ntdll, "LdrRegisterDllNotification"));
    g_ldrUnregister = reinterpret_cast<LdrUnregisterDllNotificationFn>(
        GetProcAddress(ntdll, "LdrUnregisterDllNotification"));
    if (!reg || !g_ldrUnregister) {
        g_state.store(PatchState::NotificationUnavailable, std::memory_order_release);
        return false;
    }

    const NtStatus status = reg(0, &DllNotification, nullptr, &g_dllNotifyCookie);
    if (status < 0 || !g_dllNotifyCookie) {
        g_state.store(PatchState::NotificationFailed, std::memory_order_release);
        return false;
    }

    if (HMODULE ga = GetModuleHandleW(L"GameAssembly.dll")) {
        const PatchState state = TryPatchOnce(ga);
        if (state != PatchState::Armed && state != PatchState::Patching &&
            state != PatchState::Patched) {
            return false;
        }
    }
    return true;
}

bool FinalizeRuntime(DWORD timeout_ms) {
    HMODULE ga = WaitForModule(L"GameAssembly.dll", timeout_ms);
    if (!ga) return false;

    PatchState state = g_state.load(std::memory_order_acquire);
    if (state == PatchState::Armed)
        state = TryPatchOnce(ga);

    const ULONGLONG start = GetTickCount64();
    while (state == PatchState::Patching && GetTickCount64() - start < timeout_ms) {
        Sleep(1);
        state = g_state.load(std::memory_order_acquire);
    }

    LOG_INFO("UIR_LAYOUT_OVERRIDE_RESULT GameAssembly=%p state=%s",
        reinterpret_cast<void*>(ga), StateName(state));
    if (state != PatchState::Patched) return false;

    const uintptr_t target = g_applyAddress.load(std::memory_order_acquire);
    const uint32_t field = g_fieldOffset.load(std::memory_order_acquire);
    uint8_t selected[10]{};
    MakeLayout(static_cast<uint8_t>(field), selected);
    if (!target || !field || !BytesEqual(target, selected, sizeof(selected))) return false;

    UnregisterNotification();
    LOG_INFO("UIR_LAYOUT_READY applyRva=0x%llX fieldOffset=0x%X locator=PATTERN_ONLY",
        static_cast<unsigned long long>(target - reinterpret_cast<uintptr_t>(ga)),
        static_cast<unsigned>(field));
    return true;
}

uint32_t LayoutFieldOffset() {
    return g_fieldOffset.load(std::memory_order_acquire);
}

}  // namespace zzz::layout_override
