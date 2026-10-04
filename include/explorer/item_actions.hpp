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
    // Capability checks include write-attribute access but never mutate items.
    // All hidden -> unhide; any visible item -> hide the entire selection.
    // Filesystem items only. System files, reparse points and unavailable items
    // are rejected before any selected item changes. Folder children are untouched.
    static HRESULT canToggleHidden(IShellItemArray* selection, bool& willHide);
    // Every item is opened and validated first. A later OS failure triggers
    // best-effort rollback of hidden bits only; rollbackFailure, when supplied,
    // receives any rollback error while the original operation error is returned.
    static HRESULT toggleHidden(IShellItemArray* selection,
                                HRESULT* rollbackFailure = nullptr);
    // Filesystem paths are preferred; virtual items use absolute parsing names.
    // Shell errors propagate, and text is replaced only after complete success.
    // This method does not read or publish clipboard data.
    static HRESULT quotedPaths(IShellItemArray* selection, std::wstring& text);
};

} // namespace explorer
