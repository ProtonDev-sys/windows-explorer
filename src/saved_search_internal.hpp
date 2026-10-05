#pragma once
#include <windows.h>
#include <string_view>

namespace explorer::saved_search_internal {
// Writer and editable reader accept the same resolved/public scalar types.
bool supportedValueType(std::wstring_view type) noexcept;
// Uses the reader's byte/DOM depth/node limits before a generated search is
// published. Input is in memory; this performs no file IO or scope resolution.
HRESULT validateSerializedLimits(std::string_view xml);
}
