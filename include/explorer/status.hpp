#pragma once
#include <shobjidl.h>
#include <cstdint>
#include <optional>
#include <string>

namespace explorer {
struct SelectionStatus {
    DWORD count = 0;
    std::optional<std::uint64_t> bytes;
};
// Uses only the Shell's fast property store. No streams, thumbnails, content
// reads, directory recursion, or hydration requests. Call on selection changes,
// then retain the result rather than querying each status timer tick.
HRESULT selectionStatus(IShellItemArray* selection, SelectionStatus* output);
std::wstring statusText(unsigned itemCount, const SelectionStatus& selected);
}
