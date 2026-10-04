#pragma once

#include "explorer/core.hpp"
#include <shobjidl.h>
#include <wrl/client.h>

namespace explorer {

struct SavedSearchMetadata {
    std::wstring query;
    Microsoft::WRL::ComPtr<IShellItem> scope;
    bool recursive = true;
};

// The caller initializes COM. Supports the documented single-scope, unresolved
// string-condition format written by saveSearch. Unsupported external shapes
// return ERROR_NOT_SUPPORTED; a failure leaves the entire output unchanged.
HRESULT readSavedSearch(const std::filesystem::path& path, SavedSearchMetadata* result);

} // namespace explorer
