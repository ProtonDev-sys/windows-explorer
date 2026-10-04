#pragma once

#include "explorer/core.hpp"
#include <shobjidl.h>
#include <wrl/client.h>
#include <vector>
#include <optional>

namespace explorer {

struct SearchScopeRule {
    Microsoft::WRL::ComPtr<IShellItem> folder;
    bool recursive = true;
    bool excluded = false;
};

// Native presentation model. The persisted XML supports Details/Icons/Tiles;
// other native modes use an explicit per-app companion. Missing public fields
// retain provider defaults; unknown/internal XML fields are never discarded.
enum class SearchViewMode { Details, Icons, Tiles, SmallIcons, List, Content };
struct SearchViewOrder {
    std::wstring property;
    SORTDIRECTION direction = SORT_ASCENDING;
};
struct SearchViewPresentation {
    std::optional<SearchViewMode> mode;
    std::optional<int> iconSize;
    std::optional<std::vector<std::wstring>> visibleColumns;
    std::optional<SearchViewOrder> groupBy;
    std::optional<std::vector<SearchViewOrder>> sort;
};
HRESULT validateSearchViewPresentation(const SearchViewPresentation& presentation);
HRESULT captureSearchViewPresentation(IFolderView2* view, SearchViewPresentation* result);
HRESULT applySearchViewPresentation(IFolderView2* view, const SearchViewPresentation& presentation);
// List/Small icons/Content are native view modes but not legal persisted XML
// mode tokens. This explicitly projects the public XML subset; the caller
// retains the full mode in an app-owned companion before claiming persistence.
HRESULT nativeSearchViewPresentation(const SearchViewPresentation& actual, SearchViewPresentation* result);

// Creates an in-memory native Shell search folder. The caller must initialize
// COM and owns the returned interface. A null scope searches This PC; a supplied
// folder searches that location and its descendants. Non-recursive searches
// require a filesystem folder. Explicit filename word-prefix ($<) conditions
// return ERROR_NOT_SUPPORTED for a Windows 10 native handler compatibility
// issue. No UI is created here.
HRESULT createSearchFolder(const std::wstring& query, IShellItem* scope, IShellItem** result,
                           bool recursive = true);
// Native union scope, with up to 256 folder locations. Library scopes expand
// to their included folders. Non-recursive scope applies to each location.
HRESULT createSearchFolderForScopes(const std::wstring& query, IShellItemArray* scopes,
                                    IShellItem** result, bool recursive = true);
// Preserves individual include recursion and recursive child exclusions.
// Provider-dependent exclusions (shallow, same as an included root, or an
// immediate child of a shallow include) return ERROR_NOT_SUPPORTED. Mixed
// shallow scopes/exclusions require filesystem locations for path conditions.
HRESULT createSearchFolderForScopeRules(const std::wstring& query,
                                       const std::vector<SearchScopeRule>& scopes, IShellItem** result);

// Saves the native condition tree as a documented .search-ms file. Generic
// leaves are normalized with their parser context; relative dates stay
// unresolved so they are evaluated again when opened. CreateNew is the default
// and never overwrites. UserConfirmed is reserved for a successful native Save
// dialog with overwrite prompting: it publishes a complete same-directory
// temporary file, preserving the existing target's DACL, creation time, basic
// attributes and native compression. Read-only, reparse and EFS targets are
// refused; existing handles that deny deletion retain their sharing error.
// Filesystem folders, exact known-folder roots and native Library location
// unions are supported. Unsupported scope/condition shapes return
// ERROR_NOT_SUPPORTED before creating the file.
enum class SearchSaveMode { CreateNew, UserConfirmed };
HRESULT saveSearch(const std::wstring& query, IShellItem* scope, bool recursive,
                   const std::filesystem::path& path, SearchSaveMode mode = SearchSaveMode::CreateNew,
                   const SearchViewPresentation* presentation = nullptr);
HRESULT saveSearchForScopes(const std::wstring& query, IShellItemArray* scopes, bool recursive,
                            const std::filesystem::path& path, SearchSaveMode mode = SearchSaveMode::CreateNew,
                            const SearchViewPresentation* presentation = nullptr);
HRESULT saveSearchForScopeRules(const std::wstring& query, const std::vector<SearchScopeRule>& scopes,
                               const std::filesystem::path& path, SearchSaveMode mode = SearchSaveMode::CreateNew,
                               const SearchViewPresentation* presentation = nullptr);

} // namespace explorer
