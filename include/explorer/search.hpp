#pragma once

#include "explorer/core.hpp"
#include <shobjidl.h>

namespace explorer {

// Creates an in-memory native Shell search folder. The caller must initialize
// COM and owns the returned interface. A null scope searches This PC; a supplied
// folder searches that location and its descendants. No UI is created here.
HRESULT createSearchFolder(const std::wstring& query, IShellItem* scope, IShellItem** result);

} // namespace explorer
