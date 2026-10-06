#include "explorer/preview_diagnostics.hpp"
#include "explorer/headless_visual.hpp"
#include <objbase.h>
#include <propsys.h>
#include <shobjidl.h>
#include <shlguid.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cwchar>
#include <new>
#include <utility>

namespace explorer {
namespace {
using Microsoft::WRL::ComPtr;
constexpr HRESULT invalidData = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
HRESULT nativeError(DWORD fallback = ERROR_GEN_FAILURE) noexcept {
    const auto error = GetLastError();
    return HRESULT_FROM_WIN32(error ? error : fallback);
}
struct RegistryKey {
    HKEY value = nullptr;
    ~RegistryKey() { if (value) RegCloseKey(value); }
};
struct NativeFile {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~NativeFile() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct ShellText {
    PWSTR value = nullptr;
    ~ShellText() { CoTaskMemFree(value); }
};
HRESULT objectName(HANDLE handle, std::wstring& result) {
    DWORD bytes = 0;
    SetLastError(ERROR_SUCCESS);
    if (GetUserObjectInformationW(handle, UOI_NAME, nullptr, 0, &bytes)) return invalidData;
    const auto error = GetLastError();
    if (error != ERROR_INSUFFICIENT_BUFFER) return HRESULT_FROM_WIN32(error ? error : ERROR_INVALID_DATA);
    if (bytes < sizeof(wchar_t) || bytes > 65536 || bytes % sizeof(wchar_t)) return invalidData;
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    DWORD returned = 0;
    if (!GetUserObjectInformationW(handle, UOI_NAME, value.data(), bytes, &returned)) return nativeError();
    if (returned > bytes || returned < sizeof(wchar_t) || returned % sizeof(wchar_t) ||
        value[returned / sizeof(wchar_t) - 1] != L'\0') return invalidData;
    const auto length = returned / sizeof(wchar_t) - 1;
    if (!length || value.find(L'\0') != length) return invalidData;
    value.resize(length);
    result = std::move(value);
    return S_OK;
}
HRESULT verifyContext(const PreviewDiagnosticContext& context) {
    if (!context.desktop || !context.station || context.desktopName.empty() || context.inputName.empty() ||
        context.desktopName == context.inputName || GetProcessWindowStation() != context.station ||
        GetThreadDesktop(GetCurrentThreadId()) != context.desktop) return E_ACCESSDENIED;
    std::wstring actual;
    auto hr = objectName(context.desktop, actual);
    if (FAILED(hr)) return hr;
    if (actual != context.desktopName) return E_ACCESSDENIED;
    struct InputDesktop {
        HDESK value = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
        ~InputDesktop() { if (value) CloseDesktop(value); }
    } input;
    if (!input.value) return nativeError();
    hr = objectName(input.value, actual);
    return FAILED(hr) ? hr : actual == context.inputName ? S_OK : E_ACCESSDENIED;
}
struct Budget {
    const PreviewDiagnosticContext& context;
    ULONGLONG deadline;
    const std::atomic_bool* cancelled;
    HRESULT& status;
    HRESULT check() {
        if (FAILED(status)) return status;
        if (cancelled && cancelled->load(std::memory_order_relaxed))
            return status = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        if (!deadline || GetTickCount64() >= deadline)
            return status = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        const auto hr = verifyContext(context);
        if (FAILED(hr)) status = hr;
        return hr;
    }
};
HRESULT inspectRegistryValue(Budget& budget, HKEY root, const wchar_t* path,
    const wchar_t* name, bool readDword, PreviewRegistryReadback& result) {
    auto hr = budget.check();
    if (FAILED(hr)) return hr;
    RegistryKey key;
    const auto opened = RegOpenKeyExW(root, path, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key.value);
    result.keyRead = HRESULT_FROM_WIN32(opened);
    result.keyPresent = opened == ERROR_SUCCESS;
    if (opened != ERROR_SUCCESS) return budget.check();
    hr = budget.check();
    if (FAILED(hr)) return hr;
    const auto queried = RegQueryValueExW(key.value, name, nullptr, &result.type, nullptr, &result.bytes);
    result.valueRead = HRESULT_FROM_WIN32(queried);
    result.valuePresent = queried == ERROR_SUCCESS;
    if (queried != ERROR_SUCCESS || !readDword) return budget.check();
    if (result.type != REG_DWORD || result.bytes != sizeof(DWORD)) {
        result.dataRead = HRESULT_FROM_WIN32(ERROR_DATATYPE_MISMATCH);
        return budget.check();
    }
    hr = budget.check();
    if (FAILED(hr)) return hr;
    DWORD type = REG_NONE, bytes = sizeof(DWORD), value = 0;
    const auto read = RegQueryValueExW(key.value, name, nullptr, &type,
        reinterpret_cast<BYTE*>(&value), &bytes);
    result.dataRead = HRESULT_FROM_WIN32(read);
    if (read == ERROR_SUCCESS && (type != result.type || bytes != result.bytes)) result.dataRead = invalidData;
    if (result.dataRead == S_OK) { result.value = value; result.dwordValid = true; }
    return budget.check();
}
bool sameIdentity(const FILE_ID_INFO& left, const FILE_ID_INFO& right) noexcept {
    return left.VolumeSerialNumber == right.VolumeSerialNumber &&
        std::memcmp(left.FileId.Identifier, right.FileId.Identifier, sizeof(left.FileId.Identifier)) == 0;
}
bool sameMetadata(const FILE_BASIC_INFO& left, const FILE_BASIC_INFO& right) noexcept {
    // Reading can legitimately update LastAccessTime. Preserve and report the
    // complete native readback; compare creation/write/change/attributes.
    return left.CreationTime.QuadPart == right.CreationTime.QuadPart &&
        left.LastWriteTime.QuadPart == right.LastWriteTime.QuadPart &&
        left.ChangeTime.QuadPart == right.ChangeTime.QuadPart && left.FileAttributes == right.FileAttributes;
}
HRESULT readSource(Budget& budget, HANDLE file, FILE_ID_INFO& identity, FILE_BASIC_INFO& basic) {
    auto hr = budget.check();
    if (FAILED(hr)) return hr;
    if (!GetFileInformationByHandleEx(file, FileIdInfo, &identity, sizeof(identity))) return nativeError();
    hr = budget.check();
    if (FAILED(hr)) return hr;
    if (!GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic))) return nativeError();
    constexpr DWORD unsafe = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE |
        FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS;
    if (basic.FileAttributes & unsafe) return E_ACCESSDENIED;
    hr = budget.check();
    if (FAILED(hr)) return hr;
    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard))) return nativeError();
    if (standard.Directory || standard.DeletePending ||
        standard.EndOfFile.QuadPart != static_cast<LONGLONG>(ownedPreviewRtfBytes))
        return invalidData;
    return budget.check();
}
HRESULT readFileBytes(Budget& budget, HANDLE file, std::span<const BYTE> expected, bool& matches) {
    auto hr = budget.check();
    if (FAILED(hr)) return hr;
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) return nativeError();
    std::array<BYTE, ownedPreviewRtfBytes + 1> bytes{};
    DWORD total = 0;
    while (total < bytes.size()) {
        hr = budget.check();
        if (FAILED(hr)) return hr;
        DWORD copied = 0;
        if (!ReadFile(file, bytes.data() + total, static_cast<DWORD>(bytes.size()) - total, &copied, nullptr))
            return nativeError();
        if (copied > bytes.size() - total) return invalidData;
        total += copied;
        if (!copied) break;
    }
    matches = total == expected.size() && std::equal(expected.begin(), expected.end(), bytes.begin());
    hr = budget.check();
    return FAILED(hr) ? hr : matches ? S_OK : invalidData;
}
HRESULT selectedPath(IShellItem* item, ShellText& path) {
    const auto hr = item->GetDisplayName(SIGDN_FILESYSPATH, &path.value);
    if (FAILED(hr)) return hr;
    if (!path.value || !path.value[0] || wcsnlen_s(path.value, 32768) >= 32768) return invalidData;
    return S_OK;
}
} // namespace

HRESULT capturePreviewDiagnosticContext(const PrivateDesktop& desktop, PreviewDiagnosticContext* result) noexcept {
    if (!result) return E_POINTER;
    try {
        if (PrivateDesktop::current() != &desktop) return E_ACCESSDENIED;
        auto hr = desktop.verifyIsolation();
        if (FAILED(hr)) return hr;
        PreviewDiagnosticContext candidate;
        candidate.desktop = GetThreadDesktop(GetCurrentThreadId());
        candidate.station = GetProcessWindowStation();
        candidate.desktopName = desktop.name();
        candidate.inputName = desktop.originalInputName();
        hr = verifyContext(candidate);
        if (SUCCEEDED(hr)) *result = std::move(candidate);
        return hr;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (...) { return E_FAIL; }
}

HRESULT inspectPreviewPolicies(const PreviewDiagnosticContext& context, REFCLSID associationHandler,
    ULONGLONG deadline, PreviewPolicySnapshot* result, const std::atomic_bool* cancelled) noexcept {
    if (!result) return E_POINTER;
    *result = {};
    try {
        Budget budget{context, deadline, cancelled, result->budgetStatus};
        result->guardBefore = budget.check();
        if (FAILED(result->guardBefore)) return result->completed = result->guardBefore;
        wchar_t classId[40]{};
        if (IsEqualGUID(associationHandler, GUID_NULL) || !StringFromGUID2(associationHandler, classId, 40))
            return result->completed = E_INVALIDARG;
        constexpr auto policies = L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer";
        constexpr auto handlers = L"Software\\Microsoft\\Windows\\CurrentVersion\\PreviewHandlers";
        constexpr auto approved = L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved";
        struct Query { HKEY root; const wchar_t* path; const wchar_t* value; bool dword; PreviewRegistryReadback* output; };
        const std::array<Query, 9> queries{{
            {HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"ShowPreviewHandlers", true, &result->showPreviewHandlers},
            {HKEY_CURRENT_USER, policies, L"NoReadingPane", true, &result->userNoReadingPane},
            {HKEY_LOCAL_MACHINE, policies, L"NoReadingPane", true, &result->machineNoReadingPane},
            {HKEY_CURRENT_USER, policies, L"EnforceShellExtensionSecurity", true, &result->userEnforceShellExtensionSecurity},
            {HKEY_LOCAL_MACHINE, policies, L"EnforceShellExtensionSecurity", true, &result->machineEnforceShellExtensionSecurity},
            {HKEY_CURRENT_USER, handlers, classId, false, &result->userPreviewHandlers},
            {HKEY_LOCAL_MACHINE, handlers, classId, false, &result->machinePreviewHandlers},
            {HKEY_CURRENT_USER, approved, classId, false, &result->userApproved},
            {HKEY_LOCAL_MACHINE, approved, classId, false, &result->machineApproved}
        }};
        for (const auto& query : queries) {
            const auto hr = inspectRegistryValue(budget, query.root, query.path, query.value, query.dword, *query.output);
            if (FAILED(hr)) return result->completed = hr;
        }
        result->guardAfter = budget.check();
        return result->completed = result->guardAfter;
    } catch (const std::bad_alloc&) { return result->completed = E_OUTOFMEMORY; }
      catch (...) { return result->completed = E_FAIL; }
}

HRESULT inspectOwnedPreviewStream(const PreviewDiagnosticContext& context, IShellItem* actualSelected,
    const FILE_ID_INFO& expectedIdentity, std::span<const BYTE> expectedBytes, ULONGLONG deadline,
    IInitializeWithStream* freshApprovedInitializer, IStream** retainedStream, PreviewStreamReadback* result,
    const std::atomic_bool* cancelled, const FILE_BASIC_INFO* originalBasic) noexcept {
    if (!result || !retainedStream || !actualSelected) return E_POINTER;
    if (*retainedStream || expectedBytes.size() != ownedPreviewRtfBytes || !expectedBytes.data()) return E_INVALIDARG;
    *result = {};
    try {
        Budget budget{context, deadline, cancelled, result->budgetStatus};
        result->guardBefore = budget.check();
        if (FAILED(result->guardBefore)) return result->completed = result->guardBefore;
        APTTYPE type{}; APTTYPEQUALIFIER qualifier{};
        result->apartmentRead = CoGetApartmentType(&type, &qualifier);
        if (FAILED(result->apartmentRead)) return result->completed = result->apartmentRead;
        if (type != APTTYPE_STA && type != APTTYPE_MAINSTA) return result->completed = E_ACCESSDENIED;
        ComPtr<IShellItem> selected = actualSelected;
        ComPtr<IInitializeWithStream> initializer = freshApprovedInitializer;
        auto hr = budget.check();
        if (FAILED(hr)) return result->completed = hr;
        constexpr SFGAOF requested = SFGAO_FILESYSTEM | SFGAO_FOLDER | SFGAO_LINK;
        SFGAOF attributes = 0;
        result->attributesRead = selected->GetAttributes(requested, &attributes);
        result->shellAttributes = attributes;
        if (FAILED(result->attributesRead)) return result->completed = result->attributesRead;
        if (!(attributes & SFGAO_FILESYSTEM) || (attributes & (SFGAO_FOLDER | SFGAO_LINK)))
            return result->completed = E_ACCESSDENIED;
        hr = budget.check();
        if (FAILED(hr)) return result->completed = hr;
        ShellText path;
        result->pathRead = selectedPath(selected.Get(), path);
        if (FAILED(result->pathRead)) return result->completed = result->pathRead;
        hr = budget.check();
        if (FAILED(hr)) return result->completed = hr;
        // Exclude data-write/delete opens while verifying the actual selected
        // stream. Attribute-only opens remain compatible with this read lease.
        NativeFile file{CreateFileW(path.value, FILE_READ_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL, nullptr)};
        result->sourceOpen = file.value == INVALID_HANDLE_VALUE ? nativeError() : S_OK;
        if (FAILED(result->sourceOpen)) return result->completed = result->sourceOpen;
        result->sourceBefore = readSource(budget, file.value, result->identityBefore, result->basicBefore);
        if (FAILED(result->sourceBefore)) return result->completed = result->sourceBefore;
        result->identityMatchesBefore = sameIdentity(expectedIdentity, result->identityBefore);
        if (!result->identityMatchesBefore) return result->completed = invalidData;
        result->originalMetadataMatchesBefore = originalBasic && sameMetadata(*originalBasic, result->basicBefore);
        if (originalBasic && !result->originalMetadataMatchesBefore) return result->completed = invalidData;
        result->sourceBytesBefore = readFileBytes(budget, file.value, expectedBytes, result->fileBytesMatchBefore);
        if (FAILED(result->sourceBytesBefore)) return result->completed = result->sourceBytesBefore;
        ComPtr<IBindCtx> bind;
        hr = budget.check();
        if (FAILED(hr)) return result->completed = hr;
        result->bindContext = CreateBindCtx(0, &bind);
        if (FAILED(result->bindContext)) return result->completed = result->bindContext;
        BIND_OPTS options{}; options.cbStruct = sizeof(options); options.grfMode = STGM_READ;
        options.dwTickCountDeadline = static_cast<DWORD>(deadline);
        if (!options.dwTickCountDeadline) options.dwTickCountDeadline = 1;
        result->bindMode = options.grfMode;
        hr = budget.check();
        if (FAILED(hr)) return result->completed = hr;
        result->bindOptions = bind->SetBindOptions(&options);
        if (FAILED(result->bindOptions)) return result->completed = result->bindOptions;
        ComPtr<IStream> stream;
        hr = budget.check();
        if (FAILED(hr)) return result->completed = hr;
        result->bindRead = selected->BindToHandler(bind.Get(), BHID_Stream, IID_PPV_ARGS(&stream));
        if (FAILED(result->bindRead)) return result->completed = result->bindRead;
        if (!stream) return result->completed = E_UNEXPECTED;
        hr = budget.check();
        if (FAILED(hr)) return result->completed = hr;
        STATSTG stat{};
        result->statRead = stream->Stat(&stat, STATFLAG_NONAME);
        if (FAILED(result->statRead)) return result->completed = result->statRead;
        result->streamType = stat.type; result->streamMode = stat.grfMode; result->streamSize = stat.cbSize.QuadPart;
        if (stat.type != STGTY_STREAM || stat.cbSize.QuadPart != ownedPreviewRtfBytes || (stat.grfMode & 3u) != STGM_READ)
            return result->completed = invalidData;
        hr = budget.check();
        if (FAILED(hr)) return result->completed = hr;
        LARGE_INTEGER zero{};
        result->seekRead = stream->Seek(zero, STREAM_SEEK_SET, nullptr);
        if (FAILED(result->seekRead)) return result->completed = result->seekRead;
        std::array<BYTE, ownedPreviewRtfBytes + 1> bytes{};
        while (result->streamBytes < bytes.size()) {
            hr = budget.check();
            if (FAILED(hr)) return result->completed = hr;
            ULONG copied = 0;
            result->streamRead = stream->Read(bytes.data() + result->streamBytes,
                static_cast<ULONG>(bytes.size()) - result->streamBytes, &copied);
            if (FAILED(result->streamRead)) return result->completed = result->streamRead;
            if (copied > bytes.size() - result->streamBytes) return result->completed = invalidData;
            result->streamBytes += copied;
            if (!copied) break;
        }
        result->streamBytesMatch = result->streamBytes == expectedBytes.size() &&
            std::equal(expectedBytes.begin(), expectedBytes.end(), bytes.begin());
        if (!result->streamBytesMatch) return result->completed = invalidData;
        hr = budget.check();
        if (FAILED(hr)) return result->completed = hr;
        result->rewindRead = stream->Seek(zero, STREAM_SEEK_SET, nullptr);
        if (FAILED(result->rewindRead)) return result->completed = result->rewindRead;
        // The caller retains this exact stream until ALL handler interfaces
        // have been released, including when Initialize itself reports error.
        hr = stream.CopyTo(retainedStream);
        if (FAILED(hr)) return result->completed = hr;
        if (initializer) {
            hr = budget.check();
            if (FAILED(hr)) return result->completed = hr;
            result->initializerAttempted = true;
            result->initialized = initializer->Initialize(stream.Get(), STGM_READ);
        }
        result->sourceAfter = readSource(budget, file.value, result->identityAfter, result->basicAfter);
        result->identityMatchesAfter = SUCCEEDED(result->sourceAfter) && sameIdentity(expectedIdentity, result->identityAfter);
        result->metadataUnchanged = SUCCEEDED(result->sourceAfter) && sameMetadata(result->basicBefore, result->basicAfter);
        result->originalMetadataMatchesAfter = originalBasic && SUCCEEDED(result->sourceAfter) && sameMetadata(*originalBasic, result->basicAfter);
        result->sourceBytesAfter = readFileBytes(budget, file.value, expectedBytes, result->fileBytesMatchAfter);
        hr = budget.check();
        if (SUCCEEDED(hr)) {
            ShellText afterPath;
            result->pathAfter = selectedPath(selected.Get(), afterPath);
            if (SUCCEEDED(result->pathAfter)) {
                hr = budget.check();
                if (SUCCEEDED(hr)) {
                    NativeFile afterFile{CreateFileW(afterPath.value, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL, nullptr)};
                    if (afterFile.value == INVALID_HANDLE_VALUE) result->pathAfter = nativeError();
                    else {
                        FILE_ID_INFO afterIdentity{};
                        result->pathAfter = GetFileInformationByHandleEx(afterFile.value, FileIdInfo, &afterIdentity, sizeof(afterIdentity)) ? S_OK : nativeError();
                        result->pathIdentityUnchanged = SUCCEEDED(result->pathAfter) && sameIdentity(expectedIdentity, afterIdentity);
                    }
                } else result->pathAfter = hr;
            }
        } else result->pathAfter = hr;
        result->guardAfter = budget.check();
        if (FAILED(result->guardAfter)) return result->completed = result->guardAfter;
        if (FAILED(result->sourceAfter)) return result->completed = result->sourceAfter;
        if (FAILED(result->sourceBytesAfter)) return result->completed = result->sourceBytesAfter;
        if (FAILED(result->pathAfter)) return result->completed = result->pathAfter;
        if (!result->identityMatchesAfter || !result->metadataUnchanged || !result->pathIdentityUnchanged ||
            (originalBasic && !result->originalMetadataMatchesAfter))
            return result->completed = invalidData;
        return result->completed = initializer ? result->initialized : S_OK;
    } catch (const std::bad_alloc&) { return result->completed = E_OUTOFMEMORY; }
      catch (...) { return result->completed = E_FAIL; }
}

} // namespace explorer
