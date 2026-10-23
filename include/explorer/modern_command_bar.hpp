#pragma once

#include <windows.h>
#include <commctrl.h>
#include <array>

namespace explorer {

// A native, keyboard-accessible toolbar. All methods and destruction belong to
// the parent's UI thread. The object cannot move while its HWND is alive.
// No command execution, menus, Ribbon framework, or process theme policy is owned.
class ModernCommandBar final {
public:
    ModernCommandBar() noexcept = default;
    ~ModernCommandBar();
    ModernCommandBar(const ModernCommandBar&) = delete;
    ModernCommandBar& operator=(const ModernCommandBar&) = delete;

    HRESULT create(HWND parent, HINSTANCE instance);
    void reset() noexcept;
    HWND window() const noexcept { return toolbar_; }
    // Height of the last layout (initially one row), scaled to the supplied DPI.
    int height(UINT dpi) const noexcept;
    // Uses left/top/right in parent client pixels; bottom is not a height cap.
    // First layout this bar, then reserve height(dpi) for it and lay out content.
    // Native rows grow to retain every command, including at narrow widths.
    void layout(const RECT& bounds, UINT dpi);
    // Borrowed font, valid until replaced/reset. nullptr restores the DPI font.
    void setFont(HFONT font);
    void setCommandState(UINT command, bool enabled, bool checked = false);
    // Call after the host's refreshProcessTheme(); does not change OS settings.
    void refreshTheme();

    // Forward WM_NOTIFY here before other handlers. Handles custom draw/tips.
    // Returns false for TBN_DROPDOWN: the parent opens its existing SortMenu,
    // ViewMenu or FileMenu popup using NMTOOLBAR::iItem and rcButton (toolbar
    // client coordinates), and returns TBDDRET_NODEFAULT. Normal buttons send
    // WM_COMMAND, LOWORD(wParam) = existing Command ID, lParam = window().
    // Handle menu IDs via WM_COMMAND too for native keyboard/accelerator routes.
    bool notification(NMHDR* notification, LRESULT& result);

private:
    static LRESULT CALLBACK subclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    void metrics(UINT dpi);
    void rebuildImages();
    int rowHeight() const noexcept;

    HWND toolbar_ = nullptr;
    HFONT font_ = nullptr; // borrowed; null selects ownedFont_
    HFONT ownedFont_ = nullptr;
    HIMAGELIST images_ = nullptr, disabledImages_ = nullptr;
    std::array<int, 9> imageIndices_{};
    RECT bounds_{};
    UINT dpi_ = 96;
    int height_ = 40;
    bool laidOut_ = false;
};

} // namespace explorer
