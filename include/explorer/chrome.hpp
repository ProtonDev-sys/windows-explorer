#pragma once
#include <windows.h>
#include <commctrl.h>

namespace explorer {
struct ChromeButtonState {
    bool enabled = true, hot = false, pressed = false, checked = false;
};
// Numeric private-desktop diagnostics for the actual custom-draw DC. No route
// text or pixels are retained, and normal interactive drawing is not traced.
struct ChromeDrawReadback {
    UINT command = 0, dpi = 0;
    RECT bounds{};
    POINT viewportOrigin{}, windowOrigin{};
    XFORM transform{1, 0, 0, 1, 0, 0};
    bool transformRead = false;
    int mapMode = 0, graphicsMode = 0, deviceDpiX = 0, deviceDpiY = 0;
    UINT textAlignment = 0;
    ChromeButtonState state;
};
HRESULT chromeToolbarDrawReadback(HWND toolbar, UINT command, ChromeDrawReadback* output) noexcept;
HRESULT drawNavigationButton(HWND owner, HDC dc, const RECT& bounds, UINT command,
                             const ChromeButtonState& state, UINT dpi = 96);
HRESULT drawStatusViewButton(HWND owner, HDC dc, const RECT& bounds, UINT command,
                             const ChromeButtonState& state, UINT dpi = 96);
void drawSearchGlyph(HDC dc, const RECT& bounds, bool enabled, UINT dpi = 96);
// The installed Ribbon omits a host-owned minimize affordance. Keep a native
// accessible button and draw its current public UI_PKEY_Minimized state.
HRESULT applyRibbonCollapseButton(HWND button);
bool drawRibbonCollapseButton(const DRAWITEMSTRUCT& draw, bool minimized, UINT dpi = 96);
// Parent NM_CUSTOMDRAW handler for only the Navigation / View shortcuts
// toolbars. Native text/tooltip/accessibility identities stay on each button.
LRESULT chromeToolbarCustomDraw(NMTBCUSTOMDRAW& draw, bool status, UINT dpi = 96);
// Keeps native toolbar buttons, text, tooltips and accessibility while drawing
// the address breadcrumb's Shell icon and horizontal expansion chevrons.
LRESULT chromeBreadcrumbCustomDraw(NMTBCUSTOMDRAW& draw, UINT dpi = 96);
HRESULT applyChrome(HWND navigation, HWND breadcrumbs, HWND address, HWND search,
                    HWND statusViews, HWND addressActions = nullptr);
}
