#include "explorer/ui_direction.hpp"
#include <algorithm>
#include <utility>

namespace explorer {
namespace {
HRESULT nativeError(DWORD fallback) noexcept {
    const auto error = GetLastError();
    return HRESULT_FROM_WIN32(error ? error : fallback);
}
bool validWindow(HWND window) noexcept { return !window || IsWindow(window) != FALSE; }
bool normalized(const RECT& bounds) noexcept { return bounds.left <= bounds.right && bounds.top <= bounds.bottom; }
HRESULT readLanguages(std::vector<std::wstring>& names) {
    // MUI_UI_FALLBACK is the full resource-loader list, not the formatting
    // locale or only the explicitly set thread languages.
    constexpr DWORD flags = MUI_LANGUAGE_NAME | MUI_UI_FALLBACK;
    ULONG count = 0, characters = 0;
    SetLastError(ERROR_SUCCESS);
    if (!GetThreadPreferredUILanguages(flags, &count, nullptr, &characters)) return nativeError(ERROR_INVALID_DATA);
    if (!count || characters < 2 || characters > 32768) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    std::wstring buffer(characters, L'\0');
    SetLastError(ERROR_SUCCESS);
    if (!GetThreadPreferredUILanguages(flags, &count, buffer.data(), &characters)) return nativeError(ERROR_INVALID_DATA);
    if (characters > buffer.size() || characters < 2 || buffer[characters - 1] || buffer[characters - 2])
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    buffer.resize(characters);
    std::vector<std::wstring> candidate;
    size_t start = 0;
    while (start + 1 < buffer.size() && buffer[start]) {
        const auto end = buffer.find(L'\0', start);
        if (end == buffer.npos || end == start || end - start >= LOCALE_NAME_MAX_LENGTH)
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        candidate.emplace_back(buffer.substr(start, end - start));
        start = end + 1;
    }
    if (candidate.size() != count) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    names = std::move(candidate);
    return S_OK;
}
HRESULT readLocale(std::wstring_view locale, UiDirectionPolicy& result) {
    if (locale.empty() || locale.size() >= LOCALE_NAME_MAX_LENGTH || locale.find(L'\0') != locale.npos)
        return E_INVALIDARG;
    const std::wstring name(locale);
    DWORD layout = 0;
    constexpr int layoutCharacters = static_cast<int>(sizeof(layout) / sizeof(wchar_t));
    SetLastError(ERROR_SUCCESS);
    const auto read = GetLocaleInfoEx(name.c_str(), LOCALE_IREADINGLAYOUT | LOCALE_RETURN_NUMBER,
        reinterpret_cast<LPWSTR>(&layout), layoutCharacters);
    if (!read) return nativeError(ERROR_INVALID_DATA);
    if (read != layoutCharacters || layout > 3) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    UiDirectionPolicy candidate;
    candidate.language = name;
    candidate.nativeReadingLayout = layout;
    candidate.rightToLeft = layout == 1;
    candidate.provenance = UiDirectionProvenance::NativeLocale;
    candidate.nativeStatus = S_OK;
    result = std::move(candidate);
    return S_OK;
}
HRESULT mapPoints(HWND source, HWND destination, POINT* points, UINT count) noexcept {
    if (!validWindow(source) || !validWindow(destination)) return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
    SetLastError(ERROR_SUCCESS);
    const auto mapped = MapWindowPoints(source, destination, points, count);
    if (!mapped && GetLastError() != ERROR_SUCCESS) return nativeError(ERROR_INVALID_WINDOW_HANDLE);
    return S_OK;
}
UINT directionFlags(bool rightToLeft, UINT baseFlags) noexcept {
    const auto retained = baseFlags & ~(TPM_CENTERALIGN | TPM_RIGHTALIGN | TPM_LAYOUTRTL);
    // TrackPopupMenu[Ex] reverses horizontal alignment for a mirrored owner.
    // Native LEFTALIGN therefore follows that owner's logical leading edge;
    // adding RIGHTALIGN would reflect the physical placement a second time.
    return retained | TPM_LEFTALIGN | (rightToLeft ? TPM_LAYOUTRTL : 0u);
}
} // namespace

HRESULT loadLocaleUiDirection(std::wstring_view locale, UiDirectionPolicy* output) noexcept {
    if (!output) return E_POINTER;
    try {
        UiDirectionPolicy candidate;
        const auto read = readLocale(locale, candidate);
        if (FAILED(read)) return read;
        *output = std::move(candidate);
        return S_OK;
    } catch (...) { return E_OUTOFMEMORY; }
}
HRESULT loadThreadUiDirection(UiDirectionPolicy* output) noexcept {
    if (!output) return E_POINTER;
    try {
        UiDirectionPolicy candidate;
        std::vector<std::wstring> languages;
        const auto languageRead = readLanguages(languages);
        candidate.nativeStatus = languageRead;
        if (SUCCEEDED(languageRead)) {
            const auto localeRead = readLocale(languages.front(), candidate);
            if (FAILED(localeRead)) {
                candidate = UiDirectionPolicy{};
                candidate.language = languages.front();
                candidate.nativeStatus = localeRead;
            }
            candidate.uiLanguages = std::move(languages);
        }
        *output = std::move(candidate);
        return S_OK;
    } catch (...) { return E_OUTOFMEMORY; }
}
HRESULT windowUiDirection(HWND window, bool* rightToLeft) noexcept {
    if (!rightToLeft) return E_POINTER;
    if (!window || !IsWindow(window)) return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
    SetLastError(ERROR_SUCCESS);
    const auto style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if (!style && GetLastError() != ERROR_SUCCESS) return nativeError(ERROR_INVALID_WINDOW_HANDLE);
    *rightToLeft = (style & WS_EX_LAYOUTRTL) != 0;
    return S_OK;
}
HRESULT mapUiPoint(HWND source, HWND destination, const POINT& input, POINT* output) noexcept {
    if (!output) return E_POINTER;
    auto candidate = input;
    const auto mapped = mapPoints(source, destination, &candidate, 1);
    if (FAILED(mapped)) return mapped;
    *output = candidate;
    return S_OK;
}
HRESULT mapUiRect(HWND source, HWND destination, const RECT& input, RECT* output) noexcept {
    if (!output) return E_POINTER;
    if (!normalized(input)) return E_INVALIDARG;
    auto candidate = input;
    // Microsoft requires exactly two points for native mirrored RECT swapping:
    // https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-mapwindowpoints
    const auto mapped = mapPoints(source, destination, reinterpret_cast<POINT*>(&candidate), 2);
    if (FAILED(mapped)) return mapped;
    if (candidate.left > candidate.right) std::swap(candidate.left, candidate.right);
    if (candidate.top > candidate.bottom) std::swap(candidate.top, candidate.bottom);
    *output = candidate;
    return S_OK;
}
HRESULT popupUiFlags(HWND owner, UINT baseFlags, UINT* output) noexcept {
    if (!output) return E_POINTER;
    bool rightToLeft = false;
    const auto read = windowUiDirection(owner, &rightToLeft);
    if (FAILED(read)) return read;
    *output = directionFlags(rightToLeft, baseFlags);
    return S_OK;
}
HRESULT popupUiPlacement(HWND owner, const RECT& screenBounds, UINT baseFlags, UiPopupPlacement* output) noexcept {
    if (!output) return E_POINTER;
    if (screenBounds.left >= screenBounds.right || screenBounds.top >= screenBounds.bottom) return E_INVALIDARG;
    UiPopupPlacement candidate;
    const auto read = windowUiDirection(owner, &candidate.rightToLeft);
    if (FAILED(read)) return read;
    candidate.flags = directionFlags(candidate.rightToLeft, baseFlags);
    candidate.anchor = {candidate.rightToLeft ? screenBounds.right : screenBounds.left, screenBounds.bottom};
    candidate.exclusion = screenBounds;
    *output = candidate;
    return S_OK;
}
} // namespace explorer
