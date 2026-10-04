#pragma once

#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace explorer {

enum class LibraryKind { General, Documents, Music, Pictures, Videos };

struct LibraryKindInfo {
    LibraryKind kind;
    std::wstring_view label;
    const GUID* folderType;
};

std::span<const LibraryKindInfo> libraryKinds() noexcept;
std::optional<LibraryKind> libraryKindForType(REFGUID type) noexcept;

struct LibraryFolder {
    Microsoft::WRL::ComPtr<IShellItem> item;
    std::wstring displayName;
    // Empty for non-filesystem storage locations; item remains usable.
    std::filesystem::path path;
};

// Owns an actual IShellLibrary. Caller initializes COM on its current thread.
// Mutations are in memory until commit; save creates a new description file.
// No dialog, Known Folder save, registry write or global Shell setting is used.
class ShellLibrary final {
public:
    static constexpr std::size_t MaximumFolders = 256;
    static constexpr std::size_t MaximumPathUnits = 32767;
    static constexpr std::size_t MaximumNameUnits = 244; // plus .library-ms

    static HRESULT create(ShellLibrary& result);
    static HRESULT load(IShellItem* libraryItem, bool writable, ShellLibrary& result);
    bool valid() const noexcept;
    bool writable() const noexcept;
    IShellLibrary* native() const noexcept;

    // Outputs change only on success; S_FALSE from GetFolders is preserved.
    HRESULT folders(std::vector<LibraryFolder>& result) const;
    HRESULT defaultSaveFolder(Microsoft::WRL::ComPtr<IShellItem>& result,
                              DEFAULTSAVEFOLDERTYPE type = DSFT_DETECT) const;
    HRESULT defaultSavePath(std::filesystem::path& result,
                            DEFAULTSAVEFOLDERTYPE type = DSFT_DETECT) const;
    HRESULT folderType(GUID& result) const;

    // Path overloads require an existing, absolute filesystem directory.
    // Duplicate add returns ERROR_ALREADY_EXISTS; absent remove/default returns
    // ERROR_NOT_FOUND. Native Shell failures are returned without translation.
    HRESULT addFolder(const std::filesystem::path& folder);
    HRESULT removeFolder(const std::filesystem::path& folder);
    HRESULT setDefaultSaveFolder(const std::filesystem::path& folder,
                                 DEFAULTSAVEFOLDERTYPE type = DSFT_PRIVATE);
    // Existing included items can be removed even if their folder is offline.
    HRESULT removeFolder(IShellItem* folder);
    HRESULT setDefaultSaveFolder(IShellItem* folder,
                                 DEFAULTSAVEFOLDERTYPE type = DSFT_PRIVATE);
    HRESULT optimize(LibraryKind kind);
    HRESULT commit();

    // Explicit directory and basename only. Native LSF_FAILIFTHERE guarantees
    // no overwrite; the Shell appends .library-ms. No default Libraries path.
    HRESULT save(const std::filesystem::path& directory,
                 const std::wstring& nameWithoutExtension,
                 Microsoft::WRL::ComPtr<IShellItem>& savedItem);

private:
    HRESULT locate(IShellItem* candidate, bool& included, DWORD& count) const;
    Microsoft::WRL::ComPtr<IShellLibrary> library_;
    bool writable_ = false;
};

} // namespace explorer
