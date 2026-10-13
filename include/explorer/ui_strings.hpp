#pragma once

#include <windows.h>
#include <string>
#include <string_view>
#include <vector>

namespace explorer {

// These resource IDs describe the audited Windows 10 build 19045 resource
// family, not a public Windows resource-ID ABI. Other builds use host text.
enum class UiText {
    Back, Forward, BackTooltip, ForwardTooltip, Up, UpTooltip, RecentLocations,
    AddressBar, AddressName, SearchBox, SearchAction, SearchCue,
    Refresh, RefreshTooltip, NavigationButtons, AddressToolbar, AllLocations,
    MinimiseRibbon, MinimiseRibbonTooltip, ExpandRibbon, ExpandRibbonTooltip,
    PreviewPaneName, PreviewSelectFile, PreviewUnavailable,
    Count
};
enum class UiStringProvenance { Native, AuthorFallback };
enum class UiTemplateSlot { MessageInsert, StringInsert };
struct UiString {
    std::wstring text;
    UiStringProvenance provenance = UiStringProvenance::AuthorFallback;
    HRESULT nativeStatus = E_PENDING;
    std::wstring module; // Full system resource-module path for Native results.
    UINT resourceId = 0;
    std::vector<std::wstring> uiLanguages; // Actual ordered MUI fallback list.
};

// Returns complete native or authored text, with its honest provenance. Each
// thread retains system data/image modules and copied labels until its actual
// UI language/fallback key changes. No language or system setting is written.
// Output is unchanged on an invalid key, null output or allocation failure.
HRESULT loadUiString(UiText key, UiString* output) noexcept;

// Only AddressName, SearchCue and RefreshTooltip are templates. Inserts are
// copied literally: percent signs in a folder name are never interpreted.
HRESULT formatUiString(UiText key, std::wstring_view replacement, UiString* output) noexcept;

// Exactly one %1 (MessageInsert) or %s (StringInsert) is required. Every other
// percent sequence, embedded NUL, malformed UTF-16 and oversized input fails
// without changing output. A template is limited to 4096 UTF-16 code units and
// a replacement to 32767; neither is truncated.
HRESULT replaceUiStringTemplate(std::wstring_view pattern, UiTemplateSlot slot,
    std::wstring_view replacement, std::wstring* output) noexcept;

} // namespace explorer
