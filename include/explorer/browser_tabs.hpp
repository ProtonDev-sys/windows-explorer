#pragma once

#include <windows.h>
#include <commctrl.h>
#include <cstdint>
#include <string_view>
#include <vector>

namespace explorer {

// Owns tab chrome only. The parent owns each tab's independent content window.
// All access, including destruction, belongs to the parent's UI thread.
class BrowserTabs final {
public:
    using TabId = std::uint64_t;
    static constexpr TabId NoTab = 0;
    static constexpr UINT ControlId = 9000;
    static constexpr UINT NewTabCommand = 9001;
    static constexpr UINT CloseTabCommand = 9002;

    BrowserTabs() noexcept = default;
    ~BrowserTabs();
    BrowserTabs(const BrowserTabs&) = delete;
    BrowserTabs& operator=(const BrowserTabs&) = delete;
    BrowserTabs(BrowserTabs&&) = delete;
    BrowserTabs& operator=(BrowserTabs&&) = delete;

    // Three sibling HWNDs: notifications/commands go directly to parent.
    // Creation does not show or activate the parent or create any content.
    HRESULT create(HWND parent, HINSTANCE instance) noexcept;
    // Destroys chrome and clears entries. IDs are never reused by this object,
    // even across reset/create. The borrowed font remains configured.
    void reset() noexcept;

    // Appends, selecting only when no tab is selected. On failure *result is
    // NoTab and entries/selection are unchanged. Empty labels use "New tab".
    HRESULT add(std::wstring_view label, TabId* result) noexcept;
    // Programmatic mutations do not synthesize TCN_SELCHANGE. Parent updates
    // content after them and on WM_NOTIFY/TCN_SELCHANGE from window().
    HRESULT select(TabId id) noexcept;
    // Active removal selects the right neighbor, otherwise the left neighbor.
    // Removing another tab preserves selection by identity, not by index.
    HRESULT remove(TabId id) noexcept;
    HRESULT setLabel(TabId id, std::wstring_view label) noexcept;

    // Reads native selection, including changes made by native keyboard/mouse
    // handling. NoTab represents no selection; invalid indexes never yield IDs.
    TabId selected() const noexcept;
    const std::vector<TabId>& ids() const noexcept { return ids_; }
    int indexOf(TabId id) const noexcept;
    TabId idAt(int index) const noexcept;

    HWND window() const noexcept { return tabs_; }
    HWND newButton() const noexcept { return new_; }
    HWND closeButton() const noexcept { return close_; }
    // Bounds are in parent client pixels. Widths/heights scale from 96 DPI.
    // Reserves both buttons before tab space; native single-line tabs scroll.
    void layout(const RECT& bounds, UINT dpi) noexcept;
    static int height(UINT dpi) noexcept;
    // Borrowed font; caller keeps it alive until changed or this object dies.
    void setFont(HFONT font) noexcept;
    // Call after refreshing the process theme on settings/theme changes.
    HRESULT applyTheme() noexcept;

private:
    static LRESULT CALLBACK controlSubclass(HWND window, UINT message, WPARAM wparam,
        LPARAM lparam, UINT_PTR subclass, DWORD_PTR reference) noexcept;
    HRESULT checkWindow() const noexcept;
    void updateCloseButton() noexcept;

    HWND tabs_ = nullptr;
    HWND new_ = nullptr;
    HWND close_ = nullptr;
    HFONT font_ = nullptr;
    std::vector<TabId> ids_;
    TabId nextId_ = 1;
};

} // namespace explorer
