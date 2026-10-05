#pragma once

#include <windows.h>
#include <shobjidl.h>
#include <string>
#include <vector>

namespace explorer {

// One literal quoted path per line, joined with CRLF and no trailing newline.
// This is clipboard text, not escaping for a particular command-line parser.
// Failure leaves formatted unchanged; no filesystem or clipboard access occurs.
HRESULT formatQuotedPaths(const std::vector<std::wstring>& paths, std::wstring& formatted);

class ItemActions final {
public:
    // Filesystem paths are preferred; virtual items use absolute parsing names.
    // Shell errors propagate, and text is replaced only after complete success.
    // This method does not read or publish clipboard data.
    static HRESULT quotedPaths(IShellItemArray* selection, std::wstring& text);
};

} // namespace explorer
