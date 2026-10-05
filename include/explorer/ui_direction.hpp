#pragma once

#include <windows.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace explorer {

enum class UiDirectionProvenance { NativeLocale, AuthorFallback };
struct UiDirectionPolicy {
    bool rightToLeft = false;
    UiDirectionProvenance provenance = UiDirectionProvenance::AuthorFallback;
    HRESULT nativeStatus = E_PENDING;
    std::optional<DWORD> nativeReadingLayout;
    std::wstring language;
    std::vector<std::wstring> uiLanguages;
};

// Uses the first name in the actual ordered thread MUI resource-loader list,
// including its user/system/neutral fallbacks. It never writes language or
// layout settings. A native lookup failure returns a completed, explicitly
// authored LTR policy; allocation/invalid-output failures preserve output.
HRESULT loadThreadUiDirection(UiDirectionPolicy* output) noexcept;

// Read-only native locale policy for explicit locale names. Only native
// reading-layout value 1 mirrors horizontal controls; values 2/3 describe
// vertical scripts and are retained without claiming horizontal RTL. Empty,
// embedded-NUL, unsupported and invalid names fail without changing output.
HRESULT loadLocaleUiDirection(std::wstring_view locale, UiDirectionPolicy* output) noexcept;

// Reads the actual window's WS_EX_LAYOUTRTL, including native inheritance.
// Null/stale HWNDs and null output fail without changing output.
HRESULT windowUiDirection(HWND window, bool* rightToLeft) noexcept;

// Null source/destination means screen coordinates, as in MapWindowPoints.
// An independent POINT is always mapped with cPoints=1; a normalized RECT is
// mapped with cPoints=2 so native mirrored left/right swapping applies. Inverted
// input rectangles, invalid HWNDs and null output preserve the caller's output.
HRESULT mapUiPoint(HWND source, HWND destination, const POINT& input, POINT* output) noexcept;
HRESULT mapUiRect(HWND source, HWND destination, const RECT& input, RECT* output) noexcept;

// Sets logical-leading horizontal alignment and text layout from the actual
// owner HWND, replacing only TPM_CENTERALIGN/RIGHTALIGN/LAYOUTRTL. Every other
// caller flag (return-command, mouse button, vertical placement, etc.) remains.
// Native LEFTALIGN follows the owner's mirrored coordinates: physical left for
// an LTR owner and right for a WS_EX_LAYOUTRTL owner. Text layout is independent.
HRESULT popupUiFlags(HWND owner, UINT baseFlags, UINT* output) noexcept;
struct UiPopupPlacement {
    bool rightToLeft = false;
    UINT flags = 0;
    POINT anchor{};
    RECT exclusion{};
};

// For a dropdown below a positive, normalized screen rectangle. Logical leading
// is its physical left in LTR and right in RTL; both anchor at the bottom edge.
// Does not create or show a menu. Errors leave every output field unchanged.
HRESULT popupUiPlacement(HWND owner, const RECT& screenBounds, UINT baseFlags,
    UiPopupPlacement* output) noexcept;

} // namespace explorer
