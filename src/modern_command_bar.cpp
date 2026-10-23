#include "explorer/modern_command_bar.hpp"
#include "explorer/commands.hpp"
#include "explorer/theme.hpp"

#include <algorithm>
#include <cstdint>

namespace explorer {
namespace {
struct Button {
    UINT command;
    const wchar_t* text;
    const wchar_t* tip;
    wchar_t glyph;
    bool dropdown;
};
// Authored labels, public Segoe Fluent Icons / Segoe MDL2 Assets glyphs. Text
// remains present if an optional icon font or glyph is unavailable. No private
// shell DLL resources or undocumented system image indices are used.
constexpr Button buttons[]{
    {NewFolder, L"New folder", L"New folder (Ctrl+Shift+N)", L'\xe8f4', false},
    {Cut, L"Cut", L"Cut (Ctrl+X)", L'\xe8c6', false},
    {Copy, L"Copy", L"Copy (Ctrl+C)", L'\xe8c8', false},
    {Paste, L"Paste", L"Paste (Ctrl+V)", L'\xe77f', false},
    {Rename, L"Rename", L"Rename (F2)", L'\xe8ac', false},
    {Delete, L"Delete", L"Delete (Delete)", L'\xe74d', false},
    {SortMenu, L"Sort", L"Sort options", L'\xe8cb', true},
    {ViewMenu, L"View", L"View options", L'\xe8a9', true},
    {FileMenu, L"More", L"More options", L'\xe712', true},
};
constexpr size_t buttonCount = sizeof(buttons) / sizeof(buttons[0]);
int px(int value, UINT dpi) noexcept { return MulDiv(value, static_cast<int>(dpi ? dpi : 96), 96); }
UINT validDpi(UINT dpi) noexcept { return std::clamp(dpi ? dpi : 96u, 48u, 768u); }
HRESULT lastFailure() noexcept {
    const DWORD error = GetLastError();
    return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
}
const Button* findButton(UINT command) noexcept {
    for (const auto& button : buttons) if (button.command == command) return &button;
    return nullptr;
}

// Rasterize a public native font glyph with grayscale coverage, then explicitly
// premultiply the palette color. GDI text otherwise leaves a 32-bit DIB's alpha
// at zero. Image lists, not custom button windows, render these optional icons.
int addGlyph(HIMAGELIST list, HDC dc, wchar_t glyph, int size, COLORREF color) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* storage = nullptr;
    const auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &storage, nullptr, 0);
    if (!bitmap || !storage) { if (bitmap) DeleteObject(bitmap); return I_IMAGENONE; }
    const auto previous = SelectObject(dc, bitmap);
    auto* pixels = static_cast<std::uint32_t*>(storage);
    std::fill_n(pixels, size * size, 0u);
    RECT bounds{0, 0, size, size};
    DrawTextW(dc, &glyph, 1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();
    for (int index = 0; index < size * size; ++index) {
        const auto value = pixels[index];
        const auto alpha = std::max({value & 255u, (value >> 8) & 255u, (value >> 16) & 255u});
        pixels[index] = (alpha << 24) | ((GetRValue(color) * alpha / 255) << 16) |
            ((GetGValue(color) * alpha / 255) << 8) | (GetBValue(color) * alpha / 255);
    }
    SelectObject(dc, previous);
    const int index = ImageList_Add(list, bitmap, nullptr);
    DeleteObject(bitmap);
    return index < 0 ? I_IMAGENONE : index;
}
} // namespace

ModernCommandBar::~ModernCommandBar() { reset(); }

HRESULT ModernCommandBar::create(HWND parent, HINSTANCE instance) {
    if (!parent || !IsWindow(parent) || !instance) return E_INVALIDARG;
    DWORD process = 0;
    const auto thread = GetWindowThreadProcessId(parent, &process);
    if (thread != GetCurrentThreadId() || process != GetCurrentProcessId()) return E_ACCESSDENIED;
    if (toolbar_) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    // Also release optional resources if the previous parent destroyed its HWND.
    reset();
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES};
    if (!InitCommonControlsEx(&controls)) return lastFailure();
    dpi_ = validDpi(GetDpiForWindow(parent));
    toolbar_ = CreateWindowExW(0, TOOLBARCLASSNAMEW, L"File commands",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBSTYLE_FLAT | TBSTYLE_LIST |
        TBSTYLE_TOOLTIPS | TBSTYLE_WRAPABLE | CCS_NORESIZE | CCS_NOPARENTALIGN | CCS_NODIVIDER,
        0, 0, 1, 1, parent, nullptr, instance, nullptr);
    if (!toolbar_) return lastFailure();
    if (!SetWindowSubclass(toolbar_, subclass, 1, reinterpret_cast<DWORD_PTR>(this))) {
        const auto status = lastFailure(); reset(); return status;
    }
    SendMessageW(toolbar_, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    SendMessageW(toolbar_, CCM_SETUNICODEFORMAT, TRUE, 0);
    SendMessageW(toolbar_, TB_SETEXTENDEDSTYLE, 0,
        TBSTYLE_EX_DRAWDDARROWS | TBSTYLE_EX_MIXEDBUTTONS | TBSTYLE_EX_DOUBLEBUFFER);
    SendMessageW(toolbar_, TB_SETMAXTEXTROWS, 1, 0);
    metrics(dpi_);
    for (const auto& entry : buttons) {
        TBBUTTON button{};
        button.iBitmap = I_IMAGENONE;
        button.idCommand = static_cast<int>(entry.command);
        button.fsState = TBSTATE_ENABLED;
        button.fsStyle = BTNS_SHOWTEXT | (entry.dropdown ? BTNS_WHOLEDROPDOWN : BTNS_BUTTON);
        // TB_ADDBUTTONS copies this permanent Unicode caption into its string pool.
        button.iString = reinterpret_cast<INT_PTR>(entry.text);
        if (!SendMessageW(toolbar_, TB_ADDBUTTONSW, 1, reinterpret_cast<LPARAM>(&button))) {
            reset(); return E_OUTOFMEMORY;
        }
    }
    refreshTheme();
    height_ = rowHeight();
    // Tooltips are owned popups and do not inherit the parent's mirrored layout.
    const auto tips = reinterpret_cast<HWND>(SendMessageW(toolbar_, TB_GETTOOLTIPS, 0, 0));
    if (tips && (GetWindowLongPtrW(toolbar_, GWL_EXSTYLE) & WS_EX_LAYOUTRTL))
        SetWindowLongPtrW(tips, GWL_EXSTYLE, GetWindowLongPtrW(tips, GWL_EXSTYLE) | WS_EX_LAYOUTRTL);
    return S_OK;
}

void ModernCommandBar::reset() noexcept {
    if (toolbar_) {
        const auto owned = toolbar_;
        // Remove the only callback before destroying the control. Parent-first
        // destruction clears toolbar_ in WM_NCDESTROY, preventing HWND reuse.
        RemoveWindowSubclass(owned, subclass, 1);
        toolbar_ = nullptr;
        DestroyWindow(owned);
    }
    if (images_) ImageList_Destroy(images_);
    if (disabledImages_) ImageList_Destroy(disabledImages_);
    if (ownedFont_) DeleteObject(ownedFont_);
    images_ = disabledImages_ = nullptr;
    font_ = ownedFont_ = nullptr;
    imageIndices_.fill(I_IMAGENONE);
    bounds_ = {};
    laidOut_ = false;
    dpi_ = 96;
    height_ = 40;
}

int ModernCommandBar::height(UINT dpi) const noexcept {
    return std::max(1, MulDiv(height_, static_cast<int>(validDpi(dpi)), static_cast<int>(dpi_)));
}

int ModernCommandBar::rowHeight() const noexcept {
    int textHeight = px(16, dpi_);
    if (const auto dc = GetDC(toolbar_)) {
        const auto font = font_ ? font_ : ownedFont_;
        const auto previous = font ? SelectObject(dc, font) : nullptr;
        TEXTMETRICW metric{};
        if (GetTextMetricsW(dc, &metric)) textHeight = metric.tmHeight;
        if (previous) SelectObject(dc, previous);
        ReleaseDC(toolbar_, dc);
    }
    return std::max(px(36, dpi_), textHeight + px(16, dpi_));
}

void ModernCommandBar::metrics(UINT dpi) {
    dpi_ = validDpi(dpi);
    if (!font_) {
        NONCLIENTMETRICSW metrics{sizeof(metrics)};
        HFONT replacement = nullptr;
        if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi_))
            replacement = CreateFontIndirectW(&metrics.lfMessageFont);
        if (replacement) {
            SendMessageW(toolbar_, WM_SETFONT, reinterpret_cast<WPARAM>(replacement), FALSE);
            if (ownedFont_) DeleteObject(ownedFont_);
            ownedFont_ = replacement;
        } else SendMessageW(toolbar_, WM_SETFONT, reinterpret_cast<WPARAM>(ownedFont_ ? ownedFont_ :
            static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT))), FALSE);
    } else SendMessageW(toolbar_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
    SendMessageW(toolbar_, TB_SETPADDING, 0, MAKELPARAM(px(12, dpi_), px(10, dpi_)));
    SendMessageW(toolbar_, TB_SETBUTTONSIZE, 0, MAKELPARAM(px(80, dpi_), rowHeight()));
}

void ModernCommandBar::rebuildImages() {
    const int size = px(16, dpi_);
    HIMAGELIST normal = ImageList_Create(size, size, ILC_COLOR32, static_cast<int>(buttonCount), 0);
    HIMAGELIST disabled = ImageList_Create(size, size, ILC_COLOR32, static_cast<int>(buttonCount), 0);
    const auto dc = CreateCompatibleDC(nullptr);
    HFONT glyphFont = nullptr;
    if (dc) for (const auto* face : {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"}) {
        const auto candidate = CreateFontW(-size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
            DEFAULT_PITCH, face);
        if (!candidate) continue;
        const auto previous = SelectObject(dc, candidate);
        wchar_t actual[LF_FACESIZE]{};
        GetTextFaceW(dc, LF_FACESIZE, actual);
        SelectObject(dc, previous);
        if (_wcsicmp(actual, face) == 0) { glyphFont = candidate; break; }
        DeleteObject(candidate);
    }
    imageIndices_.fill(I_IMAGENONE);
    if (normal && disabled && dc && glyphFont) {
        const auto previous = SelectObject(dc, glyphFont);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        const auto palette = themePalette();
        for (size_t index = 0; index < buttonCount; ++index) {
            WORD glyph = 0xffff;
            if (GetGlyphIndicesW(dc, &buttons[index].glyph, 1, &glyph, GGI_MARK_NONEXISTING_GLYPHS) == GDI_ERROR ||
                glyph == 0xffff) continue;
            const auto image = addGlyph(normal, dc, buttons[index].glyph, size, palette.controlText);
            const auto muted = addGlyph(disabled, dc, buttons[index].glyph, size, palette.disabledText);
            if (image >= 0 && image == muted) imageIndices_[index] = image;
            // On a partial append failure indices could diverge; omit subsequent
            // icons rather than attach the wrong disabled representation.
            if (image < 0 || muted < 0 || image != muted) break;
        }
        SelectObject(dc, previous);
    }
    if (glyphFont) DeleteObject(glyphFont);
    if (dc) DeleteDC(dc);
    SendMessageW(toolbar_, TB_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(normal));
    SendMessageW(toolbar_, TB_SETDISABLEDIMAGELIST, 0, reinterpret_cast<LPARAM>(disabled));
    if (images_) ImageList_Destroy(images_);
    if (disabledImages_) ImageList_Destroy(disabledImages_);
    images_ = normal;
    disabledImages_ = disabled;
    for (size_t index = 0; index < buttonCount; ++index) {
        TBBUTTONINFOW info{sizeof(info)};
        info.dwMask = TBIF_IMAGE;
        info.iImage = imageIndices_[index];
        SendMessageW(toolbar_, TB_SETBUTTONINFOW, buttons[index].command, reinterpret_cast<LPARAM>(&info));
    }
}

void ModernCommandBar::layout(const RECT& bounds, UINT dpi) {
    if (!toolbar_) return;
    const auto previousDpi = dpi_;
    bounds_ = bounds;
    laidOut_ = true;
    dpi = validDpi(dpi);
    if (dpi != previousDpi) { metrics(dpi); rebuildImages(); }
    const int width = static_cast<int>(std::clamp<LONGLONG>(
        static_cast<LONGLONG>(bounds.right) - bounds.left, 1, 32767));
    const int row = rowHeight();
    const auto dc = GetDC(toolbar_);
    HGDIOBJ previousFont = nullptr;
    if (dc) previousFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(toolbar_, WM_GETFONT, 0, 0)));
    for (size_t index = 0; index < buttonCount; ++index) {
        SIZE text{px(80, dpi_), 0};
        if (dc) GetTextExtentPoint32W(dc, buttons[index].text, lstrlenW(buttons[index].text), &text);
        int desired = text.cx + px(24, dpi_);
        if (imageIndices_[index] >= 0) desired += px(22, dpi_);
        if (buttons[index].dropdown) desired += px(16, dpi_);
        TBBUTTONINFOW info{sizeof(info)};
        info.dwMask = TBIF_SIZE;
        // Even if less than a whole label fits, keep a native focusable button,
        // its full accessible caption and tooltip. No command is hidden.
        info.cx = static_cast<WORD>(std::min(width, desired));
        SendMessageW(toolbar_, TB_SETBUTTONINFOW, buttons[index].command, reinterpret_cast<LPARAM>(&info));
    }
    if (dc) { if (previousFont) SelectObject(dc, previousFont); ReleaseDC(toolbar_, dc); }
    // Native WM_SIZE/TB_AUTOSIZE performs wrapping at the actual client width.
    // Measure every item, rather than assuming TB_GETMAXSIZE means one row.
    SetWindowPos(toolbar_, nullptr, bounds.left, bounds.top, width, row,
        SWP_NOACTIVATE | SWP_NOZORDER);
    SendMessageW(toolbar_, TB_AUTOSIZE, 0, 0);
    height_ = row;
    for (size_t index = 0; index < buttonCount; ++index) {
        RECT item{};
        if (SendMessageW(toolbar_, TB_GETITEMRECT, index, reinterpret_cast<LPARAM>(&item)))
            height_ = std::max(height_, static_cast<int>(item.bottom) + px(2, dpi_));
    }
    SetWindowPos(toolbar_, nullptr, bounds.left, bounds.top, width, height_,
        SWP_NOACTIVATE | SWP_NOZORDER);
    InvalidateRect(toolbar_, nullptr, TRUE);
}

void ModernCommandBar::setFont(HFONT font) {
    font_ = font;
    if (!toolbar_) return;
    metrics(dpi_);
    if (laidOut_) layout(bounds_, dpi_);
    else height_ = rowHeight();
}

void ModernCommandBar::setCommandState(UINT command, bool enabled, bool checked) {
    if (!toolbar_ || !findButton(command)) return;
    // These native messages retain wrapping/hot state and publish accessibility
    // state changes. BTNS_CHECK is unnecessary: the host owns checked state.
    SendMessageW(toolbar_, TB_ENABLEBUTTON, command, MAKELONG(enabled, 0));
    SendMessageW(toolbar_, TB_CHECKBUTTON, command, MAKELONG(checked, 0));
}

void ModernCommandBar::refreshTheme() {
    if (!toolbar_) return;
    applyWindowTheme(toolbar_);
    rebuildImages();
    if (laidOut_) layout(bounds_, dpi_);
    InvalidateRect(toolbar_, nullptr, TRUE);
}

bool ModernCommandBar::notification(NMHDR* header, LRESULT& result) {
    if (!header || !toolbar_ || header->hwndFrom != toolbar_) return false;
    if (header->code == TBN_GETINFOTIPW) {
        const auto tip = reinterpret_cast<NMTBGETINFOTIPW*>(header);
        const auto entry = findButton(static_cast<UINT>(tip->iItem));
        if (!entry || !tip->pszText || tip->cchTextMax <= 0) return false;
        lstrcpynW(tip->pszText, entry->tip, tip->cchTextMax);
        result = 0;
        return true;
    }
    if (header->code != NM_CUSTOMDRAW) return false;
    auto& draw = *reinterpret_cast<NMTBCUSTOMDRAW*>(header);
    result = CDRF_DODEFAULT;
    // Preserve the OS's high contrast drawing, focus rectangles and semantics.
    if (themeState().highContrast) return true;
    if (draw.nmcd.dwDrawStage == CDDS_PREPAINT) {
        RECT client{}; GetClientRect(toolbar_, &client);
        FillRect(draw.nmcd.hdc, &client, themeBrush(ThemeSurface::Ribbon));
        result = CDRF_NOTIFYITEMDRAW;
    } else if (draw.nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
        const auto palette = themePalette();
        draw.clrText = (draw.nmcd.uItemState & (CDIS_DISABLED | CDIS_GRAYED)) ? palette.disabledText : palette.controlText;
        draw.clrBtnFace = palette.ribbon;
        draw.clrBtnHighlight = palette.hover;
        draw.clrHighlightHotTrack = palette.hover;
        draw.clrMark = palette.selection;
        draw.nStringBkMode = TRANSPARENT;
        draw.nHLStringBkMode = TRANSPARENT;
        result = TBCDRF_USECDCOLORS | TBCDRF_HILITEHOTTRACK;
    }
    return true;
}

LRESULT CALLBACK ModernCommandBar::subclass(HWND window, UINT message, WPARAM wParam,
    LPARAM lParam, UINT_PTR id, DWORD_PTR data) {
    const auto self = reinterpret_cast<ModernCommandBar*>(data);
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, subclass, id);
        if (self->toolbar_ == window) self->toolbar_ = nullptr;
    } else if (message == WM_ERASEBKGND) {
        RECT bounds{}; GetClientRect(window, &bounds);
        FillRect(reinterpret_cast<HDC>(wParam), &bounds, themeBrush(ThemeSurface::Ribbon));
        return 1;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

} // namespace explorer
