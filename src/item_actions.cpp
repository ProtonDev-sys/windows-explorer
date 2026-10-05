#include "explorer/item_actions.hpp"

#include <objbase.h>
#include <wrl/client.h>
#include <memory>
#include <new>

namespace explorer {
namespace {
using Microsoft::WRL::ComPtr;

struct TaskDeleter {
    void operator()(void* value) const { CoTaskMemFree(value); }
};
using TaskString = std::unique_ptr<wchar_t, TaskDeleter>;

HRESULT countItems(IShellItemArray* selection, DWORD& count) {
    if (!selection) return E_INVALIDARG;
    const HRESULT hr = selection->GetCount(&count);
    return FAILED(hr) ? hr : count ? S_OK : E_INVALIDARG;
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
