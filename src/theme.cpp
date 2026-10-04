#include "explorer/theme.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <array>
#include <mutex>
#include <unordered_map>

namespace explorer {
namespace {
using Microsoft::WRL::ComPtr;
enum class PreferredAppMode { Default, AllowDark, ForceDark, ForceLight };
using SetPreferredAppMode = PreferredAppMode(WINAPI*)(PreferredAppMode);
using AllowDarkModeForApp = bool(WINAPI*)(bool);
using AllowDarkModeForWindow = bool(WINAPI*)(HWND, bool);
using FlushMenuThemes = void(WINAPI*)();
using RtlGetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
struct CompositionAttributeData { int attribute; void* data; SIZE_T bytes; };
using SetWindowCompositionAttribute = BOOL(WINAPI*)(HWND, CompositionAttributeData*);

// Internal compatibility property used by the native Windows 10 Ribbon. It is
// intentionally separate from documented UI_PKEY_Global* HSB color properties.
constexpr PROPERTYKEY RibbonDarkMode{
    {2004, 0x7363, 0x696e, {0x84, 0x41, 0x79, 0x8a, 0xcf, 0x5a, 0xeb, 0xb7}}, VT_BOOL};

struct Backend {
    HMODULE module = nullptr;
    DWORD build = 0;
    SetPreferredAppMode setMode = nullptr;
    AllowDarkModeForApp allowApp = nullptr;
    AllowDarkModeForWindow allowWindow = nullptr;
    FlushMenuThemes flushMenus = nullptr;
    SetWindowCompositionAttribute setComposition = nullptr;
    bool compatible = false;
    Backend() noexcept {
        OSVERSIONINFOW version{};
        version.dwOSVersionInfoSize = sizeof(version);
        const auto ntdll = GetModuleHandleW(L"ntdll.dll");
        const auto getVersion = ntdll ? reinterpret_cast<RtlGetVersion>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
        if (!getVersion || getVersion(&version) < 0 || version.dwMajorVersion != 10 || version.dwMinorVersion != 0) return;
        build = version.dwBuildNumber;
        // Ordinal 135 changed signature in 1903. Never call an unrecognized ABI.
        const bool legacy = build == 17763;
        const bool modern = build == 18362 || build == 18363 || (build >= 19041 && build <= 19045) ||
            build == 22000 || build == 22621 || build == 22631 || build == 26100;
        if (!legacy && !modern) return;
        module = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) return;
        allowWindow = reinterpret_cast<AllowDarkModeForWindow>(GetProcAddress(module, MAKEINTRESOURCEA(133)));
        const auto appPolicy = GetProcAddress(module, MAKEINTRESOURCEA(135));
        if (modern) setMode = reinterpret_cast<SetPreferredAppMode>(appPolicy);
        else allowApp = reinterpret_cast<AllowDarkModeForApp>(appPolicy);
        flushMenus = reinterpret_cast<FlushMenuThemes>(GetProcAddress(module, MAKEINTRESOURCEA(136)));
        if (modern && build < 22000) {
            const auto user32 = GetModuleHandleW(L"user32.dll");
            setComposition = user32 ? reinterpret_cast<SetWindowCompositionAttribute>(GetProcAddress(user32, "SetWindowCompositionAttribute")) : nullptr;
        }
        compatible = allowWindow && appPolicy;
    }
    ~Backend() { if (module) FreeLibrary(module); }
};
Backend& backend() noexcept { static Backend value; return value; }

struct Cache {
    std::mutex mutex;
    ThemeState state;
    ThemePalette palette;
    bool initialized = false;
};
Cache& cache() noexcept { static Cache value; return value; }
struct RibbonDefaults {
    DWORD thread = 0;
    std::array<ULONG, 3> colors{};
    bool wasDark = false;
};
struct RibbonCache {
    std::mutex mutex;
    // Borrowed framework identities; forgetRibbonTheme removes before reset.
    std::unordered_map<IUnknown*, RibbonDefaults> defaults;
};
RibbonCache& ribbonCache() noexcept { static RibbonCache value; return value; }

ThemePalette makePalette(const ThemeState& state) noexcept {
    ThemePalette value;
    if (state.actual == ThemeMode::Dark && !state.highContrast) {
        // Flat surfaces measured from the stock Windows 10 online reference.
        value.content = value.ribbon = RGB(32, 32, 32);
        value.navigation = value.address = RGB(25, 25, 25);
        value.status = RGB(51, 51, 51);
        value.caption = RGB(0, 0, 0);
        value.text = RGB(255, 255, 255);
        value.controlText = value.captionText = value.text;
        value.disabledText = RGB(128, 128, 128);
        value.border = RGB(83, 83, 83);
        value.selection = RGB(77, 77, 77);
        value.selectionText = value.text;
        value.hover = RGB(56, 56, 56);
    } else {
        value.content = value.navigation = value.address = GetSysColor(COLOR_WINDOW);
        value.ribbon = value.status = GetSysColor(COLOR_BTNFACE);
        value.caption = GetSysColor(COLOR_ACTIVECAPTION);
        value.text = GetSysColor(COLOR_WINDOWTEXT);
        value.controlText = GetSysColor(COLOR_BTNTEXT);
        value.captionText = GetSysColor(COLOR_CAPTIONTEXT);
        value.disabledText = GetSysColor(COLOR_GRAYTEXT);
        value.border = GetSysColor(COLOR_WINDOWFRAME);
        value.selection = value.hover = GetSysColor(COLOR_HIGHLIGHT);
        value.selectionText = GetSysColor(COLOR_HIGHLIGHTTEXT);
    }
    return value;
}

int systemSurface(ThemeSurface surface) noexcept {
    switch (surface) {
    case ThemeSurface::Ribbon: case ThemeSurface::Status: return COLOR_BTNFACE;
    case ThemeSurface::Caption: return COLOR_ACTIVECAPTION;
    default: return COLOR_WINDOW;
    }
}

HRESULT updateTheme(ThemeMode requested) noexcept {
    auto& native = backend();
    ThemeState state;
    state.requested = requested;
    state.windowsBuild = native.build;
    state.nativeDarkAvailable = native.compatible;
    state.forcedDarkAvailable = native.compatible && native.setMode;
    DWORD preference = 1;
    DWORD size = sizeof(preference);
    const LSTATUS read = RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &preference, &size);
    state.appsUseLightTheme = read != ERROR_SUCCESS || preference != 0;
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    const bool readContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) != FALSE;
    // On an unexpected query failure, favor system colors over custom colors.
    state.highContrast = !readContrast || (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
    state.actual = resolveThemeMode(requested, state.appsUseLightTheme, state.highContrast,
        state.nativeDarkAvailable, state.forcedDarkAvailable);
    if (native.compatible) {
        if (native.setMode) {
            PreferredAppMode policy = PreferredAppMode::ForceLight;
            if (state.actual == ThemeMode::Dark) policy = requested == ThemeMode::Dark ?
                PreferredAppMode::ForceDark : PreferredAppMode::AllowDark;
            native.setMode(policy);
        } else native.allowApp(state.actual == ThemeMode::Dark);
        if (native.flushMenus) native.flushMenus();
    }
    auto& values = cache();
    {
        const std::lock_guard lock(values.mutex);
        values.state = state;
        values.palette = makePalette(state);
        values.initialized = true;
    }
    return requested == ThemeMode::Dark && state.actual != ThemeMode::Dark && !state.highContrast ? S_FALSE : S_OK;
}

bool ownedWindow(HWND window) noexcept {
    DWORD process = 0;
    const DWORD thread = GetWindowThreadProcessId(window, &process);
    return thread == GetCurrentThreadId() && process == GetCurrentProcessId();
}

thread_local bool applying = false;
struct ApplyingGuard {
    ApplyingGuard() noexcept { applying = true; }
    ~ApplyingGuard() { applying = false; }
};

void applyControl(HWND window, const ThemeState& state, const ThemePalette& palette) noexcept {
    if (!ownedWindow(window)) return;
    auto& native = backend();
    const bool dark = state.actual == ThemeMode::Dark && !state.highContrast;
    if (native.compatible) native.allowWindow(window, dark);
    wchar_t type[128]{};
    GetClassNameW(window, type, static_cast<int>(std::size(type)));
    if (_wcsicmp(type, WC_TREEVIEWW) == 0) {
        SetWindowTheme(window, L"Explorer", nullptr);
        TreeView_SetBkColor(window, palette.navigation);
        TreeView_SetTextColor(window, palette.text);
    } else if (_wcsicmp(type, WC_LISTVIEWW) == 0) {
        // IExplorerBrowser owns the ItemsView theme; replacing it changes the
        // stock selection/hover painting. Opt-in plus public color messages suffice.
        ListView_SetBkColor(window, palette.content);
        ListView_SetTextBkColor(window, palette.content);
        ListView_SetTextColor(window, palette.text);
    } else if (_wcsicmp(type, L"Edit") == 0 || _wcsicmp(type, L"Button") == 0 ||
               _wcsicmp(type, L"Static") == 0 || _wcsicmp(type, WC_HEADERW) == 0) {
        SetWindowTheme(window, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    } else if (_wcsicmp(type, L"ComboBox") == 0 || _wcsicmp(type, L"ComboBoxEx32") == 0) {
        SetWindowTheme(window, dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
    }
}

struct ApplyContext { ThemeState state; ThemePalette palette; };
BOOL CALLBACK applyChild(HWND window, LPARAM parameter) noexcept {
    const auto& context = *reinterpret_cast<const ApplyContext*>(parameter);
    applyControl(window, context.state, context.palette);
    return TRUE;
}
} // namespace

COLORREF ThemePalette::surface(ThemeSurface value) const noexcept {
    switch (value) {
    case ThemeSurface::Navigation: return navigation;
    case ThemeSurface::Ribbon: return ribbon;
    case ThemeSurface::Address: return address;
    case ThemeSurface::Status: return status;
    case ThemeSurface::Caption: return caption;
    default: return content;
    }
}
COLORREF ThemePalette::foreground(ThemeSurface value) const noexcept {
    switch (value) {
    case ThemeSurface::Ribbon: case ThemeSurface::Status: return controlText;
    case ThemeSurface::Caption: return captionText;
    default: return text;
    }
}

ThemeMode resolveThemeMode(ThemeMode requested, bool appsUseLightTheme,
    bool highContrast, bool nativeDarkAvailable, bool forcedDarkAvailable) noexcept {
    if (highContrast || !nativeDarkAvailable || requested == ThemeMode::Light) return ThemeMode::Light;
    if (requested == ThemeMode::Dark) return forcedDarkAvailable || !appsUseLightTheme ? ThemeMode::Dark : ThemeMode::Light;
    return appsUseLightTheme ? ThemeMode::Light : ThemeMode::Dark;
}

HRESULT initializeProcessTheme(ThemeMode mode) noexcept {
    if (mode != ThemeMode::Auto && mode != ThemeMode::Light && mode != ThemeMode::Dark) return E_INVALIDARG;
    return updateTheme(mode);
}
HRESULT refreshProcessTheme() noexcept { return updateTheme(themeState().requested); }
ThemeState themeState() noexcept {
    auto& values = cache();
    const std::lock_guard lock(values.mutex);
    return values.state;
}
ThemePalette themePalette() noexcept {
    auto& values = cache();
    const std::lock_guard lock(values.mutex);
    return values.initialized ? values.palette : makePalette(values.state);
}
ThemePalette themePaletteFor(ThemeMode actual, bool highContrast) noexcept {
    ThemeState state;
    state.actual = actual;
    state.highContrast = highContrast;
    return makePalette(state);
}

HRESULT applyWindowTheme(HWND window) noexcept {
    if (!window || !IsWindow(window)) return E_INVALIDARG;
    if (!ownedWindow(window)) return E_ACCESSDENIED;
    if (applying) return S_FALSE;
    const ApplyingGuard guard;
    const ApplyContext context{themeState(), themePalette()};
    applyControl(window, context.state, context.palette);
    EnumChildWindows(window, applyChild, reinterpret_cast<LPARAM>(&context));
    auto& native = backend();
    const BOOL dark = context.state.actual == ThemeMode::Dark && !context.state.highContrast;
    if (!(GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD) && native.compatible) {
        SetWindowTheme(window, L"Explorer", nullptr);
        // Attribute 20 is documented on Win11. 19/20 on Win10 are explicitly
        // compatibility-only; a failure leaves the native caption untouched.
        HRESULT caption = DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
        if (FAILED(caption) && native.build < 22000)
            caption = DwmSetWindowAttribute(window, 19, &dark, sizeof(dark));
        if (native.setComposition) {
            BOOL darkComposition = dark;
            CompositionAttributeData data{26, &darkComposition, sizeof(darkComposition)};
            if (native.setComposition(window, &data)) caption = S_OK;
        }
        if (FAILED(caption) && dark) return S_FALSE;
        // Refresh native nonclient painting for its existing activation state.
        // This does not activate the window or change focus/the input desktop.
        SendMessageW(window, WM_NCACTIVATE, GetActiveWindow() == window, 0);
    }
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    return S_OK;
}

HRESULT applyRibbonTheme(IUIFramework* framework) noexcept {
    if (!framework) return E_POINTER;
    const auto state = themeState();
    if (!state.nativeDarkAvailable) return S_FALSE;
    ComPtr<IPropertyStore> store;
    HRESULT result = framework->QueryInterface(IID_PPV_ARGS(&store));
    if (FAILED(result)) return result;
    ComPtr<IUnknown> identity;
    result = framework->QueryInterface(IID_PPV_ARGS(&identity));
    if (FAILED(result)) return result;
    const std::array<const PROPERTYKEY*, 3> keys{
        &UI_PKEY_GlobalBackgroundColor, &UI_PKEY_GlobalHighlightColor, &UI_PKEY_GlobalTextColor};
    RibbonDefaults defaults;
    bool captured = false;
    {
        auto& ribbons = ribbonCache();
        const std::lock_guard lock(ribbons.mutex);
        if (const auto found = ribbons.defaults.find(identity.Get()); found != ribbons.defaults.end()) {
            defaults = found->second;
            captured = true;
        }
    }
    if (captured && defaults.thread != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
    if (!captured) {
        defaults.thread = GetCurrentThreadId();
        for (size_t i = 0; i < keys.size(); ++i) {
            PROPVARIANT original{};
            result = store->GetValue(*keys[i], &original);
            if (SUCCEEDED(result) && original.vt != VT_UI4) result = E_UNEXPECTED;
            if (SUCCEEDED(result)) defaults.colors[i] = original.ulVal;
            PropVariantClear(&original);
            if (FAILED(result)) return result;
        }
    }
    const bool dark = state.actual == ThemeMode::Dark && !state.highContrast;
    PROPVARIANT value{};
    result = InitPropVariantFromBoolean(dark, &value);
    if (SUCCEEDED(result)) result = store->SetValue(RibbonDarkMode, value);
    PropVariantClear(&value);
    if (FAILED(result)) return result;
    result = store->Commit();
    if (FAILED(result)) return result;
    // Native DarkModeRibbon changes Global* values itself. Restore the original
    // factory/native Light palette when leaving Dark, never a chosen HSB color.
    if (!dark && defaults.wasDark) {
        for (size_t i = 0; i < keys.size(); ++i) {
            PROPVARIANT original{};
            result = InitPropVariantFromUInt32(defaults.colors[i], &original);
            if (SUCCEEDED(result)) result = store->SetValue(*keys[i], original);
            PropVariantClear(&original);
            if (FAILED(result)) return result;
        }
        result = store->Commit();
        if (FAILED(result)) return result;
    }
    defaults.wasDark = dark;
    try {
        auto& ribbons = ribbonCache();
        const std::lock_guard lock(ribbons.mutex);
        ribbons.defaults.insert_or_assign(identity.Get(), defaults);
    } catch (...) { return E_OUTOFMEMORY; }
    return S_OK;
}
void forgetRibbonTheme(IUIFramework* framework) noexcept {
    if (!framework) return;
    ComPtr<IUnknown> identity;
    if (FAILED(framework->QueryInterface(IID_PPV_ARGS(&identity)))) return;
    auto& ribbons = ribbonCache();
    const std::lock_guard lock(ribbons.mutex);
    ribbons.defaults.erase(identity.Get());
}

HBRUSH themeBrush(ThemeSurface surface) noexcept {
    const auto state = themeState();
    if (state.actual != ThemeMode::Dark || state.highContrast) return GetSysColorBrush(systemSurface(surface));
    struct Brushes {
        std::array<HBRUSH, 6> values{};
        Brushes() noexcept {
            ThemeState dark;
            dark.actual = ThemeMode::Dark;
            const auto palette = makePalette(dark);
            for (size_t i = 0; i < values.size(); ++i)
                values[i] = CreateSolidBrush(palette.surface(static_cast<ThemeSurface>(i)));
        }
        ~Brushes() { for (auto value : values) if (value) DeleteObject(value); }
    };
    static Brushes brushes;
    const auto index = static_cast<size_t>(surface);
    return index < brushes.values.size() ? brushes.values[index] : nullptr;
}

HBRUSH themeControlColor(HWND control, HDC dc, UINT message, ThemeSurface surface) noexcept {
    if (!control || !dc || !ownedWindow(control)) return nullptr;
    const auto state = themeState();
    if (state.actual != ThemeMode::Dark && !state.highContrast) return nullptr;
    if (message < WM_CTLCOLORMSGBOX || message > WM_CTLCOLORSTATIC) return nullptr;
    const auto palette = themePalette();
    SetTextColor(dc, IsWindowEnabled(control) ? palette.foreground(surface) : palette.disabledText);
    SetBkColor(dc, palette.surface(surface));
    SetBkMode(dc, message == WM_CTLCOLORSTATIC ? TRANSPARENT : OPAQUE);
    return themeBrush(surface);
}
} // namespace explorer
