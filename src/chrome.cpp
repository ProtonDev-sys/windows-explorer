#include "explorer/chrome.hpp"
#include "explorer/commands.hpp"
#include "explorer/theme.hpp"
#include "explorer/headless_visual.hpp"
#include "explorer/ui_direction.hpp"
#include <uxtheme.h>
#include <vsstyle.h>
#include <vssym32.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <array>
#include <map>
#include <mutex>
#include <string>
#include <tuple>

namespace explorer {
namespace {
constexpr wchar_t addressActionBorder[] = L"WindowsExplorer.AddressActionBorder";
constexpr wchar_t ribbonCollapseHot[] = L"WindowsExplorer.RibbonCollapseHot";
struct DrawRecord { HWND toolbar = nullptr; ChromeDrawReadback value; };
thread_local std::array<DrawRecord, 7> privateDrawRecords;
constexpr std::array<UINT, 7> tracedCommands{Back, Forward, HistoryMenu, Up, Refresh, Address, AddressList};
void recordPrivateDraw(const NMTBCUSTOMDRAW& draw, const ChromeButtonState& state, UINT dpi) noexcept {
    const auto desktop = PrivateDesktop::current();
    if (!desktop || !desktop->ready()) return;
    for (size_t index = 0; index < tracedCommands.size(); ++index) {
        if (draw.nmcd.dwItemSpec != tracedCommands[index]) continue;
        auto& record = privateDrawRecords[index];
        record.toolbar = draw.nmcd.hdr.hwndFrom;
        record.value = {};
        auto& value = record.value;
        value.command = tracedCommands[index]; value.dpi = dpi;
        value.bounds = draw.nmcd.rc; value.state = state;
        GetViewportOrgEx(draw.nmcd.hdc, &value.viewportOrigin);
        GetWindowOrgEx(draw.nmcd.hdc, &value.windowOrigin);
        value.mapMode = GetMapMode(draw.nmcd.hdc);
        value.graphicsMode = GetGraphicsMode(draw.nmcd.hdc);
        value.transformRead = GetWorldTransform(draw.nmcd.hdc, &value.transform) != FALSE;
        value.deviceDpiX = GetDeviceCaps(draw.nmcd.hdc, LOGPIXELSX);
        value.deviceDpiY = GetDeviceCaps(draw.nmcd.hdc, LOGPIXELSY);
        value.textAlignment = GetTextAlign(draw.nmcd.hdc);
        return;
    }
}
int px(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi), 96); }
bool rightToLeft(HWND owner) noexcept {
    bool mirrored=false;
    return SUCCEEDED(windowUiDirection(owner,&mirrored))&&mirrored;
}
bool mirroredDc(HDC dc) noexcept {
    const auto layout=GetLayout(dc);
    return layout!=GDI_ERROR&&(layout&LAYOUT_RTL)!=0;
}
struct BitmapOrientation {
    HDC dc;
    DWORD previous=GDI_ERROR;
    explicit BitmapOrientation(HDC target) : dc(target) {
        const auto layout=GetLayout(dc);
        if(layout!=GDI_ERROR&&(layout&LAYOUT_RTL)&&!(layout&LAYOUT_BITMAPORIENTATIONPRESERVED))
            previous=SetLayout(dc,layout|LAYOUT_BITMAPORIENTATIONPRESERVED);
    }
    ~BitmapOrientation() {if(previous!=GDI_ERROR)SetLayout(dc,previous);}
};
struct ThemeHandle {
    HTHEME value;
    ThemeHandle(HWND owner, const wchar_t* name) : value(OpenThemeData(owner, name)) {}
    ~ThemeHandle() { if (value) CloseThemeData(value); }
};
struct IconCache {
    std::map<std::pair<UINT, UINT>, HICON> icons;
    std::mutex mutex;
    ~IconCache() { for (const auto& icon : icons) DestroyIcon(icon.second); }
    HICON get(UINT resource, UINT size) {
        std::lock_guard lock(mutex);
        const auto key = std::make_pair(resource, size);
        if (const auto found = icons.find(key); found != icons.end()) return found->second;
        wchar_t directory[MAX_PATH]{};
        if (!GetSystemDirectoryW(directory, ARRAYSIZE(directory))) return nullptr;
        const std::wstring path = std::wstring(directory) + L"\\shell32.dll";
        HICON icon = nullptr;
        if (FAILED(SHDefExtractIconW(path.c_str(), -static_cast<int>(resource), 0,
                                    &icon, nullptr, MAKELONG(size, 0))) || !icon) return nullptr;
        icons.emplace(key, icon);
        return icon;
    }
};
IconCache& icons() { static IconCache cache; return cache; }
struct BitmapStrips {
    HMODULE module = nullptr;
    std::map<UINT, HBITMAP> bitmaps;
    std::mutex mutex;
    ~BitmapStrips() {
        for (const auto& entry : bitmaps) DeleteObject(entry.second);
        if (module) FreeLibrary(module);
    }
    bool draw(HDC dc, const RECT& bounds, UINT resource, int cell, int cells, int size) {
        std::lock_guard lock(mutex);
        if (!module) {
            wchar_t directory[MAX_PATH]{};
            if (!GetSystemDirectoryW(directory, ARRAYSIZE(directory))) return false;
            const auto path = std::wstring(directory) + L"\\ExplorerFrame.dll";
            module = LoadLibraryExW(path.c_str(), nullptr,
                LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        }
        if (!module) return false;
        HBITMAP bitmap = nullptr;
        if (const auto found = bitmaps.find(resource); found != bitmaps.end()) bitmap = found->second;
        else {
            bitmap = static_cast<HBITMAP>(LoadImageW(module, MAKEINTRESOURCEW(resource),
                IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
            if (!bitmap) return false;
            BITMAP loaded{};
            if (!GetObjectW(bitmap, sizeof(loaded), &loaded) || !loaded.bmBits ||
                loaded.bmBitsPixel != 32 || loaded.bmHeight <= 0 || loaded.bmWidth <= 0 ||
                loaded.bmWidth != loaded.bmHeight * cells || loaded.bmWidth > 4096) {
                DeleteObject(bitmap); return false;
            }
            // ExplorerFrame RT_BITMAP address strips store straight BGRA.
            // AlphaBlend requires premultiplied color, including transparent
            // edge pixels. Convert the owned DIB once before caching it.
            for (LONG row = 0; row < loaded.bmHeight; ++row) {
                auto* pixel = static_cast<BYTE*>(loaded.bmBits) +
                    static_cast<size_t>(row) * loaded.bmWidthBytes;
                for (LONG column = 0; column < loaded.bmWidth; ++column, pixel += 4)
                    for (int channel = 0; channel < 3; ++channel)
                        pixel[channel] = static_cast<BYTE>((static_cast<unsigned>(pixel[channel]) * pixel[3] + 127) / 255);
            }
            bitmaps.emplace(resource, bitmap);
        }
        BITMAP details{};
        if (!GetObjectW(bitmap, sizeof(details), &details) || details.bmBitsPixel != 32 ||
            details.bmHeight <= 0 || details.bmWidth != details.bmHeight * cells || cell < 0 || cell >= cells)
            return false;
        const auto source = CreateCompatibleDC(dc);
        if (!source) return false;
        const auto previous = SelectObject(source, bitmap);
        const BLENDFUNCTION alpha{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        const BitmapOrientation orientation(dc);
        // The cache preserves native colors and converts their original alpha
        // representation for this native GDI compositing operation.
        const bool drawn = AlphaBlend(dc, (bounds.left + bounds.right - size) / 2,
            (bounds.top + bounds.bottom - size) / 2, size, size, source,
            cell * details.bmHeight, 0, details.bmHeight, details.bmHeight, alpha) != FALSE;
        SelectObject(source, previous);
        DeleteDC(source);
        return drawn;
    }
};
BitmapStrips& strips() { static BitmapStrips cache; return cache; }
struct GlyphFonts {
    std::map<std::tuple<int, int, BYTE>, HFONT> fonts;
    std::mutex mutex;
    ~GlyphFonts() { for (const auto& entry : fonts) DeleteObject(entry.second); }
    HFONT get(int size, int weight, BYTE quality) {
        std::lock_guard lock(mutex);
        const auto key = std::make_tuple(size, weight, quality);
        if (const auto found = fonts.find(key); found != fonts.end()) return found->second;
        const auto font = CreateFontW(-size, 0, 0, 0, weight, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, quality,
            DEFAULT_PITCH, L"Segoe MDL2 Assets");
        if (font) fonts.emplace(key, font);
        return font;
    }
};
GlyphFonts& glyphFonts() { static GlyphFonts cache; return cache; }
bool fontGlyph(HDC dc, const RECT& bounds, wchar_t glyph, int size, COLORREF color,
               int weight = FW_NORMAL, BYTE quality = DEFAULT_QUALITY) {
    const auto font = glyphFonts().get(size, weight, quality);
    if (!font) return false;
    const auto previous = SelectObject(dc, font);
    WORD index = 0xffff;
    const bool available = GetGlyphIndicesW(dc, &glyph, 1, &index,
        GGI_MARK_NONEXISTING_GLYPHS) != GDI_ERROR && index != 0xffff;
    bool drawn = false;
    if (available) {
        const auto oldColor = SetTextColor(dc, color);
        const auto oldMode = SetBkMode(dc, TRANSPARENT);
        const auto oldAlignment=mirroredDc(dc)?SetTextAlign(dc,TA_LEFT|TA_TOP|TA_NOUPDATECP):GDI_ERROR;
        auto rectangle = bounds;
        drawn = DrawTextW(dc, &glyph, 1, &rectangle,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX) != 0;
        if(oldAlignment!=GDI_ERROR)SetTextAlign(dc,oldAlignment);
        SetBkMode(dc, oldMode);
        SetTextColor(dc, oldColor);
    }
    SelectObject(dc, previous);
    return drawn;
}
bool nativeAddressGlyph(HDC dc, const RECT& bounds, int cell, UINT dpi) {
    const bool dark = themeState().actual == ThemeMode::Dark;
    constexpr UINT normal[]{288, 295, 296, 320, 321, 322, 323};
    constexpr UINT white[]{300, 301, 302, 328, 329, 330, 331};
    constexpr int sizes[]{16, 20, 24, 32, 40, 48, 64};
    const auto size = px(16, dpi);
    size_t index = 0;
    while (index + 1 < ARRAYSIZE(sizes) && sizes[index] < size) ++index;
    return strips().draw(dc, bounds, dark ? white[index] : normal[index], cell, 4, size);
}
bool nativeNavigationGlyph(HDC dc, const RECT& bounds, UINT command,
                           const ChromeButtonState& state, UINT dpi, bool rtl) {
    const auto size = px(16, dpi);
    const auto theme = themeState();
    const auto color = theme.actual == ThemeMode::Dark ?
        (state.enabled ? RGB(153, 153, 153) : themePalette().disabledText) :
        (state.enabled ? RGB(128, 128, 128) : RGB(226, 226, 226));
    // Modern Windows 10 uses flat navigation glyphs. ExplorerFrame's six-cell
    // bitmap contains the older outlined blue Back/Forward artwork instead.
    if (command == Back || command == Forward || command == HistoryMenu) {
        auto glyphBounds = bounds;
        OffsetRect(&glyphBounds, px(command == Forward ? 1 : 2, dpi),
                   command == HistoryMenu ? -px(1, dpi) : 0);
        const auto glyphCommand=rtl?(command==Back?Forward:command==Forward?Back:command):command;
        return fontGlyph(dc, glyphBounds, glyphCommand == Back ? L'\xe72b' :
            glyphCommand == Forward ? L'\xe72a' : L'\xe70d',
            px(command == HistoryMenu ? 6 : 12, dpi), color, FW_SEMIBOLD, ANTIALIASED_QUALITY);
    }
    if (command == Up) {
        if (theme.actual == ThemeMode::Dark)
            return fontGlyph(dc, bounds, L'\xe74a', px(12, dpi), color);
        const auto icon = icons().get(state.enabled ? 16817 : 16818, static_cast<UINT>(size));
        return icon && DrawIconEx(dc, (bounds.left + bounds.right - size) / 2 + px(2, dpi),
            (bounds.top + bounds.bottom - size) / 2 + px(1, dpi), icon, size, size, 0, nullptr,
            DI_NORMAL|(rtl?DI_NOMIRROR:0));
    }
    if (command == Refresh) {
        // Four-cell Go / Stop / Refresh / Down strip used by the address bar.
        auto glyphBounds = bounds;
        OffsetRect(&glyphBounds, -px(3, dpi), px(1, dpi));
        return nativeAddressGlyph(dc, glyphBounds, 2, dpi);
    }
    return false;
}
COLORREF glyphColor(const ChromeButtonState& state) {
    const auto palette = themePalette();
    return state.enabled ? palette.controlText : palette.disabledText;
}
void background(HWND owner, HDC dc, const RECT& bounds, const ChromeButtonState& state,
                ThemeSurface surface = ThemeSurface::Address) {
    FillRect(dc, &bounds, themeBrush(surface));
    if (!state.hot && !state.pressed && !state.checked) return;
    ThemeHandle theme(owner, L"Toolbar");
    const int nativeState = state.pressed ? TS_PRESSED : state.checked ? TS_CHECKED : TS_HOT;
    if (!theme.value || FAILED(DrawThemeBackground(theme.value, dc, TP_BUTTON, nativeState, &bounds, nullptr))) {
        const auto brush = CreateSolidBrush(themePalette().hover);
        if (brush) { FillRect(dc, &bounds, brush); DeleteObject(brush); }
    }
}
void lineGlyph(HDC dc, const RECT& bounds, UINT command, const ChromeButtonState& state, UINT dpi, bool rtl) {
    const int centerX = (bounds.left + bounds.right) / 2;
    const int centerY = (bounds.top + bounds.bottom) / 2;
    const int radius = std::max(3, px(4, dpi));
    const auto pen = CreatePen(PS_SOLID, std::max(1, px(1, dpi)), glyphColor(state));
    if (!pen) return;
    const auto previous = SelectObject(dc, pen);
    if (command == HistoryMenu) {
        MoveToEx(dc, centerX - px(2, dpi), centerY - 1, nullptr);
        LineTo(dc, centerX, centerY + px(2, dpi));
        LineTo(dc, centerX + px(3, dpi), centerY - px(2, dpi));
    } else if (command == Refresh) {
        const int left = centerX - radius, top = centerY - radius;
        Arc(dc, left, top, centerX + radius + 1, centerY + radius + 1,
            centerX + radius, centerY - 1, centerX, top);
        MoveToEx(dc, centerX + radius, top, nullptr);
        LineTo(dc, centerX + radius, centerY);
        LineTo(dc, centerX, centerY);
    } else if (command == Up) {
        MoveToEx(dc, centerX, centerY + radius + 1, nullptr);
        LineTo(dc, centerX, centerY - radius);
        MoveToEx(dc, centerX - radius, centerY, nullptr);
        LineTo(dc, centerX, centerY - radius);
        LineTo(dc, centerX + radius + 1, centerY + 1);
    } else {
        // GDI primitives already mirror with their DC. Reverse only the
        // remaining difference between actual window and DC direction.
        const int direction = (command == Back ? -1 : 1)*(rtl&&!mirroredDc(dc)?-1:1);
        MoveToEx(dc, centerX - direction * radius, centerY, nullptr);
        LineTo(dc, centerX + direction * radius, centerY);
        MoveToEx(dc, centerX, centerY - radius, nullptr);
        LineTo(dc, centerX + direction * radius, centerY);
        LineTo(dc, centerX, centerY + radius);
    }
    SelectObject(dc, previous);
    DeleteObject(pen);
}
LRESULT CALLBACK searchProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                           UINT_PTR id, DWORD_PTR) {
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, searchProc, id);
    const auto result = DefSubclassProc(window, message, wParam, lParam);
    if (message == WM_PAINT || message == WM_PRINTCLIENT) {
        const auto dc = message == WM_PAINT ? GetDC(window) : reinterpret_cast<HDC>(wParam);
        if (dc) {
            const UINT dpi = GetDpiForWindow(window);
            RECT bounds{}; GetClientRect(window, &bounds);
            // Classic EDIT consumes inherited mirroring into right alignment
            // and RTL reading while retaining an unmirrored client DC. Use
            // the actual host direction and this paint DC's coordinates.
            const auto leadingWidth = std::min<LONG>(px(40, dpi), bounds.right - bounds.left);
            if (rightToLeft(GetAncestor(window, GA_ROOT)) && !mirroredDc(dc))
                bounds.left = bounds.right - leadingWidth;
            else bounds.right = bounds.left + leadingWidth;
            FillRect(dc, &bounds, themeBrush(ThemeSurface::Address));
            drawSearchGlyph(dc, bounds, IsWindowEnabled(window) != FALSE, dpi);
            if (message == WM_PAINT) ReleaseDC(window, dc);
        }
    }
    return result;
}
LRESULT CALLBACK addressBorderProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                  UINT_PTR id, DWORD_PTR) {
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, addressBorderProc, id);
    const auto result = DefSubclassProc(window, message, wParam, lParam);
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_THEMECHANGED)
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME);
    if (message == WM_NCPAINT || message == WM_PRINT) {
        const auto dc = message == WM_NCPAINT ? GetWindowDC(window) : reinterpret_cast<HDC>(wParam);
        if (dc) {
            RECT bounds{}; GetWindowRect(window, &bounds);
            bounds.right -= bounds.left; bounds.bottom -= bounds.top;
            bounds.left = bounds.top = 0;
            const auto theme = themeState();
            const auto color = theme.highContrast ? GetSysColor(COLOR_WINDOWFRAME) :
                GetFocus() == window ? RGB(0, 120, 215) :
                theme.actual == ThemeMode::Dark ? themePalette().border : RGB(217, 217, 217);
            const auto brush = CreateSolidBrush(color);
            if (brush) { FrameRect(dc, &bounds, brush); DeleteObject(brush); }
            if (message == WM_NCPAINT) ReleaseDC(window, dc);
        }
    }
    return result;
}
LRESULT CALLBACK ribbonCollapseProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                   UINT_PTR id, DWORD_PTR) {
    if (wParam && (message == WM_PRINTCLIENT ||
        (message == WM_PRINT && (lParam & PRF_CLIENT)))) {
        // The themed BUTTON default print path can omit BS_OWNERDRAW's parent
        // callback. Route native printing through that same real draw handler.
        DRAWITEMSTRUCT draw{};
        draw.CtlType = ODT_BUTTON; draw.CtlID = static_cast<UINT>(GetDlgCtrlID(window));
        draw.itemAction = ODA_DRAWENTIRE; draw.hwndItem = window;
        draw.hDC = reinterpret_cast<HDC>(wParam); GetClientRect(window, &draw.rcItem);
        const auto state = SendMessageW(window, BM_GETSTATE, 0, 0);
        if (state & BST_PUSHED) draw.itemState |= ODS_SELECTED;
        if (state & BST_FOCUS) draw.itemState |= ODS_FOCUS;
        if (!IsWindowEnabled(window)) draw.itemState |= ODS_DISABLED;
        if (GetPropW(window, ribbonCollapseHot)) draw.itemState |= ODS_HOTLIGHT;
        if (SendMessageW(window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) draw.itemState |= ODS_NOFOCUSRECT;
        return SendMessageW(GetParent(window), WM_DRAWITEM, draw.CtlID, reinterpret_cast<LPARAM>(&draw));
    }
    if (message == WM_MOUSEMOVE && IsWindowEnabled(window) && !GetPropW(window, ribbonCollapseHot)) {
        if (SetPropW(window, ribbonCollapseHot, reinterpret_cast<HANDLE>(1))) {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
            if (!TrackMouseEvent(&tracking)) RemovePropW(window, ribbonCollapseHot);
            InvalidateRect(window, nullptr, FALSE);
        }
    } else if (message == WM_MOUSELEAVE || message == WM_ENABLE || message == WM_CANCELMODE) {
        if (RemovePropW(window, ribbonCollapseHot)) InvalidateRect(window, nullptr, FALSE);
    } else if (message == WM_NCDESTROY) {
        RemovePropW(window, ribbonCollapseHot);
        RemoveWindowSubclass(window, ribbonCollapseProc, id);
    }
    return DefSubclassProc(window, message, wParam, lParam);
}
}
HRESULT applyRibbonCollapseButton(HWND button) {
    if (!button || !IsWindow(button)) return E_INVALIDARG;
    DWORD process = 0;
    const auto thread = GetWindowThreadProcessId(button, &process);
    if (process != GetCurrentProcessId() || thread != GetCurrentThreadId()) return E_ACCESSDENIED;
    const auto hr = applyWindowTheme(button);
    if (FAILED(hr)) return hr;
    return SetWindowSubclass(button, ribbonCollapseProc, 1, 0) ? S_OK :
        HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_GEN_FAILURE);
}
bool drawRibbonCollapseButton(const DRAWITEMSTRUCT& draw, bool minimized, UINT dpi) {
    if (draw.CtlType != ODT_BUTTON || !draw.hDC || !draw.hwndItem || !dpi) return false;
    DWORD process = 0;
    const auto thread = GetWindowThreadProcessId(draw.hwndItem, &process);
    if (process != GetCurrentProcessId() || thread != GetCurrentThreadId()) return false;
    const int saved = SaveDC(draw.hDC);
    if (!saved) return false;
    ChromeButtonState state;
    state.enabled = !(draw.itemState & ODS_DISABLED);
    state.pressed = (draw.itemState & ODS_SELECTED) != 0;
    state.hot = (draw.itemState & ODS_HOTLIGHT) != 0 || GetPropW(draw.hwndItem, ribbonCollapseHot) != nullptr;
    background(draw.hwndItem, draw.hDC, draw.rcItem, state, ThemeSurface::Content);
    const auto theme = themeState();
    const auto color = theme.highContrast || theme.actual == ThemeMode::Dark ? glyphColor(state) :
        state.enabled ? RGB(128, 128, 128) : themePalette().disabledText;
    if (!fontGlyph(draw.hDC, draw.rcItem, minimized ? L'\xe70d' : L'\xe70e', px(10, dpi), color)) {
        const int middleX = (draw.rcItem.left + draw.rcItem.right) / 2;
        const int middleY = (draw.rcItem.top + draw.rcItem.bottom) / 2;
        const int direction = minimized ? 1 : -1;
        const auto pen = CreatePen(PS_SOLID, std::max(1, px(1, dpi)), color);
        if (pen) {
            const auto oldPen = SelectObject(draw.hDC, pen);
            MoveToEx(draw.hDC, middleX - px(3, dpi), middleY - direction * px(2, dpi), nullptr);
            LineTo(draw.hDC, middleX, middleY + direction * px(1, dpi));
            LineTo(draw.hDC, middleX + px(3, dpi), middleY - direction * px(2, dpi));
            SelectObject(draw.hDC, oldPen);
            DeleteObject(pen);
        }
    }
    if ((draw.itemState & ODS_FOCUS) && !(draw.itemState & ODS_NOFOCUSRECT)) {
        auto focus = draw.rcItem;
        InflateRect(&focus, -px(2, dpi), -px(2, dpi));
        DrawFocusRect(draw.hDC, &focus);
    }
    RestoreDC(draw.hDC, saved);
    return true;
}
HRESULT chromeToolbarDrawReadback(HWND toolbar, UINT command, ChromeDrawReadback* output) noexcept {
    if (!output) return E_POINTER;
    if (!toolbar || !IsWindow(toolbar)) return E_INVALIDARG;
    DWORD process = 0;
    const auto thread = GetWindowThreadProcessId(toolbar, &process);
    const auto desktop = PrivateDesktop::current();
    if (!desktop || !desktop->ready() || process != GetCurrentProcessId() || thread != GetCurrentThreadId())
        return E_ACCESSDENIED;
    for (const auto& record : privateDrawRecords) {
        if (record.toolbar == toolbar && record.value.command == command) { *output = record.value; return S_OK; }
    }
    return S_FALSE;
}
HRESULT drawNavigationButton(HWND owner, HDC dc, const RECT& bounds, UINT command,
                             const ChromeButtonState& state, UINT dpi) {
    if (!dc || !dpi) return E_INVALIDARG;
    if (command != Back && command != Forward && command != HistoryMenu && command != Up && command != Refresh) return E_INVALIDARG;
    const auto rtl=rightToLeft(owner);
    background(owner, dc, bounds, state);
    if (!themeState().highContrast && nativeNavigationGlyph(dc, bounds, command, state, dpi, rtl)) return S_OK;
    if (command != Up && command != Refresh) {
        ThemeHandle theme(owner, L"Navigation");
        const int part = command == Back ? NAV_BACKBUTTON : command == Forward ? NAV_FORWARDBUTTON : NAV_MENUBUTTON;
        const int nativeState = !state.enabled ? NAV_BB_DISABLED : state.pressed ? NAV_BB_PRESSED : state.hot ? NAV_BB_HOT : NAV_BB_NORMAL;
        if (theme.value && IsThemePartDefined(theme.value, part, nativeState)) {
            if(!rtl&&SUCCEEDED(DrawThemeBackground(theme.value,dc,part,nativeState,&bounds,nullptr)))return S_OK;
            if(rtl) {
                const auto dcMirrored=mirroredDc(dc);
                const int rtlPart=dcMirrored?part:command==Back?NAV_FORWARDBUTTON:command==Forward?NAV_BACKBUTTON:part;
                DTBGOPTS options{sizeof(options),static_cast<DWORD>(dcMirrored?DTBG_MIRRORDC:DTBG_NOMIRROR),{}};
                if(SUCCEEDED(DrawThemeBackgroundEx(theme.value,dc,rtlPart,nativeState,&bounds,&options)))return S_OK;
            }
        }
    }
    if(rtl&&command==Refresh&&fontGlyph(dc,bounds,L'\xe72c',px(12,dpi),glyphColor(state)))return S_OK;
    lineGlyph(dc, bounds, command, state, dpi, rtl);
    return S_OK;
}
void drawSearchGlyph(HDC dc, const RECT& bounds, bool enabled, UINT dpi) {
    if (!dc || !dpi) return;
    const ChromeButtonState state{enabled};
    const auto theme = themeState();
    const auto color = theme.highContrast ? glyphColor(state) :
        theme.actual == ThemeMode::Dark ? (enabled ? RGB(153, 153, 153) : themePalette().disabledText) :
        (enabled ? RGB(128, 128, 128) : RGB(172, 172, 172));
    if (fontGlyph(dc, bounds, L'\xe721', px(12, dpi), color)) return;
    const int left = (bounds.left + bounds.right) / 2 - px(5, dpi);
    const int top = (bounds.top + bounds.bottom) / 2 - px(5, dpi);
    const auto pen = CreatePen(PS_SOLID, std::max(1, px(1, dpi)), color);
    if (!pen) return;
    const auto oldPen = SelectObject(dc, pen);
    const auto oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, left, top, left + px(8, dpi), top + px(8, dpi));
    MoveToEx(dc, left + px(1, dpi), top + px(7, dpi), nullptr);
    LineTo(dc, left - px(3, dpi), top + px(11, dpi));
    SelectObject(dc, oldPen); SelectObject(dc, oldBrush); DeleteObject(pen);
}
LRESULT chromeToolbarCustomDraw(NMTBCUSTOMDRAW& draw, UINT dpi) {
    const bool addressAction = GetPropW(draw.nmcd.hdr.hwndFrom, addressActionBorder) != nullptr;
    if (draw.nmcd.dwDrawStage == CDDS_PREPAINT)
        return CDRF_NOTIFYITEMDRAW | (addressAction ? CDRF_NOTIFYPOSTPAINT : 0);
    if (draw.nmcd.dwDrawStage == CDDS_POSTPAINT && addressAction) {
        RECT bounds{};
        GetClientRect(draw.nmcd.hdr.hwndFrom, &bounds);
        const auto theme = themeState();
        const auto color = theme.highContrast ? GetSysColor(COLOR_WINDOWFRAME) :
            theme.actual == ThemeMode::Dark ? themePalette().border : RGB(217, 217, 217);
        const auto brush = CreateSolidBrush(color);
        if (brush) {
            const RECT top{0, 0, bounds.right, 1};
            const RECT bottom{0, bounds.bottom - 1, bounds.right, bounds.bottom};
            const RECT right{bounds.right - 1, 0, bounds.right, bounds.bottom};
            for (const auto& edge : {top, bottom, right}) FillRect(draw.nmcd.hdc, &edge, brush);
            DeleteObject(brush);
        }
        return CDRF_DODEFAULT;
    }
    if (draw.nmcd.dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;
    const auto flags = draw.nmcd.uItemState;
    ChromeButtonState state;
    state.enabled = (flags & (CDIS_DISABLED | CDIS_GRAYED)) == 0;
    state.hot = (flags & CDIS_HOT) != 0;
    state.pressed = (flags & CDIS_SELECTED) != 0;
    state.checked = (flags & CDIS_CHECKED) != 0;
    const auto command = static_cast<UINT>(draw.nmcd.dwItemSpec);
    recordPrivateDraw(draw, state, dpi);
    const auto hr = drawNavigationButton(draw.nmcd.hdr.hwndFrom,
        draw.nmcd.hdc, draw.nmcd.rc, command, state, dpi);
    return SUCCEEDED(hr) ? CDRF_SKIPDEFAULT : CDRF_DODEFAULT;
}
LRESULT chromeBreadcrumbCustomDraw(NMTBCUSTOMDRAW& draw, UINT dpi) {
    if (draw.nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (draw.nmcd.dwDrawStage != CDDS_ITEMPREPAINT || !draw.nmcd.hdc || !dpi)
        return CDRF_DODEFAULT;
    const auto owner = draw.nmcd.hdr.hwndFrom;
    const auto rtl=rightToLeft(owner);
    const auto command = static_cast<UINT>(draw.nmcd.dwItemSpec);
    TBBUTTONINFOW button{sizeof(button)};
    button.dwMask = TBIF_STYLE | TBIF_IMAGE | TBIF_LPARAM;
    if (SendMessageW(owner, TB_GETBUTTONINFOW, command,
                     reinterpret_cast<LPARAM>(&button)) < 0) return CDRF_DODEFAULT;
    ChromeButtonState state;
    const auto flags = draw.nmcd.uItemState;
    state.enabled = (flags & (CDIS_DISABLED | CDIS_GRAYED)) == 0;
    state.hot = (flags & CDIS_HOT) != 0;
    state.pressed = (flags & CDIS_SELECTED) != 0;
    state.checked = (flags & CDIS_CHECKED) != 0;
    if (command == Address || command == AddressList) recordPrivateDraw(draw, state, dpi);
    background(owner, draw.nmcd.hdc, draw.nmcd.rc, state);
    if (button.fsStyle & BTNS_SEP) return CDRF_SKIPDEFAULT;
    if (command == BreadcrumbOverflow) {
        const auto font = reinterpret_cast<HFONT>(SendMessageW(owner, WM_GETFONT, 0, 0));
        const auto previousFont = font ? SelectObject(draw.nmcd.hdc, font) : nullptr;
        const auto previousColor = SetTextColor(draw.nmcd.hdc, glyphColor(state));
        const auto previousMode = SetBkMode(draw.nmcd.hdc, TRANSPARENT);
        auto content = draw.nmcd.rc;
        if(!rtl)DrawTextW(draw.nmcd.hdc,L"\u00bb",1,&content,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
        else {
            // The Latin guillemet is bidirectionally mirrored by text layout.
            // Draw the two breadcrumb-direction chevrons once in the native DC
            // rather than combining character and window reflection.
            const auto pen=CreatePen(PS_SOLID,std::max(1,px(1,dpi)),glyphColor(state));
            if(pen) {
                const auto oldPen=SelectObject(draw.nmcd.hdc,pen);
                const int direction=mirroredDc(draw.nmcd.hdc)?1:-1;
                const int centerX=(content.left+content.right)/2,centerY=(content.top+content.bottom)/2;
                for(const int offset:{-px(2,dpi),px(2,dpi)}) {
                    MoveToEx(draw.nmcd.hdc,centerX+offset-direction*px(2,dpi),centerY-px(3,dpi),nullptr);
                    LineTo(draw.nmcd.hdc,centerX+offset+direction*px(2,dpi),centerY);
                    LineTo(draw.nmcd.hdc,centerX+offset-direction*px(2,dpi),centerY+px(3,dpi));
                }
                SelectObject(draw.nmcd.hdc,oldPen);DeleteObject(pen);
            }
        }
        SetBkMode(draw.nmcd.hdc, previousMode);
        SetTextColor(draw.nmcd.hdc, previousColor);
        if (previousFont) SelectObject(draw.nmcd.hdc, previousFont);
        if (flags & CDIS_FOCUS) {
            auto focus = draw.nmcd.rc;
            InflateRect(&focus, -px(1, dpi), -px(2, dpi));
            DrawFocusRect(draw.nmcd.hdc, &focus);
        }
        return CDRF_SKIPDEFAULT;
    }
    auto content = draw.nmcd.rc;
    content.left += px(4, dpi);
    content.right -= px(14, dpi);
    const bool iconOnly = button.iImage >= 0 &&
        (!(button.fsStyle & BTNS_SHOWTEXT) || button.lParam == 1);
    if (button.iImage >= 0) {
        const auto images = reinterpret_cast<HIMAGELIST>(SendMessageW(owner, TB_GETIMAGELIST, 0, 0));
        int width = 0, height = 0;
        if (images && ImageList_GetIconSize(images, &width, &height)) {
            const BitmapOrientation orientation(draw.nmcd.hdc);
            ImageList_Draw(images, button.iImage, draw.nmcd.hdc, content.left,
                (content.top + content.bottom - height) / 2, ILD_TRANSPARENT);
            content.left += width + px(4, dpi);
        }
    }
    if (!iconOnly && command != Address && command != AddressList) {
        const auto length = SendMessageW(owner, TB_GETBUTTONTEXTW, command, 0);
        if (length > 0 && length <= 32767) {
            std::wstring label(static_cast<size_t>(length) + 1, L'\0');
            if (SendMessageW(owner, TB_GETBUTTONTEXTW, command,
                             reinterpret_cast<LPARAM>(label.data())) >= 0) {
                const auto font = reinterpret_cast<HFONT>(SendMessageW(owner, WM_GETFONT, 0, 0));
                const auto previousFont = font ? SelectObject(draw.nmcd.hdc, font) : nullptr;
                const auto previousColor = SetTextColor(draw.nmcd.hdc, glyphColor(state));
                const auto previousMode = SetBkMode(draw.nmcd.hdc, TRANSPARENT);
                const auto previousAlignment=rtl?SetTextAlign(draw.nmcd.hdc,TA_LEFT|TA_TOP|TA_NOUPDATECP):GDI_ERROR;
                DrawTextW(draw.nmcd.hdc, label.c_str(), static_cast<int>(length), &content,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS | (rtl?DT_RTLREADING:0));
                if(previousAlignment!=GDI_ERROR)SetTextAlign(draw.nmcd.hdc,previousAlignment);
                SetBkMode(draw.nmcd.hdc, previousMode);
                SetTextColor(draw.nmcd.hdc, previousColor);
                if (previousFont) SelectObject(draw.nmcd.hdc, previousFont);
            }
        }
    }
    if ((button.fsStyle & BTNS_DROPDOWN) || command == Address || command == AddressList) {
        auto arrow = draw.nmcd.rc;
        arrow.left = arrow.right - px(14, dpi);
        const auto theme = themeState();
        const auto color = theme.highContrast ? glyphColor(state) :
            theme.actual == ThemeMode::Dark ? themePalette().controlText :
            state.enabled ? RGB(128, 128, 128) : themePalette().disabledText;
        if (command == Address || command == AddressList) arrow = draw.nmcd.rc;
        auto nativeArrow = arrow;
        RECT toolbarWindow{},toolbarOuterClient{};
        int borderX = 0, borderY = 0;
        if (GetWindowRect(owner, &toolbarWindow)&&SUCCEEDED(mapUiRect(nullptr,owner,toolbarWindow,&toolbarOuterClient))) {
            borderX = -toolbarOuterClient.left;
            borderY = -toolbarOuterClient.top;
        }
        // The bordered toolbar's client origin differs from the outer address
        // row. Preserve the installed strip's alignment in that outer row.
        OffsetRect(&nativeArrow, -borderX, px(1, dpi) - borderY);
        const bool nativeDown = (command == Address || command == AddressList) &&
            !theme.highContrast && nativeAddressGlyph(draw.nmcd.hdc, nativeArrow, 3, dpi);
        if (!nativeDown && !fontGlyph(draw.nmcd.hdc, arrow,
                       (command == Address || command == AddressList) ? L'\xe70d' : rtl?L'\xe76b':L'\xe76c', px(8, dpi), color))
            return CDRF_DODEFAULT;
    }
    if (flags & CDIS_FOCUS) {
        auto focus = draw.nmcd.rc;
        InflateRect(&focus, -px(1, dpi), -px(2, dpi));
        DrawFocusRect(draw.nmcd.hdc, &focus);
    }
    return CDRF_SKIPDEFAULT;
}
HRESULT applyChrome(HWND navigation, HWND breadcrumbs, HWND address, HWND search, HWND addressActions) {
    if (!IsWindow(navigation) || !IsWindow(breadcrumbs) || !IsWindow(address) || !IsWindow(search))
        return E_INVALIDARG;
    for (const auto window : {navigation, breadcrumbs, address, search, addressActions}) {
        if (!window) continue;
        if (!IsWindow(window)) return E_INVALIDARG;
        DWORD process = 0;
        const auto thread = GetWindowThreadProcessId(window, &process);
        if (process != GetCurrentProcessId() || thread != GetCurrentThreadId()) return E_ACCESSDENIED;
    }
    for (const auto window : {navigation, breadcrumbs, address, search, addressActions}) if (window) applyWindowTheme(window);
    const UINT dpi = GetDpiForWindow(navigation);
    SendMessageW(navigation, TB_SETBUTTONSIZE, 0, MAKELPARAM(px(28, dpi), px(30, dpi)));
    // Stock navigation has a narrow history rail. Four uniform 28px buttons
    // exceed the 103px row allocation and clip the Up arrow.
    constexpr UINT navigationCommands[]{Back, Forward, HistoryMenu, Up};
    constexpr int navigationWidths[]{30, 30, 16, 24};
    for (size_t index = 0; index < ARRAYSIZE(navigationCommands); ++index) {
        TBBUTTONINFOW button{sizeof(button)};
        button.dwMask = TBIF_SIZE;
        button.cx = static_cast<WORD>(px(navigationWidths[index], dpi));
        SendMessageW(navigation, TB_SETBUTTONINFOW, navigationCommands[index], reinterpret_cast<LPARAM>(&button));
    }
    if (addressActions) SendMessageW(addressActions, TB_SETBUTTONSIZE, 0, MAKELPARAM(px(24, dpi), px(30, dpi)));
    if (addressActions && !SetPropW(addressActions, addressActionBorder, reinterpret_cast<HANDLE>(1)))
        return HRESULT_FROM_WIN32(GetLastError());
    // Classic RTL EDIT has physical client coordinates after converting its
    // inherited layout style. Reserve the actual right edge and restore the
    // opposite native font margin instead of retaining an old icon margin.
    if (rightToLeft(GetAncestor(search, GA_ROOT)))
        SendMessageW(search, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
            MAKELPARAM(EC_USEFONTINFO, px(40, dpi)));
    else SendMessageW(search, EM_SETMARGINS, EC_LEFTMARGIN, MAKELPARAM(px(40, dpi), 0));
    for (const auto window : {breadcrumbs, address, search}) {
        const auto style = GetWindowLongPtrW(window, GWL_STYLE);
        if (!(style & WS_BORDER)) {
            SetWindowLongPtrW(window, GWL_STYLE, style | WS_BORDER);
            SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
        if (!SetWindowSubclass(window, addressBorderProc, 0x57454252, 0)) return HRESULT_FROM_WIN32(GetLastError());
    }
    return SetWindowSubclass(search, searchProc, 0x57454348, 0) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}
}
