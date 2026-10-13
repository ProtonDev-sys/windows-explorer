#include "state_file.hpp"
#include "explorer/core.hpp"

#include <shlobj.h>
#include <algorithm>
#include <charconv>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <new>
#include <sstream>
#include <string_view>

namespace explorer {
namespace {
constexpr std::size_t maximumSettingsBytes = 256 * 1024;
constexpr std::size_t maximumLocationLength = 32767;

std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return {};
    const auto length = static_cast<int>(text.size());
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, result.data(), required, nullptr, nullptr)) return {};
    return result;
}

bool fromUtf8(const std::string& text, std::wstring& result) {
    if (text.empty()) { result.clear(); return true; }
    const int length = static_cast<int>(text.size());
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
    if (required <= 0) return false;
    result.resize(static_cast<std::size_t>(required));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, result.data(), required) != 0;
}

std::string escapeLocation(const std::string& text) {
    std::string result;
    for (const char ch : text) {
        switch (ch) {
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result += ch; break;
        }
    }
    return result;
}

bool unescapeLocation(const std::string& text, std::wstring& result) {
    std::string unescaped;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char ch = text[index];
        if (ch != '\\') { unescaped += ch; continue; }
        if (++index == text.size()) return false;
        switch (text[index]) {
        case '\\': unescaped += '\\'; break;
        case 'n': unescaped += '\n'; break;
        case 'r': unescaped += '\r'; break;
        case 't': unescaped += '\t'; break;
        default: return false;
        }
    }
    return fromUtf8(unescaped, result) && !result.empty() && result.size() <= maximumLocationLength && result.find(L'\0') == std::wstring::npos;
}

std::string_view trimAscii(std::string_view text) {
    const auto whitespace = [](char ch) { return ch == ' ' || ch == '\t' || ch == '\r'; };
    while (!text.empty() && whitespace(text.front())) text.remove_prefix(1);
    while (!text.empty() && whitespace(text.back())) text.remove_suffix(1);
    return text;
}

int integerOr(std::string_view text, int fallback, int minimum, int maximum) {
    text = trimAscii(text);
    int value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value >= minimum && value <= maximum ? value : fallback;
}

bool booleanOr(std::string_view text, bool fallback) {
    text = trimAscii(text);
    if (text == "true" || text == "1") return true;
    if (text == "false" || text == "0") return false;
    return fallback;
}

} // namespace

std::filesystem::path preferencesPath() {
    PWSTR localAppData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &localAppData))) {
        const std::filesystem::path path(localAppData);
        CoTaskMemFree(localAppData);
        return path / L"WindowsExplorer" / L"settings.ini";
    }
    const auto environment = expandEnvironment(L"%LOCALAPPDATA%");
    if (!environment.empty() && environment != L"%LOCALAPPDATA%") return std::filesystem::path(environment) / L"WindowsExplorer" / L"settings.ini";
    // An empty path makes a missing user profile a normal settings failure.
    return {};
}

std::wstring windowsDefaultStartupLocation() {
    DWORD launchTo = 2, size = sizeof(launchTo);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                    L"LaunchTo", RRF_RT_REG_DWORD, nullptr, &launchTo, &size) == ERROR_SUCCESS && launchTo == 1)
        return L"shell:MyComputerFolder";
    return Preferences{}.startupLocation;
}

Preferences loadPreferences(const std::filesystem::path& path) {
    const Preferences defaults;
    Preferences result = defaults;
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > maximumSettingsBytes) return result;
    std::ifstream input(path, std::ios::binary);
    if (!input) return result;
    // Bound the read as well as the initial size check if another process grows the file.
    std::string contents(maximumSettingsBytes + 1, '\0');
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    contents.resize(static_cast<std::size_t>(input.gcount()));
    if (contents.size() > maximumSettingsBytes || input.bad()) return result;
    if (contents.starts_with("\xEF\xBB\xBF")) contents.erase(0, 3);
    std::wstring checked;
    if (!fromUtf8(contents, checked) || contents.find('\0') != std::string::npos) return result;
    std::istringstream lines(contents);
    std::string line;
    bool startupLocationSpecified = false, startupPolicySpecified = false;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto stripped = trimAscii(line);
        if (stripped.empty() || stripped.front() == '#' || stripped.front() == ';') continue;
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        const auto key = trimAscii(std::string_view(line).substr(0, equals));
        const auto value = std::string_view(line).substr(equals + 1);
        if (key == "navigationPane") result.navigationPane = booleanOr(value, defaults.navigationPane);
        else if (key == "previewPane") result.previewPane = booleanOr(value, defaults.previewPane);
        else if (key == "detailsPane") result.detailsPane = booleanOr(value, defaults.detailsPane);
        else if (key == "expandToCurrent") result.expandToCurrent = booleanOr(value, defaults.expandToCurrent);
        else if (key == "showAllFolders") result.showAllFolders = booleanOr(value, defaults.showAllFolders);
        else if (key == "showLibraries") result.showLibraries = booleanOr(value, defaults.showLibraries);
        else if (key == "useWindowsStartup") {
            result.useWindowsStartup = booleanOr(value, defaults.useWindowsStartup);
            startupPolicySpecified = true;
        }
        else if (key == "searchWidth") result.searchWidth = integerOr(value, defaults.searchWidth, 90, 4096);
        else if (key == "previewWidth") result.previewWidth = integerOr(value, defaults.previewWidth, 120, 4096);
        else if (key == "showHidden") result.showHidden = booleanOr(value, defaults.showHidden);
        else if (key == "showExtensions") result.showExtensions = booleanOr(value, defaults.showExtensions);
        else if (key == "ribbonCollapsed") result.ribbonCollapsed = booleanOr(value, defaults.ribbonCollapsed);
        else if (key == "view") result.view = static_cast<ViewMode>(integerOr(value, static_cast<int>(defaults.view), 0, 7));
        else if (key == "windowWidth") result.windowWidth = integerOr(value, defaults.windowWidth, 640, 7680);
        else if (key == "windowHeight") result.windowHeight = integerOr(value, defaults.windowHeight, 480, 4320);
        else if (key == "startupLocation") {
            std::wstring location;
            if (unescapeLocation(std::string(value), location)) {
                result.startupLocation = std::move(location);
                startupLocationSpecified = true;
            } else result.startupLocation = defaults.startupLocation;
        }
    }
    if (!startupPolicySpecified && startupLocationSpecified) result.useWindowsStartup = false;
    return result;
}

HRESULT savePreferencesStatus(const std::filesystem::path& path, const Preferences& preferences) noexcept {
    try {
        if (path.empty() || preferences.startupLocation.empty() || preferences.startupLocation.size() > maximumLocationLength || preferences.startupLocation.find(L'\0') != std::wstring::npos) return E_INVALIDARG;
        const auto encodedLocation = utf8(preferences.startupLocation);
        if (encodedLocation.empty()) return E_INVALIDARG;
        const Preferences defaults;
        const auto view = static_cast<int>(preferences.view);
        std::ostringstream output;
        output << "# WindowsExplorer settings (UTF-8).\nversion=1\n" << std::boolalpha
            << "navigationPane=" << preferences.navigationPane << '\n'
            << "previewPane=" << preferences.previewPane << '\n'
            << "detailsPane=" << preferences.detailsPane << '\n'
            << "expandToCurrent=" << preferences.expandToCurrent << '\n'
            << "showAllFolders=" << preferences.showAllFolders << '\n'
            << "showLibraries=" << preferences.showLibraries << '\n'
            << "useWindowsStartup=" << preferences.useWindowsStartup << '\n'
            << "searchWidth=" << preferences.searchWidth << '\n'
            << "previewWidth=" << (preferences.previewWidth >= 120 && preferences.previewWidth <= 4096 ? preferences.previewWidth : defaults.previewWidth) << '\n'
            << "showHidden=" << preferences.showHidden << '\n'
            << "showExtensions=" << preferences.showExtensions << '\n'
            << "ribbonCollapsed=" << preferences.ribbonCollapsed << '\n'
            << "view=" << (view >= 0 && view <= 7 ? view : static_cast<int>(defaults.view)) << '\n'
            << "windowWidth=" << (preferences.windowWidth >= 640 && preferences.windowWidth <= 7680 ? preferences.windowWidth : defaults.windowWidth) << '\n'
            << "windowHeight=" << (preferences.windowHeight >= 480 && preferences.windowHeight <= 4320 ? preferences.windowHeight : defaults.windowHeight) << '\n'
            << "startupLocation=" << escapeLocation(encodedLocation) << '\n';
        if (!output) return E_FAIL;
        const auto contents = output.str();
        return writeStateFileAtomic(path, std::string_view(contents));
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}

bool savePreferences(const std::filesystem::path& path, const Preferences& preferences) {
    return SUCCEEDED(savePreferencesStatus(path, preferences));
}

std::wstring trim(const std::wstring& text) {
    const auto first = std::find_if_not(text.begin(), text.end(), [](wchar_t ch) { return std::iswspace(ch) != 0; });
    const auto last = std::find_if_not(text.rbegin(), text.rend(), [](wchar_t ch) { return std::iswspace(ch) != 0; }).base();
    return first >= last ? std::wstring{} : std::wstring(first, last);
}

std::wstring expandEnvironment(const std::wstring& text) {
    if (text.empty()) return {};
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        const DWORD required = ExpandEnvironmentStringsW(text.c_str(), nullptr, 0);
        if (!required || required > 1024 * 1024) return text;
        std::wstring result(static_cast<std::size_t>(required), L'\0');
        const DWORD copied = ExpandEnvironmentStringsW(text.c_str(), result.data(), required);
        if (!copied) return text;
        if (copied > required) continue;
        result.resize(static_cast<std::size_t>(copied) - 1);
        return result;
    }
    return text;
}

bool validLeafName(const std::wstring& name) {
    if (name.empty() || name.size() > 255 || name == L"." || name == L".." || name.back() == L' ' || name.back() == L'.') return false;
    for (std::size_t index = 0; index < name.size(); ++index) {
        const wchar_t ch = name[index];
        if (ch < 32 || std::wstring_view(L"<>:\"/\\|?*").find(ch) != std::wstring_view::npos) return false;
        if (ch >= 0xD800 && ch <= 0xDBFF) {
            if (index + 1 == name.size() || name[index + 1] < 0xDC00 || name[index + 1] > 0xDFFF) return false;
            ++index;
        } else if (ch >= 0xDC00 && ch <= 0xDFFF) return false;
    }
    std::wstring base = name.substr(0, name.find(L'.'));
    while (!base.empty() && base.back() == L' ') base.pop_back();
    std::transform(base.begin(), base.end(), base.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towupper(ch)); });
    if (base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL" || base == L"CONIN$" || base == L"CONOUT$") return false;
    if (base.size() == 4 && (base.starts_with(L"COM") || base.starts_with(L"LPT"))) {
        const wchar_t digit = base.back();
        if ((digit >= L'1' && digit <= L'9') || digit == L'\u00B9' || digit == L'\u00B2' || digit == L'\u00B3') return false;
    }
    return true;
}

std::wstring hresultMessage(HRESULT result) {
    PWSTR buffer = nullptr;
    DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(result), 0, reinterpret_cast<PWSTR>(&buffer), 0, nullptr);
    if (!length && HRESULT_FACILITY(result) == FACILITY_WIN32) {
        length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, HRESULT_CODE(result), 0, reinterpret_cast<PWSTR>(&buffer), 0, nullptr);
    }
    std::wstring message;
    if (length && buffer) message = trim(std::wstring(buffer, length));
    if (buffer) LocalFree(buffer);
    std::wostringstream code;
    code << L"0x" << std::uppercase << std::hex << std::setfill(L'0') << std::setw(8) << static_cast<std::uint32_t>(result);
    return message.empty() ? code.str() : message + L" (" + code.str() + L")";
}

} // namespace explorer
