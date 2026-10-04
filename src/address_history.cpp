#include "explorer/address_history.hpp"
#include <aclapi.h>
#include <shlobj.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>

namespace explorer {
namespace {
constexpr size_t maximumAddressLength = 32767;
constexpr size_t maximumHistoryBytes = maximumTypedAddresses * maximumAddressLength * 4 + 256;
constexpr char magic[] = "WXTypedAddressHistory1\n";
constexpr HRESULT invalidData = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
bool valid(std::wstring_view value) {
    return !value.empty() && value.size() <= maximumAddressLength && value.find(L'\0') == value.npos &&
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                            nullptr, 0, nullptr, nullptr) > 0;
}
bool same(std::wstring_view left, std::wstring_view right) {
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                                static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}
bool contains(std::span<const std::wstring> values, std::wstring_view value) {
    return std::any_of(values.begin(), values.end(), [&](const auto& candidate) { return same(candidate, value); });
}
bool validList(std::span<const std::wstring> values) {
    if (values.size() > maximumTypedAddresses) return false;
    for (size_t index = 0; index < values.size(); ++index)
        if (!valid(values[index]) || contains(values.first(index), values[index])) return false;
    return true;
}
void append32(std::string& data, size_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) data.push_back(static_cast<char>((value >> shift) & 255));
}
bool read32(std::string_view data, size_t& offset, size_t& value) {
    if (offset > data.size() || data.size() - offset < 4) return false;
    value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8)
        value |= static_cast<size_t>(static_cast<unsigned char>(data[offset++])) << shift;
    return true;
}
std::string encode(std::wstring_view value) {
    const auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                       result.data(), length, nullptr, nullptr);
    return result;
}
HRESULT decode(std::string_view data, std::vector<std::wstring>& result) {
    if (data.size() < sizeof(magic) - 1 || std::memcmp(data.data(), magic, sizeof(magic) - 1)) return invalidData;
    size_t offset = sizeof(magic) - 1, count = 0;
    if (!read32(data, offset, count) || count > maximumTypedAddresses) return invalidData;
    std::vector<std::wstring> values;
    for (size_t index = 0; index < count; ++index) {
        size_t bytes = 0;
        if (!read32(data, offset, bytes) || !bytes || bytes > maximumAddressLength * 4 || bytes > data.size() - offset)
            return invalidData;
        const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data.data() + offset,
                                              static_cast<int>(bytes), nullptr, 0);
        if (length <= 0 || static_cast<size_t>(length) > maximumAddressLength) return invalidData;
        std::wstring value(static_cast<size_t>(length), L'\0');
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data.data() + offset,
                               static_cast<int>(bytes), value.data(), length) || !valid(value) || contains(values, value))
            return invalidData;
        values.push_back(std::move(value)); offset += bytes;
    }
    if (offset != data.size()) return invalidData;
    result = std::move(values);
    return S_OK;
}
struct File {
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool remove = false;
    ~File() {
        if (handle == INVALID_HANDLE_VALUE) return;
        if (remove) {
            FILE_DISPOSITION_INFO disposition{TRUE};
            SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition));
        }
        CloseHandle(handle);
    }
};
HRESULT pathError(const std::filesystem::filesystem_error& error) {
    return HRESULT_FROM_WIN32(static_cast<DWORD>(error.code().value()));
}
HRESULT writeAtomic(const std::filesystem::path& path, const std::string& bytes) {
    File original;
    original.handle = CreateFileW(path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES | DELETE,
        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    FILE_BASIC_INFO basic{};
    FILE_ID_INFO identity{};
    std::unique_ptr<void, decltype(&LocalFree)> security(nullptr, &LocalFree);
    PACL dacl = nullptr;
    SECURITY_DESCRIPTOR_CONTROL control{};
    if (original.handle != INVALID_HANDLE_VALUE) {
        if (!GetFileInformationByHandleEx(original.handle, FileBasicInfo, &basic, sizeof(basic)) ||
            !GetFileInformationByHandleEx(original.handle, FileIdInfo, &identity, sizeof(identity)))
            return HRESULT_FROM_WIN32(GetLastError());
        if (basic.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_REPARSE_POINT))
            return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
        if (basic.FileAttributes & FILE_ATTRIBUTE_ENCRYPTED) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        PSECURITY_DESCRIPTOR raw = nullptr;
        const auto status = GetSecurityInfo(original.handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                            nullptr, nullptr, &dacl, nullptr, &raw);
        security.reset(raw);
        if (status) return HRESULT_FROM_WIN32(status);
        DWORD revision = 0;
        if (!GetSecurityDescriptorControl(raw, &control, &revision)) return HRESULT_FROM_WIN32(GetLastError());
    } else {
        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return HRESULT_FROM_WIN32(error);
    }
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return HRESULT_FROM_WIN32(static_cast<DWORD>(error.value()));
    File temporary;
    temporary.remove = true;
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        GUID id{}; wchar_t text[40]{};
        const auto hr = CoCreateGuid(&id);
        if (FAILED(hr)) return hr;
        if (!StringFromGUID2(id, text, ARRAYSIZE(text))) return E_FAIL;
        const auto name = path.parent_path() / (std::wstring(L"address-history-") + text + L".tmp");
        temporary.handle = CreateFileW(name.c_str(), GENERIC_WRITE | DELETE | WRITE_DAC,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (temporary.handle != INVALID_HANDLE_VALUE) break;
        const auto failure = GetLastError();
        if (failure != ERROR_FILE_EXISTS && failure != ERROR_ALREADY_EXISTS) return HRESULT_FROM_WIN32(failure);
    }
    if (temporary.handle == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES);
    size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD written = 0;
        if (!WriteFile(temporary.handle, bytes.data() + offset, static_cast<DWORD>(bytes.size() - offset), &written, nullptr))
            return HRESULT_FROM_WIN32(GetLastError());
        if (!written) return HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
        offset += written;
    }
    if (security) {
        const auto protection = control & SE_DACL_PROTECTED ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION;
        const auto status = SetSecurityInfo(temporary.handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | protection,
                                            nullptr, nullptr, dacl, nullptr);
        if (status) return HRESULT_FROM_WIN32(status);
        FILE_BASIC_INFO retained{};
        retained.CreationTime = basic.CreationTime;
        retained.FileAttributes = basic.FileAttributes & (FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN |
                                                        FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED);
        if (!retained.FileAttributes) retained.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        if (!SetFileInformationByHandle(temporary.handle, FileBasicInfo, &retained, sizeof(retained)))
            return HRESULT_FROM_WIN32(GetLastError());
    }
    if (!FlushFileBuffers(temporary.handle)) return HRESULT_FROM_WIN32(GetLastError());
    if (original.handle != INVALID_HANDLE_VALUE) {
        File current;
        current.handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        FILE_ID_INFO now{};
        if (current.handle == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandleEx(current.handle, FileIdInfo, &now, sizeof(now)))
            return HRESULT_FROM_WIN32(GetLastError());
        if (now.VolumeSerialNumber != identity.VolumeSerialNumber ||
            std::memcmp(now.FileId.Identifier, identity.FileId.Identifier, sizeof(identity.FileId.Identifier)))
            return HRESULT_FROM_WIN32(ERROR_RETRY);
    }
    const auto& name = path.native();
    const auto renameBytes = name.size() * sizeof(wchar_t);
    if (renameBytes > (std::numeric_limits<DWORD>::max)() - offsetof(FILE_RENAME_INFO, FileName) - sizeof(wchar_t)) return E_INVALIDARG;
    // The length excludes the explicit terminator used by Win32 translation.
    std::vector<BYTE> storage(offsetof(FILE_RENAME_INFO, FileName) + renameBytes + sizeof(wchar_t));
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    const bool replace = original.handle != INVALID_HANDLE_VALUE;
    rename->ReplaceIfExists = replace ? TRUE : FALSE;
    rename->FileNameLength = static_cast<DWORD>(renameBytes);
    std::memcpy(rename->FileName, name.data(), renameBytes);
    if (replace) {
        // The small per-app codec has no retained native-folder read handles.
        // Close its validation handle before ordinary atomic replacement;
        // readers denying deletion were already rejected by the preflight.
        CloseHandle(original.handle);
        original.handle = INVALID_HANDLE_VALUE;
    }
    if (!SetFileInformationByHandle(temporary.handle, FileRenameInfo, rename, static_cast<DWORD>(storage.size())))
        return HRESULT_FROM_WIN32(GetLastError());
    temporary.remove = false;
    return S_OK;
}
bool registryOrdinal(std::wstring_view name, unsigned& ordinal) {
    if (name.size() < 4 || CompareStringOrdinal(name.data(), 3, L"url", 3, TRUE) != CSTR_EQUAL || name[3] == L'0') return false;
    unsigned result = 0;
    for (const auto value : name.substr(3)) {
        if (value < L'0' || value > L'9' || result > 100000000) return false;
        result = result * 10 + static_cast<unsigned>(value - L'0');
    }
    ordinal = result;
    return result != 0;
}
} // namespace
bool typedAddressHistoryAllowed() {
    if (SHRestricted(REST_NORECENTDOCSHISTORY)) return false;
    DWORD showRecent = 1, bytes = sizeof(showRecent);
    const auto status = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer",
                                    L"ShowRecent", RRF_RT_REG_DWORD, nullptr, &showRecent, &bytes);
    // Folder Options' recent-files privacy choice also suppresses this host's
    // typed history. A missing value means the native default (enabled).
    return status == ERROR_SUCCESS ? showRecent != 0 : status != ERROR_ACCESS_DENIED;
}
std::filesystem::path addressHistoryPath() {
    const auto settings = preferencesPath();
    return settings.empty() ? std::filesystem::path{} : settings.parent_path() / L"address-history.dat";
}
bool rememberTypedAddress(std::vector<std::wstring>& addresses, const std::wstring& address) {
    if (!valid(address) || !validList(addresses)) return false;
    try {
        auto values = addresses;
        values.erase(std::remove_if(values.begin(), values.end(), [&](const auto& candidate) { return same(candidate, address); }), values.end());
        values.insert(values.begin(), address);
        if (values.size() > maximumTypedAddresses) values.resize(maximumTypedAddresses);
        if (values == addresses) return false;
        addresses.swap(values);
        return true;
    } catch (const std::bad_alloc&) { return false; }
}
HRESULT mergeTypedAddressHistory(std::vector<std::wstring>& addresses, std::span<const std::wstring> imported) {
    if (!validList(addresses) || imported.size() > 4096) return E_INVALIDARG;
    for (const auto& value : imported) if (!valid(value)) return E_INVALIDARG;
    try {
        auto values = addresses;
        for (const auto& value : imported)
            if (values.size() < maximumTypedAddresses && !contains(values, value)) values.push_back(value);
        addresses.swap(values);
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}
HRESULT loadAddressHistory(const std::filesystem::path& path, std::vector<std::wstring>* result) {
    if (!result) return E_POINTER;
    if (path.empty() || !path.is_absolute() || path.native().find(L'\0') != std::wstring::npos) return E_INVALIDARG;
    try {
        File file;
        file.handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (file.handle == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
        FILE_BASIC_INFO basic{}; LARGE_INTEGER length{};
        if (!GetFileInformationByHandleEx(file.handle, FileBasicInfo, &basic, sizeof(basic)) || !GetFileSizeEx(file.handle, &length))
            return HRESULT_FROM_WIN32(GetLastError());
        if (basic.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return invalidData;
        if (length.QuadPart < 0 || static_cast<ULONGLONG>(length.QuadPart) > maximumHistoryBytes) return invalidData;
        std::string data(static_cast<size_t>(length.QuadPart) + 1, '\0');
        DWORD bytes = 0;
        if (!ReadFile(file.handle, data.data(), static_cast<DWORD>(data.size()), &bytes, nullptr)) return HRESULT_FROM_WIN32(GetLastError());
        if (bytes != static_cast<DWORD>(length.QuadPart)) return invalidData;
        data.resize(bytes);
        std::vector<std::wstring> values;
        const auto hr = decode(data, values);
        if (SUCCEEDED(hr)) *result = std::move(values);
        return hr;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}
HRESULT saveAddressHistory(const std::filesystem::path& path, std::span<const std::wstring> addresses) {
    if (path.empty() || !path.is_absolute() || path.native().find(L'\0') != std::wstring::npos || !validList(addresses)) return E_INVALIDARG;
    try {
        std::string data(magic, sizeof(magic) - 1); append32(data, addresses.size());
        for (const auto& value : addresses) { const auto bytes = encode(value); append32(data, bytes.size()); data += bytes; }
        return writeAtomic(path, data);
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (const std::filesystem::filesystem_error& error) { return pathError(error); }
}
HRESULT readTypedAddressRegistry(HKEY key, std::vector<std::wstring>* result) {
    if (!result) return E_POINTER;
    if (!key) return E_INVALIDARG;
    try {
        DWORD count = 0;
        const auto queried = RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                                             &count, nullptr, nullptr, nullptr, nullptr);
        if (queried) return HRESULT_FROM_WIN32(queried);
        if (count > 4096) return invalidData;
        std::vector<std::pair<unsigned, std::wstring>> ordered;
        for (DWORD index = 0; index < count; ++index) {
            wchar_t name[256]{}; DWORD length = ARRAYSIZE(name), type = 0, bytes = 0;
            auto status = RegEnumValueW(key, index, name, &length, nullptr, &type, nullptr, &bytes);
            if (status == ERROR_NO_MORE_ITEMS) break;
            if (status == ERROR_MORE_DATA) continue;
            if (status) return HRESULT_FROM_WIN32(status);
            unsigned ordinal = 0;
            if (!registryOrdinal(std::wstring_view(name, length), ordinal) ||
                (type != REG_SZ && type != REG_EXPAND_SZ) || !bytes || bytes % sizeof(wchar_t) ||
                bytes > (maximumAddressLength + 1) * sizeof(wchar_t)) continue;
            std::wstring value(bytes / sizeof(wchar_t), L'\0');
            const auto capacity = bytes;
            status = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &bytes);
            if (status == ERROR_FILE_NOT_FOUND || status == ERROR_MORE_DATA) continue;
            if (status) return HRESULT_FROM_WIN32(status);
            if ((type != REG_SZ && type != REG_EXPAND_SZ) || bytes > capacity || bytes % sizeof(wchar_t)) continue;
            value.resize(bytes / sizeof(wchar_t));
            if (!value.empty() && value.back() == L'\0') value.pop_back();
            if (valid(value)) ordered.emplace_back(ordinal, std::move(value));
        }
        std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
        std::vector<std::wstring> values;
        for (const auto& [ordinal, value] : ordered) {
            (void)ordinal;
            if (values.size() < maximumTypedAddresses && !contains(values, value)) values.push_back(value);
        }
        *result = std::move(values);
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}
HRESULT loadWindowsTypedAddresses(std::vector<std::wstring>* result) {
    if (!result) return E_POINTER;
    HKEY key = nullptr;
    const auto status = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\TypedPaths",
                                     0, KEY_QUERY_VALUE, &key);
    if (status == ERROR_FILE_NOT_FOUND) { result->clear(); return S_FALSE; }
    if (status) return HRESULT_FROM_WIN32(status);
    const auto hr = readTypedAddressRegistry(key, result);
    RegCloseKey(key);
    return hr;
}
} // namespace explorer
