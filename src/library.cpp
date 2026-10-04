#include "explorer/library.hpp"
#include "explorer/core.hpp"

#include <shlguid.h>
#include <algorithm>
#include <array>
#include <memory>
#include <utility>

namespace explorer {
namespace {
const std::array kinds{
    LibraryKindInfo{LibraryKind::General, L"General items", &FOLDERTYPEID_GenericLibrary},
    LibraryKindInfo{LibraryKind::Documents, L"Documents", &FOLDERTYPEID_Documents},
    LibraryKindInfo{LibraryKind::Music, L"Music", &FOLDERTYPEID_Music},
    LibraryKindInfo{LibraryKind::Pictures, L"Pictures", &FOLDERTYPEID_Pictures},
    LibraryKindInfo{LibraryKind::Videos, L"Videos", &FOLDERTYPEID_Videos}
};

struct TaskMemoryDelete { void operator()(wchar_t* value) const noexcept { CoTaskMemFree(value); } };

HRESULT itemName(IShellItem* item, SIGDN kind, std::wstring& result) {
    PWSTR raw = nullptr;
    const HRESULT hr = item->GetDisplayName(kind, &raw);
    std::unique_ptr<wchar_t, TaskMemoryDelete> owned(raw);
    if (FAILED(hr)) return hr;
    if (!raw) return E_UNEXPECTED;
    result = raw;
    return hr;
}

bool validSaveType(DEFAULTSAVEFOLDERTYPE type) noexcept {
    return type == DSFT_DETECT || type == DSFT_PRIVATE || type == DSFT_PUBLIC;
}

bool startsWithInsensitive(std::wstring_view text, std::wstring_view prefix) noexcept {
    return text.size() >= prefix.size() && CompareStringOrdinal(text.data(), static_cast<int>(prefix.size()), prefix.data(), static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
}

HRESULT directoryItem(const std::filesystem::path& folder,
                      Microsoft::WRL::ComPtr<IShellItem>& result) {
    const auto& text = folder.native();
    if (text.empty() || text.size() > ShellLibrary::MaximumPathUnits || text.find(L'\0') != std::wstring::npos || !folder.is_absolute()) return E_INVALIDARG;
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr)) return E_INVALIDARG;
    // Device paths are not filesystem directory inputs to the library API.
    if (text.starts_with(L"\\\\.\\") || startsWithInsensitive(text, L"\\\\?\\GLOBALROOT\\")) return E_INVALIDARG;
    // Resolve dot components before handing a path to Shell. The library's
    // filesystem identity comparison otherwise sees "folder" and "folder\\."
    // as different parsing names for the same directory.
    const DWORD required = GetFullPathNameW(text.c_str(), 0, nullptr, nullptr);
    if (!required) return HRESULT_FROM_WIN32(GetLastError());
    if (required > ShellLibrary::MaximumPathUnits + 1) return E_INVALIDARG;
    std::wstring normalized(required, L'\0');
    const DWORD copied = GetFullPathNameW(text.c_str(), required, normalized.data(), nullptr);
    if (!copied) return HRESULT_FROM_WIN32(GetLastError());
    if (copied >= required) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    normalized.resize(copied);
    const DWORD attributes = GetFileAttributesW(normalized.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return HRESULT_FROM_WIN32(GetLastError());
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
    Microsoft::WRL::ComPtr<IShellItem> item;
    HRESULT hr = SHCreateItemFromParsingName(normalized.c_str(), nullptr, IID_PPV_ARGS(&item));
    if (FAILED(hr)) return hr;
    SFGAOF shellAttributes = 0;
    hr = item->GetAttributes(SFGAO_FILESYSTEM | SFGAO_FOLDER, &shellAttributes);
    if (FAILED(hr)) return hr;
    if ((shellAttributes & (SFGAO_FILESYSTEM | SFGAO_FOLDER)) != (SFGAO_FILESYSTEM | SFGAO_FOLDER)) return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
    result = std::move(item);
    return hr;
}
} // namespace

std::span<const LibraryKindInfo> libraryKinds() noexcept { return kinds; }

std::optional<LibraryKind> libraryKindForType(REFGUID type) noexcept {
    for (const auto& info : kinds) if (IsEqualGUID(type, *info.folderType)) return info.kind;
    // Some existing general-purpose descriptions use the generic folder view.
    if (IsEqualGUID(type, FOLDERTYPEID_Generic)) return LibraryKind::General;
    return std::nullopt;
}

HRESULT ShellLibrary::create(ShellLibrary& result) {
    // https://learn.microsoft.com/windows/win32/api/shobjidl_core/nf-shobjidl_core-shcreatelibrary
    ShellLibrary created;
    const HRESULT hr = SHCreateLibrary(IID_PPV_ARGS(&created.library_));
    if (FAILED(hr)) return hr;
    if (!created.library_) return E_UNEXPECTED;
    created.writable_ = true;
    result = std::move(created);
    return hr;
}

HRESULT ShellLibrary::load(IShellItem* libraryItem, bool writable, ShellLibrary& result) {
    if (!libraryItem) return E_INVALIDARG;
    // https://learn.microsoft.com/windows/win32/api/shobjidl_core/nf-shobjidl_core-shloadlibraryfromitem
    ShellLibrary loaded;
    const DWORD mode = (writable ? STGM_READWRITE : STGM_READ) | STGM_SHARE_DENY_NONE;
    const HRESULT hr = SHLoadLibraryFromItem(libraryItem, mode, IID_PPV_ARGS(&loaded.library_));
    if (FAILED(hr)) return hr;
    if (!loaded.library_) return E_UNEXPECTED;
    loaded.writable_ = writable;
    result = std::move(loaded);
    return hr;
}

bool ShellLibrary::valid() const noexcept { return library_ != nullptr; }
bool ShellLibrary::writable() const noexcept { return valid() && writable_; }
IShellLibrary* ShellLibrary::native() const noexcept { return library_.Get(); }

HRESULT ShellLibrary::folders(std::vector<LibraryFolder>& result) const {
    if (!library_) return E_UNEXPECTED;
    Microsoft::WRL::ComPtr<IShellItemArray> included;
    const HRESULT enumeration = library_->GetFolders(LFF_ALLITEMS, IID_PPV_ARGS(&included));
    if (FAILED(enumeration)) return enumeration;
    if (!included) return E_UNEXPECTED;
    DWORD count = 0;
    HRESULT hr = included->GetCount(&count);
    if (FAILED(hr)) return hr;
    if (count > MaximumFolders) return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
    std::vector<LibraryFolder> folders;
    folders.reserve(count);
    for (DWORD index = 0; index < count; ++index) {
        LibraryFolder entry;
        hr = included->GetItemAt(index, &entry.item);
        if (FAILED(hr)) return hr;
        if (!entry.item) return E_UNEXPECTED;
        hr = itemName(entry.item.Get(), SIGDN_NORMALDISPLAY, entry.displayName);
        if (FAILED(hr)) return hr;
        SFGAOF attributes = 0;
        hr = entry.item->GetAttributes(SFGAO_FILESYSTEM, &attributes);
        if (FAILED(hr)) return hr;
        if (attributes & SFGAO_FILESYSTEM) {
            std::wstring path;
            hr = itemName(entry.item.Get(), SIGDN_FILESYSPATH, path);
            if (FAILED(hr)) return hr;
            entry.path = std::move(path);
        }
        folders.push_back(std::move(entry));
    }
    result = std::move(folders);
    return enumeration;
}

HRESULT ShellLibrary::defaultSaveFolder(Microsoft::WRL::ComPtr<IShellItem>& result, DEFAULTSAVEFOLDERTYPE type) const {
    if (!library_) return E_UNEXPECTED;
    if (!validSaveType(type)) return E_INVALIDARG;
    Microsoft::WRL::ComPtr<IShellItem> item;
    const HRESULT hr = library_->GetDefaultSaveFolder(type, IID_PPV_ARGS(&item));
    if (FAILED(hr)) return hr;
    if (!item) return E_UNEXPECTED;
    result = std::move(item);
    return hr;
}

HRESULT ShellLibrary::defaultSavePath(std::filesystem::path& result, DEFAULTSAVEFOLDERTYPE type) const {
    Microsoft::WRL::ComPtr<IShellItem> item;
    HRESULT hr = defaultSaveFolder(item, type);
    if (FAILED(hr)) return hr;
    std::wstring path;
    hr = itemName(item.Get(), SIGDN_FILESYSPATH, path);
    if (FAILED(hr)) return hr;
    result = std::move(path);
    return hr;
}

HRESULT ShellLibrary::folderType(GUID& result) const {
    if (!library_) return E_UNEXPECTED;
    GUID type{};
    const HRESULT hr = library_->GetFolderType(&type);
    if (SUCCEEDED(hr)) result = type;
    return hr;
}

HRESULT ShellLibrary::locate(IShellItem* candidate, bool& included, DWORD& count) const {
    if (!library_) return E_UNEXPECTED;
    if (!candidate) return E_INVALIDARG;
    Microsoft::WRL::ComPtr<IShellItemArray> items;
    HRESULT hr = library_->GetFolders(LFF_ALLITEMS, IID_PPV_ARGS(&items));
    if (FAILED(hr)) return hr;
    if (!items) return E_UNEXPECTED;
    DWORD itemCount = 0;
    hr = items->GetCount(&itemCount);
    if (FAILED(hr)) return hr;
    if (itemCount > MaximumFolders) return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
    bool found = false;
    for (DWORD index = 0; index < itemCount; ++index) {
        Microsoft::WRL::ComPtr<IShellItem> item;
        hr = items->GetItemAt(index, &item);
        if (FAILED(hr)) return hr;
        if (!item) return E_UNEXPECTED;
        int order = 0;
        // Library members carry library-specific PIDLs. Canonical comparison
        // can return S_FALSE for the same filesystem directory parsed outside
        // that library; the documented filesystem fallback compares location.
        // https://learn.microsoft.com/windows/win32/api/shobjidl_core/nf-shobjidl_core-ishellitem-compare
        hr = item->Compare(candidate, SICHINT_CANONICAL | SICHINT_TEST_FILESYSPATH_IF_NOT_EQUAL, &order);
        if (FAILED(hr)) return hr;
        if (!order) { found = true; break; }
    }
    included = found;
    count = itemCount;
    return S_OK;
}

HRESULT ShellLibrary::addFolder(const std::filesystem::path& folder) {
    if (!library_) return E_UNEXPECTED;
    if (!writable_) return E_ACCESSDENIED;
    Microsoft::WRL::ComPtr<IShellItem> item;
    HRESULT hr = directoryItem(folder, item);
    if (FAILED(hr)) return hr;
    bool included = false;
    DWORD count = 0;
    hr = locate(item.Get(), included, count);
    if (FAILED(hr)) return hr;
    if (included) return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    if (count >= MaximumFolders) return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
    return library_->AddFolder(item.Get());
}

HRESULT ShellLibrary::removeFolder(const std::filesystem::path& folder) {
    if (!library_) return E_UNEXPECTED;
    if (!writable_) return E_ACCESSDENIED;
    Microsoft::WRL::ComPtr<IShellItem> item;
    const HRESULT hr = directoryItem(folder, item);
    return FAILED(hr) ? hr : removeFolder(item.Get());
}

HRESULT ShellLibrary::removeFolder(IShellItem* folder) {
    if (!library_) return E_UNEXPECTED;
    if (!writable_) return E_ACCESSDENIED;
    if (!folder) return E_INVALIDARG;
    bool included = false;
    DWORD count = 0;
    const HRESULT hr = locate(folder, included, count);
    if (FAILED(hr)) return hr;
    if (!included) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    return library_->RemoveFolder(folder);
}

HRESULT ShellLibrary::setDefaultSaveFolder(const std::filesystem::path& folder, DEFAULTSAVEFOLDERTYPE type) {
    if (!library_) return E_UNEXPECTED;
    if (!writable_) return E_ACCESSDENIED;
    if (!validSaveType(type)) return E_INVALIDARG;
    Microsoft::WRL::ComPtr<IShellItem> item;
    const HRESULT hr = directoryItem(folder, item);
    return FAILED(hr) ? hr : setDefaultSaveFolder(item.Get(), type);
}

HRESULT ShellLibrary::setDefaultSaveFolder(IShellItem* folder, DEFAULTSAVEFOLDERTYPE type) {
    if (!library_) return E_UNEXPECTED;
    if (!writable_) return E_ACCESSDENIED;
    if (!folder || !validSaveType(type)) return E_INVALIDARG;
    bool included = false;
    DWORD count = 0;
    const HRESULT hr = locate(folder, included, count);
    if (FAILED(hr)) return hr;
    if (!included) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    return library_->SetDefaultSaveFolder(type, folder);
}

HRESULT ShellLibrary::optimize(LibraryKind kind) {
    if (!library_) return E_UNEXPECTED;
    if (!writable_) return E_ACCESSDENIED;
    const auto found = std::find_if(kinds.begin(), kinds.end(), [kind](const auto& entry) { return entry.kind == kind; });
    if (found == kinds.end()) return E_INVALIDARG;
    return library_->SetFolderType(*found->folderType);
}

HRESULT ShellLibrary::commit() {
    if (!library_) return E_UNEXPECTED;
    if (!writable_) return E_ACCESSDENIED;
    // Commit requires an existing backing file; keep the native HRESULT.
    // https://learn.microsoft.com/windows/win32/api/shobjidl_core/nf-shobjidl_core-ishelllibrary-commit
    return library_->Commit();
}

HRESULT ShellLibrary::save(const std::filesystem::path& directory, const std::wstring& nameWithoutExtension,
                           Microsoft::WRL::ComPtr<IShellItem>& savedItem) {
    if (!library_) return E_UNEXPECTED;
    if (!writable_) return E_ACCESSDENIED;
    if (nameWithoutExtension.size() > MaximumNameUnits || !validLeafName(nameWithoutExtension)) return E_INVALIDARG;
    constexpr std::wstring_view extension = L".library-ms";
    if (nameWithoutExtension.size() >= extension.size() &&
        CompareStringOrdinal(nameWithoutExtension.data() + nameWithoutExtension.size() - extension.size(), static_cast<int>(extension.size()), extension.data(), static_cast<int>(extension.size()), TRUE) == CSTR_EQUAL) return E_INVALIDARG;
    Microsoft::WRL::ComPtr<IShellItem> parent;
    HRESULT hr = directoryItem(directory, parent);
    if (FAILED(hr)) return hr;
    Microsoft::WRL::ComPtr<IShellItem> saved;
    // Explicit non-null parent and LSF_FAILIFTHERE; never save to user's default Libraries folder implicitly.
    // https://learn.microsoft.com/windows/win32/api/shobjidl_core/nf-shobjidl_core-ishelllibrary-save
    hr = library_->Save(parent.Get(), nameWithoutExtension.c_str(), LSF_FAILIFTHERE, &saved);
    if (FAILED(hr)) return hr;
    if (!saved) return E_UNEXPECTED;
    savedItem = std::move(saved);
    return hr;
}

} // namespace explorer
