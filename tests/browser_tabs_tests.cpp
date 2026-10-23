#include "explorer/browser_tabs.hpp"
#include "explorer/headless_visual.hpp"
#include "explorer/theme.hpp"

#include <oleacc.h>
#include <wrl/client.h>
#include <exception>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using explorer::BrowserTabs;
using TabId = BrowserTabs::TabId;
unsigned assertions = 0;

void require(bool value, const char* description) {
    ++assertions;
    if (!value) throw std::runtime_error(description);
}
void succeeded(HRESULT status, const char* description) { require(SUCCEEDED(status), description); }

struct Notifications {
    BrowserTabs* tabs = nullptr;
    unsigned newClicks = 0, closeClicks = 0, selections = 0;
    TabId selected = BrowserTabs::NoTab;
};

LRESULT CALLBACK hostProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<Notifications*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = static_cast<Notifications*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state && state->tabs) {
        if (message == WM_COMMAND && HIWORD(wparam) == BN_CLICKED) {
            if (LOWORD(wparam) == BrowserTabs::NewTabCommand &&
                reinterpret_cast<HWND>(lparam) == state->tabs->newButton()) ++state->newClicks;
            if (LOWORD(wparam) == BrowserTabs::CloseTabCommand &&
                reinterpret_cast<HWND>(lparam) == state->tabs->closeButton()) ++state->closeClicks;
        }
        if (message == WM_NOTIFY) {
            const auto* header = reinterpret_cast<NMHDR*>(lparam);
            if (header && header->hwndFrom == state->tabs->window() &&
                header->idFrom == BrowserTabs::ControlId && header->code == TCN_SELCHANGE) {
                ++state->selections;
                state->selected = state->tabs->selected();
            }
        }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

struct Host {
    HWND window = nullptr;
    explicit Host(Notifications& state) {
        WNDCLASSW type{};
        type.lpfnWndProc = hostProcedure;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"Explorer.BrowserTabs.HeadlessTest";
        if (!RegisterClassW(&type)) require(GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "Register private test host");
        window = CreateWindowExW(0, type.lpszClassName, L"Private tab test host", WS_OVERLAPPEDWINDOW,
            0, 0, 900, 200, nullptr, nullptr, type.hInstance, &state);
        require(window != nullptr, "Create hidden private test host");
    }
    ~Host() { if (window) DestroyWindow(window); }
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;
};

TabId add(BrowserTabs& tabs, std::wstring_view label) {
    TabId id = BrowserTabs::NoTab;
    succeeded(tabs.add(label, &id), "Append native tab");
    require(id != BrowserTabs::NoTab && tabs.indexOf(id) >= 0, "Successful add exposes a valid stable ID");
    require(TabCtrl_GetItemCount(tabs.window()) == static_cast<int>(tabs.ids().size()), "Native/model count agreement");
    return id;
}

std::wstring labelAt(HWND window, int index) {
    wchar_t text[256]{};
    TCITEMW item{};
    item.mask = TCIF_TEXT;
    item.pszText = text;
    item.cchTextMax = static_cast<int>(std::size(text));
    require(SendMessageW(window, TCM_GETITEMW, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&item)) != 0,
        "Read native accessible tab label");
    return text;
}

void accessible(HWND window, LONG expectedRole, const wchar_t* expectedName = nullptr) {
    Microsoft::WRL::ComPtr<IAccessible> object;
    succeeded(AccessibleObjectFromWindow(window, static_cast<DWORD>(OBJID_CLIENT), IID_PPV_ARGS(&object)), "Native MSAA object");
    VARIANT self{};
    self.vt = VT_I4;
    self.lVal = CHILDID_SELF;
    VARIANT role{};
    succeeded(object->get_accRole(self, &role), "Native accessible role");
    const bool matches = role.vt == VT_I4 && role.lVal == expectedRole;
    VariantClear(&role);
    require(matches, "Standard native accessibility role");
    if (expectedName) {
        BSTR name = nullptr;
        const HRESULT hr = object->get_accName(self, &name);
        const bool nameMatches = SUCCEEDED(hr) && name && std::wstring_view(name, SysStringLen(name)) == expectedName;
        SysFreeString(name);
        require(nameMatches, "Native button keeps its complete accessible name");
    }
}

void basicAndSelection() {
    Notifications state;
    Host host(state);
    BrowserTabs tabs;
    state.tabs = &tabs;
    TabId output = 123;
    require(FAILED(tabs.add(L"Not created", &output)) && output == BrowserTabs::NoTab, "Uncreated add fails with no ID");
    require(tabs.add(L"Null output", nullptr) == E_POINTER, "Null ID output rejected");
    require(tabs.selected() == BrowserTabs::NoTab && tabs.ids().empty(), "Default model empty");
    require(tabs.idAt(-1) == BrowserTabs::NoTab && tabs.idAt(0) == BrowserTabs::NoTab &&
        tabs.indexOf(BrowserTabs::NoTab) == -1, "Invalid conversions return explicit sentinels");
    require(tabs.create(nullptr, GetModuleHandleW(nullptr)) == E_INVALIDARG, "Null parent rejected");
    succeeded(tabs.create(host.window, GetModuleHandleW(nullptr)), "Create native tab chrome");
    const HWND control = tabs.window();
    require(tabs.create(host.window, GetModuleHandleW(nullptr)) == HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED) &&
        tabs.window() == control, "Repeated create retains existing chrome");
    wchar_t type[64]{};
    GetClassNameW(control, type, static_cast<int>(std::size(type)));
    require(std::wstring_view(type) == WC_TABCONTROLW, "Real WC_TABCONTROL implementation");
    for (const HWND window : {control, tabs.newButton(), tabs.closeButton()}) {
        require(GetParent(window) == host.window, "Notifications route to parent through sibling controls");
        require(!IsWindowVisible(window), "Private test never shows parent or children");
        require((GetWindowLongPtrW(window, GWL_STYLE) & WS_TABSTOP) != 0, "Native keyboard focus stop");
    }
    require(!IsWindowEnabled(tabs.closeButton()) && IsWindowEnabled(tabs.newButton()), "Empty state disables only Close tab");
    accessible(control, ROLE_SYSTEM_PAGETABLIST);
    accessible(tabs.newButton(), ROLE_SYSTEM_PUSHBUTTON, L"New tab");
    accessible(tabs.closeButton(), ROLE_SYSTEM_PUSHBUTTON, L"Close tab");
    const TabId a = add(tabs, L"Documents"), b = add(tabs, L"Downloads"), c = add(tabs, L"Pictures");
    require(a != b && b != c && a != c && tabs.selected() == a, "Unique IDs and first-only automatic selection");
    require(IsWindowEnabled(tabs.closeButton()) != FALSE, "Nonempty state enables Close tab");
    for (int index = 0; index < 3; ++index)
        require(tabs.indexOf(tabs.idAt(index)) == index, "ID/index round trip");
    require(tabs.idAt(3) == BrowserTabs::NoTab && tabs.idAt((std::numeric_limits<int>::max)()) == BrowserTabs::NoTab,
        "Out of range native indexes never expose IDs");
    require(tabs.select(BrowserTabs::NoTab) == E_INVALIDARG &&
        tabs.select((std::numeric_limits<TabId>::max)()) == E_INVALIDARG && tabs.selected() == a,
        "Invalid selection leaves active tab intact");
    succeeded(tabs.select(b), "Programmatically select middle tab");
    require(tabs.selected() == b && state.selections == 0, "Programmatic selection needs no synthetic notification");
    // TCM_SETCURFOCUS follows the native selection/notification path for tabs
    // without TCS_BUTTONS. No input injection, activation or desktop switching.
    TabCtrl_SetCurFocus(control, 2);
    require(tabs.selected() == c && state.selections != 0 && state.selected == c,
        "Parent reads native selection during direct TCN_SELCHANGE");
    // BM_CLICK follows native button hit testing at client (0, 0). Creation
    // leaves zero-sized controls, so lay them out without showing the host.
    tabs.layout({0, 0, 800, BrowserTabs::height(96)}, 96);
    for (const HWND button : {tabs.newButton(), tabs.closeButton()}) {
        RECT client{};
        require(GetClientRect(button, &client) != FALSE && !IsRectEmpty(&client),
            "Native command button has a nonempty client area before BM_CLICK");
        require(!IsWindowVisible(button), "Laid-out command button remains hidden with its private host");
    }
    SendMessageW(tabs.newButton(), BM_CLICK, 0, 0);
    SendMessageW(tabs.closeButton(), BM_CLICK, 0, 0);
    require(state.newClicks == 1 && state.closeClicks == 1, "Both native commands reach parent with dedicated HWNDs/IDs");
    require(tabs.ids().size() == 3 && tabs.selected() == c, "Buttons leave content and model decisions to parent");
    succeeded(tabs.setLabel(b, L"Renamed & folder \u4e2d\u6587"), "Update native Unicode label");
    require(labelAt(control, tabs.indexOf(b)) == L"Renamed & folder \u4e2d\u6587", "Native text stores complete Unicode label");
    succeeded(tabs.setLabel(b, L""), "Empty label gets accessible fallback");
    require(labelAt(control, tabs.indexOf(b)) == L"New tab", "Empty labels have a usable name");
    const std::wstring embedded(L"invalid\0label", 13);
    require(tabs.setLabel(b, embedded) == E_INVALIDARG && labelAt(control, tabs.indexOf(b)) == L"New tab",
        "Embedded NUL cannot silently truncate a label");
    output = a;
    require(tabs.add(embedded, &output) == E_INVALIDARG && output == BrowserTabs::NoTab &&
        tabs.ids().size() == 3 && tabs.selected() == c, "Invalid insertion label preserves model and exposes no ID");
    require(tabs.setLabel(BrowserTabs::NoTab, L"Invalid") == E_INVALIDARG, "Invalid label ID rejected");
    succeeded(tabs.remove(a), "Remove inactive tab before selection");
    require(tabs.selected() == c && tabs.indexOf(c) == 1 && tabs.indexOf(a) == -1,
        "Removing earlier inactive tab preserves selected identity");
    succeeded(tabs.remove(c), "Remove active final tab");
    require(tabs.selected() == b, "Active final removal selects left neighbor");
    const TabId d = add(tabs, L"Right neighbor"), e = add(tabs, L"Last");
    succeeded(tabs.remove(b), "Remove active first tab");
    require(tabs.selected() == d, "Active first removal selects right neighbor");
    succeeded(tabs.remove(e), "Remove inactive later tab");
    require(tabs.selected() == d, "Removing later inactive tab preserves selection");
    require(tabs.remove(a) == E_INVALIDARG && tabs.selected() == d, "Retired ID cannot remove another tab");
    succeeded(tabs.remove(d), "Remove final tab");
    require(tabs.selected() == BrowserTabs::NoTab && tabs.ids().empty() &&
        !IsWindowEnabled(tabs.closeButton()), "Final removal leaves an explicit empty state");
    require(add(tabs, L"Reopened") > e, "Retired IDs are never reused");
    state.tabs = nullptr;
}

struct FailNativeMessage {
    HWND window;
    UINT message;
    LRESULT result;
    bool intercepted = false;
    FailNativeMessage(HWND target, UINT blocked, LRESULT failure) : window(target), message(blocked), result(failure) {
        require(SetWindowSubclass(window, procedure, 2, reinterpret_cast<DWORD_PTR>(this)) != FALSE,
            "Install owned native failure fixture");
    }
    ~FailNativeMessage() { RemoveWindowSubclass(window, procedure, 2); }
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
        UINT_PTR, DWORD_PTR reference) {
        auto& self = *reinterpret_cast<FailNativeMessage*>(reference);
        if (message == self.message) { self.intercepted = true; return self.result; }
        return DefSubclassProc(window, message, wparam, lparam);
    }
};

void failureAtomicity() {
    Notifications state;
    Host host(state);
    BrowserTabs tabs;
    succeeded(tabs.create(host.window, GetModuleHandleW(nullptr)), "Create failure fixture chrome");
    const TabId a = add(tabs, L"A"), b = add(tabs, L"B");
    succeeded(tabs.select(b), "Select fixture tab");
    const auto before = tabs.ids();
    TabId output = a;
    {
        FailNativeMessage failure(tabs.window(), TCM_INSERTITEMW, -1);
        require(FAILED(tabs.add(L"Rejected insertion", &output)) && failure.intercepted, "Native insertion failure propagates");
    }
    require(output == BrowserTabs::NoTab && tabs.ids() == before && tabs.selected() == b &&
        TabCtrl_GetItemCount(tabs.window()) == 2, "Failed insertion preserves entries/selection and exposes no ID");
    const TabId c = add(tabs, L"C");
    require(c == b + 1, "Failed insertion does not consume an ID");
    {
        FailNativeMessage failure(tabs.window(), TCM_DELETEITEM, FALSE);
        require(FAILED(tabs.remove(b)) && failure.intercepted, "Native deletion failure propagates");
    }
    require(tabs.ids().size() == 3 && tabs.selected() == b && TabCtrl_GetItemCount(tabs.window()) == 3,
        "Failed deletion preserves entries/selection");
    {
        FailNativeMessage failure(tabs.window(), TCM_SETITEMW, FALSE);
        require(FAILED(tabs.setLabel(b, L"Rejected rename")) && failure.intercepted, "Native label failure propagates");
    }
    require(labelAt(tabs.window(), tabs.indexOf(b)) == L"B" && tabs.selected() == b, "Failed rename retains native label and selection");
    succeeded(tabs.remove(b), "Remove active middle tab");
    require(tabs.selected() == c, "Active middle removal prefers right neighbor");
}

RECT parentRect(HWND window, HWND parent) {
    RECT rect{};
    require(GetWindowRect(window, &rect) != FALSE, "Read native layout bounds");
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&rect), 2);
    return rect;
}

void layoutAndLifetime() {
    Notifications state;
    Host host(state);
    BrowserTabs tabs;
    succeeded(tabs.create(host.window, GetModuleHandleW(nullptr)), "Create layout fixture chrome");
    const TabId first = add(tabs, L"First");
    for (int i = 0; i < 32; ++i) add(tabs, L"Scrollable folder " + std::to_wstring(i));
    require(!(GetWindowLongPtrW(tabs.window(), GWL_STYLE) & TCS_MULTILINE), "Tab overflow remains native single-line scrolling");
    for (const UINT dpi : {96u, 144u, 192u}) {
        require(BrowserTabs::height(dpi) == MulDiv(34, static_cast<int>(dpi), 96), "Logical strip height scales with DPI");
        for (const LONG width : {800L, 220L, 100L, 64L, 2L, 0L}) {
            const RECT bounds{7, 11, 7 + width, 11 + BrowserTabs::height(dpi)};
            tabs.layout(bounds, dpi);
            const RECT nativeTabs = parentRect(tabs.window(), host.window);
            const RECT newButton = parentRect(tabs.newButton(), host.window);
            const RECT closeButton = parentRect(tabs.closeButton(), host.window);
            for (const RECT rect : {nativeTabs, newButton, closeButton})
                require(rect.left >= bounds.left && rect.right <= bounds.right && rect.right >= rect.left &&
                    rect.top >= bounds.top && rect.bottom <= bounds.bottom, "Native chrome fits assigned narrow bounds");
            require(nativeTabs.right <= newButton.left && newButton.right <= closeButton.left, "Buttons never overlap tabs or each other");
            if (width >= 2) require(newButton.right > newButton.left && closeButton.right > closeButton.left,
                "Both commands retain clickable width in narrow layouts");
            require(tabs.selected() == first, "DPI/overflow layout preserves active ID");
        }
    }
    require(BrowserTabs::height(0) == BrowserTabs::height(96), "Unset DPI uses logical default");
    HFONT font = static_cast<HFONT>(GetStockObject(SYSTEM_FONT));
    tabs.setFont(font);
    for (const HWND window : {tabs.window(), tabs.newButton(), tabs.closeButton()})
        require(reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0)) == font, "All native chrome receives borrowed font");
    succeeded(tabs.applyTheme(), "Existing theme hooks apply to all native chrome");
    HWND content = CreateWindowExW(0, L"Static", L"Parent-owned content", WS_CHILD,
        0, 40, 100, 100, host.window, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(content != nullptr, "Create independent parent content sentinel");
    const HWND oldTabs = tabs.window(), oldNew = tabs.newButton(), oldClose = tabs.closeButton();
    const TabId last = tabs.ids().back();
    tabs.reset();
    require(!tabs.window() && !tabs.newButton() && !tabs.closeButton() && tabs.ids().empty() &&
        tabs.selected() == BrowserTabs::NoTab, "Reset clears chrome and model");
    require(!IsWindow(oldTabs) && !IsWindow(oldNew) && !IsWindow(oldClose) && IsWindow(content),
        "Reset destroys only owned chrome and preserves parent content");
    tabs.reset();
    succeeded(tabs.create(host.window, GetModuleHandleW(nullptr)), "Recreate after reset");
    require(add(tabs, L"New lifetime") > last && tabs.indexOf(first) == -1, "Reset/create cannot revive stale IDs");
    DestroyWindow(host.window);
    host.window = nullptr;
    require(!tabs.window() && !tabs.newButton() && !tabs.closeButton() && tabs.ids().empty() &&
        tabs.selected() == BrowserTabs::NoTab, "Parent destruction retires native handles and IDs before object teardown");
    tabs.reset();
}
} // namespace

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    // Must precede COM, themes, common-control initialization and every HWND.
    // The guard never switches or shows the input desktop. No app is launched.
    explorer::PrivateDesktop desktop;
    const HRESULT isolated = desktop.initialize();
    if (FAILED(isolated)) { std::cerr << "FAIL: PrivateDesktop initialization\n"; return 2; }
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(apartment)) { std::cerr << "FAIL: private accessibility apartment\n"; return 2; }
    int result = 0;
    try {
        succeeded(desktop.verifyIsolation(), "Private desktop verified before native controls");
        succeeded(explorer::initializeProcessTheme(), "Initialize current process theme after isolation");
        basicAndSelection();
        failureAtomicity();
        layoutAndLifetime();
        succeeded(desktop.verifyIsolation(), "Input desktop unchanged after native tests");
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        result = 1;
    }
    if (FAILED(desktop.verifyIsolation())) result = 1;
    CoUninitialize();
    std::cout << "Browser tab assertions: " << assertions << "; failures: " << result << '\n';
    return result;
}
