#include "explorer/address_history.hpp"
#include <objbase.h>
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void succeeded(HRESULT hr, const char* message) {
    if (FAILED(hr)) { std::cerr << message << " HRESULT=0x" << std::hex << static_cast<unsigned long>(hr) << std::dec << '\n';
        throw std::runtime_error(message); }
}
struct Fixture {
    fs::path root;
    Fixture() {
        GUID id{}; wchar_t name[40]{};
        require(SUCCEEDED(CoCreateGuid(&id)) && StringFromGUID2(id, name, ARRAYSIZE(name)), "create address fixture identity");
        root = fs::temp_directory_path() / (std::wstring(L"WindowsExplorer-OwnedAddressHistory-") + name);
        require(fs::create_directory(root), "create exclusively owned address fixture");
    }
    ~Fixture() { std::error_code ignored; fs::remove_all(root, ignored); }
};
struct File {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~File() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
std::string read(const fs::path& path) {
    File file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.handle == INVALID_HANDLE_VALUE) {
        succeeded(HRESULT_FROM_WIN32(GetLastError()), "open owned raw history bytes");
        throw std::runtime_error("open owned raw history returned an invalid handle");
    }
    LARGE_INTEGER length{};
    require(GetFileSizeEx(file.handle, &length) && length.QuadPart >= 0 && length.QuadPart < 3000000, "bounded raw fixture read");
    std::string bytes(static_cast<size_t>(length.QuadPart), '\0'); DWORD count = 0;
    const auto loaded = ReadFile(file.handle, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr);
    require(loaded && count == bytes.size(), "read exact owned raw history bytes");
    return bytes;
}
void write(const fs::path& path, const std::string& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); require(stream.good(), "write owned address fixture");
}
struct Identity {
    ULONGLONG volume = 0;
    std::array<BYTE, 16> identifier{};
    bool operator==(const Identity&) const = default;
};
Identity identity(const fs::path& path) {
    File file{CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    FILE_ID_INFO info{};
    require(file.handle != INVALID_HANDLE_VALUE && GetFileInformationByHandleEx(file.handle, FileIdInfo, &info, sizeof(info)),
            "read exact owned history identity");
    Identity result{info.VolumeSerialNumber}; std::memcpy(result.identifier.data(), info.FileId.Identifier, result.identifier.size());
    return result;
}
FILE_BASIC_INFO basic(const fs::path& path) {
    File file{CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    FILE_BASIC_INFO info{};
    require(file.handle != INVALID_HANDLE_VALUE && GetFileInformationByHandleEx(file.handle, FileBasicInfo, &info, sizeof(info)),
            "read owned history timestamps"); return info;
}
std::set<std::wstring> members(const fs::path& root) {
    std::set<std::wstring> result;
    for (const auto& item : fs::recursive_directory_iterator(root)) result.insert(item.path().lexically_relative(root).native());
    return result;
}
void persistenceAndFailure() {
    Fixture fixture; const auto path = fixture.root / L"nested" / L"address-history.dat";
    const std::vector<std::wstring> original{L"  C:\\資料😀\\Folder  ", L"%USERPROFILE%\\Desktop", L"\\\\server\\share\\folder",
                                           L"shell:Downloads", L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}"};
    std::vector<std::wstring> loaded{L"preserved"};
    require(FAILED(explorer::loadAddressHistory(path, &loaded)) && loaded == std::vector<std::wstring>{L"preserved"},
            "missing history changed the caller");
    succeeded(explorer::saveAddressHistory(path, original), "save literal Unicode/env/UNC/virtual typed addresses");
    succeeded(explorer::loadAddressHistory(path, &loaded), "read exact typed address history");
    require(loaded == original, "typed history expanded, trimmed or reordered original strings");
    const std::vector<std::wstring> replacement{L"C:\\replacement", L"C:\\second"};
    succeeded(explorer::saveAddressHistory(path, replacement), "publish complete address history replacement");
    succeeded(explorer::loadAddressHistory(path, &loaded), "read replacement history");
    require(loaded == replacement, "atomic replacement retained stale history");
    const auto before = read(path); const auto fileId = identity(path); const auto info = basic(path);
    const auto expectedMembers = members(fixture.root);
    const auto unchanged = [&] {
        const auto now = basic(path);
        require(read(path) == before && identity(path) == fileId && now.LastWriteTime.QuadPart == info.LastWriteTime.QuadPart &&
            now.ChangeTime.QuadPart == info.ChangeTime.QuadPart && now.FileAttributes == info.FileAttributes &&
            members(fixture.root) == expectedMembers, "rejected save changed existing data/identity/timestamps or leaked a temporary");
    };
    auto invalid = replacement; invalid.push_back(std::wstring(1, static_cast<wchar_t>(0xd800)));
    require(explorer::saveAddressHistory(path, invalid) == E_INVALIDARG, "invalid Unicode was saved"); unchanged();
    invalid = {L"C:\\same", L"c:\\SAME"};
    require(explorer::saveAddressHistory(path, invalid) == E_INVALIDARG, "case-insensitive duplicate was saved"); unchanged();
    invalid.assign(21, L"too many");
    require(explorer::saveAddressHistory(path, invalid) == E_INVALIDARG, "unbounded address MRU accepted"); unchanged();
    require(explorer::saveAddressHistory(L"relative-address-history.dat", replacement) == E_INVALIDARG &&
        explorer::loadAddressHistory(path, nullptr) == E_POINTER, "address history path/pointer guards failed"); unchanged();
    {
        File lock{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        require(lock.handle != INVALID_HANDLE_VALUE, "lock only owned address history");
        require(explorer::saveAddressHistory(path, original) == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION),
                "history save ignored a reader denying replacement"); unchanged();
    }
    require(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE, "set owned read-only history");
    const auto readOnly = basic(path);
    require(explorer::saveAddressHistory(path, original) == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) && read(path) == before &&
        identity(path) == fileId && basic(path).ChangeTime.QuadPart == readOnly.ChangeTime.QuadPart &&
        members(fixture.root) == expectedMembers, "read-only save changed original history");
    require(SetFileAttributesW(path.c_str(), info.FileAttributes) != FALSE, "restore owned history attributes");
    require(FAILED(explorer::saveAddressHistory(fixture.root, replacement)) && members(fixture.root) == expectedMembers,
            "directory target accepted or created a leftover temporary");
    succeeded(explorer::saveAddressHistory(path, {}), "save empty address history");
    succeeded(explorer::loadAddressHistory(path, &loaded), "load empty address history"); require(loaded.empty(), "empty codec retained entries");
}
void malformedCodec() {
    Fixture fixture; const auto path = fixture.root / L"address-history.dat";
    const std::vector<std::wstring> values{L"C:\\A", L"C:\\B"};
    succeeded(explorer::saveAddressHistory(path, values), "create codec corruption fixture");
    require(fs::exists(path), "successful initial codec save left no pathname");
    const auto correct = read(path);
    std::vector<std::string> malformed;
    malformed.push_back("WXSearchHistory1\n"); malformed.push_back(correct.substr(0, correct.size() - 1));
    malformed.push_back(correct + "trailing data");
    auto wrongCount = correct; const auto header = correct.find('\n') + 1; wrongCount[header] = 21; malformed.push_back(wrongCount);
    auto badUtf8 = correct; const auto token = badUtf8.find("C:\\A"); require(token != std::string::npos, "locate literal codec fixture");
    badUtf8[token] = static_cast<char>(0xc0); badUtf8[token + 1] = static_cast<char>(0xaf); malformed.push_back(badUtf8);
    auto duplicate = correct; const auto second = duplicate.find("C:\\B"); require(second != std::string::npos, "locate second codec fixture");
    duplicate[second + 3] = 'a'; malformed.push_back(duplicate);
    for (const auto& bytes : malformed) {
        write(path, bytes); std::vector<std::wstring> loaded{L"caller remains intact"};
        require(explorer::loadAddressHistory(path, &loaded) == HRESULT_FROM_WIN32(ERROR_INVALID_DATA) &&
            loaded == std::vector<std::wstring>{L"caller remains intact"}, "malformed/different-version history mutated output");
    }
    const std::vector<std::wstring> longValue{std::wstring(32767, L'界')};
    succeeded(explorer::saveAddressHistory(path, longValue), "save maximum length exact typed string");
    std::vector<std::wstring> loaded; succeeded(explorer::loadAddressHistory(path, &loaded), "restore maximum length typed string");
    require(loaded == longValue, "bounded Unicode length changed during codec roundtrip");
    require(fs::exists(path), "successful maximum-length codec load lost its pathname");
    require(explorer::saveAddressHistory(path, std::vector<std::wstring>{std::wstring(32768, L'a')}) == E_INVALIDARG,
            "oversized typed string was accepted");
    require(fs::exists(path), "rejected oversized typed string removed the history pathname");
    require(read(path).size() > 32767, "oversized typed string replaced the valid maximum");
}
void orderingAndImport() {
    std::vector<std::wstring> values;
    for (unsigned index = 0; index < 25; ++index)
        require(explorer::rememberTypedAddress(values, L"C:\\owned\\" + std::to_wstring(index)), "remember typed address");
    require(values.size() == 20 && values.front() == L"C:\\owned\\24" && values.back() == L"C:\\owned\\5", "bounded address MRU ordering failed");
    require(explorer::rememberTypedAddress(values, L"c:\\OWNED\\12") && values.front() == L"c:\\OWNED\\12" && values.size() == 20,
            "ordinal duplicate did not move/newest original spelling was lost");
    const auto original = values;
    require(!explorer::rememberTypedAddress(values, values.front()) && values == original,
            "unchanged first entry reported a new history mutation");
    require(!explorer::rememberTypedAddress(values, L"") && !explorer::rememberTypedAddress(values, std::wstring(L"a\0b", 3)) &&
        values == original, "invalid typed address changed the MRU");
    values = {L"  %USERPROFILE%\\Desktop  ", L"C:\\own"};
    const std::vector<std::wstring> imported{L"c:\\OWN", L"\\\\server\\share", L"shell:Downloads", L"SHELL:downloads"};
    succeeded(explorer::mergeTypedAddressHistory(values, imported), "merge read-only native MRU");
    require(values == std::vector<std::wstring>{L"  %USERPROFILE%\\Desktop  ", L"C:\\own", L"\\\\server\\share", L"shell:Downloads"},
            "native import displaced own priority, expanded a value or retained duplicates");
    const auto before = values;
    require(explorer::mergeTypedAddressHistory(values, std::vector<std::wstring>{L"valid", L""}) == E_INVALIDARG && values == before,
            "invalid import partially changed own history");
}
struct RegistryFixture {
    std::wstring path;
    HKEY key = nullptr;
    RegistryFixture() {
        GUID id{}; wchar_t name[40]{};
        require(SUCCEEDED(CoCreateGuid(&id)) && StringFromGUID2(id, name, ARRAYSIZE(name)), "create owned registry identity");
        path = std::wstring(L"Software\\WindowsExplorer-OwnedAddressFixture-") + name;
        DWORD disposition = 0;
        require(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, REG_OPTION_VOLATILE,
            KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, &disposition) == ERROR_SUCCESS && disposition == REG_CREATED_NEW_KEY,
            "create exclusively owned volatile registry fixture");
    }
    ~RegistryFixture() { if (key) { RegCloseKey(key); RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()); } }
    void set(const wchar_t* name, const wchar_t* text, DWORD type = REG_SZ) {
        require(RegSetValueExW(key, name, 0, type, reinterpret_cast<const BYTE*>(text),
            static_cast<DWORD>((wcslen(text) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS, "write only owned registry fixture value");
    }
};
void registryReadOnly() {
    RegistryFixture fixture;
    fixture.set(L"url10", L"\\\\server\\share"); fixture.set(L"url2", L"%USERPROFILE%\\Desktop", REG_EXPAND_SZ);
    fixture.set(L"URL1", L"C:\\資料😀"); fixture.set(L"url3", L"c:\\資料😀");
    fixture.set(L"url04", L"leading-zero-invalid"); fixture.set(L"url0", L"zero-invalid");
    fixture.set(L"url5", L"wrong type", REG_BINARY); fixture.set(L"unrelated", L"ignored");
    DWORD countBefore = 0; FILETIME before{};
    require(RegQueryInfoKeyW(fixture.key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &countBefore,
        nullptr, nullptr, nullptr, &before) == ERROR_SUCCESS, "snapshot only owned registry metadata");
    std::vector<std::wstring> values{L"old"};
    succeeded(explorer::readTypedAddressRegistry(fixture.key, &values), "read actual native registry format through owned key");
    require(values == std::vector<std::wstring>{L"C:\\資料😀", L"%USERPROFILE%\\Desktop", L"\\\\server\\share"},
            "native typed address ordering/type/ordinal dedup changed literal contents");
    DWORD countAfter = 0; FILETIME after{};
    require(RegQueryInfoKeyW(fixture.key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &countAfter,
        nullptr, nullptr, nullptr, &after) == ERROR_SUCCESS && countAfter == countBefore &&
        before.dwLowDateTime == after.dwLowDateTime && before.dwHighDateTime == after.dwHighDateTime,
        "read-only typed registry loader wrote a value");
    const auto saved = values;
    require(explorer::readTypedAddressRegistry(nullptr, &values) == E_INVALIDARG && values == saved &&
            explorer::readTypedAddressRegistry(fixture.key, nullptr) == E_POINTER, "registry injection guards changed output");
}
} // namespace
int runAddressHistoryTests() {
    unsigned failures = 0;
    const std::pair<const char*, void(*)()> groups[]{
        {"exact Unicode/env/UNC/virtual persistence and atomic failure preservation", persistenceAndFailure},
        {"separate bounded codec, malformed input and output preservation", malformedCodec},
        {"typed ordinal MRU and own-priority native import", orderingAndImport},
        {"read-only native registry format on exclusively owned volatile key", registryReadOnly}};
    for (const auto& [name, test] : groups) {
        try { test(); std::cout << "PASS: Address history: " << name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL: Address history: " << name << ": " << error.what() << '\n'; }
    }
    return static_cast<int>(failures);
}
