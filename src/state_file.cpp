#include "state_file.hpp"
#include "file_security.hpp"

#include <objbase.h>
#include <winioctl.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

namespace explorer {
namespace {
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
HRESULT failure() noexcept {
    const auto error = GetLastError();
    return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
}
bool sameIdentity(const FILE_ID_INFO& left, const FILE_ID_INFO& right) noexcept {
    return left.VolumeSerialNumber == right.VolumeSerialNumber &&
        std::memcmp(left.FileId.Identifier, right.FileId.Identifier, sizeof(left.FileId.Identifier)) == 0;
}
HRESULT write(const std::filesystem::path& path, std::span<const BYTE> bytes) {
    std::error_code error;
    const auto destination = std::filesystem::absolute(path, error);
    if (error) return HRESULT_FROM_WIN32(static_cast<DWORD>(error.value()));
    File original;
    original.handle = CreateFileW(destination.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES | DELETE,
        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    const bool replacing = original.handle != INVALID_HANDLE_VALUE;
    FILE_BASIC_INFO basic{};
    FILE_ID_INFO identity{};
    file_security::Descriptor security;
    if (replacing) {
        if (!GetFileInformationByHandleEx(original.handle, FileBasicInfo, &basic, sizeof(basic)) ||
            !GetFileInformationByHandleEx(original.handle, FileIdInfo, &identity, sizeof(identity))) return failure();
        if (basic.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_REPARSE_POINT))
            return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
        if (basic.FileAttributes & FILE_ATTRIBUTE_ENCRYPTED) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        const auto status = file_security::read(original.handle, security);
        if (FAILED(status)) return status;
    } else {
        const auto missing = GetLastError();
        if (missing != ERROR_FILE_NOT_FOUND && missing != ERROR_PATH_NOT_FOUND) return HRESULT_FROM_WIN32(missing);
    }
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error) return HRESULT_FROM_WIN32(static_cast<DWORD>(error.value()));
    File temporary;
    temporary.remove = true;
    auto attributes = file_security::attributes(security);
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        GUID id{}; wchar_t name[40]{};
        const auto status = CoCreateGuid(&id);
        if (FAILED(status)) return status;
        if (!StringFromGUID2(id, name, ARRAYSIZE(name))) return E_FAIL;
        const auto stage = destination.parent_path() / (std::wstring(L".native-state-") + name + L".tmp");
        temporary.handle = CreateFileW(stage.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE | WRITE_DAC,
            FILE_SHARE_READ | FILE_SHARE_DELETE, replacing ? &attributes : nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (temporary.handle != INVALID_HANDLE_VALUE) break;
        const auto collision = GetLastError();
        if (collision != ERROR_FILE_EXISTS && collision != ERROR_ALREADY_EXISTS) return HRESULT_FROM_WIN32(collision);
    }
    if (temporary.handle == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES);
    if (replacing) {
        const auto status = file_security::verifyCreated(temporary.handle, security);
        if (FAILED(status)) return status; // Stage is still empty.
        USHORT compression = COMPRESSION_FORMAT_NONE;
        DWORD returned = 0;
        if (DeviceIoControl(original.handle, FSCTL_GET_COMPRESSION, nullptr, 0,
            &compression, sizeof(compression), &returned, nullptr)) {
            if (!DeviceIoControl(temporary.handle, FSCTL_SET_COMPRESSION, &compression, sizeof(compression),
                nullptr, 0, &returned, nullptr)) return failure();
        } else if (basic.FileAttributes & FILE_ATTRIBUTE_COMPRESSED) return failure();
    }
    size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD written = 0;
        const auto count = static_cast<DWORD>(std::min(bytes.size() - offset,
            static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
        if (!WriteFile(temporary.handle, bytes.data() + offset, count, &written, nullptr)) return failure();
        if (!written) return HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
        offset += written;
    }
    if (replacing) {
        FILE_BASIC_INFO retained{};
        retained.CreationTime = basic.CreationTime;
        retained.FileAttributes = basic.FileAttributes & (FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN |
            FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | FILE_ATTRIBUTE_TEMPORARY);
        if (!retained.FileAttributes) retained.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        if (!SetFileInformationByHandle(temporary.handle, FileBasicInfo, &retained, sizeof(retained))) return failure();
    }
    if (!FlushFileBuffers(temporary.handle)) return failure();
    if (replacing) {
        File current;
        current.handle = CreateFileW(destination.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        FILE_ID_INFO now{};
        if (current.handle == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandleEx(current.handle, FileIdInfo, &now, sizeof(now))) return failure();
        if (!sameIdentity(identity, now)) return HRESULT_FROM_WIN32(ERROR_RETRY);
        file_security::Descriptor originalNow, stagedNow;
        auto status = file_security::read(original.handle, originalNow);
        if (SUCCEEDED(status)) status = file_security::read(temporary.handle, stagedNow);
        if (FAILED(status)) return status;
        if (!file_security::equal(security, originalNow) || !file_security::equal(security, stagedNow))
            return HRESULT_FROM_WIN32(ERROR_RETRY);
    }
    const auto& name = destination.native();
    if (name.size() > ((std::numeric_limits<DWORD>::max)() - offsetof(FILE_RENAME_INFO, FileName) - sizeof(wchar_t)) / sizeof(wchar_t))
        return E_INVALIDARG;
    const auto length = name.size() * sizeof(wchar_t);
    std::vector<BYTE> storage(offsetof(FILE_RENAME_INFO, FileName) + length + sizeof(wchar_t));
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    // Keep the original open through publication. POSIX replacement preserves
    // an existing share-delete reader's old stream while updating this name.
    rename->Flags = replacing ? FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS : 0;
    rename->FileNameLength = static_cast<DWORD>(length);
    std::memcpy(rename->FileName, name.data(), length);
    if (!SetFileInformationByHandle(temporary.handle, FileRenameInfoEx, rename, static_cast<DWORD>(storage.size()))) return failure();
    temporary.remove = false;
    return S_OK;
}
} // namespace

HRESULT writeStateFileAtomic(const std::filesystem::path& path, std::span<const BYTE> bytes) noexcept {
    if (path.empty() || path.native().find(L'\0') != std::wstring::npos || bytes.size() > (std::numeric_limits<DWORD>::max)())
        return E_INVALIDARG;
    try { return write(path, bytes); }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (const std::filesystem::filesystem_error& error) { return HRESULT_FROM_WIN32(static_cast<DWORD>(error.code().value())); }
    catch (...) { return E_FAIL; }
}

} // namespace explorer
