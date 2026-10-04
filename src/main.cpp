#include "explorer/app.hpp"
#include "explorer/shell_operations.hpp"
#include "explorer/theme.hpp"
#include "explorer/headless_crash.hpp"
#include <shellapi.h>
#include <filesystem>
#include <cstdio>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    int count = 0;
    auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return 2;
    bool headless = false;
    bool visual = false;
    bool benchmark = false;
    bool installedRibbon = false;
    explorer::ThemeMode theme = explorer::ThemeMode::Auto;
    explorer::PrivateDesktop privateDesktop;
    explorer::VisualScene scene;
    std::filesystem::path screenshot;
    std::filesystem::path crashDump;
    std::wstring location;
    std::filesystem::path report = L"headless-smoke.json";
    for (int i = 1; i < count; ++i) {
        const std::wstring arg = arguments[i];
        if (arg == L"--headless-smoke") headless = true;
        else if (arg == L"--headless-visual") { headless = true; visual = true; }
        else if (arg == L"--headless-benchmark") { headless = true; benchmark = true; }
        else if (arg == L"--installed-ribbon") installedRibbon = true;
        else if (arg == L"--report" && i + 1 < count) report = arguments[++i];
        else if (arg == L"--screenshot" && i + 1 < count) screenshot = arguments[++i];
        else if (arg == L"--crash-dump" && i + 1 < count) crashDump = arguments[++i];
        else if (arg == L"--page" && i + 1 < count) scene.page = arguments[++i];
        else if ((arg == L"--select" || arg == L"--select-shell") && i + 1 < count) scene.select = arguments[++i];
        else if (arg == L"--theme" && i + 1 < count) {
            const std::wstring value = arguments[++i];
            if (_wcsicmp(value.c_str(), L"Auto") == 0) theme = explorer::ThemeMode::Auto;
            else if (_wcsicmp(value.c_str(), L"Light") == 0) theme = explorer::ThemeMode::Light;
            else if (_wcsicmp(value.c_str(), L"Dark") == 0) theme = explorer::ThemeMode::Dark;
            else { LocalFree(arguments); return 2; }
        }
        else if (arg == L"--details") scene.details = true;
        else if (arg == L"--collapsed") scene.collapsed = true;
        else if ((arg == L"--width" || arg == L"--height" || arg == L"--dpi") && i + 1 < count) {
            wchar_t* end = nullptr;
            const auto value = wcstoul(arguments[++i], &end, 10);
            if (!end || *end || value < 96 || value > 8192) { LocalFree(arguments); return 2; }
            if (arg == L"--width") scene.width = static_cast<int>(value);
            else if (arg == L"--height") scene.height = static_cast<int>(value);
            else scene.dpi = static_cast<UINT>(value);
        }
        else if (arg == L"--view" && i + 1 < count) {
            const std::wstring view = arguments[++i];
            if (_wcsicmp(view.c_str(), L"Default") == 0) { scene.nativeView = true; continue; }
            const wchar_t* names[]{L"ExtraLargeIcons", L"LargeIcons", L"MediumIcons", L"SmallIcons", L"List", L"Details", L"Tiles", L"Content"};
            bool found = false;
            for (int index = 0; index < 8; ++index) if (_wcsicmp(view.c_str(), names[index]) == 0) {
                scene.nativeView = false;
                scene.view = static_cast<explorer::ViewMode>(index); found = true; break;
            }
            if (!found) { LocalFree(arguments); return 2; }
        }
        else if (arg == L"--path" && i + 1 < count) location = arguments[++i];
        else if (arg.starts_with(L"--")) { LocalFree(arguments); return 2; }
        else location = arg;
    }
    LocalFree(arguments);
    if (visual && (screenshot.empty() || benchmark)) return 2;
    if (installedRibbon && !headless) return 2;
    if (!crashDump.empty()) {
        const auto diagnostic = explorer::initializeHeadlessCrashDump(headless, crashDump);
        if (FAILED(diagnostic)) {
            std::fprintf(stderr, "Headless crash diagnostic initialization failed (HRESULT=0x%08lX).\n",
                static_cast<unsigned long>(diagnostic));
            return 2;
        }
    }
    if (headless && (FAILED(privateDesktop.initialize()) || FAILED(privateDesktop.verifyIsolation()))) return 5;
    if (FAILED(explorer::initializeProcessTheme(theme))) return 6;
    const auto hr = OleInitialize(nullptr);
    if (FAILED(hr)) return 3;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    auto app = new explorer::ExplorerApp(instance, headless,
        (installedRibbon || !headless) ? explorer::RibbonLayout::InstalledWindows10 : explorer::RibbonLayout::Authored);
    const auto created = app->create(location);
    int result = 0;
    if (FAILED(created)) {
        if (!headless) MessageBoxW(nullptr, explorer::hresultMessage(created).c_str(), L"Windows Explorer could not start", MB_OK | MB_ICONERROR);
        else std::fprintf(stderr, "Headless initialization failed (HRESULT=0x%08lX).\n",
            static_cast<unsigned long>(created));
        result = 4;
    } else result = visual ? app->headlessVisual(privateDesktop, std::filesystem::absolute(screenshot),
            std::filesystem::absolute(report), scene) : benchmark ? app->headlessBenchmark(std::filesystem::absolute(report)) : headless ? app->headlessSmoke(report) : app->run(showCommand);
    // The browser's site/filter retain the COM host. Destroy the owned frame
    // while this STA is alive so WM_DESTROY breaks that cycle before Release.
    // A hidden capture returns without running the normal WM_CLOSE loop.
    if (const auto frame = app->window(); IsWindow(frame) &&
        GetWindowLongPtrW(frame, GWLP_USERDATA) == reinterpret_cast<LONG_PTR>(app)) {
        if (!DestroyWindow(frame) && !result) result = 7;
    }
    if (FAILED(app->shutdownStatus())) {
        std::fprintf(stderr, "Native worker shutdown failed (HRESULT=0x%08lX).\n",
            static_cast<unsigned long>(app->shutdownStatus()));
        // A timed-out provider can still call its creator STA. Preserve that
        // apartment and its sites until process exit instead of releasing them
        // underneath a live marshaled call.
        std::fflush(stderr);
        ExitProcess(8); // Stop read-only workers before C++ static teardown.
    }
    explorer::ShellOperations::flushClipboardIfOwned();
    // Native context-menu Copy uses its own data object. Flush it only when
    // the clipboard belongs to this process; leave other applications alone.
    DWORD clipboardProcess = 0;
    if (auto owner = GetClipboardOwner()) GetWindowThreadProcessId(owner, &clipboardProcess);
    if (clipboardProcess == GetCurrentProcessId()) OleFlushClipboard();
    app->Release();
    OleUninitialize();
    if (headless && FAILED(privateDesktop.verifyIsolation())) result = 5;
    return result;
}
