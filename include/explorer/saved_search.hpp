#pragma once

#include "explorer/core.hpp"
#include "explorer/search.hpp"
#include <shobjidl.h>
#include <wrl/client.h>

namespace explorer {

struct SavedSearchMetadata {
    std::wstring query;
    Microsoft::WRL::ComPtr<IShellItem> scope;
    bool recursive = true;
    // All included locations; scope is the first location for navigation.
    Microsoft::WRL::ComPtr<IShellItemArray> scopes;
    // Includes and exclusions with each location's original recursion flag.
    // Query refinement must retain these rules; scope/recursive are only the
    // first included location's convenience fields.
    std::vector<SearchScopeRule> scopeRules;
    std::optional<SearchViewPresentation> presentation;
};

// The caller initializes COM. Supports verified union scopes, recursive child
// exclusions, native kind unions and unresolved conditions. Unsupported or
// provider-dependent external shapes remain available in the native viewer;
// metadata import returns ERROR_NOT_SUPPORTED without changing any output.
HRESULT readSavedSearch(const std::filesystem::path& path, SavedSearchMetadata* result);

} // namespace explorer
