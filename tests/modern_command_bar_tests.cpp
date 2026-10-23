// Optional standalone target: link explorer_core (including modern_command_bar)
// and oleacc. Run only with --private-desktop. No CMake changes are made here.
#include "explorer/modern_command_bar.hpp"
#include "explorer/commands.hpp"
#include "explorer/headless_visual.hpp"
#include "explorer/theme.hpp"

#include <oleacc.h>
#include <wrl/client.h>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct Apartment {
    Apartment() { require(SUCCEEDED(OleInitialize(nullptr)), "Initialize isolated OLE apartment"); }
    ~Apartment() { OleUninitialize(); }
};
struct Host {
    explorer::ModernCommandBar bar;
    HWND window = nullptr;
    UINT command = 0, dropdown = 0;
    static constexpr const wchar_t* className = L"Explorer.ModernCommandBar.PrivateTest";
    static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_NCCREATE) {
            const auto creation = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
        }
        const auto self = reinterpret_cast<Host*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self && message == WM_COMMAND && reinterpret_cast<HWND>(lParam) == self->bar.window()) {
            self->command = LOWORD(wParam); return 0;
        }
        if (self && message == WM_NOTIFY) {
            const auto notification = reinterpret_cast<NMHDR*>(lParam);
            LRESULT result = 0;
            if (self->bar.notification(notification, result)) return result;
            if (notification && notification->hwndFrom == self->bar.window() && notification->code == TBN_DROPDOWN) {
                self->dropdown = static_cast<UINT>(reinterpret_cast<NMTOOLBARW*>(notification)->iItem);
                return TBDDRET_NODEFAULT; // Never opens a popup in this fixture.
            }
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    void initialize() {
        WNDCLASSW type{};
        type.lpfnWndProc = proc;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = className;
        require(RegisterClassW(&type) != 0, "Register class only after private-desktop admission");
        window = CreateWindowExW(0, className, L"Owned hidden command bar test", WS_POPUP,
            0, 0, 1800, 900, nullptr, nullptr, type.hInstance, this);
        require(window != nullptr, "Create hidden isolated host");
        require(SUCCEEDED(bar.create(window, type.hInstance)), "Create native command bar");
    }
    ~Host() {
        if (window) DestroyWindow(window);
        UnregisterClassW(className, GetModuleHandleW(nullptr));
    }
};
constexpr std::array<UINT, 9> commands{
    explorer::NewFolder, explorer::Cut, explorer::Copy, explorer::Paste, explorer::Rename,
    explorer::Delete, explorer::SortMenu, explorer::ViewMenu, explorer::FileMenu};

void geometry(Host& host, int width, UINT dpi) {
    const RECT slot{0, 0, width, 1}; // Deliberately not enough height: wrapping must grow.
    host.bar.layout(slot, dpi);
    RECT client{}; GetClientRect(host.bar.window(), &client);
    require(client.bottom == host.bar.height(dpi), "Parent can reserve the actual wrapped height");
    for (size_t index = 0; index < commands.size(); ++index) {
        TBBUTTON button{};
        require(SendMessageW(host.bar.window(), TB_GETBUTTON, index, reinterpret_cast<LPARAM>(&button)) != 0,
            "Every semantic command remains a native button");
        require(static_cast<UINT>(button.idCommand) == commands[index] && !(button.fsState & TBSTATE_HIDDEN),
            "No command silently disappears in a narrow layout");
        RECT item{};
        require(SendMessageW(host.bar.window(), TB_GETITEMRECT, index, reinterpret_cast<LPARAM>(&item)) != 0,
            "Read actual wrapped native button bounds");
        require(item.left >= 0 && item.right <= client.right && item.right > item.left &&
            item.top >= 0 && item.bottom <= client.bottom && item.bottom > item.top,
            "Every command fits inside the measured toolbar");
        wchar_t caption[80]{};
        require(SendMessageW(host.bar.window(), TB_GETBUTTONTEXTW, commands[index],
            reinterpret_cast<LPARAM>(caption)) > 0, "Native accessible captions survive layout");
    }
}
void click(Host& host, UINT command) {
    RECT item{};
    require(SendMessageW(host.bar.window(), TB_GETRECT, command, reinterpret_cast<LPARAM>(&item)) != 0,
        "Find owned command rectangle");
    const auto point = MAKELPARAM((item.left + item.right) / 2, (item.top + item.bottom) / 2);
    // Messages target only the fixture's private HWND; there is no system input.
    SendMessageW(host.bar.window(), WM_LBUTTONDOWN, MK_LBUTTON, point);
    SendMessageW(host.bar.window(), WM_LBUTTONUP, 0, point);
}
void exercise() {
    Host host;
    host.initialize();
    require(!IsWindowVisible(host.window) && !IsWindowVisible(host.bar.window()), "Fixture never becomes visible");
    require((GetWindowLongPtrW(host.bar.window(), GWL_STYLE) & WS_TABSTOP) != 0,
        "Native toolbar participates in keyboard focus traversal");
    for (const auto dpi : {96u, 144u, 192u}) {
        geometry(host, 1600, dpi);
        const auto wide = host.bar.height(dpi);
        geometry(host, 320, dpi);
        require(host.bar.height(dpi) > wide, "Narrow widths grow native rows");
        geometry(host, 96, dpi);
    }
    geometry(host, 1600, 96);
    click(host, explorer::Copy);
    require(host.command == explorer::Copy, "Native toolbar delivers semantic WM_COMMAND to its own host");
    click(host, explorer::SortMenu);
    require(host.dropdown == explorer::SortMenu, "Native whole dropdown delivers TBN_DROPDOWN");
    host.bar.setCommandState(explorer::Copy, false, true);
    const auto state = SendMessageW(host.bar.window(), TB_GETSTATE, explorer::Copy, 0);
    require(!(state & TBSTATE_ENABLED) && (state & TBSTATE_CHECKED), "Native enabled/checked states follow host state");
    host.command = 0;
    click(host, explorer::Copy);
    require(host.command == 0, "Disabled native command cannot dispatch");
    geometry(host, 96, 96); // Disabled buttons must not break row accessibility.

    Microsoft::WRL::ComPtr<IAccessible> accessible;
    require(SUCCEEDED(AccessibleObjectFromWindow(host.bar.window(), static_cast<DWORD>(OBJID_CLIENT),
        IID_PPV_ARGS(&accessible))), "Expose the native toolbar accessibility provider");
    VARIANT child{}; child.vt = VT_I4; child.lVal = 3; // Copy, third native button.
    BSTR name = nullptr;
    const auto nameStatus = accessible->get_accName(child, &name);
    const bool named = SUCCEEDED(nameStatus) && name && std::wstring_view(name) == L"Copy";
    SysFreeString(name);
    require(named, "Native accessibility retains full command names");
    accessible.Reset();

    DestroyWindow(host.window); host.window = nullptr;
    require(host.bar.window() == nullptr, "Parent-first destruction retires the owned HWND callback");
    host.bar.reset(); host.bar.reset();
    require(host.bar.window() == nullptr, "Reset is idempotent after parent destruction");
    host.window = CreateWindowExW(0, Host::className, L"Owned replacement host", WS_POPUP,
        0, 0, 1800, 900, nullptr, nullptr, GetModuleHandleW(nullptr), &host);
    require(host.window && SUCCEEDED(host.bar.create(host.window, GetModuleHandleW(nullptr))),
        "Recreate after parent-first teardown without stale resource ownership");
    geometry(host, 320, 96);
    const auto childWindow = host.bar.window();
    host.bar.reset();
    require(!IsWindow(childWindow) && IsWindow(host.window), "Reset destroys only the owned toolbar");
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2 || std::string_view(argv[1]) != "--private-desktop") return 77;
    try {
        // This guard precedes OLE, common-control initialization and all HWNDs.
        explorer::PrivateDesktop desktop;
        require(SUCCEEDED(desktop.initialize()), "Admit a private non-input desktop before UI/OLE");
        Apartment apartment;
        require(SUCCEEDED(explorer::initializeProcessTheme()), "Read existing process theme policy");
        exercise();
        bool unchanged = false;
        require(SUCCEEDED(desktop.verifyIsolation(&unchanged)) && unchanged, "Input desktop remains unchanged");
        std::cout << "PASS modern command bar native wrapping, routing, accessibility and lifetime\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL modern command bar: " << error.what() << '\n';
        return 1;
    }
}
