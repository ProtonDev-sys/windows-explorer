#pragma once

#include <windows.h>
#include <uiribbon.h>

namespace explorer {

enum class ThemeMode { Auto, Light, Dark };
enum class ThemeSurface { Content, Navigation, Ribbon, Address, Status, Caption };

struct ThemeState {
    ThemeMode requested = ThemeMode::Auto;
    ThemeMode actual = ThemeMode::Light;
    bool appsUseLightTheme = true;
    bool highContrast = false;
    bool nativeDarkAvailable = false;
    bool forcedDarkAvailable = false;
    DWORD windowsBuild = 0;
};

struct ThemePalette {
    COLORREF content = 0, navigation = 0, ribbon = 0, address = 0, status = 0, caption = 0;
    COLORREF text = 0, controlText = 0, captionText = 0, disabledText = 0, border = 0;
    COLORREF selection = 0, selectionText = 0, hover = 0;
    COLORREF surface(ThemeSurface value) const noexcept;
    COLORREF foreground(ThemeSurface value) const noexcept;
};

// Initialize before creating the first application window. These functions read
// settings and change process-local theme policy only; they never write Windows
// settings. Refresh on WM_SETTINGCHANGE, WM_THEMECHANGED and WM_SYSCOLORCHANGE.
HRESULT initializeProcessTheme(ThemeMode mode = ThemeMode::Auto) noexcept;
HRESULT refreshProcessTheme() noexcept;
ThemeState themeState() noexcept;
ThemePalette themePalette() noexcept;
// Read-only palette policy for previews/headless high contrast checks.
ThemePalette themePaletteFor(ThemeMode actual, bool highContrast) noexcept;

// Apply only to windows owned by this process AND the calling thread. Safe to
// call for newly created Shell view descendants. Native ItemsView styling is
// preserved. Reentrant calls caused by SetWindowTheme return S_FALSE.
HRESULT applyWindowTheme(HWND window) noexcept;
// Call on the framework's initializing STA.
HRESULT applyRibbonTheme(IUIFramework* framework) noexcept;
// Call before the framework's Destroy/reset; cached defaults hold no COM refs.
void forgetRibbonTheme(IUIFramework* framework) noexcept;

// Borrowed brushes: do not DeleteObject. Native Light/high contrast brushes are
// system-owned. Paint helpers perform no registry reads or heap allocations.
HBRUSH themeBrush(ThemeSurface surface) noexcept;
// Return nullptr in native Light mode so the caller uses DefWindowProc.
HBRUSH themeControlColor(HWND control, HDC dc, UINT message,
                         ThemeSurface surface = ThemeSurface::Address) noexcept;

// Pure policy selection also permits headless high contrast/legacy tests
// without changing the user's settings. High contrast always disables Dark.
ThemeMode resolveThemeMode(ThemeMode requested, bool appsUseLightTheme,
                          bool highContrast, bool nativeDarkAvailable,
                          bool forcedDarkAvailable) noexcept;

} // namespace explorer
