#include "explorer/ui_direction.hpp"
#include "explorer/headless_visual.hpp"
#include <algorithm>
#include <array>
#include <commctrl.h>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void succeeded(HRESULT status, const char* message) {
    if (FAILED(status)) throw std::runtime_error(std::string(message) + " HRESULT=" + std::to_string(static_cast<ULONG>(status)));
}
bool samePoint(const POINT& left, const POINT& right) { return left.x == right.x && left.y == right.y; }
bool sameRect(const RECT& left, const RECT& right) {
    return left.left == right.left && left.top == right.top && left.right == right.right && left.bottom == right.bottom;
}
bool samePolicy(const explorer::UiDirectionPolicy& left, const explorer::UiDirectionPolicy& right) {
    return left.rightToLeft == right.rightToLeft && left.provenance == right.provenance && left.nativeStatus == right.nativeStatus &&
        left.nativeReadingLayout == right.nativeReadingLayout && left.language == right.language && left.uiLanguages == right.uiLanguages;
}
std::vector<std::wstring> independentLanguages() {
    ULONG count = 0, capacity = 0;
    constexpr DWORD flags = MUI_LANGUAGE_NAME | MUI_UI_FALLBACK;
    require(GetThreadPreferredUILanguages(flags, &count, nullptr, &capacity) && count && capacity > 1,
        "Independent native MUI size read failed");
    std::wstring buffer(capacity, L'\0');
    require(GetThreadPreferredUILanguages(flags, &count, buffer.data(), &capacity) != FALSE, "Independent native MUI names read failed");
    std::vector<std::wstring> languages;
    for (size_t start = 0; start + 1 < capacity && buffer[start];) {
        const auto end = buffer.find(L'\0', start);
        require(end != buffer.npos, "Native MUI name was not terminated");
        languages.emplace_back(buffer.substr(start, end - start)); start = end + 1;
    }
    require(languages.size() == count, "Independent MUI count mismatch");
    return languages;
}
DWORD independentReadingLayout(const wchar_t* name) {
    DWORD layout = MAXDWORD;
    require(GetLocaleInfoEx(name, LOCALE_IREADINGLAYOUT | LOCALE_RETURN_NUMBER, reinterpret_cast<LPWSTR>(&layout),
        static_cast<int>(sizeof(layout) / sizeof(wchar_t))) != 0, "Independent native locale direction lookup failed");
    return layout;
}
void nativeLocalePolicy() {
    const auto originalThreadLanguage = GetThreadUILanguage();
    const auto originalLanguages = independentLanguages();
    for (const auto& [language, rightToLeft] : std::array<std::pair<const wchar_t*, bool>, 6>{{
        {L"ar-SA", true}, {L"ar", true}, {L"he-IL", true}, {L"he", true}, {L"en-GB", false}, {L"en-US", false}}}) {
        explorer::UiDirectionPolicy policy;
        succeeded(explorer::loadLocaleUiDirection(language, &policy), "Read native Arabic/Hebrew/English policy");
        const auto layout = independentReadingLayout(language);
        require(policy.rightToLeft == rightToLeft && policy.rightToLeft == (layout == 1u) && policy.nativeReadingLayout == layout &&
            policy.language == language && policy.uiLanguages.empty() && policy.nativeStatus == S_OK &&
            policy.provenance == explorer::UiDirectionProvenance::NativeLocale, "Native locale policy/provenance mismatch");
    }
    explorer::UiDirectionPolicy japanese;
    succeeded(explorer::loadLocaleUiDirection(L"ja-JP", &japanese), "Read native vertical/horizontal Japanese policy");
    require(japanese.nativeReadingLayout == independentReadingLayout(L"ja-JP") && japanese.nativeReadingLayout == 2u &&
        !japanese.rightToLeft && japanese.provenance == explorer::UiDirectionProvenance::NativeLocale,
        "Vertical-script metadata was incorrectly treated as horizontal RTL");
    explorer::UiDirectionPolicy threadPolicy;
    succeeded(explorer::loadThreadUiDirection(&threadPolicy), "Read actual effective thread MUI direction");
    const auto actualLayout = independentReadingLayout(originalLanguages.front().c_str());
    require(threadPolicy.language == originalLanguages.front() && threadPolicy.uiLanguages == originalLanguages &&
        threadPolicy.nativeReadingLayout == actualLayout && threadPolicy.rightToLeft == (actualLayout == 1u) &&
        threadPolicy.provenance == explorer::UiDirectionProvenance::NativeLocale && threadPolicy.nativeStatus == S_OK,
        "Thread policy used a formatting locale or lost the actual MUI fallback order");
    require(GetThreadUILanguage() == originalThreadLanguage && independentLanguages() == originalLanguages,
        "Read-only direction lookup changed UI language settings");
}
struct OwnedWindows {
    std::wstring className;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    ATOM atom = 0;
    std::vector<HWND> handles;
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const auto creation = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            if (creation && creation->lpCreateParams) {
                // An explicit fixture root must be LTR/RTL independently of
                // any existing process default. Only this owned HWND changes;
                // children keep the native inheritance being tested below.
                const auto mirrored = *static_cast<const bool*>(creation->lpCreateParams);
                const auto style = GetWindowLongPtrW(window, GWL_EXSTYLE);
                SetLastError(ERROR_SUCCESS);
                const auto previous = SetWindowLongPtrW(window, GWL_EXSTYLE,
                    mirrored ? style | WS_EX_LAYOUTRTL : style & ~static_cast<LONG_PTR>(WS_EX_LAYOUTRTL));
                if (!previous && GetLastError() != ERROR_SUCCESS) return FALSE;
            }
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
    OwnedWindows() {
        const auto desktop = explorer::PrivateDesktop::current();
        require(desktop && desktop->ready() && SUCCEEDED(desktop->verifyIsolation()), "Direction HWND tests require the owned private desktop");
        className = L"WindowsExplorer.DirectionFixture." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
        WNDCLASSEXW type{sizeof(type)}; type.lpfnWndProc = procedure; type.hInstance = instance; type.lpszClassName = className.c_str();
        atom = RegisterClassExW(&type); require(atom != 0, "Register private direction fixture class");
    }
    ~OwnedWindows() {
        for (auto iterator = handles.rbegin(); iterator != handles.rend(); ++iterator) if (IsWindow(*iterator)) DestroyWindow(*iterator);
        if (atom) UnregisterClassW(className.c_str(), instance);
    }
    HWND add(DWORD extended, DWORD style, HWND parent, int x, int y, int width, int height) {
        bool rootMirrored = (extended & WS_EX_LAYOUTRTL) != 0;
        const auto window = CreateWindowExW(extended, className.c_str(), L"Owned direction fixture", style,
            x, y, width, height, parent, nullptr, instance, (style & WS_CHILD) ? nullptr : &rootMirrored);
        require(window != nullptr, "Create hidden owned direction fixture");
        try { handles.push_back(window); }
        catch (...) { DestroyWindow(window); throw; }
        return window;
    }
};
void verifyWindowLayout(HWND window, bool expected) {
    bool actual = !expected;
    succeeded(explorer::windowUiDirection(window, &actual), "Read actual inherited window direction");
    require(actual == expected && ((GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_LAYOUTRTL) != 0) == expected,
        "Window direction disagreed with actual native inherited style");
    const auto dc = GetDC(window); require(dc != nullptr, "Get native owned window DC");
    const auto layout = GetLayout(dc); ReleaseDC(window, dc);
    require(layout != GDI_ERROR && ((layout & LAYOUT_RTL) != 0) == expected, "Native DC layout disagreed with owned window direction");
    require(!IsWindowVisible(window), "Direction fixture made a host visible");
}
void nativeWindowsAndMappings() {
    OwnedWindows windows;
    const auto ltr = windows.add(0, WS_POPUP, nullptr, 80, 90, 640, 360);
    const auto rtl = windows.add(WS_EX_LAYOUTRTL, WS_POPUP, nullptr, 780, 90, 640, 360);
    const auto noInheritance = windows.add(WS_EX_LAYOUTRTL | WS_EX_NOINHERITLAYOUT, WS_POPUP, nullptr, 80, 510, 640, 360);
    const auto ltrChild = windows.add(0, WS_CHILD, ltr, 48, 35, 180, 70);
    const auto rtlChild = windows.add(0, WS_CHILD, rtl, 48, 35, 180, 70);
    const auto explicitRtlChild = windows.add(WS_EX_LAYOUTRTL, WS_CHILD, ltr, 250, 35, 140, 70);
    const auto ltrUnderRtl = windows.add(0, WS_CHILD, noInheritance, 48, 35, 180, 70);
    const auto ownedPopup = windows.add(0, WS_POPUP, rtl, 850, 510, 120, 80);
    for (const auto& [window, mirrored] : std::array<std::pair<HWND, bool>, 8>{{
        {ltr, false}, {rtl, true}, {noInheritance, true}, {ltrChild, false}, {rtlChild, true},
        {explicitRtlChild, true}, {ltrUnderRtl, false}, {ownedPopup, false}}}) verifyWindowLayout(window, mirrored);
    require(GetWindow(ownedPopup, GW_OWNER) == rtl, "Native owned-popup relation was lost");
    for (const auto& [parent, child] : std::array<std::pair<HWND, HWND>, 3>{{{ltr, ltrChild}, {rtl, rtlChild}, {noInheritance, ltrUnderRtl}}}) {
        RECT screen{}, parentScreen{}, client{};
        require(GetWindowRect(child, &screen) && GetWindowRect(parent, &parentScreen), "Read real parent/child screen bounds");
        const RECT expectedClient{48, 35, 228, 105};
        succeeded(explorer::mapUiRect(nullptr, parent, screen, &client), "Map actual screen child bounds to normalized parent rectangle");
        require(sameRect(client, expectedClient), "Mapped child bounds differ from actual native creation coordinates");
        RECT roundTrip{};
        succeeded(explorer::mapUiRect(parent, nullptr, client, &roundTrip), "Map parent rectangle back to actual screen coordinates");
        require(sameRect(roundTrip, screen), "Normalized mirrored rectangle did not round trip to native screen bounds");
        bool mirrored = false; succeeded(explorer::windowUiDirection(parent, &mirrored), "Read physical parent layout");
        require(screen.left == (mirrored ? parentScreen.right - 228 : parentScreen.left + 48) &&
            screen.right == (mirrored ? parentScreen.right - 48 : parentScreen.left + 228) &&
            screen.top == parentScreen.top + 35 && screen.bottom == parentScreen.top + 105,
            "Actual native child placement did not mirror its physical screen edges");
        POINT leftScreen{screen.left, screen.top}, rightScreen{screen.right, screen.bottom}, leftClient{}, rightClient{};
        succeeded(explorer::mapUiPoint(nullptr, parent, leftScreen, &leftClient), "Map independent physical left point");
        succeeded(explorer::mapUiPoint(nullptr, parent, rightScreen, &rightClient), "Map independent physical right point");
        require((mirrored && leftClient.x > rightClient.x) || (!mirrored && leftClient.x < rightClient.x),
            "Independent point mapping incorrectly applied RECT edge swapping");
        POINT pointRoundTrip{};
        succeeded(explorer::mapUiPoint(parent, nullptr, leftClient, &pointRoundTrip), "Round trip an independent mirrored point");
        require(samePoint(pointRoundTrip, leftScreen), "Independent point did not round trip through real native window coordinates");
    }
    const RECT crossInput{12, 7, 83, 49}; RECT crossMapped{}, crossBack{};
    succeeded(explorer::mapUiRect(explicitRtlChild, rtlChild, crossInput, &crossMapped), "Map between independently mirrored native children");
    succeeded(explorer::mapUiRect(rtlChild, explicitRtlChild, crossMapped, &crossBack), "Round trip between mirrored native children");
    require(sameRect(crossInput, crossBack), "Child-to-child mirrored rectangle lost native identities");
    const auto desktop = explorer::PrivateDesktop::current();
    succeeded(desktop->verifyIsolation(), "Direction mapping changed the input desktop");
}
void nativeMenuPlacementAndFailures() {
    OwnedWindows windows;
    const auto ltr = windows.add(0, WS_POPUP, nullptr, 90, 90, 400, 250);
    const auto rtl = windows.add(WS_EX_LAYOUTRTL, WS_POPUP, nullptr, 600, 90, 400, 250);
    const UINT retained = TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_VERTICAL | TPM_BOTTOMALIGN | TPM_NOANIMATION;
    const RECT actualBounds{-75, 16, 44, 82};
    for (const auto& [owner, mirrored] : std::array<std::pair<HWND, bool>, 2>{{{ltr, false}, {rtl, true}}}) {
        UINT flags = std::numeric_limits<UINT>::max();
        succeeded(explorer::popupUiFlags(owner, retained | TPM_CENTERALIGN | TPM_LAYOUTRTL, &flags), "Read actual owner popup flags");
        require(flags == (retained | TPM_LEFTALIGN | (mirrored ? TPM_LAYOUTRTL : 0u)),
            "Popup alignment/layout or unrelated native flags were lost");
        explorer::UiPopupPlacement placement;
        succeeded(explorer::popupUiPlacement(owner, actualBounds, retained | TPM_CENTERALIGN, &placement), "Read logical-leading popup anchor");
        require(placement.rightToLeft == mirrored && placement.flags == flags &&
            placement.anchor.x == (mirrored ? actualBounds.right : actualBounds.left) && placement.anchor.y == actualBounds.bottom &&
            sameRect(placement.exclusion, actualBounds), "Native owner direction did not select the exact physical dropdown edge");
    }
    explorer::UiDirectionPolicy policy;
    policy.rightToLeft = true; policy.nativeStatus = E_ABORT; policy.nativeReadingLayout = 1u;
    policy.language = L"unchanged"; policy.uiLanguages = {L"unchanged"};
    const auto originalPolicy = policy;
    for (const auto& invalid : std::array<std::wstring, 4>{L"", std::wstring(L"en-US\0extra", 11),
        std::wstring(LOCALE_NAME_MAX_LENGTH, L'a'), L"definitely-not-a-locale!"}) {
        require(FAILED(explorer::loadLocaleUiDirection(invalid, &policy)) && samePolicy(policy, originalPolicy),
            "Invalid locale changed the caller's direction policy");
    }
    require(explorer::loadLocaleUiDirection(L"en-US", nullptr) == E_POINTER && explorer::loadThreadUiDirection(nullptr) == E_POINTER,
        "Null direction-policy outputs were accepted");
    const auto stale = windows.add(0, WS_CHILD, ltr, 0, 0, 10, 10); require(DestroyWindow(stale) != FALSE, "Destroy owned stale handle fixture");
    bool direction = true;
    require(FAILED(explorer::windowUiDirection(stale, &direction)) && direction &&
        FAILED(explorer::windowUiDirection(nullptr, &direction)) && direction && explorer::windowUiDirection(ltr, nullptr) == E_POINTER,
        "Invalid window direction changed output");
    POINT point{17, 29}; const auto originalPoint = point;
    RECT rectangle{10, 20, 30, 40}; const auto originalRect = rectangle;
    require(FAILED(explorer::mapUiPoint(stale, ltr, POINT{}, &point)) && samePoint(point, originalPoint) &&
        FAILED(explorer::mapUiRect(ltr, stale, RECT{}, &rectangle)) && sameRect(rectangle, originalRect) &&
        explorer::mapUiPoint(ltr, rtl, POINT{}, nullptr) == E_POINTER && explorer::mapUiRect(ltr, rtl, RECT{}, nullptr) == E_POINTER,
        "Invalid mapping changed a caller's coordinates");
    require(explorer::mapUiRect(nullptr, ltr, RECT{30, 20, 10, 40}, &rectangle) == E_INVALIDARG && sameRect(rectangle, originalRect),
        "Inverted source rectangle changed output");
    SetLastError(ERROR_INVALID_DATA);
    succeeded(explorer::mapUiPoint(nullptr, nullptr, point, &point), "Accept native zero-offset identity mapping despite prior last error");
    require(samePoint(point, originalPoint), "Identity point mapping changed coordinates");
    succeeded(explorer::mapUiRect(nullptr, nullptr, rectangle, &rectangle), "Accept native zero-offset identity rectangle mapping");
    require(sameRect(rectangle, originalRect), "Identity rectangle mapping changed coordinates");
    UINT flags = 0x1234;
    require(FAILED(explorer::popupUiFlags(stale, retained, &flags)) && flags == 0x1234 &&
        explorer::popupUiFlags(ltr, retained, nullptr) == E_POINTER, "Invalid popup owner changed flags");
    explorer::UiPopupPlacement placement{true, 0x1234, {17, 29}, {10, 20, 30, 40}};
    const auto originalPlacement = placement;
    for (const auto bounds : std::array<RECT, 3>{{{30, 20, 10, 40}, {10, 20, 10, 40}, {10, 40, 30, 20}}}) {
        require(explorer::popupUiPlacement(ltr, bounds, retained, &placement) == E_INVALIDARG &&
            placement.rightToLeft == originalPlacement.rightToLeft && placement.flags == originalPlacement.flags &&
            samePoint(placement.anchor, originalPlacement.anchor) && sameRect(placement.exclusion, originalPlacement.exclusion),
            "Invalid popup rectangle changed caller placement");
    }
    require(FAILED(explorer::popupUiPlacement(stale, actualBounds, retained, &placement)) &&
        placement.flags == originalPlacement.flags && samePoint(placement.anchor, originalPlacement.anchor) &&
        sameRect(placement.exclusion, originalPlacement.exclusion) && placement.rightToLeft == originalPlacement.rightToLeft &&
        explorer::popupUiPlacement(ltr, actualBounds, retained, nullptr) == E_POINTER,
        "Invalid popup owner/output changed placement");
    succeeded(explorer::PrivateDesktop::current()->verifyIsolation(), "Menu placement checks changed the input desktop");
}
struct NativeMenuReadback {
    static constexpr UINT_PTR timerId = 0x51d2;
    HWND owner;
    HMENU menu;
    bool ready = false, entered = false, initialized = false, exited = false, owned = false, cancelled = false;
    unsigned windows = 0;
    RECT bounds{};
    NativeMenuReadback(HWND host, HMENU popup) : owner(host), menu(popup) {
        DWORD process = 0;
        require(explorer::PrivateDesktop::current() && SUCCEEDED(explorer::PrivateDesktop::current()->verifyIsolation()) &&
            GetWindowThreadProcessId(owner, &process) == GetCurrentThreadId() && process == GetCurrentProcessId(),
            "Native menu readback must remain on the owned private STA");
        require(SetWindowSubclass(owner, procedure, timerId, reinterpret_cast<DWORD_PTR>(this)) != FALSE, "Subclass owned native menu owner");
        ready = SetTimer(owner, timerId, 80, nullptr) != 0;
        if (!ready) RemoveWindowSubclass(owner, procedure, timerId);
    }
    ~NativeMenuReadback() { if (ready) { KillTimer(owner, timerId); RemoveWindowSubclass(owner, procedure, timerId); } }
    static LRESULT CALLBACK procedure(HWND host, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR data) {
        auto& readback = *reinterpret_cast<NativeMenuReadback*>(data);
        if (message == WM_ENTERMENULOOP) readback.entered = true;
        if (message == WM_INITMENUPOPUP && reinterpret_cast<HMENU>(wparam) == readback.menu) readback.initialized = true;
        if (message == WM_EXITMENULOOP) readback.exited = true;
        if (message == WM_TIMER && wparam == timerId) {
            KillTimer(host, timerId);
            GUITHREADINFO information{sizeof(information)};
            readback.owned = GetGUIThreadInfo(GetCurrentThreadId(), &information) && information.hwndMenuOwner == host;
            if (readback.owned) EnumThreadWindows(GetCurrentThreadId(), [](HWND candidate, LPARAM context) -> BOOL {
                auto& actual = *reinterpret_cast<NativeMenuReadback*>(context);
                DWORD process = 0; wchar_t type[64]{};
                GetClassNameW(candidate, type, static_cast<int>(std::size(type)));
                if (wcscmp(type, L"#32768") != 0 || !IsWindowVisible(candidate) ||
                    GetWindowThreadProcessId(candidate, &process) != GetCurrentThreadId() || process != GetCurrentProcessId()) return TRUE;
                RECT menuBounds{};
                if (GetWindowRect(candidate, &menuBounds)) { actual.bounds = menuBounds; ++actual.windows; }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&readback));
            readback.cancelled = EndMenu() != FALSE;
            return 0;
        }
        return DefSubclassProc(host, message, wparam, lparam);
    }
};
void nativePopupAlignmentMatrix() {
    OwnedWindows windows;
    const auto ltr = windows.add(0, WS_POPUP, nullptr, 180, 180, 640, 360);
    const auto rtl = windows.add(WS_EX_LAYOUTRTL, WS_POPUP, nullptr, 180, 180, 640, 360);
    const auto desktop = explorer::PrivateDesktop::current();
    const HWND previousActive = GetActiveWindow();
    struct RestorePresentation {
        HWND previous;
        std::array<HWND, 2> owners;
        ~RestorePresentation() { for (const auto owner : owners) ShowWindow(owner, SW_HIDE); SetActiveWindow(previous); }
    } restorePresentation{previousActive, {ltr, rtl}};
    struct MenuOwner { HMENU value = CreatePopupMenu(); ~MenuOwner() { if (value) DestroyMenu(value); } } menu;
    require(menu.value && AppendMenuW(menu.value, MF_STRING, 1, L"Owned Hebrew \u05d0\u05d1\u05d2") &&
        AppendMenuW(menu.value, MF_STRING, 2, L"Owned Arabic \u0627\u0644\u0645\u0644\u0641"), "Create owned native alignment menu");
    for (const auto& [owner, mirrored] : std::array<std::pair<HWND, bool>, 2>{{{ltr, false}, {rtl, true}}}) {
        MONITORINFO monitor{sizeof(monitor)};
        require(GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor) != FALSE, "Read native private menu monitor");
        const POINT anchor{monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left) / 2,
            monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top) / 2};
        ShowWindow(owner, SW_SHOWNOACTIVATE); SetActiveWindow(owner);
        require(GetActiveWindow() == owner && SUCCEEDED(desktop->verifyIsolation()), "Activate only the private native menu owner");
        for (const bool textRtl : {false, true}) {
            std::array<RECT, 2> observed{};
            for (size_t alignment = 0; alignment < observed.size(); ++alignment) {
                const UINT flags = TPM_RETURNCMD | TPM_NOANIMATION |
                    (alignment ? TPM_RIGHTALIGN : TPM_LEFTALIGN) | (textRtl ? TPM_LAYOUTRTL : 0u);
                NativeMenuReadback readback(owner, menu.value);
                require(readback.ready, "Arm bounded owned native menu readback");
                const auto selected = TrackPopupMenuEx(menu.value, flags, anchor.x, anchor.y, owner, nullptr);
                observed[alignment] = readback.bounds;
                const auto width = readback.bounds.right - readback.bounds.left;
                std::cout << "native-popup ownerRTL=" << mirrored << " textRTL=" << textRtl << " requestedRight=" << alignment
                    << " flags=" << flags << " loop=" << readback.entered << '/' << readback.initialized << '/' << readback.exited
                    << " owner=" << readback.owned << " windows=" << readback.windows << " cancel=" << readback.cancelled
                    << " width=" << width << " leftDelta=" << readback.bounds.left - anchor.x
                    << " rightDelta=" << readback.bounds.right - anchor.x << " topDelta=" << readback.bounds.top - anchor.y << '\n';
                require(selected == 0 && readback.entered && readback.initialized && readback.exited && readback.owned &&
                    readback.windows == 1 && readback.cancelled && width > 0 && readback.bounds.top == anchor.y &&
                    (readback.bounds.left == anchor.x || readback.bounds.right == anchor.x),
                    "Actual private native alignment menu did not realize, anchor or cancel correctly");
                succeeded(desktop->verifyIsolation(), "Native alignment menu changed input desktop");
            }
            require(observed[0].right - observed[0].left == observed[1].right - observed[1].left &&
                ((observed[0].left == anchor.x && observed[1].right == anchor.x) ||
                 (observed[0].right == anchor.x && observed[1].left == anchor.x)),
                "Native LEFT/RIGHT alignment did not select opposite exact physical edges");
        }
        // Exercise the production helper against a real inherited child HWND's
        // physical bounds, rather than assuming flag names are physical edges.
        const auto anchorControl = windows.add(0, WS_CHILD | WS_VISIBLE, owner, 80, 60, 180, 60);
        RECT anchorBounds{};
        require(GetWindowRect(anchorControl, &anchorBounds) != FALSE, "Read actual native dropdown anchor control");
        explorer::UiPopupPlacement placement;
        const UINT helperBase = TPM_RETURNCMD | TPM_NOANIMATION | TPM_RIGHTBUTTON;
        succeeded(explorer::popupUiPlacement(owner, anchorBounds, helperBase | TPM_RIGHTALIGN | TPM_CENTERALIGN, &placement),
            "Read actual production native dropdown placement");
        NativeMenuReadback helperReadback(owner, menu.value);
        require(helperReadback.ready, "Arm production native placement readback");
        const auto helperSelected = TrackPopupMenuEx(menu.value, placement.flags, placement.anchor.x, placement.anchor.y, owner, nullptr);
        std::cout << "native-popup helper ownerRTL=" << mirrored << " flags=" << placement.flags
            << " anchor=" << placement.anchor.x << '/' << placement.anchor.y
            << " leftDelta=" << helperReadback.bounds.left - anchorBounds.left
            << " rightDelta=" << helperReadback.bounds.right - anchorBounds.right
            << " topDelta=" << helperReadback.bounds.top - anchorBounds.bottom << '\n';
        require(helperSelected == 0 && helperReadback.entered && helperReadback.initialized && helperReadback.exited &&
            helperReadback.owned && helperReadback.windows == 1 && helperReadback.cancelled && placement.rightToLeft == mirrored &&
            (placement.flags & helperBase) == helperBase && ((placement.flags & TPM_LAYOUTRTL) != 0) == mirrored &&
            helperReadback.bounds.top == anchorBounds.bottom &&
            (mirrored ? helperReadback.bounds.right == anchorBounds.right : helperReadback.bounds.left == anchorBounds.left),
            "Production native dropdown did not align its exact physical logical-leading edge");
        succeeded(desktop->verifyIsolation(), "Production native dropdown changed the input desktop");
        ShowWindow(owner, SW_HIDE);
    }
    SetActiveWindow(previousActive);
    bool unchanged = false, inputVisible = true;
    succeeded(desktop->verifyIsolation(&unchanged), "Native menu matrix changed input desktop");
    succeeded(desktop->visibleWindowsOnInputDesktop(inputVisible), "Native menu matrix input-window readback failed");
    require(unchanged && !inputVisible && !IsWindowVisible(ltr) && !IsWindowVisible(rtl), "Native menu matrix leaked presentation or input state");
}
void nativeEditDirectionContract() {
    OwnedWindows windows;
    const auto ltr = windows.add(0, WS_POPUP, nullptr, 180, 180, 640, 360);
    const auto rtl = windows.add(WS_EX_LAYOUTRTL, WS_POPUP, nullptr, 180, 180, 640, 360);
    const HWND previousActive = GetActiveWindow();
    struct RestorePresentation {
        HWND previous;
        std::array<HWND, 2> owners;
        ~RestorePresentation() { for (const auto owner : owners) ShowWindow(owner, SW_HIDE); SetActiveWindow(previous); }
    } restorePresentation{previousActive, {ltr, rtl}};
    const auto desktop = explorer::PrivateDesktop::current();
    const auto originalLanguage = GetThreadUILanguage();
    const auto originalLanguages = independentLanguages();
    const std::wstring text = L"ABC - \u05d0\u05d1\u05d2 - XYZ";
    constexpr LONG_PTR directionMask = WS_EX_LAYOUTRTL | WS_EX_RIGHT | WS_EX_RTLREADING | WS_EX_LEFTSCROLLBAR;
    constexpr LONG_PTR nativeEditRtl = WS_EX_RIGHT | WS_EX_RTLREADING | WS_EX_LEFTSCROLLBAR;
    std::array<bool, 2> nativePositionContracts{};
    for (const auto& [owner, mirrored] : std::array<std::pair<HWND, bool>, 2>{{{ltr, false}, {rtl, true}}}) {
        struct EditOwner {
            HWND value;
            ~EditOwner() { if (value) DestroyWindow(value); }
        } edit{CreateWindowExW(0, L"EDIT", text.c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            80, 60, 360, 40, owner, nullptr, windows.instance, nullptr)};
        require(edit.value != nullptr, "Create an independent native EDIT without extra direction flags");
        SendMessageW(edit.value, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        SendMessageW(edit.value, EM_SETSEL, 0, 0);
        ShowWindow(owner, SW_SHOWNOACTIVATE); SetActiveWindow(owner); UpdateWindow(owner);
        require(GetActiveWindow() == owner && SUCCEEDED(desktop->verifyIsolation()), "Native EDIT must remain on its private owner");
        wchar_t type[64]{}; GetClassNameW(edit.value, type, static_cast<int>(std::size(type)));
        const auto style = GetWindowLongPtrW(edit.value, GWL_EXSTYLE);
        const auto classStyle = GetClassLongPtrW(edit.value, GCL_STYLE);
        bool editMirrored = true;
        succeeded(explorer::windowUiDirection(edit.value, &editMirrored), "Read actual native EDIT coordinate layout");
        const auto dc = GetDC(edit.value); require(dc != nullptr, "Borrow actual native EDIT DC");
        const auto layout = GetLayout(dc); const auto mapMode = GetMapMode(dc); ReleaseDC(edit.value, dc);
        require(_wcsicmp(type, L"Edit") == 0 && !editMirrored && (classStyle & CS_PARENTDC) && layout == 0u && mapMode == MM_TEXT &&
            (style & directionMask) == (mirrored ? nativeEditRtl : 0),
            "Native EDIT did not preserve its converted reading styles and unmirrored parent-DC contract");
        auto characterPosition = [&](size_t index, POINT& client, POINT& screen) {
            const auto position = SendMessageW(edit.value, EM_POSFROMCHAR, index, 0);
            require(position != -1, "Read actual native EDIT character position");
            client = POINT{static_cast<short>(LOWORD(position)), static_cast<short>(HIWORD(position))};
            succeeded(explorer::mapUiPoint(edit.value, nullptr, client, &screen), "Map actual native EDIT character position to screen");
        };
        POINT first{}, next{}, last{}, hebrew{}, hebrewNext{};
        POINT firstClient{}, nextClient{}, lastClient{}, hebrewClient{}, hebrewNextClient{};
        characterPosition(0, firstClient, first); characterPosition(1, nextClient, next);
        characterPosition(text.find(L"XYZ"), lastClient, last);
        characterPosition(text.find(L'\u05d0'), hebrewClient, hebrew);
        characterPosition(text.find(L'\u05d0') + 1, hebrewNextClient, hebrewNext);
        std::wstring readText(text.size() + 1, L'\0');
        const auto copied = GetWindowTextW(edit.value, readText.data(), static_cast<int>(readText.size()));
        readText.resize(static_cast<size_t>(copied));
        DWORD selectionStart = 0, selectionEnd = 0;
        SendMessageW(edit.value, EM_GETSEL, reinterpret_cast<WPARAM>(&selectionStart), reinterpret_cast<LPARAM>(&selectionEnd));
        std::cout << "native-edit ownerRTL=" << mirrored << " exStyle=" << style << " classStyle=" << classStyle
            << " dcLayout=" << layout << " mapMode=" << mapMode << " copiedUnits=" << copied
            << " expectedUnits=" << text.size() << " textEqual=" << (readText == text)
            << " focus=" << reinterpret_cast<UINT_PTR>(GetFocus()) << " edit=" << reinterpret_cast<UINT_PTR>(edit.value)
            << " selection=" << selectionStart << ',' << selectionEnd
            << " firstClient=" << firstClient.x << ',' << firstClient.y << " firstScreen=" << first.x << ',' << first.y
            << " nextClient=" << nextClient.x << ',' << nextClient.y << " nextScreen=" << next.x << ',' << next.y
            << " hebrewClient=" << hebrewClient.x << ',' << hebrewClient.y << " hebrewScreen=" << hebrew.x << ',' << hebrew.y
            << " hebrewNextClient=" << hebrewNextClient.x << ',' << hebrewNextClient.y << " hebrewNextScreen=" << hebrewNext.x << ',' << hebrewNext.y
            << " lastClient=" << lastClient.x << ',' << lastClient.y << " lastScreen=" << last.x << ',' << last.y
            << " LatinDelta=" << next.x - first.x << " HebrewDelta=" << hebrewNext.x - hebrew.x
            << " paragraphDelta=" << first.x - last.x << std::endl;
        // Standard EDIT reports character positions, not an unconditional glyph-order
        // contract. This native LTR class reports logical advances for this literal;
        // the converted RTL class reports its RTL run and paragraph coordinates.
        // Evaluate both only after recording both owners so one failure cannot hide
        // the other native class's actual positions.
        const bool nativeRtlReading = (style & WS_EX_RTLREADING) != 0;
        const bool sameCoordinateAdvances = next.x - first.x == nextClient.x - firstClient.x &&
            hebrewNext.x - hebrew.x == hebrewNextClient.x - hebrewClient.x &&
            first.x - last.x == firstClient.x - lastClient.x &&
            first.y == next.y && first.y == hebrew.y && first.y == hebrewNext.y && first.y == last.y;
        nativePositionContracts[mirrored ? 1u : 0u] = readText == text && copied == static_cast<int>(text.size()) &&
            selectionStart == 0u && selectionEnd == 0u && sameCoordinateAdvances && first.x < next.x &&
            (nativeRtlReading ? hebrew.x > hebrewNext.x : hebrew.x < hebrewNext.x) &&
            (nativeRtlReading ? first.x > last.x : first.x < last.x);
        ShowWindow(owner, SW_HIDE);
    }
    SetActiveWindow(previousActive);
    bool unchanged = false, inputVisible = true;
    succeeded(desktop->verifyIsolation(&unchanged), "Native EDIT checks changed input desktop");
    succeeded(desktop->visibleWindowsOnInputDesktop(inputVisible), "Native EDIT input-window readback failed");
    require(unchanged && !inputVisible && !IsWindowVisible(ltr) && !IsWindowVisible(rtl) &&
        GetThreadUILanguage() == originalLanguage && independentLanguages() == originalLanguages,
        "Native EDIT controls changed presentation, input or UI language settings");
    require(nativePositionContracts[0], "Native LTR EDIT changed literal, selection, logical character advances or paragraph coordinates");
    require(nativePositionContracts[1], "Native RTL EDIT changed literal, selection, RTL run or paragraph coordinates");
}
} // namespace

int runUiDirectionTests() {
    int failures = 0;
    for (const auto& [name, test] : std::array<std::pair<const char*, void(*)()>, 5>{{
        {"native UI language reading policy", nativeLocalePolicy}, {"owned mirrored windows and native mappings", nativeWindowsAndMappings},
        {"owner popup placement and unchanged invalid outputs", nativeMenuPlacementAndFailures},
        {"actual private LTR/RTL menu alignment matrix", nativePopupAlignmentMatrix},
        {"independent native EDIT reading and DC contract", nativeEditDirectionContract}}}) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    return failures;
}
