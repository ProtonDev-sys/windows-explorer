#include "explorer/core.hpp"
#include "explorer/headless_visual.hpp"
#include "explorer/worker_sta.hpp"

#include <objbase.h>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

int runShellOperationTests();
int runInputTests();
int runItemActionTests();
int runSearchTests();
int runSearchRefinementTests();
int runUiStringsTests();
int runUiDirectionTests();
int runContextMenuTests();
int runQuickAccessTests();
int runSavedSearchTests();
int runLibraryTests();
int runNamespaceActionTests();
int runBreadcrumbTests();
int runAppCommandTests();
int runNativeMenuStateTests();
int runSearchHistoryTests();
int runAddressHistoryTests();
int runTypedAddressTests();
int runStaWorkerTests();

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory() {
        const auto root = std::filesystem::temp_directory_path();
        for (unsigned attempt = 0; attempt < 16; ++attempt) {
            path = root / (L"WindowsExplorer-core-tests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(path, error)) return;
            if (error) throw std::runtime_error("Cannot create isolated test directory");
        }
        throw std::runtime_error("Cannot allocate isolated test directory");
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

void write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!output) throw std::runtime_error("Cannot write test fixture");
}

void defaultsAndMissingSettings() {
    TemporaryDirectory temporary;
    const auto missing = explorer::loadPreferences(temporary.path / L"missing.ini");
    require(missing.navigationPane && !missing.previewPane && !missing.detailsPane, "Default panes changed");
    require(!missing.showHidden && missing.showExtensions && !missing.ribbonCollapsed, "Default visibility changed");
    require(missing.view == explorer::ViewMode::Details && missing.windowWidth == 1200 && missing.windowHeight == 800, "Default view or window size changed");
    require(missing.searchWidth == 146, "Windows 10 default search width changed");
    require(missing.useWindowsStartup && missing.startupLocation == explorer::Preferences{}.startupLocation,
            "New startup must inherit Windows Folder Options with stock Quick access fallback");
    const auto profile = explorer::preferencesPath();
    require(!profile.empty() && profile.is_absolute() && profile.filename() == L"settings.ini", "Settings path must use a user profile location");
}

void unicodeSettingsRoundTrip() {
    TemporaryDirectory temporary;
    const auto path = temporary.path / L"\u8A2D\u5B9A\U0001F4C1" / L"settings.ini";
    explorer::Preferences original;
    original.navigationPane = false;
    original.previewPane = true;
    original.detailsPane = true;
    original.expandToCurrent = original.showAllFolders = original.showLibraries = true;
    original.useWindowsStartup = false;
    original.showHidden = true;
    original.showExtensions = false;
    original.ribbonCollapsed = true;
    original.view = explorer::ViewMode::Content;
    original.windowWidth = 2200;
    original.windowHeight = 1400;
    original.searchWidth = 281;
    original.startupLocation = L"\\\\server\\share\\\u65E5\u672C\u8A9E \U0001F4C1\\a=b%20&c\nline\tname";
    require(explorer::savePreferences(path, original), "Cannot save Unicode settings");
    const auto loaded = explorer::loadPreferences(path);
    require(loaded.startupLocation == original.startupLocation, "Unicode or escaped path did not round-trip");
    require(!loaded.navigationPane && loaded.previewPane && loaded.detailsPane && loaded.showHidden && !loaded.showExtensions && loaded.ribbonCollapsed, "Boolean settings did not round-trip");
    require(loaded.expandToCurrent && loaded.showAllFolders && loaded.showLibraries, "Native navigation preferences did not round-trip");
    require(!loaded.useWindowsStartup, "Explicit startup policy did not round-trip");
    require(loaded.searchWidth == 281, "Resizable search width did not round-trip");
    require(loaded.view == original.view && loaded.windowWidth == original.windowWidth && loaded.windowHeight == original.windowHeight, "View settings did not round-trip");
    original.startupLocation = L"shell:Downloads";
    require(explorer::savePreferences(path, original), "Atomic replacement failed");
    require(explorer::loadPreferences(path).startupLocation == original.startupLocation, "Atomic replacement lost data");
    auto invalid = original;
    invalid.startupLocation.assign(1, static_cast<wchar_t>(0xD800));
    require(!explorer::savePreferences(path, invalid), "Invalid Unicode settings must be rejected");
    require(explorer::loadPreferences(path).startupLocation == original.startupLocation, "Failed settings write corrupted the last valid settings");
    for (const auto& file : std::filesystem::directory_iterator(path.parent_path())) require(file.path() == path, "Temporary settings files leaked");
}

void invalidSettingsFallback() {
    TemporaryDirectory temporary;
    const auto path = temporary.path / L"settings.ini";
    write(path, "\xEF\xBB\xBF# optional UTF-8 BOM\r\nshowHidden = true\r\nshowExtensions=invalid\nview=99\nwindowWidth=999999999999999999\nwindowHeight=-1\nstartupLocation=bad\\escape\nnavigationPane=false\npreviewPane=1\nunknown=future\n");
    auto loaded = explorer::loadPreferences(path);
    require(loaded.showHidden && !loaded.navigationPane && loaded.previewPane, "Valid settings were not retained beside invalid values");
    require(loaded.showExtensions && loaded.view == explorer::ViewMode::Details, "Invalid boolean or view did not fall back");
    require(loaded.windowWidth == 1200 && loaded.windowHeight == 800 && loaded.startupLocation == explorer::Preferences{}.startupLocation && loaded.useWindowsStartup, "Invalid bounds or escape did not fall back");
    write(path, "windowWidth=640\nwindowHeight=4320\nview=0\nstartupLocation=  leading and trailing  \n");
    loaded = explorer::loadPreferences(path);
    require(loaded.windowWidth == 640 && loaded.windowHeight == 4320 && loaded.view == explorer::ViewMode::ExtraLargeIcons, "Valid boundary values were rejected");
    require(loaded.startupLocation == L"  leading and trailing  ", "Settings path whitespace was corrupted");
    require(!loaded.useWindowsStartup, "Legacy explicit startup location must stay authoritative");
    write(path, "startupLocation=shell:MyComputerFolder\nuseWindowsStartup=true\n");
    loaded = explorer::loadPreferences(path);
    require(loaded.useWindowsStartup && loaded.startupLocation == L"shell:MyComputerFolder",
            "Windows startup policy must coexist with retained compatibility location");
    write(path, std::string("showHidden=true\nstartupLocation=") + static_cast<char>(0xFF));
    require(!explorer::loadPreferences(path).showHidden, "Malformed UTF-8 file must fall back safely");
    write(path, std::string(256 * 1024 + 1, 'x'));
    require(explorer::loadPreferences(path).windowWidth == 1200, "Oversized settings must fall back safely");
    explorer::Preferences invalid;
    invalid.startupLocation.assign(1, static_cast<wchar_t>(0xD800));
    require(!explorer::savePreferences(path, invalid), "Invalid UTF-16 must not be persisted");
    require(!explorer::savePreferences({}, explorer::Preferences{}), "Empty settings path must fail");
    require(!explorer::savePreferences(temporary.path, explorer::Preferences{}), "Directory settings target must fail");
}

void environmentExpansion() {
    const std::wstring variable = L"WINDOWSEXPLORER_TEST_" + std::to_wstring(GetCurrentProcessId());
    require(SetEnvironmentVariableW(variable.c_str(), L"C:\\test \u65E5\u672C\U0001F4C1") != FALSE, "Cannot set test environment variable");
    const auto expanded = explorer::expandEnvironment(L"%" + variable + L"%\\child");
    SetEnvironmentVariableW(variable.c_str(), nullptr);
    require(expanded == L"C:\\test \u65E5\u672C\U0001F4C1\\child", "Environment expansion lost Unicode");
    require(explorer::expandEnvironment(L"%" + variable + L"%") == L"%" + variable + L"%", "Missing environment variable should remain unchanged");
    require(explorer::trim(L" \t\r\n folder name \n") == L"folder name" && explorer::trim(L" \r\n").empty(), "Trim failed");
}

void namesAndErrors() {
    const std::vector<std::wstring> invalid{ L"", L".", L"..", L"file.", L"file ", L"a/b", L"a\\b", L"a:b", L"a*", L"a?", L"a\"b", L"a<b", L"a>b", L"a|b", L"NUL", L"con.txt", L"COM1.tar.gz", L"lpt9", L"COM\u00B9", L"LPT\u00B2.txt", L"COM\u00B3", L"CON .txt", L"CONIN$", std::wstring(256, L'a'), std::wstring(1, L'\0'), std::wstring(1, static_cast<wchar_t>(0xD800)) };
    for (const auto& name : invalid) require(!explorer::validLeafName(name), "Invalid Windows file name was accepted");
    const std::vector<std::wstring> valid{ L"hello.txt", L".gitignore", L"COM10", L"LPT0", L"console.txt", L"space inside.txt", L"\u65E5\u672C\u8A9E\U0001F4C1.txt", std::wstring(255, L'a') };
    for (const auto& name : valid) require(explorer::validLeafName(name), "Valid Windows file name was rejected");
    require(explorer::hresultMessage(E_ACCESSDENIED).find(L"0x80070005") != std::wstring::npos, "Error message omitted HRESULT code");
    require(explorer::hresultMessage(static_cast<HRESULT>(0x81234567)).find(L"0x81234567") != std::wstring::npos, "Unknown HRESULT lacks fallback code");
}
void drainCreatorBeforeShutdown() {
    const auto drained = explorer::drainStaWorkers(5000);
    if (FAILED(drained)) {
        std::cerr << "FAIL: final creator STA worker drain HRESULT="
                  << static_cast<unsigned long>(drained) << '\n';
        std::cerr.flush();
        // Keep the initialized creator and its private desktop alive until
        // process termination if native work cannot actually be drained.
        if (!TerminateProcess(GetCurrentProcess(), 10)) std::_Exit(10);
        std::_Exit(10);
    }
}
} // namespace

int main(int argc, char** argv) {
    // Preserve actual progress before CTest terminates a stalled provider;
    // redirected console buffering otherwise hides completed test groups.
    std::cout.setf(std::ios::unitbuf);
    std::wcout.setf(std::ios::unitbuf);
    const auto suiteStarted = GetTickCount64();
    // Even console fixtures can cause a native provider to create a helper
    // HWND. Attach before any COM call and keep the input desktop untouched.
    explorer::PrivateDesktop desktop;
    const auto isolated = desktop.initialize();
    if (FAILED(isolated)) {
        std::cerr << "FAIL: initialize core private desktop HRESULT="
                  << static_cast<unsigned long>(isolated) << '\n';
        return 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--worker-only") return runStaWorkerTests();
    if (argc == 2 && std::string_view(argv[1]) == "--menu-state-only") {
        const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(initialized)) return 1;
        const auto failures = runNativeMenuStateTests();
        drainCreatorBeforeShutdown();
        CoUninitialize();
        return failures ? 1 : 0;
    }
    if (argc == 2 && (std::string_view(argv[1]) == "--search-only" || std::string_view(argv[1]) == "--namespace-only" ||
                     std::string_view(argv[1]) == "--refinement-only" || std::string_view(argv[1]) == "--ui-strings-only" ||
                     std::string_view(argv[1]) == "--saved-search-only" || std::string_view(argv[1]) == "--app-commands-only" ||
                     std::string_view(argv[1]) == "--direction-only")) {
        const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(initialized)) return 1;
        const auto mode = std::string_view(argv[1]);
        const auto failures = mode == "--search-only" ? runSearchTests() :
            mode == "--namespace-only" ? runNamespaceActionTests() :
            mode == "--refinement-only" ? runSearchRefinementTests() :
            mode == "--ui-strings-only" ? runUiStringsTests() :
            mode == "--saved-search-only" ? runSavedSearchTests() :
            mode == "--app-commands-only" ? runAppCommandTests() : runUiDirectionTests();
        drainCreatorBeforeShutdown();
        CoUninitialize();
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--worker-after-autocomplete") {
        const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(initialized)) return 1;
        const auto failures = runSearchHistoryTests() + runStaWorkerTests();
        drainCreatorBeforeShutdown();
        CoUninitialize();
        return failures ? 1 : 0;
    }
    if (argc != 1) return 2;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) {
        std::cerr << "FAIL: Cannot initialize COM for headless shell tests\n";
        return 1;
    }
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"defaults and missing settings", defaultsAndMissingSettings},
        {"Unicode settings round trip and atomic replacement", unicodeSettingsRoundTrip},
        {"invalid settings and bounded fallback", invalidSettingsFallback},
        {"Unicode environment expansion", environmentExpansion},
        {"Windows leaf names and HRESULTs", namesAndErrors}
    };
    unsigned failures = 0;
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "PASS: " << name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL: " << name << ": " << error.what() << '\n'; }
        catch (...) { ++failures; std::cerr << "FAIL: " << name << ": unknown exception\n"; }
    }
    std::cout << tests.size() - failures << '/' << tests.size() << " headless groups passed\n";
    try { failures += static_cast<unsigned>(runShellOperationTests()); }
    catch (const std::exception& error) { ++failures; std::cerr << "FAIL: Shell file operations: " << error.what() << '\n'; }
    catch (...) { ++failures; std::cerr << "FAIL: Shell file operations: unknown exception\n"; }
    failures += static_cast<unsigned>(runInputTests());
    failures += static_cast<unsigned>(runItemActionTests());
    failures += static_cast<unsigned>(runSearchTests());
    failures += static_cast<unsigned>(runSearchRefinementTests());
    failures += static_cast<unsigned>(runUiStringsTests());
    failures += static_cast<unsigned>(runUiDirectionTests());
    failures += static_cast<unsigned>(runContextMenuTests());
    failures += static_cast<unsigned>(runQuickAccessTests());
    failures += static_cast<unsigned>(runSavedSearchTests());
    failures += static_cast<unsigned>(runLibraryTests());
    failures += static_cast<unsigned>(runNamespaceActionTests());
    failures += static_cast<unsigned>(runBreadcrumbTests());
    failures += static_cast<unsigned>(runAppCommandTests());
    failures += static_cast<unsigned>(runSearchHistoryTests());
    failures += static_cast<unsigned>(runAddressHistoryTests());
    failures += static_cast<unsigned>(runTypedAddressTests());
    failures += static_cast<unsigned>(runStaWorkerTests());
    drainCreatorBeforeShutdown();
    bool inputUnchanged = false, visible = true;
    if (FAILED(desktop.verifyIsolation(&inputUnchanged)) || !inputUnchanged ||
        FAILED(desktop.visibleWindowsOnInputDesktop(visible)) || visible) {
        ++failures;
        std::cerr << "FAIL: final core private-desktop/input-window isolation\n";
    }
    CoUninitialize();
    std::cout << "Core suite elapsed_ms=" << GetTickCount64() - suiteStarted
              << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
