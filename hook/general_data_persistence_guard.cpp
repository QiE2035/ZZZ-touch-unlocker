#include "general_data_persistence_guard.h"

#include "layout_platform_override.h"
#include "logger.h"
#include "patterns.h"

#include <MinHook.h>
#include <windows.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

#include <atomic>
#include <cstdint>

namespace zzz::persistence_guard {
namespace {

constexpr const char* kSaveContextPattern =
    "48 89 45 E8 E8 ?? ?? ?? ?? 48 8B 4D E8 48 8B 51 20 "
    "49 89 C0 E8 ?? ?? ?? ?? 48 8B 75 E8 C6 46 41 00";
constexpr size_t kSerializeCallOffset = 4;
constexpr int32_t kUncapturedLayout = (-2147483647 - 1);

using SerializeObjectFn = void* (__fastcall*)(void* object);
SerializeObjectFn g_originalSerialize = nullptr;
std::atomic<bool> g_ready{false};
std::atomic<uintptr_t> g_expectedReturn{0};
std::atomic<int32_t> g_persistedLayout{kUncapturedLayout};
thread_local bool g_inHook = false;

struct ReentryGuard {
    ~ReentryGuard() { g_inHook = false; }
};

struct FieldRestoreGuard {
    int32_t* field;
    int32_t liveValue;
    ~FieldRestoreGuard() {
        if (field) *field = liveValue;
    }
};

HMODULE WaitForModule(const wchar_t* moduleName, DWORD timeoutMs) {
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        if (HMODULE mod = GetModuleHandleW(moduleName)) return mod;
        if (GetTickCount64() - start >= timeoutMs) return nullptr;
        Sleep(10);
    }
}

struct LocatedSavePath {
    uintptr_t callsite = 0;
    uintptr_t serializer = 0;
    uintptr_t returnAddress = 0;
};

LocatedSavePath LocateSavePath(HMODULE ga) {
    LocatedSavePath out{};
    if (!ga) return out;

    size_t matches = 0;
    const uintptr_t context = zzz::patterns::FindUniquePattern(
        ga, "il2cpp", kSaveContextPattern, &matches);
    if (!context) {
        LOG_ERROR("PERSIST_GUARD locator failed matches=%llu",
            static_cast<unsigned long long>(matches));
        return out;
    }

    const uintptr_t callsite = context + kSerializeCallOffset;
    if (*reinterpret_cast<const uint8_t*>(callsite) != 0xE8) return out;
    const uintptr_t serializer = zzz::patterns::DecodeRel32Target(callsite, 1, 5);
    if (!zzz::patterns::AddressInSection(ga, "il2cpp", serializer)) return out;

    out.callsite = callsite;
    out.serializer = serializer;
    out.returnAddress = callsite + 5;
    return out;
}

void* __fastcall HookSerializeObject(void* object) {
    if (!g_originalSerialize) return nullptr;
    if (g_inHook || !object) return g_originalSerialize(object);

#if defined(_MSC_VER)
    const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
#else
    const uintptr_t ret = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#endif
    if (ret != g_expectedReturn.load(std::memory_order_acquire))
        return g_originalSerialize(object);

    const uint32_t fieldOffset = zzz::layout_override::LayoutFieldOffset();
    if (!fieldOffset) return g_originalSerialize(object);

    g_inHook = true;
    ReentryGuard reentryGuard;

    auto* field = reinterpret_cast<int32_t*>(
        reinterpret_cast<uint8_t*>(object) + fieldOffset);
    const int32_t liveValue = *field;

    int32_t persistValue = g_persistedLayout.load(std::memory_order_acquire);
    if (persistValue == kUncapturedLayout) {
        int32_t expected = kUncapturedLayout;
        if (g_persistedLayout.compare_exchange_strong(
                expected, liveValue, std::memory_order_acq_rel)) {
            persistValue = liveValue;
            if (liveValue == 1) {
                LOG_WARN("PERSIST_GUARD_CAPTURE firstObservedLayout=1; baseline is already MOBILE, verify update ordering");
            } else {
                LOG_INFO("PERSIST_GUARD_CAPTURE firstObservedLayout=%d source=GENERAL_DATA_SAVE_PATH",
                    liveValue);
            }
        } else {
            persistValue = expected;
        }
    }

    *field = persistValue;
    FieldRestoreGuard restoreGuard{field, liveValue};
    return g_originalSerialize(object);
}

}  // namespace

bool Install(DWORD timeout_ms) {
    if (g_ready.load(std::memory_order_acquire)) return true;
    HMODULE ga = WaitForModule(L"GameAssembly.dll", timeout_ms);
    if (!ga) return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(ga);

    const LocatedSavePath loc = LocateSavePath(ga);
    if (!loc.serializer || !loc.returnAddress) return false;
    g_expectedReturn.store(loc.returnAddress, std::memory_order_release);

    LOG_INFO("PERSIST_GUARD_LOCATED source=PATTERN saveRva=0x%llX serializerRva=0x%llX returnRva=0x%llX fieldOffset=0x%X",
        static_cast<unsigned long long>(loc.callsite - base),
        static_cast<unsigned long long>(loc.serializer - base),
        static_cast<unsigned long long>(loc.returnAddress - base),
        static_cast<unsigned>(zzz::layout_override::LayoutFieldOffset()));

    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) return false;
    s = MH_CreateHook(reinterpret_cast<void*>(loc.serializer),
                      reinterpret_cast<void*>(&HookSerializeObject),
                      reinterpret_cast<void**>(&g_originalSerialize));
    if (s != MH_OK && s != MH_ERROR_ALREADY_CREATED) return false;
    s = MH_EnableHook(reinterpret_cast<void*>(loc.serializer));
    if (s != MH_OK && s != MH_ERROR_ENABLED) {
        MH_RemoveHook(reinterpret_cast<void*>(loc.serializer));
        return false;
    }

    g_ready.store(true, std::memory_order_release);
    LOG_INFO("PERSIST_GUARD_READY locator=PATTERN_ONLY scope=GENERAL_DATA_SERIALIZE_ONLY persistPolicy=FIRST_OBSERVED");
    return true;
}

}  // namespace zzz::persistence_guard
