#pragma once

#include "explorer/core.hpp"
#include <shobjidl.h>

namespace explorer {

// Creates an in-memory native Shell search folder. The caller must initialize
// COM and owns the returned interface. A null scope searches This PC; a supplied
// folder searches that location and its descendants. Non-recursive searches
// require a filesystem folder. Explicit filename word-prefix ($<) conditions
// return ERROR_NOT_SUPPORTED for a Windows 10 native handler compatibility
// issue. No UI is created here.
HRESULT createSearchFolder(const std::wstring& query, IShellItem* scope, IShellItem** result,
                           bool recursive = true);

// Saves the unresolved native condition tree as a documented .search-ms file,
// so relative dates are evaluated again when opened. Never overwrites a file.
// Supported scopes are filesystem folders and This PC. Unsupported condition
// value types/operators return ERROR_NOT_SUPPORTED before creating the file.
HRESULT saveSearch(const std::wstring& query, IShellItem* scope, bool recursive,
                   const std::filesystem::path& path);

} // namespace explorer
