#include "explorer/item_actions.hpp"

#include <shlobj.h>
#include <wrl/client.h>
#include <memory>
#include <new>
#include <utility>

namespace explorer {
namespace {
using Microsoft::WRL::ComPtr;

HRESULT lastError() {
    const DWORD error = GetLastError();
    return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
}

struct TaskDeleter {
    void operator()(void* value) const { CoTaskMemFree(value); }
};
using TaskString = std::unique_ptr<wchar_t, TaskDeleter>;
struct PidlDeleter {
    using pointer = LPITEMIDLIST;
    void operator()(pointer value) const noexcept { CoTaskMemFree(value); }
};
using TaskPidl = std::unique_ptr<ITEMIDLIST, PidlDeleter>;

class Handle final {
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~Handle() { if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
            value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
private:
    HANDLE value_;
};

struct AttributeItem {
    Handle handle;
    TaskPidl pidl;
    DWORD originalAttributes = 0;
    bool changed = false;
};

HRESULT countItems(IShellItemArray* selection, DWORD& count) {
    if (!selection) return E_INVALIDARG;
    const HRESULT hr = selection->GetCount(&count);
    return FAILED(hr) ? hr : count ? S_OK : E_INVALIDARG;
}

HRESULT supportedAttributes(DWORD attributes) {
    if (attributes & FILE_ATTRIBUTE_SYSTEM) return E_ACCESSDENIED;
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    return S_OK;
}

HRESULT readAttributes(HANDLE handle, DWORD& attributes) {
    FILE_BASIC_INFO info{};
    if (!GetFileInformationByHandleEx(handle, FileBasicInfo, &info, sizeof(info))) return lastError();
    attributes = info.FileAttributes;
    return supportedAttributes(attributes);
}

HRESULT prepare(IShellItemArray* selection, std::vector<AttributeItem>& items, bool& willHide) {
    DWORD count = 0;
    HRESULT hr = countItems(selection, count);
    if (FAILED(hr)) return hr;
    items.reserve(count);
    bool allHidden = true;
    for (DWORD index = 0; index < count; ++index) {
        ComPtr<IShellItem> item;
        hr = selection->GetItemAt(index, &item);
        if (FAILED(hr)) return hr;
        if (!item) return E_UNEXPECTED;
        SFGAOF attributes = 0;
        hr = item->GetAttributes(SFGAO_FILESYSTEM, &attributes);
        if (FAILED(hr)) return hr;
        if (!(attributes & SFGAO_FILESYSTEM)) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        PWSTR raw = nullptr;
        hr = item->GetDisplayName(SIGDN_FILESYSPATH, &raw);
        TaskString path(raw);
        if (FAILED(hr)) return hr;
        if (!path || !*path) return E_UNEXPECTED;

        // Denying delete sharing keeps each opened item at its selected name
        // during the batch; OPEN_REPARSE_POINT never follows the selected link.
        Handle handle(CreateFileW(path.get(), FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (handle.get() == INVALID_HANDLE_VALUE) return lastError();
        DWORD fileAttributes = 0;
        hr = readAttributes(handle.get(), fileAttributes);
        if (FAILED(hr)) return hr;
        PIDLIST_ABSOLUTE rawPidl = nullptr;
        hr = SHGetIDListFromObject(item.Get(), &rawPidl);
        TaskPidl pidl(rawPidl);
        if (FAILED(hr)) return hr;
        if (!pidl) return E_UNEXPECTED;
        allHidden = allHidden && (fileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
        items.push_back({std::move(handle), std::move(pidl), fileAttributes, false});
    }
    willHide = !allHidden;
    return S_OK;
}

DWORD withHidden(DWORD attributes, bool hidden) {
    attributes &= ~FILE_ATTRIBUTE_NORMAL;
    if (hidden) attributes |= FILE_ATTRIBUTE_HIDDEN;
    else attributes &= ~FILE_ATTRIBUTE_HIDDEN;
    return attributes ? attributes : FILE_ATTRIBUTE_NORMAL;
}

HRESULT setHidden(AttributeItem& item, bool hidden, bool& changed) {
    DWORD attributes = 0;
    HRESULT hr = readAttributes(item.handle.get(), attributes);
    if (FAILED(hr)) return hr;
    changed = false;
    if (((attributes & FILE_ATTRIBUTE_HIDDEN) != 0) == hidden) return S_OK;
    // Zero times leave timestamps unchanged. Preserve all unrelated attributes
    // read from the held handle immediately before changing the hidden bit.
    FILE_BASIC_INFO info{};
    info.FileAttributes = withHidden(attributes, hidden);
    if (!SetFileInformationByHandle(item.handle.get(), FileBasicInfo, &info, sizeof(info))) return lastError();
    changed = true;
    return S_OK;
}

void notify(const AttributeItem& item) {
    SHChangeNotify(SHCNE_ATTRIBUTES, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, item.pidl.get(), nullptr);
}
} // namespace

HRESULT formatQuotedPaths(const std::vector<std::wstring>& paths, std::wstring& formatted) {
    if (paths.empty()) return E_INVALIDARG;
    try {
        std::wstring result;
        for (const auto& path : paths) {
            if (path.empty() || path.find(L'\0') != std::wstring::npos ||
                path.find_first_of(L"\r\n") != std::wstring::npos) {
                return HRESULT_FROM_WIN32(ERROR_INVALID_NAME);
            }
            if (!result.empty()) result += L"\r\n";
            result += L'"';
            result += path;
            result += L'"';
        }
        formatted.swap(result);
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
}

HRESULT ItemActions::canToggleHidden(IShellItemArray* selection, bool& willHide) {
    try {
        std::vector<AttributeItem> items;
        bool candidate = false;
        const HRESULT hr = prepare(selection, items, candidate);
        if (SUCCEEDED(hr)) willHide = candidate;
        return hr;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
}

HRESULT ItemActions::toggleHidden(IShellItemArray* selection, HRESULT* rollbackFailure) {
    if (rollbackFailure) *rollbackFailure = S_OK;
    try {
        std::vector<AttributeItem> items;
        bool hide = false;
        HRESULT hr = prepare(selection, items, hide);
        if (FAILED(hr)) return hr;
        for (auto& item : items) {
            hr = setHidden(item, hide, item.changed);
            if (FAILED(hr)) {
                // Restore just our hidden changes; preserve independently
                // changed metadata. No rollback API can guarantee success if
                // access or filesystem availability changes during a batch.
                for (auto previous = items.rbegin(); previous != items.rend(); ++previous) {
                    if (!previous->changed) continue;
                    bool restored = false;
                    const HRESULT rollback = setHidden(*previous,
                        (previous->originalAttributes & FILE_ATTRIBUTE_HIDDEN) != 0, restored);
                    if (FAILED(rollback) && rollbackFailure && SUCCEEDED(*rollbackFailure)) {
                        *rollbackFailure = rollback;
                    }
                    notify(*previous);
                }
                return hr;
            }
        }
        for (const auto& item : items) if (item.changed) notify(item);
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
}

HRESULT ItemActions::quotedPaths(IShellItemArray* selection, std::wstring& text) {
    try {
        DWORD count = 0;
        HRESULT hr = countItems(selection, count);
        if (FAILED(hr)) return hr;
        std::vector<std::wstring> paths;
        paths.reserve(count);
        for (DWORD index = 0; index < count; ++index) {
            ComPtr<IShellItem> item;
            hr = selection->GetItemAt(index, &item);
            if (FAILED(hr)) return hr;
            if (!item) return E_UNEXPECTED;
            SFGAOF attributes = 0;
            hr = item->GetAttributes(SFGAO_FILESYSTEM, &attributes);
            if (FAILED(hr)) return hr;
            PWSTR raw = nullptr;
            // A filesystem provider's path failure must remain an error; a
            // parsing-name fallback is appropriate only for virtual items.
            hr = item->GetDisplayName((attributes & SFGAO_FILESYSTEM)
                ? SIGDN_FILESYSPATH : SIGDN_DESKTOPABSOLUTEPARSING, &raw);
            TaskString path(raw);
            if (FAILED(hr)) return hr;
            if (!path) return E_UNEXPECTED;
            paths.emplace_back(path.get());
        }
        return formatQuotedPaths(paths, text);
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
}

} // namespace explorer
