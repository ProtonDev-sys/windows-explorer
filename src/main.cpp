#include "explorer/app.hpp"
#include "explorer/shell_operations.hpp"
#include "explorer/theme.hpp"
#include "explorer/headless_crash.hpp"
#include "explorer/startup_desktop.hpp"
#include "explorer/headless_window_isolation.hpp"
#include <shellapi.h>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <limits>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    LARGE_INTEGER startupFrequency{}, startupEntry{};
    const bool startupClock = QueryPerformanceFrequency(&startupFrequency) && startupFrequency.QuadPart > 0 &&
        QueryPerformanceCounter(&startupEntry);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    int count = 0;
    auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return 2;
    int controlExit=2;
    if(explorer::runHeadlessWindowControl(count,arguments,&controlExit)) {
        LocalFree(arguments);return controlExit;
    }
    bool headless = false;
    bool explicitHeadlessSmoke = false;
    bool headlessLibrarySmoke = false;
    bool libraryIncompatibleOptions = false;
    bool previewLowDesktopDiagnostic = false;
    bool startupDesktopComparison = false, startupDesktopChild = false;
    ULONG_PTR startupMapping = 0, startupParent = 0, startupArm = 0;
    std::filesystem::path startupDirectory;
    bool visual = false;
    bool benchmark = false;
    bool installedRibbon = false;
    bool sourceDocumentsLibrary = false;
    explorer::ThemeMode theme = explorer::ThemeMode::Auto;
    explorer::StartupDesktopChild startupContext;
    explorer::PrivateDesktop privateDesktop;
    explorer::VisualScene scene;
    std::filesystem::path screenshot;
    std::filesystem::path crashDump;
    std::wstring location;
    ULONG_PTR searchContextHandle = 0;
    std::filesystem::path report = L"headless-smoke.json";
    const auto parseHandle = [](const wchar_t* argument, ULONG_PTR& output) {
        const std::wstring value = argument;
        if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos) return false;
        errno = 0; wchar_t* end = nullptr;
        const auto parsed = wcstoull(value.c_str(), &end, 10);
        if (errno == ERANGE || !end || *end || !parsed || parsed >= (std::numeric_limits<ULONG_PTR>::max)()) return false;
        output = static_cast<ULONG_PTR>(parsed); return true;
    };
    for (int i = 1; i < count; ++i) {
        const std::wstring arg = arguments[i];
        if (arg == L"--screenshot" || arg == L"--page" || arg == L"--select" || arg == L"--select-shell" ||
            arg == L"--details" || arg == L"--collapsed" || arg == L"--open-search-date-menu" ||
            arg == L"--width" || arg == L"--height" || arg == L"--dpi" || arg == L"--view" || arg == L"--path")
            libraryIncompatibleOptions = true;
        if (arg == L"--headless-smoke") { headless = true; explicitHeadlessSmoke = true; }
        else if (arg == L"--headless-library-smoke") {
            if (headlessLibrarySmoke) { LocalFree(arguments); return 2; }
            headless = true; headlessLibrarySmoke = true;
        }
        else if (arg == L"--headless-startup-desktop-comparison" && i + 1 < count) {
            if (startupDesktopComparison) { LocalFree(arguments); return 2; }
            startupDesktopComparison = true; headless = true; startupDirectory = arguments[++i];
        }
        else if (arg == L"--headless-startup-desktop-child" && i + 3 < count) {
            if (startupDesktopChild || !parseHandle(arguments[i + 1], startupMapping) ||
                !parseHandle(arguments[i + 2], startupParent) || !parseHandle(arguments[i + 3], startupArm) ||
                (startupArm != 1 && startupArm != 2)) { LocalFree(arguments); return 2; }
            startupDesktopChild = true; i += 3;
        }
        else if (arg == L"--headless-preview-low-desktop-diagnostic") {
            if (previewLowDesktopDiagnostic) { LocalFree(arguments); return 2; }
            previewLowDesktopDiagnostic = true;
        }
        else if (arg == L"--headless-visual") { headless = true; visual = true; }
        else if (arg == L"--headless-benchmark") { headless = true; benchmark = true; }
        else if (arg == L"--installed-ribbon") installedRibbon = true;
        else if (arg == L"--report" && i + 1 < count) report = arguments[++i];
        else if (arg == L"--screenshot" && i + 1 < count) screenshot = arguments[++i];
        else if (arg == L"--crash-dump" && i + 1 < count) crashDump = arguments[++i];
        else if (arg == L"--page" && i + 1 < count) scene.page = arguments[++i];
        else if (arg == L"--source-documents-library") sourceDocumentsLibrary = true;
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
        else if (arg == L"--open-search-date-menu") scene.openSearchDateMenu = true;
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
        else if (arg == L"--search-context-handle" && i + 1 < count) {
            const std::wstring value = arguments[++i];
            if (searchContextHandle || value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos) { LocalFree(arguments); return 2; }
            errno = 0; wchar_t* end = nullptr;
            const auto parsed = wcstoull(value.c_str(), &end, 10);
            if (errno == ERANGE || !end || *end || !parsed || parsed > (std::numeric_limits<ULONG_PTR>::max)()) { LocalFree(arguments); return 2; }
            searchContextHandle = static_cast<ULONG_PTR>(parsed);
        }
        else if (arg == L"--path" && i + 1 < count) location = arguments[++i];
        else if (arg.starts_with(L"--")) { LocalFree(arguments); return 2; }
        else location = arg;
    }
    LocalFree(arguments);
    if (headlessLibrarySmoke && (!headless || explicitHeadlessSmoke || visual || benchmark ||
        startupDesktopComparison || startupDesktopChild || previewLowDesktopDiagnostic || searchContextHandle ||
        sourceDocumentsLibrary || !location.empty() || libraryIncompatibleOptions || theme != explorer::ThemeMode::Auto)) return 2;
    if (searchContextHandle && (!location.empty() || visual || benchmark)) return 2;
    if (previewLowDesktopDiagnostic && (!explicitHeadlessSmoke || visual || benchmark ||
        searchContextHandle || sourceDocumentsLibrary)) return 2;
    if ((startupDesktopComparison || startupDesktopChild) && (visual || benchmark || previewLowDesktopDiagnostic ||
        searchContextHandle || sourceDocumentsLibrary || !location.empty() || !screenshot.empty() || !crashDump.empty() ||
        !scene.select.empty() || scene.details || scene.collapsed || scene.openSearchDateMenu ||
        theme != explorer::ThemeMode::Auto)) return 2;
    if (startupDesktopComparison && (startupDesktopChild || explicitHeadlessSmoke || !startupDirectory.is_absolute())) return 2;
    if (startupDesktopChild && (!explicitHeadlessSmoke || !report.is_absolute())) return 2;
    if (startupDesktopComparison) {
        explorer::StartupDesktopComparisonReadback readback;
        const auto compared = explorer::runStartupDesktopComparison(startupDirectory, installedRibbon, readback);
        std::fprintf(stderr, "Startup comparison transport=0x%08lX guard/directory/desktop/executable/finish="
            "0x%08lX/0x%08lX/0x%08lX/0x%08lX/0x%08lX noninherit/inputUnchanged/sameExe=%u/%u/%u.\n",
            static_cast<ULONG>(compared), static_cast<ULONG>(readback.guard), static_cast<ULONG>(readback.directory),
            static_cast<ULONG>(readback.desktop), static_cast<ULONG>(readback.executable), static_cast<ULONG>(readback.finish),
            static_cast<unsigned>(readback.noninheritableDesktop), static_cast<unsigned>(readback.inputUnchanged),
            static_cast<unsigned>(readback.sameExecutable));
        bool childrenPassed = true;
        for (const auto& arm : readback.arms) {
            std::fprintf(stderr, "Startup comparison arm=%lu PID/TID=%lu/%lu create/packet/resume/wait/exit/preservation="
                "0x%08lX/0x%08lX/0x%08lX/0x%08lX/0x%08lX/0x%08lX kernelExited/timedOut=%u/%u exitCode=%lu ticks=%llu/%llu.\n",
                static_cast<DWORD>(arm.arm), arm.childPid, arm.childTid, static_cast<ULONG>(arm.create),
                static_cast<ULONG>(arm.packet), static_cast<ULONG>(arm.resume), static_cast<ULONG>(arm.wait),
                static_cast<ULONG>(arm.exitRead), static_cast<ULONG>(arm.preservation), static_cast<unsigned>(arm.kernelExited),
                static_cast<unsigned>(arm.timedOut), arm.exitCode, arm.resumeTick, arm.kernelExitTick);
            childrenPassed = childrenPassed && arm.kernelExited && !arm.timedOut && arm.exitCode == 0;
        }
        std::fflush(stderr);
        return FAILED(compared) ? 5 : childrenPassed ? 0 : 1;
    }
    if (startupDesktopChild) {
        explorer::StartupDesktopChildReadback readback;
        auto claimed = startupContext.claim(reinterpret_cast<HANDLE>(startupMapping), reinterpret_cast<HANDLE>(startupParent),
            static_cast<explorer::StartupDesktopArm>(startupArm), readback);
        if (SUCCEEDED(claimed) && (report.native() != startupContext.reportPath() ||
            installedRibbon != startupContext.installedRibbon())) claimed = E_ACCESSDENIED;
        std::fprintf(stderr, "Startup child validity=0x%08lX packet/apartment/parent/identity/startup/attachment/input/executable="
            "0x%08lX/0x%08lX/0x%08lX/0x%08lX/0x%08lX/0x%08lX/0x%08lX/0x%08lX arm=%llu parentPID/TID=%lu/%lu "
            "childPID/TID=%lu/%lu session=%lu borrowedInitial/inputUnchanged=%u/%u.\n",
            static_cast<ULONG>(claimed), static_cast<ULONG>(readback.packet), static_cast<ULONG>(readback.apartment),
            static_cast<ULONG>(readback.parent), static_cast<ULONG>(readback.identity), static_cast<ULONG>(readback.startup),
            static_cast<ULONG>(readback.attachment), static_cast<ULONG>(readback.input), static_cast<ULONG>(readback.executable),
            static_cast<unsigned long long>(startupArm), readback.parentPid, readback.parentTid, readback.childPid,
            readback.childTid, readback.session, static_cast<unsigned>(readback.initialHandleBorrowed),
            static_cast<unsigned>(readback.inputUnchanged));
        std::fflush(stderr);
        if (FAILED(claimed)) return 5;
    }
    explorer::HeadlessStartupTimings startup;
    if (benchmark && !startupClock) return 2;
    if (benchmark) startup.clockStartMs = static_cast<double>(startupEntry.QuadPart) * 1000.0 /
        static_cast<double>(startupFrequency.QuadPart);
    const auto startupElapsed = [&] {
        LARGE_INTEGER counter{};
        if (!benchmark || !QueryPerformanceCounter(&counter)) return 0.0;
        return static_cast<double>(counter.QuadPart - startupEntry.QuadPart) * 1000.0 /
            static_cast<double>(startupFrequency.QuadPart);
    };
    if (visual && (screenshot.empty() || benchmark)) return 2;
    if (sourceDocumentsLibrary && (!visual || scene.page != L"Library" ||
        !location.empty() || !scene.select.empty() || searchContextHandle)) return 2;
    if (scene.openSearchDateMenu && (!visual || scene.page != L"Search" || scene.collapsed)) return 2;
    if (installedRibbon && !headless) return 2;
    if (!crashDump.empty()) {
        const auto diagnostic = explorer::initializeHeadlessCrashDump(headless, crashDump);
        if (FAILED(diagnostic)) {
            std::fprintf(stderr, "Headless crash diagnostic initialization failed (HRESULT=0x%08lX).\n",
                static_cast<unsigned long>(diagnostic));
            return 2;
        }
    }
    const auto borrowedDesktop = previewLowDesktopDiagnostic ? GetThreadDesktop(GetCurrentThreadId()) : nullptr;
    const auto finishPreviewLowDesktop = [&]() -> HRESULT {
        if (!previewLowDesktopDiagnostic || !privateDesktop.ready()) return S_OK;
        bool inputUnchanged = false;
        const auto isolation = privateDesktop.verifyIsolation(&inputUnchanged);
        const auto finished = privateDesktop.finishAtomicLowForDiagnostic();
        const bool restored = SUCCEEDED(finished) && borrowedDesktop &&
            GetThreadDesktop(GetCurrentThreadId()) == borrowedDesktop && explorer::PrivateDesktop::current() == nullptr;
        std::fprintf(stderr, "Preview LOW desktop diagnostic teardown isolation=0x%08lX inputUnchanged=%u finish=0x%08lX restored=%u.\n",
            static_cast<unsigned long>(isolation), static_cast<unsigned>(inputUnchanged),
            static_cast<unsigned long>(finished), static_cast<unsigned>(restored));
        std::fflush(stderr);
        if (FAILED(finished) || !restored) {
            // Retain any still-attached desktop/native resources until this
            // verified headless diagnostic process is stopped.
            if (!TerminateProcess(GetCurrentProcess(), 10)) std::_Exit(10);
            std::_Exit(10);
        }
        return SUCCEEDED(isolation) && inputUnchanged ? S_OK : E_ACCESSDENIED;
    };
    if (previewLowDesktopDiagnostic) {
        explorer::PrivateDesktop::DiagnosticAtomicLabelReadback security;
        const auto deadline = GetTickCount64() + 4500;
        const auto initialized = privateDesktop.initializeAtomicLowForDiagnostic(security, deadline);
        const bool strict = initialized == S_OK && security.guard == S_OK &&
            security.defaultCreated == S_OK && security.lowCreated == S_OK &&
            security.defaultSecurity == S_OK && security.lowSecurity == S_OK && security.equivalentSecurity == S_OK &&
            security.exactOwnedCurrent && security.noninheritable && security.daclEqual && security.ownerEqual &&
            security.groupEqual && security.nonlabelControlEqual && security.lowLabels == 1 &&
            security.lowRid == SECURITY_MANDATORY_LOW_RID && security.lowMask == SYSTEM_MANDATORY_LABEL_NO_WRITE_UP &&
            security.lowFlags == 0 && GetTickCount64() < deadline;
        std::fprintf(stderr,
            "Preview LOW desktop diagnostic init=0x%08lX guard=0x%08lX defaultCreate=0x%08lX lowCreate=0x%08lX "
            "defaultSecurity=0x%08lX lowSecurity=0x%08lX equivalentSecurity=0x%08lX apartment=0x%08lX/%d/%d "
            "defaultLabel=%lu/%lu/%lu/%lu/%lu lowLabel=%lu/%lu/%lu/%lu/%lu "
            "exactOwned=%u noninheritable=%u daclEqual=%u ownerEqual=%u groupEqual=%u nonlabelControlEqual=%u strict=%u; "
            "reuse unchanged native headlessSmoke; cross-run FileID equality unclaimed.\n",
            static_cast<unsigned long>(initialized), static_cast<unsigned long>(security.guard),
            static_cast<unsigned long>(security.defaultCreated), static_cast<unsigned long>(security.lowCreated),
            static_cast<unsigned long>(security.defaultSecurity), static_cast<unsigned long>(security.lowSecurity),
            static_cast<unsigned long>(security.equivalentSecurity), static_cast<unsigned long>(security.apartmentRead),
            security.apartmentType, security.apartmentQualifier,
            security.defaultLabels, security.defaultRid, security.defaultMask, security.defaultFlags, security.defaultControl,
            security.lowLabels, security.lowRid, security.lowMask, security.lowFlags, security.lowControl,
            static_cast<unsigned>(security.exactOwnedCurrent), static_cast<unsigned>(security.noninheritable),
            static_cast<unsigned>(security.daclEqual), static_cast<unsigned>(security.ownerEqual),
            static_cast<unsigned>(security.groupEqual), static_cast<unsigned>(security.nonlabelControlEqual), static_cast<unsigned>(strict));
        std::fflush(stderr);
        if (!strict) { finishPreviewLowDesktop(); return 5; }
    } else if (headless) {
        const auto attached = startupDesktopChild && startupContext.arm() == explorer::StartupDesktopArm::InitialPrivate ?
            privateDesktop.adoptInitialForDiagnostic(startupContext) : privateDesktop.initialize();
        if (startupDesktopChild) {
            std::fprintf(stderr, "Startup child guard arm=%llu attachment=0x%08lX.\n",
                static_cast<unsigned long long>(startupArm), static_cast<ULONG>(attached));
            std::fflush(stderr);
        }
        if (FAILED(attached) || FAILED(privateDesktop.verifyIsolation())) return 5;
    }
    if (benchmark) startup.desktopReadyMs = startupElapsed();
    if (FAILED(explorer::initializeProcessTheme(theme))) { finishPreviewLowDesktop(); return 6; }
    const auto hr = OleInitialize(nullptr);
    if (FAILED(hr)) { finishPreviewLowDesktop(); return 3; }
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    if (benchmark) startup.platformReadyMs = startupElapsed();
    if (sourceDocumentsLibrary) {
        auto source = std::make_unique<explorer::DocumentsLibraryVisualSource>();
        explorer::VisualCaptureReport restricted;
        const auto resolved = source->resolve(privateDesktop, location, restricted.documentsLibrarySource);
        const auto verified = SUCCEEDED(resolved) ? source->verifyMetadata(privateDesktop,
            restricted.documentsLibrarySource) : resolved;
        restricted.documentsLibrarySource.displayUnsupported = true;
        restricted.desktopName = privateDesktop.name();
        restricted.inputDesktopName = privateDesktop.originalInputName();
        const auto isolated = privateDesktop.verifyIsolation(&restricted.inputDesktopUnchanged);
        const auto observed = privateDesktop.visibleWindowsOnInputDesktop(restricted.visibleInputDesktopWindows);
        const auto reported = explorer::writeVisualCaptureReport(std::filesystem::absolute(report), restricted);
        std::fprintf(stderr, "Documents Library display restricted before App creation; protected metadata HRESULT=0x%08lX.\n",
            static_cast<unsigned long>(verified));
        source.reset(); // Release every native interface and the lease before COM.
        OleUninitialize();
        return SUCCEEDED(isolated) && SUCCEEDED(observed) && !restricted.visibleInputDesktopWindows &&
            SUCCEEDED(reported) && (SUCCEEDED(verified) || restricted.documentsLibrarySource.unavailable) ? 9 : 7;
    }
    auto app = new explorer::ExplorerApp(instance, headless,
        (installedRibbon || !headless) ? explorer::RibbonLayout::InstalledWindows10 : explorer::RibbonLayout::Authored);
    auto prepared = visual ? app->prepareHeadlessVisual(scene) : S_OK;
    if (SUCCEEDED(prepared) && searchContextHandle) {
        explorer::SearchWindowContext context;
        prepared = explorer::consumeSearchWindowContext(searchContextHandle, &context);
        if (SUCCEEDED(prepared)) prepared = app->prepareSearchWindowContext(context);
    }
    const auto created = SUCCEEDED(prepared) ? app->create(location) : prepared;
    if (benchmark) startup.createReturnedMs = startupElapsed();
    int result = 0;
    if (FAILED(created)) {
        if (!headless) MessageBoxW(nullptr, explorer::hresultMessage(created).c_str(), L"Windows Explorer could not start", MB_OK | MB_ICONERROR);
        else std::fprintf(stderr, "Headless initialization failed (HRESULT=0x%08lX).\n",
            static_cast<unsigned long>(created));
        result = 4;
    } else result = visual ? app->headlessVisual(privateDesktop, std::filesystem::absolute(screenshot),
            std::filesystem::absolute(report), scene) : benchmark ? app->headlessBenchmark(std::filesystem::absolute(report), startup) : headless ? app->headlessSmoke(report, headlessLibrarySmoke) : app->run(showCommand);
    // The browser's site/filter retain the COM host. Destroy the owned frame
    // while this STA is alive so WM_DESTROY breaks that cycle before Release.
    // A hidden capture returns without running the normal WM_CLOSE loop.
    const auto previewStopped = app->shutdownPreview();
    if (SUCCEEDED(previewStopped)) if (const auto frame = app->window(); IsWindow(frame) &&
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
        // A stalled native thread may hold locks needed by DLL detach code.
        // Stop this process without releasing its borrowed STA resources.
        if (!TerminateProcess(GetCurrentProcess(), 8)) std::_Exit(8);
        std::_Exit(8);
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
    if (FAILED(finishPreviewLowDesktop())) result = 5;
    if (startupDesktopChild) {
        const auto preserved = startupContext.verifyParentAndInput();
        const auto finished = startupContext.arm() == explorer::StartupDesktopArm::InitialPrivate ?
            privateDesktop.finishInitialForDiagnostic() : S_OK;
        std::fprintf(stderr, "Startup child teardown arm=%llu parent/input=0x%08lX finish=0x%08lX.\n",
            static_cast<unsigned long long>(startupArm), static_cast<ULONG>(preserved), static_cast<ULONG>(finished));
        std::fflush(stderr);
        if (FAILED(preserved) || FAILED(finished)) result = 5;
    }
    return result;
}
