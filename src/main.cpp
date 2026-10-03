#include "explorer/app.hpp"
#include "explorer/shell_operations.hpp"
#include <shellapi.h>
#include <filesystem>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    int count = 0;
    auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return 2;
    bool headless = false;
    std::wstring location;
    std::filesystem::path report = L"headless-smoke.json";
    for (int i = 1; i < count; ++i) {
        const std::wstring arg = arguments[i];
        if (arg == L"--headless-smoke") headless = true;
        else if (arg == L"--report" && i + 1 < count) report = arguments[++i];
        else if (arg == L"--path" && i + 1 < count) location = arguments[++i];
        else if (arg.starts_with(L"--")) { LocalFree(arguments); return 2; }
        else location = arg;
    }
    LocalFree(arguments);
    const auto hr = OleInitialize(nullptr);
    if (FAILED(hr)) return 3;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    auto app = new explorer::ExplorerApp(instance, headless);
    const auto created = app->create(location);
    int result = 0;
    if (FAILED(created)) {
        if (!headless) MessageBoxW(nullptr, explorer::hresultMessage(created).c_str(), L"Windows Explorer could not start", MB_OK | MB_ICONERROR);
        result = 4;
    } else result = headless ? app->headlessSmoke(report) : app->run(showCommand);
    explorer::ShellOperations::flushClipboardIfOwned();
    // Native context-menu Copy uses its own data object. Flush it only when
    // the clipboard belongs to this process; leave other applications alone.
    DWORD clipboardProcess = 0;
    if (auto owner = GetClipboardOwner()) GetWindowThreadProcessId(owner, &clipboardProcess);
    if (clipboardProcess == GetCurrentProcessId()) OleFlushClipboard();
    app->Release();
    OleUninitialize();
    return result;
}
