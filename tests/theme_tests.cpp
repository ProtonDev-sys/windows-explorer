#include "explorer/theme.hpp"
#include "explorer/ribbon.hpp"
#include "explorer/headless_visual.hpp"

#include <commctrl.h>
#include <propvarutil.h>
#include <uxtheme.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {
using Microsoft::WRL::ComPtr;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void succeeded(HRESULT result, const char* message) {
    if (FAILED(result)) throw std::runtime_error(std::string(message) + " HRESULT=" + std::to_string(static_cast<unsigned long>(result)));
}
struct Window { HWND value = nullptr; ~Window() { if (value) DestroyWindow(value); } };
struct Variant {
    PROPVARIANT value{};
    Variant() = default;
    Variant(const Variant&) = delete;
    Variant& operator=(const Variant&) = delete;
    Variant(Variant&& other) noexcept : value(other.value) { PropVariantInit(&other.value); }
    ~Variant() { PropVariantClear(&value); }
};
struct SettingsSnapshot {
    LSTATUS result = ERROR_SUCCESS;
    DWORD type = 0, size = 0;
    std::array<BYTE, 64> bytes{};
    bool operator==(const SettingsSnapshot&) const = default;
};
SettingsSnapshot readSetting(const wchar_t* name) {
    SettingsSnapshot snapshot;
    snapshot.size = static_cast<DWORD>(snapshot.bytes.size());
    snapshot.result = RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", name,
        RRF_RT_ANY, &snapshot.type, snapshot.bytes.data(), &snapshot.size);
    return snapshot;
}
void pump() {
    const auto deadline = GetTickCount64() + 180;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } while (GetTickCount64() < deadline);
}
LRESULT CALLBACK hostProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_ERASEBKGND) {
        RECT client{}; GetClientRect(window, &client);
        FillRect(reinterpret_cast<HDC>(wparam), &client, explorer::themeBrush(explorer::ThemeSurface::Content));
        return 1;
    }
    if (message >= WM_CTLCOLORMSGBOX && message <= WM_CTLCOLORSTATIC) {
        if (const auto brush = explorer::themeControlColor(reinterpret_cast<HWND>(lparam),
            reinterpret_cast<HDC>(wparam), message)) return reinterpret_cast<LRESULT>(brush);
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void policyTests() {
    using explorer::ThemeMode;
    using explorer::resolveThemeMode;
    require(resolveThemeMode(ThemeMode::Auto, true, false, true, true) == ThemeMode::Light, "Auto ignored Windows Light preference");
    require(resolveThemeMode(ThemeMode::Auto, false, false, true, true) == ThemeMode::Dark, "Auto ignored Windows Dark preference");
    require(resolveThemeMode(ThemeMode::Light, false, false, true, true) == ThemeMode::Light, "Explicit Light ignored");
    require(resolveThemeMode(ThemeMode::Dark, true, false, true, true) == ThemeMode::Dark, "Process-only force Dark ignored");
    require(resolveThemeMode(ThemeMode::Dark, true, false, true, false) == ThemeMode::Light, "1809 unsupported force Dark advertised");
    require(resolveThemeMode(ThemeMode::Dark, false, false, true, false) == ThemeMode::Dark, "1809 native system Dark rejected");
    for (const auto requested : {ThemeMode::Auto, ThemeMode::Light, ThemeMode::Dark}) {
        for (const bool light : {false, true}) {
            require(resolveThemeMode(requested, light, true, true, true) == ThemeMode::Light, "High contrast did not override custom dark");
            require(resolveThemeMode(requested, light, false, false, true) == ThemeMode::Light, "Missing native dark ABI enabled private calls");
        }
    }
    const auto contrast = explorer::themePaletteFor(ThemeMode::Dark, true);
    require(contrast.content == GetSysColor(COLOR_WINDOW) && contrast.text == GetSysColor(COLOR_WINDOWTEXT) &&
        contrast.status == GetSysColor(COLOR_BTNFACE) && contrast.controlText == GetSysColor(COLOR_BTNTEXT) &&
        contrast.caption == GetSysColor(COLOR_ACTIVECAPTION) && contrast.captionText == GetSysColor(COLOR_CAPTIONTEXT) &&
        contrast.selection == GetSysColor(COLOR_HIGHLIGHT) && contrast.selectionText == GetSysColor(COLOR_HIGHLIGHTTEXT),
        "High contrast palette failed paired system foreground/background colors");
    require(explorer::initializeProcessTheme(static_cast<ThemeMode>(88)) == E_INVALIDARG, "Invalid process theme accepted");
    require(explorer::applyWindowTheme(nullptr) == E_INVALIDARG, "Null theme HWND accepted");
    require(explorer::applyRibbonTheme(nullptr) == E_POINTER, "Null Ribbon framework accepted");
}

struct Pixels { UINT width = 0, height = 0; std::vector<BYTE> bgra; };
Pixels decode(const std::filesystem::path& path) {
    ComPtr<IWICImagingFactory> factory;
    succeeded(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "WIC factory");
    ComPtr<IWICBitmapDecoder> decoder;
    succeeded(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder), "Decode actual native theme PNG");
    ComPtr<IWICBitmapFrameDecode> frame;
    succeeded(decoder->GetFrame(0, &frame), "PNG frame");
    ComPtr<IWICFormatConverter> converter;
    succeeded(factory->CreateFormatConverter(&converter), "Pixel converter");
    succeeded(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom), "Pixel conversion");
    Pixels pixels;
    succeeded(converter->GetSize(&pixels.width, &pixels.height), "PNG size");
    require(pixels.width <= 2000 && pixels.height <= 2000, "Unexpected PNG dimensions");
    pixels.bgra.resize(static_cast<size_t>(pixels.width) * pixels.height * 4);
    succeeded(converter->CopyPixels(nullptr, pixels.width * 4, static_cast<UINT>(pixels.bgra.size()), pixels.bgra.data()), "Read native render pixels");
    return pixels;
}
double flatFraction(const Pixels& pixels, RECT area, COLORREF expected) {
    require(area.left >= 0 && area.top >= 0 && area.right <= static_cast<LONG>(pixels.width) &&
        area.bottom <= static_cast<LONG>(pixels.height) && area.right > area.left && area.bottom > area.top, "Theme sampling rectangle invalid");
    size_t same = 0, total = 0;
    for (LONG y = area.top; y < area.bottom; ++y) for (LONG x = area.left; x < area.right; ++x) {
        const size_t offset = (static_cast<size_t>(y) * pixels.width + x) * 4;
        if (pixels.bgra[offset] == GetBValue(expected) && pixels.bgra[offset + 1] == GetGValue(expected) &&
            pixels.bgra[offset + 2] == GetRValue(expected)) ++same;
        ++total;
    }
    return static_cast<double>(same) / total;
}
RECT toCapture(HWND host, HWND child, RECT inset, const explorer::VisualCaptureReport& report) {
    POINT origin{}; ClientToScreen(child, &origin);
    RECT outer{}; GetWindowRect(host, &outer);
    OffsetRect(&inset, origin.x - outer.left, origin.y - outer.top);
    require(!report.invisibleFrameTrimmed, "Theme test expects complete outer capture");
    return inset;
}
Variant property(IPropertyStore* store, REFPROPERTYKEY key) {
    Variant output;
    succeeded(store->GetValue(key, &output.value), "Ribbon theme property readback");
    return output;
}

bool nativeTests(explorer::PrivateDesktop& desktop, explorer::RibbonLayout layout) {
    const auto appsBefore = readSetting(L"AppsUseLightTheme");
    const auto systemBefore = readSetting(L"SystemUsesLightTheme");
    HIGHCONTRASTW contrastBefore{}; contrastBefore.cbSize = sizeof(contrastBefore);
    require(SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrastBefore), &contrastBefore, 0) != FALSE, "Read high contrast before test");
    succeeded(explorer::initializeProcessTheme(explorer::ThemeMode::Light), "Initialize native Light process");
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
    require(InitCommonControlsEx(&controls) != FALSE, "Common control initialization");
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW type{}; type.hInstance = instance; type.lpfnWndProc = hostProcedure;
    type.lpszClassName = L"WindowsExplorerPrivateThemeTest";
    require(RegisterClassW(&type) != 0, "Register private theme host");
    Window window{CreateWindowExW(0, type.lpszClassName, L"Windows 10 native theme fixture",
        WS_OVERLAPPEDWINDOW, 0, 0, 900, 550, nullptr, nullptr, instance, nullptr)};
    require(window.value != nullptr, "Create owned private theme host");
    explorer::NativeRibbon ribbon;
    succeeded(ribbon.initialize(window.value, instance, {}, layout), "Native Ribbon initialization");
    if (layout == explorer::RibbonLayout::InstalledWindows10 && ribbon.layout() != layout) {
        std::cout << "SKIP: installed Windows 10 theme layout unavailable, HRESULT=" <<
            static_cast<ULONG>(ribbon.installedLayoutStatus()) << '\n';
        return false;
    }
    ComPtr<IPropertyStore> store;
    succeeded(ribbon.framework()->QueryInterface(IID_PPV_ARGS(&store)), "Framework property store");
    ComPtr<IUnknown> frameworkIdentity, propertyIdentity;
    ComPtr<IUIFramework> roundTrip;
    succeeded(ribbon.framework()->QueryInterface(IID_PPV_ARGS(&frameworkIdentity)), "Framework COM identity");
    succeeded(store->QueryInterface(IID_PPV_ARGS(&propertyIdentity)), "Property store COM identity");
    succeeded(store->QueryInterface(IID_PPV_ARGS(&roundTrip)), "Property store framework round trip");
    require(frameworkIdentity == propertyIdentity && roundTrip.Get() == ribbon.framework(),
        "Framework/property store lost a consistent COM identity");
    ComPtr<IPropertyStore> nativeStore;
    succeeded(ribbon.nativeFramework()->QueryInterface(IID_PPV_ARGS(&nativeStore)), "Underlying native property store");
    DWORD facadeCount = 0, nativeCount = 0;
    const auto facadeCountResult = store->GetCount(&facadeCount);
    const auto nativeCountResult = nativeStore->GetCount(&nativeCount);
    require(facadeCountResult == nativeCountResult && facadeCount == nativeCount,
        "Facade changed framework property enumeration");
    for (DWORD index = 0; SUCCEEDED(nativeCountResult) && index < nativeCount; ++index) {
        PROPERTYKEY facadeKey{}, nativeKey{};
        const auto facadeKeyResult = store->GetAt(index, &facadeKey);
        const auto nativeKeyResult = nativeStore->GetAt(index, &nativeKey);
        require(facadeKeyResult == nativeKeyResult && IsEqualPropertyKey(facadeKey, nativeKey),
            "Facade translated a global property key or result");
    }
    succeeded(explorer::applyWindowTheme(window.value), "Apply initial Light to private host");
    succeeded(explorer::applyRibbonTheme(ribbon.framework()), "Apply native Ribbon Light");
    auto lightBackground = property(store.Get(), UI_PKEY_GlobalBackgroundColor);
    auto lightHighlight = property(store.Get(), UI_PKEY_GlobalHighlightColor);
    auto lightText = property(store.Get(), UI_PKEY_GlobalTextColor);
    require(lightBackground.value.vt == VT_UI4 && lightHighlight.value.vt == VT_UI4 && lightText.value.vt == VT_UI4, "Public Ribbon color types differ");
    const int top = static_cast<int>(ribbon.height()) + 12;
    Window tree{CreateWindowExW(0, WC_TREEVIEWW, L"", WS_CHILD | WS_VISIBLE | TVS_HASLINES,
        12, top, 220, 240, window.value, nullptr, instance, nullptr)};
    Window list{CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_ICON,
        245, top, 620, 240, window.value, nullptr, instance, nullptr)};
    Window edit{CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"Address fixture", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        12, top + 250, 500, 24, window.value, nullptr, instance, nullptr)};
    require(tree.value && list.value && edit.value, "Create private native controls");
    // Match native Explorer styling before the helper; it must preserve ItemsView.
    succeeded(SetWindowTheme(list.value, L"ItemsView", nullptr), "Select native ItemsView style");
    // Thread-local activation occurs solely on the private, non-input desktop.
    ShowWindow(window.value, SW_SHOWNOACTIVATE); SetActiveWindow(window.value); UpdateWindow(window.value); pump();
    require(GetActiveWindow() == window.value, "Private native caption activation failed");
    bool inputUnchanged = false;
    succeeded(desktop.verifyIsolation(&inputUnchanged), "Visible-private host isolation");
    require(inputUnchanged, "Input desktop changed for theme painting");
    HRESULT wrongThread = S_OK;
    std::thread worker([&] { wrongThread = explorer::applyWindowTheme(window.value); }); worker.join();
    require(wrongThread == E_ACCESSDENIED, "Cross-thread window theme mutation accepted");
    require(explorer::applyWindowTheme(GetDesktopWindow()) == E_ACCESSDENIED, "Foreign desktop theme mutation accepted");

    succeeded(explorer::initializeProcessTheme(explorer::ThemeMode::Dark), "Request process-only native Dark");
    const auto state = explorer::themeState();
    if (state.actual == explorer::ThemeMode::Dark) {
        const auto palette = explorer::themePalette();
        require(palette.content == RGB(32,32,32) && palette.ribbon == RGB(32,32,32) &&
            palette.navigation == RGB(25,25,25) && palette.address == RGB(25,25,25) &&
            palette.status == RGB(51,51,51) && palette.caption == RGB(0,0,0), "Dark surfaces differ from stock Windows 10 reference");
        succeeded(explorer::applyWindowTheme(window.value), "Native dark descendant opt-in");
        succeeded(explorer::applyRibbonTheme(ribbon.framework()), "Native dark Ribbon property");
        succeeded(ribbon.flush(), "Flush native Dark rendering"); pump();
        require(TreeView_GetBkColor(tree.value) == palette.navigation && TreeView_GetTextColor(tree.value) == palette.text,
            "Tree native dark colors ignored");
        require(ListView_GetBkColor(list.value) == palette.content && ListView_GetTextBkColor(list.value) == palette.content &&
            ListView_GetTextColor(list.value) == palette.text, "ItemsView native dark colors ignored");
        const auto dc = GetDC(edit.value); require(dc != nullptr, "Private edit DC");
        const auto brush = explorer::themeControlColor(edit.value, dc, WM_CTLCOLOREDIT);
        LOGBRUSH brushInfo{};
        require(brush && GetObjectW(brush, sizeof(brushInfo), &brushInfo) == sizeof(brushInfo) &&
            brushInfo.lbColor == palette.address && GetTextColor(dc) == palette.text && GetBkColor(dc) == palette.address,
            "Dark address brush/text pairing incorrect");
        EnableWindow(edit.value, FALSE);
        explorer::themeControlColor(edit.value, dc, WM_CTLCOLORSTATIC);
        require(GetTextColor(dc) == palette.disabledText, "Disabled address text lacks disabled color");
        EnableWindow(edit.value, TRUE); ReleaseDC(edit.value, dc);
        const auto path = std::filesystem::current_path()/L"artifacts"/(L"theme-native-dark-" + std::to_wstring(GetCurrentProcessId()) + L".png");
        std::filesystem::create_directories(path.parent_path());
        explorer::VisualCaptureOptions options;
        explorer::VisualCaptureReport report;
        succeeded(explorer::captureWindowPng(desktop, window.value, path, options, report), "Capture actual private native dark Ribbon and controls");
        auto inventory = path; inventory.replace_extension(L".json");
        succeeded(explorer::writeVisualCaptureReport(inventory, report), "Dark native inventory");
        const auto pixels = decode(path);
        require(flatFraction(pixels, toCapture(window.value, tree.value, {20,20,180,180}, report), RGB(25,25,25)) > 0.99,
            "Rendered navigation is not stock #191919");
        require(flatFraction(pixels, toCapture(window.value, list.value, {20,20,580,180}, report), RGB(32,32,32)) > 0.99,
            "Rendered ItemsView is not stock #202020");
        // Locate the actual command band. The host's client origin can still
        // include the caption while the native Ribbon extends into it, and
        // command positions change as the Ribbon's size definitions evolve.
        HWND commandBand = nullptr;
        EnumChildWindows(window.value, [](HWND child, LPARAM context) -> BOOL {
            wchar_t name[128]{}; GetClassNameW(child, name, static_cast<int>(std::size(name)));
            if (wcscmp(name, L"UIRibbonCommandBar") != 0) return TRUE;
            *reinterpret_cast<HWND*>(context) = child; return FALSE;
        }, reinterpret_cast<LPARAM>(&commandBand));
        require(commandBand != nullptr, "Native Ribbon command band missing");
        RECT band{}; require(GetClientRect(commandBand, &band) != FALSE, "Read native Ribbon dimensions");
        require(band.right > 850 && band.bottom > 80, "Native Ribbon fixture band unexpectedly small");
        // This fixture's 900px host leaves the rightmost20px body strip empty.
        const RECT ribbonSample{band.right - 32, 34, band.right - 12, band.bottom - 16};
        const auto ribbonArea = toCapture(window.value, commandBand, ribbonSample, report);
        const auto fraction = flatFraction(pixels, ribbonArea, RGB(32,32,32));
        if (fraction <= 0.99) std::cerr << "Native dark Ribbon sample fraction=" << fraction << " rect=" << ribbonArea.left << ','
            << ribbonArea.top << ',' << ribbonArea.right << ',' << ribbonArea.bottom << '\n';
        require(fraction > 0.99, "Native Ribbon is not stock #202020 dark background");
        require(flatFraction(pixels, {360, 8, 650, 24}, RGB(0,0,0)) > 0.99,
            "Native active caption is not stock #000000");
        std::cout << "PASS: real native Dark caption/Ribbon/ItemsView/navigation pixels, private desktop, stock Windows 10 palette; build=" << state.windowsBuild << '\n';
    } else {
        require(state.highContrast || !state.forcedDarkAvailable, "Available native Dark silently stayed Light");
        std::cout << "SKIP: native dark paint unavailable on this OS build or high contrast; system colors preserved\n";
    }

    succeeded(explorer::initializeProcessTheme(explorer::ThemeMode::Light), "Return process to native Light");
    succeeded(explorer::applyWindowTheme(window.value), "Restore Light controls");
    succeeded(explorer::applyRibbonTheme(ribbon.framework()), "Restore native Light Ribbon"); pump();
    auto restoredBackground = property(store.Get(), UI_PKEY_GlobalBackgroundColor);
    auto restoredHighlight = property(store.Get(), UI_PKEY_GlobalHighlightColor);
    auto restoredText = property(store.Get(), UI_PKEY_GlobalTextColor);
    require(restoredBackground.value.ulVal == lightBackground.value.ulVal && restoredHighlight.value.ulVal == lightHighlight.value.ulVal &&
        restoredText.value.ulVal == lightText.value.ulVal, "Light native Ribbon defaults were overwritten");
    require(TreeView_GetBkColor(tree.value) == GetSysColor(COLOR_WINDOW) && ListView_GetBkColor(list.value) == GetSysColor(COLOR_WINDOW),
        "Dark-to-Light colors stayed stale");
    const auto dc = GetDC(edit.value); require(dc != nullptr, "Light edit DC");
    require(explorer::themeControlColor(edit.value, dc, WM_CTLCOLOREDIT) == nullptr || explorer::themeState().highContrast,
        "Normal Light control painting overridden"); ReleaseDC(edit.value, dc);
    succeeded(explorer::refreshProcessTheme(), "Refresh same process preference");
    require(explorer::themeState().requested == explorer::ThemeMode::Light, "Refresh discarded explicit process preference");
    HIGHCONTRASTW contrastAfter{}; contrastAfter.cbSize = sizeof(contrastAfter);
    require(SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrastAfter), &contrastAfter, 0) != FALSE &&
        contrastAfter.dwFlags == contrastBefore.dwFlags, "Theme tests changed user high contrast settings");
    require(readSetting(L"AppsUseLightTheme") == appsBefore && readSetting(L"SystemUsesLightTheme") == systemBefore,
        "Theme tests changed Windows registry preferences");
    bool visibleInput = true;
    succeeded(desktop.visibleWindowsOnInputDesktop(visibleInput), "Theme input desktop visibility guard");
    require(!visibleInput, "Theme tests showed a window on the input desktop");
    explorer::forgetRibbonTheme(ribbon.framework());
    ribbon.reset();
    succeeded(explorer::initializeProcessTheme(explorer::ThemeMode::Auto), "Restore process Auto policy");
    std::cout << "PASS: native Light restoration, unchanged public Ribbon defaults, read-only preferences, thread ownership\n";
    return true;
}

void startupDarkRoundTrip(explorer::RibbonLayout layout) {
    succeeded(explorer::initializeProcessTheme(explorer::ThemeMode::Dark), "Initialize startup Dark policy");
    if (explorer::themeState().actual != explorer::ThemeMode::Dark) {
        succeeded(explorer::initializeProcessTheme(explorer::ThemeMode::Auto), "Restore unsupported startup Auto");
        return;
    }
    const auto instance = GetModuleHandleW(nullptr);
    Window dark{CreateWindowExW(0, L"WindowsExplorerPrivateThemeTest", L"Startup Dark fixture",
        WS_OVERLAPPEDWINDOW, 0, 0, 900, 550, nullptr, nullptr, instance, nullptr)};
    require(dark.value != nullptr, "Startup Dark window");
    explorer::NativeRibbon darkRibbon;
    succeeded(darkRibbon.initialize(dark.value, instance, {}, layout), "Startup Dark Ribbon");
    require(darkRibbon.layout() == layout, "Startup Dark silently changed Ribbon layout");
    succeeded(explorer::applyWindowTheme(dark.value), "Startup Dark native window");
    succeeded(explorer::applyRibbonTheme(darkRibbon.framework()), "Startup Dark native Ribbon");
    succeeded(explorer::initializeProcessTheme(explorer::ThemeMode::Light), "Startup Dark to Light policy");
    succeeded(explorer::applyWindowTheme(dark.value), "Startup Dark to Light window");
    succeeded(explorer::applyRibbonTheme(darkRibbon.framework()), "Startup Dark to Light Ribbon");
    Window fresh{CreateWindowExW(0, L"WindowsExplorerPrivateThemeTest", L"Fresh native Light fixture",
        WS_OVERLAPPEDWINDOW, 0, 0, 900, 550, nullptr, nullptr, instance, nullptr)};
    require(fresh.value != nullptr, "Fresh Light window");
    explorer::NativeRibbon lightRibbon;
    succeeded(lightRibbon.initialize(fresh.value, instance, {}, layout), "Fresh native Light Ribbon");
    require(lightRibbon.layout() == layout, "Fresh Light silently changed Ribbon layout");
    succeeded(explorer::applyWindowTheme(fresh.value), "Fresh Light native window");
    succeeded(explorer::applyRibbonTheme(lightRibbon.framework()), "Fresh Light native Ribbon");
    ComPtr<IPropertyStore> darkStore, lightStore;
    succeeded(darkRibbon.framework()->QueryInterface(IID_PPV_ARGS(&darkStore)), "Restored Light property store");
    succeeded(lightRibbon.framework()->QueryInterface(IID_PPV_ARGS(&lightStore)), "Fresh Light property store");
    for (const auto key : {&UI_PKEY_GlobalBackgroundColor, &UI_PKEY_GlobalHighlightColor, &UI_PKEY_GlobalTextColor}) {
        auto restored = property(darkStore.Get(), *key);
        auto factory = property(lightStore.Get(), *key);
        require(restored.value.vt == VT_UI4 && factory.value.vt == VT_UI4 && restored.value.ulVal == factory.value.ulVal,
            "Startup Dark restoration differs from fresh native Light defaults");
    }
    explorer::forgetRibbonTheme(darkRibbon.framework());
    explorer::forgetRibbonTheme(lightRibbon.framework());
    darkRibbon.reset(); lightRibbon.reset();
    succeeded(explorer::initializeProcessTheme(explorer::ThemeMode::Auto), "Restore startup test Auto");
    std::cout << "PASS: startup Dark-to-Light matches a fresh native Light framework\n";
}
} // namespace

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    if (argc > 2 || (argc == 2 && std::string_view(argv[1]) != "--installed")) return 2;
    const auto layout = argc == 2 ? explorer::RibbonLayout::InstalledWindows10 : explorer::RibbonLayout::Authored;
    // No COM, HWND, or theme activation precedes the private desktop transition.
    explorer::PrivateDesktop desktop;
    if (FAILED(desktop.initialize())) return 2;
    const auto initialized = OleInitialize(nullptr);
    if (FAILED(initialized)) return 3;
    int result = 0;
    try {
        policyTests(); std::cout << "PASS: Auto/Light/Dark, high contrast, legacy/unsupported dark policy\n";
        if (nativeTests(desktop, layout)) startupDarkRoundTrip(layout);
        else result = 77;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; result = 1; }
    OleUninitialize();
    return result;
}
