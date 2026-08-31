// launcher/main.cpp — minimal fail-closed launcher for zzz-touch-unlocker.
#include <windows.h>

#include <cstdio>
#include <string>

namespace {

constexpr wchar_t kDefaultGamePath[] =
    L"C:\\Program Files\\miHoYo Launcher\\games\\ZenlessZoneZero Game\\ZenlessZoneZero.exe";

struct Options {
    std::wstring game = kDefaultGamePath;
};

void PrintUsage() {
    wprintf(L"zzz-touch-unlocker.exe - runtime native touch restore\n\n");
    wprintf(L"Usage:\n");
    wprintf(L"  zzz-touch-unlocker.exe\n");
    wprintf(L"  zzz-touch-unlocker.exe --game \"C:\\path\\ZenlessZoneZero.exe\"\n\n");
    wprintf(L"Default game:\n  %ls\n", kDefaultGamePath);
}

bool ParseArgs(int argc, wchar_t** argv, Options& out) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--game") {
            if (i + 1 >= argc) return false;
            out.game = argv[++i];
            continue;
        }
        if (arg == L"--help" || arg == L"-h" || arg == L"/?") {
            PrintUsage();
            ExitProcess(0);
        }
        wprintf(L"ERROR: unknown argument: %ls\n", arg.c_str());
        return false;
    }
    return true;
}

std::wstring GetExeDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p = path;
    const auto pos = p.find_last_of(L'\\');
    return pos == std::wstring::npos ? std::wstring() : p.substr(0, pos);
}

std::wstring GetDllPath() {
    return GetExeDir() + L"\\zzz-touch-hook.dll";
}

bool FileExists(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool InjectDll(HANDLE process, const std::wstring& dllPath) {
    if (!process) return false;

    const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    LPVOID remote = VirtualAllocEx(process, nullptr, bytes,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        wprintf(L"VirtualAllocEx failed: %lu\n", GetLastError());
        return false;
    }

    if (!WriteProcessMemory(process, remote, dllPath.c_str(), bytes, nullptr)) {
        wprintf(L"WriteProcessMemory failed: %lu\n", GetLastError());
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return false;
    }

    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        kernel32 ? GetProcAddress(kernel32, "LoadLibraryW") : nullptr);
    if (!loadLibrary) {
        wprintf(L"GetProcAddress(LoadLibraryW) failed: %lu\n", GetLastError());
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return false;
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary,
                                       remote, 0, nullptr);
    if (!thread) {
        wprintf(L"CreateRemoteThread failed: %lu\n", GetLastError());
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return false;
    }

    const DWORD wait = WaitForSingleObject(thread, 15000);
    if (wait != WAIT_OBJECT_0) {
        wprintf(L"[inject] LoadLibraryW wait failed/timeout: %lu\n", wait);
        // The remote thread may still be reading the path; leave the allocation
        // intact. The suspended target will be terminated by the caller.
        CloseHandle(thread);
        return false;
    }

    DWORD code = 0;
    const bool gotExit = GetExitCodeThread(thread, &code) != FALSE;
    CloseHandle(thread);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);

    if (!gotExit || code == 0 || code == STILL_ACTIVE) {
        wprintf(L"[inject] LoadLibraryW failed, exit=0x%08lX gle=%lu\n",
                code, GetLastError());
        return false;
    }

    wprintf(L"[inject] LoadLibraryW succeeded\n");
    return true;
}

bool LaunchSuspendedAndInject(const std::wstring& gamePath,
                              const std::wstring& dllPath) {
    std::wstring cmdline = L"\"" + gamePath + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    wprintf(L"[launch] %ls\n", cmdline.c_str());
    if (!CreateProcessW(gamePath.c_str(), cmdline.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        wprintf(L"CreateProcessW failed: %lu\n", GetLastError());
        return false;
    }
    wprintf(L"[launch] pid=%lu suspended\n", pi.dwProcessId);

    auto fail = [&](const wchar_t* reason) {
        wprintf(L"[launch] %ls; terminating suspended game\n", reason);
        TerminateProcess(pi.hProcess, ERROR_DLL_INIT_FAILED);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return false;
    };

    wchar_t readyName[128]{};
    swprintf_s(readyName, L"Local\\ZZZTouchHookReady_%lu", pi.dwProcessId);
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, readyName);
    if (!ready) return fail(L"ready event creation failed");

    if (!InjectDll(pi.hProcess, dllPath)) {
        CloseHandle(ready);
        return fail(L"DLL injection failed");
    }

    const DWORD wr = WaitForSingleObject(ready, 10000);
    CloseHandle(ready);
    if (wr != WAIT_OBJECT_0) return fail(L"hook bootstrap timeout/error");
    wprintf(L"[launch] bootstrap ready: yes\n");

    if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1))
        return fail(L"ResumeThread failed");

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    Options opt;
    if (!ParseArgs(argc, argv, opt)) {
        PrintUsage();
        return 1;
    }

    const std::wstring dllPath = GetDllPath();
    if (!FileExists(dllPath)) {
        wprintf(L"DLL not found: %ls\n", dllPath.c_str());
        wprintf(L"Build the project first (scripts\\build.bat).\n");
        return 2;
    }
    if (!FileExists(opt.game)) {
        wprintf(L"Game executable not found: %ls\n", opt.game.c_str());
        wprintf(L"Use --game \"...\\ZenlessZoneZero.exe\" to override the default path.\n");
        return 1;
    }

    wprintf(L"zzz-touch-unlocker\n");
    wprintf(L"  game    = %ls\n", opt.game.c_str());
    wprintf(L"  locator = PATTERN_ONLY\n");
    return LaunchSuspendedAndInject(opt.game, dllPath) ? 0 : 3;
}
