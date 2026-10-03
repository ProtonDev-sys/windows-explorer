#include "explorer/core.hpp"

#include <objbase.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

int runShellOperationTests();
int runExtraOperationTests();
int runInputTests();

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
    require(missing.startupLocation == L"shell:MyComputerFolder", "Default startup location changed");
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
    original.showHidden = true;
    original.showExtensions = false;
    original.ribbonCollapsed = true;
    original.view = explorer::ViewMode::Content;
    original.windowWidth = 2200;
    original.windowHeight = 1400;
    original.startupLocation = L"\\\\server\\share\\\u65E5\u672C\u8A9E \U0001F4C1\\a=b%20&c\nline\tname";
    require(explorer::savePreferences(path, original), "Cannot save Unicode settings");
    const auto loaded = explorer::loadPreferences(path);
    require(loaded.startupLocation == original.startupLocation, "Unicode or escaped path did not round-trip");
    require(!loaded.navigationPane && loaded.previewPane && loaded.detailsPane && loaded.showHidden && !loaded.showExtensions && loaded.ribbonCollapsed, "Boolean settings did not round-trip");
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
    require(loaded.windowWidth == 1200 && loaded.windowHeight == 800 && loaded.startupLocation == L"shell:MyComputerFolder", "Invalid bounds or escape did not fall back");
    write(path, "windowWidth=640\nwindowHeight=4320\nview=0\nstartupLocation=  leading and trailing  \n");
    loaded = explorer::loadPreferences(path);
    require(loaded.windowWidth == 640 && loaded.windowHeight == 4320 && loaded.view == explorer::ViewMode::ExtraLargeIcons, "Valid boundary values were rejected");
    require(loaded.startupLocation == L"  leading and trailing  ", "Settings path whitespace was corrupted");
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

void searchAndEnvironment() {
    require(explorer::searchUri(L"kind:picture \u732B & \U0001F600", L"C:\\My folder\\\u65E5\u672C") ==
        L"search-ms:query=kind%3Apicture%20%E7%8C%AB%20%26%20%F0%9F%98%80&crumb=location:C%3A%5CMy%20folder%5C%E6%97%A5%E6%9C%AC", "Search query or scope is not UTF-8 URL encoded");
    require(explorer::searchUri(L"a&crumb=location:C:\\", L"\\\\server\\a,b") ==
        L"search-ms:query=a%26crumb%3Dlocation%3AC%3A%5C&crumb=location:%5C%5Cserver%5Ca%2Cb", "Search delimiters were not escaped");
    require(explorer::searchUri(L"", L"") == L"search-ms:query=", "Empty search must remain syntactically valid");
    require(explorer::searchUri(L"_.~-", L"") == L"search-ms:query=_.~-", "Unreserved URL characters changed");
    const std::wstring variable = L"WINDOWSEXPLORER_TEST_" + std::to_wstring(GetCurrentProcessId());
    require(SetEnvironmentVariableW(variable.c_str(), L"C:\\test \u65E5\u672C\U0001F4C1") != FALSE, "Cannot set test environment variable");
    const auto expanded = explorer::expandEnvironment(L"%" + variable + L"%\\child");
    SetEnvironmentVariableW(variable.c_str(), nullptr);
    require(expanded == L"C:\\test \u65E5\u672C\U0001F4C1\\child", "Environment expansion lost Unicode");
    require(explorer::expandEnvironment(L"%" + variable + L"%") == L"%" + variable + L"%", "Missing environment variable should remain unchanged");
    require(explorer::trim(L" \t\r\n folder name \n") == L"folder name" && explorer::trim(L" \r\n").empty(), "Trim failed");
}

void namesAndSizes() {
    const std::vector<std::wstring> invalid{ L"", L".", L"..", L"file.", L"file ", L"a/b", L"a\\b", L"a:b", L"a*", L"a?", L"a\"b", L"a<b", L"a>b", L"a|b", L"NUL", L"con.txt", L"COM1.tar.gz", L"lpt9", L"COM\u00B9", L"LPT\u00B2.txt", L"COM\u00B3", L"CON .txt", L"CONIN$", std::wstring(256, L'a'), std::wstring(1, L'\0'), std::wstring(1, static_cast<wchar_t>(0xD800)) };
    for (const auto& name : invalid) require(!explorer::validLeafName(name), "Invalid Windows file name was accepted");
    const std::vector<std::wstring> valid{ L"hello.txt", L".gitignore", L"COM10", L"LPT0", L"console.txt", L"space inside.txt", L"\u65E5\u672C\u8A9E\U0001F4C1.txt", std::wstring(255, L'a') };
    for (const auto& name : valid) require(explorer::validLeafName(name), "Valid Windows file name was rejected");
    require(explorer::formatBytes(0) == L"0 bytes" && explorer::formatBytes(1) == L"1 byte" && explorer::formatBytes(1023) == L"1023 bytes", "Byte-size singular or small value is wrong");
    require(explorer::formatBytes(1024) == L"1.00 KB" && explorer::formatBytes(1536) == L"1.50 KB" && explorer::formatBytes(1024ull * 1024) == L"1.00 MB", "Binary file-size unit conversion is wrong");
    require(explorer::formatBytes(std::numeric_limits<std::uint64_t>::max()) == L"16.00 EB", "Large file size overflowed");
    require(explorer::hresultMessage(E_ACCESSDENIED).find(L"0x80070005") != std::wstring::npos, "Error message omitted HRESULT code");
    require(explorer::hresultMessage(static_cast<HRESULT>(0x81234567)).find(L"0x81234567") != std::wstring::npos, "Unknown HRESULT lacks fallback code");
}
} // namespace

int main() {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) {
        std::cerr << "FAIL: Cannot initialize COM for headless shell tests\n";
        return 1;
    }
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"defaults and missing settings", defaultsAndMissingSettings},
        {"Unicode settings round trip and atomic replacement", unicodeSettingsRoundTrip},
        {"invalid settings and bounded fallback", invalidSettingsFallback},
        {"Unicode search URI and environment expansion", searchAndEnvironment},
        {"Windows leaf names, size formatting, HRESULTs", namesAndSizes}
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
    try { failures += static_cast<unsigned>(runExtraOperationTests()); }
    catch (const std::exception& error) { ++failures; std::cerr << "FAIL: Additional file operations: " << error.what() << '\n'; }
    catch (...) { ++failures; std::cerr << "FAIL: Additional file operations: unknown exception\n"; }
    failures += static_cast<unsigned>(runInputTests());
    CoUninitialize();
    return failures == 0 ? 0 : 1;
}
