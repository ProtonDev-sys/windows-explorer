#include "explorer/browser_tabs.hpp"
#include "explorer/theme.hpp"

#include <uxtheme.h>
#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>

namespace explorer {
namespace {
int scaled(int value, UINT dpi) noexcept {
    return MulDiv(value, static_cast<int>(dpi ? dpi : 96), 96);
}

HRESULT windowFailure() noexcept {
    const DWORD error = GetLastError();
    return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
}

HRESULT copyLabel(std::wstring_view label, std::wstring& result) {
    if (label.size() > static_cast<size_t>((std::numeric_limits<int>::max)()) ||
        label.find(L'\0') != std::wstring_view::npos) return E_INVALIDARG;
    result = label.empty() ? L"New tab" : std::wstring(label);
    return S_OK;
}
} // namespace

BrowserTabs::~BrowserTabs() { reset(); }

HRESULT BrowserTabs::create(HWND parent, HINSTANCE instance) noexcept {
    if (tabs_ || new_ || close_) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    if (!parent || !IsWindow(parent) || !instance) return E_INVALIDARG;
    DWORD process = 0;
    const DWORD thread = GetWindowThreadProcessId(parent, &process);
    if (process != GetCurrentProcessId()) return E_ACCESSDENIED;
    if (thread != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TAB_CLASSES};
    if (!InitCommonControlsEx(&controls)) return windowFailure();

    const auto make = [&](HWND& output, const wchar_t* type, const wchar_t* text,
                          DWORD style, UINT command) -> HRESULT {
        SetLastError(ERROR_SUCCESS);
        output = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            WS_CLIPSIBLINGS | style, 0, 0, 0, 0, parent,
            reinterpret_cast<HMENU>(static_cast<UINT_PTR>(command)), instance, nullptr);
        if (!output) return windowFailure();
        if (!SetWindowSubclass(output, controlSubclass, 1, reinterpret_cast<DWORD_PTR>(this))) {
            const HRESULT hr = windowFailure();
            DestroyWindow(output);
            output = nullptr;
            return hr;
        }
        return S_OK;
    };
    HRESULT hr = make(tabs_, WC_TABCONTROLW, L"Browser tabs",
        TCS_SINGLELINE | TCS_FIXEDWIDTH | TCS_TOOLTIPS | TCS_FOCUSONBUTTONDOWN, ControlId);
    if (SUCCEEDED(hr)) hr = make(new_, L"Button", L"New tab", BS_PUSHBUTTON, NewTabCommand);
    if (SUCCEEDED(hr)) hr = make(close_, L"Button", L"Close tab", BS_PUSHBUTTON, CloseTabCommand);
    if (FAILED(hr)) { reset(); return hr; }
    setFont(font_);
    updateCloseButton();
    hr = applyTheme();
    if (FAILED(hr)) reset();
    return hr;
}

void BrowserTabs::reset() noexcept {
    // Clear handles first: synchronous WM_NCDESTROY must not retain stale HWNDs.
    const HWND windows[]{tabs_, new_, close_};
    tabs_ = new_ = close_ = nullptr;
    ids_.clear();
    for (const HWND window : windows) if (window) DestroyWindow(window);
}

LRESULT CALLBACK BrowserTabs::controlSubclass(HWND window, UINT message, WPARAM wparam,
    LPARAM lparam, UINT_PTR subclass, DWORD_PTR reference) noexcept {
    if (message == WM_NCDESTROY) {
        auto& self = *reinterpret_cast<BrowserTabs*>(reference);
        if (self.tabs_ == window) {
            self.tabs_ = nullptr;
            self.ids_.clear();
        }
        if (self.new_ == window) self.new_ = nullptr;
        if (self.close_ == window) self.close_ = nullptr;
        RemoveWindowSubclass(window, controlSubclass, subclass);
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

HRESULT BrowserTabs::checkWindow() const noexcept {
    if (!tabs_) return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
    return GetWindowThreadProcessId(tabs_, nullptr) == GetCurrentThreadId() ? S_OK : RPC_E_WRONG_THREAD;
}

HRESULT BrowserTabs::add(std::wstring_view label, TabId* result) noexcept {
    if (!result) return E_POINTER;
    *result = NoTab;
    HRESULT hr = checkWindow();
    if (FAILED(hr)) return hr;
    if (nextId_ == NoTab || ids_.size() >= static_cast<size_t>((std::numeric_limits<int>::max)()))
        return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
    try {
        std::wstring text;
        hr = copyLabel(label, text);
        if (FAILED(hr)) return hr;
        // Complete all throwing work before modifying the native control.
        ids_.reserve(ids_.size() + 1);
        const TabId previous = selected();
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = text.data();
        const int index = static_cast<int>(ids_.size());
        if (SendMessageW(tabs_, TCM_INSERTITEMW, static_cast<WPARAM>(index),
            reinterpret_cast<LPARAM>(&item)) == -1) return E_FAIL;
        const TabId id = nextId_++;
        ids_.push_back(id);
        if (previous == NoTab) TabCtrl_SetCurSel(tabs_, index);
        else TabCtrl_SetCurSel(tabs_, indexOf(previous));
        updateCloseButton();
        *result = id;
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (const std::length_error&) { return E_OUTOFMEMORY; }
}

HRESULT BrowserTabs::select(TabId id) noexcept {
    const HRESULT hr = checkWindow();
    if (FAILED(hr)) return hr;
    const int index = indexOf(id);
    if (index < 0) return E_INVALIDARG;
    TabCtrl_SetCurSel(tabs_, index);
    updateCloseButton();
    return selected() == id ? S_OK : E_FAIL;
}

HRESULT BrowserTabs::remove(TabId id) noexcept {
    const HRESULT hr = checkWindow();
    if (FAILED(hr)) return hr;
    const int index = indexOf(id);
    if (index < 0) return E_INVALIDARG;
    TabId replacement = selected();
    if (replacement == id) {
        replacement = idAt(index + 1);
        if (replacement == NoTab) replacement = idAt(index - 1);
    }
    if (!TabCtrl_DeleteItem(tabs_, index)) return E_FAIL;
    ids_.erase(ids_.begin() + index);
    TabCtrl_SetCurSel(tabs_, indexOf(replacement));
    updateCloseButton();
    return S_OK;
}

HRESULT BrowserTabs::setLabel(TabId id, std::wstring_view label) noexcept {
    HRESULT hr = checkWindow();
    if (FAILED(hr)) return hr;
    const int index = indexOf(id);
    if (index < 0) return E_INVALIDARG;
    try {
        std::wstring text;
        hr = copyLabel(label, text);
        if (FAILED(hr)) return hr;
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = text.data();
        return SendMessageW(tabs_, TCM_SETITEMW, static_cast<WPARAM>(index),
            reinterpret_cast<LPARAM>(&item)) ? S_OK : E_FAIL;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (const std::length_error&) { return E_OUTOFMEMORY; }
}

BrowserTabs::TabId BrowserTabs::selected() const noexcept {
    return tabs_ ? idAt(TabCtrl_GetCurSel(tabs_)) : NoTab;
}

int BrowserTabs::indexOf(TabId id) const noexcept {
    const auto found = std::find(ids_.begin(), ids_.end(), id);
    return found == ids_.end() ? -1 : static_cast<int>(found - ids_.begin());
}

BrowserTabs::TabId BrowserTabs::idAt(int index) const noexcept {
    return index >= 0 && static_cast<size_t>(index) < ids_.size() ? ids_[static_cast<size_t>(index)] : NoTab;
}

void BrowserTabs::updateCloseButton() noexcept {
    if (close_) EnableWindow(close_, selected() != NoTab);
}

int BrowserTabs::height(UINT dpi) noexcept { return scaled(34, dpi); }

void BrowserTabs::layout(const RECT& bounds, UINT dpi) noexcept {
    if (!tabs_) return;
    const int width = (std::max)(0L, bounds.right - bounds.left);
    const int rowHeight = (std::max)(0L, bounds.bottom - bounds.top);
    const int gap = (std::min)(scaled(4, dpi), width / 6);
    const int buttonWidth = (std::min)((std::max)(0, (width - 2 * gap) / 2),
        (std::clamp)((width - scaled(48, dpi) - 2 * gap) / 2, scaled(32, dpi), scaled(76, dpi)));
    const int tabWidth = (std::max)(0, width - 2 * buttonWidth - 2 * gap);
    const int buttonHeight = (std::min)(rowHeight, scaled(28, dpi));
    const int buttonTop = bounds.top + (rowHeight - buttonHeight) / 2;
    const UINT flags = SWP_NOACTIVATE | SWP_NOZORDER;
    SetWindowPos(tabs_, nullptr, bounds.left, bounds.top, tabWidth, rowHeight, flags);
    SetWindowPos(new_, nullptr, bounds.left + tabWidth + gap, buttonTop,
        buttonWidth, buttonHeight, flags);
    SetWindowPos(close_, nullptr, bounds.left + tabWidth + gap + buttonWidth + gap,
        buttonTop, buttonWidth, buttonHeight, flags);
    TabCtrl_SetItemSize(tabs_, scaled(160, dpi), scaled(26, dpi));
}

void BrowserTabs::setFont(HFONT font) noexcept {
    font_ = font ? font : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    for (const HWND window : {tabs_, new_, close_}) if (window)
        SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
}

HRESULT BrowserTabs::applyTheme() noexcept {
    HRESULT hr = checkWindow();
    if (FAILED(hr)) return hr;
    const auto state = themeState();
    SetWindowTheme(tabs_, state.actual == ThemeMode::Dark && !state.highContrast ?
        L"DarkMode_Explorer" : L"Explorer", nullptr);
    for (const HWND window : {tabs_, new_, close_}) if (window) {
        hr = applyWindowTheme(window);
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}

} // namespace explorer
