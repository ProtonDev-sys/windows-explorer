#include "explorer/search_presentation_store.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string_view>
#include <aclapi.h>
#include <memory>

namespace explorer {
namespace {
constexpr char magic[] = "WXSearchViewCompanion1\n";
constexpr size_t maximumBytes = 132000;
constexpr HRESULT invalidData = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
struct File {
    HANDLE value = INVALID_HANDLE_VALUE;
    bool remove = false;
    ~File() {
        if (value == INVALID_HANDLE_VALUE) return;
        if (remove) { FILE_DISPOSITION_INFO disposition{TRUE}; SetFileInformationByHandle(value, FileDispositionInfo, &disposition, sizeof(disposition)); }
        CloseHandle(value);
    }
};
struct QueryIdentity {
    ULONGLONG volume = 0;
    std::array<BYTE, 16> file{};
    LONGLONG size = 0, modified = 0, changed = 0;
    std::wstring canonical;
    bool operator==(const QueryIdentity&) const = default;
};
bool validPath(const std::filesystem::path& path) {
    return !path.empty() && path.is_absolute() && path.native().size() <= 32767 && path.native().find(L'\0') == std::wstring::npos;
}
HRESULT identify(const std::filesystem::path& path, QueryIdentity& result) {
    if (!validPath(path) || _wcsicmp(path.extension().c_str(), L".search-ms") != 0) return E_INVALIDARG;
    File file;
    file.value = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file.value == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    FILE_ID_INFO id{}; FILE_STANDARD_INFO standard{}; FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandleEx(file.value, FileIdInfo, &id, sizeof(id)) ||
        !GetFileInformationByHandleEx(file.value, FileStandardInfo, &standard, sizeof(standard)) ||
        !GetFileInformationByHandleEx(file.value, FileBasicInfo, &basic, sizeof(basic))) return HRESULT_FROM_WIN32(GetLastError());
    if (basic.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    const auto length = GetFinalPathNameByHandleW(file.value, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!length || length > 32768) return length ? E_INVALIDARG : HRESULT_FROM_WIN32(GetLastError());
    std::wstring canonical(length, L'\0');
    const auto actual = GetFinalPathNameByHandleW(file.value, canonical.data(), length, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!actual || actual >= length) return HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_RETRY);
    canonical.resize(actual);
    QueryIdentity candidate;
    candidate.volume = id.VolumeSerialNumber; std::copy(std::begin(id.FileId.Identifier), std::end(id.FileId.Identifier), candidate.file.begin());
    candidate.size = standard.EndOfFile.QuadPart; candidate.modified = basic.LastWriteTime.QuadPart; candidate.changed = basic.ChangeTime.QuadPart;
    candidate.canonical = std::move(canonical); result = std::move(candidate); return S_OK;
}
std::wstring key(const QueryIdentity& id) {
    constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring name = L"view-";
    for (int shift = 60; shift >= 0; shift -= 4) name += digits[(id.volume >> shift) & 15];
    name += L'-';
    for (const auto byte : id.file) { name += digits[byte >> 4]; name += digits[byte & 15]; }
    return name + L".dat";
}
void append(std::string& output, ULONGLONG number, unsigned bytes) {
    for (unsigned index = 0; index < bytes; ++index) output.push_back(static_cast<char>((number >> (index * 8)) & 255));
}
bool number(std::string_view input, size_t& offset, ULONGLONG& result, unsigned bytes) {
    if (offset > input.size() || bytes > input.size() - offset) return false;
    result = 0;
    for (unsigned index = 0; index < bytes; ++index) result |= static_cast<ULONGLONG>(static_cast<unsigned char>(input[offset++])) << (index * 8);
    return true;
}
HRESULT decode(std::string_view input, QueryIdentity& identity, SearchViewPresentation& presentation) {
    if (input.size() < sizeof(magic) - 1 || std::memcmp(input.data(), magic, sizeof(magic) - 1)) return invalidData;
    size_t offset = sizeof(magic) - 1; ULONGLONG value = 0;
    if (!number(input, offset, identity.volume, 8)) return invalidData;
    for (auto& byte : identity.file) { if (!number(input, offset, value, 1)) return invalidData; byte = static_cast<BYTE>(value); }
    if (!number(input, offset, value, 8)) return invalidData; identity.size = static_cast<LONGLONG>(value);
    if (!number(input, offset, value, 8)) return invalidData; identity.modified = static_cast<LONGLONG>(value);
    if (!number(input, offset, value, 8)) return invalidData; identity.changed = static_cast<LONGLONG>(value);
    if (!number(input, offset, value, 4) || value > static_cast<unsigned>(SearchViewMode::Content)) return invalidData;
    presentation.mode = static_cast<SearchViewMode>(value);
    if (!number(input, offset, value, 4) || value < 16 || value > 256) return invalidData;
    presentation.iconSize = static_cast<int>(value);
    if (!number(input, offset, value, 4) || !value || value > 32767 || value * 2 != input.size() - offset) return invalidData;
    identity.canonical.clear(); identity.canonical.reserve(static_cast<size_t>(value));
    const auto count = value;
    for (ULONGLONG index = 0; index < count; ++index) {
        if (!number(input, offset, value, 2)) return invalidData;
        identity.canonical.push_back(static_cast<wchar_t>(value));
    }
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, identity.canonical.data(), static_cast<int>(identity.canonical.size()), nullptr, 0, nullptr, nullptr) ||
        identity.canonical.find(L'\0') != std::wstring::npos || !validPath(identity.canonical)) return invalidData;
    return S_OK;
}
HRESULT read(const std::filesystem::path& path, std::string& result) {
    File file;
    file.value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file.value == INVALID_HANDLE_VALUE) { const auto error = GetLastError(); return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? S_FALSE : HRESULT_FROM_WIN32(error); }
    FILE_BASIC_INFO basic{}; LARGE_INTEGER length{};
    if (!GetFileInformationByHandleEx(file.value, FileBasicInfo, &basic, sizeof(basic)) || !GetFileSizeEx(file.value, &length)) return HRESULT_FROM_WIN32(GetLastError());
    if ((basic.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || length.QuadPart <= 0 || length.QuadPart > maximumBytes) return invalidData;
    std::string bytes(static_cast<size_t>(length.QuadPart), '\0'); DWORD received = 0;
    if (!ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &received, nullptr)) return HRESULT_FROM_WIN32(GetLastError());
    if (received != bytes.size()) return invalidData;
    result = std::move(bytes); return S_OK;
}
HRESULT write(const std::filesystem::path& directory, const std::wstring& name, const std::string& bytes) {
    std::error_code error; std::filesystem::create_directories(directory, error);
    if (error) return HRESULT_FROM_WIN32(static_cast<DWORD>(error.value()));
    File folder;
    // Keep this validated destination directory from being renamed while its
    // create-new temporary and atomic publication use the same absolute path.
    folder.value = CreateFileW(directory.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (folder.value == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    FILE_BASIC_INFO directoryInfo{};
    if (!GetFileInformationByHandleEx(folder.value, FileBasicInfo, &directoryInfo, sizeof(directoryInfo))) return HRESULT_FROM_WIN32(GetLastError());
    if (!(directoryInfo.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (directoryInfo.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    const auto target = directory / name;
    std::string original;
    auto hr = read(target, original);
    if (FAILED(hr)) return hr;
    const bool exists = hr == S_OK;
    File retained;
    FILE_BASIC_INFO retainedBasic{};
    PACL retainedDacl = nullptr;
    SECURITY_DESCRIPTOR_CONTROL retainedControl{};
    std::unique_ptr<void, decltype(&LocalFree)> retainedSecurity(nullptr, &LocalFree);
    if (exists) {
        QueryIdentity prior; SearchViewPresentation old;
        if (FAILED(hr = decode(original, prior, old)) || key(prior) != name) return FAILED(hr) ? hr : invalidData;
        retained.value = CreateFileW(target.c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (retained.value == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
        FILE_BASIC_INFO basic{};
        if (!GetFileInformationByHandleEx(retained.value, FileBasicInfo, &basic, sizeof(basic))) return HRESULT_FROM_WIN32(GetLastError());
        if (basic.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_READONLY)) return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
        if (basic.FileAttributes & FILE_ATTRIBUTE_ENCRYPTED) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        retainedBasic = basic;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        const auto security = GetSecurityInfo(retained.value, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                             nullptr, nullptr, &retainedDacl, nullptr, &descriptor);
        retainedSecurity.reset(descriptor);
        if (security) return HRESULT_FROM_WIN32(security);
        DWORD revision = 0;
        if (!GetSecurityDescriptorControl(descriptor, &retainedControl, &revision)) return HRESULT_FROM_WIN32(GetLastError());
        LARGE_INTEGER length{};
        if (!GetFileSizeEx(retained.value, &length)) return HRESULT_FROM_WIN32(GetLastError());
        if (length.QuadPart != static_cast<LONGLONG>(original.size())) return HRESULT_FROM_WIN32(ERROR_RETRY);
        std::string checked(original.size(), '\0'); DWORD received = 0;
        if (!ReadFile(retained.value, checked.data(), static_cast<DWORD>(checked.size()), &received, nullptr)) return HRESULT_FROM_WIN32(GetLastError());
        if (received != checked.size() || checked != original) return HRESULT_FROM_WIN32(ERROR_RETRY);
    }
    GUID id{}; wchar_t text[40]{}; if (FAILED(hr = CoCreateGuid(&id))) return hr;
    if (!StringFromGUID2(id, text, 40)) return E_FAIL;
    File temporary; temporary.remove = true;
    const auto temporaryPath = directory / (std::wstring(L"view-write-") + text + L".tmp");
    temporary.value = CreateFileW(temporaryPath.c_str(), GENERIC_WRITE | DELETE | WRITE_DAC, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (temporary.value == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    DWORD written = 0;
    if (!WriteFile(temporary.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)) return HRESULT_FROM_WIN32(GetLastError());
    if (written != bytes.size()) return HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
    if (retainedSecurity) {
        const auto protection = retainedControl & SE_DACL_PROTECTED ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION;
        const auto security = SetSecurityInfo(temporary.value, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | protection,
                                             nullptr, nullptr, retainedDacl, nullptr);
        if (security) return HRESULT_FROM_WIN32(security);
        FILE_BASIC_INFO kept{}; kept.CreationTime = retainedBasic.CreationTime;
        kept.FileAttributes = retainedBasic.FileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED);
        if (!kept.FileAttributes) kept.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        if (!SetFileInformationByHandle(temporary.value, FileBasicInfo, &kept, sizeof(kept))) return HRESULT_FROM_WIN32(GetLastError());
    }
    if (!FlushFileBuffers(temporary.value)) return HRESULT_FROM_WIN32(GetLastError());
    const auto& finalName = target.native();
    const auto length = finalName.size() * sizeof(wchar_t);
    std::vector<BYTE> buffer(offsetof(FILE_RENAME_INFO, FileName) + length + sizeof(wchar_t));
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    rename->ReplaceIfExists = exists ? TRUE : FALSE;
    rename->FileNameLength = static_cast<DWORD>(length); std::memcpy(rename->FileName, finalName.data(), length);
    if (retained.value != INVALID_HANDLE_VALUE) { CloseHandle(retained.value); retained.value = INVALID_HANDLE_VALUE; }
    if (!SetFileInformationByHandle(temporary.value, FileRenameInfo, rename, static_cast<DWORD>(buffer.size()))) return HRESULT_FROM_WIN32(GetLastError());
    temporary.remove = false; return S_OK;
}
} // namespace
std::filesystem::path searchPresentationDirectory() {
    const auto preferences = preferencesPath();
    return preferences.empty() ? std::filesystem::path{} : preferences.parent_path() / L"saved-search-views";
}
HRESULT saveSearchPresentationCompanion(const std::filesystem::path& query, const std::filesystem::path& directory,
                                       const SearchViewPresentation& actual) {
    if (!validPath(directory) || !actual.mode || !actual.iconSize) return E_INVALIDARG;
    try {
        auto hr = validateSearchViewPresentation(actual); if (FAILED(hr)) return hr;
        QueryIdentity identity; if (FAILED(hr = identify(query, identity))) return hr;
        std::string bytes(magic, sizeof(magic) - 1);
        append(bytes, identity.volume, 8); for (const auto value : identity.file) append(bytes, value, 1);
        append(bytes, static_cast<ULONGLONG>(identity.size), 8); append(bytes, static_cast<ULONGLONG>(identity.modified), 8); append(bytes, static_cast<ULONGLONG>(identity.changed), 8);
        append(bytes, static_cast<unsigned>(*actual.mode), 4); append(bytes, static_cast<unsigned>(*actual.iconSize), 4); append(bytes, identity.canonical.size(), 4);
        for (const auto value : identity.canonical) append(bytes, value, 2);
        if (bytes.size() > maximumBytes) return invalidData;
        hr = write(directory, key(identity), bytes); if (FAILED(hr)) return hr;
        QueryIdentity current; if (FAILED(hr = identify(query, current))) return hr;
        return current == identity ? S_OK : HRESULT_FROM_WIN32(ERROR_RETRY);
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (const std::filesystem::filesystem_error&) { return E_INVALIDARG; }
}
HRESULT loadSearchPresentationCompanion(const std::filesystem::path& query, const std::filesystem::path& directory,
                                       SearchViewPresentation* result) {
    if (!result) return E_POINTER;
    if (!validPath(directory)) return E_INVALIDARG;
    try {
        QueryIdentity current; auto hr = identify(query, current); if (FAILED(hr)) return hr;
        std::string bytes; hr = read(directory / key(current), bytes); if (hr != S_OK) return hr;
        QueryIdentity stored; SearchViewPresentation presentation;
        if (FAILED(hr = decode(bytes, stored, presentation))) return hr;
        if (!(current == stored)) return S_FALSE;
        QueryIdentity final;
        if (FAILED(hr = identify(query, final))) return hr;
        if (!(final == current)) return S_FALSE;
        auto candidate = *result; candidate.mode = presentation.mode; candidate.iconSize = presentation.iconSize;
        if (FAILED(hr = validateSearchViewPresentation(candidate))) return hr;
        *result = std::move(candidate); return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (const std::filesystem::filesystem_error&) { return E_INVALIDARG; }
}
} // namespace explorer
