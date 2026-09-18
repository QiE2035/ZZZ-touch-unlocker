// Standalone Windows regression test: compile with -municode under MinGW.
#define wmain LauncherMainForTest
#include "../launcher/main.cpp"
#undef wmain
#include <cstdlib>

void Check(bool condition) {
    if (!condition) { fputs("FAIL\n", stderr); std::exit(1); }
}

const std::vector<std::wstring> kForwarded = {
    L"-screen-width", L"1920", L"-screen-height", L"1080",
    L"-screen-fullscreen", L"0", L"-monitor", L"1",
    L"", L"with spaces", L"with\ttab", L"a\"b", L"C:\\path with spaces\\",
    L"backslash\\\"quote", L"\\\\", L"\u4e2d\u6587", L"&|<>^%", L"-m", L"--help"
};

bool Parse(std::vector<std::wstring> args, Options& options) {
    std::vector<wchar_t*> pointers;
    for (auto& arg : args) pointers.push_back(arg.data());
    return ParseArgs(static_cast<int>(pointers.size()), pointers.data(), options);
}

int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && std::wstring(argv[1]) == L"--child") {
        Check(argc == static_cast<int>(kForwarded.size()) + 2);
        for (size_t i = 0; i < kForwarded.size(); ++i)
            Check(argv[i + 2] == kForwarded[i]);
        return 0;
    }
    Options options;
    Check(Parse({L"launcher", L"-m", L"0", L"-screen-width", L"1920",
                 L"-screen-height", L"1080", L"-screen-fullscreen", L"0",
                 L"-monitor", L"1"}, options));
    Check(options.mode == 0);
    Check(options.gameArgs == std::vector<std::wstring>(kForwarded.begin(), kForwarded.begin() + 8));
    options = Options{};
    Check(Parse({L"launcher", L"-monitor", L"1", L"--game", L"C:\\game path\\game.exe",
                 L"--mode", L"0", L"--", L"-m", L"1", L"--help", L""}, options));
    Check(options.mode == 0 && options.game == L"C:\\game path\\game.exe");
    Check(options.gameArgs == std::vector<std::wstring>({L"-monitor", L"1", L"-m", L"1", L"--help", L""}));
    options = Options{};
    Check(Parse({L"launcher"}, options) && options.mode == 1 && options.gameArgs.empty());
    for (auto args : {std::vector<std::wstring>{L"launcher", L"-m"},
                     {L"launcher", L"--mode", L"2"}, {L"launcher", L"--game"}}) {
        options = Options{};
        Check(!Parse(args, options));
    }
    wchar_t exe[MAX_PATH]{};
    Check(GetModuleFileNameW(nullptr, exe, MAX_PATH) != 0);
    auto args = kForwarded;
    args.insert(args.begin(), L"--child");
    auto command = BuildGameCommandLine(exe, args);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    Check(CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi));
    Check(WaitForSingleObject(pi.hProcess, 10000) == WAIT_OBJECT_0);
    DWORD code = 1;
    Check(GetExitCodeProcess(pi.hProcess, &code) && code == 0);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    puts("PASS: option parsing and actual child-process argument round trip");
    return 0;
}
