#include "explorer/app.hpp"
#include "explorer/saved_search.hpp"
#include "explorer/search.hpp"
#include "explorer/chrome.hpp"
#include "explorer/theme.hpp"
#include "explorer/ribbon_commands.hpp"
#include "explorer/ui_strings.hpp"
#include "explorer/preview_diagnostics.hpp"
#include "explorer/headless_window_isolation.hpp"
#include <propkey.h>
#include <shlwapi.h>
#include <shlguid.h>
#include <propvarutil.h>
#include <propsys.h>
#include <aclapi.h>
#include <structuredquery.h>
#include <commctrl.h>
#include <oleacc.h>
#include <UIRibbonPropertyHelpers.h>
#include <uiautomation.h>
#include <wrl/implements.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <thread>
#include <tuple>

namespace explorer {
namespace {
bool visibleWindowObserved = false;
// This test oracle reads the public current-view HWND and its independently
// owned parent/frame geometry. It never uses the product's cached layout slot.
struct NativePaneGeometry {
    HRESULT read = E_PENDING, viewRead = E_PENDING, serviceRead = E_PENDING;
    HRESULT statusRead = E_PENDING, accessibleRead = E_PENDING, roleRead = E_PENDING;
    HRESULT locationRead = E_PENDING, boundsRead = E_PENDING, ownerRead = E_PENDING;
    HRESULT childCountRead = E_PENDING, childrenRead = E_PENDING, childQueryRead = E_PENDING, finalViewRead = E_PENDING;
    HRESULT finalBoundsRead = E_PENDING, directionRead = E_PENDING, finalFooterRead = E_PENDING;
    HRESULT paneHitRead = E_PENDING, gripHitRead = E_PENDING;
    HRESULT rootStyleRead = E_PENDING, paneStyleRead = E_PENDING, gripStyleRead = E_PENDING;
    HWND view = nullptr, parent = nullptr, frame = nullptr, footerWindow = nullptr, statusWindow = nullptr;
    HWND expectedGrip = nullptr, paneHit = nullptr, gripHit = nullptr;
    DWORD paneHitError = ERROR_SUCCESS, gripHitError = ERROR_SUCCESS;
    DWORD rootStyleError = ERROR_SUCCESS, paneStyleError = ERROR_SUCCESS, gripStyleError = ERROR_SUCCESS;
    LONG_PTR rootStyle = 0, paneStyle = 0, gripStyle = 0;
    BOOL rootVisible = FALSE, paneVisible = FALSE, gripVisible = FALSE;
    RECT rootClient{}, parentClient{}, frameClient{}, content{}, pane{}, grip{}, gripWindow{}, footer{};
    unsigned nodes = 0, candidates = 0;
    bool complete = true, found = false, footerFull = false, partition = false, inputReachable = false;
    bool paneOwned = false, gripOwned = false, visibilityConsistent = false;
};
bool paneGeometryOwned(HWND window, HWND root) noexcept {
    DWORD process = 0;
    return window && IsWindow(window) && GetWindowThreadProcessId(window, &process) == GetCurrentThreadId() &&
        process == GetCurrentProcessId() && (window == root || IsChild(root, window));
}
HRESULT paneGeometryError() noexcept {
    const auto error = GetLastError();
    return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
}
bool paneGeometryPositive(const RECT& value) noexcept {
    return value.right > value.left && value.bottom > value.top;
}
bool paneGeometryDisjoint(const RECT& first, const RECT& second) noexcept {
    RECT intersection{}; return !IntersectRect(&intersection, &first, &second);
}
NativePaneGeometry readNativePaneGeometry(IShellView* retainedView, HWND root, HWND pane,
    const RECT& logicalGrip, bool preview, ULONGLONG deadline, const std::function<bool()>& current, HWND gripController = nullptr) {
    NativePaneGeometry result;
    result.expectedGrip = gripController;
    const auto desktop = PrivateDesktop::current();
    if (!desktop || desktop->verifyIsolation() != S_OK) { result.read = E_ACCESSDENIED; return result; }
    const auto live = [&] { return GetTickCount64() < deadline && PrivateDesktop::current() == desktop && current() && paneGeometryOwned(root, root); };
    const auto bounds = [&](HWND window, bool client, RECT& value) -> HRESULT {
        if (!live() || !paneGeometryOwned(window, root)) return E_ABORT;
        SetLastError(ERROR_SUCCESS);
        const auto native = client ? GetClientRect(window, &value) : GetWindowRect(window, &value);
        if (!native) return paneGeometryError();
        const auto mapped = client ? mapUiRect(window, nullptr, value, &value) : S_OK;
        return mapped == S_OK && !live() ? E_ABORT : mapped;
    };
    if (!retainedView || !live()) { result.read = E_ABORT; return result; }
    result.viewRead = retainedView->GetWindow(&result.view);
    if (result.viewRead != S_OK || !live() || !paneGeometryOwned(result.view, root) || result.view == root) {
        result.read = result.viewRead == S_OK ? E_ABORT : result.viewRead; return result;
    }
    result.parent = GetAncestor(result.view, GA_PARENT); result.frame = result.view;
    unsigned depth = 0;
    while (GetAncestor(result.frame, GA_PARENT) != root) {
        if (++depth > 16) { result.read = HRESULT_FROM_WIN32(ERROR_MORE_DATA); return result; }
        result.frame = GetAncestor(result.frame, GA_PARENT);
        if (!paneGeometryOwned(result.frame, root) || result.frame == root) { result.read = E_ACCESSDENIED; return result; }
    }
    if (!paneGeometryOwned(result.parent, root) || result.frame == result.view ||
        !(result.parent == result.frame || IsChild(result.frame, result.parent))) { result.read = E_ACCESSDENIED; return result; }
    result.boundsRead = bounds(root, true, result.rootClient);
    if (result.boundsRead == S_OK) result.boundsRead = bounds(result.parent, true, result.parentClient);
    if (result.boundsRead == S_OK) result.boundsRead = bounds(result.frame, true, result.frameClient);
    if (result.boundsRead == S_OK) result.boundsRead = bounds(result.view, false, result.content);
    if (result.boundsRead == S_OK && preview) result.boundsRead = bounds(pane, false, result.pane);
    if (result.boundsRead == S_OK && preview) result.boundsRead = mapUiRect(root, nullptr, logicalGrip, &result.grip);
    if (result.boundsRead != S_OK || !live()) { result.read = result.boundsRead == S_OK ? E_ABORT : result.boundsRead; return result; }
    bool rtl = false; result.directionRead = windowUiDirection(root, &rtl);
    const auto dpi = GetDpiForWindow(root);
    if (result.directionRead != S_OK || !dpi || !live()) { result.read = E_ABORT; return result; }
    ComPtr<IShellBrowser> browser;
    ComPtr<IAccessible> retainedFooter;
    LONG retainedFooterChild = CHILDID_SELF;
    result.serviceRead = IUnknown_QueryService(retainedView, SID_STopLevelBrowser, IID_PPV_ARGS(&browser));
    if (!live()) { result.read = E_ABORT; return result; }
    if (result.serviceRead == S_OK && browser) {
        HWND status = nullptr; result.statusRead = browser->GetControlWindow(FCW_STATUS, &status);
        result.statusWindow = status; // Preserve the raw optional control output independently of the MSAA footer.
        if (!live()) { result.read = E_ABORT; return result; }
        if (result.statusRead == S_OK && paneGeometryOwned(status, root) &&
            (status == result.frame || IsChild(result.frame, status))) {
            result.locationRead = bounds(status, false, result.footer);
            if (result.locationRead == S_OK) { result.found = true; result.footerWindow = status; }
        }
    }
    // FCW_STATUS is legitimately NULL on this native frame. Discover only its
    // public STATUSBAR role, with no item/tree/text/name enumeration.
    if (!result.found) {
        ComPtr<IAccessible> accessible;
        result.accessibleRead = AccessibleObjectFromWindow(result.frame, static_cast<DWORD>(OBJID_CLIENT), IID_PPV_ARGS(&accessible));
        if (!live()) { result.read = E_ABORT; return result; }
        std::function<void(IAccessible*, const VARIANT&, unsigned)> walk;
        walk = [&](IAccessible* object, const VARIANT& child, unsigned level) {
            if (!result.complete) return;
            if (!live()) { result.complete = false; result.read = E_ABORT; return; }
            if (!object || level > 12 || result.nodes >= 256) { result.complete = false; result.read = HRESULT_FROM_WIN32(ERROR_MORE_DATA); return; }
            ++result.nodes;
            HWND owner = nullptr; result.ownerRead = WindowFromAccessibleObject(object, &owner);
            if (!live() || result.ownerRead != S_OK || !paneGeometryOwned(owner, root) ||
                !(owner == result.frame || IsChild(result.frame, owner))) {
                result.complete = false; result.read = FAILED(result.ownerRead) ? result.ownerRead : E_ACCESSDENIED; return;
            }
            VARIANT role{};
            struct Role { VARIANT& value; ~Role() { VariantClear(&value); } } releaseRole{role};
            result.roleRead = object->get_accRole(child, &role);
            if (!live() || result.roleRead != S_OK || role.vt != VT_I4) {
                result.complete = false; result.read = FAILED(result.roleRead) ? result.roleRead : E_UNEXPECTED; return;
            }
            if (role.lVal == ROLE_SYSTEM_STATUSBAR) {
                long x = 0, y = 0, width = 0, height = 0;
                result.locationRead = object->accLocation(&x, &y, &width, &height, child); ++result.candidates;
                const auto right = static_cast<LONGLONG>(x) + width, bottom = static_cast<LONGLONG>(y) + height;
                if (!live() || result.locationRead != S_OK || width <= 0 || height <= 0 ||
                    right > std::numeric_limits<LONG>::max() || bottom > std::numeric_limits<LONG>::max()) {
                    result.complete = false; result.read = FAILED(result.locationRead) ? result.locationRead : E_UNEXPECTED; return;
                }
                const RECT actual{x, y, static_cast<LONG>(right), static_cast<LONG>(bottom)};
                if (result.found && !EqualRect(&actual, &result.footer)) { result.complete = false; result.read = E_UNEXPECTED; return; }
                if (child.vt != VT_I4) { result.complete = false; result.read = E_UNEXPECTED; return; }
                result.found = true; result.footer = actual; result.footerWindow = owner;
                retainedFooter = object; retainedFooterChild = child.lVal; return;
            }
            if (role.lVal == ROLE_SYSTEM_LIST || role.lVal == ROLE_SYSTEM_LISTITEM || role.lVal == ROLE_SYSTEM_OUTLINE ||
                role.lVal == ROLE_SYSTEM_OUTLINEITEM || role.lVal == ROLE_SYSTEM_CELL || child.vt != VT_I4 || child.lVal != CHILDID_SELF) return;
            long count = 0; result.childCountRead = object->get_accChildCount(&count);
            if (!live() || result.childCountRead != S_OK || count < 0 || count > 128) {
                result.complete = false; result.read = FAILED(result.childCountRead) ? result.childCountRead : E_UNEXPECTED; return;
            }
            if (!count) return;
            std::array<VARIANT, 128> children{};
            struct Children { std::array<VARIANT,128>& values; ~Children() { for (auto& value : values) VariantClear(&value); } } releaseChildren{children};
            long received = 0; result.childrenRead = AccessibleChildren(object, 0, count, children.data(), &received);
            if (!live() || FAILED(result.childrenRead) || received != count) {
                result.complete = false; result.read = FAILED(result.childrenRead) ? result.childrenRead : E_UNEXPECTED; return;
            }
            for (long index = 0; index < received && result.complete; ++index) {
                const auto& value = children[static_cast<size_t>(index)];
                if (value.vt == VT_DISPATCH && value.pdispVal) {
                    ComPtr<IAccessible> descendant; result.childQueryRead = value.pdispVal->QueryInterface(IID_PPV_ARGS(&descendant));
                    if (!live() || result.childQueryRead != S_OK || !descendant) {
                        result.complete = false; result.read = FAILED(result.childQueryRead) ? result.childQueryRead : E_NOINTERFACE; return;
                    }
                    VARIANT self{}; self.vt = VT_I4; self.lVal = CHILDID_SELF; walk(descendant.Get(), self, level + 1);
                } else if (value.vt == VT_I4) walk(object, value, level + 1);
            }
        };
        if (result.accessibleRead == S_OK && accessible) { VARIANT self{}; self.vt = VT_I4; self.lVal = CHILDID_SELF; walk(accessible.Get(), self, 0); }
        else { result.complete = false; result.read = result.accessibleRead == S_OK ? E_NOINTERFACE : result.accessibleRead; }
    }
    browser.Reset();
    if (!result.complete || !result.found) {
        if (result.read == E_PENDING) result.read = live() ? HRESULT_FROM_WIN32(ERROR_NOT_FOUND) : E_ABORT;
        return result;
    }
    HWND finalView = nullptr;
    if (live()) result.finalViewRead = retainedView->GetWindow(&finalView);
    if (!live() || GetAncestor(result.view, GA_PARENT) != result.parent || !paneGeometryOwned(result.parent, root) ||
        !paneGeometryOwned(result.frame, root) || GetAncestor(result.frame, GA_PARENT) != root ||
        !(result.parent == result.frame || IsChild(result.frame, result.parent)) || result.finalViewRead != S_OK ||
        finalView != result.view || desktop->verifyIsolation() != S_OK) {
        result.read = FAILED(result.finalViewRead) ? result.finalViewRead : E_ABORT; return result;
    }
    // The native STATUSBAR may expose a region in its frame's accessibility
    // object rather than a dedicated HWND. Keep its exact public object/child.
    RECT finalFooter{};
    if (retainedFooter) {
        VARIANT child{}; child.vt = VT_I4; child.lVal = retainedFooterChild;
        long x = 0, y = 0, width = 0, height = 0;
        result.finalFooterRead = retainedFooter->accLocation(&x, &y, &width, &height, child);
        const auto right = static_cast<LONGLONG>(x)+width, bottom = static_cast<LONGLONG>(y)+height;
        if (result.finalFooterRead == S_OK && width > 0 && height > 0 &&
            right <= std::numeric_limits<LONG>::max() && bottom <= std::numeric_limits<LONG>::max())
            finalFooter = {x,y,static_cast<LONG>(right),static_cast<LONG>(bottom)};
        else if (result.finalFooterRead == S_OK) result.finalFooterRead = E_UNEXPECTED;
        retainedFooter.Reset();
    } else if (result.found) result.finalFooterRead = bounds(result.footerWindow, false, finalFooter);
    if (result.finalFooterRead != S_OK || !EqualRect(&finalFooter, &result.footer) || !live()) {
        result.read = FAILED(result.finalFooterRead) ? result.finalFooterRead : E_ABORT; return result;
    }
    // Service/MSAA calls and Releases may reenter the creator. Verify the exact
    // geometry again after the last native call, not merely its HWND identity.
    RECT finalRoot{}, finalParent{}, finalFrame{}, finalContent{}, finalPane{}, finalGrip{};
    result.finalBoundsRead = bounds(root, true, finalRoot);
    if (result.finalBoundsRead == S_OK) result.finalBoundsRead = bounds(result.parent, true, finalParent);
    if (result.finalBoundsRead == S_OK) result.finalBoundsRead = bounds(result.frame, true, finalFrame);
    if (result.finalBoundsRead == S_OK) result.finalBoundsRead = bounds(result.view, false, finalContent);
    if (result.finalBoundsRead == S_OK && preview) result.finalBoundsRead = bounds(pane, false, finalPane);
    if (result.finalBoundsRead == S_OK && preview) result.finalBoundsRead = mapUiRect(root, nullptr, logicalGrip, &finalGrip);
    bool finalRtl = !rtl; const auto finalDirectionRead = windowUiDirection(root, &finalRtl);
    if (result.finalBoundsRead != S_OK || finalDirectionRead != S_OK || finalRtl != rtl || GetDpiForWindow(root) != dpi ||
        !EqualRect(&finalRoot, &result.rootClient) || !EqualRect(&finalParent, &result.parentClient) ||
        !EqualRect(&finalFrame, &result.frameClient) || !EqualRect(&finalContent, &result.content) ||
        (preview && (!EqualRect(&finalPane, &result.pane) || !EqualRect(&finalGrip, &result.grip))) || !live()) {
        result.read = FAILED(result.finalBoundsRead) ? result.finalBoundsRead : E_ABORT; return result;
    }
    const auto visibility = [&](HWND window, LONG_PTR& style, BOOL& visible, DWORD& error) -> HRESULT {
        if (!live() || !paneGeometryOwned(window, root)) return E_ABORT;
        SetLastError(ERROR_SUCCESS);
        style = GetWindowLongPtrW(window, GWL_STYLE); error = GetLastError();
        if (!style && error != ERROR_SUCCESS) return HRESULT_FROM_WIN32(error);
        visible = IsWindowVisible(window);
        return live() ? S_OK : E_ABORT;
    };
    result.rootStyleRead = visibility(root, result.rootStyle, result.rootVisible, result.rootStyleError);
    result.paneOwned = pane && paneGeometryOwned(pane, root) && GetAncestor(pane, GA_PARENT) == root;
    result.gripOwned = gripController && paneGeometryOwned(gripController, root) && GetAncestor(gripController, GA_PARENT) == root;
    result.paneStyleRead = pane ? visibility(pane, result.paneStyle, result.paneVisible, result.paneStyleError) : S_FALSE;
    result.gripStyleRead = gripController ? visibility(gripController, result.gripStyle, result.gripVisible, result.gripStyleError) : S_FALSE;
    // IsWindowVisible includes the parent's state. A shown direct child beneath
    // the original hidden headless root must retain its own WS_VISIBLE bit;
    // presented roots still require actual effective child visibility.
    // https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-iswindowvisible
    const bool paneShown = result.paneOwned && result.paneStyleRead == S_OK && (result.paneStyle & WS_VISIBLE) &&
        (result.paneVisible != FALSE) == (result.rootVisible != FALSE);
    const bool gripShown = result.gripOwned && result.gripStyleRead == S_OK && (result.gripStyle & WS_VISIBLE) &&
        (result.gripVisible != FALSE) == (result.rootVisible != FALSE);
    result.visibilityConsistent = result.rootStyleRead == S_OK && (preview ? paneShown && (!gripController || gripShown) :
        (!pane || (result.paneOwned && result.paneStyleRead == S_OK && !(result.paneStyle & WS_VISIBLE) && !result.paneVisible)) &&
        (!gripController || (result.gripOwned && result.gripStyleRead == S_OK && !(result.gripStyle & WS_VISIBLE) && !result.gripVisible)));
    result.footerFull = result.found && result.complete && paneGeometryPositive(result.footer) &&
        result.footer.left == result.frameClient.left && result.footer.right == result.frameClient.right &&
        result.footer.bottom == result.frameClient.bottom && result.footer.top >= result.parentClient.bottom;
    if (!preview) result.partition = EqualRect(&result.content, &result.parentClient) &&
        result.visibilityConsistent && IsRectEmpty(&logicalGrip);
    else result.partition = paneGeometryPositive(result.content) && paneGeometryPositive(result.pane) && paneGeometryPositive(result.grip) &&
        result.content.top == result.parentClient.top && result.content.bottom == result.parentClient.bottom &&
        result.pane.top == result.parentClient.top && result.pane.bottom == result.parentClient.bottom &&
        result.grip.top == result.parentClient.top && result.grip.bottom == result.parentClient.bottom &&
        (rtl ? result.pane.left == result.parentClient.left && result.pane.right == result.grip.left &&
            result.grip.right == result.content.left && result.content.right == result.parentClient.right :
            result.content.left == result.parentClient.left && result.content.right == result.grip.left &&
            result.grip.right == result.pane.left && result.pane.right == result.parentClient.right) &&
        paneGeometryDisjoint(result.footer, result.content) && paneGeometryDisjoint(result.footer, result.pane) && paneGeometryDisjoint(result.footer, result.grip);
    if (preview) {
        const auto hit = [&](const RECT& rectangle, HWND& window, DWORD& error) -> HRESULT {
            if (!live() || !paneGeometryPositive(rectangle)) return E_ABORT;
            const POINT physical{rectangle.left+(rectangle.right-rectangle.left)/2, rectangle.top+(rectangle.bottom-rectangle.top)/2};
            POINT local{}; const auto mapped = mapUiPoint(nullptr, root, physical, &local);
            if (mapped != S_OK) return mapped;
            SetLastError(ERROR_SUCCESS);
            window = ChildWindowFromPointEx(root, local, CWP_SKIPINVISIBLE); error = GetLastError();
            return window && live() ? S_OK : window ? E_ABORT : HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
        };
        result.paneHitRead = hit(result.pane, result.paneHit, result.paneHitError);
        result.gripHitRead = hit(result.grip, result.gripHit, result.gripHitError);
        const bool exactGrip = gripController ? result.gripOwned &&
            bounds(gripController, false, result.gripWindow) == S_OK && EqualRect(&result.gripWindow, &result.grip) :
            result.gripHit == root;
        result.inputReachable = result.paneHitRead == S_OK && result.gripHitRead == S_OK && result.paneHit == pane &&
            result.gripHit == (gripController ? gripController : root) && exactGrip && result.visibilityConsistent && live();
        result.partition = result.partition && result.inputReachable;
    }
    LONG_PTR finalRootStyle = 0, finalPaneStyle = 0, finalGripStyle = 0;
    BOOL finalRootVisible = FALSE, finalPaneVisible = FALSE, finalGripVisible = FALSE;
    DWORD finalStyleError = ERROR_SUCCESS;
    const auto finalRootStyleRead = visibility(root, finalRootStyle, finalRootVisible, finalStyleError);
    const auto finalPaneStyleRead = pane ? visibility(pane, finalPaneStyle, finalPaneVisible, finalStyleError) : S_FALSE;
    const auto finalGripStyleRead = gripController ? visibility(gripController, finalGripStyle, finalGripVisible, finalStyleError) : S_FALSE;
    if (finalRootStyleRead != result.rootStyleRead || finalRootStyleRead != S_OK ||
        finalPaneStyleRead != result.paneStyleRead || finalGripStyleRead != result.gripStyleRead ||
        finalRootStyle != result.rootStyle || finalPaneStyle != result.paneStyle || finalGripStyle != result.gripStyle ||
        finalRootVisible != result.rootVisible || finalPaneVisible != result.paneVisible || finalGripVisible != result.gripVisible ||
        (pane && (!paneGeometryOwned(pane, root) || GetAncestor(pane, GA_PARENT) != root)) ||
        (gripController && (!paneGeometryOwned(gripController, root) || GetAncestor(gripController, GA_PARENT) != root))) {
        result.read = E_ABORT; result.partition = false; result.inputReachable = false; return result;
    }
    if (!live() || GetAncestor(result.view, GA_PARENT) != result.parent || GetAncestor(result.frame, GA_PARENT) != root ||
        !(result.parent == result.frame || IsChild(result.frame, result.parent)) ||
        !paneGeometryOwned(result.view, root) || !paneGeometryOwned(result.parent, root) || !paneGeometryOwned(result.frame, root)) result.read = E_ABORT;
    else if (result.complete) result.read = result.found ? S_OK : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    return result;
}
std::wstring paneGeometryFacts(const NativePaneGeometry& value) {
    const auto rect = [](const RECT& r) { return std::to_wstring(r.left)+L","+std::to_wstring(r.top)+L","+std::to_wstring(r.right)+L","+std::to_wstring(r.bottom); };
    return L"raw geometry view/bounds/service/status/MSAA/role/location/final="+hresultMessage(value.viewRead)+L"/"+
        hresultMessage(value.boundsRead)+L"/"+hresultMessage(value.serviceRead)+L"/"+hresultMessage(value.statusRead)+L"/"+
        hresultMessage(value.accessibleRead)+L"/"+hresultMessage(value.roleRead)+L"/"+hresultMessage(value.locationRead)+L"/"+hresultMessage(value.read)+
        L"; raw MSAA owner/count/children/childQI/finalView="+hresultMessage(value.ownerRead)+L"/"+
        hresultMessage(value.childCountRead)+L"/"+hresultMessage(value.childrenRead)+L"/"+hresultMessage(value.childQueryRead)+L"/"+hresultMessage(value.finalViewRead)+
        L"; final geometry/direction/footer="+hresultMessage(value.finalBoundsRead)+L"/"+hresultMessage(value.directionRead)+L"/"+hresultMessage(value.finalFooterRead)+
        L"; actual pane/grip hit HRESULT="+hresultMessage(value.paneHitRead)+L"/"+hresultMessage(value.gripHitRead)+
        L"; pane/grip/expectedGrip HWND="+std::to_wstring(reinterpret_cast<UINT_PTR>(value.paneHit))+L"/"+
        std::to_wstring(reinterpret_cast<UINT_PTR>(value.gripHit))+L"/"+std::to_wstring(reinterpret_cast<UINT_PTR>(value.expectedGrip))+
        L"; hit native errors/inputReachable="+std::to_wstring(value.paneHitError)+L"/"+std::to_wstring(value.gripHitError)+L"/"+std::to_wstring(value.inputReachable)+
        L"; root/pane/grip style HRESULT="+hresultMessage(value.rootStyleRead)+L"/"+hresultMessage(value.paneStyleRead)+L"/"+hresultMessage(value.gripStyleRead)+
        L"; style native errors="+std::to_wstring(value.rootStyleError)+L"/"+std::to_wstring(value.paneStyleError)+L"/"+std::to_wstring(value.gripStyleError)+
        L"; raw root/pane/grip style="+std::to_wstring(static_cast<DWORD>(value.rootStyle))+L"/"+
        std::to_wstring(static_cast<DWORD>(value.paneStyle))+L"/"+std::to_wstring(static_cast<DWORD>(value.gripStyle))+
        L"; effective root/pane/grip visible="+std::to_wstring(value.rootVisible)+L"/"+std::to_wstring(value.paneVisible)+L"/"+std::to_wstring(value.gripVisible)+
        L"; pane/grip owned/visibilityConsistent="+std::to_wstring(value.paneOwned)+L"/"+std::to_wstring(value.gripOwned)+L"/"+
        std::to_wstring(value.visibilityConsistent)+L"; actual grip HWND rectangle="+rect(value.gripWindow)+
        L"; raw optional FCW_STATUS HWND="+std::to_wstring(reinterpret_cast<UINT_PTR>(value.statusWindow))+
        L"; exact public HWND view/parent/frame/footer="+std::to_wstring(reinterpret_cast<UINT_PTR>(value.view))+L"/"+
        std::to_wstring(reinterpret_cast<UINT_PTR>(value.parent))+L"/"+std::to_wstring(reinterpret_cast<UINT_PTR>(value.frame))+L"/"+
        std::to_wstring(reinterpret_cast<UINT_PTR>(value.footerWindow))+L"; nodes/candidates/complete/fullFooter/partition="+
        std::to_wstring(value.nodes)+L"/"+std::to_wstring(value.candidates)+L"/"+std::to_wstring(value.complete)+L"/"+
        std::to_wstring(value.footerFull)+L"/"+std::to_wstring(value.partition)+L"; physical rootClient/parent/frame/content/pane/grip/footer="+
        rect(value.rootClient)+L"/"+rect(value.parentClient)+L"/"+rect(value.frameClient)+L"/"+rect(value.content)+L"/"+rect(value.pane)+L"/"+rect(value.grip)+L"/"+rect(value.footer);
}
constexpr std::array<const wchar_t*, 8> ViewNames{
    L"Extra large icons", L"Large icons", L"Medium icons", L"Small icons",
    L"List", L"Details", L"Tiles", L"Content"};
bool isExternalSearch(PCIDLIST_ABSOLUTE pidl) {
    PWSTR raw = nullptr;
    if (!pidl || FAILED(SHGetNameFromIDList(pidl, SIGDN_DESKTOPABSOLUTEPARSING, &raw))) return false;
    const std::wstring name(raw); CoTaskMemFree(raw);
    if (_wcsnicmp(name.c_str(), L"search-ms:", 10) == 0) return true;
    if (name.size() < 10 || _wcsicmp(name.c_str() + name.size() - 10, L".search-ms") != 0) return false;
    ComPtr<IShellItem2> item;
    if (FAILED(SHCreateItemFromIDList(pidl, IID_PPV_ARGS(&item)))) return false;
    raw = nullptr;
    if (FAILED(item->GetString(PKEY_ItemType, &raw))) return false;
    const bool saved = raw && _wcsicmp(raw, L".search-ms") == 0;
    CoTaskMemFree(raw); return saved;
}
std::wstring textOf(HWND window) {
    const auto length = GetWindowTextLengthW(window);
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(window, value.data(), length + 1);
    value.resize(length);
    return value;
}
std::wstring itemName(IShellItem* item, SIGDN format) {
    PWSTR value = nullptr;
    if (!item || FAILED(item->GetDisplayName(format, &value))) return {};
    std::wstring result = value;
    CoTaskMemFree(value);
    return result;
}
bool pumpUntil(const std::function<bool()>& ready, DWORD timeoutMs) {
    struct DispatchEvent {
        HANDLE value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ~DispatchEvent() { if(value)CloseHandle(value); }
    } event;
    if(!event.value)return false;
    auto observeWindows = [] {
        if (const auto desktop = PrivateDesktop::current()) {
            bool visible = true;
            if (FAILED(desktop->visibleWindowsOnInputDesktop(visible)) || visible) visibleWindowObserved = true;
            return;
        }
        EnumWindows([](HWND hwnd, LPARAM) -> BOOL {
            DWORD process = 0; GetWindowThreadProcessId(hwnd, &process);
            if (process == GetCurrentProcessId() && IsWindowVisible(hwnd)) visibleWindowObserved = true;
            return TRUE;
        }, 0);
    };
    const auto end = GetTickCount64() + timeoutMs;
    do {
        observeWindows();
        MSG message;
        // Native providers can continuously post state-change work. Recheck
        // both the predicate and deadline after bounded batches rather than
        // requiring their queue to become completely empty.
        unsigned dispatched = 0;
        while (dispatched < 16 && GetTickCount64() < end &&
               PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) return false;
            TranslateMessage(&message);
            DispatchMessageW(&message);
            observeWindows();
            ++dispatched;
        }
        if (ready()) return true;
        const auto now=GetTickCount64();
        if(now>=end)break;
        // Native state providers marshal callbacks to this STA. A window-only
        // wait can leave those COM calls queued even when messages are pumped.
        // The owned unsignaled event bounds COM dispatch without a busy loop.
        DWORD signaled=0;
        const auto waited=CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS|COWAIT_DISPATCH_WINDOW_MESSAGES,
            static_cast<DWORD>(std::min<ULONGLONG>(5,end-now)),1,&event.value,&signaled);
        observeWindows();
        if(FAILED(waited)&&waited!=RPC_S_CALLPENDING)return false;
    } while (GetTickCount64() < end);
    return ready();
}
struct PrivatePresentation {
    const PrivateDesktop* desktop = PrivateDesktop::current();
    HWND window = nullptr;
    HWND previousActive = nullptr;
    bool ready = false;
    bool wasVisible = false;
    bool activated = false;
    explicit PrivatePresentation(HWND host, bool activate = false) : window(host), wasVisible(IsWindowVisible(host) != FALSE) {
        bool inputVisible = true;
        if (!desktop || FAILED(desktop->verifyIsolation()) ||
            FAILED(desktop->visibleWindowsOnInputDesktop(inputVisible)) || inputVisible) return;
        DWORD process = 0;
        if (GetWindowThreadProcessId(host, &process) != GetCurrentThreadId() || process != GetCurrentProcessId()) return;
        ShowWindow(window, SW_SHOWNOACTIVATE);
        if (activate) {
            previousActive = GetActiveWindow();
            SetActiveWindow(window); // Thread-local owned HWND on the never-switched desktop.
            activated = GetActiveWindow() == window;
        }
        UpdateWindow(window);
        const auto settle = GetTickCount64() + 250;
        pumpUntil([&] { return GetTickCount64() >= settle; }, 500);
        ready = IsWindowVisible(window) && (!activate || activated) && SUCCEEDED(desktop->verifyIsolation()) &&
            SUCCEEDED(desktop->visibleWindowsOnInputDesktop(inputVisible)) && !inputVisible;
    }
    ~PrivatePresentation() {
        if (activated) SetActiveWindow(previousActive);
        if (window && !wasVisible) ShowWindow(window, SW_HIDE);
    }
};

// Tracks only the flat menu supplied by this fixture on its already verified
// private owner. The native menu loop dispatches its owned timer; no input is
// injected and no command is invoked. The subclass/timer never outlive data.
struct PrivateMenuObservation {
    static constexpr UINT_PTR identity = 0x51d1;
    HWND owner = nullptr;
    HMENU menu = nullptr;
    bool ready = false, entered = false, initialized = false, exited = false;
    bool ownerConfirmed = false, cancelled = false;
    unsigned visibleMenus = 0;
    RECT bounds{};
    PrivateMenuObservation(HWND host, HMENU popup) : owner(host), menu(popup) {
        const auto desktop = PrivateDesktop::current(); DWORD process = 0;
        if (!desktop || FAILED(desktop->verifyIsolation()) ||
            GetWindowThreadProcessId(host, &process) != GetCurrentThreadId() || process != GetCurrentProcessId()) return;
        if (!SetWindowSubclass(owner, procedure, identity, reinterpret_cast<DWORD_PTR>(this))) return;
        ready = SetTimer(owner, identity, 150, nullptr) != 0;
        if (!ready) RemoveWindowSubclass(owner, procedure, identity);
    }
    ~PrivateMenuObservation() {
        if (ready) { KillTimer(owner, identity); RemoveWindowSubclass(owner, procedure, identity); }
    }
    static LRESULT CALLBACK procedure(HWND host, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR data) {
        auto& observation = *reinterpret_cast<PrivateMenuObservation*>(data);
        if (message == WM_ENTERMENULOOP) observation.entered = true;
        if (message == WM_INITMENUPOPUP && reinterpret_cast<HMENU>(wparam) == observation.menu) observation.initialized = true;
        if (message == WM_EXITMENULOOP) observation.exited = true;
        if (message == WM_TIMER && wparam == identity) {
            KillTimer(host, identity);
            GUITHREADINFO information{sizeof(information)};
            observation.ownerConfirmed = GetGUIThreadInfo(GetCurrentThreadId(), &information) && information.hwndMenuOwner == host;
            if (observation.ownerConfirmed) EnumThreadWindows(GetCurrentThreadId(), [](HWND candidate, LPARAM context) -> BOOL {
                auto& value = *reinterpret_cast<PrivateMenuObservation*>(context); DWORD process = 0;
                wchar_t type[64]{}; GetClassNameW(candidate, type, static_cast<int>(std::size(type)));
                if (wcscmp(type, L"#32768") != 0 || !IsWindowVisible(candidate) ||
                    GetWindowThreadProcessId(candidate, &process) != GetCurrentThreadId() || process != GetCurrentProcessId()) return TRUE;
                RECT actual{};
                if (GetWindowRect(candidate, &actual)) { value.bounds = actual; ++value.visibleMenus; }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&observation));
            // Cancel even when initialization/readback failed, so a failed
            // assertion cannot leave the private native menu loop running.
            observation.cancelled = EndMenu() != FALSE;
            return 0;
        }
        return DefSubclassProc(host, message, wparam, lparam);
    }
};
std::string jsonString(const std::wstring& value) {
    auto bytes = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), utf8.data(), bytes, nullptr, nullptr);
    std::string result = "\"";
    for (unsigned char c : utf8) {
        if (c == '"' || c == '\\') { result += '\\'; result += c; }
        else if (c < 0x20) { char escaped[7]; sprintf_s(escaped, "\\u%04x", c); result += escaped; }
        else result += c;
    }
    return result + '"';
}

// Read the native providers without invoking a control or changing focus.
// Control names and types are recorded in the test report for review.
struct AccessibleResult {
    bool passed = false;
    std::wstring detail;
    ComPtr<IUIAutomationElement> element;
    HRESULT budgetStatus = S_OK;
};
AccessibleResult accessibleElement(IUIAutomation* automation, IUIAutomationElement* scope,
                                   HWND window, const wchar_t* name, CONTROLTYPEID type,
                                   TreeScope treeScope = TreeScope_Descendants) {
    AccessibleResult result;
    if (!automation) { result.detail = L"UI Automation is unavailable"; return result; }
    HRESULT hr = S_OK;
    if (window) hr = automation->ElementFromHandle(window, &result.element);
    else if (scope) {
        VARIANT property{};
        ComPtr<IUIAutomationCondition> condition;
        if (name) {
            property.vt = VT_BSTR;
            property.bstrVal = SysAllocString(name);
            hr = property.bstrVal ? automation->CreatePropertyCondition(UIA_NamePropertyId, property, &condition) : E_OUTOFMEMORY;
            VariantClear(&property);
        } else {
            property.vt = VT_I4; property.lVal = type;
            hr = automation->CreatePropertyCondition(UIA_ControlTypePropertyId, property, &condition);
        }
        if (SUCCEEDED(hr)) hr = scope->FindFirst(treeScope, condition.Get(), &result.element);
    } else hr = E_INVALIDARG;
    if (FAILED(hr) || !result.element) {
        result.detail = L"Native accessibility element not found: " + hresultMessage(FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
        return result;
    }
    CONTROLTYPEID actualType = 0;
    BSTR actualName = nullptr;
    hr = result.element->get_CurrentControlType(&actualType);
    if (SUCCEEDED(hr)) hr = result.element->get_CurrentName(&actualName);
    const std::wstring observedName = actualName ? actualName : L"";
    SysFreeString(actualName);
    result.detail = L"Name=" + observedName + L"; ControlType=" + std::to_wstring(actualType) + L"; " + hresultMessage(hr);
    result.passed = SUCCEEDED(hr) && actualType == type && !observedName.empty() && (!name || observedName == name);
    return result;
}
HRESULT nativeFileIdentity(const std::filesystem::path& path, FILE_ID_INFO& identity) {
    const auto handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    const bool read = GetFileInformationByHandleEx(handle, FileIdInfo, &identity, sizeof(identity)) != FALSE;
    const auto result = read ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    CloseHandle(handle);
    return result;
}
struct PaneRendererWindows;
struct PaneObservation {
    HRESULT read = E_PENDING;
    ULONGLONG observationDeadline = 0;
    bool matched = false, privateWindows = false, privacyChecked = false, inputUnchanged = false;
    unsigned visibleElements = 0, handlerWindows = 0;
    RECT bounds{};
    std::wstring detail;
    std::shared_ptr<const PaneRendererWindows> rendererAdmission;
};

struct PreviewDesktopAccessDiagnostic {
    HRESULT created = E_PENDING, token = E_PENDING, lowToken = E_PENDING, finished = E_PENDING;
    HRESULT restored = E_PENDING, originalTokenPreserved = E_PENDING;
    DWORD originalRid = 0, lowRid = 0, policy = 0;
    unsigned tokenReadbacks = 0, reverts = 0;
    // Index 0 is READOBJECTS (0x01), index 1 adds CREATEWINDOW and
    // WRITEOBJECTS (0x83). The exact same masks/token test two newly created
    // sibling objects whose nonlabel security descriptors must match.
    std::array<HRESULT, 2> processDefault{E_PENDING, E_PENDING}, lowDefault{E_PENDING, E_PENDING};
    std::array<HRESULT, 2> processLowLabel{E_PENDING, E_PENDING}, lowLowLabel{E_PENDING, E_PENDING};
    PrivateDesktop::DiagnosticAtomicLabelReadback security;
    bool completedWithinBudget = false, discriminating = false;
    std::wstring detail() const {
        const auto pair = [](const auto& values) { return hresultMessage(values[0]) + L"/" + hresultMessage(values[1]); };
        return L"paired atomic-created desktop comparison init/token/low-token/finish/desktop-restore=" + hresultMessage(created) + L"/" +
            hresultMessage(token) + L"/" + hresultMessage(lowToken) + L"/" + hresultMessage(finished) + L"/" +
            hresultMessage(restored) + L"; process/low-token RID/policy=" +
            std::to_wstring(originalRid) + L"/" + std::to_wstring(lowRid) + L"/" + std::to_wstring(policy) +
            L"; effective low-token readbacks/reverts=" + std::to_wstring(tokenReadbacks) + L"/" + std::to_wstring(reverts) +
            L"; paired READ0x01/WRITE0x83 process-default/low-default/process-low-label/low-low-label=" +
            pair(processDefault) + L"; " + pair(lowDefault) + L"; " + pair(processLowLabel) + L"; " + pair(lowLowLabel) +
            L"; atomic guard/default-create/low-create/default-read/low-read/security-equivalence=" +
            hresultMessage(security.guard) + L"/" + hresultMessage(security.defaultCreated) + L"/" +
            hresultMessage(security.lowCreated) + L"/" + hresultMessage(security.defaultSecurity) + L"/" +
            hresultMessage(security.lowSecurity) + L"/" + hresultMessage(security.equivalentSecurity) +
            L"; exact owned current/noninheritable=" + std::to_wstring(security.exactOwnedCurrent) + L"/" +
            std::to_wstring(security.noninheritable) + L"; precreation apartment HRESULT/type/qualifier=" + hresultMessage(security.apartmentRead) + L"/" +
            std::to_wstring(security.apartmentType) + L"/" + std::to_wstring(security.apartmentQualifier) +
            L"; labels/RID/mask/flags default=" + std::to_wstring(security.defaultLabels) + L"/" + std::to_wstring(security.defaultRid) +
            L"/" + std::to_wstring(security.defaultMask) + L"/" + std::to_wstring(security.defaultFlags) +
            L"; low=" + std::to_wstring(security.lowLabels) + L"/" + std::to_wstring(security.lowRid) + L"/" +
            std::to_wstring(security.lowMask) + L"/" + std::to_wstring(security.lowFlags) +
            L"; native descriptor control default/low=" + std::to_wstring(security.defaultControl) + L"/" + std::to_wstring(security.lowControl) +
            L"; DACL/owner/group/nonlabel-control equal=" + std::to_wstring(security.daclEqual) + L"/" +
            std::to_wstring(security.ownerEqual) + L"/" + std::to_wstring(security.groupEqual) + L"/" + std::to_wstring(security.nonlabelControlEqual) +
            L"; original process token preserved=" + hresultMessage(originalTokenPreserved) +
            L"; paired controls completed within budget=" + std::to_wstring(completedWithinBudget) +
            L"; paired low admission differs=" + std::to_wstring(discriminating) +
            L"; same-object transition/surrogate token/render cause unproven; comparison SetWindow/DoPreview=0/0";
    }
};

// No caller COM initialization or HWND creation on this fresh worker. The original app desktop is never
// relabeled. This tests low-token admission to two new disposable siblings,
// not the surrogate's token, parenting, messaging, or actual pane rendering.
PreviewDesktopAccessDiagnostic previewDesktopAccessDiagnostic(const std::atomic_bool& cancelled, ULONGLONG deadline) {
    PreviewDesktopAccessDiagnostic result;
    const auto initialDesktop = GetThreadDesktop(GetCurrentThreadId());
    const auto budget = [&] { return !cancelled.load() && GetTickCount64() < deadline; };
    const auto failure = [] { const auto error = GetLastError(); return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE); };
    const auto stopUnsafe = [] {
        std::fprintf(stderr, "headless preview desktop/token restoration failed; no further activation\n"); std::fflush(stderr);
        if (!TerminateProcess(GetCurrentProcess(), 9)) std::_Exit(9);
        std::_Exit(9);
    };
    struct Token {
        HANDLE value = nullptr;
        ~Token() { if (value) CloseHandle(value); }
    } process, low;
    struct Identity {
        DWORD rid = 0, policy = 0;
        TOKEN_STATISTICS statistics{};
    } originalIdentity, lowIdentity;
    const auto identity = [&](HANDLE handle, Identity& value) -> HRESULT {
        alignas(TOKEN_MANDATORY_LABEL) std::array<BYTE, sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE> bytes{};
        DWORD size = 0;
        if (!GetTokenInformation(handle, TokenIntegrityLevel, bytes.data(), static_cast<DWORD>(bytes.size()), &size)) return failure();
        if (size < sizeof(TOKEN_MANDATORY_LABEL) || size > bytes.size()) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        const auto label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(bytes.data());
        const auto address = reinterpret_cast<UINT_PTR>(label->Label.Sid);
        const auto begin = reinterpret_cast<UINT_PTR>(bytes.data());
        if (address < begin + sizeof(TOKEN_MANDATORY_LABEL) || address > begin + size ||
            begin + size - address < offsetof(SID, SubAuthority) + sizeof(DWORD)) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        const auto sid = static_cast<const SID*>(label->Label.Sid);
        const SID_IDENTIFIER_AUTHORITY authority = SECURITY_MANDATORY_LABEL_AUTHORITY;
        if (sid->SubAuthorityCount != 1 || !IsValidSid(label->Label.Sid) ||
            std::memcmp(&sid->IdentifierAuthority, &authority, sizeof(authority)) != 0 ||
            !(label->Label.Attributes & SE_GROUP_INTEGRITY)) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        value.rid = sid->SubAuthority[0];
        TOKEN_MANDATORY_POLICY policy{};
        if (!GetTokenInformation(handle, TokenMandatoryPolicy, &policy, sizeof(policy), &size)) return failure();
        if (size != sizeof(policy)) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        if (!GetTokenInformation(handle, TokenStatistics, &value.statistics, sizeof(value.statistics), &size)) return failure();
        if (size != sizeof(value.statistics)) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        value.policy = policy.Policy;
        return S_OK;
    };
    // A fresh worker must have no thread impersonation token. No inherited or
    // caller token may be discarded as part of this diagnostic.
    const auto noThreadToken = [&] {
        Token unexpected;
        if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &unexpected.value)) return false;
        return GetLastError() == ERROR_NO_TOKEN;
    };
    if (!initialDesktop || PrivateDesktop::current() || !noThreadToken() || !budget()) {
        result.token = E_ACCESSDENIED;
        result.restored = initialDesktop && !PrivateDesktop::current() && noThreadToken() ? S_OK : E_ACCESSDENIED;
        return result;
    }
    result.token = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &process.value) ? S_OK : failure();
    if (SUCCEEDED(result.token)) result.token = identity(process.value, originalIdentity);
    result.originalRid = originalIdentity.rid; result.policy = originalIdentity.policy;
    if (SUCCEEDED(result.token) && (originalIdentity.rid < SECURITY_MANDATORY_MEDIUM_RID ||
        originalIdentity.statistics.TokenType != TokenPrimary)) result.token = E_ACCESSDENIED;
    if (SUCCEEDED(result.token) && budget()) result.lowToken = DuplicateTokenEx(process.value,
        TOKEN_QUERY | TOKEN_IMPERSONATE | TOKEN_ADJUST_DEFAULT, nullptr, SecurityImpersonation, TokenImpersonation, &low.value) ? S_OK : failure();
    alignas(SID) std::array<BYTE, SECURITY_MAX_SID_SIZE> lowSid{};
    DWORD sidBytes = static_cast<DWORD>(lowSid.size());
    if (SUCCEEDED(result.lowToken)) {
        if (!CreateWellKnownSid(WinLowLabelSid, nullptr, lowSid.data(), &sidBytes)) result.lowToken = failure();
        else {
            TOKEN_MANDATORY_LABEL label{{lowSid.data(), SE_GROUP_INTEGRITY}};
            if (!SetTokenInformation(low.value, TokenIntegrityLevel, &label, sizeof(label) + sidBytes)) result.lowToken = failure();
        }
    }
    if (SUCCEEDED(result.lowToken)) result.lowToken = identity(low.value, lowIdentity);
    result.lowRid = lowIdentity.rid;
    if (SUCCEEDED(result.lowToken) && (lowIdentity.rid != SECURITY_MANDATORY_LOW_RID || lowIdentity.policy != originalIdentity.policy ||
        lowIdentity.statistics.TokenType != TokenImpersonation || lowIdentity.statistics.ImpersonationLevel != SecurityImpersonation))
        result.lowToken = E_ACCESSDENIED;
    if (SUCCEEDED(result.lowToken) && budget()) {
        PrivateDesktop comparison;
        result.created = comparison.initializeAtomicLowForDiagnostic(result.security, deadline);
        const auto openPair = [&](const std::wstring& targetName, std::array<HRESULT, 2>& values, bool impersonate) {
            if (!budget() || PrivateDesktop::current() != &comparison || FAILED(comparison.verifyIsolation()) || !noThreadToken()) return;
            if (impersonate && !ImpersonateLoggedOnUser(low.value)) { values.fill(failure()); return; }
            // Stack-only native reads/opens until unconditional token revert.
            Token effective;
            Identity actual;
            HRESULT effectiveRead = S_OK;
            if (impersonate) {
                effectiveRead = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &effective.value) ? S_OK : failure();
                if (SUCCEEDED(effectiveRead)) effectiveRead = identity(effective.value, actual);
                if (SUCCEEDED(effectiveRead) && (actual.rid != SECURITY_MANDATORY_LOW_RID || actual.policy != lowIdentity.policy ||
                    actual.statistics.TokenType != TokenImpersonation || actual.statistics.ImpersonationLevel != SecurityImpersonation ||
                    actual.statistics.TokenId.LowPart != lowIdentity.statistics.TokenId.LowPart ||
                    actual.statistics.TokenId.HighPart != lowIdentity.statistics.TokenId.HighPart)) effectiveRead = E_ACCESSDENIED;
                if (SUCCEEDED(effectiveRead)) ++result.tokenReadbacks;
            }
            std::array<HDESK, 2> handles{};
            constexpr std::array<ACCESS_MASK, 2> masks{DESKTOP_READOBJECTS,
                DESKTOP_READOBJECTS | DESKTOP_CREATEWINDOW | DESKTOP_WRITEOBJECTS};
            for (size_t index = 0; index < handles.size(); ++index) {
                if (FAILED(effectiveRead)) { values[index] = effectiveRead; continue; }
                if (!budget()) { values[index] = HRESULT_FROM_WIN32(ERROR_TIMEOUT); continue; }
                handles[index] = OpenDesktopW(targetName.c_str(), 0, FALSE, masks[index]);
                values[index] = handles[index] ? S_OK : failure();
            }
            if (impersonate) {
                if (!RevertToSelf() || !noThreadToken()) stopUnsafe();
                ++result.reverts;
            }
            for (const auto handle : handles) if (handle && !CloseDesktop(handle)) {
                const auto error = GetLastError();
                // A close error must not masquerade as an admission denial.
                std::fprintf(stderr, "headless preview comparison CloseDesktop error=%lu\n", static_cast<unsigned long>(error));
                std::fflush(stderr); stopUnsafe();
            }
        };
        if (SUCCEEDED(result.created)) {
            openPair(comparison.diagnosticDefaultName(), result.processDefault, false);
            openPair(comparison.diagnosticDefaultName(), result.lowDefault, true);
            openPair(comparison.name(), result.processLowLabel, false);
            openPair(comparison.name(), result.lowLowLabel, true);
        }
        // No existing desktop is relabeled; observe restoration and both
        // retained native handle closes here.
        if (comparison.ready()) {
            result.finished = comparison.finishAtomicLowForDiagnostic();
            if (FAILED(result.finished)) stopUnsafe();
        }
    }
    // PrivateDesktop teardown must restore the exact borrowed initial desktop
    // before the existing COM activation attaches to the original app desktop.
    result.restored = GetThreadDesktop(GetCurrentThreadId()) == initialDesktop &&
        !PrivateDesktop::current() && noThreadToken() ? S_OK : E_ACCESSDENIED;
    if (FAILED(result.restored)) stopUnsafe();
    if (SUCCEEDED(result.token)) {
        Identity preserved;
        result.originalTokenPreserved = identity(process.value, preserved);
        if (SUCCEEDED(result.originalTokenPreserved) && (preserved.rid != originalIdentity.rid || preserved.policy != originalIdentity.policy ||
            preserved.statistics.TokenType != TokenPrimary || preserved.statistics.TokenId.LowPart != originalIdentity.statistics.TokenId.LowPart ||
            preserved.statistics.TokenId.HighPart != originalIdentity.statistics.TokenId.HighPart)) result.originalTokenPreserved = E_ACCESSDENIED;
    }
    result.completedWithinBudget = budget();
    result.discriminating = result.completedWithinBudget && SUCCEEDED(result.created) && SUCCEEDED(result.finished) && SUCCEEDED(result.restored) &&
        SUCCEEDED(result.security.equivalentSecurity) && SUCCEEDED(result.originalTokenPreserved) &&
        result.tokenReadbacks == 2 && result.reverts == 2 &&
        result.processDefault == std::array<HRESULT, 2>{S_OK, S_OK} && result.processLowLabel == std::array<HRESULT, 2>{S_OK, S_OK} &&
        (result.lowDefault[0] == S_OK || result.lowDefault[0] == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)) &&
        result.lowDefault[1] == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) &&
        result.lowLowLabel == std::array<HRESULT, 2>{S_OK, S_OK};
    return result;
}

// Activation/stream initialization is diagnostic only: no preview parent is
// supplied and DoPreview is never called. It cannot satisfy rendering proof.
std::wstring nativePreviewActivationDiagnostic(REFCLSID handler, IShellItem* actualSelected,
    const FILE_ID_INFO& expectedIdentity, const FILE_BASIC_INFO& expectedBasic, const std::vector<char>& source) {
    const auto desktop = PrivateDesktop::current();
    if (!desktop || FAILED(desktop->verifyIsolation()) || !actualSelected || source.size() != ownedPreviewRtfBytes)
        return L"native preview no-UI diagnostic rejected owned source/desktop";
    PreviewDiagnosticContext context;
    auto sourceGuard = capturePreviewDiagnosticContext(*desktop, &context);
    PIDLIST_ABSOLUTE rawSelected = nullptr;
    if (SUCCEEDED(sourceGuard)) sourceGuard = SHGetIDListFromObject(actualSelected, &rawSelected);
    Pidl nativeSelected(rawSelected);
    Pidl selectedIdentity(SUCCEEDED(sourceGuard) && nativeSelected ? ILCloneFull(nativeSelected.get()) : nullptr);
    if (SUCCEEDED(sourceGuard) && !selectedIdentity) sourceGuard = nativeSelected ? E_OUTOFMEMORY : E_UNEXPECTED;
    const auto selectedPath = SUCCEEDED(sourceGuard) ? itemName(actualSelected, SIGDN_FILESYSPATH) : std::wstring{};
    FILE_ID_INFO creatorIdentity{};
    if (SUCCEEDED(sourceGuard)) sourceGuard = selectedPath.empty() ? E_UNEXPECTED : nativeFileIdentity(selectedPath, creatorIdentity);
    if (SUCCEEDED(sourceGuard) && (creatorIdentity.VolumeSerialNumber != expectedIdentity.VolumeSerialNumber ||
        std::memcmp(creatorIdentity.FileId.Identifier, expectedIdentity.FileId.Identifier, sizeof(expectedIdentity.FileId.Identifier)) != 0))
        sourceGuard = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    if (FAILED(sourceGuard)) return L"native preview selected PIDL/FileID/context guard=" + hresultMessage(sourceGuard) + L"; factory calls=0";
    // LOCAL_SERVER chooses the surrogate route, which must be proven separately
    // from the caller's native InprocServer32 identity check.
    wchar_t classId[40]{};
    HRESULT route = StringFromGUID2(handler, classId, static_cast<int>(std::size(classId))) ? S_OK : E_FAIL;
    const std::wstring classKey = std::wstring(L"CLSID\\") + classId;
    const std::wstring appKey = L"AppID\\{6d2b5079-2f0b-48dd-ab7f-97cec514d30b}";
    const auto noKey = [](HKEY root, const std::wstring& path) {
        HKEY key = nullptr;
        const auto read = RegOpenKeyExW(root, path.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key);
        if (key) RegCloseKey(key);
        return read == ERROR_FILE_NOT_FOUND || read == ERROR_PATH_NOT_FOUND ? S_OK :
            HRESULT_FROM_WIN32(read == ERROR_SUCCESS ? ERROR_NOT_SUPPORTED : read);
    };
    const auto noValue = [](HKEY root, const std::wstring& path, const wchar_t* name) {
        DWORD bytes = 0;
        const auto read = RegGetValueW(root, path.c_str(), name, RRF_RT_ANY | RRF_SUBKEY_WOW6464KEY,
            nullptr, nullptr, &bytes);
        return read == ERROR_FILE_NOT_FOUND || read == ERROR_PATH_NOT_FOUND ? S_OK :
            HRESULT_FROM_WIN32(read == ERROR_SUCCESS || read == ERROR_MORE_DATA ? ERROR_NOT_SUPPORTED : read);
    };
    for (const auto name : {L"LocalServer32", L"LocalServer", L"TreatAs", L"AutoTreatAs"})
        if (SUCCEEDED(route)) route = noKey(HKEY_CLASSES_ROOT, classKey + L"\\" + name);
    std::array<wchar_t, 128> actualApp{};
    DWORD bytes = static_cast<DWORD>(sizeof(actualApp));
    if (SUCCEEDED(route)) route = HRESULT_FROM_WIN32(RegGetValueW(HKEY_CLASSES_ROOT, classKey.c_str(), L"AppID",
        RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, actualApp.data(), &bytes));
    GUID app{}, expectedApp{};
    if (SUCCEEDED(route) && (FAILED(CLSIDFromString(actualApp.data(), &app)) ||
        FAILED(CLSIDFromString(L"{6d2b5079-2f0b-48dd-ab7f-97cec514d30b}", &expectedApp)) || !IsEqualGUID(app, expectedApp)))
        route = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    std::array<wchar_t, 32768> system{};
    const auto systemLength = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
    FILE_ID_INFO expectedServer{};
    if (SUCCEEDED(route)) route = systemLength && systemLength < system.size() ?
        nativeFileIdentity(std::filesystem::path(system.data()) / L"prevhost.exe", expectedServer) : HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    for (const auto root : {HKEY_CLASSES_ROOT, HKEY_LOCAL_MACHINE}) {
        const auto path = root == HKEY_CLASSES_ROOT ? appKey : L"SOFTWARE\\Classes\\" + appKey;
        for (const auto name : {L"LocalService", L"RunAs", L"DllSurrogateExecutable", L"RemoteServerName"})
            if (SUCCEEDED(route)) route = noValue(root, path, name);
        if (FAILED(route)) break;
        std::array<wchar_t, 32768> server{}, expanded{};
        DWORD type = 0; bytes = static_cast<DWORD>(sizeof(server));
        route = HRESULT_FROM_WIN32(RegGetValueW(root, path.c_str(), L"DllSurrogate",
            RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND | RRF_SUBKEY_WOW6464KEY,
            &type, server.data(), &bytes));
        if (SUCCEEDED(route) && type == REG_EXPAND_SZ) {
            const auto length = ExpandEnvironmentStringsW(server.data(), expanded.data(), static_cast<DWORD>(expanded.size()));
            if (!length || length > expanded.size()) route = HRESULT_FROM_WIN32(length ? ERROR_INSUFFICIENT_BUFFER : GetLastError());
        }
        const std::filesystem::path serverPath = type == REG_EXPAND_SZ ? expanded.data() : server.data();
        FILE_ID_INFO actualServer{};
        if (SUCCEEDED(route)) route = serverPath.is_absolute() ? nativeFileIdentity(serverPath, actualServer) : HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        if (SUCCEEDED(route) && (actualServer.VolumeSerialNumber != expectedServer.VolumeSerialNumber ||
            std::memcmp(actualServer.FileId.Identifier, expectedServer.FileId.Identifier, sizeof(expectedServer.FileId.Identifier)) != 0))
            route = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }
    if (FAILED(route)) return L"native preview no-UI surrogate route unavailable=" + hresultMessage(route) + L"; factory calls=0";
    struct Label {
        HRESULT read = E_PENDING;
        bool present = false;
        DWORD rid = 0, mask = 0;
    } label;
    struct DesktopRead {
        HDESK handle = nullptr;
        ~DesktopRead() { if (handle) CloseDesktop(handle); }
    } query{OpenDesktopW(desktop->name().c_str(), 0, FALSE,
        READ_CONTROL | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS)};
    struct DescriptorRead {
        PSECURITY_DESCRIPTOR value = nullptr;
        ~DescriptorRead() { if (value) LocalFree(value); }
    } descriptor;
    label.read = query.handle ? HRESULT_FROM_WIN32(GetSecurityInfo(query.handle, SE_WINDOW_OBJECT,
        LABEL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, nullptr, &descriptor.value)) : HRESULT_FROM_WIN32(GetLastError());
    if (SUCCEEDED(label.read)) {
        PACL acl = nullptr; BOOL present = FALSE, defaulted = FALSE;
        if (!GetSecurityDescriptorSacl(descriptor.value, &present, &acl, &defaulted)) label.read = HRESULT_FROM_WIN32(GetLastError());
        else if (present && acl) for (DWORD index = 0; index < acl->AceCount; ++index) {
            void* raw = nullptr;
            if (!GetAce(acl, index, &raw)) { label.read = HRESULT_FROM_WIN32(GetLastError()); break; }
            const auto ace = static_cast<const SYSTEM_MANDATORY_LABEL_ACE*>(raw);
            if (ace->Header.AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE) continue;
            constexpr auto sidOffset = offsetof(SYSTEM_MANDATORY_LABEL_ACE, SidStart);
            const auto sid = reinterpret_cast<const SID*>(&ace->SidStart);
            const auto sidBytes = ace->Header.AceSize >= sidOffset ? ace->Header.AceSize - sidOffset : 0;
            const auto expectedSidBytes = offsetof(SID, SubAuthority) + static_cast<size_t>(sidBytes >= offsetof(SID, SubAuthority) ? sid->SubAuthorityCount : 0) * sizeof(DWORD);
            if (sidBytes < offsetof(SID, SubAuthority) || expectedSidBytes > sidBytes || !sid->SubAuthorityCount ||
                !IsValidSid(const_cast<SID*>(sid))) {
                label.read = HRESULT_FROM_WIN32(ERROR_INVALID_DATA); break;
            }
            label.present = true; label.mask = ace->Mask;
            label.rid = sid->SubAuthority[sid->SubAuthorityCount - 1];
        }
    }
    struct Activation {
        HRESULT apartment = E_PENDING, cancellation = E_PENDING, factory = E_PENDING, instance = E_PENDING;
        HRESULT preview = E_PENDING, initializer = E_PENDING, stream = E_PENDING, initialized = E_PENDING;
        HRESULT selectedItem = E_PENDING, policiesRead = E_PENDING, cancellationDisabled = E_PENDING, desktopRestored = E_PENDING;
        PreviewPolicySnapshot policies;
        PreviewStreamReadback boundSource;
        PreviewDesktopAccessDiagnostic desktopAccess;
        bool completedWithinWorkerBudget = false;
    };
    const auto targetDesktop = GetThreadDesktop(GetCurrentThreadId());
    const auto cancelled = std::make_shared<std::atomic_bool>(false);
    const auto deadline = GetTickCount64() + 4500;
    std::promise<Activation> promise;
    auto future = promise.get_future();
    std::thread worker([handler, source, expectedIdentity, expectedBasic, context, target = std::move(selectedIdentity),
        targetDesktop, cancelled, deadline, output = std::move(promise)]() mutable {
        Activation result;
        const auto initialDesktop = GetThreadDesktop(GetCurrentThreadId());
        const auto noThreadToken = [] {
            HANDLE token = nullptr;
            if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) { CloseHandle(token); return false; }
            return GetLastError() == ERROR_NO_TOKEN;
        };
        if (!initialDesktop || PrivateDesktop::current() || !noThreadToken()) {
            result.desktopAccess.token = E_ACCESSDENIED;
            output.set_value(result); return;
        }
        std::fprintf(stderr, "headless-preview-no-ui phase=desktop-access-comparison\n"); std::fflush(stderr);
        try { result.desktopAccess = previewDesktopAccessDiagnostic(*cancelled, deadline); }
        catch (const std::bad_alloc&) { result.desktopAccess.created = E_OUTOFMEMORY; }
        catch (...) { result.desktopAccess.created = E_FAIL; }
        // This check also covers exceptions during initialization/security
        // readback. COM must not begin after an uncertain token/desktop restore.
        result.desktopAccess.restored = GetThreadDesktop(GetCurrentThreadId()) == initialDesktop &&
            !PrivateDesktop::current() && noThreadToken() ? S_OK : E_ACCESSDENIED;
        if (FAILED(result.desktopAccess.restored)) {
            std::fprintf(stderr, "headless preview comparison restoration failed; no further activation\n"); std::fflush(stderr);
            if (!TerminateProcess(GetCurrentProcess(), 9)) std::_Exit(9);
            std::_Exit(9);
        }
        struct Apartment {
            HDESK previous = GetThreadDesktop(GetCurrentThreadId());
            HRESULT initialized = E_ACCESSDENIED, cancellation = E_PENDING;
            explicit Apartment(HDESK target) {
                if (SetThreadDesktop(target)) initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                else initialized = HRESULT_FROM_WIN32(GetLastError());
                if (SUCCEEDED(initialized)) cancellation = CoEnableCallCancellation(nullptr);
            }
            HRESULT finish(HRESULT& disabled) {
                if (SUCCEEDED(cancellation)) {
                    disabled = CoDisableCallCancellation(nullptr);
                    cancellation = E_PENDING;
                }
                if (SUCCEEDED(initialized)) { CoUninitialize(); initialized = E_PENDING; }
                if (!previous) return E_ACCESSDENIED;
                if (!SetThreadDesktop(previous)) {
                    const auto error = GetLastError();
                    return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
                }
                return GetThreadDesktop(GetCurrentThreadId()) == previous ? S_OK : E_ACCESSDENIED;
            }
            ~Apartment() {
                if (SUCCEEDED(cancellation)) CoDisableCallCancellation(nullptr);
                if (SUCCEEDED(initialized)) CoUninitialize();
            }
        } apartment(targetDesktop);
        struct NativeInterfaces {
            ComPtr<IStream> stream;
            ComPtr<IClassFactory> factory;
            ComPtr<IUnknown> instance;
            ComPtr<IPreviewHandler> preview;
            ComPtr<IInitializeWithStream> initializer;
            ComPtr<IShellItem> selected;
            void release() {
                // Initialize's source stream stays open until every retained
                // handler interface has released, on this initialized STA.
                initializer.Reset(); preview.Reset(); instance.Reset(); factory.Reset();
                stream.Reset(); selected.Reset();
            }
            ~NativeInterfaces() { release(); }
        } native;
        try {
            result.apartment = apartment.initialized; result.cancellation = apartment.cancellation;
            const auto budget = [&] { return !cancelled->load() && GetTickCount64() < deadline; };
            const auto phase = [](const char* name) {
                std::fprintf(stderr, "headless-preview-no-ui phase=%s\n", name); std::fflush(stderr);
            };
            if (SUCCEEDED(result.apartment) && SUCCEEDED(result.cancellation) && budget()) {
                phase("selected-pidl-item"); result.selectedItem = SHCreateItemFromIDList(target.get(), IID_PPV_ARGS(&native.selected));
            }
            if (SUCCEEDED(result.selectedItem) && budget()) {
                phase("read-only-policies"); result.policiesRead = inspectPreviewPolicies(context, handler, deadline, &result.policies, cancelled.get());
            }
            if (SUCCEEDED(result.policiesRead) && budget()) {
                phase("factory"); result.factory = CoGetClassObject(handler, CLSCTX_LOCAL_SERVER, nullptr, IID_PPV_ARGS(&native.factory));
            }
            if (SUCCEEDED(result.factory) && budget()) {
                phase("instance"); result.instance = native.factory->CreateInstance(nullptr, IID_PPV_ARGS(&native.instance));
            }
            if (SUCCEEDED(result.instance) && budget()) {
                phase("preview-qi"); result.preview = native.instance.As(&native.preview);
            }
            if (SUCCEEDED(result.instance) && budget()) {
                phase("initializer-qi"); result.initializer = native.instance.As(&native.initializer);
            }
            if (SUCCEEDED(result.preview) && SUCCEEDED(result.initializer) && budget()) {
                phase("initialize-actual-selected-read-only-bhid-stream");
                result.stream = inspectOwnedPreviewStream(context, native.selected.Get(), expectedIdentity,
                    std::span<const BYTE>(reinterpret_cast<const BYTE*>(source.data()), source.size()), deadline,
                    native.initializer.Get(), native.stream.GetAddressOf(), &result.boundSource, cancelled.get(), &expectedBasic);
                result.initialized = result.boundSource.initialized;
            }
        } catch (const std::bad_alloc&) { result.initialized = E_OUTOFMEMORY; }
          catch (...) { result.initialized = E_FAIL; }
        native.release();
        result.desktopRestored = apartment.finish(result.cancellationDisabled);
        if (FAILED(result.desktopRestored) || !noThreadToken()) {
            std::fprintf(stderr, "headless preview stream diagnostic failed exact worker desktop/token restoration\n"); std::fflush(stderr);
            if (!TerminateProcess(GetCurrentProcess(), 9)) std::_Exit(9);
            std::_Exit(9);
        }
        result.completedWithinWorkerBudget = !cancelled->load() && GetTickCount64() < deadline;
        output.set_value(result);
    });
    const auto nativeThread = static_cast<HANDLE>(worker.native_handle());
    const auto ready = [&] { return WaitForSingleObject(nativeThread, 0) == WAIT_OBJECT_0 &&
        future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; };
    const bool completed = pumpUntil(ready, 5000);
    if (!completed) {
        cancelled->store(true);
        CoCancelCall(GetThreadId(nativeThread), 0);
        if (!pumpUntil(ready, 5000)) {
            std::fprintf(stderr, "headless preview no-UI diagnostic did not join after cancellation; private resources retained\n");
            std::fflush(stderr);
            if (!TerminateProcess(GetCurrentProcess(), 9)) std::_Exit(9);
            std::_Exit(9);
        }
    }
    worker.join();
    const auto actual = future.get();
    const auto preserved = desktop->verifyIsolation();
    std::wostringstream numeric;
    const auto code = [](HRESULT value) { return static_cast<ULONG>(value); };
    const auto policy = [&](const wchar_t* name, const PreviewRegistryReadback& value) {
        numeric << L"; " << name << L" key/value/data HRESULT=" << code(value.keyRead) << L"/" << code(value.valueRead) << L"/" << code(value.dataRead)
            << L"; key/value present=" << value.keyPresent << L"/" << value.valuePresent << L"; type/bytes/DWORDvalid/value="
            << value.type << L"/" << value.bytes << L"/" << value.dwordValid << L"/" << value.value;
    };
    numeric << L"; native-selected-item/policies/worker-cancellation-disable/desktop-restore HRESULT=" << code(actual.selectedItem) << L"/"
        << code(actual.policiesRead) << L"/" << code(actual.cancellationDisabled) << L"/" << code(actual.desktopRestored)
        << L"; policy guardBefore/guardAfter/budget/completed HRESULT=" << code(actual.policies.guardBefore) << L"/" << code(actual.policies.guardAfter)
        << L"/" << code(actual.policies.budgetStatus) << L"/" << code(actual.policies.completed);
    policy(L"ShowPreviewHandlers", actual.policies.showPreviewHandlers);
    policy(L"HKCU.NoReadingPane", actual.policies.userNoReadingPane); policy(L"HKLM.NoReadingPane", actual.policies.machineNoReadingPane);
    policy(L"HKCU.EnforceShellExtensionSecurity", actual.policies.userEnforceShellExtensionSecurity);
    policy(L"HKLM.EnforceShellExtensionSecurity", actual.policies.machineEnforceShellExtensionSecurity);
    policy(L"HKCU.PreviewHandlers.actualAssociationCLSID", actual.policies.userPreviewHandlers);
    policy(L"HKLM.PreviewHandlers.actualAssociationCLSID", actual.policies.machinePreviewHandlers);
    policy(L"HKCU.Approved.actualAssociationCLSID", actual.policies.userApproved);
    policy(L"HKLM.Approved.actualAssociationCLSID", actual.policies.machineApproved);
    const auto& bound = actual.boundSource;
    numeric << L"; BHID_Stream guardBefore/guardAfter/budget/apartment HRESULT=" << code(bound.guardBefore) << L"/" << code(bound.guardAfter)
        << L"/" << code(bound.budgetStatus) << L"/" << code(bound.apartmentRead)
        << L"; source attributes/path/open/before/bytesBefore HRESULT=" << code(bound.attributesRead) << L"/" << code(bound.pathRead) << L"/"
        << code(bound.sourceOpen) << L"/" << code(bound.sourceBefore) << L"/" << code(bound.sourceBytesBefore)
        << L"; bindContext/options/bind/stat/seek/read/rewind/initialize HRESULT=" << code(bound.bindContext) << L"/" << code(bound.bindOptions) << L"/"
        << code(bound.bindRead) << L"/" << code(bound.statRead) << L"/" << code(bound.seekRead) << L"/" << code(bound.streamRead) << L"/"
        << code(bound.rewindRead) << L"/" << code(bound.initialized)
        << L"; sourceAfter/bytesAfter/pathAfter/completed HRESULT=" << code(bound.sourceAfter) << L"/" << code(bound.sourceBytesAfter) << L"/"
        << code(bound.pathAfter) << L"/" << code(bound.completed)
        << L"; native attributes/bindMode/streamType/streamMode/streamSize/readBytes=" << bound.shellAttributes << L"/" << bound.bindMode << L"/"
        << bound.streamType << L"/" << bound.streamMode << L"/" << bound.streamSize << L"/" << bound.streamBytes
        << L"; full FileID before/after/path preserved=" << bound.identityMatchesBefore << L"/" << bound.identityMatchesAfter << L"/" << bound.pathIdentityUnchanged
        << L"; complete83 fileBefore/stream/fileAfter equal=" << bound.fileBytesMatchBefore << L"/" << bound.streamBytesMatch << L"/" << bound.fileBytesMatchAfter
        << L"; attrs/creation/write/change preserved/originalBefore/originalAfter=" << bound.metadataUnchanged << L"/" << bound.originalMetadataMatchesBefore << L"/"
        << bound.originalMetadataMatchesAfter << L"; fresh actual-stream initializer attempted=" << bound.initializerAttempted;
    return L"verified native surrogate route=" + hresultMessage(route) + L"; owned desktop label HRESULT/present/RID/mask=" + hresultMessage(label.read) + L"/" + std::to_wstring(label.present) +
        L"/" + std::to_wstring(label.rid) + L"/" + std::to_wstring(label.mask) + L"; no-UI apartment/cancellation/factory/instance/previewQI/initializerQI/stream/initialize=" +
        hresultMessage(actual.apartment) + L"/" + hresultMessage(actual.cancellation) + L"/" + hresultMessage(actual.factory) + L"/" +
        hresultMessage(actual.instance) + L"/" + hresultMessage(actual.preview) + L"/" + hresultMessage(actual.initializer) + L"/" +
        hresultMessage(actual.stream) + L"/" + hresultMessage(actual.initialized) + L"; actual kernel exit joined within5000ms=" + std::to_wstring(completed) +
        L"; complete work/cleanup within shared4500ms=" + std::to_wstring(actual.completedWithinWorkerBudget) +
        L"; input/private isolation=" + hresultMessage(preserved) + L"; owned RTF bytes=" + std::to_wstring(source.size()) +
        L"; SetWindow/DoPreview calls=0/0; " + actual.desktopAccess.detail() + numeric.str();
}

struct PaneRendererWindows {
    std::vector<PrivateWindowSnapshot> windows;
    std::vector<PrivateWindowAdmission> admissions;
    PrivateWindowSnapshot parent{}, root{};
    size_t count = 0;
    HRESULT read = S_OK;
};
HRESULT paneWindowSnapshot(HWND window, PrivateWindowSnapshot& value) {
    value = {}; value.window = window;
    SetLastError(ERROR_SUCCESS);
    value.thread = GetWindowThreadProcessId(window, &value.process); value.threadError = GetLastError();
    if (!value.thread || !value.process || !IsWindow(window)) return E_ACCESSDENIED;
    value.parent = GetParent(window); value.root = GetAncestor(window, GA_ROOT);
    SetLastError(ERROR_SUCCESS);
    value.classRead = GetClassNameW(window, value.type.data(), static_cast<int>(value.type.size()));
    value.classError = GetLastError();
    if (value.classRead <= 0 || value.classRead >= static_cast<int>(value.type.size()) - 1 || !value.root) return E_ACCESSDENIED;
    SetLastError(ERROR_SUCCESS);
    const auto geometry = GetWindowRect(window, &value.rectangle); const auto geometryError = GetLastError();
    value.geometry = geometry ? S_OK : HRESULT_FROM_WIN32(geometryError ? geometryError : ERROR_GEN_FAILURE);
    if (!geometry) return value.geometry;
    SetLastError(ERROR_SUCCESS);
    value.borrowedDesktop = GetThreadDesktop(value.thread); value.desktopError = GetLastError();
    DWORD bytes = 0;
    if (value.borrowedDesktop) {
        SetLastError(ERROR_SUCCESS);
        const auto named = GetUserObjectInformationW(value.borrowedDesktop, UOI_NAME, value.desktop.data(),
            static_cast<DWORD>(sizeof(value.desktop)), &bytes); value.desktopError = GetLastError();
        value.desktopRead = named && value.desktop.front() && !value.desktop.back() ? S_OK :
            HRESULT_FROM_WIN32(value.desktopError ? value.desktopError : ERROR_INVALID_DATA);
    } else value.desktopRead = HRESULT_FROM_WIN32(value.desktopError ? value.desktopError : ERROR_GEN_FAILURE);
    return S_OK;
}
bool samePaneWindow(const PrivateWindowSnapshot& before, const PrivateWindowSnapshot& after) {
    return before.window == after.window && before.parent == after.parent && before.root == after.root &&
        before.process == after.process && before.thread == after.thread && before.classRead == after.classRead &&
        before.type == after.type && before.geometry == S_OK && after.geometry == S_OK &&
        EqualRect(&before.rectangle, &after.rectangle) && before.desktopRead == after.desktopRead &&
        before.borrowedDesktop == after.borrowedDesktop && before.desktop == after.desktop;
}
PaneRendererWindows paneRendererWindows(HWND pane, const RECT& region, const std::wstring& desktopName) {
    struct Enumeration { HWND pane; const RECT* region; const std::wstring* desktop; PaneRendererWindows value; } state{pane, &region, &desktopName, {}};
    state.value.windows.resize(256); state.value.admissions.resize(256);
    auto& parent = state.value.parent; auto& root = state.value.root;
    state.value.read = paneWindowSnapshot(pane, parent);
    if (state.value.read == S_OK) state.value.read = paneWindowSnapshot(parent.root, root);
    if (state.value.read != S_OK || parent.process != GetCurrentProcessId() || root.process != GetCurrentProcessId() ||
        parent.desktopRead != S_OK || root.desktopRead != S_OK || std::wstring_view(parent.desktop.data()) != desktopName ||
        std::wstring_view(root.desktop.data()) != desktopName || !EqualRect(&parent.rectangle, &region)) {
        state.value.read = E_ACCESSDENIED; return state.value;
    }
    EnumChildWindows(pane, [](HWND child, LPARAM context) -> BOOL {
        auto& state = *reinterpret_cast<Enumeration*>(context);
        RECT bounds{};
        if (!IsWindowVisible(child)) return TRUE;
        if (!GetWindowRect(child, &bounds)) { state.value.read = E_ACCESSDENIED; return FALSE; }
        if (bounds.right <= bounds.left || bounds.bottom <= bounds.top || bounds.left < state.region->left ||
            bounds.right > state.region->right || bounds.top < state.region->top || bounds.bottom > state.region->bottom) return TRUE;
        if (state.value.count == state.value.windows.size()) { state.value.read = HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW); return FALSE; }
        auto& value = state.value.windows[state.value.count];
        state.value.read = paneWindowSnapshot(child, value);
        if (state.value.read != S_OK || !IsChild(state.pane, child) ||
            (value.desktopRead == S_OK && std::wstring_view(value.desktop.data()) != *state.desktop) ||
            (value.process == GetCurrentProcessId() && value.desktopRead != S_OK)) {
            state.value.read = E_ACCESSDENIED; return FALSE;
        }
        ++state.value.count; return TRUE;
    }, reinterpret_cast<LPARAM>(&state));
    return state.value;
}
bool paneRenderersCurrent(HWND pane, const RECT& region, const std::wstring& desktopName, const PaneRendererWindows& admitted) {
    const auto current = paneRendererWindows(pane, region, desktopName);
    if (current.read != S_OK || current.count != admitted.count || !samePaneWindow(current.parent, admitted.parent) ||
        !samePaneWindow(current.root, admitted.root)) return false;
    for (size_t index = 0; index < current.count; ++index) {
        const auto& window = current.windows[index];
        const auto found = std::find_if(admitted.windows.begin(), admitted.windows.begin() + admitted.count,
            [&](const auto& original) { return original.window == window.window; });
        if (found == admitted.windows.begin() + admitted.count || !samePaneWindow(*found, window)) return false;
        const auto offset = static_cast<size_t>(found - admitted.windows.begin());
        if (window.process != GetCurrentProcessId() && admitted.admissions[offset] == PrivateWindowAdmission::None) return false;
    }
    return true;
}

// A pane has no documented HWND getter. Derive its physical region from the
// actual DefView and host client, then inspect only visible providers within
// that region. Content-list and navigation names cannot satisfy this proof.
PaneObservation observeNativePane(HWND host, HWND content, const std::vector<std::wstring>& expected,
    const std::wstring& absent, bool preview, HWND ownedPreview = nullptr,
    const WindowIsolationControlReport* controls = nullptr, const std::function<HRESULT()>& sourceReady = {}) {
    PaneObservation rejected;
    const auto deadline = GetTickCount64() + 4500;
    rejected.observationDeadline = deadline;
    const auto rectangle = [](const RECT& value) {
        return std::to_wstring(value.left) + L"," + std::to_wstring(value.top) + L"," +
            std::to_wstring(value.right) + L"," + std::to_wstring(value.bottom);
    };
    rejected.detail = L"pane observation guard; host HWND=" + std::to_wstring(reinterpret_cast<UINT_PTR>(host)) +
        L"; content HWND=" + std::to_wstring(reinterpret_cast<UINT_PTR>(content));
    const auto desktop = PrivateDesktop::current();
    DWORD process = 0;
    RECT hostBounds{}, contentBounds{};
    if (!desktop || FAILED(desktop->verifyIsolation()) ||
        GetWindowThreadProcessId(host, &process) != GetCurrentThreadId() || process != GetCurrentProcessId() ||
        !IsChild(host, content) || !GetClientRect(host, &hostBounds) ||
        FAILED(mapUiRect(host, nullptr, hostBounds, &hostBounds)) || !GetWindowRect(content, &contentBounds)) {
        rejected.read = E_ACCESSDENIED;
        rejected.detail += L"; host bounds=" + rectangle(hostBounds) + L"; content bounds=" + rectangle(contentBounds);
        return rejected;
    }
    bool rtl = false;
    if (FAILED(windowUiDirection(host, &rtl))) { rejected.read = E_INVALIDARG; return rejected; }
    RECT region{rtl ? hostBounds.left : contentBounds.right, contentBounds.top,
        rtl ? contentBounds.left : hostBounds.right, contentBounds.bottom};
    if(ownedPreview) {
        DWORD paneProcess=0;
        if(!preview||GetWindowThreadProcessId(ownedPreview,&paneProcess)!=GetCurrentThreadId()||
           paneProcess!=GetCurrentProcessId()||!IsChild(host,ownedPreview)||IsChild(content,ownedPreview)||
           !GetWindowRect(ownedPreview,&region)||region.left<hostBounds.left||region.right>hostBounds.right||
           region.top<hostBounds.top||region.bottom>hostBounds.bottom) {
            rejected.read=E_ACCESSDENIED;return rejected;
        }
    }
    rejected.detail += L"; RTL=" + std::to_wstring(rtl) + L"; host bounds=" + rectangle(hostBounds) +
        L"; content bounds=" + rectangle(contentBounds) + L"; pane bounds=" + rectangle(region);
    if (region.right <= region.left || region.bottom <= region.top) {
        rejected.read = E_UNEXPECTED;
        return rejected;
    }
    const auto targetDesktop = GetThreadDesktop(GetCurrentThreadId());
    const auto desktopName = desktop->name();
    const auto inputName = desktop->originalInputName();
    PaneRendererWindows admitted;
    unsigned exactDesktopAdmissions = 0, inferredAdmissions = 0;
    if (preview) {
        // The automatic reference has no accepted App ticket/owned render pane.
        // It grants no permission to inspect a foreign renderer's content.
        if (!ownedPreview || !controls || !sourceReady) {
            rejected.read = E_ACCESSDENIED; rejected.detail += L"; foreign Preview content requires creator-calibrated admission"; return rejected;
        }
        HRESULT readyRead = E_PENDING;
        const auto remaining = GetTickCount64() < deadline ? static_cast<DWORD>(deadline - GetTickCount64()) : 0;
        const bool ready = remaining && pumpUntil([&] { readyRead = sourceReady(); return readyRead != E_PENDING; }, remaining);
        if (!ready || readyRead != S_OK || GetTickCount64() >= deadline) {
            rejected.read = readyRead != E_PENDING ? readyRead : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            rejected.detail += L"; actual creator Ready/source HRESULT=" + hresultMessage(readyRead); return rejected;
        }
        admitted = paneRendererWindows(ownedPreview, region, desktopName);
        if (admitted.read != S_OK) { rejected.read = admitted.read; return rejected; }
        for (size_t index = 0; index < admitted.count; ++index) {
            auto& native = admitted.windows[index];
            if (native.process == GetCurrentProcessId()) continue;
            PrivateWindowAdmissionReport admission;
            const auto before = sourceReady();
            const auto admissionRead = before == S_OK ? admitForeignPrivateWindow(*desktop, ownedPreview,
                native.window, *controls, deadline, &admission) : before;
            const auto after = sourceReady();
            if (admissionRead != S_OK || after != S_OK || !samePaneWindow(native, admission.targetBefore)) {
                rejected.read = admissionRead != S_OK ? admissionRead : after != S_OK ? after : E_ABORT;
                rejected.detail += L"; creator renderer admission HRESULT=" + hresultMessage(admissionRead) +
                    L"; source before/after=" + hresultMessage(before) + L"/" + hresultMessage(after); return rejected;
            }
            native = admission.targetAfter; admitted.admissions[index] = admission.admission;
            if (admission.admission == PrivateWindowAdmission::ExactDesktopQuery) ++exactDesktopAdmissions;
            else if (admission.admission == PrivateWindowAdmission::MessageChannelInference) ++inferredAdmissions;
        }
        if (!exactDesktopAdmissions && !inferredAdmissions) { rejected.read = E_UNEXPECTED; rejected.detail += L"; actual Ready has no visible foreign renderer"; return rejected; }
        if (sourceReady() != S_OK || !paneRenderersCurrent(ownedPreview, region, desktopName, admitted)) { rejected.read = E_ABORT; return rejected; }
    }
    const auto cancelled = std::make_shared<std::atomic_bool>(false);
    std::promise<PaneObservation> promise;
    auto future = promise.get_future();
    std::thread worker([host, ownedPreview, admitted, exactDesktopAdmissions, inferredAdmissions, region, expected, absent, preview, targetDesktop, desktopName, inputName,
        cancelled, deadline, output = std::move(promise)]() mutable {
        PaneObservation result;
        result.observationDeadline = deadline;
        result.bounds = region;
        unsigned phase = 1, polls = 0, names = 0, values = 0, documents = 0;
        int availableElements = 0;
        HRESULT nameRead = E_PENDING, valueRead = E_PENDING, documentRead = E_PENDING;
        HRESULT valuePatternRead = E_PENDING, textPatternRead = E_PENDING, rangeRead = E_PENDING, textRead = E_PENDING;
        std::wstring intersectingElements, samples;
        struct Apartment {
            HDESK previous = GetThreadDesktop(GetCurrentThreadId());
            HRESULT read = E_ACCESSDENIED;
            HRESULT initialized = E_ACCESSDENIED;
            bool cancellationEnabled = false;
            explicit Apartment(HDESK target) {
                if (SetThreadDesktop(target)) initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                read = initialized;
                if (SUCCEEDED(read)) { read = CoEnableCallCancellation(nullptr); cancellationEnabled = SUCCEEDED(read); }
            }
            ~Apartment() {
                if (cancellationEnabled) CoDisableCallCancellation(nullptr);
                if (SUCCEEDED(initialized)) CoUninitialize();
                SetThreadDesktop(previous);
            }
        } apartment(targetDesktop);
        try {
            result.read = apartment.read;
            ComPtr<IUIAutomation> automation;
            ComPtr<IUIAutomation2> timeout;
            ComPtr<IUIAutomationCondition> everything;
            if (SUCCEEDED(result.read)) { phase = 2; result.read = CoCreateInstance(CLSID_CUIAutomation8, nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)); }
            if (SUCCEEDED(result.read)) { phase = 3; result.read = automation.As(&timeout); }
            if (SUCCEEDED(result.read)) { phase = 4; result.read = timeout->put_ConnectionTimeout(700); }
            if (SUCCEEDED(result.read)) result.read = timeout->put_TransactionTimeout(700);
            if (SUCCEEDED(result.read)) result.read = timeout->put_AutoSetFocus(FALSE);
            if (SUCCEEDED(result.read)) { phase = 5; result.read = automation->CreateTrueCondition(&everything); }
            const auto budget = [&] { return !cancelled->load() && GetTickCount64() < deadline; };
            const auto userObjectName = [](HDESK value) {
                wchar_t name[256]{}; DWORD bytes = 0;
                return value && GetUserObjectInformationW(value, UOI_NAME, name, sizeof(name), &bytes) ?
                    std::wstring(name) : std::wstring{};
            };
            do {
                if (FAILED(result.read) || !budget()) break;
                // Native creator admission precedes all foreign UIA calls;
                // unchanged plain HWND identities must still hold on this MTA.
                if (preview && !paneRenderersCurrent(ownedPreview, region, desktopName, admitted)) {
                    result.read = E_ACCESSDENIED; result.privacyChecked = true; result.privateWindows = false; break;
                }
                ++polls;
                ComPtr<IUIAutomationElement> root;
                ComPtr<IUIAutomationElementArray> elements;
                phase = 6;
                result.read = automation->ElementFromHandle(preview ? ownedPreview : host, &root);
                if (SUCCEEDED(result.read) && !root) result.read = E_NOINTERFACE;
                if (SUCCEEDED(result.read) && budget()) { phase = 7; result.read = root->FindAll(TreeScope_Descendants, everything.Get(), &elements); }
                int count = 0;
                if (SUCCEEDED(result.read) && elements && budget()) { phase = 8; result.read = elements->get_Length(&count); }
                availableElements = count;
                std::wstring text;
                result.visibleElements = 0;
                names = values = documents = 0;
                intersectingElements.clear();
                samples.clear();
                nameRead = valueRead = documentRead = valuePatternRead = textPatternRead = rangeRead = textRead = E_PENDING;
                // Only pane-contained providers for these owned source files
                // contribute diagnostic samples. Keep both per-value and total
                // limits independent of the stricter semantic assertion.
                const auto sample = [&](const wchar_t* label, BSTR value) {
                    if (!value || samples.size() >= 1024) return;
                    const std::wstring prefix = std::wstring(L"; ") + label + L"=[";
                    samples.append(prefix, 0, std::min(prefix.size(), 1024 - samples.size()));
                    samples.append(value, std::min<size_t>({SysStringLen(value), 256, 1024 - samples.size()}));
                    if (samples.size() < 1024) samples += L']';
                };
                unsigned recordedElements = 0;
                phase = SUCCEEDED(result.read) ? 9 : phase;
                for (int index = 0; SUCCEEDED(result.read) && index < std::min(count, 256) && budget(); ++index) {
                    ComPtr<IUIAutomationElement> element;
                    RECT bounds{}; BOOL offscreen = TRUE;
                    if (FAILED(elements->GetElement(index, &element)) || !element ||
                        FAILED(element->get_CurrentIsOffscreen(&offscreen)) || offscreen ||
                        FAILED(element->get_CurrentBoundingRectangle(&bounds)) || bounds.right <= bounds.left || bounds.bottom <= bounds.top) continue;
                    const bool contained = bounds.left >= region.left && bounds.right <= region.right &&
                        bounds.top >= region.top && bounds.bottom <= region.bottom;
                    RECT intersection{};
                    if (recordedElements < 6 && IntersectRect(&intersection, &bounds, &region)) {
                        CONTROLTYPEID type = 0; element->get_CurrentControlType(&type);
                        intersectingElements += L"; candidate=" + std::to_wstring(type) + L"/contained=" + std::to_wstring(contained) +
                            L"/bounds=" + std::to_wstring(bounds.left) + L"," + std::to_wstring(bounds.top) + L"," +
                            std::to_wstring(bounds.right) + L"," + std::to_wstring(bounds.bottom);
                        ++recordedElements;
                    }
                    if (!contained) continue;
                    if (preview) {
                        if (!budget()) { result.read = HRESULT_FROM_WIN32(ERROR_TIMEOUT); break; }
                        int elementProcess = 0;
                        const auto processRead = element->get_CurrentProcessId(&elementProcess);
                        const bool admittedProcess = elementProcess > 0 && (static_cast<DWORD>(elementProcess) == GetCurrentProcessId() ||
                            std::any_of(admitted.windows.begin(), admitted.windows.begin() + admitted.count,
                                [&](const auto& native) { return native.process == static_cast<DWORD>(elementProcess); }));
                        if (processRead != S_OK || !admittedProcess || !budget() ||
                            !paneRenderersCurrent(ownedPreview, region, desktopName, admitted) || !budget()) {
                            result.read = E_ACCESSDENIED; result.privacyChecked = true; result.privateWindows = false; break;
                        }
                    }
                    ++result.visibleElements;
                    BSTR name = nullptr;
                    nameRead = element->get_CurrentName(&name);
                    if (SUCCEEDED(nameRead) && name) { ++names; sample(L"Name", name); text.append(name, std::min<size_t>(SysStringLen(name), 1024)); text += L'\n'; }
                    SysFreeString(name);
                    ComPtr<IUIAutomationValuePattern> value;
                    if (budget()) valuePatternRead = valueRead = element->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&value));
                    if (budget() && SUCCEEDED(valueRead) && value) {
                        if (preview && (!paneRenderersCurrent(ownedPreview, region, desktopName, admitted) || !budget())) {
                            result.read = E_ACCESSDENIED; result.privacyChecked = true; result.privateWindows = false; break;
                        }
                        BSTR actual = nullptr;
                        valueRead = value->get_CurrentValue(&actual);
                        if (SUCCEEDED(valueRead) && actual) { ++values; sample(L"Value", actual); text.append(actual, std::min<size_t>(SysStringLen(actual), 1024)); text += L'\n'; }
                        SysFreeString(actual);
                    }
                    if (preview && budget()) {
                        ComPtr<IUIAutomationTextPattern> pattern;
                        ComPtr<IUIAutomationTextRange> range;
                        textPatternRead = documentRead = element->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&pattern));
                        if (SUCCEEDED(documentRead) && pattern) rangeRead = documentRead = pattern->get_DocumentRange(&range);
                        if (SUCCEEDED(documentRead) && range) {
                            if (!budget() || !paneRenderersCurrent(ownedPreview, region, desktopName, admitted) || !budget()) {
                                result.read = E_ACCESSDENIED; result.privacyChecked = true; result.privateWindows = false; break;
                            }
                            BSTR actual = nullptr;
                            textRead = documentRead = range->GetText(2048, &actual);
                            if (SUCCEEDED(documentRead) && actual) { ++documents; sample(L"Text", actual); text.append(actual, SysStringLen(actual)); text += L'\n'; }
                            SysFreeString(actual);
                        }
                    }
                    if (text.size() > 16384) { result.read = HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW); break; }
                }
                struct Windows {
                    HWND host; RECT region; std::wstring desktop;
                    bool valid = true; unsigned foreign = 0, visited = 0, contained = 0, invalidDesktop = 0;
                    DWORD desktopError = ERROR_SUCCESS;
                    std::set<DWORD> processes{GetCurrentProcessId()};
                    const PaneRendererWindows* admissions = nullptr;
                } windows{host, region, desktopName};
                if (preview) {
                    windows.admissions = &admitted;
                    windows.valid = paneRenderersCurrent(ownedPreview, region, desktopName, admitted);
                }
                EnumChildWindows(host, [](HWND child, LPARAM context) -> BOOL {
                    auto& state = *reinterpret_cast<Windows*>(context);
                    ++state.visited;
                    RECT bounds{};
                    if (!IsWindowVisible(child) || !GetWindowRect(child, &bounds) || bounds.right <= bounds.left || bounds.bottom <= bounds.top ||
                        bounds.left < state.region.left || bounds.right > state.region.right ||
                        bounds.top < state.region.top || bounds.bottom > state.region.bottom) return TRUE;
                    ++state.contained;
                    DWORD childProcess = 0;
                    const auto thread = GetWindowThreadProcessId(child, &childProcess);
                    wchar_t name[256]{}; DWORD bytes = 0;
                    const auto childDesktop = GetThreadDesktop(thread);
                    if (!childDesktop) state.desktopError = GetLastError();
                    const bool sameDesktop = childDesktop && GetUserObjectInformationW(childDesktop, UOI_NAME, name, sizeof(name), &bytes) &&
                        state.desktop == name && IsChild(state.host, child);
                    if (childDesktop && !name[0]) state.desktopError = GetLastError();
                    bool permitted = sameDesktop;
                    if (!permitted && state.admissions && childProcess != GetCurrentProcessId() &&
                        (!name[0]) && IsChild(state.host, child)) {
                        for (size_t index = 0; index < state.admissions->count; ++index) {
                            const auto& admittedWindow = state.admissions->windows[index];
                            PrivateWindowSnapshot current;
                            if (admittedWindow.window == child && state.admissions->admissions[index] == PrivateWindowAdmission::MessageChannelInference &&
                                paneWindowSnapshot(child, current) == S_OK && samePaneWindow(admittedWindow, current)) {
                                permitted = true; break;
                            }
                        }
                    }
                    if (!permitted) ++state.invalidDesktop;
                    state.valid = permitted && state.valid;
                    if (childProcess != GetCurrentProcessId()) { ++state.foreign; state.processes.insert(childProcess); }
                    return TRUE;
                }, reinterpret_cast<LPARAM>(&windows));
                HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS | DESKTOP_ENUMERATE);
                const auto inputOpenError = input ? ERROR_SUCCESS : GetLastError();
                result.inputUnchanged = userObjectName(input) == inputName && inputName != desktopName;
                bool visibleInput = false;
                struct Input { const std::set<DWORD>* processes; bool* visible; } inputState{&windows.processes, &visibleInput};
                SetLastError(ERROR_SUCCESS);
                const bool inspected = input && EnumDesktopWindows(input, [](HWND candidate, LPARAM context) -> BOOL {
                    auto& state = *reinterpret_cast<Input*>(context); DWORD owner = 0; GetWindowThreadProcessId(candidate, &owner);
                    if (state.processes->contains(owner) && IsWindowVisible(candidate)) *state.visible = true;
                    return TRUE;
                }, reinterpret_cast<LPARAM>(&inputState));
                const auto inputEnumError = inspected ? ERROR_SUCCESS : GetLastError();
                if (input) CloseDesktop(input);
                result.privacyChecked = true;
                result.privateWindows = windows.valid && inspected && result.inputUnchanged && !visibleInput;
                result.handlerWindows = windows.foreign;
                const auto matched = static_cast<unsigned>(std::count_if(expected.begin(), expected.end(), [&](const auto& token) {
                    return !token.empty() && text.find(token) != std::wstring::npos;
                }));
                result.matched = SUCCEEDED(result.read) && budget() && matched == expected.size() && !expected.empty() &&
                    (absent.empty() || text.find(absent) == std::wstring::npos) && result.privateWindows && (!preview || result.handlerWindows != 0);
                result.detail = L"pane visible elements=" + std::to_wstring(result.visibleElements) + L"; semantic matches=" +
                    std::to_wstring(matched) + L"/" + std::to_wstring(expected.size()) + L"; obsolete token absent=" +
                    std::to_wstring(absent.empty() || text.find(absent) == std::wstring::npos) + L"; foreign handler HWNDs=" +
                    std::to_wstring(result.handlerWindows) + L"; private HWNDs=" + std::to_wstring(result.privateWindows) +
                    L"; child HWNDs visited/contained/invalidDesktop=" + std::to_wstring(windows.visited) + L"/" +
                    std::to_wstring(windows.contained) + L"/" + std::to_wstring(windows.invalidDesktop) +
                    L"; desktop native error=" + std::to_wstring(windows.desktopError) + L"; input open/enum native error=" +
                    std::to_wstring(inputOpenError) + L"/" + std::to_wstring(inputEnumError) + L"; input inspected/unchanged/visible=" +
                    std::to_wstring(inspected) + L"/" + std::to_wstring(result.inputUnchanged) + L"/" + std::to_wstring(visibleInput) +
                    L"; bounds=" + std::to_wstring(region.left) + L"," + std::to_wstring(region.top) + L"," +
                    std::to_wstring(region.right) + L"," + std::to_wstring(region.bottom) + L"; read=" + hresultMessage(result.read);
                if (preview) result.detail += L"; creator exact-desktop admissions=" + std::to_wstring(exactDesktopAdmissions) +
                    L"; calibrated message-channel INFERENCES=" + std::to_wstring(inferredAdmissions) + L"; inference is not a queried HDESK equality";
                if (result.matched || !result.privateWindows) break;
                Sleep(40);
            } while (budget());
            if (!result.matched && SUCCEEDED(result.read)) result.read = HRESULT_FROM_WIN32(cancelled->load() ? ERROR_CANCELLED : ERROR_TIMEOUT);
        } catch (const std::bad_alloc&) { result.read = E_OUTOFMEMORY; }
          catch (...) { result.read = E_FAIL; }
        result.detail += L"; observation phase=" + std::to_wstring(phase) + L"; polls=" + std::to_wstring(polls) +
            L"; UIA available=" + std::to_wstring(availableElements) + L"; Name/Value/Text reads=" + std::to_wstring(names) + L"/" +
            std::to_wstring(values) + L"/" + std::to_wstring(documents) + L"; Value/Text HRESULT=" + hresultMessage(valueRead) + L"/" +
            hresultMessage(documentRead) + L"; Name/ValuePattern/TextPattern/DocumentRange/Text HRESULT=" + hresultMessage(nameRead) + L"/" +
            hresultMessage(valuePatternRead) + L"/" + hresultMessage(textPatternRead) + L"/" + hresultMessage(rangeRead) + L"/" +
            hresultMessage(textRead) + L"; final HRESULT=" + hresultMessage(result.read) + intersectingElements + samples;
        output.set_value(std::move(result));
    });
    // Publishing the value does not prove UIA/COM teardown has returned. Wait
    // for the real kernel thread before joining or releasing its native site.
    const auto nativeThread = static_cast<HANDLE>(worker.native_handle());
    HRESULT creatorSourceStatus = S_OK;
    const auto ready = [&] {
        if (preview && creatorSourceStatus == S_OK && GetTickCount64() < deadline) {
            creatorSourceStatus = sourceReady();
            if (creatorSourceStatus != S_OK) { cancelled->store(true); CoCancelCall(GetThreadId(nativeThread), 0); }
        }
        return WaitForSingleObject(nativeThread, 0) == WAIT_OBJECT_0 &&
            future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
    };
    const auto now = GetTickCount64();
    const bool completed = pumpUntil(ready, static_cast<DWORD>((now < deadline ? deadline - now : 0) + 500));
    if (!completed) {
        cancelled->store(true);
        CoCancelCall(GetThreadId(nativeThread), 0);
        if (!pumpUntil(ready, 5000)) {
            std::fprintf(stderr, "headless pane UIA failed to join after cancellation; private owner/resources retained\n");
            std::fflush(stderr);
            if (!TerminateProcess(GetCurrentProcess(), 9)) std::_Exit(9);
            std::_Exit(9);
        }
    }
    worker.join();
    auto result = future.get();
    if (!completed) { result.matched = false; result.read = HRESULT_FROM_WIN32(ERROR_TIMEOUT); }
    if (preview && (creatorSourceStatus != S_OK || sourceReady() != S_OK || !paneRenderersCurrent(ownedPreview, region, desktopName, admitted) ||
        FAILED(desktop->verifyIsolation()))) {
        result.matched = false; result.privateWindows = false; result.privacyChecked = true; result.read = E_ABORT;
        result.detail += L"; creator post-kernel-exit source/native identity fence failed";
    }
    if (preview && result.privateWindows) result.rendererAdmission = std::make_shared<const PaneRendererWindows>(std::move(admitted));
    return result;
}

std::wstring paneHostingRequestsJson(const HeadlessPaneHostingRequests& requests) {
    const auto guid = [](REFGUID value) {
        std::array<wchar_t, 40> text{};
        return StringFromGUID2(value, text.data(), static_cast<int>(text.size())) ? std::wstring(text.data()) : std::wstring{};
    };
    std::wostringstream json;
    json << L"{\"overflow\":" << (requests.overflow ? L"true" : L"false")
        << L",\"frameIID\":\"" << guid(IID_IPreviewHandlerFrame) << L"\",\"frameQIIncludingServiceDelegation\":[";
    for (UINT index = 0; index < requests.frameCount; ++index) {
        const auto& row = requests.frameQueries[index];
        if (index) json << L",";
        json << L"{\"hresult\":" << static_cast<ULONG>(row.result) << L",\"calls\":" << row.calls << L"}";
    }
    json << L"],\"services\":[";
    for (UINT index = 0; index < requests.serviceCount; ++index) {
        const auto& row = requests.services[index];
        if (index) json << L",";
        json << L"{\"sid\":\"" << guid(row.service) << L"\",\"iid\":\"" << guid(row.iid)
            << L"\",\"hresult\":" << static_cast<ULONG>(row.result) << L",\"calls\":" << row.calls << L"}";
    }
    json << L"],\"panes\":[";
    for (UINT index = 0; index < requests.paneCount; ++index) {
        const auto& row = requests.panes[index];
        if (index) json << L",";
        json << L"{\"pane\":\"" << guid(row.pane) << L"\",\"hresult\":" << static_cast<ULONG>(row.result)
            << L",\"flags\":" << row.flags << L",\"outputPresent\":" << (row.outputPresent ? L"true" : L"false")
            << L",\"calls\":" << row.calls << L"}";
    }
    json << L"]}";
    return json.str();
}

class NativePaneSite final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IServiceProvider, IExplorerPaneVisibility> {
public:
    using Base = Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
        IServiceProvider, IExplorerPaneVisibility>;
    bool preview = false;
    bool trace = false;
    HeadlessPaneHostingRequests requests;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        const auto result = Base::QueryInterface(iid, output);
        if (trace) requests.frame(iid, result);
        return result;
    }
    HRESULT STDMETHODCALLTYPE QueryService(REFGUID service, REFIID iid, void** output) override {
        HRESULT result;
        if (!output) result = E_POINTER;
        else {
            *output = nullptr;
            result = service == SID_ExplorerPaneVisibility ? QueryInterface(iid, output) : E_NOINTERFACE;
        }
        if (trace) requests.service(service, iid, result);
        return result;
    }
    HRESULT STDMETHODCALLTYPE GetPaneState(REFEXPLORERPANE pane, EXPLORERPANESTATE* state) override {
        if (!state) {
            if (trace) requests.pane(pane, E_POINTER, nullptr);
            return E_POINTER;
        }
        const bool visible = preview ? pane == EP_PreviewPane : pane == EP_DetailsPane;
        *state = static_cast<EXPLORERPANESTATE>((visible ? EPS_DEFAULT_ON : EPS_DEFAULT_OFF) | EPS_FORCE);
        if (trace) requests.pane(pane, S_OK, state);
        return S_OK;
    }
};

// The paired Share fixture changes only the outer ExplorerBrowser site. This
// independent browser never receives the App site and never displays its owner.
// Its actual completion callback and view remain alive until Destroy returns.
struct ShareSiteReference {
    class Navigation final : public Microsoft::WRL::RuntimeClass<
        Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IExplorerBrowserEvents> {
    public:
        bool finished = false;
        HRESULT status = E_PENDING;
        Pidl completed;
        HRESULT STDMETHODCALLTYPE OnNavigationPending(PCIDLIST_ABSOLUTE) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE OnViewCreated(IShellView*) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE OnNavigationComplete(PCIDLIST_ABSOLUTE location) override {
            completed.reset(location ? ILCloneFull(location) : nullptr);
            status = completed ? S_OK : E_OUTOFMEMORY;
            finished = true;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnNavigationFailed(PCIDLIST_ABSOLUTE) override {
            status = E_FAIL; finished = true; return S_OK;
        }
    };
    HWND owner = nullptr;
    ComPtr<IExplorerBrowser> browser;
    ComPtr<IShellView> view;
    ComPtr<Navigation> navigation;
    DWORD cookie = 0;
    HRESULT create(HINSTANCE instance, IShellItem* folder, EXPLORER_BROWSER_OPTIONS options,
                   const FOLDERSETTINGS& settings) {
        const auto desktop = PrivateDesktop::current();
        if (!desktop || FAILED(desktop->verifyIsolation()) || !folder || !(options & EBO_NOPERSISTVIEWSTATE))
            return E_ACCESSDENIED;
        owner = CreateWindowExW(0, L"STATIC", L"Owned independent Share site", WS_POPUP,
            0, 0, 800, 600, nullptr, nullptr, instance, nullptr);
        if (!owner) return HRESULT_FROM_WIN32(GetLastError());
        if (IsWindowVisible(owner)) return E_ACCESSDENIED;
        auto hr = CoCreateInstance(CLSID_ExplorerBrowser, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&browser));
        if (SUCCEEDED(hr)) hr = browser->SetOptions(options);
        RECT bounds{0, 0, 800, 600};
        if (SUCCEEDED(hr)) hr = browser->Initialize(owner, &bounds, &settings);
        if (SUCCEEDED(hr)) {
            navigation = Microsoft::WRL::Make<Navigation>();
            hr = navigation ? browser->Advise(navigation.Get(), &cookie) : E_OUTOFMEMORY;
        }
        if (SUCCEEDED(hr)) hr = browser->BrowseToObject(folder, SBSP_ABSOLUTE);
        return hr;
    }
    ~ShareSiteReference() {
        view.Reset();
        if (browser) {
            if (cookie) browser->Unadvise(cookie);
            browser->Destroy(); browser.Reset();
        }
        navigation.Reset();
        if (owner) DestroyWindow(owner);
    }
};
// Independent public native browser with only the documented pane service.
// It receives the same owned files but never receives the App's view site.
struct NativePaneReference {
    HWND owner = nullptr;
    ComPtr<IExplorerBrowser> browser;
    ComPtr<IShellView> view;
    ComPtr<IFolderView2> folderView;
    ComPtr<NativePaneSite> site;
    ComPtr<ShareSiteReference::Navigation> navigation;
    DWORD cookie = 0;
    HRESULT create(HINSTANCE instance, IShellItem* folder, bool preview) {
        const auto desktop = PrivateDesktop::current();
        if (!desktop || FAILED(desktop->verifyIsolation()) || !folder) return E_ACCESSDENIED;
        owner = CreateWindowExW(0, L"STATIC", L"Owned independent native pane reference", WS_OVERLAPPEDWINDOW,
            20, 20, 1000, 700, nullptr, nullptr, instance, nullptr);
        if (!owner) return HRESULT_FROM_WIN32(GetLastError());
        auto hr = CoCreateInstance(CLSID_ExplorerBrowser, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&browser));
        site = Microsoft::WRL::Make<NativePaneSite>();
        if (!site) return E_OUTOFMEMORY;
        site->preview = preview;
        site->trace = true;
        ComPtr<IObjectWithSite> located;
        if (SUCCEEDED(hr)) hr = browser.As(&located);
        if (SUCCEEDED(hr)) hr = located->SetSite(static_cast<IServiceProvider*>(site.Get()));
        if (SUCCEEDED(hr)) hr = browser->SetOptions(static_cast<EXPLORER_BROWSER_OPTIONS>(
            EBO_SHOWFRAMES | EBO_NOTRAVELLOG | EBO_NOPERSISTVIEWSTATE | EBO_NOBORDER));
        RECT bounds{}; GetClientRect(owner, &bounds);
        FOLDERSETTINGS settings{FVM_DETAILS, FWF_AUTOARRANGE};
        if (SUCCEEDED(hr)) hr = browser->Initialize(owner, &bounds, &settings);
        navigation = Microsoft::WRL::Make<ShareSiteReference::Navigation>();
        if (SUCCEEDED(hr)) hr = navigation ? browser->Advise(navigation.Get(), &cookie) : E_OUTOFMEMORY;
        if (SUCCEEDED(hr)) hr = browser->BrowseToObject(folder, SBSP_ABSOLUTE);
        if (SUCCEEDED(hr) && !pumpUntil([&] { return navigation->finished; }, 4000)) hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        if (SUCCEEDED(hr)) hr = navigation->status;
        if (SUCCEEDED(hr)) hr = browser->GetCurrentView(IID_PPV_ARGS(&view));
        if (SUCCEEDED(hr)) hr = view.As(&folderView);
        if (SUCCEEDED(hr) && !pumpUntil([&] { int count = 0; return SUCCEEDED(folderView->ItemCount(SVGIO_ALLVIEW, &count)) && count == 4; }, 1500))
            hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        return hr;
    }
    ~NativePaneReference() {
        folderView.Reset(); view.Reset();
        if (browser) {
            if (cookie) browser->Unadvise(cookie);
            browser->Destroy();
            ComPtr<IObjectWithSite> located;
            if (SUCCEEDED(browser.As(&located))) located->SetSite(nullptr);
            browser.Reset();
        }
        navigation.Reset(); site.Reset();
        if (owner) DestroyWindow(owner);
    }
};
using NativeFileIdentity = std::pair<ULONGLONG, std::array<BYTE, 16>>;
HRESULT nativeArrayIdentities(IShellItemArray* items, std::set<NativeFileIdentity>& identities) {
    identities.clear();
    if (!items) return E_POINTER;
    DWORD count = 0;
    auto hr = items->GetCount(&count);
    for (DWORD index = 0; SUCCEEDED(hr) && index < count; ++index) {
        ComPtr<IShellItem> item;
        hr = items->GetItemAt(index, &item);
        PWSTR path = nullptr;
        if (SUCCEEDED(hr)) hr = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
        FILE_ID_INFO identity{};
        if (SUCCEEDED(hr)) hr = nativeFileIdentity(path, identity);
        CoTaskMemFree(path);
        if (SUCCEEDED(hr)) {
            std::array<BYTE, 16> id{};
            std::copy(std::begin(identity.FileId.Identifier), std::end(identity.FileId.Identifier), id.begin());
            if (!identities.emplace(identity.VolumeSerialNumber, id).second) hr = E_UNEXPECTED;
        }
    }
    return hr;
}
AccessibleResult accessibleFileMenu(IUIAutomation* automation, IUIAutomationElement* scope) {
    AccessibleResult result;
    ComPtr<IUIAutomationTreeWalker> walker;
    if (!automation || !scope || FAILED(automation->get_ControlViewWalker(&walker))) return result;
    std::vector<ComPtr<IUIAutomationElement>> pending;
    ComPtr<IUIAutomationElement> initial;
    if (SUCCEEDED(walker->GetFirstChildElement(scope, &initial)) && initial) pending.push_back(initial);
    unsigned inspected = 0;
    const auto end = GetTickCount64() + 5000;
    while (!pending.empty() && inspected++ < 80 && GetTickCount64() < end) {
        auto element = std::move(pending.back()); pending.pop_back();
        BSTR name = nullptr; CONTROLTYPEID type = 0;
        element->get_CurrentName(&name); element->get_CurrentControlType(&type);
        const std::wstring observed = name ? name : L""; SysFreeString(name);
        if (!observed.empty()) result.detail += L"[" + observed + L";" + std::to_wstring(type) + L"] ";
        if (observed == L"File tab") {
            result.element = element;
            result.passed = type == UIA_ButtonControlTypeId;
            result.detail = L"Name=" + observed + L"; ControlType=" + std::to_wstring(type) +
                L"; native Ribbon ApplicationMenu provider";
            return result;
        }
        ComPtr<IUIAutomationElement> sibling;
        if (SUCCEEDED(walker->GetNextSiblingElement(element.Get(), &sibling)) && sibling) pending.push_back(sibling);
        // Never enumerate a folder's items merely to identify Ribbon chrome.
        if (type == UIA_ListControlTypeId || type == UIA_TreeControlTypeId || type == UIA_DataGridControlTypeId) continue;
        ComPtr<IUIAutomationElement> child;
        if (SUCCEEDED(walker->GetFirstChildElement(element.Get(), &child)) && child) pending.push_back(child);
    }
    result.detail = L"Native File menu was absent from bounded chrome traversal; " + result.detail;
    return result;
}

AccessibleResult accessibleExpandedMenu(IUIAutomation* automation, IUIAutomationElement* ribbon,
                                       HWND host, const wchar_t* name, int minimumRows = 1,
                                       const std::vector<std::wstring>& requiredRows = {}, bool galleryParentOnly = false,
                                       ULONGLONG deadline = 0, const std::atomic_bool* cancellation = nullptr,
                                       bool expandCollapseOnly = false) {
    AccessibleResult result;
    if (!automation || !ribbon || !name) { result.detail = L"Native Ribbon accessibility scope unavailable"; return result; }
    HRESULT budgetStatus = S_OK;
    const auto withinBudget = [&] {
        if (FAILED(budgetStatus)) return false;
        if (cancellation && cancellation->load(std::memory_order_relaxed))
            budgetStatus = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        else if (deadline && GetTickCount64() >= deadline)
            budgetStatus = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        result.budgetStatus = budgetStatus;
        return SUCCEEDED(budgetStatus);
    };
    // A UIA timeout limits one provider request, not a traversal. Recheck the
    // caller's common budget before and after every request; an overrun stays
    // a failure even if the native provider eventually returns valid data.
    const auto readNative = [&](const auto& request) -> HRESULT {
        if (!withinBudget()) return budgetStatus;
        const auto status = request();
        return withinBudget() ? status : budgetStatus;
    };
    VARIANT property{}; property.vt = VT_BSTR; property.bstrVal = SysAllocString(name);
    ComPtr<IUIAutomationCondition> condition;
    auto hr = property.bstrVal ? readNative([&] { return automation->CreatePropertyCondition(UIA_NamePropertyId, property, &condition); }) : E_OUTOFMEMORY;
    VariantClear(&property);
    ComPtr<IUIAutomationElementArray> candidates;
    if (SUCCEEDED(hr)) hr = readNative([&] { return ribbon->FindAll(TreeScope_Descendants, condition.Get(), &candidates); });
    int count = 0;
    if (SUCCEEDED(hr)) hr = candidates ? readNative([&] { return candidates->get_Length(&count); }) : E_UNEXPECTED;
    ComPtr<IUIAutomationExpandCollapsePattern> expand;
    ComPtr<IUIAutomationInvokePattern> invokeParent;
    const bool safeParentInvoke = !expandCollapseOnly && (wcscmp(name, L"Sort by") == 0 || wcscmp(name, L"Group by") == 0 ||
        wcscmp(name, L"Add columns") == 0 || wcscmp(name, L"Copy to") == 0);
    std::wstring diagnostics;
    for (int index = 0; SUCCEEDED(hr) && withinBudget() && index < std::min(count, 16); ++index) {
        ComPtr<IUIAutomationElement> candidate;
        if (FAILED(readNative([&] { return candidates->GetElement(index, &candidate); })) || !candidate) continue;
        BOOL enabled = FALSE;
        if (FAILED(readNative([&] { return candidate->get_CurrentIsEnabled(&enabled); })) || !enabled) continue;
        CONTROLTYPEID control = 0; BSTR id = nullptr, className = nullptr;
        readNative([&] { return candidate->get_CurrentControlType(&control); });
        readNative([&] { return candidate->get_CurrentAutomationId(&id); });
        readNative([&] { return candidate->get_CurrentClassName(&className); });
        diagnostics += L"; candidate type=" + std::to_wstring(control) + L" id=" + (id ? id : L"") +
            L" class=" + (className ? className : L"");
        SysFreeString(id); SysFreeString(className);
        if (control != UIA_ButtonControlTypeId && control != UIA_SplitButtonControlTypeId &&
            control != UIA_MenuItemControlTypeId && control != UIA_ComboBoxControlTypeId) continue;
        if (galleryParentOnly) {
            // An Items gallery's whole SplitButton is directly below its
            // Ribbon toolbar. Same-caption action/arrow descendants are not
            // eligible parent expansion targets.
            ComPtr<IUIAutomationTreeWalker> walker;
            ComPtr<IUIAutomationElement> parent;
            CONTROLTYPEID parentType = 0; BSTR parentName = nullptr;
            BOOL offscreen = TRUE; RECT bounds{}, parentBounds{}, ribbonBounds{};
            auto parentRead = readNative([&] { return automation->get_ControlViewWalker(&walker); });
            if (SUCCEEDED(parentRead)) parentRead = walker ? readNative([&] { return walker->GetParentElement(candidate.Get(), &parent); }) : E_UNEXPECTED;
            if (SUCCEEDED(parentRead)) parentRead = parent ? readNative([&] { return parent->get_CurrentControlType(&parentType); }) : E_UNEXPECTED;
            if (SUCCEEDED(parentRead)) parentRead = readNative([&] { return parent->get_CurrentName(&parentName); });
            if (SUCCEEDED(parentRead)) parentRead = readNative([&] { return parent->get_CurrentBoundingRectangle(&parentBounds); });
            if (SUCCEEDED(parentRead)) parentRead = readNative([&] { return candidate->get_CurrentBoundingRectangle(&bounds); });
            if (SUCCEEDED(parentRead)) parentRead = readNative([&] { return ribbon->get_CurrentBoundingRectangle(&ribbonBounds); });
            if (SUCCEEDED(parentRead)) parentRead = readNative([&] { return candidate->get_CurrentIsOffscreen(&offscreen); });
            const auto contains = [](const RECT& outer, const RECT& inner) {
                return inner.right > inner.left && inner.bottom > inner.top && inner.left >= outer.left && inner.top >= outer.top &&
                    inner.right <= outer.right && inner.bottom <= outer.bottom;
            };
            const bool wholeGallery = control == UIA_SplitButtonControlTypeId && SUCCEEDED(parentRead) &&
                parentType == UIA_ToolBarControlTypeId && (!parentName || wcscmp(parentName, name) != 0) && !offscreen &&
                contains(parentBounds, bounds) && contains(ribbonBounds, parentBounds);
            SysFreeString(parentName);
            if (!wholeGallery) continue;
        }
        if (wcscmp(name, L"Open") == 0 && control == UIA_SplitButtonControlTypeId) {
            // Windows exposes a split button's primary action and its arrow
            // as two same-name SplitButton siblings. The primary provider's
            // Expand can dispatch the default action. Require the narrow,
            // right-aligned arrow inside its actual same-name native Group.
            ComPtr<IUIAutomationTreeWalker> walker;
            ComPtr<IUIAutomationElement> parent;
            RECT bounds{}, parentBounds{}; CONTROLTYPEID parentType = 0;
            BSTR parentName = nullptr;
            const auto arrowRead = readNative([&] { return candidate->get_CurrentBoundingRectangle(&bounds); });
            auto parentRead = readNative([&] { return automation->get_ControlViewWalker(&walker); });
            if (SUCCEEDED(parentRead)) parentRead = walker ? readNative([&] { return walker->GetParentElement(candidate.Get(), &parent); }) : E_UNEXPECTED;
            if (SUCCEEDED(parentRead)) parentRead = parent ? readNative([&] { return parent->get_CurrentBoundingRectangle(&parentBounds); }) : E_UNEXPECTED;
            if (SUCCEEDED(parentRead)) parentRead = readNative([&] { return parent->get_CurrentControlType(&parentType); });
            if (SUCCEEDED(parentRead)) parentRead = readNative([&] { return parent->get_CurrentName(&parentName); });
            const auto maximumArrowWidth = MulDiv(24, static_cast<int>(GetDpiForWindow(host)), 96);
            const bool arrow = SUCCEEDED(arrowRead) && SUCCEEDED(parentRead) &&
                parentType == UIA_GroupControlTypeId && parentName && wcscmp(parentName, name) == 0 &&
                bounds.right > bounds.left && bounds.right - bounds.left <= maximumArrowWidth &&
                bounds.bottom > bounds.top && bounds.left > parentBounds.left &&
                bounds.right == parentBounds.right && bounds.top >= parentBounds.top && bounds.bottom <= parentBounds.bottom;
            SysFreeString(parentName);
            diagnostics += L"; split bounds=[" + std::to_wstring(bounds.left) + L"," + std::to_wstring(bounds.top) +
                L"," + std::to_wstring(bounds.right) + L"," + std::to_wstring(bounds.bottom) + L"]/right-arrow=" +
                std::to_wstring(arrow);
            if (!arrow) continue;
        }
        ComPtr<IUIAutomationExpandCollapsePattern> pattern;
        if (FAILED(readNative([&] { return candidate->GetCurrentPatternAs(UIA_ExpandCollapsePatternId, IID_PPV_ARGS(&pattern)); })) || !pattern) {
            if (safeParentInvoke) {
                ComPtr<IUIAutomationInvokePattern> parent;
                if (SUCCEEDED(readNative([&] { return candidate->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&parent)); })) && parent) {
                    result.element = candidate; invokeParent = parent; break;
                }
            }
            continue;
        }
        ExpandCollapseState state = ExpandCollapseState_LeafNode;
        if (SUCCEEDED(readNative([&] { return pattern->get_CurrentExpandCollapseState(&state); })) && state != ExpandCollapseState_LeafNode) {
            result.element = candidate; expand = pattern; break;
        }
    }
    if (!expand && !invokeParent) {
        result.detail = std::wstring(L"Native enabled expandable control=") + name + L" not found; candidates=" + std::to_wstring(count) +
            L"; " + hresultMessage(FAILED(budgetStatus) ? budgetStatus : hr) + diagnostics;
        return result;
    }
    // Expand/collapse only the native control on its owner's private desktop.
    // A small whitelist permits Invoke only for compiled dropdown parents
    // whose sole action opens a menu. Never Invoke Open's default action or a
    // menu leaf; those dispatch native filesystem/Shell commands.
    bool expansionAttempted = false;
    hr = readNative([&] {
        expansionAttempted = true;
        return expand ? expand->Expand() : invokeParent->Invoke();
    });
    ExpandCollapseState expanded = ExpandCollapseState_LeafNode;
    if (SUCCEEDED(hr) && expand) hr = readNative([&] { return expand->get_CurrentExpandCollapseState(&expanded); });
    VARIANT type{}; type.vt = VT_I4; type.lVal = UIA_MenuItemControlTypeId;
    ComPtr<IUIAutomationCondition> menus, lists, choices;
    if (SUCCEEDED(hr)) hr = readNative([&] { return automation->CreatePropertyCondition(UIA_ControlTypePropertyId, type, &menus); });
    type.lVal = UIA_ListItemControlTypeId;
    if (SUCCEEDED(hr)) hr = readNative([&] { return automation->CreatePropertyCondition(UIA_ControlTypePropertyId, type, &lists); });
    if (SUCCEEDED(hr)) hr = readNative([&] { return automation->CreateOrCondition(menus.Get(), lists.Get(), &choices); });
    ComPtr<IUIAutomationElementArray> children;
    if (SUCCEEDED(hr)) hr = readNative([&] { return ribbon->FindAll(TreeScope_Descendants, choices.Get(), &children); });
    int childCount = 0, visibleChildren = 0;
    if (SUCCEEDED(hr)) hr = children ? readNative([&] { return children->get_Length(&childCount); }) : E_UNEXPECTED;
    const auto end = GetTickCount64() + 2000;
    for (int index = 0; SUCCEEDED(hr) && withinBudget() && index < std::min(childCount, 128) && GetTickCount64() < end; ++index) {
        ComPtr<IUIAutomationElement> child;
        BOOL offscreen = TRUE; RECT bounds{};
        if (SUCCEEDED(readNative([&] { return children->GetElement(index, &child); })) && child &&
            SUCCEEDED(readNative([&] { return child->get_CurrentIsOffscreen(&offscreen); })) && !offscreen &&
            SUCCEEDED(readNative([&] { return child->get_CurrentBoundingRectangle(&bounds); })) && bounds.right > bounds.left && bounds.bottom > bounds.top)
            ++visibleChildren;
    }
    // Ribbon dropdowns are separate owned top-level HWNDs, not necessarily
    // accessibility descendants of their anchor. Inspect only such private
    // popups, never the desktop root or another process's windows.
    struct PopupEnumeration {
        HWND host;
        std::vector<HWND> windows;
        ULONGLONG deadline;
        const std::atomic_bool* cancellation;
        bool cleanup = false;
    } popups{host, {}, deadline, cancellation};
    const auto collectPopup = [](HWND popup, LPARAM context) -> BOOL {
        auto& result = *reinterpret_cast<PopupEnumeration*>(context);
        if (!result.cleanup && ((result.deadline && GetTickCount64() >= result.deadline) ||
            (result.cancellation && result.cancellation->load(std::memory_order_relaxed)))) return FALSE;
        DWORD process = 0; GetWindowThreadProcessId(popup, &process);
        if (process == GetCurrentProcessId() && popup != result.host && IsWindowVisible(popup) &&
            GetAncestor(popup, GA_ROOTOWNER) == result.host &&
            std::find(result.windows.begin(), result.windows.end(), popup) == result.windows.end()) result.windows.push_back(popup);
        return TRUE;
    };
    const auto popupDeadline = GetTickCount64() + 2000;
    do {
        if (!withinBudget()) break;
        popups.windows.clear();
        EnumThreadWindows(GetWindowThreadProcessId(host, nullptr), collectPopup, reinterpret_cast<LPARAM>(&popups));
        if (!withinBudget()) break;
        if (!popups.windows.empty()) break;
        Sleep(5); // The owner's STA is pumped by probeNativeMenus, not this worker.
    } while (SUCCEEDED(hr) && withinBudget() && GetTickCount64() < popupDeadline);
    std::wstring visibleStaticNames;
    std::vector<bool> matchedRows(requiredRows.size(),false);
    const auto anchorVisibleChildren = visibleChildren;
    const auto rowsDeadline = GetTickCount64() + 2000;
    do {
        if (!withinBudget()) break;
        visibleChildren = anchorVisibleChildren;
        visibleStaticNames.clear();
        std::fill(matchedRows.begin(),matchedRows.end(),false);
        for (const auto popup : popups.windows) {
            if (!withinBudget()) break;
            ComPtr<IUIAutomationElement> popupRoot;
            if (FAILED(readNative([&] { return automation->ElementFromHandle(popup, &popupRoot); })) || !popupRoot) continue;
            // Native command galleries expose action rows as Buttons as well as
            // MenuItems/ListItems; the separate popup ownership is essential.
            type.lVal = UIA_ButtonControlTypeId;
            ComPtr<IUIAutomationCondition> buttons, popupChoices;
            if (FAILED(readNative([&] { return automation->CreatePropertyCondition(UIA_ControlTypePropertyId, type, &buttons); })) ||
                FAILED(readNative([&] { return automation->CreateOrCondition(choices.Get(), buttons.Get(), &popupChoices); }))) continue;
            ComPtr<IUIAutomationElementArray> rows;
            if (FAILED(readNative([&] { return popupRoot->FindAll(TreeScope_Descendants, popupChoices.Get(), &rows); })) || !rows) continue;
            int rowsCount = 0; readNative([&] { return rows->get_Length(&rowsCount); });
            for (int index = 0; withinBudget() && index < std::min(rowsCount, 128); ++index) {
                ComPtr<IUIAutomationElement> row; BOOL offscreen = TRUE; RECT bounds{};
                if (FAILED(readNative([&] { return rows->GetElement(index, &row); })) || !row ||
                    FAILED(readNative([&] { return row->get_CurrentIsOffscreen(&offscreen); })) ||
                    offscreen || FAILED(readNative([&] { return row->get_CurrentBoundingRectangle(&bounds); })) ||
                    bounds.right <= bounds.left || bounds.bottom <= bounds.top) continue;
                ++visibleChildren;
                BSTR label = nullptr;
                if (SUCCEEDED(readNative([&] { return row->get_CurrentName(&label); })) && label) {
                    for (size_t required = 0; required < requiredRows.size(); ++required)
                        if (requiredRows[required] == label) matchedRows[required] = true;
                    for (const auto* allowed : {L"Name", L"Date modified", L"Type", L"Size", L"Ascending", L"Descending"})
                        if (wcscmp(label, allowed) == 0) visibleStaticNames += std::wstring(L"[") + allowed + L"]";
                }
                SysFreeString(label);
            }
        }
        if (visibleChildren >= minimumRows && std::all_of(matchedRows.begin(),matchedRows.end(),[](bool matched){return matched;})) break;
        if (!withinBudget()) break;
        Sleep(5); // Popup HWND creation can precede its native accessibility rows.
    } while (SUCCEEDED(hr) && withinBudget() && GetTickCount64() < rowsDeadline);
    const auto requiredMatched = static_cast<size_t>(std::count(matchedRows.begin(),matchedRows.end(),true));
    if (withinBudget() && (visibleChildren < minimumRows || requiredMatched != requiredRows.size())) {
        // Record only provider structure and numeric geometry. Native MRU or
        // application names are deliberately absent from these diagnostics.
        ComPtr<IUIAutomationCondition> everything;
        readNative([&] { return automation->CreateTrueCondition(&everything); });
        std::vector<HWND> diagnosticWindows{host};
        diagnosticWindows.insert(diagnosticWindows.end(), popups.windows.begin(), popups.windows.end());
        for (const auto popup : diagnosticWindows) {
            if (!withinBudget()) break;
            wchar_t className[80]{}; RECT popupBounds{};
            GetClassNameW(popup, className, static_cast<int>(std::size(className)));
            GetWindowRect(popup, &popupBounds);
            ComPtr<IUIAutomationElement> scope;
            const auto scopeRead = readNative([&] { return automation->ElementFromHandle(popup, &scope); });
            ComPtr<IUIAutomationElementArray> descendants;
            const auto descendantsRead = scope && everything ?
                readNative([&] { return scope->FindAll(TreeScope_Descendants, everything.Get(), &descendants); }) : scopeRead;
            int length = 0;
            if (descendants) readNative([&] { return descendants->get_Length(&length); });
            diagnostics += std::wstring(L"; popup structure class=") + className + L" bounds=[" +
                std::to_wstring(popupBounds.left) + L"," + std::to_wstring(popupBounds.top) + L"," +
                std::to_wstring(popupBounds.right) + L"," + std::to_wstring(popupBounds.bottom) +
                L"]/read=" + hresultMessage(descendantsRead) + L"/descendants=" + std::to_wstring(length);
            // Inspect native popup rows fully; the host inventory is bounded
            // to chrome and never records a Shell item's accessible name.
            for (int index = 0; descendants && withinBudget() && index < std::min(length, popup == host ? 12 : 32); ++index) {
                ComPtr<IUIAutomationElement> row; CONTROLTYPEID rowType = 0;
                BOOL offscreen = TRUE; RECT bounds{};
                if (FAILED(readNative([&] { return descendants->GetElement(index, &row); })) || !row) continue;
                readNative([&] { return row->get_CurrentControlType(&rowType); });
                readNative([&] { return row->get_CurrentIsOffscreen(&offscreen); });
                readNative([&] { return row->get_CurrentBoundingRectangle(&bounds); });
                diagnostics += L" {" + std::to_wstring(rowType) + L"/off=" + std::to_wstring(offscreen) +
                    L"/rect=" + std::to_wstring(bounds.left) + L"," + std::to_wstring(bounds.top) + L"," +
                    std::to_wstring(bounds.right) + L"," + std::to_wstring(bounds.bottom) + L"}";
            }
        }
    }
    // Expansion posts native popup work. Read state after those real rows
    // materialize, instead of racing the immediate Expand return.
    if (SUCCEEDED(hr) && expand) hr = readNative([&] { return expand->get_CurrentExpandCollapseState(&expanded); });
    // Once expansion was attempted, cancellation stops further inspection,
    // not cleanup. The owner keeps pumping and retains this native provider
    // until its Collapse and owned-popup cancellation have finished.
    const auto collapse = expansionAttempted ? (expand ? expand->Collapse() : invokeParent->Invoke()) : S_FALSE;
    ExpandCollapseState collapsed = ExpandCollapseState_LeafNode;
    auto collapsedRead = expand ? readNative([&] { return expand->get_CurrentExpandCollapseState(&collapsed); }) : S_OK;
    if (expansionAttempted) {
        // Expand can have returned after the work deadline while its popup
        // creation was still queued. Discover that actual owned window before
        // cancelling; never enumerate or dismiss another host's popup.
        popups.cleanup = true;
        EnumThreadWindows(GetWindowThreadProcessId(host, nullptr), collectPopup, reinterpret_cast<LPARAM>(&popups));
    }
    const auto ownedPopupVisible = [&](HWND popup) {
        DWORD process = 0;
        return IsWindow(popup) && GetWindowThreadProcessId(popup, &process) == GetWindowThreadProcessId(host, nullptr) &&
            process == GetCurrentProcessId() && GetAncestor(popup, GA_ROOTOWNER) == host && IsWindowVisible(popup);
    };
    for (const auto popup : popups.windows) if (ownedPopupVisible(popup)) PostMessageW(popup, WM_CANCELMODE, 0, 0);
    const auto closeDeadline = GetTickCount64() + 500;
    const bool cancelledExpansion = expansionAttempted && !withinBudget();
    bool popupsClosed = false;
    do {
        if (cancelledExpansion) {
            // Keep the existing bounded close-observation window when work
            // was cancelled, including a popup posted by a late Expand return.
            EnumThreadWindows(GetWindowThreadProcessId(host, nullptr), collectPopup, reinterpret_cast<LPARAM>(&popups));
            for (const auto popup : popups.windows) if (ownedPopupVisible(popup)) PostMessageW(popup, WM_CANCELMODE, 0, 0);
        }
        popupsClosed = std::none_of(popups.windows.begin(), popups.windows.end(), ownedPopupVisible);
        if (!popupsClosed || cancelledExpansion) Sleep(5);
    } while ((!popupsClosed || cancelledExpansion) && GetTickCount64() < closeDeadline);
    // Native Collapse also posts its final accessibility state change. Read
    // it after the owned popup has actually disappeared.
    if (expand && popupsClosed) collapsedRead = readNative([&] { return expand->get_CurrentExpandCollapseState(&collapsed); });
    result.passed = withinBudget() && SUCCEEDED(hr) && (invokeParent || expanded == ExpandCollapseState_Expanded) && visibleChildren >= minimumRows &&
        requiredMatched == requiredRows.size() &&
        SUCCEEDED(collapse) && SUCCEEDED(collapsedRead) && popupsClosed &&
        (invokeParent || collapsed == ExpandCollapseState_Collapsed);
    result.detail = std::wstring(L"Native dropdown=") + name + L"; expanded=" + std::to_wstring(expanded) +
        L"; visible menu/list items=" + std::to_wstring(visibleChildren) + L"; read=" + hresultMessage(hr) +
        L"; required native folder rows=" + std::to_wstring(requiredRows.size()) + L"; exact matches=" + std::to_wstring(requiredMatched) +
        L"; collapse=" + hresultMessage(collapse) + L"; collapsedState=" + std::to_wstring(collapsed) +
        L"; collapsedRead=" + hresultMessage(collapsedRead) + L"; owned popup count=" + std::to_wstring(popups.windows.size()) +
        L"; popups closed=" + std::to_wstring(popupsClosed ? 1 : 0) +
        L"; budget=" + hresultMessage(budgetStatus) + L"; static row names=" + visibleStaticNames + L"; leaf commands invoked=0" + diagnostics;
    return result;
}

struct OwnedOrderExpansion {
    bool expanded = false;
    HRESULT setup = E_PENDING, budget = E_PENDING, desktopRestored = E_PENDING;
    std::wstring detail;
};

OwnedOrderExpansion expandOwnedOrderDropdown(HWND host, HWND ribbonWindow, const std::wstring& label) {
    OwnedOrderExpansion rejected;
    const auto isolation = PrivateDesktop::current();
    bool inputVisible = true;
    const DWORD creator = GetCurrentThreadId(), process = GetCurrentProcessId();
    const auto desktop = GetThreadDesktop(creator);
    const auto ownedRibbon = [=] {
        DWORD hostProcess = 0, ribbonProcess = 0;
        wchar_t windowClass[64]{};
        return host && ribbonWindow && IsWindow(host) && IsWindow(ribbonWindow) && IsChild(host, ribbonWindow) &&
            GetWindowThreadProcessId(host, &hostProcess) == creator && hostProcess == process &&
            GetWindowThreadProcessId(ribbonWindow, &ribbonProcess) == creator && ribbonProcess == process &&
            GetClassNameW(ribbonWindow, windowClass, static_cast<int>(std::size(windowClass))) &&
            wcscmp(windowClass, L"UIRibbonCommandBar") == 0 && IsWindowVisible(host) && IsWindowVisible(ribbonWindow);
    };
    if (!isolation || FAILED(isolation->verifyIsolation()) ||
        FAILED(isolation->visibleWindowsOnInputDesktop(inputVisible)) || inputVisible || !ownedRibbon() || label.empty()) {
        rejected.setup = E_ACCESSDENIED;
        return rejected;
    }
    const auto deadline = GetTickCount64() + 4000;
    const auto cancelled = std::make_shared<std::atomic_bool>(false);
    std::promise<OwnedOrderExpansion> output;
    auto future = output.get_future();
    std::thread worker([desktop, host, ribbonWindow, label, deadline, cancelled, ownedRibbon, output = std::move(output)]() mutable {
        OwnedOrderExpansion result;
        const auto previous = GetThreadDesktop(GetCurrentThreadId());
        const bool attached = SetThreadDesktop(desktop) != FALSE;
        const auto attachError = attached ? ERROR_SUCCESS : GetLastError();
        const auto initialized = attached && GetThreadDesktop(GetCurrentThreadId()) == desktop ?
            CoInitializeEx(nullptr, COINIT_MULTITHREADED) : HRESULT_FROM_WIN32(attachError ? attachError : ERROR_ACCESS_DENIED);
        const auto callCancellation = SUCCEEDED(initialized) ? CoEnableCallCancellation(nullptr) : E_PENDING;
        try {
            // Only plain receipts cross the worker boundary. Every UIA object,
            // including the actual ExpandCollapse pattern, dies before COM.
            ComPtr<IUIAutomation> automation;
            ComPtr<IUIAutomation2> limits;
            ComPtr<IUIAutomationElement> ribbonRoot;
            auto read = initialized;
            if (SUCCEEDED(read)) read = callCancellation;
            if (SUCCEEDED(read) && (!ownedRibbon() || cancelled->load() || GetTickCount64() >= deadline)) read = E_ABORT;
            if (SUCCEEDED(read)) read = CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation));
            if (SUCCEEDED(read)) read = automation ? automation.As(&limits) : E_NOINTERFACE;
            if (SUCCEEDED(read)) read = limits ? limits->put_ConnectionTimeout(400) : E_NOINTERFACE;
            if (SUCCEEDED(read)) read = limits->put_TransactionTimeout(400);
            if (SUCCEEDED(read)) read = limits->put_AutoSetFocus(FALSE);
            if (SUCCEEDED(read)) read = automation->ElementFromHandle(ribbonWindow, &ribbonRoot);
            int actualProcess = 0;
            if (SUCCEEDED(read)) read = ribbonRoot ? ribbonRoot->get_CurrentProcessId(&actualProcess) : E_NOINTERFACE;
            if (SUCCEEDED(read) && (static_cast<DWORD>(actualProcess) != GetCurrentProcessId() || !ownedRibbon())) read = E_ACCESSDENIED;
            result.setup = read;
            if (SUCCEEDED(read)) {
                const auto actual = accessibleExpandedMenu(automation.Get(), ribbonRoot.Get(), host, label.c_str(),
                    1, {}, false, deadline, cancelled.get(), true);
                result.expanded = actual.passed;
                result.budget = actual.budgetStatus;
                result.detail = actual.detail;
            }
        } catch (const std::bad_alloc&) { result.setup = E_OUTOFMEMORY; }
          catch (...) { result.setup = E_FAIL; }
        if (SUCCEEDED(callCancellation)) CoDisableCallCancellation(nullptr);
        if (SUCCEEDED(initialized)) CoUninitialize();
        const bool restored = previous && SetThreadDesktop(previous) && GetThreadDesktop(GetCurrentThreadId()) == previous;
        result.desktopRestored = restored ? S_OK : E_ACCESSDENIED;
        if (!restored) {
            std::fprintf(stderr, "headless ordering UIA failed exact private worker desktop restoration\n"); std::fflush(stderr);
            if (!TerminateProcess(GetCurrentProcess(), 9)) std::_Exit(9);
            std::_Exit(9);
        }
        output.set_value(std::move(result));
    });
    const auto nativeThread = static_cast<HANDLE>(worker.native_handle());
    const auto ready = [&] { return WaitForSingleObject(nativeThread, 0) == WAIT_OBJECT_0 &&
        future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; };
    const bool completed = pumpUntil(ready, 4000);
    if (!completed) {
        cancelled->store(true);
        CoCancelCall(GetThreadId(nativeThread), 0);
        if (!pumpUntil(ready, 5000)) {
            std::fprintf(stderr, "headless ordering UIA did not join after cancellation; owned HWND/desktop retained\n"); std::fflush(stderr);
            if (!TerminateProcess(GetCurrentProcess(), 9)) std::_Exit(9);
            std::_Exit(9);
        }
    }
    worker.join();
    auto result = future.get();
    if (!completed) { result.expanded = false; result.budget = HRESULT_FROM_WIN32(ERROR_TIMEOUT); }
    inputVisible = true;
    if (!ownedRibbon() || FAILED(isolation->verifyIsolation()) ||
        FAILED(isolation->visibleWindowsOnInputDesktop(inputVisible)) || inputVisible) {
        result.expanded = false; result.setup = E_ACCESSDENIED;
    }
    return result;
}

}

int ExplorerApp::headlessSmoke(const std::filesystem::path& report, bool libraryOnly) {
    struct Check { std::string name; bool passed; std::wstring detail; ULONGLONG milliseconds; };
    std::vector<Check> checks;
    const auto started = GetTickCount64();
    auto check = [&](const char* name, bool passed, const std::wstring& detail = L"", ULONGLONG ms = 0) {
        checks.push_back({name, passed, detail, ms});
        std::cerr << "headless-check " << name << " passed=" << (passed ? "true" : "false")
                  << " elapsed_ms=" << GetTickCount64() - started;
        if (!passed && !detail.empty()) std::cerr << " detail=" << jsonString(detail);
        std::cerr << std::endl;
    };
    auto probeNativeMenus = [&](const std::vector<std::pair<const char*, const wchar_t*>>& requests, UINT tab = 0,
                                std::vector<std::wstring> requiredLibraryRows = {}) {
        auto collectionDiagnostic = [&](UINT command, const wchar_t* phase) {
            RibbonCollectionReadback state;
            const auto read = ribbon_.collectionReadback(command, state);
            return std::wstring(L"; ") + phase + L" collection callback " + std::to_wstring(command) + L"=" +
                hresultMessage(read) + L"/registered=" + std::to_wstring(state.registered) +
                L"/nativeType=" + std::to_wstring(state.nativeType) + L"/sourceRequests=" +
                std::to_wstring(state.sourceRequests) + L"/currentVariantType=" +
                std::to_wstring(state.currentVariantType) + L"/lastInvalidation=" + hresultMessage(state.lastInvalidation);
        };
        PrivatePresentation presentation(window_, true);
        const auto tabRead = tab ? ribbon_.selectTab(tab) : S_OK;
        const auto desktop = GetThreadDesktop(GetCurrentThreadId());
        HWND ribbonWindow = nullptr;
        EnumChildWindows(window_, [](HWND child, LPARAM context) -> BOOL {
            wchar_t name[64]{}; GetClassNameW(child, name, static_cast<int>(std::size(name)));
            if (_wcsicmp(name, L"UIRibbonCommandBar") == 0) { *reinterpret_cast<HWND*>(context) = child; return FALSE; }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ribbonWindow));
        std::wstring nativeMenuProperties = L"; requested tab=" + std::to_wstring(tab) + L"/" + hresultMessage(tabRead);
        ComPtr<IShellItemArray> nativeMenuSelection;
        DWORD nativeMenuSelectionCount = 0; SFGAOF nativeMenuAttributes = 0;
        auto nativeMenuSelectionRead = selection(nativeMenuSelection);
        if (SUCCEEDED(nativeMenuSelectionRead) && nativeMenuSelection)
            nativeMenuSelectionRead = nativeMenuSelection->GetCount(&nativeMenuSelectionCount);
        if (SUCCEEDED(nativeMenuSelectionRead) && nativeMenuSelectionCount)
            nativeMenuSelectionRead = nativeMenuSelection->GetAttributes(SIATTRIBFLAGS_AND,
                SFGAO_FILESYSTEM | SFGAO_FOLDER | SFGAO_LINK, &nativeMenuAttributes);
        std::wstring nativeMenuItemType;
        if (nativeMenuSelection && nativeMenuSelectionCount == 1) {
            ComPtr<IShellItem> item; ComPtr<IShellItem2> properties;
            PWSTR type = nullptr;
            if (SUCCEEDED(nativeMenuSelection->GetItemAt(0, &item)) && SUCCEEDED(item.As(&properties)) &&
                SUCCEEDED(properties->GetString(PKEY_ItemType, &type)) && type) nativeMenuItemType = type;
            CoTaskMemFree(type);
        }
        std::unique_ptr<NativeNamespaceCommandChildren> independentOpenWith;
        const auto independentOpenRead = namespaceActions_.queryCommandChildren(L"Windows.OpenWith", &independentOpenWith,
            NamespaceMenuScope::Selection);
        nativeMenuProperties += L"; selectedRead=" + hresultMessage(nativeMenuSelectionRead) +
            L"; actual selected=" + std::to_wstring(nativeMenuSelectionCount) +
            L"; cached selected=" + std::to_wstring(selectionCount_) + L"; attributes=" + std::to_wstring(nativeMenuAttributes) +
            L"; itemType=" + nativeMenuItemType + L"; stateDirty=" + std::to_wstring(selectionStateDirty_) +
            L"; namespaceDirty=" + std::to_wstring(namespaceDirty_) + L"; direct OpenWith=" + hresultMessage(independentOpenRead) +
            L"/" + std::to_wstring(independentOpenWith ? independentOpenWith->entries().size() : 0);
        const auto cachedOpenWith = commandCapabilities_.find(RibbonOpenWith);
        nativeMenuProperties += L"; navigating=" + std::to_wstring(navigating_) +
            L"; OpenWith cached capability=" + (cachedOpenWith == commandCapabilities_.end() ? L"absent" :
                hresultMessage(cachedOpenWith->second.status) + L"/enabled=" + std::to_wstring(cachedOpenWith->second.enabled));
        for (const UINT command : {static_cast<UINT>(RibbonOpenMenu), static_cast<UINT>(RibbonOpenWith),
                                  static_cast<UINT>(LibraryDefault)}) {
            const auto retained = ribbonCommandChildren_.find(command);
            nativeMenuProperties += L"; retained snapshot " + std::to_wstring(command) + L" present=" +
                std::to_wstring(retained != ribbonCommandChildren_.end()) + L"/object=" +
                std::to_wstring(retained != ribbonCommandChildren_.end() && retained->second != nullptr) + L"/count=" +
                std::to_wstring(retained != ribbonCommandChildren_.end() && retained->second ?
                    retained->second->entries().size() : 0);
            PROPVARIANT value{}; UINT count = 0;
            const auto nativeCommand = ribbon_.nativeCommandId(command);
            auto read = ribbon_.nativeFramework() ? ribbon_.nativeFramework()->GetUICommandProperty(
                nativeCommand, UI_PKEY_ItemsSource, &value) : E_UNEXPECTED;
            ComPtr<IUICollection> collection;
            if (SUCCEEDED(read)) read = value.vt == VT_UNKNOWN && value.punkVal ?
                value.punkVal->QueryInterface(IID_PPV_ARGS(&collection)) : E_NOINTERFACE;
            if (SUCCEEDED(read)) read = collection ? collection->GetCount(&count) : E_UNEXPECTED;
            PropVariantClear(&value);
            nativeMenuProperties += L"; native collection " + std::to_wstring(command) + L"/mapped=" +
                std::to_wstring(nativeCommand) + L"=" + hresultMessage(read) + L"/" + std::to_wstring(count);
            nativeMenuProperties += collectionDiagnostic(command, L"before expansion");
        }
        auto worker = std::async(std::launch::async, [desktop, ribbonWindow, requests, nativeMenuProperties, requiredLibraryRows,
                                                     host = window_, valid = presentation.ready && SUCCEEDED(tabRead)] {
            std::vector<Check> results;
            struct Apartment {
                HRESULT status = E_ACCESSDENIED;
                explicit Apartment(HDESK desktop) { if (SetThreadDesktop(desktop)) status = CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
                ~Apartment() { if (SUCCEEDED(status)) CoUninitialize(); }
            } apartment(desktop);
            ComPtr<IUIAutomation> automation;
            if (valid && SUCCEEDED(apartment.status)) CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation));
            ComPtr<IUIAutomation2> timeouts;
            if (automation && SUCCEEDED(automation.As(&timeouts))) {
                timeouts->put_ConnectionTimeout(1000); timeouts->put_TransactionTimeout(1000); timeouts->put_AutoSetFocus(FALSE);
            }
            ComPtr<IUIAutomationElement> root;
            if (automation && ribbonWindow) automation->ElementFromHandle(ribbonWindow, &root);
            for (const auto& request : requests) {
                std::cerr << "headless-menu-probe " << request.first << " begin" << std::endl;
                const auto minimumRows = std::string_view(request.first) == "native_stock_library_default_dropdown_hierarchy" ? 3 :
                    std::string_view(request.first) == "native_stock_library_optimize_dropdown_hierarchy" ? 5 : 1;
                const auto& requiredRows = std::string_view(request.first) == "native_stock_library_default_dropdown_hierarchy" ?
                    requiredLibraryRows : std::vector<std::wstring>{};
                const auto result = accessibleExpandedMenu(automation.Get(), root.Get(), host, request.second, minimumRows, requiredRows);
                const bool completeLibraryFixture = std::string_view(request.first) != "native_stock_library_default_dropdown_hierarchy" ||
                    (requiredRows.size() == 3 && std::none_of(requiredRows.begin(),requiredRows.end(),[](const auto& row){return row.empty();}));
                results.push_back({request.first, result.passed && completeLibraryFixture, result.detail + nativeMenuProperties, 0});
            }
            return results;
        });
        pumpUntil([&] { return worker.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; }, 12000);
        for (auto result : worker.get()) {
            if (result.name == "native_stock_open_with_dropdown_hierarchy" ||
                result.name == "native_stock_library_default_dropdown_hierarchy") {
                const auto command = result.name == "native_stock_open_with_dropdown_hierarchy" ?
                    static_cast<UINT>(RibbonOpenWith) : static_cast<UINT>(LibraryDefault);
                const auto retained = ribbonCommandChildren_.find(command);
                result.detail += L"; after expansion retained command " + std::to_wstring(command) + L" present=" +
                    std::to_wstring(retained != ribbonCommandChildren_.end()) + L"/object=" +
                    std::to_wstring(retained != ribbonCommandChildren_.end() && retained->second != nullptr) + L"/count=" +
                    std::to_wstring(retained != ribbonCommandChildren_.end() && retained->second ?
                        retained->second->entries().size() : 0);
                PROPVARIANT source{}; UINT count = 0;
                const auto nativeCommand = ribbon_.nativeCommandId(command);
                auto read = ribbon_.nativeFramework() ? ribbon_.nativeFramework()->GetUICommandProperty(
                    nativeCommand, UI_PKEY_ItemsSource, &source) : E_UNEXPECTED;
                ComPtr<IUICollection> collection;
                if (SUCCEEDED(read)) read = source.vt == VT_UNKNOWN && source.punkVal ?
                    source.punkVal->QueryInterface(IID_PPV_ARGS(&collection)) : E_NOINTERFACE;
                if (SUCCEEDED(read)) read = collection ? collection->GetCount(&count) : E_UNEXPECTED;
                PropVariantClear(&source);
                result.detail += L"; after expansion native command " + std::to_wstring(command) + L"/mapped=" +
                    std::to_wstring(nativeCommand) + L"=" + hresultMessage(read) + L"/" + std::to_wstring(count);
                result.detail += collectionDiagnostic(command, L"after expansion");
            }
            check(result.name.c_str(), result.passed, result.detail);
        }
    };
    // Keep the owned fixture beside the caller's report, normally the build or
    // artifacts directory. %TEMP% is below hidden AppData and cannot establish
    // expansion through visible native navigation-tree ancestors.
    const auto fixture = std::filesystem::absolute(report).parent_path() /
        (L"WindowsExplorer-smoke-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(started));
    auto atLocation = [&](const std::filesystem::path& path) {
        if(!currentPidl_)return false;
        ComPtr<IShellItem> current;
        auto hr=SHCreateItemFromIDList(currentPidl_.get(),IID_PPV_ARGS(&current));
        const auto actualPath=SUCCEEDED(hr)?itemName(current.Get(),SIGDN_FILESYSPATH):std::wstring{};
        if(actualPath.empty())return false;
        FILE_ID_INFO expectedIdentity{},actualIdentity{};
        hr=nativeFileIdentity(path,expectedIdentity);
        if(SUCCEEDED(hr))hr=nativeFileIdentity(actualPath,actualIdentity);
        return SUCCEEDED(hr)&&expectedIdentity.VolumeSerialNumber==actualIdentity.VolumeSerialNumber&&
            std::memcmp(expectedIdentity.FileId.Identifier,actualIdentity.FileId.Identifier,
                sizeof(expectedIdentity.FileId.Identifier))==0;
    };
    auto nativeBoolean = [&](UINT command, const PROPERTYKEY& key, bool& value) {
        if (!ribbon_.valid()) return false;
        const auto flush = ribbon_.flush();
        if (FAILED(flush)) {
            std::cerr << "headless-property command=" << command << " flush=" << static_cast<unsigned long>(flush) << std::endl;
            return false;
        }
        PROPVARIANT property{};
        const auto read = ribbon_.framework()->GetUICommandProperty(command, key, &property);
        BOOL flag = FALSE;
        const auto typed = SUCCEEDED(read) ? PropVariantToBoolean(property, &flag) : read;
        PropVariantClear(&property);
        if (FAILED(typed)) {
            std::cerr << "headless-property command=" << command << " property=" << key.pid
                      << " result=" << static_cast<unsigned long>(typed) << std::endl;
            return false;
        }
        value = flag != FALSE;
        return true;
    };
    auto commandEnabled = [&](UINT command) {
        bool value = false;
        return nativeBoolean(command, UI_PKEY_Enabled, value) && value;
    };
    auto commandDisabled = [&](UINT command) {
        bool value = true;
        return nativeBoolean(command, UI_PKEY_Enabled, value) && !value;
    };
    auto commandRegistered = [&](UINT command) {
        bool value = false;
        return nativeBoolean(command, UI_PKEY_Enabled, value);
    };
    auto commandChecked = [&](UINT command, bool expected) {
        bool value = !expected;
        return nativeBoolean(command, UI_PKEY_BooleanValue, value) && value == expected;
    };
    auto contextAvailable = [&](UINT command, bool expected) {
        if (!ribbon_.valid() || FAILED(ribbon_.flush())) return false;
        const auto context = command == RibbonSearchContext ? RibbonContext::Search :
            command == RibbonLibraryContext ? RibbonContext::Library : RibbonContext::None;
        if (context == RibbonContext::None) return false;
        UINT nativeIdentifier = 0, availability = UI_CONTEXTAVAILABILITY_NOTAVAILABLE;
        const auto read = ribbon_.contextAvailable(context, nativeIdentifier, availability);
        if (FAILED(read)) std::cerr << "headless-context logical=" << static_cast<UINT>(context)
            << " nativeId=" << nativeIdentifier << " status=" << static_cast<unsigned long>(read) << std::endl;
        return SUCCEEDED(read) && nativeIdentifier != 0 &&
            (availability != UI_CONTEXTAVAILABILITY_NOTAVAILABLE) == expected;
    };
    auto selectNativeTab = [&](UINT tab) {
        PrivatePresentation presentation(window_);
        if (!presentation.ready) return E_ACCESSDENIED;
        const auto hr = ribbon_.selectTab(tab);
        if (SUCCEEDED(hr)) ribbon_.flush();
        return hr;
    };
    std::error_code filesystemError;
    bool libraryFixtureOwned = false;
    auto runLibraryFixture = [&] {
        // This scope owns the genuine native Library fixture. Keep its slow
        // provider work separate from the general App checks.
        HRESULT hr = S_OK;
        bool ready = false;
        const auto libraryFolder = fixture / L"Unicode-\u65e5\u672c\u8a9e";
        const auto libraryMember = libraryFolder / L"library-member.txt";
        const std::array librarySourceFolders{libraryFolder, fixture / L"Subfolder" / L"Library location A",
            fixture / L"Subfolder" / L"Library location B"};
        std::filesystem::create_directories(fixture.parent_path());
        if (!std::filesystem::create_directory(fixture)) throw std::runtime_error("Library fixture already exists");
        libraryFixtureOwned = true;
        std::filesystem::create_directory(fixture / L"Subfolder");
        for (const auto& folder : librarySourceFolders) std::filesystem::create_directory(folder);
        {
            std::ofstream member(libraryMember, std::ios::binary);
            member << "owned library fixture";
            member.close();
            if (!member) throw std::runtime_error("Library member write failed");
        }
        struct LibrarySource {
            FILE_ID_INFO identity{};
            FILE_BASIC_INFO basic{};
            std::vector<char> bytes;
            HRESULT status = E_UNEXPECTED;
        };
        const auto readLibrarySource = [](const std::filesystem::path& path, bool readBytes) {
            LibrarySource result;
            struct Handle {
                HANDLE value = INVALID_HANDLE_VALUE;
                ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
            } handle{CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | (readBytes ? GENERIC_READ : 0),
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
            if (handle.value == INVALID_HANDLE_VALUE) {
                result.status = HRESULT_FROM_WIN32(GetLastError()); return result;
            }
            if (!GetFileInformationByHandleEx(handle.value, FileIdInfo, &result.identity, sizeof(result.identity)) ||
                !GetFileInformationByHandleEx(handle.value, FileBasicInfo, &result.basic, sizeof(result.basic))) {
                result.status = HRESULT_FROM_WIN32(GetLastError()); return result;
            }
            if (readBytes) {
                LARGE_INTEGER size{};
                if (!GetFileSizeEx(handle.value, &size)) {
                    result.status = HRESULT_FROM_WIN32(GetLastError()); return result;
                }
                if (size.QuadPart < 0 || size.QuadPart > 1024 * 1024) {
                    result.status = HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE); return result;
                }
                result.bytes.resize(static_cast<size_t>(size.QuadPart));
                DWORD read = 0;
                if (!ReadFile(handle.value, result.bytes.data(), static_cast<DWORD>(result.bytes.size()), &read, nullptr)) {
                    result.status = HRESULT_FROM_WIN32(GetLastError()); return result;
                }
                if (read != result.bytes.size()) { result.status = HRESULT_FROM_WIN32(ERROR_HANDLE_EOF); return result; }
            }
            result.status = S_OK; return result;
        };
        const auto sameLibraryIdentity = [](const FILE_ID_INFO& left, const FILE_ID_INFO& right) {
            return left.VolumeSerialNumber == right.VolumeSerialNumber &&
                std::memcmp(left.FileId.Identifier, right.FileId.Identifier, sizeof(left.FileId.Identifier)) == 0;
        };
        const auto libraryIdentity = [](const FILE_ID_INFO& value) {
            std::array<BYTE, 16> bytes{};
            std::copy(std::begin(value.FileId.Identifier), std::end(value.FileId.Identifier), bytes.begin());
            return NativeFileIdentity{value.VolumeSerialNumber, bytes};
        };
        const auto sameLibraryBasic = [](const FILE_BASIC_INFO& left, const FILE_BASIC_INFO& right, bool includeAccess = true) {
            return left.FileAttributes == right.FileAttributes && left.CreationTime.QuadPart == right.CreationTime.QuadPart &&
                left.LastWriteTime.QuadPart == right.LastWriteTime.QuadPart && left.ChangeTime.QuadPart == right.ChangeTime.QuadPart &&
                (!includeAccess || left.LastAccessTime.QuadPart == right.LastAccessTime.QuadPart);
        };
        std::array<LibrarySource, 3> libraryFoldersBefore;
        bool librarySourcesReady = true;
        for (size_t index = 0; index < librarySourceFolders.size(); ++index) {
            libraryFoldersBefore[index] = readLibrarySource(librarySourceFolders[index], false);
            librarySourcesReady = librarySourcesReady && libraryFoldersBefore[index].status == S_OK &&
                (libraryFoldersBefore[index].basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        }
        const auto libraryMemberBefore = readLibrarySource(libraryMember, true);
        librarySourcesReady = librarySourcesReady && libraryMemberBefore.status == S_OK &&
            std::string(libraryMemberBefore.bytes.begin(), libraryMemberBefore.bytes.end()) == "owned library fixture";
        check("library_owned_unicode_member_fixture", librarySourcesReady);
        if (!librarySourcesReady) throw std::runtime_error("Library source fixture unavailable");
        const bool libraryInitialReady = pumpUntil([&] { return currentPidl_ && folderView_ && !navigating_; }, 10000);
        check("initial_shell_navigation", libraryInitialReady);
        if (!libraryInitialReady) throw std::runtime_error("Initial Library host view unavailable");
        const auto libraryOriginNavigation = navigate(fixture.wstring());
        const bool libraryOriginReady = SUCCEEDED(libraryOriginNavigation) &&
            pumpUntil([&] { return !navigating_ && folderView_ && atLocation(fixture); }, 5000);
        check("library_owned_origin_navigation", libraryOriginReady, hresultMessage(libraryOriginNavigation));
        if (!libraryOriginReady) throw std::runtime_error("Library origin view unavailable");
        const auto libraryTimedCall = [&](const char* method, const auto& call) {
            const auto began = GetTickCount64();
            std::cerr << "headless-library-call " << method << " begin elapsed_ms=" << began - started << std::endl;
            const auto result = call();
            std::cerr << "headless-library-call " << method << " end hresult=" << static_cast<long>(result)
                << " elapsed_ms=" << GetTickCount64() - began << std::endl;
            return result;
        };
        struct LibraryHost {
            ComPtr<IShellView> view;
            ComPtr<IFolderView2> folder;
            Pidl location;
            std::vector<Pidl> history;
            int historyIndex = -1, selectedCount = -1, focusedIndex = -2;
            unsigned navigation = 0;
            UINT64 generation = 0;
            HRESULT status = E_UNEXPECTED, selectionRead = E_UNEXPECTED, focusRead = E_UNEXPECTED;
            std::set<NativeFileIdentity> selected;
            NativeFileIdentity focused{};
        };
        const auto captureLibraryHost = [&] {
            LibraryHost result;
            result.view = view_; result.folder = folderView_;
            result.location.reset(currentPidl_ ? ILCloneFull(currentPidl_.get()) : nullptr);
            result.historyIndex = historyIndex_; result.navigation = navigationCount_; result.generation = namespaceGeneration_;
            bool historyCopied = true;
            for (const auto& entry : history_) {
                result.history.emplace_back(entry ? ILCloneFull(entry.get()) : nullptr);
                historyCopied = historyCopied && (!entry || result.history.back());
            }
            if (!result.view || !result.folder || !result.location || !historyCopied) return result;
            result.status = result.folder->ItemCount(SVGIO_SELECTION, &result.selectedCount);
            ComPtr<IShellItemArray> selected;
            result.selectionRead = result.folder->GetSelection(FALSE, &selected);
            if (result.status == S_OK && result.selectedCount >= 0) {
                if (selected && (result.selectionRead == S_OK ||
                    (result.selectionRead == S_FALSE && result.selectedCount == 0))) {
                    result.status = nativeArrayIdentities(selected.Get(), result.selected);
                    if (result.status == S_OK && result.selected.size() != static_cast<size_t>(result.selectedCount))
                        result.status = E_UNEXPECTED;
                } else if (!(result.selectedCount == 0 && !selected &&
                    result.selectionRead == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))) result.status = E_UNEXPECTED;
            } else result.status = E_UNEXPECTED;
            result.focusRead = result.folder->GetFocusedItem(&result.focusedIndex);
            if (result.status == S_OK && result.focusRead == S_OK && result.focusedIndex >= 0) {
                ComPtr<IShellItem> focused;
                result.status = result.folder->GetItem(result.focusedIndex, IID_PPV_ARGS(&focused));
                FILE_ID_INFO identity{};
                if (result.status == S_OK && focused) result.status = nativeFileIdentity(itemName(focused.Get(), SIGDN_FILESYSPATH), identity);
                else if (result.status == S_OK) result.status = E_UNEXPECTED;
                if (result.status == S_OK) result.focused = libraryIdentity(identity);
            } else if (!(result.focusRead == S_FALSE && result.focusedIndex == -1)) result.status = E_UNEXPECTED;
            if (!currentPidl_ || ILGetSize(result.location.get()) != ILGetSize(currentPidl_.get()) ||
                std::memcmp(result.location.get(), currentPidl_.get(), ILGetSize(result.location.get())) != 0 ||
                result.view.Get() != view_.Get() || result.folder.Get() != folderView_.Get() ||
                result.navigation != navigationCount_ || result.generation != namespaceGeneration_ || navigating_ || closing_)
                result.status = HRESULT_FROM_WIN32(ERROR_RETRY);
            return result;
        };
        const auto sameLibraryHost = [&](const LibraryHost& before, bool preserveSelection = true) {
            const auto after = captureLibraryHost();
            bool sameHistory = before.historyIndex == after.historyIndex && before.history.size() == after.history.size();
            for (size_t index = 0; sameHistory && index < before.history.size(); ++index) {
                const auto& left = before.history[index]; const auto& right = after.history[index];
                sameHistory = static_cast<bool>(left) == static_cast<bool>(right) && (!left ||
                    (ILGetSize(left.get()) == ILGetSize(right.get()) &&
                     std::memcmp(left.get(), right.get(), ILGetSize(left.get())) == 0));
            }
            return before.status == S_OK && after.status == S_OK && sameHistory &&
                before.view.Get() == after.view.Get() && before.folder.Get() == after.folder.Get() &&
                before.navigation == after.navigation && (!preserveSelection ||
                    (before.generation == after.generation && before.selectionRead == after.selectionRead &&
                     before.selectedCount == after.selectedCount && before.selected == after.selected &&
                     before.focusRead == after.focusRead && before.focusedIndex == after.focusedIndex && before.focused == after.focused)) &&
                ILGetSize(before.location.get()) == ILGetSize(after.location.get()) &&
                std::memcmp(before.location.get(), after.location.get(), ILGetSize(before.location.get())) == 0 &&
                before.view.Get() == view_.Get() && before.folder.Get() == folderView_.Get() &&
                before.navigation == navigationCount_ && (!preserveSelection || before.generation == namespaceGeneration_) && !navigating_ && !closing_;
        };
        const auto libraryOrigin = captureLibraryHost();
        check("library_owned_origin_full_native_source_ready", libraryOrigin.status == S_OK, hresultMessage(libraryOrigin.status));
        if (libraryOrigin.status != S_OK) throw std::runtime_error("Library origin identity snapshot unavailable");
        const std::array additionalLibraryFolders{fixture / L"Subfolder" / L"Library location A",
            fixture / L"Subfolder" / L"Library location B"};
        for (const auto& folder : additionalLibraryFolders) std::filesystem::create_directories(folder);
        ShellLibrary fixtureLibrary;
        ComPtr<IShellItem> fixtureLibraryItem;
        hr = libraryTimedCall("Create", [&] { return ShellLibrary::create(fixtureLibrary); });
        if (SUCCEEDED(hr)) hr = libraryTimedCall("AddFolder.Unicode", [&] { return fixtureLibrary.addFolder(libraryFolder); });
        for (const auto& folder : additionalLibraryFolders)
            if (SUCCEEDED(hr)) hr = libraryTimedCall("AddFolder.Additional", [&] { return fixtureLibrary.addFolder(folder); });
        if (SUCCEEDED(hr)) hr = libraryTimedCall("Optimize.Documents", [&] { return fixtureLibrary.optimize(LibraryKind::Documents); });
        if (SUCCEEDED(hr)) hr = libraryTimedCall("SetDefaultSaveFolder", [&] { return fixtureLibrary.setDefaultSaveFolder(libraryFolder); });
        if (SUCCEEDED(hr)) hr = libraryTimedCall("Save", [&] { return fixtureLibrary.save(fixture / L"Subfolder", L"Fixture library", fixtureLibraryItem); });
        const auto libraryReleaseStarted = GetTickCount64();
        std::cerr << "headless-library-call Release begin" << std::endl;
        fixtureLibrary = ShellLibrary{};
        std::cerr << "headless-library-call Release end elapsed_ms=" << GetTickCount64() - libraryReleaseStarted
            << " native_hresult_unavailable=true" << std::endl;
        const bool libraryOriginPreserved = sameLibraryHost(libraryOrigin);
        check("library_native_construction_preserves_origin_view_history_and_selection", libraryOriginPreserved);
        if (!libraryOriginPreserved) throw std::runtime_error("Library origin changed during construction");
        const auto libraryReturnedDescriptorPath = itemName(fixtureLibraryItem.Get(), SIGDN_FILESYSPATH);
        const auto libraryDescriptorPath = fixture / L"Subfolder" / L"Fixture library.library-ms";
        FILE_ID_INFO libraryReturnedDescriptorIdentity{};
        const auto libraryReturnedDescriptorRead = libraryReturnedDescriptorPath.empty() ? E_UNEXPECTED :
            nativeFileIdentity(libraryReturnedDescriptorPath, libraryReturnedDescriptorIdentity);
        const auto libraryDescriptorBefore = readLibrarySource(libraryDescriptorPath, true);
        const bool libraryDescriptorReady = SUCCEEDED(hr) && libraryReturnedDescriptorRead == S_OK &&
            libraryDescriptorBefore.status == S_OK && !libraryDescriptorBefore.bytes.empty() &&
            sameLibraryIdentity(libraryReturnedDescriptorIdentity, libraryDescriptorBefore.identity) && sameLibraryHost(libraryOrigin);
        check("library_saved_native_descriptor_source_ready", libraryDescriptorReady,
            L"returned item=" + hresultMessage(libraryReturnedDescriptorRead) + L"; owned descriptor=" +
            hresultMessage(libraryDescriptorBefore.status));
        if (!libraryDescriptorReady) throw std::runtime_error("Library descriptor or retained origin unavailable");
        if (SUCCEEDED(hr)) hr = libraryTimedCall("BrowseToSavedLibrary", [&] { return browser_->BrowseToObject(fixtureLibraryItem.Get(), SBSP_ABSOLUTE); });
        const auto libraryLoadObservedStarted = GetTickCount64();
        ready = SUCCEEDED(hr) && pumpUntil([&] { return !navigating_ && library_.valid() && contextPage_ == ContextPage::Library; }, 5000);
        std::cerr << "headless-library-call LoadObserved host_ready=" << ready << " elapsed_ms="
            << GetTickCount64() - libraryLoadObservedStarted << " native_load_hresult_unavailable=true" << std::endl;
        check("library_context_in_hidden_host", ready && contextAvailable(RibbonLibraryContext, true) && commandRegistered(LibraryLocations) &&
            commandRegistered(LibraryDefault) && commandRegistered(LibraryOptimize), hresultMessage(hr));
        std::vector<LibraryFolder> includedFolders;
        std::filesystem::path defaultLibraryPath;
        std::error_code libraryIdentityError;
        check("library_context_reads_included_default_location", ready && SUCCEEDED(library_.folders(includedFolders)) &&
            includedFolders.size() == 3 && SUCCEEDED(library_.defaultSavePath(defaultLibraryPath)) &&
            std::filesystem::equivalent(defaultLibraryPath, libraryFolder, libraryIdentityError) && !libraryIdentityError);
        const auto libraryMenuHost = captureLibraryHost();
        if (ready && ribbon_.layout() == RibbonLayout::InstalledWindows10) {
            std::wstring defaultLabel, optimizeLabel;
            ribbon_.commandLabel(LibraryDefault, defaultLabel);
            ribbon_.commandLabel(RibbonLibraryOptimizeMenu, optimizeLabel);
            std::vector<std::wstring> requiredLibraryRows;
            for (const auto& folder : includedFolders) requiredLibraryRows.push_back(folder.displayName);
            probeNativeMenus({{"native_stock_library_default_dropdown_hierarchy", defaultLabel.c_str()},
                {"native_stock_library_optimize_dropdown_hierarchy", optimizeLabel.c_str()}}, RibbonLibraryTab, std::move(requiredLibraryRows));
            std::unique_ptr<NativeNamespaceCommandChildren> nativeSaveLocations;
            const auto nativeLocationsRead = backgroundActions_.queryCommandChildren(
                L"Windows.LibraryDefaultSaveLocation", &nativeSaveLocations, NamespaceMenuScope::Background);
            UINT expectedLocation = UI_COLLECTION_INVALIDINDEX, checkedLocations = 0;
            if (nativeSaveLocations) for (UINT index = 0; index < nativeSaveLocations->entries().size(); ++index) {
                const auto& entry = nativeSaveLocations->entries()[index];
                if (SUCCEEDED(entry.stateStatus) && !(entry.state & ECS_HIDDEN) && (entry.state & ECS_CHECKED)) {
                    expectedLocation = index; ++checkedLocations;
                }
            }
            PROPVARIANT selectedLocation{}; ULONG actualLocation = UI_COLLECTION_INVALIDINDEX;
            auto selectedLocationRead = ribbon_.framework()->GetUICommandProperty(
                LibraryDefault, UI_PKEY_SelectedItem, &selectedLocation);
            if (SUCCEEDED(selectedLocationRead)) selectedLocationRead = PropVariantToUInt32(selectedLocation, &actualLocation);
            PropVariantClear(&selectedLocation);
            check("native_stock_library_save_location_tracks_actual_checked_child", SUCCEEDED(nativeLocationsRead) &&
                nativeSaveLocations && nativeSaveLocations->entries().size() == 3 && checkedLocations == 1 &&
                SUCCEEDED(selectedLocationRead) && actualLocation == expectedLocation,
                L"Native current save-location children=" + std::to_wstring(nativeSaveLocations ?
                    nativeSaveLocations->entries().size() : 0) + L"; checked=" + std::to_wstring(checkedLocations) +
                L"; selected=" + std::to_wstring(actualLocation) + L"; expected=" + std::to_wstring(expectedLocation) +
                L"; provider=" + hresultMessage(nativeLocationsRead) + L"; selectedRead=" + hresultMessage(selectedLocationRead));
        }
        const auto libraryMenuHostAfter = captureLibraryHost();
        const bool libraryMenuSourcePreserved = ready && sameLibraryHost(libraryMenuHost, false);
        check("library_native_context_and_menus_preserve_owned_view_and_history", libraryMenuSourcePreserved,
            L"selection before/after=" + std::to_wstring(libraryMenuHost.selectedCount) + L"/" +
            std::to_wstring(libraryMenuHostAfter.selectedCount) + L"; focused before/after=" +
            std::to_wstring(libraryMenuHost.focusedIndex) + L"/" + std::to_wstring(libraryMenuHostAfter.focusedIndex) +
            L"; generation before/after=" + std::to_wstring(libraryMenuHost.generation) + L"/" +
            std::to_wstring(libraryMenuHostAfter.generation));
        if (!libraryMenuSourcePreserved) throw std::runtime_error("Library context source changed during menu readback");
        const auto librariesNavigation = libraryTimedCall("BrowseToLibrariesRoot", [&] { return navigate(L"shell:Libraries"); });
        const bool librariesFactoryReady = SUCCEEDED(librariesNavigation) && pumpUntil([&] {
            return !navigating_ && librariesRoot_ && nativeLibraryFactoryReady_ && newItemTypes_ &&
                newItemTypes_->entries().size() == 1;
        }, 5000);
        const auto nativeFactoryState = ribbonState(NewFolder);
        const auto nativeFactoryLabel = newItemTypes_ && newItemTypes_->entries().size() == 1 ?
            newItemTypes_->entries().front().label : std::wstring{};
        const auto factoryHostGuard = execute(NewFolder);
        const auto factoryRibbonGuard = executeRibbon(NewFolder);
        check("libraries_root_retains_native_factory_and_blocks_headless_creation", librariesFactoryReady &&
            nativeFactoryState.enabled && !nativeFactoryLabel.empty() && nativeFactoryState.label == nativeFactoryLabel &&
            factoryHostGuard == E_ACCESSDENIED && factoryRibbonGuard == E_ACCESSDENIED,
            L"navigate=" + hresultMessage(librariesNavigation) + L"; factory ready=" + std::to_wstring(nativeLibraryFactoryReady_) +
            L"; native children=" + std::to_wstring(newItemTypes_ ? newItemTypes_->entries().size() : 0) +
            L"; factory label=" + nativeFactoryLabel + L"; host guard=" + hresultMessage(factoryHostGuard) +
            L"; Ribbon guard=" + hresultMessage(factoryRibbonGuard));
        libraryTimedCall("BrowseToOwnedOrigin", [&] { return navigate(fixture.wstring()); });
        ready = pumpUntil([&] { return !navigating_ && atLocation(fixture); }, 5000);
        check("library_context_removed_after_navigation", ready && !library_.valid() && contextPage_ == ContextPage::None &&
            contextAvailable(RibbonLibraryContext, false));

        bool libraryMembersExact = includedFolders.size() == librarySourceFolders.size();
        std::set<NativeFileIdentity> expectedLibraryFolders, actualLibraryFolders;
        for (const auto& source : libraryFoldersBefore) expectedLibraryFolders.insert(libraryIdentity(source.identity));
        for (const auto& folder : includedFolders) {
            FILE_ID_INFO identity{};
            const auto read = folder.item ? nativeFileIdentity(itemName(folder.item.Get(), SIGDN_FILESYSPATH), identity) : E_POINTER;
            libraryMembersExact = libraryMembersExact && read == S_OK;
            if (read == S_OK) libraryMembersExact = actualLibraryFolders.insert(libraryIdentity(identity)).second && libraryMembersExact;
        }
        bool libraryFoldersPreserved = true;
        std::wstring librarySourceDetail;
        for (size_t index = 0; index < librarySourceFolders.size(); ++index) {
            const auto after = readLibrarySource(librarySourceFolders[index], false);
            const auto& before = libraryFoldersBefore[index];
            const bool sameId = after.status == S_OK && sameLibraryIdentity(before.identity, after.identity);
            libraryFoldersPreserved = libraryFoldersPreserved && sameId &&
                before.basic.FileAttributes == after.basic.FileAttributes &&
                before.basic.CreationTime.QuadPart == after.basic.CreationTime.QuadPart &&
                before.basic.LastWriteTime.QuadPart == after.basic.LastWriteTime.QuadPart;
            librarySourceDetail += L"; folder " + std::to_wstring(index) + L" HR/id=" + hresultMessage(after.status) +
                L"/" + std::to_wstring(sameId) + L"; attributes before/after=" + std::to_wstring(before.basic.FileAttributes) +
                L"/" + std::to_wstring(after.basic.FileAttributes) + L"; creation before/after=" +
                std::to_wstring(before.basic.CreationTime.QuadPart) + L"/" + std::to_wstring(after.basic.CreationTime.QuadPart) +
                L"; write before/after=" + std::to_wstring(before.basic.LastWriteTime.QuadPart) + L"/" +
                std::to_wstring(after.basic.LastWriteTime.QuadPart) + L"; change before/after=" +
                std::to_wstring(before.basic.ChangeTime.QuadPart) + L"/" + std::to_wstring(after.basic.ChangeTime.QuadPart) +
                L"; access before/after=" + std::to_wstring(before.basic.LastAccessTime.QuadPart) + L"/" +
                std::to_wstring(after.basic.LastAccessTime.QuadPart);
        }
        const auto libraryMemberAfter = readLibrarySource(libraryMember, true);
        const bool libraryMemberPreserved = libraryMemberAfter.status == S_OK &&
            sameLibraryIdentity(libraryMemberBefore.identity, libraryMemberAfter.identity) &&
            // ReadFile can update access time, including after handle close.
            // Preserve write metadata strictly and report access separately.
            sameLibraryBasic(libraryMemberBefore.basic, libraryMemberAfter.basic, false) &&
            libraryMemberBefore.bytes == libraryMemberAfter.bytes;
        // Descriptor snapshots allow native writes/deletes while opening and
        // are sampled evidence; they are not a write-denying lease.
        const auto libraryDescriptorAfter = readLibrarySource(libraryDescriptorPath, true);
        const bool descriptorComparison = libraryDescriptorBefore.status == S_OK && libraryDescriptorAfter.status == S_OK;
        librarySourceDetail += L"; member HR/fullID/bytes/attributes-create-write-change=" + hresultMessage(libraryMemberAfter.status) + L"/" +
            std::to_wstring(libraryMemberAfter.status == S_OK && sameLibraryIdentity(libraryMemberBefore.identity, libraryMemberAfter.identity)) +
            L"/" + std::to_wstring(libraryMemberBefore.bytes == libraryMemberAfter.bytes) + L"/" +
            std::to_wstring(sameLibraryBasic(libraryMemberBefore.basic, libraryMemberAfter.basic, false)) +
            L"; member access before/after=" + std::to_wstring(libraryMemberBefore.basic.LastAccessTime.QuadPart) + L"/" +
            std::to_wstring(libraryMemberAfter.basic.LastAccessTime.QuadPart) +
            L"; descriptor sampled HR before/after=" + hresultMessage(libraryDescriptorBefore.status) + L"/" +
            hresultMessage(libraryDescriptorAfter.status) + L"; descriptor fullID/bytes/basic unchanged=" +
            std::to_wstring(descriptorComparison && sameLibraryIdentity(libraryDescriptorBefore.identity, libraryDescriptorAfter.identity)) +
            L"/" + std::to_wstring(descriptorComparison && libraryDescriptorBefore.bytes == libraryDescriptorAfter.bytes) +
            L"/" + std::to_wstring(descriptorComparison && sameLibraryBasic(libraryDescriptorBefore.basic, libraryDescriptorAfter.basic)) +
            L"; descriptor byte counts before/after=" + std::to_wstring(libraryDescriptorBefore.bytes.size()) + L"/" +
            std::to_wstring(libraryDescriptorAfter.bytes.size());
        check("library_native_members_and_owned_unicode_source_preserved",
            libraryMembersExact && expectedLibraryFolders.size() == 3 && actualLibraryFolders == expectedLibraryFolders &&
            libraryFoldersPreserved && libraryMemberPreserved && descriptorComparison && ready && atLocation(fixture),
            librarySourceDetail);
        std::cerr << "headless-library-source " << jsonString(librarySourceDetail) << std::endl;
    };

    try {
        check("host_stays_hidden", !IsWindowVisible(window_));
        if (libraryOnly) {
            runLibraryFixture();
        } else {
        bool chromeControlsReady = true;
        constexpr std::array<UINT, 4> navigationCommands{Back, Forward, HistoryMenu, Up};
        constexpr std::array<int, 4> navigationWidths{30, 30, 16, 24};
        const auto chromeDpi = GetDpiForWindow(nav_);
        for (size_t index = 0; index < navigationCommands.size(); ++index) {
            TBBUTTONINFOW button{sizeof(button)};
            button.dwMask = TBIF_SIZE;
            chromeControlsReady = chromeControlsReady && SendMessageW(nav_, TB_GETBUTTONINFOW,
                navigationCommands[index], reinterpret_cast<LPARAM>(&button)) >= 0 &&
                button.cx == MulDiv(navigationWidths[index], static_cast<int>(chromeDpi), 96);
        }
        chromeControlsReady = chromeControlsReady &&
            LOWORD(SendMessageW(search_, EM_GETMARGINS, 0, 0)) == MulDiv(40, static_cast<int>(chromeDpi), 96) &&
            GetPropW(addressActions_, L"WindowsExplorer.AddressActionBorder") != nullptr;
        for (const auto control : {breadcrumbs_, address_, search_})
            chromeControlsReady = chromeControlsReady && (GetWindowLongPtrW(control, GWL_STYLE) & WS_BORDER) != 0;
        check("production_chrome_initializes_actual_navigation_search_and_address_controls", chromeControlsReady);
        check("chrome_rejects_missing_mandatory_control_before_mutation",
            applyChrome(nullptr, breadcrumbs_, address_, search_, addressActions_) == E_INVALIDARG &&
            applyChrome(nav_, nullptr, address_, search_, addressActions_) == E_INVALIDARG &&
            applyChrome(nav_, breadcrumbs_, nullptr, search_, addressActions_) == E_INVALIDARG &&
            applyChrome(nav_, breadcrumbs_, address_, nullptr, addressActions_) == E_INVALIDARG);
        check("initial_shell_navigation", pumpUntil([&] { return currentPidl_ && folderView_ && !navigating_; }, 10000));
        EXPLORER_BROWSER_OPTIONS browserOptions = EBO_NONE;
        check("headless_disables_view_persistence", SUCCEEDED(browser_->GetOptions(&browserOptions)) &&
            (browserOptions & EBO_NOPERSISTVIEWSTATE));
        std::vector<UINT> toolbarCommands;
        bool toolbarBelow = true;
        const bool toolbarDefaults = SUCCEEDED(ribbon_.quickAccessCommands(toolbarCommands)) &&
            toolbarCommands == std::vector<UINT>{Properties, NewFolder} && SUCCEEDED(ribbon_.quickAccessBelow(toolbarBelow));
        check("quick_access_toolbar_defaults", toolbarDefaults && !toolbarBelow && !quickAccessModel_.belowRibbon());
        // Verify order through the native framework's QAT command collection.
        quickAccessModel_.add(Copy, 0);
        quickAccessModel_.move(NewFolder, 0);
        rebuildQuickAccess();
        bool toolbarOrder = SUCCEEDED(ribbon_.quickAccessCommands(toolbarCommands)) &&
            toolbarCommands.size() == quickAccessModel_.commands().size();
        for (size_t i = 0; i < quickAccessModel_.commands().size(); ++i) {
            toolbarOrder = toolbarOrder && toolbarCommands[i] == static_cast<UINT>(quickAccessModel_.commands()[i]);
        }
        check("quick_access_toolbar_custom_order", toolbarOrder);
        quickAccessModel_.add(PermanentDelete); rebuildQuickAccess();
        check("quick_access_permanent_delete_requires_selection", commandDisabled(PermanentDelete));
        execute(QuickAccessPlacement);
        check("quick_access_toolbar_below_ribbon", SUCCEEDED(ribbon_.quickAccessBelow(toolbarBelow)) && toolbarBelow);
        execute(QuickAccessReset);
        check("quick_access_toolbar_reset_above", !quickAccessModel_.belowRibbon() &&
            SUCCEEDED(ribbon_.quickAccessBelow(toolbarBelow)) && !toolbarBelow &&
            SUCCEEDED(ribbon_.quickAccessCommands(toolbarCommands)) && toolbarCommands == std::vector<UINT>{Properties, NewFolder} &&
            quickAccessModel_.commands() == std::vector<Command>{Properties, NewFolder});
        std::filesystem::create_directories(fixture / L"Subfolder");
        std::filesystem::create_directories(fixture / L"Subfolder" / L"One-time expansion" / L"Deep");
        std::filesystem::create_directories(fixture / L"Unicode-\u65e5\u672c\u8a9e");
        for (int i = 0; i < 1000; ++i) {
            std::ofstream stream(fixture / (L"file-" + std::to_wstring(i) + L".txt"));
            stream << "headless fixture " << i;
        }
        std::ofstream(fixture / L"hidden.txt") << "hidden";
        SetFileAttributesW((fixture / L"hidden.txt").c_str(), FILE_ATTRIBUTE_HIDDEN);
        std::ofstream(fixture / L"protected.txt") << "protected";
        SetFileAttributesW((fixture / L"protected.txt").c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
        ComPtr<IShellView> supersededNativeView = view_;
        const auto navigation = navigate(fixture.wstring());
        auto ready = pumpUntil([&] {
            int count = 0;
            return !navigating_ && folderView_ && atLocation(fixture) &&
                   SUCCEEDED(folderView_->ItemCount(SVGIO_ALLVIEW, &count)) && count >= 1002;
        }, 15000);
        int count = -1;
        if (folderView_) folderView_->ItemCount(SVGIO_ALLVIEW, &count);
        check("enumerate_1000_files", SUCCEEDED(navigation) && ready && count == 1002,
              L"Visible items: " + std::to_wstring(count) + L"; actual=" + currentLocation_ + L"; expected=" + fixture.wstring(), lastNavigationMs_);
        check("owned_navigation_matches_exact_native_folder_identity_and_rejects_sibling",
            ready && atLocation(fixture) && !atLocation(fixture / L"Subfolder"));
        check("breadcrumbs_and_history", breadcrumbsPidls_.size() >= 2 && historyIndex_ >= 1);
        check("native_view_interface", folderView_ && view_);
        if(ready&&view_&&supersededNativeView) {
            ComPtr<IUnknown> previousIdentity,activeIdentity;
            const bool distinct=SUCCEEDED(supersededNativeView.As(&previousIdentity))&&
                SUCCEEDED(view_.As(&activeIdentity))&&previousIdentity.Get()!=activeIdentity.Get();
            check("state_notification_fixture_retains_distinct_actual_views",distinct);
            if(distinct) {
                // Exercise the actual browser callback with the real retained
                // old view and current view. The transaction observes deferred
                // work without dispatching or replacing any native provider.
                CommandRefreshScope transaction(*this);
                const auto selectionDirty=selectionStateDirty_,namespaceDirty=namespaceDirty_;
                const auto queued=deferredUpdateQueued_,pending=commandRefreshPending_;
                const auto generation=namespaceGeneration_;
                const auto originalCurrent=headlessCurrentViewStateEvents_;
                const auto originalStale=headlessStaleViewStateEvents_;
                bool ignored=true;
                for(const ULONG change:{CDBOSC_SELCHANGE,CDBOSC_RENAME,CDBOSC_STATECHANGE}) {
                    ignored=OnStateChange(supersededNativeView.Get(),change)==S_OK&&ignored;
                    ignored=selectionStateDirty_==selectionDirty&&namespaceDirty_==namespaceDirty&&
                        deferredUpdateQueued_==queued&&commandRefreshPending_==pending&&
                        namespaceGeneration_==generation&&headlessStaleViewStateEvents_[change]==originalStale[change]+1&&
                        headlessCurrentViewStateEvents_[change]==originalCurrent[change]&&ignored;
                }
                check("superseded_native_view_notifications_preserve_command_generation",ignored);
                const auto staleBeforeNull=headlessStaleViewStateEvents_[CDBOSC_SELCHANGE];
                check("missing_native_view_notification_preserves_command_generation",OnStateChange(nullptr,CDBOSC_SELCHANGE)==S_OK&&
                    headlessStaleViewStateEvents_[CDBOSC_SELCHANGE]==staleBeforeNull+1&&
                    selectionStateDirty_==selectionDirty&&namespaceDirty_==namespaceDirty&&
                    deferredUpdateQueued_==queued&&commandRefreshPending_==pending&&namespaceGeneration_==generation);
                bool focusIgnored=true;
                for(const ULONG change:{CDBOSC_SETFOCUS,CDBOSC_KILLFOCUS}) {
                    const auto before=headlessCurrentViewStateEvents_[change];
                    focusIgnored=OnStateChange(view_.Get(),change)==S_OK&&headlessCurrentViewStateEvents_[change]==before+1&&
                        selectionStateDirty_==selectionDirty&&namespaceDirty_==namespaceDirty&&
                        deferredUpdateQueued_==queued&&commandRefreshPending_==pending&&namespaceGeneration_==generation&&focusIgnored;
                }
                check("current_native_view_focus_notifications_preserve_command_generation",focusIgnored);
                bool currentAccepted=true;
                for(const ULONG change:{CDBOSC_SELCHANGE,CDBOSC_RENAME,CDBOSC_STATECHANGE}) {
                    const auto before=headlessCurrentViewStateEvents_[change];
                    currentAccepted=OnStateChange(view_.Get(),change)==S_OK&&headlessCurrentViewStateEvents_[change]==before+1&&
                        selectionStateDirty_&&(change==CDBOSC_SELCHANGE||namespaceDirty_)&&commandRefreshPending_&&
                        namespaceGeneration_==generation&&currentAccepted;
                }
                check("current_native_view_notifications_request_real_command_refresh",currentAccepted);
            }
        }
        supersededNativeView.Reset();
        if(ready&&view_&&folderView_) {
            // Navigation completion alone does not activate the native view
            // or establish its complete selection/data-object readback.
            ComPtr<IShellView> originalView=view_;
            ComPtr<IFolderView2> originalFolderView=folderView_;
            PrivatePresentation selectionPresentation(window_,true);
            const auto activation=selectionPresentation.ready?originalView->UIActivate(SVUIA_ACTIVATE_NOFOCUS):E_ACCESSDENIED;
            const auto deadline=GetTickCount64()+10000;
            const auto waitForSelection=[&](const std::function<bool()>& predicate) {
                const auto now=GetTickCount64();
                return now<deadline&&pumpUntil(predicate,static_cast<DWORD>(deadline-now));
            };
            struct ChildIds {
                std::vector<Pidl> owned;
                std::vector<PCUITEMID_CHILD> children;
            } identities;
            HRESULT prepared=activation;
            std::array<NativeFileIdentity,3> expectedIds{};
            for(const auto* name:{L"file-0.txt",L"file-1.txt",L"file-2.txt"}) {
                if(FAILED(prepared))break;
                ComPtr<IShellItem> item;PIDLIST_ABSOLUTE raw=nullptr;
                auto hr=SHCreateItemFromParsingName((fixture/name).c_str(),nullptr,IID_PPV_ARGS(&item));
                if(SUCCEEDED(hr))hr=SHGetIDListFromObject(item.Get(),&raw);
                Pidl id(raw);
                FILE_ID_INFO fileId{};
                if(SUCCEEDED(hr))hr=nativeFileIdentity(fixture/name,fileId);
                if(FAILED(hr)||!id){prepared=FAILED(hr)?hr:E_UNEXPECTED;break;}
                auto& expected=expectedIds[identities.owned.size()];expected.first=fileId.VolumeSerialNumber;
                std::copy(std::begin(fileId.FileId.Identifier),std::end(fileId.FileId.Identifier),expected.second.begin());
                identities.children.push_back(ILFindLastID(id.get()));identities.owned.push_back(std::move(id));
            }
            const auto selectPair=[&](size_t tail)->HRESULT {
                if(FAILED(prepared)||identities.children.size()!=3)return FAILED(prepared)?prepared:E_UNEXPECTED;
                if(view_.Get()!=originalView.Get()||folderView_.Get()!=originalFolderView.Get())return E_ABORT;
                auto hr=originalView->SelectItem(nullptr,SVSI_DESELECTOTHERS);
                std::array<PCUITEMID_CHILD,2> pair{identities.children[0],identities.children[tail]};
                if(SUCCEEDED(hr))hr=originalFolderView->SelectAndPositionItems(static_cast<UINT>(pair.size()),pair.data(),nullptr,SVSI_SELECT|SVSI_NOTAKEFOCUS);
                if(SUCCEEDED(hr))hr=OnStateChange(originalView.Get(),CDBOSC_SELCHANGE);
                return hr;
            };
            HRESULT selectionRead=E_PENDING,countRead=E_PENDING,identityRead=E_PENDING;
            HRESULT attributesRead=E_PENDING,dataBind=E_PENDING,dataRead=E_PENDING;
            DWORD nativeCount=0;UINT cidaCount=0;SFGAOF nativeAttributes=0;
            const auto readPair=[&](size_t tail,ComPtr<IShellItemArray>& items,std::set<NativeFileIdentity>& ids) {
                countRead=identityRead=attributesRead=dataBind=dataRead=E_PENDING;nativeCount=cidaCount=0;nativeAttributes=0;
                selectionRead=originalFolderView->GetSelection(FALSE,&items);
                if(FAILED(selectionRead)||!items)return false;
                countRead=items->GetCount(&nativeCount);
                if(FAILED(countRead)||nativeCount!=2)return false;
                identityRead=nativeArrayIdentities(items.Get(),ids);
                if(FAILED(identityRead)||ids!=std::set<NativeFileIdentity>{expectedIds[0],expectedIds[tail]})return false;
                constexpr SFGAOF attributes=SFGAO_FILESYSTEM|SFGAO_HIDDEN|SFGAO_CANCOPY|SFGAO_CANMOVE|
                    SFGAO_CANDELETE|SFGAO_CANRENAME|SFGAO_HASPROPSHEET|SFGAO_FOLDER|SFGAO_LINK;
                attributesRead=items->GetAttributes(static_cast<SIATTRIBFLAGS>(SIATTRIBFLAGS_AND|SIATTRIBFLAGS_ALLITEMS),attributes,&nativeAttributes);
                if(FAILED(attributesRead))return false;
                ComPtr<IDataObject> data;dataBind=items->BindToHandler(nullptr,BHID_DataObject,IID_PPV_ARGS(&data));
                if(FAILED(dataBind)||!data)return false;
                const auto format=RegisterClipboardFormatW(CFSTR_SHELLIDLIST);
                if(!format){dataRead=HRESULT_FROM_WIN32(GetLastError());return false;}
                FORMATETC request{static_cast<CLIPFORMAT>(format),nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};
                struct Medium{STGMEDIUM value{};~Medium(){if(value.tymed)ReleaseStgMedium(&value);}}medium;
                dataRead=data->GetData(&request,&medium.value);
                if(FAILED(dataRead))return false;
                if(medium.value.tymed!=TYMED_HGLOBAL||!medium.value.hGlobal||GlobalSize(medium.value.hGlobal)<sizeof(UINT)) {
                    dataRead=HRESULT_FROM_WIN32(ERROR_INVALID_DATA);return false;
                }
                const auto bytes=GlobalLock(medium.value.hGlobal);
                if(!bytes){const auto error=GetLastError();dataRead=error?HRESULT_FROM_WIN32(error):E_OUTOFMEMORY;return false;}
                std::memcpy(&cidaCount,bytes,sizeof(cidaCount));GlobalUnlock(medium.value.hGlobal);
                return cidaCount==nativeCount&&view_.Get()==originalView.Get()&&folderView_.Get()==originalFolderView.Get();
            };
            const auto selectionDetail=[&] {
                auto detail=L"; activation="+hresultMessage(activation)+L"; preparation="+hresultMessage(prepared)+
                    L"; selection="+hresultMessage(selectionRead)+L"; count="+hresultMessage(countRead)+L"/"+std::to_wstring(nativeCount)+
                    L"; FileIDs="+hresultMessage(identityRead)+L"; attributes="+hresultMessage(attributesRead)+L"/"+std::to_wstring(nativeAttributes)+
                    L"; data bind="+hresultMessage(dataBind)+L"; CIDA="+hresultMessage(dataRead)+L"/"+std::to_wstring(cidaCount)+
                    L"; cached count="+std::to_wstring(selectionCount_)+L"; cached identities="+
                    std::to_wstring(commandSelectionIdentities_?commandSelectionIdentities_->size():0)+L"; dirty="+
                    std::to_wstring(selectionStateDirty_)+L"/"+std::to_wstring(namespaceDirty_)+L"; common deadline="+
                    hresultMessage(GetTickCount64()>=deadline?HRESULT_FROM_WIN32(ERROR_TIMEOUT):S_OK);
                for(const auto& [id,capability]:commandCapabilities_)if(!capability.selectionVerbs.empty())
                    detail+=L"; native command "+std::to_wstring(id)+L"="+hresultMessage(capability.status)+
                        L"/slow completed="+std::to_wstring(capability.slowStateCompleted);
                return detail;
            };
            const auto selected=selectPair(1);
            ComPtr<IShellItemArray> firstSelection;std::set<NativeFileIdentity> firstIds,changedIds;
            bool snapshotReady=false,workerCompleted=false,workerInFlight=false;
            const bool actualWorkerStarted=SUCCEEDED(selected)&&waitForSelection([&] {
                if(!snapshotReady) {
                    if(!readPair(1,firstSelection,firstIds))return false;
                    OnStateChange(originalView.Get(),CDBOSC_SELCHANGE);updateCommands();
                    snapshotReady=selectionCount_==2&&!selectionStateDirty_&&!namespaceDirty_&&commandSelectionIdentities_&&
                        commandSelectionIdentities_->size()==2&&commandSelectionView_==originalView.Get();
                }
                if(!snapshotReady)return false;
                startPendingCommandStates();
                if(!selectionStateBatch_||selectionStateDirty_||namespaceDirty_)return false;
                // Record observed completion for diagnostics; both completed
                // and in-flight native batches must retain their generation.
                workerCompleted=selectionStateBatch_->completed();
                workerInFlight=!workerCompleted;
                return true;
            });
            const auto generation=namespaceGeneration_;
            const auto batch=selectionStateBatch_.get();
            const auto reusedBefore=headlessEquivalentSelectionRefreshes_;
            bool retained=actualWorkerStarted&&batch;
            HRESULT retainedRead=E_PENDING;
            for(unsigned duplicate=0;duplicate<3&&retained;++duplicate) {
                retained=OnStateChange(originalView.Get(),CDBOSC_SELCHANGE)==S_OK;
                updateCommands();
                retained=retained&&namespaceGeneration_==generation&&selectionStateBatch_.get()==batch&&
                    !selectionStateDirty_&&!namespaceDirty_&&commandSelectionView_==originalView.Get()&&view_.Get()==originalView.Get();
                std::vector<NamespaceSelectionVerbState> states;
                if(retained)retainedRead=batch->pollSelectionVerbBatch(&states);
                retained=retained&&retainedRead!=HRESULT_FROM_WIN32(ERROR_CANCELLED);
            }
            check("identical_complete_selection_retains_actual_native_batch_generation",snapshotReady&&actualWorkerStarted&&
                retained&&headlessEquivalentSelectionRefreshes_>=reusedBefore+3,
                L"snapshot/actual worker/retained="+std::to_wstring(snapshotReady)+L"/"+std::to_wstring(actualWorkerStarted)+L"/"+
                std::to_wstring(retained)+L"; identity reuse delta="+std::to_wstring(headlessEquivalentSelectionRefreshes_-reusedBefore)+
                L"; select="+hresultMessage(selected)+L"; worker initially completed/in flight="+std::to_wstring(workerCompleted)+L"/"+
                std::to_wstring(workerInFlight)+
                L"; retained poll="+hresultMessage(retainedRead)+selectionDetail());
            auto firstRead=firstSelection?S_OK:E_PENDING;
            if(SUCCEEDED(firstRead))firstRead=nativeArrayIdentities(firstSelection.Get(),firstIds);
            const auto changed=selectPair(2);
            ComPtr<IShellItemArray> changedSelection;
            const bool changedReady=SUCCEEDED(changed)&&waitForSelection([&] {
                if(!readPair(2,changedSelection,changedIds))return false;
                OnStateChange(originalView.Get(),CDBOSC_SELCHANGE);updateCommands();
                return selectionCount_==2&&!selectionStateDirty_&&!namespaceDirty_&&namespaceGeneration_>generation&&
                    commandSelectionIdentities_&&commandSelectionIdentities_->size()==2&&commandSelectionView_==originalView.Get();
            });
            auto changedRead=changedSelection?S_OK:E_PENDING;
            if(SUCCEEDED(changedRead))changedRead=nativeArrayIdentities(changedSelection.Get(),changedIds);
            const auto expectedIdentity=[&](const wchar_t* name) {
                FILE_ID_INFO info{};std::array<BYTE,16> id{};
                const auto hr=nativeFileIdentity(fixture/name,info);
                std::copy(std::begin(info.FileId.Identifier),std::end(info.FileId.Identifier),id.begin());
                return std::pair{hr,NativeFileIdentity{info.VolumeSerialNumber,id}};
            };
            const auto [headRead,headId]=expectedIdentity(L"file-0.txt");
            const auto [tailRead,tailId]=expectedIdentity(L"file-2.txt");
            check("same_count_changed_native_tail_rebuilds_command_generation",SUCCEEDED(firstRead)&&SUCCEEDED(changed)&&
                SUCCEEDED(changedRead)&&SUCCEEDED(headRead)&&SUCCEEDED(tailRead)&&SUCCEEDED(prepared)&&changedReady&&
                firstIds.size()==2&&changedIds.size()==2&&firstIds!=changedIds&&changedIds==std::set<NativeFileIdentity>{headId,tailId}&&
                headId==expectedIds[0]&&tailId==expectedIds[2]&&selectionCount_==2&&
                namespaceGeneration_>generation&&commandSelectionIdentities_&&commandSelectionIdentities_->size()==2,
                L"first FileIDs="+hresultMessage(firstRead)+L"/"+std::to_wstring(firstIds.size())+L"; select changed="+hresultMessage(changed)+
                L"; changed FileIDs="+hresultMessage(changedRead)+L"/"+std::to_wstring(changedIds.size())+L"; changed ready="+
                std::to_wstring(changedReady)+L"; expected head/tail="+hresultMessage(headRead)+L"/"+hresultMessage(tailRead)+
                L"; generation="+std::to_wstring(generation)+L"/"+std::to_wstring(namespaceGeneration_)+selectionDetail());
            bool metadataRefresh=true;
            for(const ULONG change:{CDBOSC_RENAME,CDBOSC_STATECHANGE}) {
                const auto before=namespaceGeneration_;
                metadataRefresh=OnStateChange(view_.Get(),change)==S_OK&&namespaceDirty_&&metadataRefresh;
                updateCommands();
                metadataRefresh=namespaceGeneration_>before&&selectionCount_==2&&metadataRefresh;
            }
            check("same_native_selection_rename_and_state_notifications_refresh_metadata",metadataRefresh);
            const auto cleared=view_->SelectItem(nullptr,SVSI_DESELECTOTHERS);
            if(SUCCEEDED(cleared))OnStateChange(view_.Get(),CDBOSC_SELCHANGE);
            updateCommands();
            check("selection_identity_fixture_restores_actual_empty_selection",SUCCEEDED(cleared)&&selectionCount_==0);
        }
        if (ready) {
            const bool originalFullPathTitle = fullPathTitle_;
            fullPathTitle_ = true;
            updateFrameTitle();
            check("full_path_title_owned_directory", physicalDirectory_ && textOf(window_) == fixture.wstring(),
                L"Owned native physical directory caption uses its complete path");
            fullPathTitle_ = originalFullPathTitle;
            updateFrameTitle();
            {
                unsigned probeCalls = 0;
                PrivatePresentation selectionPresentation(window_,true);
                check("native_command_reentry_presentation_isolated",selectionPresentation.ready);
                HRESULT nestedStateRead = E_PENDING;
                const auto deferredBefore = headlessDeferredCommandUpdates();
                PITEMID_CHILD selectedChild = nullptr;
                auto realSelection = folderView_->Item(3,&selectedChild);
                if(SUCCEEDED(realSelection)&&selectedChild)
                    realSelection = view_->SelectItem(selectedChild,
                        SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_NOTAKEFOCUS);
                else if(SUCCEEDED(realSelection))realSelection=E_UNEXPECTED;
                CoTaskMemFree(selectedChild);
                headlessCommandReentryProbe_ = [&] {
                    ++probeCalls;
                    updateCommands(); updateNamespace(); pollCommandStates(); cancelCommandStates();
                    nestedStateRead = OnStateChange(view_.Get(), CDBOSC_SELCHANGE);
                };
                struct ClearReentryProbe {
                    std::function<void()>& probe;
                    ~ClearReentryProbe() { probe = {}; }
                } clearProbe{headlessCommandReentryProbe_};
                selectionStateDirty_ = namespaceDirty_ = true;
                updateCommands();
                int actualSelected = -1;
                HRESULT cachedCopyRead = E_PENDING;
                bool cachedCopyEnabled = false;
                const bool settled = SUCCEEDED(realSelection) && pumpUntil([&] {
                    const auto found = commandCapabilities_.find(Copy);
                    cachedCopyRead = found == commandCapabilities_.end() ? E_PENDING : found->second.status;
                    cachedCopyEnabled = found != commandCapabilities_.end() && found->second.enabled;
                    return SUCCEEDED(folderView_->ItemCount(SVGIO_SELECTION, &actualSelected)) && actualSelected == 1 &&
                        !selectionStateDirty_ && !namespaceDirty_ && !commandRefreshActive_ && !commandRefreshPending_ &&
                        !commandStatesCancelPending_ && !commandItemsRefreshPending_ &&
                        SUCCEEDED(cachedCopyRead) && cachedCopyEnabled;
                }, 5000);
                ComPtr<IShellItemArray> actualItems;
                SFGAOF actualAttributes = 0;
                DWORD actualArrayCount = 0;
                auto actualAttributesRead = selection(actualItems);
                if (SUCCEEDED(actualAttributesRead) && actualItems)
                    actualAttributesRead = actualItems->GetCount(&actualArrayCount);
                if (SUCCEEDED(actualAttributesRead) && actualItems)
                    actualAttributesRead = actualItems->GetAttributes(
                        static_cast<SIATTRIBFLAGS>(SIATTRIBFLAGS_AND | SIATTRIBFLAGS_ALLITEMS),
                        SFGAO_CANCOPY, &actualAttributes);
                const bool providerCopyEligible = SUCCEEDED(actualAttributesRead) && actualArrayCount == 1 &&
                    (actualAttributes & SFGAO_CANCOPY) && namespaceActions_.facts().selectionCount == 1 &&
                    SUCCEEDED(namespaceActions_.facts().nativeAttributesStatus) &&
                    (commandContext().selectionAttributes & SFGAO_CANCOPY);
                const bool deferred = headlessDeferredCommandUpdates() > deferredBefore;
                const bool consumedOnce = probeCalls == 1 && !headlessCommandReentryProbe_;
                const auto remainingGuardFlags = (selectionStateDirty_ ? 1u : 0u) | (namespaceDirty_ ? 2u : 0u) |
                    (commandRefreshActive_ ? 4u : 0u) | (commandRefreshPending_ ? 8u : 0u) |
                    (commandStatesCancelPending_ ? 16u : 0u) | (commandItemsRefreshPending_ ? 32u : 0u);
                const auto deferredAfter = headlessDeferredCommandUpdates();
                headlessCommandReentryProbe_ = {};
                const auto cleared = view_->SelectItem(nullptr, SVSI_DESELECTOTHERS);
                const bool restoredSelection = SUCCEEDED(cleared) && pumpUntil([&] {
                    int selectedCount = -1;
                    return SUCCEEDED(folderView_->ItemCount(SVGIO_SELECTION, &selectedCount)) && selectedCount == 0 &&
                        !selectionStateDirty_ && !namespaceDirty_ && !commandRefreshActive_ && !commandRefreshPending_ &&
                        !commandStatesCancelPending_ && !commandItemsRefreshPending_;
                }, 5000);
                std::fprintf(stderr, "headless-reentry calls=%u deferred_before=%llu deferred_after=%llu nested=0x%08lX selected=%d array_count=%lu attrs=0x%08lX attrs_hr=0x%08lX copy_hr=0x%08lX copy_enabled=%u guard_flags=%u settled=%u restored=%u provider_copy=%u\n",
                    probeCalls, deferredBefore, deferredAfter, static_cast<unsigned long>(nestedStateRead), actualSelected,
                    static_cast<unsigned long>(actualArrayCount), static_cast<unsigned long>(actualAttributes),
                    static_cast<unsigned long>(actualAttributesRead), static_cast<unsigned long>(cachedCopyRead),
                    cachedCopyEnabled ? 1u : 0u, remainingGuardFlags, settled ? 1u : 0u,
                    restoredSelection ? 1u : 0u, providerCopyEligible ? 1u : 0u);
                std::fflush(stderr);
                check("native_command_refresh_defers_reentrant_provider_callbacks", settled && consumedOnce && deferred &&
                    SUCCEEDED(nestedStateRead) && providerCopyEligible && restoredSelection,
                    L"Probe calls=" + std::to_wstring(probeCalls) + L"; deferred=" + std::to_wstring(deferred ? 1 : 0) +
                    L"; actual selection=" + std::to_wstring(actualSelected) + L"; full native attrs=" +
                    hresultMessage(actualAttributesRead) + L"; cached Copy=" + hresultMessage(cachedCopyRead) +
                    L"/" + std::to_wstring(cachedCopyEnabled ? 1 : 0) + L"; settled/restored=" +
                    std::to_wstring(settled ? 1 : 0) + L"/" + std::to_wstring(restoredSelection ? 1 : 0));
            }
            {
                PrivatePresentation presentation(window_,true);
                check("accessibility_phase_on_private_desktop", presentation.ready);
                const auto originalSearchWidth = preferences_.searchWidth;
                auto clientBoundsOf = [&](HWND control) {
                    RECT bounds{};
                    GetWindowRect(control, &bounds);
                    MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&bounds), 2);
                    return bounds;
                };
                const auto searchBefore = clientBoundsOf(search_);
                const auto addressBefore = clientBoundsOf(address_);
                bool splitterResized = false;
                std::wstring splitterDiagnostic;
                if (presentation.ready) {
                    // Send events only to the owned private HWND. This tests the
                    // actual message handler without desktop input injection.
                    const int x = searchBefore.left - px(4);
                    const int y = (searchBefore.top + searchBefore.bottom) / 2;
                    SendMessageW(window_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
                    const bool captured = searchResizing_ && GetCapture() == window_;
                    SendMessageW(window_, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x - px(40), y));
                    const auto searchAfter = clientBoundsOf(search_);
                    const auto addressAfter = clientBoundsOf(address_);
                    SendMessageW(window_, WM_LBUTTONUP, 0, MAKELPARAM(x - px(40), y));
                    splitterResized = captured && !searchResizing_ && GetCapture() != window_ &&
                        preferences_.searchWidth == originalSearchWidth + 40 &&
                        searchAfter.right == searchBefore.right &&
                        searchAfter.right - searchAfter.left == searchBefore.right - searchBefore.left + px(40) &&
                        addressAfter.right - addressAfter.left == addressBefore.right - addressBefore.left - px(40);
                    splitterDiagnostic = L"captured=" + std::to_wstring(captured ? 1 : 0) +
                        L"; preference delta=" + std::to_wstring(preferences_.searchWidth - originalSearchWidth) +
                        L"; search width delta=" + std::to_wstring((searchAfter.right - searchAfter.left) - (searchBefore.right - searchBefore.left)) +
                        L"; search right delta=" + std::to_wstring(searchAfter.right - searchBefore.right) +
                        L"; address width delta=" + std::to_wstring((addressAfter.right - addressAfter.left) - (addressBefore.right - addressBefore.left));
                }
                preferences_.searchWidth = originalSearchWidth;
                layout();
                check("native_search_splitter_resizes_actual_fields", splitterResized, splitterDiagnostic);
                HWND accessibilityFolderWindow = nullptr;
                if (view_) view_->GetWindow(&accessibilityFolderWindow);
                UiString accessibleAddressName, accessibleSearchName;
                const auto addressNameRead = loadUiString(UiText::AddressBar, &accessibleAddressName);
                const auto searchNameRead = loadUiString(UiText::SearchBox, &accessibleSearchName);
                check("native_accessibility_host_resource_names", SUCCEEDED(addressNameRead) && SUCCEEDED(searchNameRead),
                    hresultMessage(addressNameRead) + L"; " + hresultMessage(searchNameRead));
                const auto accessibilityDesktop = GetThreadDesktop(GetCurrentThreadId());
                auto accessibility = std::async(std::launch::async, [&, accessibilityFolderWindow, accessibilityDesktop,
                    addressCaption = accessibleAddressName.text, searchCaption = accessibleSearchName.text] {
                std::vector<Check> items;
                auto accessibleCheckResult = [&](const char* test, bool passed, const std::wstring& detail = L"") {
                    items.push_back({test, passed, detail, 0});
                };
                // A UIA client inspecting its own windows must use a windowless
                // MTA worker while the owner STA dispatches Windows messages.
                // Attach the worker to the same never-switched private desktop
                // before COM creates anything; never inspect desktop-root UI.
                struct AutomationApartment {
                    HRESULT result = E_ACCESSDENIED;
                    explicit AutomationApartment(HDESK desktop) {
                        if (SetThreadDesktop(desktop)) result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                    }
                    ~AutomationApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
                } apartment(accessibilityDesktop);
                ComPtr<IUIAutomation> automation;
                const auto automationResult = SUCCEEDED(apartment.result) ?
                    CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)) : apartment.result;
                ComPtr<IUIAutomation2> automationTimeouts;
                if (automation && SUCCEEDED(automation.As(&automationTimeouts))) {
                    automationTimeouts->put_ConnectionTimeout(3000);
                    automationTimeouts->put_TransactionTimeout(3000);
                    automationTimeouts->put_AutoSetFocus(FALSE);
                }
                auto accessibleHost = accessibleElement(automation.Get(), nullptr, window_, nullptr, UIA_WindowControlTypeId);
                accessibleCheckResult("native_accessibility_host", SUCCEEDED(automationResult) && accessibleHost.passed, accessibleHost.detail);
                HWND nativeRibbonWindow = nullptr;
                EnumChildWindows(window_, [](HWND child, LPARAM context) -> BOOL {
                    wchar_t className[128]{};
                    GetClassNameW(child, className, static_cast<int>(std::size(className)));
                    if (_wcsicmp(className, L"UIRibbonCommandBar") != 0) return TRUE;
                    *reinterpret_cast<HWND*>(context) = child;
                    return FALSE;
                }, reinterpret_cast<LPARAM>(&nativeRibbonWindow));
                ComPtr<IUIAutomationElement> accessibleRibbon;
                if (automation && nativeRibbonWindow) automation->ElementFromHandle(nativeRibbonWindow, &accessibleRibbon);
                auto accessibleCheck = [&](const char* test, IUIAutomationElement* scope, HWND handle, const wchar_t* name, CONTROLTYPEID type) {
                    auto result = accessibleElement(automation.Get(), scope, handle, name, type);
                    accessibleCheckResult(test, result.passed, result.detail);
                    return result.element;
                };
                for (const auto& tab : std::array<std::pair<const char*, const wchar_t*>, 3>{{
                         {"native_accessibility_home_tab", L"Home"}, {"native_accessibility_share_tab", L"Share"},
                         {"native_accessibility_view_tab", L"View"}}})
                    accessibleCheck(tab.first, accessibleRibbon.Get(), nullptr, tab.second, UIA_TabItemControlTypeId);
                auto accessibleFile = accessibleFileMenu(automation.Get(), accessibleRibbon.Get());
                accessibleCheckResult("native_accessibility_file_menu", accessibleFile.passed, accessibleFile.detail);
                // Native QAT realization can finish after its collection and
                // docking properties. Observe the same real provider until it
                // is present; never infer accessibility from those properties.
                auto qatResult = accessibleElement(automation.Get(), accessibleHost.element.Get(), nullptr,
                    L"Quick Access Toolbar", UIA_ToolBarControlTypeId);
                auto readQat = [&] {
                    auto result = accessibleElement(automation.Get(), accessibleHost.element.Get(), nullptr,
                        L"Quick Access Toolbar", UIA_ToolBarControlTypeId);
                    if(!result.passed&&accessibleRibbon)
                        result=accessibleElement(automation.Get(),accessibleRibbon.Get(),nullptr,
                            L"Quick Access Toolbar",UIA_ToolBarControlTypeId);
                    // Server 2022's native Ribbon exposes this same toolbar
                    // as "Quick Access"; its type and owned scope still apply.
                    if(!result.passed&&accessibleRibbon)
                        result=accessibleElement(automation.Get(),accessibleRibbon.Get(),nullptr,
                            L"Quick Access",UIA_ToolBarControlTypeId);
                    return result;
                };
                const auto qatDeadline = GetTickCount64()+2000;
                unsigned qatObservations = 1;
                while(!qatResult.passed&&GetTickCount64()<qatDeadline) {
                    Sleep(50);
                    qatResult=readQat();
                    ++qatObservations;
                }
                qatResult.detail+=L"; observations="+std::to_wstring(qatObservations);
                if(!qatResult.passed&&automation&&accessibleRibbon) {
                    // Diagnose only toolbar providers inside this owned native
                    // Ribbon. Keep the required QAT name/type assertion intact.
                    VARIANT toolbarType{};toolbarType.vt=VT_I4;toolbarType.lVal=UIA_ToolBarControlTypeId;
                    ComPtr<IUIAutomationCondition> toolbarCondition;
                    ComPtr<IUIAutomationElementArray> toolbars;
                    auto toolbarRead=automation->CreatePropertyCondition(UIA_ControlTypePropertyId,toolbarType,&toolbarCondition);
                    if(SUCCEEDED(toolbarRead))toolbarRead=accessibleRibbon->FindAll(TreeScope_Descendants,toolbarCondition.Get(),&toolbars);
                    int toolbarCount=0;
                    if(SUCCEEDED(toolbarRead)&&toolbars)toolbarRead=toolbars->get_Length(&toolbarCount);
                    qatResult.detail+=L"; native Ribbon toolbars="+std::to_wstring(toolbarCount)+L"/"+hresultMessage(toolbarRead);
                    for(int index=0;SUCCEEDED(toolbarRead)&&index<toolbarCount&&index<16;++index) {
                        ComPtr<IUIAutomationElement> toolbar;BSTR toolbarName=nullptr;
                        toolbarRead=toolbars->GetElement(index,&toolbar);
                        if(SUCCEEDED(toolbarRead))toolbarRead=toolbar->get_CurrentName(&toolbarName);
                        if(SUCCEEDED(toolbarRead))qatResult.detail+=L"; toolbar name="+std::wstring(toolbarName?toolbarName:L"");
                        SysFreeString(toolbarName);
                    }
                }
                accessibleCheckResult("native_accessibility_quick_access",qatResult.passed,qatResult.detail);
                auto accessibleQat=qatResult.element;
                accessibleCheck("native_accessibility_qat_properties", accessibleQat.Get(), nullptr, L"Properties", UIA_ButtonControlTypeId);
                accessibleCheck("native_accessibility_qat_new_folder", accessibleQat.Get(), nullptr, L"New folder", UIA_ButtonControlTypeId);
                accessibleCheck("native_accessibility_address", nullptr, address_, addressCaption.c_str(), UIA_EditControlTypeId);
                accessibleCheck("native_accessibility_search", nullptr, search_, searchCaption.c_str(), UIA_EditControlTypeId);
                accessibleCheck("native_accessibility_navigation", accessibleHost.element.Get(), nullptr, nullptr, UIA_TreeControlTypeId);
                ComPtr<IUIAutomationElement> accessibleFolderRoot;
                if (automation && accessibilityFolderWindow) automation->ElementFromHandle(accessibilityFolderWindow, &accessibleFolderRoot);
                accessibleCheck("native_accessibility_folder_view", accessibleFolderRoot.Get(), nullptr, nullptr, UIA_ListControlTypeId);
                return items;
                });
                const bool accessibilityReady = pumpUntil([&] {
                    return accessibility.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
                }, 12000);
                check("native_accessibility_mta_worker_bounded", accessibilityReady);
                for (const auto& item : accessibility.get()) check(item.name.c_str(), item.passed, item.detail);
                ComPtr<INameSpaceTreeControl2> navigationTree;
                auto treeResult = IUnknown_QueryService(browser_.Get(), SID_SNavigationPane, IID_PPV_ARGS(&navigationTree));
                std::wstring treeSource = L"ExplorerBrowser SID_SNavigationPane";
                if (FAILED(treeResult) && view_) {
                    treeResult = IUnknown_QueryService(view_.Get(), SID_SNavigationPane, IID_PPV_ARGS(&navigationTree));
                    treeSource = L"IShellView SID_SNavigationPane";
                }
                if (FAILED(treeResult) && view_) {
                    ComPtr<IObjectWithSite> located;
                    ComPtr<IServiceProvider> site;
                    treeResult = view_.As(&located);
                    if (SUCCEEDED(treeResult)) treeResult = located->GetSite(IID_PPV_ARGS(&site));
                    if (SUCCEEDED(treeResult)) treeResult = site->QueryService(SID_SNavigationPane, IID_PPV_ARGS(&navigationTree));
                    treeSource = L"IShellView site SID_SNavigationPane";
                }
                check("native_navigation_tree_service", SUCCEEDED(treeResult) && navigationTree,
                    treeSource + L"; " + hresultMessage(treeResult));
                if (navigationTree) {
                    const bool originalAll = showAllFolders_, originalLibraries = showLibraries_, originalExpand = expandCurrent_;
                    for (const bool all : {false, true}) {
                        showAllFolders_ = all;
                        auto apply = applyNavigationOptions();
                        NSTCSTYLE2 actual = NSTCS2_DEFAULT;
                        const auto read = navigationTree->GetControlStyle2(NSTCS2_DISPLAYPINNEDONLY, &actual);
                        check(all ? "native_navigation_show_all_readback" : "native_navigation_pinned_readback",
                            SUCCEEDED(apply) && SUCCEEDED(read) &&
                            ((actual & NSTCS2_DISPLAYPINNEDONLY) != 0) == !all,
                            L"Apply=" + hresultMessage(apply) + L"; Read=" + hresultMessage(read) + L"; style=" + std::to_wstring(actual));
                    }
                    ComPtr<IShellItem> libraries;
                    auto libraryResult = SHGetKnownFolderItem(FOLDERID_Libraries, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&libraries));
                    for (const bool visible : {true, false}) {
                        showLibraries_ = visible;
                        auto apply = applyNavigationOptions();
                        ComPtr<IShellItemArray> roots; DWORD rootCount = 0; bool present = false;
                        auto read = navigationTree->GetRootItems(&roots);
                        if (SUCCEEDED(read)) read = roots->GetCount(&rootCount);
                        for (DWORD i = 0; SUCCEEDED(read) && i < rootCount; ++i) {
                            ComPtr<IShellItem> root; int order = 1;
                            read = roots->GetItemAt(i, &root);
                            if (SUCCEEDED(read) && libraries) read = root->Compare(libraries.Get(), SICHINT_CANONICAL, &order);
                            if (SUCCEEDED(read) && !order) present = true;
                        }
                        check(visible ? "native_navigation_libraries_visible_readback" : "native_navigation_libraries_hidden_readback",
                            SUCCEEDED(libraryResult) && SUCCEEDED(apply) && SUCCEEDED(read) && present == visible,
                            L"Apply=" + hresultMessage(apply) + L"; Read=" + hresultMessage(read) + L"; roots=" + std::to_wstring(rootCount));
                    }
                    showAllFolders_ = true;
                    expandCurrent_ = true;
                    auto apply = applyNavigationOptions();
                    const auto expansionItems = navigationExpansion_;
                    const auto expansionStartIndex = navigationExpansionIndex_;
                    ComPtr<IShellItem> current; RECT itemBounds{};
                    auto read = currentFolder(current);
                    if (SUCCEEDED(read)) {
                        // Namespace ancestors materialize asynchronously as the
                        // native tree expands them. Pump its deferred work, then
                        // require actual native bounds for the selected folder.
                        pumpUntil([&] {
                            read = navigationTree->GetItemRect(current.Get(), &itemBounds);
                            return SUCCEEDED(read) && itemBounds.right > itemBounds.left &&
                                itemBounds.bottom > itemBounds.top;
                        }, 5000);
                    }
                    std::wstring expansionDiagnostic = L"; initial chain=" + std::to_wstring(expansionItems.size()) +
                        L"; initial index=" + std::to_wstring(expansionStartIndex) +
                        L"; final index=" + std::to_wstring(navigationExpansionIndex_) +
                        L"; active chain=" + std::to_wstring(navigationExpansion_.size()) +
                        L"; navigating=" + std::to_wstring(navigating_ ? 1 : 0) +
                        L"; last expansion=" + hresultMessage(navigationExpansionStatus_);
                    for (size_t i = 0; i < std::min<size_t>(expansionItems.size(), 12); ++i) {
                        NSTCITEMSTATE state{}; RECT bounds{};
                        const auto stateRead = navigationTree->GetItemState(expansionItems[i].Get(),
                            static_cast<NSTCITEMSTATE>(NSTCIS_EXPANDED | NSTCIS_DISABLED), &state);
                        const auto boundsRead = navigationTree->GetItemRect(expansionItems[i].Get(), &bounds);
                        SFGAOF attributes = 0;
                        const auto attributeRead = expansionItems[i]->GetAttributes(
                            SFGAO_HASSUBFOLDER | SFGAO_FOLDER | SFGAO_HIDDEN | SFGAO_SYSTEM, &attributes);
                        expansionDiagnostic += L"; ancestor " + std::to_wstring(i) + L" state=" + hresultMessage(stateRead) +
                            L"/" + std::to_wstring(static_cast<unsigned>(state)) + L" bounds=" + hresultMessage(boundsRead) +
                            L" attributes=" + hresultMessage(attributeRead) + L"/" + std::to_wstring(attributes);
                    }
                    check("native_navigation_expand_current_visible_readback", SUCCEEDED(apply) && SUCCEEDED(read) &&
                        itemBounds.right > itemBounds.left && itemBounds.bottom > itemBounds.top,
                        L"Apply=" + hresultMessage(apply) + L"; native item bounds=" + hresultMessage(read) + expansionDiagnostic);
                    showAllFolders_ = originalAll; showLibraries_ = originalLibraries; expandCurrent_ = originalExpand;
                    applyNavigationOptions();
                    // The branch existed before native tree enumeration. Its
                    // initially absent leaf distinguishes this one-time action
                    // from an already-expanded path without a stale child cache.
                    const auto oneTimeFolder = fixture / L"Subfolder" / L"One-time expansion" / L"Deep";
                    showAllFolders_ = true;
                    expandCurrent_ = false;
                    const bool persistentPreference = preferences_.expandToCurrent;
                    const auto oneTimeNavigation = navigate(oneTimeFolder.wstring());
                    const bool oneTimeReady = SUCCEEDED(oneTimeNavigation) && pumpUntil([&] {
                        return !navigating_ && atLocation(oneTimeFolder);
                    }, 5000);
                    ComPtr<IShellItem> oneTimeItem;
                    RECT oneTimeBounds{};
                    auto oneTimeRead = oneTimeReady ? currentFolder(oneTimeItem) : E_UNEXPECTED;
                    RECT beforeOneTimeBounds{};
                    const auto beforeOneTimeRead = oneTimeItem ? navigationTree->GetItemRect(oneTimeItem.Get(), &beforeOneTimeBounds) : E_UNEXPECTED;
                    const bool initiallyCollapsed = FAILED(beforeOneTimeRead) ||
                        beforeOneTimeBounds.right <= beforeOneTimeBounds.left || beforeOneTimeBounds.bottom <= beforeOneTimeBounds.top;
                    const auto oneTimeCommand = oneTimeReady ? execute(ExpandAncestors) : E_UNEXPECTED;
                    if (SUCCEEDED(oneTimeRead) && SUCCEEDED(oneTimeCommand)) pumpUntil([&] {
                        oneTimeRead = navigationTree->GetItemRect(oneTimeItem.Get(), &oneTimeBounds);
                        return SUCCEEDED(oneTimeRead) && oneTimeBounds.right > oneTimeBounds.left &&
                            oneTimeBounds.bottom > oneTimeBounds.top;
                    }, 5000);
                    check("native_ctrl_shift_e_one_time_expansion", oneTimeReady && initiallyCollapsed && SUCCEEDED(oneTimeCommand) &&
                        SUCCEEDED(oneTimeRead) && oneTimeBounds.right > oneTimeBounds.left &&
                        oneTimeBounds.bottom > oneTimeBounds.top && !expandCurrent_ &&
                        preferences_.expandToCurrent == persistentPreference,
                        L"Initially collapsed=" + std::to_wstring(initiallyCollapsed) + L"; Command=" + hresultMessage(oneTimeCommand) + L"; actual leaf bounds=" +
                        hresultMessage(oneTimeRead) + L"; persistent expansion remains disabled=" +
                        std::to_wstring(!expandCurrent_ ? 1 : 0));
                    showAllFolders_ = originalAll; showLibraries_ = originalLibraries; expandCurrent_ = originalExpand;
                    const auto returnNavigation = navigate(fixture.wstring());
                    check("one_time_expansion_returns_to_owned_fixture", SUCCEEDED(returnNavigation) && pumpUntil([&] {
                        return !navigating_ && atLocation(fixture);
                    }, 5000));
                    applyNavigationOptions();
                }
            }
            check("host_hidden_after_accessibility_phase", !IsWindowVisible(window_));
            check("filesystem_commands_enabled_for_physical_directory", physicalDirectory_ && filesystemFolder_ &&
                commandEnabled(NewFolder) && commandEnabled(RibbonNewMenu));
            ComPtr<IShellItem> newItemFolder;
            NativeContextMenu newItems;
            auto newMenuResult = currentFolder(newItemFolder);
            if (SUCCEEDED(newMenuResult)) newMenuResult = newItems.createNewItems(window_, newItemFolder.Get(), view_.Get());
            std::vector<ContextMenuEntry> newEntries;
            if (SUCCEEDED(newMenuResult)) newMenuResult = newItems.enumerate(newEntries);
            check("registered_new_item_menu_in_hidden_host", SUCCEEDED(newMenuResult) && !newEntries.empty(), hresultMessage(newMenuResult));
            newItems.reset();
            for (int mode = 0; mode < 8; ++mode) {
                const auto hr = setView(static_cast<ViewMode>(mode));
                FOLDERVIEWMODE actual = FVM_AUTO; int size = 0;
                const auto read = folderView_->GetViewModeAndIconSize(&actual, &size);
                constexpr std::array<FOLDERVIEWMODE, 8> modes{FVM_ICON, FVM_ICON, FVM_ICON, FVM_SMALLICON, FVM_LIST, FVM_DETAILS, FVM_TILE, FVM_CONTENT};
                constexpr std::array<int, 8> sizes{256, 96, 48, 16, 16, 16, 48, 32};
                check(("view_" + std::to_string(mode)).c_str(), SUCCEEDED(hr) && SUCCEEDED(read) && actual == modes[mode] && size == sizes[mode],
                      std::wstring(ViewNames[mode]) + L": mode=" + std::to_wstring(actual) + L", size=" + std::to_wstring(size));
            }
            setView(ViewMode::Details);
            auto sortResult = setSort(PKEY_ItemNameDisplay);
            SORTCOLUMN sort{};
            check("sort_by_name", SUCCEEDED(sortResult) && SUCCEEDED(folderView_->GetSortColumns(&sort, 1)) &&
                  IsEqualPropertyKey(sort.propkey, PKEY_ItemNameDisplay) && sort.direction == SORT_ASCENDING);
            auto groupResult = setGroup(PKEY_ItemTypeText);
            PROPERTYKEY grouping{}; BOOL groupAscending = FALSE;
            check("group_by_type", SUCCEEDED(groupResult) && SUCCEEDED(folderView_->GetGroupBy(&grouping, &groupAscending)) &&
                  IsEqualPropertyKey(grouping, PKEY_ItemTypeText) && groupAscending);
            groupResult = setGroup(PKEY_Null);
            check("remove_grouping", SUCCEEDED(groupResult) && SUCCEEDED(folderView_->GetGroupBy(&grouping, &groupAscending)) && IsEqualPropertyKey(grouping, PKEY_Null));
            {
                // Exercise the real App dispatch against a complete native
                // sort array, then independently change the native view and
                // read its actual framework checked-property state.
                const auto retainedView = view_;
                const auto retainedFolder = folderView_;
                Pidl retainedLocation(ILCloneFull(currentPidl_.get()));
                const auto retainedNavigation = navigationCount_;
                const auto retainedHistory = history_.size();
                const auto retainedHistoryIndex = historyIndex_;
                const auto retainedRecent = recentSearches_;
                const auto sameHost = [&] {
                    return retainedLocation && currentPidl_ && ILGetSize(retainedLocation.get()) == ILGetSize(currentPidl_.get()) &&
                        std::memcmp(retainedLocation.get(), currentPidl_.get(), ILGetSize(retainedLocation.get())) == 0 &&
                        view_.Get() == retainedView.Get() && folderView_.Get() == retainedFolder.Get() && !closing_ && !navigating_ &&
                        navigationCount_ == retainedNavigation;
                };
                int priorSelectionCount = -1, priorSortCount = 0, priorFocusedIndex = -1;
                const auto priorSelectionCountRead = retainedFolder->ItemCount(SVGIO_SELECTION, &priorSelectionCount);
                ComPtr<IShellItemArray> priorSelection;
                const auto priorSelectionRead = retainedFolder->GetSelection(FALSE, &priorSelection);
                std::vector<SORTCOLUMN> priorSort;
                auto priorSortRead = retainedFolder->GetSortColumnCount(&priorSortCount);
                if (priorSortRead == S_OK && priorSortCount >= 0) {
                    priorSort.resize(static_cast<size_t>(priorSortCount));
                    if (priorSortCount) priorSortRead = retainedFolder->GetSortColumns(priorSort.data(), priorSortCount);
                } else if (priorSortRead == S_OK) priorSortRead = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                PROPERTYKEY priorGroup = PKEY_Null; BOOL priorGroupDirection = TRUE;
                const auto priorGroupRead = retainedFolder->GetGroupBy(&priorGroup, &priorGroupDirection);
                DWORD priorFolderFlags = 0, priorFocusedFlags = 0;
                auto priorFocusRead = retainedFolder->GetCurrentFolderFlags(&priorFolderFlags);
                if (priorFocusRead == S_OK) priorFocusRead = retainedFolder->GetFocusedItem(&priorFocusedIndex);
                PITEMID_CHILD rawPriorFocus = nullptr;
                if (priorFocusRead == S_OK && priorFocusedIndex >= 0) priorFocusRead = retainedFolder->Item(priorFocusedIndex, &rawPriorFocus);
                else if (priorFocusRead == S_OK) priorFocusRead = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                Pidl priorFocusedChild(rawPriorFocus);
                if (priorFocusRead == S_OK && priorFocusedChild) priorFocusRead = retainedFolder->GetSelectionState(priorFocusedChild.get(), &priorFocusedFlags);
                else if (priorFocusRead == S_OK) priorFocusRead = E_UNEXPECTED;
                ComPtr<IShellItem> priorFocusedItem; FILE_ID_INFO priorFocusedId{};
                if (priorFocusRead == S_OK) priorFocusRead = retainedFolder->GetItem(priorFocusedIndex, IID_PPV_ARGS(&priorFocusedItem));
                if (priorFocusRead == S_OK && priorFocusedItem)
                    priorFocusRead = nativeFileIdentity(itemName(priorFocusedItem.Get(), SIGDN_FILESYSPATH), priorFocusedId);
                else if (priorFocusRead == S_OK) priorFocusRead = E_UNEXPECTED;
                ComPtr<IShellItemArray> selectedBefore;
                std::set<NativeFileIdentity> selectionBefore;
                auto setup = priorSelectionCountRead == S_OK && priorSelectionCount == 0 &&
                    priorSelectionRead == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) && !priorSelection && priorSortRead == S_OK &&
                    (priorGroupRead == S_OK || priorGroupRead == S_FALSE) && priorFocusRead == S_OK && sameHost() ? S_OK : E_UNEXPECTED;
                const auto sourcePath = fixture / L"file-0.txt";
                FILE_ID_INFO sourceBefore{};
                if (SUCCEEDED(setup)) setup = nativeFileIdentity(sourcePath, sourceBefore);
                const auto sourceTime = std::filesystem::last_write_time(sourcePath);
                std::ifstream sourceInput(sourcePath, std::ios::binary);
                const std::string sourceBytes{std::istreambuf_iterator<char>(sourceInput), std::istreambuf_iterator<char>()};
                ComPtr<IShellItem> seedItem; PIDLIST_ABSOLUTE rawSeed = nullptr;
                if (SUCCEEDED(setup)) setup = SHCreateItemFromParsingName(sourcePath.c_str(), nullptr, IID_PPV_ARGS(&seedItem));
                if (SUCCEEDED(setup)) setup = SHGetIDListFromObject(seedItem.Get(), &rawSeed);
                Pidl seedId(rawSeed);
                // Preserve the original hidden-view snapshot first. Native
                // presentation/tab startup may select an item, so finish that
                // one-time setup before the single controlled selection.
                std::unique_ptr<PrivatePresentation> sortPresentation;
                HRESULT sortTabRead = E_PENDING;
                if (SUCCEEDED(setup) && seedId && sameHost()) {
                    sortPresentation = std::make_unique<PrivatePresentation>(window_, true);
                    sortTabRead = sortPresentation->ready && sameHost() ? ribbon_.selectTab(RibbonViewTab) : E_ACCESSDENIED;
                    if (FAILED(sortTabRead)) setup = sortTabRead;
                    else if (!sameHost()) setup = HRESULT_FROM_WIN32(ERROR_RETRY);
                }
                if (SUCCEEDED(setup) && seedId && sameHost()) setup = retainedView->SelectItem(ILFindLastID(seedId.get()), SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_NOTAKEFOCUS);
                else if (SUCCEEDED(setup)) setup = HRESULT_FROM_WIN32(ERROR_RETRY);
                int seededCount = -1;
                if (SUCCEEDED(setup)) setup = retainedFolder->ItemCount(SVGIO_SELECTION, &seededCount);
                if (SUCCEEDED(setup)) setup = retainedFolder->GetSelection(FALSE, &selectedBefore);
                if (SUCCEEDED(setup)) setup = nativeArrayIdentities(selectedBefore.Get(), selectionBefore);
                std::array<BYTE, 16> sourceId{};
                std::copy(std::begin(sourceBefore.FileId.Identifier), std::end(sourceBefore.FileId.Identifier), sourceId.begin());
                const bool exactSeed = setup == S_OK && seededCount == 1 &&
                    selectionBefore == std::set<NativeFileIdentity>{{sourceBefore.VolumeSerialNumber, sourceId}};
                std::wstring intactDetail;
                const auto intact = [&](bool expectSeed = true) {
                    const bool hostBefore = sameHost();
                    ComPtr<IShellItemArray> selectedAfter;
                    std::set<NativeFileIdentity> selectionAfter;
                    FILE_ID_INFO sourceAfter{};
                    int actualSelected = -1;
                    const auto countRead = retainedFolder->ItemCount(SVGIO_SELECTION, &actualSelected);
                    const auto selectedRead = selection(selectedAfter);
                    const auto identitiesRead = SUCCEEDED(selectedRead) ? nativeArrayIdentities(selectedAfter.Get(), selectionAfter) : selectedRead;
                    const auto sourceRead = nativeFileIdentity(sourcePath, sourceAfter);
                    std::ifstream input(sourcePath, std::ios::binary);
                    const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
                    const bool sourceIdentityPreserved = SUCCEEDED(sourceRead) && sourceBefore.VolumeSerialNumber == sourceAfter.VolumeSerialNumber &&
                        std::memcmp(sourceBefore.FileId.Identifier, sourceAfter.FileId.Identifier, sizeof(sourceBefore.FileId.Identifier)) == 0;
                    const bool sourceBytesPreserved = bytes == sourceBytes;
                    const bool sourceTimePreserved = std::filesystem::last_write_time(sourcePath) == sourceTime;
                    const bool sourcePreserved = sourceIdentityPreserved && sourceBytesPreserved && sourceTimePreserved;
                    const bool locationPreserved = retainedLocation && currentPidl_ && ILGetSize(retainedLocation.get()) == ILGetSize(currentPidl_.get()) &&
                        std::memcmp(retainedLocation.get(), currentPidl_.get(), ILGetSize(retainedLocation.get())) == 0;
                    const bool selectionPreserved = countRead == S_OK && (expectSeed ?
                        actualSelected == 1 && SUCCEEDED(identitiesRead) && selectionAfter == selectionBefore :
                        actualSelected == 0 && selectedRead == priorSelectionRead && !selectedAfter);
                    intactDetail = L"selection(countHR/count/arrayHR/idsHR/same)=" + hresultMessage(countRead) + L"/" + std::to_wstring(actualSelected) +
                        L"/" + hresultMessage(selectedRead) + L"/" + hresultMessage(identitiesRead) + L"/" + std::to_wstring(selectionPreserved) +
                        L"; source(HR/id/bytes/time)=" + hresultMessage(sourceRead) + L"/" + std::to_wstring(sourceIdentityPreserved) + L"/" +
                        std::to_wstring(sourceBytesPreserved) + L"/" + std::to_wstring(sourceTimePreserved) + L"; location/view/folder=" +
                        std::to_wstring(locationPreserved) + L"/" + std::to_wstring(view_.Get() == retainedView.Get()) + L"/" +
                        std::to_wstring(folderView_.Get() == retainedFolder.Get()) + L"; closing/navigating=" + std::to_wstring(closing_) + L"/" +
                        std::to_wstring(navigating_) + L"; navigation/historyCount/historyIndex/MRU=" + std::to_wstring(navigationCount_ == retainedNavigation) +
                        L"/" + std::to_wstring(history_.size() == retainedHistory) + L"/" + std::to_wstring(historyIndex_ == retainedHistoryIndex) +
                        L"/" + std::to_wstring(recentSearches_ == retainedRecent) + L"; coherent(before/after)=" + std::to_wstring(hostBefore) + L"/" + std::to_wstring(sameHost());
                    return selectionPreserved && sourcePreserved && locationPreserved &&
                        view_.Get() == retainedView.Get() && folderView_.Get() == retainedFolder.Get() && !closing_ && !navigating_ &&
                        navigationCount_ == retainedNavigation && history_.size() == retainedHistory && historyIndex_ == retainedHistoryIndex &&
                        recentSearches_ == retainedRecent;
                };
                const std::array<SORTCOLUMN, 2> original{{
                    {PKEY_ItemNameDisplay, SORT_ASCENDING}, {PKEY_Size, SORT_DESCENDING}}};
                if (SUCCEEDED(setup)) setup = sameHost() ? retainedFolder->SetSortColumns(original.data(), static_cast<int>(original.size())) : HRESULT_FROM_WIN32(ERROR_RETRY);
                const auto orderKeyText = [](const PROPERTYKEY& key) {
                    wchar_t guid[40]{};
                    return (StringFromGUID2(key.fmtid, guid, static_cast<int>(std::size(guid))) ? std::wstring(guid) : L"unreadable-guid") +
                        L":" + std::to_wstring(key.pid);
                };
                const auto cacheDetail = [&] {
                    return L"cache(sortValid/key/ascending/groupValid)=" + std::to_wstring(sortPropertyValid_) + L"/" + orderKeyText(sortProperty_) +
                        L"/" + std::to_wstring(ascending_) + L"/" + std::to_wstring(groupPropertyValid_) + L"; orderView/orderNav/currentNav/fence=" +
                        std::to_wstring(orderStateView_ == retainedFolder.Get()) + L"/" + std::to_wstring(orderStateNavigation_) + L"/" +
                        std::to_wstring(navigationCount_) + L"/" + std::to_wstring(sameHost());
                };
                std::wstring sortDetail, booleanDetail;
                const auto exactSort = [&](SORTDIRECTION direction) {
                    const bool hostBefore = sameHost();
                    int count = -1;
                    std::vector<SORTCOLUMN> actual;
                    const auto countRead = hostBefore ? retainedFolder->GetSortColumnCount(&count) : E_ABORT;
                    auto read = countRead;
                    HRESULT columnsRead = E_PENDING;
                    if (read == S_OK && count >= 0 && sameHost()) {
                        actual.resize(static_cast<size_t>(count));
                        read = columnsRead = count ? retainedFolder->GetSortColumns(actual.data(), count) : S_OK;
                    } else if (read == S_OK) read = HRESULT_FROM_WIN32(count < 0 ? ERROR_INVALID_DATA : ERROR_RETRY);
                    sortDetail = L"countHR/count/columnsHR=" + hresultMessage(countRead) + L"/" + std::to_wstring(count) + L"/" + hresultMessage(columnsRead);
                    for (size_t index = 0; columnsRead == S_OK && index < actual.size(); ++index)
                        sortDetail += L"; column[" + std::to_wstring(index) + L"]=" + orderKeyText(actual[index].propkey) + L"/" + std::to_wstring(actual[index].direction);
                    sortDetail += L"; coherent(before/after)=" + std::to_wstring(hostBefore) + L"/" + std::to_wstring(sameHost());
                    return read == S_OK && actual.size() == original.size() && IsEqualPropertyKey(actual[0].propkey, original[0].propkey) && actual[0].direction == direction &&
                        IsEqualPropertyKey(actual[1].propkey, original[1].propkey) && actual[1].direction == original[1].direction;
                };
                const auto checkedSort = [&](UINT command, bool expected, const wchar_t* phase) {
                    const bool hostBefore = sameHost();
                    const auto flushed = hostBefore && ribbon_.valid() ? ribbon_.flush() : E_ABORT;
                    struct Property { PROPVARIANT value{}; ~Property(){PropVariantClear(&value);} } rawProperty;
                    auto& property = rawProperty.value;
                    const auto read = SUCCEEDED(flushed) && sameHost() ?
                        ribbon_.framework()->GetUICommandProperty(command, UI_PKEY_BooleanValue, &property) : E_ABORT;
                    const UINT type = property.vt;
                    const auto rawBoolean = property.vt == VT_BOOL ? std::to_wstring(property.boolVal) : L"unavailable";
                    BOOL checked = FALSE;
                    const auto converted = SUCCEEDED(read) ? PropVariantToBoolean(property, &checked) : read;
                    PropVariantClear(&property);
                    RibbonCollectionReadback registration;
                    const auto registrationRead = ribbon_.collectionReadback(command, registration);
                    booleanDetail += std::wstring(L"; ") + phase + L" command=" + std::to_wstring(command) + L" flush/read/VT/rawBOOL/converted/value=" +
                        hresultMessage(flushed) + L"/" + hresultMessage(read) + L"/" + std::to_wstring(type) + L"/" + rawBoolean + L"/" +
                        hresultMessage(converted) + L"/" + std::to_wstring(checked != FALSE) + L"; registered(HR/present/type)=" +
                        hresultMessage(registrationRead) + L"/" + std::to_wstring(registration.registered) + L"/" + std::to_wstring(registration.nativeType) +
                        L"; coherent(before/after)=" + std::to_wstring(hostBefore) + L"/" + std::to_wstring(sameHost());
                    return SUCCEEDED(converted) && (checked != FALSE) == expected;
                };
                const bool nativeSortSetup = exactSeed && SUCCEEDED(setup) && SUCCEEDED(sortTabRead) && sortPresentation && sortPresentation->ready &&
                    retainedLocation && !sourceBytes.empty() && exactSort(SORT_ASCENDING) && intact();
                HRESULT sortLabelRead = E_PENDING;
                std::wstring sortLabel;
                OwnedOrderExpansion sortExpansion;
                HWND orderRibbonWindow = nullptr;
                if (nativeSortSetup && sameHost()) {
                    if (SUCCEEDED(sortTabRead) && sameHost()) sortLabelRead = ribbon_.commandLabel(SortMenu, sortLabel);
                    if (SUCCEEDED(sortLabelRead) && sameHost() && intact()) {
                        EnumChildWindows(window_, [](HWND child, LPARAM context) -> BOOL {
                            wchar_t name[64]{};
                            if (GetClassNameW(child, name, static_cast<int>(std::size(name))) && wcscmp(name, L"UIRibbonCommandBar") == 0) {
                                *reinterpret_cast<HWND*>(context) = child; return FALSE;
                            }
                            return TRUE;
                        }, reinterpret_cast<LPARAM>(&orderRibbonWindow));
                        sortExpansion = expandOwnedOrderDropdown(window_, orderRibbonWindow, sortLabel);
                    }
                }
                const bool genuineTwoColumns = nativeSortSetup && SUCCEEDED(sortTabRead) && SUCCEEDED(sortLabelRead) &&
                    sortExpansion.expanded && sortExpansion.desktopRestored == S_OK && sameHost() && intact() && exactSort(SORT_ASCENDING);
                std::wstring orderRealizationDetail;
                const auto realizeOrderValues = [&](const std::wstring& label, const wchar_t* phase) {
                    // Toggle values are queried lazily when the native control
                    // is revealed, even after invalidation. Reveal once after
                    // each mutation; never set a property or execute a leaf.
                    // https://learn.microsoft.com/windows/win32/windowsribbon/windowsribbon-controls-togglebutton
                    const bool coherentBefore = sameHost() && intact();
                    const auto readOrder = [&](std::vector<SORTCOLUMN>& columns, PROPERTYKEY& group, BOOL& direction, HRESULT& groupedRead) {
                        if (!sameHost()) return E_ABORT;
                        int count = -1;
                        auto read = retainedFolder->GetSortColumnCount(&count);
                        if (read != S_OK) return read;
                        if (count < 0) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                        if (!sameHost()) return E_ABORT;
                        columns.resize(static_cast<size_t>(count));
                        if (count) read = retainedFolder->GetSortColumns(columns.data(), count);
                        if (read != S_OK || !sameHost()) return read != S_OK ? read : E_ABORT;
                        groupedRead = retainedFolder->GetGroupBy(&group, &direction);
                        return sameHost() && (groupedRead == S_OK || groupedRead == S_FALSE) ? S_OK :
                            sameHost() ? groupedRead : E_ABORT;
                    };
                    std::vector<SORTCOLUMN> beforeColumns, afterColumns;
                    PROPERTYKEY beforeGroup = PKEY_Null, afterGroup = PKEY_Null;
                    BOOL beforeDirection = TRUE, afterDirection = TRUE;
                    HRESULT beforeGroupedRead = E_PENDING, afterGroupedRead = E_PENDING;
                    const auto beforeRead = coherentBefore ? readOrder(beforeColumns, beforeGroup, beforeDirection, beforeGroupedRead) : E_ABORT;
                    const auto realized = beforeRead == S_OK && sameHost() && intact() ?
                        expandOwnedOrderDropdown(window_, orderRibbonWindow, label) : OwnedOrderExpansion{};
                    const auto afterRead = sameHost() ? readOrder(afterColumns, afterGroup, afterDirection, afterGroupedRead) : E_ABORT;
                    const bool orderPreserved = beforeRead == S_OK && afterRead == S_OK && beforeColumns.size() == afterColumns.size() &&
                        std::equal(beforeColumns.begin(), beforeColumns.end(), afterColumns.begin(), [](const auto& first, const auto& second) {
                            return IsEqualPropertyKey(first.propkey, second.propkey) && first.direction == second.direction;
                        }) && beforeGroupedRead == afterGroupedRead && IsEqualPropertyKey(beforeGroup, afterGroup) &&
                        (beforeGroupedRead == S_FALSE || beforeDirection == afterDirection);
                    const bool coherentAfter = sameHost() && intact();
                    orderRealizationDetail += std::wstring(L"; ") + phase + L" realization(setup/budget/restored/expanded/coherent)=" +
                        hresultMessage(realized.setup) + L"/" + hresultMessage(realized.budget) + L"/" + hresultMessage(realized.desktopRestored) +
                        L"/" + std::to_wstring(realized.expanded) + L"/" + std::to_wstring(coherentBefore) + L"/" +
                        std::to_wstring(coherentAfter) + L"; native order(before/after/preserved)=" + hresultMessage(beforeRead) + L"/" +
                        hresultMessage(afterRead) + L"/" + std::to_wstring(orderPreserved) + L"; " + realized.detail;
                    return coherentBefore && coherentAfter && realized.setup == S_OK && realized.budget == S_OK &&
                        realized.desktopRestored == S_OK && realized.expanded && orderPreserved;
                };
                auto descending = genuineTwoColumns ? execute(SortDescending) : E_UNEXPECTED;
                updateCommands();
                const bool descendingNative = SUCCEEDED(descending) && exactSort(SORT_DESCENDING) && sortPropertyValid_ && !ascending_ &&
                    orderStateView_ == retainedFolder.Get() && orderStateNavigation_ == retainedNavigation && sameHost() && intact();
                const bool descendingRealized = descendingNative && realizeOrderValues(sortLabel, L"DESC");
                const bool exactDescending = descendingRealized && exactSort(SORT_DESCENDING) && checkedSort(SortDescending, true, L"DESC-required") &&
                    checkedSort(SortAscending, false, L"DESC-required") && intact();
                const auto descendingSortDetail = SUCCEEDED(descending) ? sortDetail : L"not read; command gated";
                if (!exactDescending && sameHost()) {
                    (void)checkedSort(SortDescending, true, L"DESC-failure-diagnostic");
                    if (sameHost()) (void)checkedSort(SortAscending, false, L"DESC-failure-diagnostic");
                    if (sameHost()) (void)intact();
                }
                const auto descendingCacheDetail = cacheDetail(), descendingIntactDetail = intactDetail;
                auto ascending = exactDescending ? execute(SortAscending) : E_UNEXPECTED;
                updateCommands();
                const bool ascendingNative = SUCCEEDED(ascending) && exactSort(SORT_ASCENDING) && sortPropertyValid_ && ascending_ &&
                    orderStateView_ == retainedFolder.Get() && orderStateNavigation_ == retainedNavigation && sameHost() && intact();
                const bool ascendingRealized = ascendingNative && realizeOrderValues(sortLabel, L"ASC");
                const bool exactAscending = ascendingRealized && exactSort(SORT_ASCENDING) && checkedSort(SortAscending, true, L"ASC-required") &&
                    checkedSort(SortDescending, false, L"ASC-required") && intact();
                const auto ascendingSortDetail = SUCCEEDED(ascending) ? sortDetail : L"not read; command gated";
                if (SUCCEEDED(ascending) && !exactAscending && sameHost()) {
                    (void)checkedSort(SortAscending, true, L"ASC-failure-diagnostic");
                    if (sameHost()) (void)checkedSort(SortDescending, false, L"ASC-failure-diagnostic");
                    if (sameHost()) (void)intact();
                }
                const auto ascendingCacheDetail = cacheDetail(), ascendingIntactDetail = intactDetail;
                check("sort_direction_preserves_complete_native_secondary_columns_and_selection", genuineTwoColumns && exactDescending && exactAscending,
                    L"native two-column setup=" + hresultMessage(setup) + L"; descending/ascending=" + hresultMessage(descending) + L"/" +
                    hresultMessage(ascending) + L"; exact readbacks=" + std::to_wstring(exactDescending) + L"/" + std::to_wstring(exactAscending) +
                    L"; prior count/read=" + hresultMessage(priorSelectionCountRead) + L"/" + std::to_wstring(priorSelectionCount) + L"/" +
                    hresultMessage(priorSelectionRead) + L"; prior sort/group/focus=" + hresultMessage(priorSortRead) + L"/" +
                    hresultMessage(priorGroupRead) + L"/" + hresultMessage(priorFocusRead) + L"; exact seed=" + std::to_wstring(exactSeed) +
                    L"; native dropdown(tab/label/setup/budget/restored/expanded)=" + hresultMessage(sortTabRead) + L"/" +
                    hresultMessage(sortLabelRead) + L"/" + hresultMessage(sortExpansion.setup) + L"/" + hresultMessage(sortExpansion.budget) + L"/" +
                    hresultMessage(sortExpansion.desktopRestored) + L"/" + std::to_wstring(sortExpansion.expanded) + L"; " + sortExpansion.detail +
                    L"; DESC {" + descendingSortDetail + L"; " + descendingCacheDetail + L"; " + descendingIntactDetail + L"}; ASC {" +
                    ascendingSortDetail + L"; " + ascendingCacheDetail + L"; " + ascendingIntactDetail + L"}" + booleanDetail + orderRealizationDetail);
                const SORTCOLUMN independent{PKEY_DateModified, SORT_DESCENDING};
                auto independentSort = exactAscending && sameHost() ? retainedFolder->SetSortColumns(&independent, 1) : E_ABORT;
                auto independentGroup = SUCCEEDED(independentSort) && sameHost() ? retainedFolder->SetGroupBy(PKEY_ItemTypeText, FALSE) : E_ABORT;
                updateCommands();
                OwnedOrderExpansion groupExpansion;
                std::wstring groupLabel;
                bool independentSortRealized = false;
                HRESULT groupLabelRead = E_PENDING, beforeGroupRead = E_PENDING, afterGroupRead = E_PENDING;
                HRESULT beforeOrderRead = E_PENDING, afterOrderRead = E_PENDING;
                int beforeOrderCount = -1, afterOrderCount = -1;
                PROPERTYKEY beforeGroupProperty = PKEY_Null, afterGroupProperty = PKEY_Null;
                BOOL beforeGroupDirection = TRUE, afterGroupDirection = TRUE;
                SORTCOLUMN beforeGroupOrder{}, afterGroupOrder{};
                if (SUCCEEDED(independentSort) && SUCCEEDED(independentGroup) && sameHost() && intact()) {
                    beforeGroupRead = retainedFolder->GetGroupBy(&beforeGroupProperty, &beforeGroupDirection);
                    if (sameHost()) beforeOrderRead = retainedFolder->GetSortColumnCount(&beforeOrderCount);
                    if (beforeOrderRead == S_OK && beforeOrderCount == 1 && sameHost())
                        beforeOrderRead = retainedFolder->GetSortColumns(&beforeGroupOrder, 1);
                    if (beforeGroupRead == S_OK && IsEqualPropertyKey(beforeGroupProperty, PKEY_ItemTypeText) && !beforeGroupDirection &&
                        beforeOrderRead == S_OK && beforeOrderCount == 1 && IsEqualPropertyKey(beforeGroupOrder.propkey, independent.propkey) &&
                        beforeGroupOrder.direction == independent.direction && sameHost() && intact()) {
                        groupLabelRead = ribbon_.commandLabel(GroupMenu, groupLabel);
                        if (SUCCEEDED(groupLabelRead) && sameHost()) {
                            groupExpansion = expandOwnedOrderDropdown(window_, orderRibbonWindow, groupLabel);
                            if (groupExpansion.expanded && groupExpansion.budget == S_OK && groupExpansion.desktopRestored == S_OK && sameHost() && intact())
                                independentSortRealized = realizeOrderValues(sortLabel, L"independent Date DESC");
                        }
                    }
                    if (sameHost()) afterGroupRead = retainedFolder->GetGroupBy(&afterGroupProperty, &afterGroupDirection);
                    if (sameHost()) afterOrderRead = retainedFolder->GetSortColumnCount(&afterOrderCount);
                    if (afterOrderRead == S_OK && afterOrderCount == 1 && sameHost())
                        afterOrderRead = retainedFolder->GetSortColumns(&afterGroupOrder, 1);
                }
                const bool groupMaterialized = groupExpansion.expanded && groupExpansion.desktopRestored == S_OK && sameHost() &&
                    beforeGroupRead == S_OK && afterGroupRead == S_OK && IsEqualPropertyKey(beforeGroupProperty, afterGroupProperty) &&
                    beforeGroupDirection == afterGroupDirection && beforeOrderRead == S_OK && afterOrderRead == S_OK &&
                    beforeOrderCount == 1 && afterOrderCount == beforeOrderCount &&
                    IsEqualPropertyKey(beforeGroupOrder.propkey, afterGroupOrder.propkey) && beforeGroupOrder.direction == afterGroupOrder.direction && intact();
                const bool authored = ribbon_.layout() == RibbonLayout::Authored;
                const bool independentChecks = SUCCEEDED(independentSort) && SUCCEEDED(independentGroup) && groupMaterialized && independentSortRealized &&
                    (!authored || (commandChecked(SortDate, true) && commandChecked(SortName, false) && commandChecked(GroupType, true) && commandChecked(GroupNone, false))) && intact();
                const auto unrelatedDirection = independentChecks ? execute(SortAscending) : E_UNEXPECTED;
                updateCommands();
                auto changedGroup = SUCCEEDED(unrelatedDirection) && intact() ? execute(GroupName) : E_UNEXPECTED;
                updateCommands();
                PROPERTYKEY actualGroup{}; BOOL actualDirection = TRUE;
                const auto changedGroupRead = retainedFolder->GetGroupBy(&actualGroup, &actualDirection);
                const bool changedGroupNative = SUCCEEDED(changedGroup) && changedGroupRead == S_OK &&
                    IsEqualPropertyKey(actualGroup, PKEY_ItemNameDisplay) && !actualDirection && sameHost() && intact();
                const bool changedSortRealized = changedGroupNative && realizeOrderValues(sortLabel, L"independent Date ASC");
                const bool changedGroupRealized = changedSortRealized && realizeOrderValues(groupLabel, L"independent Name reverse group");
                const auto changedGroupFinalRead = changedGroupRealized && sameHost() ?
                    retainedFolder->GetGroupBy(&actualGroup, &actualDirection) : E_PENDING;
                const bool independentGroupDirection = changedGroupRealized && changedGroupFinalRead == S_OK &&
                    IsEqualPropertyKey(actualGroup, PKEY_ItemNameDisplay) && !actualDirection && checkedSort(SortAscending, true, L"GROUP-required") &&
                    (!authored || (commandChecked(GroupName, true) && commandChecked(GroupType, false))) && intact();
                check("native_sort_group_property_checks_and_independent_group_direction", independentChecks && independentGroupDirection,
                    L"independent sort/group=" + hresultMessage(independentSort) + L"/" + hresultMessage(independentGroup) + L"; group command/read=" +
                    hresultMessage(changedGroup) + L"/" + hresultMessage(changedGroupRead) + L"/" + hresultMessage(changedGroupFinalRead) +
                    L"; unrelated item ascending=" + hresultMessage(unrelatedDirection) +
                    L"; reverse group=" + std::to_wstring(!actualDirection) +
                    L"; authored checked properties=" + std::to_wstring(authored) + L"; Group dropdown(label/setup/budget/restored/expanded/preserved)=" +
                    hresultMessage(groupLabelRead) + L"/" + hresultMessage(groupExpansion.setup) + L"/" + hresultMessage(groupExpansion.budget) + L"/" +
                    hresultMessage(groupExpansion.desktopRestored) + L"/" + std::to_wstring(groupExpansion.expanded) + L"/" + std::to_wstring(groupMaterialized) +
                    L"; native group before/after=" + hresultMessage(beforeGroupRead) + L"/" + hresultMessage(afterGroupRead) + L"/" +
                    orderKeyText(beforeGroupProperty) + L"/" + orderKeyText(afterGroupProperty) + L"/" + std::to_wstring(beforeGroupDirection) + L"/" +
                    std::to_wstring(afterGroupDirection) + L"; native sort before/after=" + hresultMessage(beforeOrderRead) + L"/" + hresultMessage(afterOrderRead) +
                    L"/" + std::to_wstring(beforeOrderCount) + L"/" + std::to_wstring(afterOrderCount) + L"/" + orderKeyText(beforeGroupOrder.propkey) + L"/" +
                    orderKeyText(afterGroupOrder.propkey) + L"/" + std::to_wstring(beforeGroupOrder.direction) + L"/" + std::to_wstring(afterGroupOrder.direction) +
                    L"; " + groupExpansion.detail + L"; " + cacheDetail() + booleanDetail + orderRealizationDetail);
                const auto restoreSort = sameHost() && priorSortRead == S_OK ? retainedFolder->SetSortColumns(priorSort.data(), priorSortCount) : E_UNEXPECTED;
                const auto restoreGroup = sameHost() && (priorGroupRead == S_OK || priorGroupRead == S_FALSE) ?
                    retainedFolder->SetGroupBy(priorGroup, priorGroupDirection) : E_UNEXPECTED;
                const auto restoreSelection = sameHost() ? retainedView->SelectItem(nullptr, SVSI_DESELECTOTHERS) : E_ABORT;
                const auto restoreFocus = sameHost() && SUCCEEDED(restoreSelection) && priorFocusRead == S_OK && priorFocusedChild ?
                    retainedView->SelectItem(priorFocusedChild.get(), (priorFocusedFlags & SVSI_SELECT) | SVSI_FOCUSED | SVSI_NOTAKEFOCUS) : E_UNEXPECTED;
                updateCommands();
                int restoredSortCount = -1, restoredFocusedIndex = -1;
                std::vector<SORTCOLUMN> restoredSort(priorSort.size());
                auto sortRead = retainedFolder->GetSortColumnCount(&restoredSortCount);
                if (sortRead == S_OK && restoredSortCount == priorSortCount && restoredSortCount)
                    sortRead = retainedFolder->GetSortColumns(restoredSort.data(), restoredSortCount);
                const bool sortRestored = sortRead == S_OK && restoredSortCount == priorSortCount &&
                    std::equal(priorSort.begin(), priorSort.end(), restoredSort.begin(), [](const auto& left, const auto& right) {
                        return IsEqualPropertyKey(left.propkey, right.propkey) && left.direction == right.direction;
                    });
                PROPERTYKEY restoredGroup = PKEY_Null; BOOL restoredGroupDirection = FALSE;
                const auto groupRead = retainedFolder->GetGroupBy(&restoredGroup, &restoredGroupDirection);
                const bool groupRestored = groupRead == priorGroupRead && IsEqualPropertyKey(priorGroup, restoredGroup) &&
                    (groupRead == S_FALSE || restoredGroupDirection == priorGroupDirection);
                DWORD restoredFolderFlags = 0, restoredFocusedFlags = 0;
                auto focusRead = retainedFolder->GetCurrentFolderFlags(&restoredFolderFlags);
                if (focusRead == S_OK) focusRead = retainedFolder->GetFocusedItem(&restoredFocusedIndex);
                ComPtr<IShellItem> restoredFocusedItem; FILE_ID_INFO restoredFocusedId{};
                if (focusRead == S_OK && restoredFocusedIndex >= 0)
                    focusRead = retainedFolder->GetItem(restoredFocusedIndex, IID_PPV_ARGS(&restoredFocusedItem));
                else if (focusRead == S_OK) focusRead = E_UNEXPECTED;
                if (focusRead == S_OK && restoredFocusedItem)
                    focusRead = nativeFileIdentity(itemName(restoredFocusedItem.Get(), SIGDN_FILESYSPATH), restoredFocusedId);
                else if (focusRead == S_OK) focusRead = E_UNEXPECTED;
                if (focusRead == S_OK) focusRead = retainedFolder->GetSelectionState(priorFocusedChild.get(), &restoredFocusedFlags);
                const bool focusRestored = focusRead == S_OK && restoredFolderFlags == priorFolderFlags && restoredFocusedFlags == priorFocusedFlags &&
                    priorFocusedId.VolumeSerialNumber == restoredFocusedId.VolumeSerialNumber &&
                    std::memcmp(priorFocusedId.FileId.Identifier, restoredFocusedId.FileId.Identifier, sizeof(priorFocusedId.FileId.Identifier)) == 0;
                const bool restored = SUCCEEDED(restoreSort) && SUCCEEDED(restoreGroup) && SUCCEEDED(restoreSelection) && SUCCEEDED(restoreFocus) &&
                    sortRestored && groupRestored && focusRestored && intact(false);
                check("sort_group_regression_restores_native_fixture", restored,
                    L"restore sort/group/selection/focus=" + hresultMessage(restoreSort) + L"/" + hresultMessage(restoreGroup) + L"/" +
                    hresultMessage(restoreSelection) + L"/" + hresultMessage(restoreFocus) + L"; exact sort/group/focus=" +
                    std::to_wstring(sortRestored) + L"/" + std::to_wstring(groupRestored) + L"/" + std::to_wstring(focusRestored) +
                    L"; focus read=" + hresultMessage(focusRead) + L"; original/actual folder and item flags=" +
                    std::to_wstring(priorFolderFlags) + L"/" + std::to_wstring(restoredFolderFlags) + L"; " +
                    std::to_wstring(priorFocusedFlags) + L"/" + std::to_wstring(restoredFocusedFlags));
            }
            {
                // A native background worker supplies the real completion and
                // STA teardown. The one-shot seam models its final HRESULT as
                // E_PENDING without touching Explorer's Undo/Redo history.
                updateCommands();
                auto quietGeneration = namespaceGeneration_;
                auto quietSince = GetTickCount64();
                const bool stableGeneration = pumpUntil([&] {
                    if (selectionStateDirty_ || namespaceDirty_) updateCommands();
                    const auto now = GetTickCount64();
                    const bool quiet = !navigating_ && !selectionStateDirty_ && !namespaceDirty_ &&
                        !deferredUpdateQueued_ && !commandRefreshActive_ && !commandRefreshPending_ &&
                        !commandStatesCancelPending_;
                    if (!quiet || namespaceGeneration_ != quietGeneration) {
                        quietGeneration = namespaceGeneration_; quietSince = now;
                        return false;
                    }
                    return now - quietSince >= 200;
                }, 5000);
                cancelCommandStates();
                const auto providerGeneration = namespaceGeneration_;
                auto savedCapabilities = std::move(commandCapabilities_);
                commandCapabilities_.clear();
                AppCommandCapability probe;
                const auto binding = appCommandBinding(Undo);
                if(binding)probe.binding=*binding;
                probe.status=E_PENDING;
                commandCapabilities_.emplace(Undo,std::move(probe));
                commandStateStartAt_=0;
                headlessCompletedPendingCommand_=Undo;
                startPendingCommandStates();
                bool actualWorkerStarted=commandStateTasks_.contains(Undo);
                const bool settled=pumpUntil([&] {
                    // Cancelled providers can still hold worker slots during
                    // native teardown. Allow the normal ERROR_BUSY retry path.
                    actualWorkerStarted=actualWorkerStarted||commandStateTasks_.contains(Undo);
                    pollCommandStates();
                    actualWorkerStarted=actualWorkerStarted||commandStateTasks_.contains(Undo);
                    const auto found=commandCapabilities_.find(Undo);
                    const bool consumed=found!=commandCapabilities_.end()&&found->second.slowStateCompleted&&
                        !headlessCompletedPendingCommand_;
                    // The timer can finish and consume the whole task between
                    // observations. Only its successful real native result
                    // establishes acceptance in that case.
                    actualWorkerStarted=actualWorkerStarted||
                        (consumed&&SUCCEEDED(headlessCompletedPendingOriginalStatus_));
                    return consumed;
                },5000);
                auto found=commandCapabilities_.find(Undo);
                const auto actualProviderRead=headlessCompletedPendingOriginalStatus_;
                const auto remainingTask=commandStateTasks_.find(Undo);
                const bool taskExists=remainingTask!=commandStateTasks_.end();
                const bool taskCompleted=taskExists&&remainingTask->second->completed();
                const bool markerRemaining=headlessCompletedPendingCommand_.has_value();
                const auto postCapabilityStatus=found!=commandCapabilities_.end()?found->second.status:E_UNEXPECTED;
                const auto generationDelta=namespaceGeneration_-providerGeneration;
                const bool retainedPending=found!=commandCapabilities_.end()&&found->second.status==E_PENDING&&
                    found->second.slowStateCompleted&&!found->second.enabled&&!found->second.checked;
                bool noRestart=settled&&retainedPending&&!commandStatesPending();
                for(unsigned pass=0;pass<4;++pass) {
                    pollCommandStates();
                    found=commandCapabilities_.find(Undo);
                    noRestart=noRestart&&found!=commandCapabilities_.end()&&found->second.status==E_PENDING&&
                        found->second.slowStateCompleted&&!found->second.enabled&&
                        commandStateTasks_.empty()&&!selectionStateBatch_&&!commandStatesPending();
                }
                check("completed_native_pending_state_stays_disabled_without_restarting",stableGeneration&&generationDelta==0&&
                    actualWorkerStarted&&SUCCEEDED(actualProviderRead)&&settled&&retainedPending&&noRestart,
                    L"Actual worker started="+std::to_wstring(actualWorkerStarted)+L"; completed pending consumed="+
                    std::to_wstring(settled)+L"; actual pre-injection native read="+hresultMessage(actualProviderRead)+
                    L"; native E_PENDING retained="+std::to_wstring(retainedPending)+
                    L"; repeated polling did not restart="+std::to_wstring(noRestart)+
                    L"; stable generation="+std::to_wstring(stableGeneration)+L"; generation delta="+std::to_wstring(generationDelta)+
                    L"; task exists/completed="+std::to_wstring(taskExists)+L"/"+std::to_wstring(taskCompleted)+
                    L"; marker remaining="+std::to_wstring(markerRemaining)+L"; post capability="+hresultMessage(postCapabilityStatus));
                // The normal generation refresh creates new capabilities from
                // the actual current native view; it must not inherit the
                // previous generation's completed-but-unresolved marker.
                selectionStateDirty_=namespaceDirty_=true;
                updateCommands();
                found=commandCapabilities_.find(Undo);
                check("new_command_generation_requeries_completed_pending_provider",found!=commandCapabilities_.end()&&
                    !found->second.slowStateCompleted&&!headlessCompletedPendingCommand_&&
                    !selectionStateDirty_&&!namespaceDirty_,
                    L"Fresh native generation discarded the previous completed pending marker");
                cancelCommandStates();
                commandCapabilities_=std::move(savedCapabilities);
                selectionStateDirty_=namespaceDirty_=true;
                updateCommands();
            }
            {
                ComPtr<ExplorerApp> noView;
                noView.Attach(new ExplorerApp(instance_, true));
                const bool original = noView->checkboxes_;
                const auto rejected = noView->execute(Checkboxes);
                check("checkbox_command_without_native_view_preserves_state", rejected == E_UNEXPECTED &&
                    noView->checkboxes_ == original && !noView->window());
            }
            DWORD originalCheckboxFlags = 0;
            const auto originalCheckboxRead = folderView_->GetCurrentFolderFlags(&originalCheckboxFlags);
            auto checkboxResult = execute(Checkboxes);
            DWORD currentFlags = 0;
            const auto checkboxRead = folderView_->GetCurrentFolderFlags(&currentFlags);
            check("checkbox_selection_flag", SUCCEEDED(originalCheckboxRead) && SUCCEEDED(checkboxResult) &&
                SUCCEEDED(checkboxRead) && ((currentFlags ^ originalCheckboxFlags) & FWF_CHECKSELECT) &&
                checkboxes_ == ((currentFlags & FWF_CHECKSELECT) != 0),
                L"original/actual flags=" + std::to_wstring(originalCheckboxFlags) + L"/" + std::to_wstring(currentFlags));
            execute(Checkboxes);
            {
                const auto enabledOutsideHost = folderView_->SetCurrentFolderFlags(FWF_CHECKSELECT, FWF_CHECKSELECT);
                const auto toggledNative = execute(Checkboxes);
                DWORD observed = FWF_CHECKSELECT;
                const auto read = folderView_->GetCurrentFolderFlags(&observed);
                check("checkbox_command_toggles_actual_native_flags", SUCCEEDED(enabledOutsideHost) &&
                    SUCCEEDED(toggledNative) && SUCCEEDED(read) && !(observed & FWF_CHECKSELECT) && !checkboxes_);
                const auto enabled = folderView_->SetCurrentFolderFlags(FWF_CHECKSELECT, FWF_CHECKSELECT);
                // Deferred native view work observes this independent change;
                // it must not write an older host cache back to the provider.
                SendMessageW(window_, WM_APP + 2, 0, 0); // App's deferred native-view-ready message.
                DWORD deferredFlags = 0;
                const auto deferredRead = folderView_->GetCurrentFolderFlags(&deferredFlags);
                check("checkbox_deferred_view_preserves_independent_native_flags", SUCCEEDED(enabled) &&
                    SUCCEEDED(deferredRead) && (deferredFlags & FWF_CHECKSELECT) && checkboxes_);
                const auto checkboxTab = selectNativeTab(RibbonViewTab);
                updateCommands();
                bool nativeChecked = false;
                const auto nativeCheckboxRead = nativeBoolean(Checkboxes, UI_PKEY_BooleanValue, nativeChecked);
                check("checkbox_ribbon_state_tracks_native_view_change", SUCCEEDED(enabled) && SUCCEEDED(checkboxTab) &&
                    checkboxes_ && nativeCheckboxRead && nativeChecked,
                    L"private View tab=" + hresultMessage(checkboxTab) + L"; actual/host/Ribbon checked=" +
                    std::to_wstring((deferredFlags & FWF_CHECKSELECT) != 0) + L"/" + std::to_wstring(checkboxes_) + L"/" +
                    std::to_wstring(nativeChecked) + L"; Ribbon property read=" + std::to_wstring(nativeCheckboxRead));
                folderView_->SetCurrentFolderFlags(FWF_CHECKSELECT, originalCheckboxFlags & FWF_CHECKSELECT);
                updateCommands();
                selectNativeTab(RibbonHomeTab);
            }
            {
                PrivatePresentation presentation(window_, true);
                HWND nativeView=nullptr;const auto windowRead=view_->GetWindow(&nativeView);
                const auto previousFocus=GetFocus();const auto originalView=preferences_.view;
                FOLDERVIEWMODE originalNativeMode=FVM_AUTO;int originalNativeSize=0;
                const auto originalRead=folderView_->GetViewModeAndIconSize(&originalNativeMode,&originalNativeSize);
                Pidl originalLocation(ILCloneFull(currentPidl_.get()));const auto originalHistory=history_.size();
                int originalSelection=-1;folderView_->ItemCount(SVGIO_SELECTION,&originalSelection);
                const auto detailsResult=setView(ViewMode::Details);
                // Read only the native control/provider focus state. No item
                // enumeration, focus seeding, retries or keyboard input is
                // added to the original region and toolbar checks.
                const auto focusDiagnosticStarted=GetTickCount64();
                const auto focusDiagnosticDeadline=focusDiagnosticStarted+5000;
                const auto windowDiagnostic=[](HWND target) {
                    wchar_t type[128]{};if(target)GetClassNameW(target,type,128);
                    return std::to_wstring(reinterpret_cast<UINT_PTR>(target))+L"/"+type;
                };
                const auto accessibleDiagnostic=[&](HWND target) {
                    if(GetTickCount64()>=focusDiagnosticDeadline)return std::wstring(L"diagnostic budget exhausted");
                    ComPtr<IAccessible> root;
                    const auto opened=AccessibleObjectFromWindow(target,static_cast<DWORD>(OBJID_CLIENT),IID_PPV_ARGS(&root));
                    std::wstring result=L"HWND="+windowDiagnostic(target)+L"; client="+hresultMessage(opened);
                    if(FAILED(opened)||!root)return result;
                    const auto roleState=[&](IAccessible* object,LONG child) {
                        VARIANT argument{};argument.vt=VT_I4;argument.lVal=child;
                        VARIANT role{},state{};
                        const auto roleRead=object->get_accRole(argument,&role);
                        const auto stateRead=object->get_accState(argument,&state);
                        HWND owner=nullptr;const auto ownerRead=WindowFromAccessibleObject(object,&owner);
                        const auto detail=L"child="+std::to_wstring(child)+L"/role="+hresultMessage(roleRead)+L"/"+
                            std::to_wstring(role.vt)+L"/"+std::to_wstring(role.vt==VT_I4?role.lVal:0)+L"/state="+
                            hresultMessage(stateRead)+L"/"+std::to_wstring(state.vt)+L"/"+
                            std::to_wstring(state.vt==VT_I4?state.lVal:0)+L"/owner="+hresultMessage(ownerRead)+L"/"+
                            windowDiagnostic(owner);
                        VariantClear(&role);VariantClear(&state);return detail;
                    };
                    result+=L"; root "+roleState(root.Get(),CHILDID_SELF);
                    LONG children=0;const auto childrenRead=root->get_accChildCount(&children);
                    result+=L"; child count="+hresultMessage(childrenRead)+L"/"+std::to_wstring(children);
                    VARIANT focused{};const auto focusRead=root->get_accFocus(&focused);
                    result+=L"; accFocus="+hresultMessage(focusRead)+L"/"+std::to_wstring(focused.vt);
                    if(SUCCEEDED(focusRead)&&focused.vt==VT_I4)result+=L"/"+roleState(root.Get(),focused.lVal);
                    else if(SUCCEEDED(focusRead)&&focused.vt==VT_DISPATCH&&focused.pdispVal) {
                        ComPtr<IAccessible> child;const auto childRead=focused.pdispVal->QueryInterface(IID_PPV_ARGS(&child));
                        result+=L"/QI="+hresultMessage(childRead);
                        if(SUCCEEDED(childRead)&&child)result+=L"/"+roleState(child.Get(),CHILDID_SELF);
                    }
                    VariantClear(&focused);return result;
                };
                const auto focusDiagnostic=[&] {
                    if(GetTickCount64()>=focusDiagnosticDeadline)return std::wstring(L"diagnostic budget exhausted");
                    const auto region=currentFocusRegion();const auto focus=GetFocus();
                    return L"region="+(region?std::to_wstring(static_cast<unsigned>(*region)):L"none")+
                        L"; focus="+windowDiagnostic(focus)+L"; focus client=["+
                        (focus?accessibleDiagnostic(focus):L"none")+L"]";
                };
                std::wstring focusDetail=L"initial before UIActivate=["+focusDiagnostic()+L"]; ";
                const auto initialFocusActivation=view_->UIActivate(SVUIA_ACTIVATE_FOCUS);
                focusDetail+=L"initial UIActivate="+hresultMessage(initialFocusActivation)+L"; after=["+focusDiagnostic()+L"]; ";
                FOLDERVIEWMODE focusNativeMode=FVM_AUTO;int focusNativeSize=0;
                const auto focusModeRead=folderView_->GetViewModeAndIconSize(&focusNativeMode,&focusNativeSize);
                focusDetail+=L"mode="+hresultMessage(focusModeRead)+L"/"+std::to_wstring(focusNativeMode)+L"/"+
                    std::to_wstring(focusNativeSize)+L"; native view=["+accessibleDiagnostic(nativeView)+L"]; ";
                struct NativeFocusDiagnostic {
                    const decltype(accessibleDiagnostic)* read;std::wstring* detail;
                    ULONGLONG deadline;unsigned visited=0,recorded=0;
                } focusDiscovery{&accessibleDiagnostic,&focusDetail,focusDiagnosticDeadline};
                if(nativeView)EnumChildWindows(nativeView,[](HWND child,LPARAM context)->BOOL {
                    auto& state=*reinterpret_cast<NativeFocusDiagnostic*>(context);
                    if(++state.visited>64||GetTickCount64()>=state.deadline)return FALSE;
                    wchar_t type[64]{};GetClassNameW(child,type,64);
                    if(_wcsicmp(type,L"DirectUIHWND")==0||_wcsicmp(type,WC_HEADERW)==0) {
                        *state.detail+=L"native provider=["+(*state.read)(child)+L"]; ";
                        if(++state.recorded>=8)return FALSE;
                    }
                    return TRUE;
                },reinterpret_cast<LPARAM>(&focusDiscovery));
                constexpr std::array forwardRegions{FocusRegion::Sorting,FocusRegion::Status,FocusRegion::Toolbar,
                    FocusRegion::Navigation,FocusRegion::FolderView};
                constexpr std::array reverseRegions{FocusRegion::Navigation,FocusRegion::Toolbar,FocusRegion::Status,
                    FocusRegion::Sorting,FocusRegion::FolderView};
                bool forward=true,reverse=true;HWND actualTree=nullptr;
                for(const auto expected:forwardRegions) {
                    focusDetail+=L"forward before=["+focusDiagnostic()+L"]; ";
                    const auto focused=execute(FocusNext);const auto actual=currentFocusRegion();
                    forward=forward&&focused==S_OK&&actual==expected;
                    if(expected==FocusRegion::Navigation)actualTree=GetFocus();
                    focusDetail+=L"forward="+std::to_wstring(static_cast<unsigned>(expected))+L"/"+hresultMessage(focused)+
                        L"/actual="+(actual?std::to_wstring(static_cast<unsigned>(*actual)):L"none")+L"; after=["+focusDiagnostic()+L"]; ";
                }
                for(const auto expected:reverseRegions) {
                    focusDetail+=L"reverse before=["+focusDiagnostic()+L"]; ";
                    const auto focused=execute(FocusPrevious);const auto actual=currentFocusRegion();
                    reverse=reverse&&focused==S_OK&&actual==expected;
                    focusDetail+=L"reverse="+std::to_wstring(static_cast<unsigned>(expected))+L"/"+hresultMessage(focused)+
                        L"/actual="+(actual?std::to_wstring(static_cast<unsigned>(*actual)):L"none")+L"; after=["+focusDiagnostic()+L"]; ";
                }
                focusDetail+=L"diagnostic HWNDs visited/recorded="+std::to_wstring(focusDiscovery.visited)+L"/"+
                    std::to_wstring(focusDiscovery.recorded)+L"; diagnostic elapsed_ms="+
                    std::to_wstring(GetTickCount64()-focusDiagnosticStarted)+L"; ";
                check("f6_native_windows10_content_header_status_toolbar_tree_and_reverse",presentation.ready&&
                    SUCCEEDED(windowRead)&&SUCCEEDED(originalRead)&&detailsResult==S_OK&&forward&&reverse,focusDetail);
                wchar_t treeClass[64]{};if(actualTree)GetClassNameW(actualTree,treeClass,64);
                check("f6_navigation_target_is_actual_owned_namespace_tree",actualTree&&
                    _wcsicmp(treeClass,WC_TREEVIEWW)==0&&IsChild(window_,actualTree));

                struct ToolbarStop{HWND control;int button;};std::vector<ToolbarStop> stops;
                auto appendStops=[&](HWND bar) {
                    RECT client{};GetClientRect(bar,&client);const auto count=SendMessageW(bar,TB_BUTTONCOUNT,0,0);
                    for(int index=0;index<count;++index) {
                        TBBUTTON button{};RECT item{},visible{};
                        if(SendMessageW(bar,TB_GETBUTTON,index,reinterpret_cast<LPARAM>(&button))&&
                           (button.fsState&TBSTATE_ENABLED)&&!(button.fsState&TBSTATE_HIDDEN)&&!(button.fsStyle&BTNS_SEP)&&
                           SendMessageW(bar,TB_GETITEMRECT,index,reinterpret_cast<LPARAM>(&item))&&IntersectRect(&visible,&client,&item))
                            stops.push_back({bar,index});
                    }
                };
                appendStops(nav_);const auto enabledNavigation=stops.size();appendStops(breadcrumbs_);
                appendStops(addressActions_);stops.push_back({search_,-1});
                const auto atStop=[](const ToolbarStop& stop) {
                    return GetFocus()==stop.control&&(stop.button<0||SendMessageW(stop.control,TB_GETHOTITEM,0,0)==stop.button);
                };
                execute(FocusPrevious);execute(FocusPrevious); // Content -> tree -> first active toolbar button.
                bool tabs=!stops.empty()&&enabledNavigation>0&&atStop(stops.front());
                bool tabMessages=true;
                for(size_t index=1;index<=stops.size();++index) {
                    MSG tab{};tab.hwnd=GetFocus();tab.message=WM_KEYDOWN;tab.wParam=VK_TAB;
                    const auto processed=preprocess(tab);tabMessages=tabMessages&&processed;
                    tabs=tabs&&atStop(stops[index%stops.size()]);
                }
                bool backwardsTabs=tabs;
                for(size_t step=1;step<=stops.size();++step) {
                    const auto moved=cycleToolbarFocus(true);
                    backwardsTabs=backwardsTabs&&moved==S_OK&&atStop(stops[(stops.size()-step)%stops.size()]);
                }
                check("toolbar_tab_visits_each_enabled_native_button_breadcrumb_refresh_search",tabs&&tabMessages,
                    L"Native stops="+std::to_wstring(stops.size())+L"; enabled navigation="+std::to_wstring(enabledNavigation));
                check("toolbar_shift_tab_reverses_actual_native_button_cycle",backwardsTabs);
                check("toolbar_native_controls_are_tab_stops",(GetWindowLongPtrW(nav_,GWL_STYLE)&WS_TABSTOP)&&
                    (GetWindowLongPtrW(breadcrumbs_,GWL_STYLE)&WS_TABSTOP)&&(GetWindowLongPtrW(addressActions_,GWL_STYLE)&WS_TABSTOP));
                // Begin owned thread-only history-key dispatcher regression.
                {
                    struct KeyboardRestore {
                        std::array<BYTE,256> original{};
                        std::wstring& error; std::wstring savedError;
                        HWND focus = GetFocus(); bool ready = false, restored = false;
                        explicit KeyboardRestore(std::wstring& value) : error(value), savedError(value) {
                            MSG queued{}; PeekMessageW(&queued,nullptr,0,0,PM_NOREMOVE);
                            ready = GetKeyboardState(original.data()) != FALSE;
                        }
                        bool chord(unsigned modifiers) {
                            if (!ready) return false;
                            auto requested = original;
                            constexpr std::array<UINT,9> modifierKeys{VK_CONTROL,VK_LCONTROL,VK_RCONTROL,
                                VK_SHIFT,VK_LSHIFT,VK_RSHIFT,VK_MENU,VK_LMENU,VK_RMENU};
                            for (const UINT key : modifierKeys) requested[key] &= 1;
                            for (const auto pair : {std::pair{1u,UINT(VK_CONTROL)},std::pair{2u,UINT(VK_SHIFT)},std::pair{4u,UINT(VK_MENU)}})
                                if (modifiers & pair.first) requested[pair.second] |= 0x80;
                            std::array<BYTE,256> actual{};
                            return SetKeyboardState(requested.data()) && GetKeyboardState(actual.data()) && actual == requested &&
                                ((GetKeyState(VK_CONTROL)&0x8000)!=0) == ((modifiers&1)!=0) &&
                                ((GetKeyState(VK_SHIFT)&0x8000)!=0) == ((modifiers&2)!=0) &&
                                ((GetKeyState(VK_MENU)&0x8000)!=0) == ((modifiers&4)!=0);
                        }
                        bool restore() {
                            std::array<BYTE,256> actual{};
                            const bool keys = ready && SetKeyboardState(original.data()) &&
                                GetKeyboardState(actual.data()) && actual == original;
                            if (focus && IsWindow(focus)) SetFocus(focus); else SetFocus(nullptr);
                            error = savedError;
                            restored = keys && GetFocus() == (focus && IsWindow(focus) ? focus : nullptr) && error == savedError;
                            return restored;
                        }
                        ~KeyboardRestore() { if (!restored) restore(); }
                    } keyboard(lastError_);
                    struct AddressVisibilityRestore {
                        bool& editing; bool original; HWND address, breadcrumbs; bool addressVisible, breadcrumbsVisible;
                        void restore() {
                            editing = original; ShowWindow(address,addressVisible?SW_SHOW:SW_HIDE);
                            ShowWindow(breadcrumbs,breadcrumbsVisible?SW_SHOW:SW_HIDE);
                        }
                        ~AddressVisibilityRestore() { restore(); }
                    } addressVisibility{addressEditing_,addressEditing_,address_,breadcrumbs_,
                        IsWindowVisible(address_)!=FALSE,IsWindowVisible(breadcrumbs_)!=FALSE};
                    struct OwnedKeyboardControls {
                        HWND edit=nullptr, richEdit=nullptr, auxiliary=nullptr, auxiliaryEdit=nullptr;
                        HMODULE module=nullptr;
                        void release() {
                            if (edit) DestroyWindow(edit); if (richEdit) DestroyWindow(richEdit);
                            if (auxiliary) DestroyWindow(auxiliary); if (module) FreeLibrary(module);
                            edit=richEdit=auxiliary=auxiliaryEdit=nullptr; module=nullptr;
                        }
                        ~OwnedKeyboardControls() { release(); }
                    } controls;
                    const auto deadline = GetTickCount64()+10000;
                    const auto withinBudget = [&] { return GetTickCount64()<deadline; };
                    unsigned modifierReadbacks=0, modifierFailures=0;
                    const auto setChord = [&](unsigned mask) {
                        const bool exact=keyboard.chord(mask);
                        if (exact) ++modifierReadbacks; else ++modifierFailures;
                        return exact;
                    };
                    const auto selectionIds = [&](std::set<NativeFileIdentity>& values,int& selectedCount) {
                        values.clear(); selectedCount=-1;
                        auto status=folderView_->ItemCount(SVGIO_SELECTION,&selectedCount);
                        if (FAILED(status)) return status;
                        if (selectedCount<0 || selectedCount>4096) return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
                        if (!selectedCount) return S_OK;
                        ComPtr<IShellItemArray> items;
                        status=folderView_->Items(SVGIO_SELECTION,IID_PPV_ARGS(&items));
                        DWORD actualCount=0;
                        if (SUCCEEDED(status)) status=items?items->GetCount(&actualCount):E_UNEXPECTED;
                        if (SUCCEEDED(status) && actualCount!=static_cast<DWORD>(selectedCount)) status=E_UNEXPECTED;
                        if (SUCCEEDED(status)) status=nativeArrayIdentities(items.Get(),values);
                        return SUCCEEDED(status) && values.size()!=actualCount ? E_UNEXPECTED : status;
                    };
                    struct OwnedFileStamp {
                        NativeFileIdentity identity; DWORD attributes=0; ULONGLONG size=0,creation=0,write=0;
                        std::string bytes;
                        bool operator==(const OwnedFileStamp&) const = default;
                    };
                    using OwnedFileSnapshot=std::map<std::filesystem::path,OwnedFileStamp>;
                    const auto fileSnapshot = [&](OwnedFileSnapshot& output) {
                        output.clear(); size_t bytes=0; std::error_code error;
                        std::filesystem::recursive_directory_iterator item(fixture,error), end;
                        if (error) return HRESULT_FROM_WIN32(static_cast<DWORD>(error.value()));
                        for (;item!=end;item.increment(error)) {
                            if (error) return HRESULT_FROM_WIN32(static_cast<DWORD>(error.value()));
                            if (!withinBudget()) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                            if (output.size()>=4096) return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
                            WIN32_FILE_ATTRIBUTE_DATA basic{};
                            if (!GetFileAttributesExW(item->path().c_str(),GetFileExInfoStandard,&basic))
                                return HRESULT_FROM_WIN32(GetLastError());
                            if (basic.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) return E_ACCESSDENIED;
                            FILE_ID_INFO raw{}; auto status=nativeFileIdentity(item->path(),raw);
                            if (FAILED(status)) return status;
                            OwnedFileStamp stamp; stamp.identity.first=raw.VolumeSerialNumber;
                            std::copy(std::begin(raw.FileId.Identifier),std::end(raw.FileId.Identifier),stamp.identity.second.begin());
                            stamp.attributes=basic.dwFileAttributes;
                            stamp.size=(ULONGLONG(basic.nFileSizeHigh)<<32)|basic.nFileSizeLow;
                            stamp.creation=(ULONGLONG(basic.ftCreationTime.dwHighDateTime)<<32)|basic.ftCreationTime.dwLowDateTime;
                            stamp.write=(ULONGLONG(basic.ftLastWriteTime.dwHighDateTime)<<32)|basic.ftLastWriteTime.dwLowDateTime;
                            if (!(basic.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)) {
                                if (stamp.size>1024*1024 || bytes>1024*1024-stamp.size) return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
                                std::ifstream input(item->path(),std::ios::binary);
                                if (!input.good()) return E_FAIL;
                                stamp.bytes.assign(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>());
                                if (stamp.bytes.size()!=stamp.size) return E_UNEXPECTED;
                                bytes+=stamp.bytes.size();
                            }
                            if (!output.emplace(item->path().lexically_relative(fixture),std::move(stamp)).second) return E_UNEXPECTED;
                        }
                        return error?HRESULT_FROM_WIN32(static_cast<DWORD>(error.value())):S_OK;
                    };
                    const auto keyLocation=Pidl(ILCloneFull(currentPidl_.get()));
                    const auto keyHistoryIndex=historyIndex_; std::vector<Pidl> keyHistory;
                    bool historyReady=keyLocation!=nullptr;
                    for (const auto& location:history_) {
                        keyHistory.emplace_back(ILCloneFull(location.get()));
                        historyReady=historyReady&&keyHistory.back()!=nullptr;
                    }
                    std::set<NativeFileIdentity> beforeSelection; int beforeCount=-1;
                    const auto selectionRead=selectionIds(beforeSelection,beforeCount);
                    OwnedFileSnapshot beforeFiles,afterFiles;
                    const auto filesRead=fileSnapshot(beforeFiles);
                    const bool prerequisites=headless_&&presentation.ready&&keyboard.ready&&historyReady&&
                        SUCCEEDED(selectionRead)&&SUCCEEDED(filesRead)&&SUCCEEDED(windowRead)&&nativeView&&withinBudget();
                    check("history_key_fixture_private_thread_full_selection_and_owned_files",prerequisites,
                        L"Selection="+hresultMessage(selectionRead)+L"/"+std::to_wstring(beforeCount)+
                        L"; files="+hresultMessage(filesRead)+L"/"+std::to_wstring(beforeFiles.size())+
                        L"; keyboard snapshot="+std::to_wstring(keyboard.ready));
                    const auto identitiesPreserved = [&] {
                        if (!currentPidl_||!keyLocation||!ILIsEqual(currentPidl_.get(),keyLocation.get())||
                            historyIndex_!=keyHistoryIndex||history_.size()!=keyHistory.size()) return false;
                        for (size_t index=0;index<keyHistory.size();++index)
                            if (!history_[index]||!ILIsEqual(history_[index].get(),keyHistory[index].get())) return false;
                        std::set<NativeFileIdentity> afterSelection; int afterCount=-1;
                        return SUCCEEDED(selectionIds(afterSelection,afterCount))&&afterCount==beforeCount&&afterSelection==beforeSelection;
                    };
                    const auto denied=L"Command: "+hresultMessage(E_ACCESSDENIED);
                    struct Target { HWND window; const char* check; };
                    const std::array targets{
                        Target{nav_,"history_keys_actual_navigation_toolbar_headless_guard"},
                        Target{breadcrumbs_,"history_keys_actual_breadcrumb_toolbar_headless_guard"},
                        Target{addressActions_,"history_keys_actual_refresh_toolbar_headless_guard"},
                        Target{nativeView,"history_keys_actual_native_view_headless_guard"}};
                    for (const auto& target:targets) {
                        bool guarded=prerequisites; unsigned attempts=0; std::wstring detail;
                        for (const UINT key:{UINT('Z'),UINT('Y')}) {
                            if (!guarded||!withinBudget()||!setChord(1)) { guarded=false; break; }
                            SetFocus(target.window);
                            const auto focus=GetFocus();
                            const bool focused=focus==target.window||(target.window==nativeView&&IsChild(nativeView,focus));
                            lastError_.clear(); MSG message{};message.hwnd=target.window;message.message=WM_KEYDOWN;message.wParam=key;
                            const bool consumed=focused&&preprocess(message); ++attempts;
                            const bool exactError=lastError_==denied, preserved=identitiesPreserved();
                            guarded=guarded&&consumed&&exactError&&preserved;
                            detail+=L"key="+std::to_wstring(key)+L"/focus="+std::to_wstring(focused)+
                                L"/consumed="+std::to_wstring(consumed)+L"/exact denial="+std::to_wstring(exactError)+
                                L"/identities="+std::to_wstring(preserved)+L"; ";
                        }
                        check(target.check,guarded&&attempts==2,detail);
                    }
                    if (prerequisites&&withinBudget()) controls.edit=CreateWindowExW(0,L"EDIT",L"owned history-key edit",WS_CHILD|WS_VISIBLE,
                        -100,-100,1,1,window_,nullptr,instance_,nullptr);
                    if (prerequisites&&withinBudget()) controls.module=LoadLibraryExW(L"Msftedit.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
                    if (controls.module) controls.richEdit=CreateWindowExW(0,L"RICHEDIT50W",L"owned history-key rich edit",WS_CHILD|WS_VISIBLE,
                        -100,-100,1,1,window_,nullptr,instance_,nullptr);
                    const auto editing=prerequisites&&withinBudget()?execute(Address):E_ACCESSDENIED;
                    const std::array edits{Target{address_,"history_keys_address_edit_remain_text_control_owned"},
                        Target{search_,"history_keys_search_edit_remain_text_control_owned"},
                        Target{controls.edit,"history_keys_actual_edit_remain_text_control_owned"},
                        Target{controls.richEdit,"history_keys_actual_richedit_remain_text_control_owned"}};
                    for (const auto& target:edits) {
                        bool native=prerequisites&&SUCCEEDED(editing)&&target.window; unsigned attempts=0;
                        for (const UINT key:{UINT('Z'),UINT('Y')}) {
                            if (!native||!withinBudget()||!setChord(1)) { native=false; break; }
                            SetFocus(target.window); const auto text=textOf(target.window);lastError_=L"owned keyboard sentinel";
                            MSG message{};message.hwnd=target.window;message.message=WM_KEYDOWN;message.wParam=key;
                            native=GetFocus()==target.window&&!preprocess(message)&&lastError_==L"owned keyboard sentinel"&&
                                textOf(target.window)==text&&identitiesPreserved(); ++attempts;
                        }
                        check(target.check,native&&attempts==2);
                    }
                    addressVisibility.restore();
                    if (prerequisites&&withinBudget()) controls.auxiliary=CreateWindowExW(0,L"STATIC",L"owned auxiliary history-key host",WS_POPUP|WS_VISIBLE,
                        -100,-100,1,1,window_,nullptr,instance_,nullptr);
                    if (controls.auxiliary) controls.auxiliaryEdit=CreateWindowExW(0,L"EDIT",L"owned auxiliary history-key edit",WS_CHILD|WS_VISIBLE,
                        0,0,1,1,controls.auxiliary,nullptr,instance_,nullptr);
                    bool auxiliary=prerequisites&&controls.auxiliary&&controls.auxiliaryEdit; unsigned auxiliaryAttempts=0;
                    for (const UINT key:{UINT('Z'),UINT('Y')}) {
                        if (!auxiliary||!withinBudget()||!setChord(1)) { auxiliary=false; break; }
                        SetFocus(controls.auxiliaryEdit);lastError_=L"owned keyboard sentinel";
                        MSG message{};message.hwnd=controls.auxiliaryEdit;message.message=WM_KEYDOWN;message.wParam=key;
                        auxiliary=GetFocus()==controls.auxiliaryEdit&&!preprocess(message)&&lastError_==L"owned keyboard sentinel";
                        message.hwnd=nav_;
                        auxiliary=auxiliary&&!preprocess(message)&&lastError_==L"owned keyboard sentinel"&&identitiesPreserved();++auxiliaryAttempts;
                    }
                    check("history_keys_auxiliary_message_and_outside_focus_do_not_route",auxiliary&&auxiliaryAttempts==2);
                    bool modifiers=prerequisites; unsigned modifierAttempts=0;
                    if (prerequisites) SetFocus(nav_);
                    for (unsigned mask=0;mask<8;++mask) if (mask!=1) for (const UINT key:{UINT('Z'),UINT('Y')}) {
                        if (!modifiers||!withinBudget()||!setChord(mask)) { modifiers=false; continue; }
                        lastError_=L"owned keyboard sentinel"; MSG message{};message.hwnd=nav_;message.message=WM_KEYDOWN;message.wParam=key;
                        modifiers=GetFocus()==nav_&&!preprocess(message)&&lastError_==L"owned keyboard sentinel"; ++modifierAttempts;
                    }
                    check("history_keys_actual_host_modifier_and_altgr_boundaries",modifiers&&modifierAttempts==14&&identitiesPreserved());
                    check("history_keys_thread_modifier_state_exact_readback",prerequisites&&modifierReadbacks==32&&modifierFailures==0,
                        L"Exact full keyboard/modifier readbacks="+std::to_wstring(modifierReadbacks)+L"; failures="+std::to_wstring(modifierFailures));
                    const auto afterFilesRead=fileSnapshot(afterFiles);
                    check("history_keys_preserve_all_owned_file_identities_bytes_attributes_and_timestamps",
                        prerequisites&&SUCCEEDED(afterFilesRead)&&beforeFiles==afterFiles,hresultMessage(afterFilesRead));
                    check("history_keys_bounded_private_dispatch",prerequisites&&withinBudget());
                    addressVisibility.restore();
                    controls.release();
                    check("history_keys_restore_exact_thread_keyboard_focus_and_error",keyboard.restore());
                }
                // End owned thread-only history-key dispatcher regression.
                execute(Address);const bool addressFocused=GetFocus()==address_&&addressEditing_;
                const auto addressTab=cycleToolbarFocus(false);const bool refreshFocused=GetFocus()==addressActions_;
                const auto searchTab=cycleToolbarFocus(false);const bool searchFocused=GetFocus()==search_;
                check("toolbar_address_edit_tabs_to_refresh_then_search",addressFocused&&addressTab==S_OK&&refreshFocused&&
                    searchTab==S_OK&&searchFocused&&!addressEditing_);
                check("direct_search_shortcut_retains_real_search_edit",execute(FocusSearch)==S_OK&&GetFocus()==search_);
                const auto historyIndex=SendMessageW(nav_,TB_COMMANDTOINDEX,HistoryMenu,0);
                SetFocus(nav_);SendMessageW(nav_,TB_SETHOTITEM,historyIndex,0);
                MSG enter{};enter.hwnd=nav_;enter.message=WM_KEYDOWN;enter.wParam=VK_RETURN;
                const auto enterFocus=GetFocus();const auto enterHot=SendMessageW(nav_,TB_GETHOTITEM,0,0);TBBUTTON enterButton{};
                const auto enterButtonRead=enterHot>=0?SendMessageW(nav_,TB_GETBUTTON,enterHot,reinterpret_cast<LPARAM>(&enterButton)):FALSE;
                const bool enterControl=(GetKeyState(VK_CONTROL)&0x8000)!=0;
                const bool enterShift=(GetKeyState(VK_SHIFT)&0x8000)!=0;
                const bool enterAlt=(GetKeyState(VK_MENU)&0x8000)!=0;
                const bool enterProcessed=historyIndex>=0&&preprocess(enter);
                check("toolbar_enter_routes_real_button_through_headless_popup_guard",historyIndex>=0&&enterProcessed&&
                    currentPidl_&&originalLocation&&ILIsEqual(currentPidl_.get(),originalLocation.get())&&history_.size()==originalHistory,
                    L"before focus="+windowDiagnostic(enterFocus)+L"; owned="+
                    std::to_wstring(enterFocus==window_||IsChild(window_,enterFocus))+L"; ctrl/shift/alt="+
                    std::to_wstring(enterControl)+L"/"+std::to_wstring(enterShift)+L"/"+std::to_wstring(enterAlt)+
                    L"; history/hot/button read/id/state/style="+std::to_wstring(historyIndex)+L"/"+
                    std::to_wstring(enterHot)+L"/"+std::to_wstring(enterButtonRead)+L"/"+
                    std::to_wstring(enterButton.idCommand)+L"/"+std::to_wstring(enterButton.fsState)+L"/"+
                    std::to_wstring(enterButton.fsStyle)+L"; consumed="+std::to_wstring(enterProcessed)+L"; after focus="+
                    windowDiagnostic(GetFocus())+L"; error="+lastError_);
                if(!stops.empty()&&stops.front().button>=0) {
                    TBBUTTON button{};SendMessageW(nav_,TB_GETBUTTON,stops.front().button,reinterpret_cast<LPARAM>(&button));
                    SendMessageW(nav_,TB_ENABLEBUTTON,button.idCommand,FALSE);
                    SetFocus(search_);const auto skipped=cycleToolbarFocus(false);
                    check("toolbar_tab_skips_disabled_native_navigation_button",skipped==S_OK&&GetFocus()&&
                        !(GetFocus()==nav_&&SendMessageW(nav_,TB_GETHOTITEM,0,0)==stops.front().button));
                    SetFocus(nav_);SendMessageW(nav_,TB_SETHOTITEM,stops.front().button,0);
                    check("toolbar_enter_does_not_activate_disabled_native_button",!preprocess(enter)&&
                        currentPidl_&&originalLocation&&ILIsEqual(currentPidl_.get(),originalLocation.get()));
                    SendMessageW(nav_,TB_ENABLEBUTTON,button.idCommand,TRUE);
                }
                if(actualTree) {
                    EnableWindow(actualTree,FALSE);SetFocus(nav_);
                    const auto skipped=execute(FocusNext);
                    check("f6_skips_disabled_native_navigation_region",skipped==S_OK&&currentFocusRegion()==FocusRegion::FolderView);
                    EnableWindow(actualTree,TRUE);
                }
                const auto iconsResult=folderView_->SetViewModeAndIconSize(FVM_ICON,48);
                view_->UIActivate(SVUIA_ACTIVATE_FOCUS);
                const auto iconFocus=execute(FocusNext);
                check("f6_icon_view_skips_unavailable_native_details_header",iconsResult==S_OK&&iconFocus==S_OK&&
                    currentFocusRegion()==FocusRegion::Status);
                HWND auxiliary=CreateWindowExW(0,L"EDIT",L"Owned auxiliary edit",0,0,0,1,1,
                    HWND_MESSAGE,nullptr,instance_,nullptr);
                MSG foreignKey{};foreignKey.hwnd=auxiliary;foreignKey.message=WM_KEYDOWN;foreignKey.wParam=VK_F6;
                const auto beforeForeign=GetFocus();
                bool foreignIgnored=auxiliary&&!preprocess(foreignKey)&&GetFocus()==beforeForeign;
                foreignKey.wParam=VK_TAB;foreignIgnored=foreignIgnored&&!preprocess(foreignKey)&&GetFocus()==beforeForeign;
                check("keyboard_messages_for_auxiliary_window_remain_native",foreignIgnored);
                if(auxiliary)DestroyWindow(auxiliary);
                int finalSelection=-1;folderView_->ItemCount(SVGIO_SELECTION,&finalSelection);
                check("focus_navigation_preserves_native_selection_and_current_identity",originalSelection>=0&&
                    finalSelection==originalSelection&&currentPidl_&&originalLocation&&
                    ILIsEqual(currentPidl_.get(),originalLocation.get())&&history_.size()==originalHistory);
                if(SUCCEEDED(originalRead))folderView_->SetViewModeAndIconSize(originalNativeMode,originalNativeSize);
                preferences_.view=originalView;namespaceDirty_=selectionStateDirty_=true;updateCommands();
                if(previousFocus&&IsWindow(previousFocus))SetFocus(previousFocus);
                else view_->UIActivate(SVUIA_ACTIVATE_NOFOCUS);
                bool inputVisible=true,unchanged=false;
                check("f6_keyboard_checks_preserve_input_desktop_isolation",presentation.desktop&&
                    SUCCEEDED(presentation.desktop->verifyIsolation(&unchanged))&&unchanged&&
                    SUCCEEDED(presentation.desktop->visibleWindowsOnInputDesktop(inputVisible))&&!inputVisible);
            }
            ComPtr<IColumnManager> columns;
            UINT columnCount = 0;
            const bool columnsAvailable = SUCCEEDED(folderView_.As(&columns)) &&
                SUCCEEDED(columns->GetColumnCount(CM_ENUM_VISIBLE, &columnCount)) && columnCount >= 1;
            check("native_details_columns", columnsAvailable, std::to_wstring(columnCount));
            std::vector<PROPERTYKEY> originalColumns(columnCount);
            if (columns && columnCount) {
                const auto columnsResult = columns->GetColumns(CM_ENUM_VISIBLE, originalColumns.data(), columnCount);
                auto sizing = sizeColumns();
                bool widthsValid = SUCCEEDED(columnsResult) && SUCCEEDED(sizing);
                for (const auto& key : originalColumns) {
                    CM_COLUMNINFO info{sizeof(info)}; info.dwMask = CM_MASK_WIDTH;
                    widthsValid = widthsValid && SUCCEEDED(columns->GetColumnInfo(key, &info)) && info.uWidth > 0 && info.uWidth < 100000;
                }
                check("autosize_native_details_columns", widthsValid);
            }
            RECT originalRect{}, fullscreenRect{}, restoredRect{};
            GetWindowRect(window_, &originalRect);
            const auto originalStyle = GetWindowLongPtrW(window_, GWL_STYLE);
            auto fullscreenResult = execute(Fullscreen);
            GetWindowRect(window_, &fullscreenRect);
            check("fullscreen_hidden_window", SUCCEEDED(fullscreenResult) && fullscreen_ &&
                  (GetWindowLongPtrW(window_, GWL_STYLE) & WS_OVERLAPPEDWINDOW) == 0 && !IsWindowVisible(window_) &&
                  fullscreenRect.right > fullscreenRect.left && fullscreenRect.bottom > fullscreenRect.top);
            fullscreenResult = execute(Fullscreen);
            GetWindowRect(window_, &restoredRect);
            check("fullscreen_restores_style_and_geometry", SUCCEEDED(fullscreenResult) && !fullscreen_ && !IsWindowVisible(window_) &&
                  GetWindowLongPtrW(window_, GWL_STYLE) == originalStyle && EqualRect(&originalRect, &restoredRect));
            const auto viewTabResult = selectNativeTab(RibbonViewTab);
            const bool columnsRegistered = commandRegistered(ColumnsMenu);
            const bool sizeRegistered = commandRegistered(SizeColumns);
            const bool hideRegistered = commandRegistered(HideSelected);
            check("view_commands_exposed", SUCCEEDED(viewTabResult) && columnsRegistered && sizeRegistered && hideRegistered,
                hresultMessage(viewTabResult) + L"; ColumnsMenu=" + std::to_wstring(ColumnsMenu) + L":" + std::to_wstring(columnsRegistered) +
                L"; SizeColumns=" + std::to_wstring(SizeColumns) + L":" + std::to_wstring(sizeRegistered) +
                L"; HideSelected=" + std::to_wstring(HideSelected) + L":" + std::to_wstring(hideRegistered));
            if (ribbon_.layout() == RibbonLayout::InstalledWindows10) probeNativeMenus({
                {"native_stock_sort_dropdown_hierarchy", L"Sort by"},
                {"native_stock_group_dropdown_hierarchy", L"Group by"},
                {"native_stock_columns_dropdown_hierarchy", L"Add columns"}});
            MINMAXINFO minimum{}; SendMessageW(window_, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&minimum));
            SetWindowPos(window_, nullptr, 0, 0, minimum.ptMinTrackSize.x, minimum.ptMinTrackSize.y, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            RECT clientBounds{}; GetClientRect(window_, &clientBounds);
            struct RibbonBounds { HWND host; RECT client; bool found = false; bool fits = true; } bounds{window_, clientBounds};
            EnumChildWindows(window_, [](HWND child, LPARAM value) -> BOOL {
                auto& state = *reinterpret_cast<RibbonBounds*>(value);
                wchar_t className[64]{}; GetClassNameW(child, className, static_cast<int>(std::size(className)));
                if (wcscmp(className, L"UIRibbonCommandBar") == 0) {
                    RECT rect{}; GetWindowRect(child, &rect);
                    MapWindowPoints(nullptr, state.host, reinterpret_cast<POINT*>(&rect), 2);
                    state.found = true;
                    state.fits = state.fits && rect.left >= state.client.left && rect.right <= state.client.right &&
                        rect.top >= state.client.top && rect.bottom <= state.client.bottom && rect.right > rect.left && rect.bottom > rect.top;
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&bounds));
            const bool controlsFit = bounds.found && bounds.fits && ribbon_.height() > 0 &&
                ribbon_.height() <= static_cast<UINT>(clientBounds.bottom);
            check("view_ribbon_fits_minimum_window", controlsFit);
            SetWindowPos(window_, nullptr, 0, 0, originalRect.right - originalRect.left, originalRect.bottom - originalRect.top,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            selectNativeTab(RibbonHomeTab);
            const auto nativeSelectionActivation = view_->UIActivate(SVUIA_ACTIVATE_NOFOCUS);
            struct SelectionReadback {
                HRESULT operation = E_PENDING;
                HRESULT stateStatus = E_PENDING;
                EXPCMDSTATE state = ECS_DISABLED;
                bool stateReady = false;
                HRESULT countStatus = E_PENDING;
                int count = -1;
                bool countReady = false;
            };
            auto waitForSelectionCount = [&](int expected, SelectionReadback& result) {
                result.countReady = pumpUntil([&] {
                    result.count = -1;
                    result.countStatus = folderView_->ItemCount(SVGIO_SELECTION, &result.count);
                    return SUCCEEDED(result.countStatus) && result.count == expected && !selectionStateDirty_;
                }, 5000);
                return result.countReady;
            };
            auto nativeSelectionOperation = [&](UINT command, int expected) {
                SelectionReadback result;
                const auto name = command == SelectAll ? L"Windows.SelectAll" :
                    command == SelectNone ? L"Windows.SelectNone" : L"Windows.InvertSelection";
                auto selectionStageStarted = GetTickCount64();
                updateNamespace();
                std::fprintf(stderr, "headless-selection command=%u phase=namespace elapsed_ms=%llu\n", command,
                    static_cast<unsigned long long>(GetTickCount64() - selectionStageStarted));
                std::fflush(stderr);
                selectionStageStarted = GetTickCount64();
                result.stateReady = pumpUntil([&] {
                    NamespaceCommandState state;
                    result.stateStatus = backgroundActions_.queryCommandState(name, &state, NamespaceMenuScope::Background);
                    result.state = state.state;
                    return (SUCCEEDED(result.stateStatus) && state.enabled()) ||
                        result.stateStatus == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) ||
                        result.stateStatus == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) ||
                        result.stateStatus == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
                        result.stateStatus == E_NOINTERFACE || result.stateStatus == E_NOTIMPL;
                }, 2000);
                std::fprintf(stderr, "headless-selection command=%u phase=provider elapsed_ms=%llu hresult=0x%08lX ready=%u\n", command,
                    static_cast<unsigned long long>(GetTickCount64() - selectionStageStarted),
                    static_cast<unsigned long>(result.stateStatus), result.stateReady ? 1u : 0u);
                std::fflush(stderr);
                // Preserve the actual operation HRESULT independently of the
                // readiness/count checks. Native view commands can complete
                // through queued selection-change notifications on this STA.
                selectionStageStarted = GetTickCount64();
                result.operation = execute(command);
                std::fprintf(stderr, "headless-selection command=%u phase=operation elapsed_ms=%llu hresult=0x%08lX\n", command,
                    static_cast<unsigned long long>(GetTickCount64() - selectionStageStarted),
                    static_cast<unsigned long>(result.operation));
                std::fflush(stderr);
                selectionStageStarted = GetTickCount64();
                waitForSelectionCount(expected, result);
                std::fprintf(stderr, "headless-selection command=%u phase=count elapsed_ms=%llu hresult=0x%08lX actual=%d expected=%d ready=%u\n", command,
                    static_cast<unsigned long long>(GetTickCount64() - selectionStageStarted),
                    static_cast<unsigned long>(result.countStatus), result.count, expected, result.countReady ? 1u : 0u);
                std::fflush(stderr);
                return result;
            };
            auto selectionReadbackDetail = [&](const SelectionReadback& result, int expected) {
                const auto phase = !result.stateReady ? L"provider-state" : FAILED(result.operation) ? L"operation" :
                    !result.countReady ? L"native-count" : L"complete";
                return std::wstring(L"phase=") + phase + L"; activation=" + hresultMessage(nativeSelectionActivation) +
                    L"; operation=" + hresultMessage(result.operation) + L"; stateStatus=" + hresultMessage(result.stateStatus) +
                    L"; nativeState=" + std::to_wstring(result.state) + L"; countStatus=" + hresultMessage(result.countStatus) +
                    L"; nativeCount=" + std::to_wstring(result.count) + L"; expectedCount=" + std::to_wstring(expected);
            };
            const auto initialAll = nativeSelectionOperation(SelectAll, 1002);
            ComPtr<IShellItemArray> selected;
            DWORD selectedCount = 0;
            if (SUCCEEDED(selection(selected))) selected->GetCount(&selectedCount);
            check("select_all", SUCCEEDED(nativeSelectionActivation) && initialAll.stateReady &&
                SUCCEEDED(initialAll.operation) && initialAll.countReady && selectedCount == 1002,
                selectionReadbackDetail(initialAll, 1002));
            const auto initialNone = nativeSelectionOperation(SelectNone, 0);
            check("select_none", initialNone.stateReady && SUCCEEDED(initialNone.operation) && initialNone.countReady,
                selectionReadbackDetail(initialNone, 0));
            const auto initialInvert = nativeSelectionOperation(Invert, 1002);
            selected.Reset(); selectedCount = 0;
            if (SUCCEEDED(selection(selected)) && selected) selected->GetCount(&selectedCount);
            check("invert_selection", initialInvert.stateReady && SUCCEEDED(initialInvert.operation) &&
                initialInvert.countReady && selectedCount == 1002, selectionReadbackDetail(initialInvert, 1002));
            const auto beforeSeedNone = nativeSelectionOperation(SelectNone, 0);
            // Test native selection identities, not only its cardinality. The
            // complement must preserve item focus and checkbox/view flags.
            ComPtr<IShellItemArray> allSelectionItems;
            std::set<NativeFileIdentity> allSelectionIds, seedIds, complementIds, restoredIds;
            auto seedStage = GetTickCount64();
            std::fprintf(stderr, "headless-selection phase=seed-identities begin\n"); std::fflush(stderr);
            auto identityResult = folderView_->Items(SVGIO_ALLVIEW, IID_PPV_ARGS(&allSelectionItems));
            if (SUCCEEDED(identityResult)) identityResult = nativeArrayIdentities(allSelectionItems.Get(), allSelectionIds);
            std::fprintf(stderr, "headless-selection phase=seed-identities elapsed_ms=%llu hresult=0x%08lX count=%zu\n",
                static_cast<unsigned long long>(GetTickCount64()-seedStage),static_cast<unsigned long>(identityResult),allSelectionIds.size()); std::fflush(stderr);
            seedStage = GetTickCount64();
            DWORD selectionOriginalFlags = 0;
            auto flagResult = folderView_->GetCurrentFolderFlags(&selectionOriginalFlags);
            if (SUCCEEDED(flagResult)) flagResult = folderView_->SetCurrentFolderFlags(FWF_CHECKSELECT, FWF_CHECKSELECT);
            DWORD selectionTestFlags = 0;
            if (SUCCEEDED(flagResult)) flagResult = folderView_->GetCurrentFolderFlags(&selectionTestFlags);
            std::fprintf(stderr, "headless-selection phase=seed-flags elapsed_ms=%llu hresult=0x%08lX\n",
                static_cast<unsigned long long>(GetTickCount64()-seedStage),static_cast<unsigned long>(flagResult)); std::fflush(stderr);
            seedStage = GetTickCount64();
            auto seedResult = folderView_->SelectItem(3, SVSI_SELECT | SVSI_FOCUSED | SVSI_NOTAKEFOCUS);
            for (const int index : {107, 631}) {
                if (SUCCEEDED(seedResult)) seedResult = folderView_->SelectItem(index, SVSI_SELECT | SVSI_NOTAKEFOCUS);
            }
            std::fprintf(stderr, "headless-selection phase=seed-operation elapsed_ms=%llu hresult=0x%08lX\n",
                static_cast<unsigned long long>(GetTickCount64()-seedStage),static_cast<unsigned long>(seedResult)); std::fflush(stderr);
            seedStage = GetTickCount64();
            SelectionReadback seedCount;
            const bool seedCountReady = waitForSelectionCount(3, seedCount);
            std::fprintf(stderr, "headless-selection phase=seed-readback elapsed_ms=%llu count=%d ready=%u dirty=%u\n",
                static_cast<unsigned long long>(GetTickCount64()-seedStage),seedCount.count,seedCountReady?1u:0u,selectionStateDirty_?1u:0u); std::fflush(stderr);
            selected.Reset();
            if (SUCCEEDED(seedResult)) seedResult = selection(selected);
            if (SUCCEEDED(seedResult)) seedResult = nativeArrayIdentities(selected.Get(), seedIds);
            int focusedBefore = -1, focusedAfter = -1;
            const auto focusResult = folderView_->GetFocusedItem(&focusedBefore);
            {
                // The preceding, unchanged mixed-selection fixture supplies
                // three real selected files and focused item 3. Exercise only
                // native focus here; never seed or rewrite their selection.
                PrivatePresentation presentation(window_,true);
                const auto focusStarted=GetTickCount64();
                const auto deadline=focusStarted+10000;
                const auto nativeView=view_;const auto nativeFolder=folderView_;
                HWND contentView=nullptr;const auto viewRead=nativeView->GetWindow(&contentView);
                struct ContentRoot {ComPtr<IAccessible> object;HWND window=nullptr;unsigned visited=0;} content;
                if(presentation.ready&&SUCCEEDED(viewRead))EnumChildWindows(contentView,[](HWND child,LPARAM context)->BOOL {
                    auto& root=*reinterpret_cast<ContentRoot*>(context);if(++root.visited>64)return FALSE;
                    wchar_t type[64]{};GetClassNameW(child,type,64);DWORD process=0;
                    if(_wcsicmp(type,L"DirectUIHWND")!=0||GetWindowThreadProcessId(child,&process)!=GetCurrentThreadId()||
                       process!=GetCurrentProcessId()||!IsWindowVisible(child)||!IsWindowEnabled(child))return TRUE;
                    ComPtr<IAccessible> object;VARIANT self{};self.vt=VT_I4;self.lVal=CHILDID_SELF;VARIANT role{};
                    const auto opened=AccessibleObjectFromWindow(child,static_cast<DWORD>(OBJID_CLIENT),IID_PPV_ARGS(&object));
                    const auto read=SUCCEEDED(opened)?object->get_accRole(self,&role):opened;
                    const bool list=SUCCEEDED(read)&&role.vt==VT_I4&&role.lVal==ROLE_SYSTEM_LIST;VariantClear(&role);
                    HWND owner=nullptr;if(list&&SUCCEEDED(WindowFromAccessibleObject(object.Get(),&owner))&&owner==child) {
                        root.object=object;root.window=child;return FALSE;
                    }
                    return TRUE;
                },reinterpret_cast<LPARAM>(&content));
                struct ThreadRestore {
                    std::array<BYTE,256> keys{};HWND focus=GetFocus();std::wstring& error;std::wstring savedError;
                    std::array<BYTE,256> expected{};
                    bool ready=false,restored=false;
                    explicit ThreadRestore(std::wstring& value):error(value),savedError(value) {
                        MSG queued{};PeekMessageW(&queued,nullptr,0,0,PM_NOREMOVE);ready=GetKeyboardState(keys.data())!=FALSE;
                        if(ready)expected=keys;
                    }
                    bool apply(unsigned mask) {
                        if(!ready)return false;auto requested=keys;
                        for(const UINT key:{VK_CONTROL,VK_LCONTROL,VK_RCONTROL,VK_SHIFT,VK_LSHIFT,VK_RSHIFT,VK_MENU,VK_LMENU,VK_RMENU})
                            requested[key]&=1;
                        if(mask&2)requested[VK_SHIFT]|=0x80;
                        expected=requested;
                        std::array<BYTE,256> actual{};
                        return SetKeyboardState(requested.data())&&GetKeyboardState(actual.data())&&actual==requested&&
                            !(GetKeyState(VK_CONTROL)&0x8000)&&((GetKeyState(VK_SHIFT)&0x8000)!=0)==((mask&2)!=0)&&
                            !(GetKeyState(VK_MENU)&0x8000);
                    }
                    std::wstring readback() const {
                        std::array<BYTE,256> actual{};const bool read=GetKeyboardState(actual.data())!=FALSE;
                        std::wstring detail=L"keyboard read/exact="+std::to_wstring(read)+L"/"+
                            std::to_wstring(read&&actual==expected)+L"; generic Ctrl/Shift/Alt="+
                            std::to_wstring(static_cast<unsigned short>(GetKeyState(VK_CONTROL)))+L"/"+
                            std::to_wstring(static_cast<unsigned short>(GetKeyState(VK_SHIFT)))+L"/"+
                            std::to_wstring(static_cast<unsigned short>(GetKeyState(VK_MENU)))+L"; changed bytes=";
                        if(read)for(size_t index=0;index<actual.size();++index)if(actual[index]!=expected[index])
                            detail+=std::to_wstring(index)+L":"+std::to_wstring(expected[index])+L"->"+std::to_wstring(actual[index])+L",";
                        return detail;
                    }
                    bool exact() const {
                        std::array<BYTE,256> actual{};return GetKeyboardState(actual.data())&&actual==expected;
                    }
                    bool restore() {
                        std::array<BYTE,256> actual{};const bool exact=ready&&SetKeyboardState(keys.data())&&
                            GetKeyboardState(actual.data())&&actual==keys;
                        SetFocus(focus&&IsWindow(focus)?focus:nullptr);error=savedError;
                        restored=exact&&GetFocus()==(focus&&IsWindow(focus)?focus:nullptr)&&error==savedError;return restored;
                    }
                    ~ThreadRestore(){if(!restored)restore();}
                } thread(lastError_);
                struct FileFact {
                    NativeFileIdentity identity{};DWORD attributes=0;ULONGLONG size=0,created=0,modified=0;std::string bytes;
                    bool operator==(const FileFact&) const=default;
                };
                using FileFacts=std::map<std::wstring,FileFact>;
                const auto files=[&](FileFacts& facts) {
                    facts.clear();size_t bytes=0;
                    try {
                        for(const auto& entry:std::filesystem::recursive_directory_iterator(fixture)) {
                            if(GetTickCount64()>=deadline||facts.size()>=4096)return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                            WIN32_FILE_ATTRIBUTE_DATA data{};
                            if(!GetFileAttributesExW(entry.path().c_str(),GetFileExInfoStandard,&data))return HRESULT_FROM_WIN32(GetLastError());
                            if(data.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)return E_UNEXPECTED;
                            FILE_ID_INFO id{};auto read=nativeFileIdentity(entry.path(),id);if(FAILED(read))return read;
                            FileFact fact;fact.identity.first=id.VolumeSerialNumber;
                            std::copy(std::begin(id.FileId.Identifier),std::end(id.FileId.Identifier),fact.identity.second.begin());
                            fact.attributes=data.dwFileAttributes;fact.size=(ULONGLONG(data.nFileSizeHigh)<<32)|data.nFileSizeLow;
                            fact.created=(ULONGLONG(data.ftCreationTime.dwHighDateTime)<<32)|data.ftCreationTime.dwLowDateTime;
                            fact.modified=(ULONGLONG(data.ftLastWriteTime.dwHighDateTime)<<32)|data.ftLastWriteTime.dwLowDateTime;
                            if(!(fact.attributes&FILE_ATTRIBUTE_DIRECTORY)) {
                                if(fact.size>1024*1024-bytes)return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
                                std::ifstream stream(entry.path(),std::ios::binary);if(!stream)return E_FAIL;
                                fact.bytes.assign(std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>());
                                if(stream.bad()||fact.bytes.size()!=fact.size)return E_FAIL;bytes+=fact.bytes.size();
                            }
                            if(!facts.emplace(entry.path().lexically_relative(fixture).wstring(),std::move(fact)).second)return E_UNEXPECTED;
                        }
                        return S_OK;
                    }catch(...){return E_FAIL;}
                };
                FileFacts originalFiles;const auto filesRead=files(originalFiles);
                Pidl location(ILCloneFull(currentPidl_.get()));const auto historyIndex=historyIndex_;const auto generation=namespaceGeneration_;
                std::vector<Pidl> history;bool historyReady=location!=nullptr;
                for(const auto& item:history_){history.emplace_back(ILCloneFull(item.get()));historyReady=historyReady&&history.back()!=nullptr;}
                const auto focusedFile=[&](int& index,NativeFileIdentity& identity) {
                    auto read=nativeFolder->GetFocusedItem(&index);ComPtr<IShellItem> item;
                    if(SUCCEEDED(read))read=index>=0?nativeFolder->GetItem(index,IID_PPV_ARGS(&item)):E_UNEXPECTED;
                    PWSTR path=nullptr;if(SUCCEEDED(read))read=item->GetDisplayName(SIGDN_FILESYSPATH,&path);
                    FILE_ID_INFO id{};if(SUCCEEDED(read))read=path?nativeFileIdentity(path,id):E_UNEXPECTED;CoTaskMemFree(path);
                    if(SUCCEEDED(read)){identity.first=id.VolumeSerialNumber;
                        std::copy(std::begin(id.FileId.Identifier),std::end(id.FileId.Identifier),identity.second.begin());}
                    return read;
                };
                int originalFocused=-1;NativeFileIdentity originalFocusedId{};const auto focusedRead=focusedFile(originalFocused,originalFocusedId);
                const auto identityDiagnostic=[](const NativeFileIdentity& id) {
                    std::wstring text=std::to_wstring(id.first)+L":";
                    for(const auto byte:id.second)text+=std::to_wstring(byte)+L",";return text;
                };
                const auto preserved=[&](std::wstring& diagnostic) {
                    const bool owner=nativeView.Get()==view_.Get()&&nativeFolder.Get()==folderView_.Get()&&generation==namespaceGeneration_;
                    const bool current=currentPidl_&&location&&ILIsEqual(currentPidl_.get(),location.get());
                    bool sameHistory=historyIndex_==historyIndex&&history_.size()==history.size();
                    for(size_t index=0;index<std::min(history_.size(),history.size());++index)
                        sameHistory=sameHistory&&history_[index]&&ILIsEqual(history_[index].get(),history[index].get());
                    ComPtr<IShellItemArray> actual;std::set<NativeFileIdentity> identities;int count=-1,focused=-1;NativeFileIdentity focusedId{};
                    const auto countRead=nativeFolder->ItemCount(SVGIO_SELECTION,&count);
                    const auto selectionRead=nativeFolder->GetSelection(FALSE,&actual);
                    const auto identitiesRead=actual?nativeArrayIdentities(actual.Get(),identities):E_POINTER;
                    const auto itemRead=focusedFile(focused,focusedId);
                    const bool exact=owner&&current&&sameHistory&&SUCCEEDED(countRead)&&count==3&&SUCCEEDED(selectionRead)&&
                        SUCCEEDED(identitiesRead)&&identities==seedIds&&SUCCEEDED(itemRead)&&focused==originalFocused&&focusedId==originalFocusedId;
                    diagnostic=L"preserved="+std::to_wstring(exact)+L"; owner/current/history="+std::to_wstring(owner)+L"/"+
                        std::to_wstring(current)+L"/"+std::to_wstring(sameHistory)+L"; generation original/actual="+
                        std::to_wstring(generation)+L"/"+std::to_wstring(namespaceGeneration_)+L"; selection count="+
                        hresultMessage(countRead)+L"/"+std::to_wstring(count)+L"; selection array/IDs="+hresultMessage(selectionRead)+L"/"+
                        hresultMessage(identitiesRead)+L"/"+std::to_wstring(identities==seedIds)+L"; focused read/index original/actual="+
                        hresultMessage(itemRead)+L"/"+std::to_wstring(originalFocused)+L"/"+std::to_wstring(focused)+L"; focused FileID original/actual="+
                        identityDiagnostic(originalFocusedId)+L"/"+identityDiagnostic(focusedId)+
                        L"; complete owned file corpus verified once after both controls; actual selected FileIDs=";
                    for(const auto& id:identities)diagnostic+=L"["+identityDiagnostic(id)+L"]";
                    return exact;
                };
                const auto focusWindowDiagnostic=[](HWND target) {
                    wchar_t type[128]{};if(target)GetClassNameW(target,type,128);
                    return std::to_wstring(reinterpret_cast<UINT_PTR>(target))+L"/"+type;
                };
                const auto providerFocus=[&](LONG& role,std::wstring& diagnostic) {
                    const auto focusedWindow=GetFocus();VARIANT focused{};
                    VARIANT self{};self.vt=VT_I4;self.lVal=CHILDID_SELF;VARIANT rootRole{},rootState{};
                    const auto rootRoleRead=content.object?content.object->get_accRole(self,&rootRole):E_POINTER;
                    const auto rootStateRead=content.object?content.object->get_accState(self,&rootState):E_POINTER;
                    const auto focusRead=content.object?content.object->get_accFocus(&focused):E_POINTER;auto read=focusRead;
                    ComPtr<IAccessible> object=content.object;
                    VARIANT child{};child.vt=VT_I4;child.lVal=CHILDID_SELF;
                    HRESULT objectRead=E_PENDING;
                    if(SUCCEEDED(read)&&focused.vt==VT_DISPATCH&&focused.pdispVal)read=objectRead=focused.pdispVal->QueryInterface(IID_PPV_ARGS(&object));
                    else if(SUCCEEDED(read)&&focused.vt==VT_I4)child.lVal=focused.lVal;
                    else read=E_UNEXPECTED;
                    if(SUCCEEDED(read)&&!object)read=E_NOINTERFACE;
                    VARIANT actualRole{},state{};HRESULT roleRead=E_PENDING,stateRead=E_PENDING,ownerRead=E_PENDING;
                    HWND owner=nullptr;
                    if(SUCCEEDED(read)) {
                        roleRead=object->get_accRole(child,&actualRole);stateRead=object->get_accState(child,&state);
                        ownerRead=WindowFromAccessibleObject(object.Get(),&owner);
                    }
                    const bool valid=content.object&&focusedWindow==content.window&&SUCCEEDED(read)&&
                        SUCCEEDED(roleRead)&&SUCCEEDED(stateRead)&&SUCCEEDED(ownerRead)&&actualRole.vt==VT_I4&&state.vt==VT_I4&&
                        (state.lVal&STATE_SYSTEM_FOCUSED)&&owner==content.window;
                    role=actualRole.vt==VT_I4?actualRole.lVal:0;
                    // Callers independently verify the actual host region at
                    // their assertion boundary. Avoid a second full native
                    // hierarchy scan solely to repeat it in diagnostic text.
                    diagnostic=L"provider valid="+std::to_wstring(valid)+L"; native/root/GetFocus/owner="+
                        focusWindowDiagnostic(contentView)+L"/"+focusWindowDiagnostic(content.window)+L"/"+focusWindowDiagnostic(focusedWindow)+L"/"+
                        focusWindowDiagnostic(owner)+L"; root role HRESULT/VT/value="+hresultMessage(rootRoleRead)+L"/"+
                        std::to_wstring(rootRole.vt)+L"/"+std::to_wstring(rootRole.vt==VT_I4?rootRole.lVal:0)+L"; root state HRESULT/VT/value="+
                        hresultMessage(rootStateRead)+L"/"+std::to_wstring(rootState.vt)+L"/"+
                        std::to_wstring(rootState.vt==VT_I4?rootState.lVal:0)+L"; accFocus HRESULT/VT/childID="+hresultMessage(focusRead)+L"/"+
                        std::to_wstring(focused.vt)+L"/"+std::to_wstring(focused.vt==VT_I4?focused.lVal:0)+L"; QI="+hresultMessage(objectRead)+
                        L"; role HRESULT/VT/value="+hresultMessage(roleRead)+L"/"+std::to_wstring(actualRole.vt)+L"/"+std::to_wstring(role)+
                        L"; state HRESULT/VT/value="+hresultMessage(stateRead)+L"/"+std::to_wstring(state.vt)+L"/"+
                        std::to_wstring(state.vt==VT_I4?state.lVal:0)+L"; owner HRESULT="+hresultMessage(ownerRead);
                    VariantClear(&focused);VariantClear(&actualRole);VariantClear(&state);VariantClear(&rootRole);VariantClear(&rootState);return valid;
                };
                const bool focusFixtureReady=seedIds.size()==3&&presentation.ready&&content.object&&thread.ready&&historyReady&&
                    SUCCEEDED(filesRead)&&SUCCEEDED(focusedRead)&&originalFocused==3;
                bool reverseTransfer=focusFixtureReady;unsigned attempts=0,readbacks=0;std::wstring detail;
                const auto focusStage=[&](std::wstring_view stage) {
                    detail+=L"stage="+std::wstring(stage)+L"; elapsed_ms="+
                        std::to_wstring(GetTickCount64()-focusStarted)+L"; ";
                };
                focusStage(L"initial-native-and-source-proof");
                for(const unsigned mask:{0u,2u}) {
                    std::wstring iterationState;
                    const bool iterationPreserved=focusFixtureReady&&preserved(iterationState);
                    const bool iterationKeyboard=thread.exact();
                    const bool iterationFence=currentPidl_&&location&&ILIsEqual(currentPidl_.get(),location.get())&&
                        nativeView.Get()==view_.Get()&&nativeFolder.Get()==folderView_.Get()&&
                        generation==namespaceGeneration_&&!closing_;
                    if(!iterationPreserved||!iterationKeyboard||!iterationFence||GetTickCount64()>=deadline||!thread.apply(mask)) {
                        reverseTransfer=false;
                        detail+=L"focus control refused before modifier setup; state/keyboard/fence="+
                            std::to_wstring(iterationPreserved)+L"/"+std::to_wstring(iterationKeyboard)+L"/"+
                            std::to_wstring(iterationFence)+L"/["+iterationState+L"]; ";break;
                    }
                    ++readbacks;LONG headerRole=0,reverseRole=0;
                    std::wstring reverseHeaderDiagnostic,reverseDiagnostic,reverseState;
                    // Exercise the actual product route from a verified Header.
                    // Bare composite-root focus observations are retained in
                    // earlier artifacts, not asserted as a file-row contract.
                    const auto reverseActivated=GetTickCount64()<deadline?
                        nativeView->UIActivate(SVUIA_ACTIVATE_FOCUS):E_UNEXPECTED;
                    detail+=L"modifiers="+std::to_wstring(mask)+L"; production setup UIActivate="+
                        hresultMessage(reverseActivated)+L"/["+thread.readback()+L"]; ";
                    const auto reverseStart=currentFocusRegion();
                    const auto reseeded=reverseActivated==S_OK?(reverseStart==FocusRegion::FolderView?execute(FocusNext):
                        (reverseStart==FocusRegion::Sorting?S_OK:E_FAIL)):E_UNEXPECTED;
                    const auto reseededKeyboard=thread.readback();
                    const bool reverseHeaderProvider=providerFocus(headerRole,reverseHeaderDiagnostic);
                    const bool reverseHeader=reseeded==S_OK&&currentFocusRegion()==FocusRegion::Sorting&&reverseHeaderProvider&&
                        (headerRole==ROLE_SYSTEM_SPLITBUTTON||headerRole==ROLE_SYSTEM_COLUMNHEADER);
                    detail+=L"production Header seed="+hresultMessage(reseeded)+L"/["+reverseHeaderDiagnostic+L"]/["+reseededKeyboard+L"]; ";
                    const auto reversed=reverseHeader?execute(FocusPrevious):E_UNEXPECTED;
                    const bool reversedKeyboardExact=thread.exact();
                    const auto reversedKeyboard=thread.readback();
                    const bool reverseProvider=providerFocus(reverseRole,reverseDiagnostic);const bool reversePreserved=preserved(reverseState);
                    const bool reverse=reversed==S_OK&&currentFocusRegion()==FocusRegion::FolderView&&reverseProvider&&
                        (reverseRole==ROLE_SYSTEM_LISTITEM||reverseRole==ROLE_SYSTEM_LIST)&&reversePreserved&&reversedKeyboardExact;
                    reverseTransfer=reverseTransfer&&reverseHeader&&reverse;++attempts;
                    detail+=L"production reverse="+hresultMessage(reversed)+L"/accepted="+std::to_wstring(reverse)+L"/["+
                        reverseDiagnostic+L"]/["+reverseState+L"]/["+reversedKeyboard+L"]; ";
                    const bool reverseFence=currentPidl_&&location&&ILIsEqual(currentPidl_.get(),location.get())&&
                        nativeView.Get()==view_.Get()&&nativeFolder.Get()==folderView_.Get()&&generation==namespaceGeneration_&&!closing_;
                    if(!reversePreserved||!reversedKeyboardExact||!reverseFence) {
                        reverseTransfer=false;
                        detail+=L"focus control stopped after production state/keyboard/fence preservation failure; ";break;
                    }
                    focusStage(mask==0?L"neutral-production-reverse":L"shift-production-reverse");
                }
                focusStage(L"before-final-complete-source-proof");
                FileFacts finalFiles;const auto finalFilesRead=files(finalFiles);
                focusStage(L"after-final-complete-source-proof");
                const bool corpusPreserved=SUCCEEDED(finalFilesRead)&&finalFiles==originalFiles;
                detail+=L"final complete file corpus read/original count/actual count/exact="+hresultMessage(finalFilesRead)+L"/"+
                    std::to_wstring(originalFiles.size())+L"/"+std::to_wstring(finalFiles.size())+L"/"+std::to_wstring(corpusPreserved)+L"; ";
                check("native_f6_reverse_from_actual_header_reaches_content_and_preserves_full_owned_state",reverseTransfer&&attempts==2&&corpusPreserved,detail);
                const bool restored=thread.restore();
                check("native_content_focus_fixture_restores_exact_thread_keyboard_focus_error_and_budget",readbacks==2&&
                    corpusPreserved&&GetTickCount64()<deadline&&restored);
            }
            const HWND ownedFocusBefore = GetFocus();
            std::set<NativeFileIdentity> expectedComplement;
            std::set_difference(allSelectionIds.begin(), allSelectionIds.end(), seedIds.begin(), seedIds.end(),
                std::inserter(expectedComplement, expectedComplement.end()));
            std::wstring selectedItemDiagnostic;
            for (const int start : {-1, 0, 1, 500, 501, 1000, 1001, 1002}) {
                int nativeIndex = -2;
                const auto selectedRead = folderView_->GetSelectedItem(start, &nativeIndex);
                selectedItemDiagnostic += L"; GetSelectedItem(" + std::to_wstring(start) + L")=" +
                    hresultMessage(selectedRead) + L"/" + std::to_wstring(nativeIndex);
            }
            const auto mixedInvert = nativeSelectionOperation(Invert, 999);
            selected.Reset();
            auto complementResult = selection(selected);
            if (SUCCEEDED(complementResult)) complementResult = nativeArrayIdentities(selected.Get(), complementIds);
            DWORD flagsAfterInvert = 0;
            const auto focusAfterResult = folderView_->GetFocusedItem(&focusedAfter);
            const auto flagsAfterResult = folderView_->GetCurrentFolderFlags(&flagsAfterInvert);
            check("mixed_selection_exact_native_complement", SUCCEEDED(identityResult) && allSelectionIds.size() == 1002 &&
                SUCCEEDED(beforeSeedNone.operation) && beforeSeedNone.countReady && seedCountReady &&
                SUCCEEDED(seedResult) && seedIds.size() == 3 && mixedInvert.stateReady &&
                SUCCEEDED(mixedInvert.operation) && mixedInvert.countReady &&
                SUCCEEDED(complementResult) && complementIds == expectedComplement && complementIds.size() == 999,
                selectionReadbackDetail(mixedInvert, 999) + L"; seedCount=" + std::to_wstring(seedCount.count) +
                L"; seedCountStatus=" + hresultMessage(seedCount.countStatus) + L"; seeded=" + std::to_wstring(seedIds.size()) +
                L"; actual exact complement=" + std::to_wstring(complementIds.size()) + selectedItemDiagnostic);
            check("mixed_selection_preserves_native_focus_and_flags", SUCCEEDED(flagResult) &&
                (selectionTestFlags & FWF_CHECKSELECT) && SUCCEEDED(focusResult) && focusedBefore == 3 &&
                SUCCEEDED(focusAfterResult) && focusedAfter == focusedBefore && GetFocus() == ownedFocusBefore &&
                SUCCEEDED(flagsAfterResult) && flagsAfterInvert == selectionTestFlags,
                L"focused item before/after=" + std::to_wstring(focusedBefore) + L"/" + std::to_wstring(focusedAfter) +
                L"; flags before/after=" + std::to_wstring(selectionTestFlags) + L"/" + std::to_wstring(flagsAfterInvert));
            const auto restoreSeed = nativeSelectionOperation(Invert, 3);
            selected.Reset();
            auto restoreSeedRead = selection(selected);
            if (SUCCEEDED(restoreSeedRead)) restoreSeedRead = nativeArrayIdentities(selected.Get(), restoredIds);
            check("mixed_selection_double_inverse_restores_exact_files", restoreSeed.stateReady &&
                SUCCEEDED(restoreSeed.operation) && restoreSeed.countReady &&
                SUCCEEDED(restoreSeedRead) && restoredIds == seedIds && restoredIds.size() == 3,
                selectionReadbackDetail(restoreSeed, 3) + L"; restored native identities=" + std::to_wstring(restoredIds.size()));
            const auto exactAll = nativeSelectionOperation(SelectAll, 1002);
            selected.Reset(); restoredIds.clear();
            auto exactAllRead = selection(selected);
            if (SUCCEEDED(exactAllRead)) exactAllRead = nativeArrayIdentities(selected.Get(), restoredIds);
            check("select_all_exact_native_files", exactAll.stateReady && SUCCEEDED(exactAll.operation) &&
                exactAll.countReady && SUCCEEDED(exactAllRead) && restoredIds == allSelectionIds && restoredIds.size() == 1002,
                selectionReadbackDetail(exactAll, 1002));
            const auto exactNone = nativeSelectionOperation(SelectNone, 0);
            check("select_none_after_mixed_native_files", exactNone.stateReady && SUCCEEDED(exactNone.operation) &&
                exactNone.countReady, selectionReadbackDetail(exactNone, 0));
            folderView_->SetCurrentFolderFlags(FWF_CHECKSELECT, selectionOriginalFlags & FWF_CHECKSELECT);
            {
                // A separate owned App proves genuine native media readiness
                // and publication cancellation without changing this corpus.
                const auto mediaRoot=fixture.parent_path()/(fixture.filename().wstring()+L"-Kind");
                const auto leaveRoot=fixture.parent_path()/(fixture.filename().wstring()+L"-Kind-empty");
                struct MediaFiles {
                    std::filesystem::path root,destination;
                    bool rootCreated=false,destinationCreated=false;
                    ~MediaFiles(){std::error_code ignored;if(rootCreated)std::filesystem::remove_all(root,ignored);
                        if(destinationCreated)std::filesystem::remove_all(destination,ignored);}
                }mediaFiles{mediaRoot,leaveRoot};
                mediaFiles.rootCreated=std::filesystem::create_directory(mediaRoot);
                if(!mediaFiles.rootCreated)throw std::runtime_error("Owned Kind source directory already exists");
                mediaFiles.destinationCreated=std::filesystem::create_directory(leaveRoot);
                if(!mediaFiles.destinationCreated)throw std::runtime_error("Owned Kind destination directory already exists");
                const auto textPath=mediaRoot/L"Final unrelated.txt";
                const std::string musicBytes="owned fast Kind fixture; never decoded or played";
                const std::string textBytes="owned final nonmedia counterexample";
                std::vector<std::filesystem::path> mediaPaths;
                std::set<NativeFileIdentity> musicIds,allIds;
                std::vector<FILE_ID_INFO> sourceIds;
                std::vector<std::filesystem::file_time_type> sourceTimes;
                bool sourcesReady=true;
                for(unsigned index=0;index<257;++index) {
                    const auto path=mediaRoot/(L"Owned music "+std::to_wstring(index)+L".mp3");
                    std::ofstream output(path,std::ios::binary);output<<musicBytes;output.close();
                    FILE_ID_INFO id{};const auto read=nativeFileIdentity(path,id);std::array<BYTE,16> bytes{};
                    std::copy(std::begin(id.FileId.Identifier),std::end(id.FileId.Identifier),bytes.begin());
                    sourcesReady=sourcesReady&&output.good()&&read==S_OK;
                    musicIds.insert({id.VolumeSerialNumber,bytes});mediaPaths.push_back(path);sourceIds.push_back(id);
                    sourceTimes.push_back(std::filesystem::last_write_time(path));
                }
                {std::ofstream output(textPath,std::ios::binary);output<<textBytes;sourcesReady=sourcesReady&&output.good();}
                FILE_ID_INFO textIdentity{};sourcesReady=sourcesReady&&nativeFileIdentity(textPath,textIdentity)==S_OK;
                std::array<BYTE,16> textId{};std::copy(std::begin(textIdentity.FileId.Identifier),std::end(textIdentity.FileId.Identifier),textId.begin());
                allIds=musicIds;allIds.insert({textIdentity.VolumeSerialNumber,textId});
                mediaPaths.push_back(textPath);sourceIds.push_back(textIdentity);sourceTimes.push_back(std::filesystem::last_write_time(textPath));
                const auto primaryView=view_;const auto primaryFolder=folderView_;
                int primarySelectedCount=-1;ComPtr<IShellItemArray> primarySelection;
                const auto primaryCountRead=primaryFolder->ItemCount(SVGIO_SELECTION,&primarySelectedCount);
                const auto primarySelectionRead=primaryFolder->GetSelection(FALSE,&primarySelection);
                std::set<NativeFileIdentity> primarySelectedIds;
                const auto primaryIdentityRead=primarySelection?nativeArrayIdentities(primarySelection.Get(),primarySelectedIds):
                    primaryCountRead==S_OK&&primarySelectedCount==0?S_OK:E_UNEXPECTED;
                const auto primaryNavigation=navigationCount_;const auto primaryHistory=history_.size();const auto primaryHistoryIndex=historyIndex_;
                Pidl primaryLocation(currentPidl_?ILCloneFull(currentPidl_.get()):nullptr);
                const auto releaseMedia=[](ExplorerApp* app){
                    if(app->window_&&IsWindow(app->window_))SendMessageW(app->window_,WM_CLOSE,0,0);
                    if(FAILED(app->shutdownStatus_)) {
                        std::fprintf(stderr,"headless Kind owned child did not drain native workers HRESULT=0x%08lX\n",
                            static_cast<unsigned long>(app->shutdownStatus_));std::fflush(stderr);
                        if(!TerminateProcess(GetCurrentProcess(),9))std::_Exit(9);std::_Exit(9);
                    }
                    app->Release();
                };
                std::unique_ptr<ExplorerApp,decltype(releaseMedia)> child(new ExplorerApp(instance_,true,requestedRibbonLayout_),releaseMedia);
                const auto created=sourcesReady&&musicIds.size()==257&&allIds.size()==258?child->create(mediaRoot.wstring()):E_UNEXPECTED;
                const bool childReady=created==S_OK&&pumpUntil([&]{int count=-1;return !child->navigating_&&child->folderView_&&
                    child->folderView_->ItemCount(SVGIO_ALLVIEW,&count)==S_OK&&count==258;},5000);
                const auto nativeView=child->view_;const auto nativeFolder=child->folderView_;
                Pidl nativeLocation(child->currentPidl_?ILCloneFull(child->currentPidl_.get()):nullptr);
                const auto nativeKindNavigation=child->navigationCount_;const auto historySize=child->history_.size();const auto historyIndex=child->historyIndex_;
                auto seed=childReady?S_OK:E_UNEXPECTED;
                // Build the selected native child list from the exact owned
                // media identities, leaving the final unrelated file out.
                std::vector<Pidl> musicPidls;std::vector<PCUITEMID_CHILD> musicChildren;
                for(const auto& path:mediaPaths) {
                    if(path==textPath)break;
                    ComPtr<IShellItem> music;PIDLIST_ABSOLUTE raw=nullptr;
                    auto read=SHCreateItemFromParsingName(path.c_str(),nullptr,IID_PPV_ARGS(&music));
                    if(read==S_OK)read=SHGetIDListFromObject(music.Get(),&raw);
                    Pidl pidl(raw);if(read!=S_OK||!pidl){seed=read==S_OK?E_UNEXPECTED:read;break;}
                    musicChildren.push_back(ILFindLastID(pidl.get()));musicPidls.push_back(std::move(pidl));
                }
                // Clear once before applying SELECT to the full native array;
                // DESELECTOTHERS describes a single selected item and must not
                // collapse this 257-item fixture as each item is selected.
                if(seed==S_OK&&musicChildren.size()==257)seed=nativeView?nativeView->SelectItem(nullptr,SVSI_DESELECTOTHERS):E_NOINTERFACE;
                if(seed==S_OK&&musicChildren.size()==257)seed=nativeFolder->SelectAndPositionItems(static_cast<UINT>(musicChildren.size()),
                    musicChildren.data(),nullptr,SVSI_SELECT|SVSI_NOTAKEFOCUS);
                const auto selectedIds=[&](const std::set<NativeFileIdentity>& expected) {
                    ComPtr<IShellItemArray> selected;std::set<NativeFileIdentity> actual;DWORD count=0;
                    return child->folderView_&&child->folderView_->GetSelection(FALSE,&selected)==S_OK&&selected&&selected->GetCount(&count)==S_OK&&
                        count==expected.size()&&nativeArrayIdentities(selected.Get(),actual)==S_OK&&actual==expected;
                };
                // Pause only this owned polling timer, never the provider or
                // COM pump, so the real ready task can be inspected directly.
                const auto realKindReady=[&](DWORD count) {
                    if(child->selectionStateDirty_||child->namespaceDirty_)child->updateCommands();
                    child->startPendingSelectionKinds();KillTimer(child->window_,4);
                    return !child->navigating_&&!child->selectionStateDirty_&&!child->namespaceDirty_&&
                        child->selectionKindsRequest_.countKnown&&child->selectionKindsRequest_.count==count&&
                        child->selectionKindsRequest_.pending&&child->selectionKindsRequest_.task&&child->selectionKindsRequest_.task->completed();
                };
                const bool musicReady=seed==S_OK&&pumpUntil([&]{return realKindReady(257);},5000)&&selectedIds(musicIds);
                const bool nativeMenuUnfinished=child->selectionStateBatch_&&!child->selectionStateBatch_->completed();
                const bool actualCommandResultsUnpublished=child->commandStatesPending();
                const bool commandResultsUnpublished=musicReady&&actualCommandResultsUnpublished;
                if(musicReady)child->pollSelectionKinds();
                const auto contextMatches=[&](RibbonContext context,bool expected) {
                    child->ribbon_.flush();UINT native=0,actual=UI_CONTEXTAVAILABILITY_NOTAVAILABLE;
                    return child->ribbon_.contextAvailable(context,native,actual)==S_OK&&native&&
                        (expected?actual!=UI_CONTEXTAVAILABILITY_NOTAVAILABLE:actual==UI_CONTEXTAVAILABILITY_NOTAVAILABLE);
                };
                const bool musicPublished=musicReady&&!child->selectionKindsRequest_.pending&&child->selectionKindsRequest_.status==S_OK&&
                    child->selectionKinds_.count==257&&child->selectionKinds_.music&&!child->selectionKinds_.video&&
                    contextMatches(RibbonContext::Music,true)&&contextMatches(RibbonContext::Video,false)&&selectedIds(musicIds);
                std::wstring kindFailureDiagnostic;
                if(!musicPublished||!commandResultsUnpublished) {
                    // Snapshot the request before any native readback can pump
                    // its timer or replace the source. These facts diagnose
                    // the original failure; they do not retry publication.
                    struct KindFailureSnapshot {
                        UINT64 generation=0,revision=0,requestGeneration=0,requestRevision=0;
                        unsigned navigation=0,requestNavigation=0;
                        DWORD count=0,publishedCount=0;
                        HRESULT status=E_PENDING;
                        bool known=false,pending=false,completed=false,music=false,video=false;
                        bool selectionDirty=false,namespaceDirty=false,navigating=false,closing=false,cancelPending=false;
                        IShellView* view=nullptr;IFolderView2* folder=nullptr;IShellItemArray* selection=nullptr;
                        NamespaceCommandStateTask* task=nullptr;
                    };
                    const auto kindFailureSnapshot=[&] {
                        const auto& request=child->selectionKindsRequest_;
                        return KindFailureSnapshot{child->namespaceGeneration_,child->commandSourceRevision_,request.generation,
                            request.sourceRevision,child->navigationCount_,request.navigation,request.count,child->selectionKinds_.count,
                            request.status,request.countKnown,request.pending,request.task&&request.task->completed(),
                            child->selectionKinds_.music,child->selectionKinds_.video,child->selectionStateDirty_,child->namespaceDirty_,
                            child->navigating_,child->closing_,child->commandStatesCancelPending_,child->view_.Get(),child->folderView_.Get(),
                            request.selection.Get(),request.task.get()};
                    };
                    const auto kindBefore=kindFailureSnapshot();
                    const bool kindSourceCurrentBefore=child->selectionKindsSourceCurrent();
                    const bool kindResultsPendingBefore=child->commandStatesPending();
                    ComPtr<IFolderView2> kindDiagnosticFolder=child->folderView_;
                    int kindNativeCount=-1;DWORD kindArrayCount=0;
                    const auto kindCountRead=kindDiagnosticFolder?kindDiagnosticFolder->ItemCount(SVGIO_SELECTION,&kindNativeCount):E_UNEXPECTED;
                    ComPtr<IShellItemArray> kindDiagnosticSelection;std::set<NativeFileIdentity> kindDiagnosticIds;
                    const auto kindSelectionRead=kindDiagnosticFolder?kindDiagnosticFolder->GetSelection(FALSE,&kindDiagnosticSelection):E_UNEXPECTED;
                    const auto kindArrayCountRead=kindSelectionRead==S_OK&&kindDiagnosticSelection?
                        kindDiagnosticSelection->GetCount(&kindArrayCount):E_UNEXPECTED;
                    const auto kindIdentityRead=kindArrayCountRead==S_OK?
                        nativeArrayIdentities(kindDiagnosticSelection.Get(),kindDiagnosticIds):E_UNEXPECTED;
                    UINT kindMusicIdentifier=0,kindMusicAvailability=0,kindVideoIdentifier=0,kindVideoAvailability=0;
                    const auto kindMusicRead=child->ribbon_.contextAvailable(RibbonContext::Music,kindMusicIdentifier,kindMusicAvailability);
                    const auto kindVideoRead=child->ribbon_.contextAvailable(RibbonContext::Video,kindVideoIdentifier,kindVideoAvailability);
                    const auto kindAfter=kindFailureSnapshot();
                    const bool kindSourceCurrentAfter=child->selectionKindsSourceCurrent();
                    const bool kindReadCoherent=kindBefore.generation==kindAfter.generation&&kindBefore.revision==kindAfter.revision&&
                        kindBefore.requestGeneration==kindAfter.requestGeneration&&kindBefore.requestRevision==kindAfter.requestRevision&&
                        kindBefore.navigation==kindAfter.navigation&&kindBefore.requestNavigation==kindAfter.requestNavigation&&
                        kindBefore.view==kindAfter.view&&kindBefore.folder==kindAfter.folder&&kindBefore.folder==kindDiagnosticFolder.Get()&&
                        kindBefore.selection==kindAfter.selection&&kindBefore.task==kindAfter.task&&kindBefore.count==kindAfter.count&&
                        kindBefore.known==kindAfter.known&&kindBefore.pending==kindAfter.pending&&kindBefore.status==kindAfter.status&&
                        kindBefore.selectionDirty==kindAfter.selectionDirty&&kindBefore.namespaceDirty==kindAfter.namespaceDirty&&
                        kindBefore.navigating==kindAfter.navigating&&kindBefore.closing==kindAfter.closing&&kindBefore.cancelPending==kindAfter.cancelPending;
                    kindFailureDiagnostic=L"; seed/childReady="+hresultMessage(seed)+L"/"+std::to_wstring(childReady)+
                        L"; pre-native request known/count/status/pending/task/completed="+std::to_wstring(kindBefore.known)+L"/"+
                        std::to_wstring(kindBefore.count)+L"/"+hresultMessage(kindBefore.status)+L"/"+std::to_wstring(kindBefore.pending)+L"/"+
                        std::to_wstring(kindBefore.task!=nullptr)+L"/"+std::to_wstring(kindBefore.completed)+
                        L"; pre-native published count/music/video="+std::to_wstring(kindBefore.publishedCount)+L"/"+
                        std::to_wstring(kindBefore.music)+L"/"+std::to_wstring(kindBefore.video)+
                        L"; pre-native generation/revision/request generation/revision/navigation/request navigation="+
                        std::to_wstring(kindBefore.generation)+L"/"+std::to_wstring(kindBefore.revision)+L"/"+
                        std::to_wstring(kindBefore.requestGeneration)+L"/"+std::to_wstring(kindBefore.requestRevision)+L"/"+
                        std::to_wstring(kindBefore.navigation)+L"/"+std::to_wstring(kindBefore.requestNavigation)+
                        L"; pre-native dirty selection/namespace/navigating/closing/cancel="+std::to_wstring(kindBefore.selectionDirty)+L"/"+
                        std::to_wstring(kindBefore.namespaceDirty)+L"/"+std::to_wstring(kindBefore.navigating)+L"/"+
                        std::to_wstring(kindBefore.closing)+L"/"+std::to_wstring(kindBefore.cancelPending)+
                        L"; actual command results pending="+std::to_wstring(kindResultsPendingBefore)+
                        L"; native SVGIO_SELECTION HRESULT/count="+hresultMessage(kindCountRead)+L"/"+std::to_wstring(kindNativeCount)+
                        L"; GetSelection/array/count="+hresultMessage(kindSelectionRead)+L"/"+hresultMessage(kindArrayCountRead)+L"/"+
                        std::to_wstring(kindArrayCount)+L"; full FileIDs HRESULT/count/exact="+hresultMessage(kindIdentityRead)+L"/"+
                        std::to_wstring(kindDiagnosticIds.size())+L"/"+std::to_wstring(kindIdentityRead==S_OK&&kindDiagnosticIds==musicIds)+
                        L"; native Music HRESULT/id/availability="+hresultMessage(kindMusicRead)+L"/"+std::to_wstring(kindMusicIdentifier)+L"/"+
                        std::to_wstring(kindMusicAvailability)+L"; native Video HRESULT/id/availability="+hresultMessage(kindVideoRead)+L"/"+
                        std::to_wstring(kindVideoIdentifier)+L"/"+std::to_wstring(kindVideoAvailability)+
                        L"; after-native request known/count/status/pending/task/completed="+std::to_wstring(kindAfter.known)+L"/"+
                        std::to_wstring(kindAfter.count)+L"/"+hresultMessage(kindAfter.status)+L"/"+std::to_wstring(kindAfter.pending)+L"/"+
                        std::to_wstring(kindAfter.task!=nullptr)+L"/"+std::to_wstring(kindAfter.completed)+
                        L"; after-native source/request coherent="+std::to_wstring(kindReadCoherent)+
                        L"; source-current before/after="+std::to_wstring(kindSourceCurrentBefore)+L"/"+std::to_wstring(kindSourceCurrentAfter);
                }
                check("async_kind_full_native_music_context_publishes_without_command_result_poll",musicPublished&&commandResultsUnpublished,
                    L"expectedOwnedMusicFiles="+std::to_wstring(musicIds.size())+L"; Kind ready="+std::to_wstring(musicReady)+
                    L"; native menu still running="+std::to_wstring(nativeMenuUnfinished)+L"; command results unpublished="+
                    std::to_wstring(actualCommandResultsUnpublished)+kindFailureDiagnostic);
                // Commit the real empty source between identical selections.
                // Otherwise a clear/reselect in one deferred interval may
                // correctly retain the already-published Music snapshot.
                const auto observeEmptyKind=[&](HRESULT& nativeSelectionRead) {
                    child->updateCommands();
                    int nativeCount=-1;DWORD arrayCount=MAXDWORD;
                    ComPtr<IShellItemArray> emptySelection;
                    const auto countRead=nativeFolder?nativeFolder->ItemCount(SVGIO_SELECTION,&nativeCount):E_UNEXPECTED;
                    const auto selectionRead=nativeFolder?nativeFolder->GetSelection(FALSE,&emptySelection):E_UNEXPECTED;
                    nativeSelectionRead=selectionRead;
                    const auto arrayRead=emptySelection?emptySelection->GetCount(&arrayCount):S_FALSE;
                    const bool nativeEmpty=countRead==S_OK&&nativeCount==0&&
                        (emptySelection?(selectionRead==S_OK||selectionRead==S_FALSE)&&arrayRead==S_OK&&arrayCount==0:
                            selectionRead==HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
                    const bool contextsEmpty=contextMatches(RibbonContext::Music,false)&&contextMatches(RibbonContext::Video,false);
                    const auto& request=child->selectionKindsRequest_;
                    return nativeEmpty&&contextsEmpty&&!child->selectionStateDirty_&&!child->namespaceDirty_&&!child->navigating_&&
                        request.countKnown&&request.count==0&&request.status==S_OK&&!request.pending&&!request.task&&
                        child->selectionKinds_.count==0&&!child->selectionKinds_.music&&!child->selectionKinds_.video&&
                        child->currentPidl_&&nativeLocation&&ILIsEqual(child->currentPidl_.get(),nativeLocation.get())&&
                        !child->closing_&&child->view_.Get()==nativeView.Get()&&child->folderView_.Get()==nativeFolder.Get()&&
                        child->navigationCount_==nativeKindNavigation;
                };
                const auto reseedFailureDiagnostic=[&] {
                    const auto& request=child->selectionKindsRequest_;
                    const auto known=request.countKnown,pending=request.pending,completed=request.task&&request.task->completed();
                    const auto count=request.count;const auto status=request.status;
                    const auto generation=request.generation,revision=request.sourceRevision;
                    const auto taskPresent=request.task!=nullptr;
                    int nativeCount=-1;const auto countRead=nativeFolder?nativeFolder->ItemCount(SVGIO_SELECTION,&nativeCount):E_UNEXPECTED;
                    return L"; failed reseed request known/count/status/pending/task/completed="+std::to_wstring(known)+L"/"+
                        std::to_wstring(count)+L"/"+hresultMessage(status)+L"/"+std::to_wstring(pending)+L"/"+
                        std::to_wstring(taskPresent)+L"/"+std::to_wstring(completed)+L"; request generation/revision="+
                        std::to_wstring(generation)+L"/"+std::to_wstring(revision)+L"; native count HRESULT/count="+
                        hresultMessage(countRead)+L"/"+std::to_wstring(nativeCount);
                };
                // A fresh actual Music selection supplies the old completion.
                auto reseed=musicPublished?child->execute(SelectNone):E_UNEXPECTED;
                HRESULT oldEmptySelectionRead=E_PENDING;
                const bool oldEmpty=reseed==S_OK&&observeEmptyKind(oldEmptySelectionRead);
                if(reseed==S_OK&&!oldEmpty)reseed=E_UNEXPECTED;
                if(oldEmpty)reseed=nativeFolder->SelectAndPositionItems(static_cast<UINT>(musicChildren.size()),musicChildren.data(),nullptr,
                    SVSI_SELECT|SVSI_NOTAKEFOCUS);
                const bool oldReady=reseed==S_OK&&pumpUntil([&]{return realKindReady(257);},5000)&&selectedIds(musicIds);
                const auto oldGeneration=child->namespaceGeneration_;
                NamespaceCommandStateTask* newTask=nullptr;UINT64 newGeneration=0;bool changed=false,newPending=false;
                if(oldReady) {
                    child->headlessBeforeKindsPublication_=[&] {
                        const auto changedRead=child->execute(SelectAll);
                        changed=changedRead==S_OK&&selectedIds(allIds);
                        child->updateCommands();child->startPendingSelectionKinds();KillTimer(child->window_,4);
                        newGeneration=child->namespaceGeneration_;newTask=child->selectionKindsRequest_.task.get();
                        newPending=child->selectionKindsRequest_.pending;
                    };
                    child->pollSelectionKinds();
                }
                const bool oldDiscarded=oldReady&&changed&&newGeneration>oldGeneration&&newPending&&newTask&&
                    child->namespaceGeneration_==newGeneration&&child->selectionKindsRequest_.task.get()==newTask&&
                    child->selectionKindsRequest_.pending&&child->selectionKindsRequest_.status==E_PENDING&&
                    !child->selectionKinds_.music&&!child->selectionKinds_.video&&selectedIds(allIds);
                const bool mixedReady=oldDiscarded&&pumpUntil([&]{return realKindReady(258);},5000);
                if(mixedReady)child->pollSelectionKinds();
                const bool mixedPublished=mixedReady&&child->selectionKindsRequest_.status==S_OK&&child->selectionKinds_.count==258&&
                    !child->selectionKinds_.music&&!child->selectionKinds_.video&&contextMatches(RibbonContext::Music,false)&&
                    contextMatches(RibbonContext::Video,false)&&selectedIds(allIds);
                check("async_kind_reentrant_new_native_selection_keeps_new_task_and_final_counterexample",oldDiscarded&&mixedPublished,
                    L"old/new generation="+std::to_wstring(oldGeneration)+L"/"+std::to_wstring(newGeneration)+L"; source changed="+
                    std::to_wstring(changed)+L"; new pending/task="+std::to_wstring(newPending)+L"/"+std::to_wstring(newTask!=nullptr)+
                    L"; old empty/ready="+std::to_wstring(oldEmpty)+L"/"+std::to_wstring(oldReady)+
                    L"; old native empty selection="+hresultMessage(oldEmptySelectionRead)+
                    L"; old discarded="+std::to_wstring(oldDiscarded)+L"; native mixed result="+std::to_wstring(mixedPublished)+
                    (!oldReady?reseedFailureDiagnostic():L""));
                const auto retainedHistory=child->history_.size();const auto retainedHistoryIndex=child->historyIndex_;
                const bool nativeContextPreserved=mixedPublished&&child->view_.Get()==nativeView.Get()&&child->folderView_.Get()==nativeFolder.Get()&&
                    child->currentPidl_&&nativeLocation&&ILIsEqual(child->currentPidl_.get(),nativeLocation.get())&&
                    child->navigationCount_==nativeKindNavigation&&retainedHistory==historySize&&retainedHistoryIndex==historyIndex;
                auto beforeLeave=nativeContextPreserved?child->execute(SelectNone):E_UNEXPECTED;
                HRESULT leavingEmptySelectionRead=E_PENDING;
                const bool leavingEmpty=beforeLeave==S_OK&&observeEmptyKind(leavingEmptySelectionRead);
                if(beforeLeave==S_OK&&!leavingEmpty)beforeLeave=E_UNEXPECTED;
                if(leavingEmpty)beforeLeave=nativeFolder->SelectAndPositionItems(static_cast<UINT>(musicChildren.size()),musicChildren.data(),nullptr,
                    SVSI_SELECT|SVSI_NOTAKEFOCUS);
                const bool leavingReady=beforeLeave==S_OK&&pumpUntil([&]{return realKindReady(257);},5000);
                auto leave=leavingReady?child->navigate(leaveRoot.wstring()):E_UNEXPECTED;
                if(leavingReady)child->pollCommandStates();
                const bool left=leave==S_OK&&pumpUntil([&]{int count=-1;return !child->navigating_&&child->folderView_&&
                    child->folderView_->ItemCount(SVGIO_ALLVIEW,&count)==S_OK&&count==0;},5000);
                child->updateCommands();child->startPendingCommandStates();
                const bool noStaleContext=left&&!child->selectionKinds_.music&&!child->selectionKinds_.video&&
                    contextMatches(RibbonContext::Music,false)&&contextMatches(RibbonContext::Video,false)&&
                    child->view_.Get()!=nativeView.Get()&&child->navigationCount_>nativeKindNavigation;
                check("async_kind_real_navigation_discards_old_music_completion",nativeContextPreserved&&leavingReady&&noStaleContext,
                    L"native leave="+hresultMessage(leave)+L"; previous source/history intact="+std::to_wstring(nativeContextPreserved)+
                    L"; old empty/ready="+std::to_wstring(leavingEmpty)+L"/"+std::to_wstring(leavingReady)+
                    L"; old native empty selection="+hresultMessage(leavingEmptySelectionRead)+
                    L"; actual empty destination/no stale context="+std::to_wstring(noStaleContext)+
                    (!leavingReady?reseedFailureDiagnostic():L""));
                child->headlessBeforeKindsPublication_={};child.reset();
                bool sourcesPreserved=true;
                for(size_t index=0;index<mediaPaths.size();++index) {
                    FILE_ID_INFO after{};const auto read=nativeFileIdentity(mediaPaths[index],after);
                    std::ifstream input(mediaPaths[index],std::ios::binary);const std::string bytes{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
                    sourcesPreserved=sourcesPreserved&&read==S_OK&&sourceIds[index].VolumeSerialNumber==after.VolumeSerialNumber&&
                        std::memcmp(sourceIds[index].FileId.Identifier,after.FileId.Identifier,sizeof(after.FileId.Identifier))==0&&
                        bytes==(index==257?textBytes:musicBytes)&&std::filesystem::last_write_time(mediaPaths[index])==sourceTimes[index];
                }
                const auto desktop=PrivateDesktop::current();
                int primarySelectedAfter=-1;ComPtr<IShellItemArray> primarySelectionAfter;std::set<NativeFileIdentity> primaryIdsAfter;
                const auto primaryCountAfter=primaryFolder->ItemCount(SVGIO_SELECTION,&primarySelectedAfter);
                const auto primarySelectionAfterRead=primaryFolder->GetSelection(FALSE,&primarySelectionAfter);
                const auto primaryAfterIdsRead=primarySelectionAfter?nativeArrayIdentities(primarySelectionAfter.Get(),primaryIdsAfter):
                    primaryCountAfter==S_OK&&primarySelectedAfter==0?S_OK:E_UNEXPECTED;
                const bool primarySelectionPreserved=primaryCountRead==S_OK&&primaryIdentityRead==S_OK&&primaryCountAfter==S_OK&&
                    primarySelectedAfter==primarySelectedCount&&primarySelectionAfterRead==primarySelectionRead&&
                    static_cast<bool>(primarySelectionAfter)==static_cast<bool>(primarySelection)&&primaryAfterIdsRead==S_OK&&primaryIdsAfter==primarySelectedIds;
                check("async_kind_owned_source_and_original_app_preserved",sourcesReady&&sourcesPreserved&&primarySelectionPreserved&&primaryLocation&&currentPidl_&&
                    ILIsEqual(primaryLocation.get(),currentPidl_.get())&&view_.Get()==primaryView.Get()&&folderView_.Get()==primaryFolder.Get()&&
                    navigationCount_==primaryNavigation&&history_.size()==primaryHistory&&historyIndex_==primaryHistoryIndex&&
                    desktop&&desktop->verifyIsolation()==S_OK,L"native media source identities="+std::to_wstring(mediaPaths.size()));
            }
            if (ribbon_.layout() == RibbonLayout::InstalledWindows10) {
                folderView_->SelectItem(3, SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_NOTAKEFOCUS);
                pumpUntil([&] { return !selectionStateDirty_; }, 2000);
                ribbon_.invalidateState(); ribbon_.flush();
                probeNativeMenus({{"native_stock_copy_to_dropdown_hierarchy", L"Copy to"},
                                  {"native_stock_open_with_dropdown_hierarchy", L"Open"}});
                execute(SelectNone);
            }
            auto hr = navigate((fixture / L"Subfolder").wstring());
            check("navigate_subfolder", SUCCEEDED(hr) && pumpUntil([&] { return !navigating_ && atLocation(fixture / L"Subfolder"); }, 5000));
            hr = execute(Back);
            check("back", SUCCEEDED(hr) && pumpUntil([&] { return !navigating_ && atLocation(fixture); }, 5000));
            hr = execute(Forward);
            check("forward", SUCCEEDED(hr) && pumpUntil([&] { return !navigating_ && atLocation(fixture / L"Subfolder"); }, 5000));
            hr = execute(Up);
            check("up", SUCCEEDED(hr) && pumpUntil([&] { return !navigating_ && atLocation(fixture); }, 5000));
            execute(HiddenItems);
            ready = pumpUntil([&] { int total = 0; return !navigating_ && folderView_ && SUCCEEDED(folderView_->ItemCount(SVGIO_ALLVIEW, &total)) && total == 1003; }, 10000);
            check("show_hidden_items", ready);
            const auto sampleCurrentPaneGeometry = [&](ULONGLONG geometryDeadline) {
                const auto geometryView = view_;
                const auto geometryFolder = folderView_;
                const auto geometryNavigation = navigationCount_;
                Pidl geometryLocation(currentPidl_ ? ILCloneFull(currentPidl_.get()) : nullptr);
                const auto geometryCurrent = [&] {
                    return !closing_ && !navigating_ && geometryView && geometryFolder && geometryLocation && currentPidl_ &&
                        view_.Get() == geometryView.Get() && folderView_.Get() == geometryFolder.Get() &&
                        navigationCount_ == geometryNavigation && ILGetSize(geometryLocation.get()) == ILGetSize(currentPidl_.get()) &&
                        std::memcmp(geometryLocation.get(), currentPidl_.get(), ILGetSize(geometryLocation.get())) == 0;
                };
                return readNativePaneGeometry(geometryView.Get(), window_, previewPane_, previewSplitter_,
                    preferences_.previewPane, geometryDeadline, geometryCurrent, previewGrip_);
            };
            const auto previewPolicyDeadline = GetTickCount64() + 5000;
            const auto originalPaneFrame = sampleCurrentPaneGeometry(previewPolicyDeadline);
            execute(PreviewPane);
            const auto previewPolicyNow = GetTickCount64();
            ready = previewPolicyNow < previewPolicyDeadline && pumpUntil([&] { return !navigating_ && folderView_; },
                static_cast<DWORD>(previewPolicyDeadline - previewPolicyNow));
            EXPLORERPANESTATE pane = EPS_DONTCARE; GetPaneState(EP_PreviewPane, &pane);
            check("preview_pane_policy", ready && preferences_.previewPane && (pane & EPS_DEFAULT_OFF) &&
                (pane & EPS_FORCE) && previewPane_ && previewRender_ && IsChild(window_,previewPane_) &&
                GetWindowThreadProcessId(previewPane_,nullptr)==GetCurrentThreadId());
            const auto previewPolicyGeometry = sampleCurrentPaneGeometry(previewPolicyDeadline);
            check("native_preview_toggle_preserves_full_native_footer_and_partitions_actual_content_slot",
                ready && originalPaneFrame.read == S_OK && originalPaneFrame.footerFull && originalPaneFrame.partition &&
                previewPolicyGeometry.read == S_OK && previewPolicyGeometry.footerFull && previewPolicyGeometry.partition &&
                EqualRect(&originalPaneFrame.frameClient, &previewPolicyGeometry.frameClient) &&
                EqualRect(&originalPaneFrame.footer, &previewPolicyGeometry.footer),
                L"before=" + paneGeometryFacts(originalPaneFrame) + L"; after=" + paneGeometryFacts(previewPolicyGeometry));
            const auto detailsPolicyDeadline = GetTickCount64() + 5000;
            execute(DetailsPane);
            const auto detailsPolicyNow = GetTickCount64();
            ready = detailsPolicyNow < detailsPolicyDeadline && pumpUntil([&] { return !navigating_ && folderView_; },
                static_cast<DWORD>(detailsPolicyDeadline - detailsPolicyNow));
            GetPaneState(EP_PreviewPane, &pane);
            check("panes_mutually_exclusive", ready && preferences_.detailsPane && !preferences_.previewPane && (pane & EPS_DEFAULT_OFF));
            const auto detailsPolicyGeometry = sampleCurrentPaneGeometry(detailsPolicyDeadline);
            check("native_details_transition_restores_full_public_view_and_keeps_native_footer",
                ready && detailsPolicyGeometry.read == S_OK && detailsPolicyGeometry.footerFull && detailsPolicyGeometry.partition &&
                EqualRect(&originalPaneFrame.frameClient, &detailsPolicyGeometry.frameClient) &&
                EqualRect(&originalPaneFrame.footer, &detailsPolicyGeometry.footer), paneGeometryFacts(detailsPolicyGeometry));
            {
                // Small local files avoid cloud recall and shared associations.
                // The browser selects the actual items. Production hosts their
                // registered native handler; the fixture draws no substitute.
                const auto paneRoot = fixture / L"Subfolder" / L"Native pane rendering";
                std::filesystem::create_directory(paneRoot);
                const std::array paths{paneRoot / L"Owned image A.bmp", paneRoot / L"Owned image B.bmp",
                    paneRoot / L"Owned preview A.rtf", paneRoot / L"Owned preview B.rtf"};
                const std::array<std::wstring, 2> previewTokens{L"OWNED-PREVIEW-ALPHA-41871", L"OWNED-PREVIEW-BETA-92653"};
                const auto writeBitmap = [](const std::filesystem::path& path, LONG width, LONG height, BYTE color) {
                    const DWORD stride = (static_cast<DWORD>(width) * 3 + 3) & ~3u;
                    const DWORD bytes = stride * static_cast<DWORD>(height);
                    BITMAPFILEHEADER file{}; file.bfType = 0x4d42;
                    file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER); file.bfSize = file.bfOffBits + bytes;
                    BITMAPINFOHEADER info{}; info.biSize = sizeof(info); info.biWidth = width; info.biHeight = height;
                    info.biPlanes = 1; info.biBitCount = 24; info.biCompression = BI_RGB; info.biSizeImage = bytes;
                    std::vector<BYTE> pixels(bytes, 0);
                    for (LONG y = 0; y < height; ++y) for (LONG x = 0; x < width; ++x) {
                        const auto offset = static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 3;
                        pixels[offset] = color; pixels[offset + 1] = static_cast<BYTE>(x * 2); pixels[offset + 2] = static_cast<BYTE>(y * 3);
                    }
                    std::ofstream output(path, std::ios::binary);
                    output.write(reinterpret_cast<const char*>(&file), sizeof(file));
                    output.write(reinterpret_cast<const char*>(&info), sizeof(info));
                    output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
                    return output.good();
                };
                bool filesReady = writeBitmap(paths[0], 72, 48, 41) && writeBitmap(paths[1], 120, 80, 193);
                for (size_t index = 0; index < previewTokens.size(); ++index) {
                    std::ofstream output(paths[index + 2], std::ios::binary);
                    output << "{\\rtf1\\ansi\\deff0 {\\fonttbl {\\f0 Segoe UI;}}\\f0\\fs32 ";
                    // These fixture tokens are fixed ASCII RTF text.
                    for (const auto character : previewTokens[index]) output.put(static_cast<char>(character));
                    output << "\\par}";
                    filesReady = output.good() && filesReady;
                }
                std::array<ComPtr<IShellItem2>, 4> items;
                std::array<FILE_ID_INFO, 4> before{};
                FILE_BASIC_INFO originalPreviewBasic{};
                HRESULT originalPreviewBasicRead = E_PENDING;
                std::array<std::vector<char>, 4> originalBytes;
                std::array<std::vector<std::wstring>, 2> detailsTokens;
                auto stage = filesReady ? S_OK : E_FAIL;
                const auto readBytes = [](const std::filesystem::path& path) {
                    std::ifstream input(path, std::ios::binary);
                    return std::vector<char>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>{});
                };
                const auto identity = [](const FILE_ID_INFO& value) {
                    std::array<BYTE, 16> bytes{};
                    std::copy(std::begin(value.FileId.Identifier), std::end(value.FileId.Identifier), bytes.begin());
                    return NativeFileIdentity{value.VolumeSerialNumber, bytes};
                };
                for (size_t index = 0; SUCCEEDED(stage) && index < paths.size(); ++index) {
                    const auto attributes = GetFileAttributesW(paths[index].c_str());
                    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE |
                        FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS))) { stage = E_ACCESSDENIED; break; }
                    stage = SHCreateItemFromParsingName(paths[index].c_str(), nullptr, IID_PPV_ARGS(&items[index]));
                    if (SUCCEEDED(stage)) stage = nativeFileIdentity(paths[index], before[index]);
                    originalBytes[index] = readBytes(paths[index]);
                }
                if (SUCCEEDED(stage)) {
                    struct SourceMetadataHandle {
                        HANDLE value = INVALID_HANDLE_VALUE;
                        ~SourceMetadataHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
                    } source{CreateFileW(paths[2].c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL, nullptr)};
                    originalPreviewBasicRead = source.value == INVALID_HANDLE_VALUE ? HRESULT_FROM_WIN32(GetLastError()) :
                        GetFileInformationByHandleEx(source.value, FileBasicInfo, &originalPreviewBasic, sizeof(originalPreviewBasic)) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
                    if (FAILED(originalPreviewBasicRead)) stage = originalPreviewBasicRead;
                }
                for (size_t index = 0; SUCCEEDED(stage) && index < detailsTokens.size(); ++index) {
                    detailsTokens[index].push_back(itemName(items[index].Get(), SIGDN_NORMALDISPLAY));
                    if (detailsTokens[index].back().empty()) { stage = E_UNEXPECTED; break; }
                    for (const auto& key : std::array<PROPERTYKEY, 2>{PKEY_Image_Dimensions, PKEY_Size}) {
                        PROPVARIANT property{}; wchar_t text[512]{};
                        stage = items[index]->GetProperty(key, &property);
                        if (SUCCEEDED(stage)) stage = PSFormatForDisplay(key, property, PDFF_DEFAULT, text, static_cast<DWORD>(std::size(text)));
                        PropVariantClear(&property);
                        if (SUCCEEDED(stage) && text[0]) detailsTokens[index].emplace_back(text); else if (SUCCEEDED(stage)) stage = E_UNEXPECTED;
                    }
                }
                ComPtr<IQueryAssociations> association;
                wchar_t originalHandler[128]{}; DWORD handlerCharacters = static_cast<DWORD>(std::size(originalHandler));
                auto handlerRead = SUCCEEDED(stage) ? items[2]->BindToHandler(nullptr, BHID_AssociationArray,
                    IID_PPV_ARGS(&association)) : stage;
                if (SUCCEEDED(handlerRead)) handlerRead = association->GetString(ASSOCF_NOTRUNCATE, ASSOCSTR_SHELLEXTENSION,
                    L"{8895b1c6-b41f-4c1c-a562-0d564250836f}", originalHandler, &handlerCharacters);
                GUID handler{}, nativeRtf{};
                const bool nativeClass = SUCCEEDED(handlerRead) && SUCCEEDED(CLSIDFromString(originalHandler, &handler)) &&
                    SUCCEEDED(CLSIDFromString(L"{a42c2ccb-67d3-46fa-abe6-7d2f3488c7a3}", &nativeRtf)) && IsEqualGUID(handler, nativeRtf);
                HRESULT moduleRead = E_NOTIMPL;
                bool nativeModule = false;
                if (nativeClass) {
                    std::array<wchar_t, 32768> server{}, expanded{}, system{};
                    DWORD type = 0, bytes = static_cast<DWORD>(sizeof(server));
                    const auto key = std::wstring(L"CLSID\\") + originalHandler + L"\\InprocServer32";
                    const auto read = RegGetValueW(HKEY_CLASSES_ROOT, key.c_str(), nullptr,
                        RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND | RRF_SUBKEY_WOW6464KEY,
                        &type, server.data(), &bytes);
                    moduleRead = read == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(read);
                    std::filesystem::path serverPath;
                    if (SUCCEEDED(moduleRead)) {
                        if (type == REG_EXPAND_SZ) {
                            const auto length = ExpandEnvironmentStringsW(server.data(), expanded.data(), static_cast<DWORD>(expanded.size()));
                            if (!length) moduleRead = HRESULT_FROM_WIN32(GetLastError());
                            else if (length > expanded.size()) moduleRead = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
                            else serverPath = expanded.data();
                        } else serverPath = server.data();
                    }
                    const auto systemLength = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
                    FILE_ID_INFO expectedModule{}, actualModule{};
                    if (SUCCEEDED(moduleRead) && (!systemLength || systemLength >= system.size() || !serverPath.is_absolute()))
                        moduleRead = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                    if (SUCCEEDED(moduleRead)) moduleRead = nativeFileIdentity(std::filesystem::path(system.data()) / L"shell32.dll", expectedModule);
                    if (SUCCEEDED(moduleRead)) moduleRead = nativeFileIdentity(serverPath, actualModule);
                    nativeModule = SUCCEEDED(moduleRead) && identity(expectedModule) == identity(actualModule);
                    if (SUCCEEDED(moduleRead) && !nativeModule) moduleRead = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
                }
                const bool nativeHandler = nativeClass && nativeModule;
                check("native_pane_owned_sources_and_original_preview_association", SUCCEEDED(stage),
                    L"source read=" + hresultMessage(stage) + L"; effective preview association=" + hresultMessage(handlerRead) +
                    L"; original CLSID=" + originalHandler + L"; effective64bitserver=" + hresultMessage(moduleRead) +
                    L"; actual native shell32 FileID=" + std::to_wstring(nativeModule) + L"; system RTF eligible=" + std::to_wstring(nativeHandler) +
                    L"; original RTF attrs/times=" + hresultMessage(originalPreviewBasicRead));
                const auto mainView = view_;
                const auto mainNavigation = navigationCount_;
                const auto mainHistory = history_.size();
                const auto mainHistoryIndex = historyIndex_;
                const auto mainQuery = activeQuery_;
                Pidl mainLocation(currentPidl_ ? ILCloneFull(currentPidl_.get()) : nullptr);
                std::wstring previewActivationDiagnostic;
                std::wstring previewTargetDiagnostic;
                auto releasePaneApp = [](ExplorerApp* value) {
                    if (value->window_ && IsWindow(value->window_)) SendMessageW(value->window_, WM_CLOSE, 0, 0);
                    if (FAILED(value->shutdownStatus_)) {
                        std::fprintf(stderr, "headless Pane owned child did not drain native workers HRESULT=0x%08lX\n",
                            static_cast<unsigned long>(value->shutdownStatus_)); std::fflush(stderr);
                        if (!TerminateProcess(GetCurrentProcess(), 9)) std::_Exit(9);
                        std::_Exit(9);
                    }
                    value->Release();
                };
                std::unique_ptr<ExplorerApp, decltype(releasePaneApp)> paneApp(
                    new ExplorerApp(instance_, true, requestedRibbonLayout_), releasePaneApp);
                paneApp->headlessPaneHostingTrace_ = true;
                VisualScene scene; scene.details = true;
                auto created = SUCCEEDED(stage) ? paneApp->prepareHeadlessVisual(scene) : stage;
                paneApp->preferences_.navigationPane = false;
                if (SUCCEEDED(created)) created = paneApp->create(paneRoot.wstring());
                const auto paneReady = [&] {
                    if (paneApp->closing_ || paneApp->navigating_ || !paneApp->folderView_ || !paneApp->view_) return false;
                    ComPtr<IShellItem> folder;
                    int comparison = 1;
                    ComPtr<IShellItem> expected;
                    int total = 0;
                    return SUCCEEDED(SHCreateItemFromParsingName(paneRoot.c_str(), nullptr, IID_PPV_ARGS(&expected))) &&
                        SUCCEEDED(paneApp->currentFolder(folder)) && SUCCEEDED(folder->Compare(expected.Get(), SICHINT_CANONICAL, &comparison)) &&
                        comparison == 0 && SUCCEEDED(paneApp->folderView_->ItemCount(SVGIO_ALLVIEW, &total)) && total == 4;
                };
                bool actualReady = SUCCEEDED(created) && pumpUntil(paneReady, 5000);
                const auto exactSelected = [&](IFolderView2* folder, size_t index) {
                    ComPtr<IShellItemArray> selected; std::set<NativeFileIdentity> ids;
                    return folder && SUCCEEDED(folder->GetSelection(FALSE, &selected)) &&
                        SUCCEEDED(nativeArrayIdentities(selected.Get(), ids)) && ids == std::set<NativeFileIdentity>{identity(before[index])};
                };
                struct FocusedPaneItem { HRESULT read = E_PENDING; int index = -1; bool exact = false; };
                const auto focusedItem = [&](IFolderView2* folder, size_t index) {
                    FocusedPaneItem result;
                    result.read = folder ? folder->GetFocusedItem(&result.index) : E_POINTER;
                    ComPtr<IShellItem> item;
                    if (SUCCEEDED(result.read)) result.read = result.index >= 0 ?
                        folder->GetItem(result.index, IID_PPV_ARGS(&item)) : E_UNEXPECTED;
                    PWSTR path = nullptr;
                    if (SUCCEEDED(result.read)) result.read = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
                    FILE_ID_INFO actual{};
                    if (SUCCEEDED(result.read)) result.read = path ? nativeFileIdentity(path, actual) : E_UNEXPECTED;
                    CoTaskMemFree(path);
                    result.exact = SUCCEEDED(result.read) && identity(actual) == identity(before[index]);
                    return result;
                };
                const auto selectOnce = [&](IShellView* nativeView, IFolderView2* folder, size_t index) {
                    PIDLIST_ABSOLUTE raw = nullptr;
                    auto selected = SHGetIDListFromObject(items[index].Get(), &raw); Pidl target(raw);
                    if (SUCCEEDED(selected)) selected = nativeView->SelectItem(ILFindLastID(target.get()),
                        SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_FOCUSED | SVSI_NOTAKEFOCUS | SVSI_ENSUREVISIBLE);
                    return SUCCEEDED(selected) && pumpUntil([&] { return exactSelected(folder, index) && focusedItem(folder, index).exact; }, 2500);
                };
                const auto capturePane = [&](HWND host, HWND content, const PaneObservation& observed, const wchar_t* name, VisualCaptureReport& captured, HWND ownedPreview=nullptr) {
                    const auto rendererCurrent = [&] {
                        const auto currentPrivate = PrivateDesktop::current();
                        return !ownedPreview || (currentPrivate && observed.rendererAdmission &&
                            paneRenderersCurrent(ownedPreview, observed.bounds, currentPrivate->name(), *observed.rendererAdmission) &&
                            SUCCEEDED(currentPrivate->verifyIsolation()));
                    };
                    if (!rendererCurrent()) return E_ACCESSDENIED;
                    RECT frame{}; bool rtl = false;
                    HWND nativeFrame = ownedPreview?ownedPreview:GetParent(content);
                    for (; nativeFrame && nativeFrame != host; nativeFrame = ownedPreview?nullptr:GetParent(nativeFrame)) {
                        if (GetClientRect(nativeFrame, &frame) && SUCCEEDED(mapUiRect(nativeFrame, nullptr, frame, &frame)) &&
                            observed.bounds.left >= frame.left && observed.bounds.right <= frame.right &&
                            observed.bounds.top >= frame.top && observed.bounds.bottom <= frame.bottom) break;
                    }
                    if (!nativeFrame || nativeFrame == host || FAILED(windowUiDirection(host, &rtl))) return E_INVALIDARG;
                    // Observation only: distinguish actual sibling occlusion
                    // from the native root's printing/composition behavior.
                    const auto captureDesktop = PrivateDesktop::current();
                    PrivateWindowSnapshot captureRoot{}, captureContent{}, capturePaneWindow{};
                    const auto captureRootRead = ownedPreview ? paneWindowSnapshot(host, captureRoot) : E_PENDING;
                    const auto captureContentRead = ownedPreview ? paneWindowSnapshot(content, captureContent) : E_PENDING;
                    const auto capturePaneRead = ownedPreview ? paneWindowSnapshot(ownedPreview, capturePaneWindow) : E_PENDING;
                    const auto captureEpoch = paneApp->previewEpoch_, captureRevision = paneApp->commandSourceRevision_;
                    const auto captureView = paneApp->view_.Get(); const auto captureFolder = paneApp->folderView_.Get();
                    const auto captureNavigation = paneApp->navigationCount_; const auto captureLocation = paneApp->currentPidl_.get();
                    const auto printComposition = [&](const wchar_t* phase, HRESULT printStatus) {
                        if (!ownedPreview) return;
                        struct LastErrorRestore { DWORD value = GetLastError(); ~LastErrorRestore() { SetLastError(value); } } lastErrorRestore;
                        const auto sourceCurrent = [&] {
                            return !paneApp->closing_ && !paneApp->navigating_ && paneApp->window_ == host &&
                                paneApp->view_.Get() == captureView && paneApp->folderView_.Get() == captureFolder &&
                                paneApp->navigationCount_ == captureNavigation && paneApp->currentPidl_.get() == captureLocation &&
                                paneApp->previewEpoch_ == captureEpoch && paneApp->commandSourceRevision_ == captureRevision &&
                                GetWindowLongPtrW(host, GWLP_USERDATA) == reinterpret_cast<LONG_PTR>(paneApp.get());
                        };
                        const auto tupleCurrent = [&] {
                            if (!captureDesktop || PrivateDesktop::current() != captureDesktop || !sourceCurrent() ||
                                captureDesktop->verifyIsolation() != S_OK ||
                                (observed.observationDeadline && GetTickCount64() >= observed.observationDeadline)) return false;
                            PrivateWindowSnapshot rootNow{}, contentNow{}, paneNow{};
                            const auto own = [&](const PrivateWindowSnapshot& value) {
                                return value.thread == GetCurrentThreadId() && value.process == GetCurrentProcessId() &&
                                    value.root == host && value.desktopRead == S_OK && std::wstring_view(value.desktop.data()) == captureDesktop->name();
                            };
                            return captureRootRead == S_OK && captureContentRead == S_OK && capturePaneRead == S_OK &&
                                own(captureRoot) && own(captureContent) && own(capturePaneWindow) && capturePaneWindow.parent == host &&
                                paneWindowSnapshot(host, rootNow) == S_OK && samePaneWindow(captureRoot, rootNow) &&
                                paneWindowSnapshot(content, contentNow) == S_OK && samePaneWindow(captureContent, contentNow) &&
                                paneWindowSnapshot(ownedPreview, paneNow) == S_OK && samePaneWindow(capturePaneWindow, paneNow) && sourceCurrent();
                        };
                        const bool before = tupleCurrent();
                        std::wostringstream diagnostic;
                        diagnostic << L"phase=" << phase << L"; source=" << name << L"; print=" << hresultMessage(printStatus)
                            << L"; root/content/pane/nativeCaptureFrame=" << reinterpret_cast<UINT_PTR>(host) << L"/"
                            << reinterpret_cast<UINT_PTR>(content) << L"/" << reinterpret_cast<UINT_PTR>(ownedPreview) << L"/"
                            << reinterpret_cast<UINT_PTR>(nativeFrame) << L"; tuple reads=" << hresultMessage(captureRootRead) << L"/"
                            << hresultMessage(captureContentRead) << L"/" << hresultMessage(capturePaneRead) << L"; private/source/tuple before=" << before;
                        if (before) {
                            HWND browserFrame = content; unsigned ancestors = 0;
                            while (browserFrame && GetAncestor(browserFrame, GA_PARENT) != host && ++ancestors <= 16)
                                browserFrame = GetAncestor(browserFrame, GA_PARENT);
                            if (ancestors > 16 || !browserFrame || GetAncestor(browserFrame, GA_PARENT) != host) browserFrame = nullptr;
                            const POINT physical{observed.bounds.left + (observed.bounds.right - observed.bounds.left) / 2,
                                observed.bounds.top + (observed.bounds.bottom - observed.bounds.top) / 2};
                            POINT local{}; const auto mapped = mapUiPoint(nullptr, host, physical, &local);
                            SetLastError(ERROR_SUCCESS);
                            const auto hit = mapped == S_OK ? ChildWindowFromPointEx(host, local, CWP_SKIPINVISIBLE) : nullptr;
                            const auto hitError = GetLastError();
                            diagnostic << L"; actual direct browser frame=" << reinterpret_cast<UINT_PTR>(browserFrame)
                                << L"; physical/local point=" << physical.x << L"," << physical.y << L"/" << local.x << L"," << local.y
                                << L"; map=" << hresultMessage(mapped) << L"; CWP_SKIPINVISIBLE hit/error=" << reinterpret_cast<UINT_PTR>(hit)
                                << L"/" << hitError << L"; hit pane/browserFrame=" << (hit == ownedPreview) << L"/" << (hit == browserFrame);
                            unsigned rows = 0; HWND child = GetTopWindow(host);
                            for (; child && rows < 64; child = GetWindow(child, GW_HWNDNEXT), ++rows) {
                                if (!sourceCurrent() || (observed.observationDeadline && GetTickCount64() >= observed.observationDeadline)) break;
                                DWORD process = 0; SetLastError(ERROR_SUCCESS);
                                const auto thread = GetWindowThreadProcessId(child, &process); const auto threadError = GetLastError();
                                const auto parent = GetAncestor(child, GA_PARENT);
                                const bool owned = parent == host && thread == GetCurrentThreadId() && process == GetCurrentProcessId();
                                diagnostic << L"; child[" << rows << L"] HWND/parent/PID/TID/error/creatorOwned=" << reinterpret_cast<UINT_PTR>(child)
                                    << L"/" << reinterpret_cast<UINT_PTR>(parent) << L"/" << process << L"/" << thread << L"/" << threadError << L"/" << owned;
                                if (!owned) continue;
                                std::array<wchar_t, 128> type{}; SetLastError(ERROR_SUCCESS);
                                const auto classRead = GetClassNameW(child, type.data(), static_cast<int>(type.size())); const auto classError = GetLastError();
                                SetLastError(ERROR_SUCCESS);
                                const auto style = GetWindowLongPtrW(child, GWL_STYLE); const auto styleError = GetLastError();
                                SetLastError(ERROR_SUCCESS);
                                const auto exStyle = GetWindowLongPtrW(child, GWL_EXSTYLE); const auto exStyleError = GetLastError();
                                RECT rectangle{}; SetLastError(ERROR_SUCCESS);
                                const auto rectangleRead = GetWindowRect(child, &rectangle); const auto rectangleError = GetLastError();
                                diagnostic << L"; class/count/error=[";
                                if (classRead > 0 && classRead < static_cast<int>(type.size()) - 1) diagnostic.write(type.data(), classRead);
                                diagnostic << L"]/" << classRead << L"/" << classError << L"; style/exStyle/errors=" << static_cast<UINT_PTR>(style)
                                    << L"/" << static_cast<UINT_PTR>(exStyle) << L"/" << styleError << L"/" << exStyleError << L"; visible=" << IsWindowVisible(child)
                                    << L"; rect/read/error=" << rectangle.left << L"," << rectangle.top << L"," << rectangle.right << L"," << rectangle.bottom
                                    << L"/" << rectangleRead << L"/" << rectangleError;
                            }
                            diagnostic << L"; child rows/unreadOrOverflow=" << rows << L"/" << (child != nullptr);
                        }
                        diagnostic << L"; fresh private/source/tuple after=" << tupleCurrent()
                            << L"; returned capture colors/hash=" << captured.inspectionUniqueColors << L"/" << captured.inspectionPixelHash
                            << L"; failed capture statistics unavailable: captureWindowPng publishes report only on success";
                        std::cerr << "headless-preview-print-composition " << jsonString(diagnostic.str()) << std::endl;
                    };
                    const auto paneHitCurrent = [&] {
                        if (!ownedPreview) return true;
                        if (!captureDesktop || PrivateDesktop::current() != captureDesktop || captureDesktop->verifyIsolation() != S_OK ||
                            (observed.observationDeadline && GetTickCount64() >= observed.observationDeadline) ||
                            paneApp->closing_ || paneApp->navigating_ || paneApp->window_ != host ||
                            paneApp->view_.Get() != captureView || paneApp->folderView_.Get() != captureFolder ||
                            paneApp->navigationCount_ != captureNavigation || paneApp->currentPidl_.get() != captureLocation ||
                            paneApp->previewEpoch_ != captureEpoch || paneApp->commandSourceRevision_ != captureRevision ||
                            GetWindowLongPtrW(host, GWLP_USERDATA) != reinterpret_cast<LONG_PTR>(paneApp.get()) ||
                            !paneGeometryOwned(host, host) || !paneGeometryOwned(ownedPreview, host) ||
                            GetAncestor(ownedPreview, GA_PARENT) != host) return false;
                        const POINT physical{observed.bounds.left+(observed.bounds.right-observed.bounds.left)/2,
                            observed.bounds.top+(observed.bounds.bottom-observed.bounds.top)/2};
                        POINT local{};
                        if (mapUiPoint(nullptr, host, physical, &local) != S_OK ||
                            ChildWindowFromPointEx(host, local, CWP_SKIPINVISIBLE) != ownedPreview) return false;
                        PrivateWindowSnapshot rootNow{}, paneNow{};
                        return paneWindowSnapshot(host, rootNow) == S_OK && samePaneWindow(captureRoot, rootNow) &&
                            paneWindowSnapshot(ownedPreview, paneNow) == S_OK && samePaneWindow(capturePaneWindow, paneNow) &&
                            captureDesktop->verifyIsolation() == S_OK && paneApp->previewEpoch_ == captureEpoch &&
                            paneApp->commandSourceRevision_ == captureRevision;
                    };
                    VisualCaptureOptions options; options.includeFrame = false;
                    options.minimumUniqueColors = 2; options.requireVisibleChildren = false;
                    options.nativeClientCropSource = host;
                    options.nativeClientCropPrintLayout = rtl ? LAYOUT_RTL : 0;
                    options.nativeClientCropPrintFlags = 0;
                    options.pixelInspectionBounds = observed.bounds;
                    OffsetRect(&options.pixelInspectionBounds, -frame.left, -frame.top);
                    InflateRect(&options.pixelInspectionBounds, -4, -4);
                    const auto path = std::filesystem::absolute(report).parent_path() / (report.stem().wstring() + L"-pane-" + name + L"-" +
                        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L".png");
                    options.nativeClientCropSourceImage = path.parent_path() / (path.stem().wstring() + L"-root-source.png");
                    // A verified private surface can diagnose semantic failure;
                    // successful rendering still requires observed.matched.
                    HRESULT printed = E_UNEXPECTED;
                    if (observed.privacyChecked && observed.privateWindows && PrivateDesktop::current() &&
                        RedrawWindow(host, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW) && GdiFlush() && rendererCurrent() && paneHitCurrent()) {
                        printComposition(L"before-root-print", E_PENDING);
                        printed = captureWindowPng(*PrivateDesktop::current(), nativeFrame, path, options, captured);
                    }
                    const bool currentAfterPrint = rendererCurrent() && paneHitCurrent();
                    printComposition(L"after-renderer-source-guard", printed);
                    return currentAfterPrint ? printed : E_ACCESSDENIED;
                };
                bool paneIsolationPreserved = true;
                WindowIsolationControlReport paneMessageControls;
                HRESULT paneCalibration = E_PENDING;
                const auto originalPaneSourcesCurrent = [&] {
                    for (size_t sourceIndex = 0; sourceIndex < paths.size(); ++sourceIndex) {
                        FILE_ID_INFO sourceIdentity{};
                        if (nativeFileIdentity(paths[sourceIndex], sourceIdentity) != S_OK || identity(sourceIdentity) != identity(before[sourceIndex]) ||
                            readBytes(paths[sourceIndex]) != originalBytes[sourceIndex]) return false;
                    }
                    struct PreviewSourceHandle {
                        HANDLE value = INVALID_HANDLE_VALUE;
                        ~PreviewSourceHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
                    } originalSource{CreateFileW(paths[2].c_str(), FILE_READ_ATTRIBUTES,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL, nullptr)};
                    FILE_BASIC_INFO currentBasic{};
                    return originalPreviewBasicRead == S_OK && originalSource.value != INVALID_HANDLE_VALUE &&
                        GetFileInformationByHandleEx(originalSource.value, FileBasicInfo, &currentBasic, sizeof(currentBasic)) &&
                        currentBasic.FileAttributes == originalPreviewBasic.FileAttributes &&
                        currentBasic.CreationTime.QuadPart == originalPreviewBasic.CreationTime.QuadPart &&
                        currentBasic.LastWriteTime.QuadPart == originalPreviewBasic.LastWriteTime.QuadPart &&
                        currentBasic.ChangeTime.QuadPart == originalPreviewBasic.ChangeTime.QuadPart;
                };
                const auto verifyCase = [&](bool preview) {
                    std::array<PaneObservation, 2> observations;
                    std::array<VisualCaptureReport, 2> captures;
                    std::array<HRESULT, 2> prints{E_PENDING, E_PENDING};
                    std::array<bool, 2> selections{}, retainedStates{};
                    std::array<bool, 2> previewSourcesPreserved{};
                    std::array<std::uint64_t, 2> observedPreviewEpochs{};
                    std::array<FocusedPaneItem, 2> focusedItems;
                    bool passed = actualReady;
                    PrivatePresentation presentation(paneApp->window_, true);
                    const auto retainedView = paneApp->view_;
                    const auto retainedFolder = paneApp->folderView_;
                    const auto navigation = paneApp->navigationCount_;
                    const auto historyCount = paneApp->history_.size();
                    const auto historyIndex = paneApp->historyIndex_;
                    const auto query = paneApp->activeQuery_;
                    const auto revision = paneApp->searchInteractionRevision_;
                    Pidl location(paneApp->currentPidl_ ? ILCloneFull(paneApp->currentPidl_.get()) : nullptr);
                    ComPtr<IObjectWithSite> located, browserLocated;
                    ComPtr<IUnknown> viewSite, siteIdentity, appIdentity;
                    auto siteRead = paneApp->browser_.As(&browserLocated);
                    if (SUCCEEDED(siteRead)) siteRead = browserLocated->GetSite(IID_PPV_ARGS(&siteIdentity));
                    if (SUCCEEDED(siteRead)) siteRead = paneApp->QueryInterface(IID_PPV_ARGS(&appIdentity));
                    if (SUCCEEDED(siteRead)) siteRead = retainedView ? retainedView.As(&located) : E_UNEXPECTED;
                    if (SUCCEEDED(siteRead)) siteRead = located->GetSite(IID_PPV_ARGS(&viewSite));
                    const bool originalSite = SUCCEEDED(siteRead) && siteIdentity.Get() == appIdentity.Get() && viewSite;
                    HWND content = nullptr;
                    const auto windowRead = retainedView ? retainedView->GetWindow(&content) : E_UNEXPECTED;
                    passed = passed && presentation.ready && originalSite && SUCCEEDED(windowRead);
                    // Match run(): show the owned private frame, then activate
                    // its actual view before the file-selection notification.
                    const auto activation = passed ? retainedView->UIActivate(SVUIA_ACTIVATE_FOCUS) : E_ACCESSDENIED;
                    passed = passed && SUCCEEDED(activation) && !paneApp->closing_ && !paneApp->navigating_ &&
                        paneApp->view_.Get() == retainedView.Get() && paneApp->folderView_.Get() == retainedFolder.Get();
                    const bool caseReady = passed;
                    const auto caseUnchanged = [&] {
                        return !paneApp->closing_ && !paneApp->navigating_ && paneApp->view_.Get() == retainedView.Get() &&
                            paneApp->folderView_.Get() == retainedFolder.Get() && paneApp->navigationCount_ == navigation &&
                            paneApp->history_.size() == historyCount && paneApp->historyIndex_ == historyIndex &&
                            paneApp->activeQuery_ == query && paneApp->searchInteractionRevision_ == revision &&
                            location && paneApp->currentPidl_ && ILGetSize(location.get()) == ILGetSize(paneApp->currentPidl_.get()) &&
                            std::memcmp(location.get(), paneApp->currentPidl_.get(), ILGetSize(location.get())) == 0;
                    };
                    for (size_t index = 0; index < observations.size(); ++index) {
                        const size_t file = preview ? index + 2 : index;
                        const bool precedingSourceCurrent = index == 0 || !preview ||
                            (retainedStates[index - 1] && previewSourcesPreserved[index - 1] && caseUnchanged() &&
                             observedPreviewEpochs[index - 1] && paneApp->previewEpoch_ == observedPreviewEpochs[index - 1] &&
                             paneApp->previewSourceCurrent(true) && exactSelected(retainedFolder.Get(), file - 1) &&
                             focusedItem(retainedFolder.Get(), file - 1).exact && originalPaneSourcesCurrent() && caseUnchanged() &&
                             paneApp->previewEpoch_ == observedPreviewEpochs[index - 1]);
                        const bool selected = caseReady && (index == 0 || observations[index - 1].privateWindows) &&
                            precedingSourceCurrent &&
                            !paneApp->closing_ && !paneApp->navigating_ && paneApp->view_.Get() == retainedView.Get() &&
                            paneApp->folderView_.Get() == retainedFolder.Get() && paneApp->navigationCount_ == navigation &&
                            selectOnce(retainedView.Get(), retainedFolder.Get(), file);
                        selections[index] = selected;
                        auto& observedPreviewEpoch = observedPreviewEpochs[index];
                        const auto previewSourceReady = [&]() -> HRESULT {
                            const auto& unchanged = caseUnchanged;
                            if (!selected || !unchanged() || !exactSelected(retainedFolder.Get(), file) ||
                                !focusedItem(retainedFolder.Get(), file).exact || !unchanged()) return E_ABORT;
                            if (!originalPaneSourcesCurrent()) return E_ABORT;
                            ComPtr<IUnknown> currentSite;
                            if (!located || located->GetSite(IID_PPV_ARGS(&currentSite)) != S_OK || currentSite.Get() != viewSite.Get() || !unchanged()) return E_ABORT;
                            const auto epoch = paneApp->previewEpoch_;
                            if (observedPreviewEpoch && epoch != observedPreviewEpoch) return E_ABORT;
                            if (!paneApp->previewHost_) return E_PENDING;
                            const auto status = paneApp->previewHost_->status();
                            if (status.requestedEpoch != epoch || status.activeEpoch != epoch || !status.ready) {
                                if (status.stage == PreviewHostStage::Failed || status.stage == PreviewHostStage::Stopped)
                                    return FAILED(status.result) ? status.result : E_FAIL;
                                return E_PENDING;
                            }
                            const auto& visuals=status.visuals;
                            if(status.nativeResult==E_PENDING||visuals.pending||
                               visuals.requestedRevision!=visuals.attemptedRevision)return E_PENDING;
                            const auto palette=themePalette();
                            LOGFONTW font{};
                            const bool fontRead=paneApp->font_&&GetObjectW(paneApp->font_,sizeof(font),&font)==sizeof(font);
                            if(fontRead) {
                                const auto end=std::find(font.lfFaceName,font.lfFaceName+LF_FACESIZE,L'\0');
                                if(end==font.lfFaceName+LF_FACESIZE)return E_UNEXPECTED;
                                std::fill(end,font.lfFaceName+LF_FACESIZE,L'\0');
                            }
                            // Optional native rejection remains a raw receipt.
                            // It cannot stand in for the content/pixel proof.
                            const bool exactVisuals=visuals.requestedRevision&&
                                visuals.requestedRevision==visuals.attemptedRevision&&visuals.attemptedEpoch==epoch&&!visuals.pending&&
                                visuals.requested.backgroundPresent&&visuals.requested.textPresent&&
                                visuals.requested.background==palette.surface(ThemeSurface::Content)&&
                                visuals.requested.text==palette.foreground(ThemeSurface::Content)&&
                                visuals.requested.fontPresent==fontRead&&
                                (!fontRead||std::memcmp(&visuals.requested.font,&font,sizeof(font))==0)&&
                                visuals.attempted.backgroundPresent==visuals.requested.backgroundPresent&&
                                visuals.attempted.textPresent==visuals.requested.textPresent&&
                                visuals.attempted.fontPresent==visuals.requested.fontPresent&&
                                visuals.attempted.background==visuals.requested.background&&visuals.attempted.text==visuals.requested.text&&
                                (!fontRead||std::memcmp(&visuals.attempted.font,&font,sizeof(font))==0)&&
                                visuals.queryAttempted&&
                                (visuals.queryResult!=S_OK||
                                 (visuals.backgroundAttempted&&visuals.textAttempted&&visuals.fontAttempted==fontRead));
                            if(status.nativeResult!=S_OK||!exactVisuals)return E_UNEXPECTED;
                            if (!paneApp->previewSourceCurrent(true) || paneApp->previewEpoch_ != epoch || !unchanged() ||
                                !exactSelected(retainedFolder.Get(), file) || !focusedItem(retainedFolder.Get(), file).exact || !unchanged()) return E_ABORT;
                            if (!paneApp->previewRender_ || !IsWindowVisible(paneApp->previewRender_)) return E_PENDING;
                            observedPreviewEpoch = epoch;
                            return S_OK;
                        };
                        if (selected) observations[index] = observeNativePane(paneApp->window_, content,
                            preview ? std::vector<std::wstring>{previewTokens[index]} : detailsTokens[index],
                            index ? (preview ? previewTokens[0] : detailsTokens[0].front()) : L"", preview,
                            preview?paneApp->previewPane_:nullptr, preview ? &paneMessageControls : nullptr,
                            preview ? std::function<HRESULT()>(previewSourceReady) : std::function<HRESULT()>{});
                        if(preview) {
                            const auto status=paneApp->previewHost_?paneApp->previewHost_->status():PreviewHostStatus{};
                            observations[index].detail+=L"; actual manager(stage/nativeStage/result/native/cleanup)="+
                                std::to_wstring(static_cast<unsigned>(status.stage))+L"/"+
                                std::to_wstring(static_cast<unsigned>(status.nativeStage))+L"/"+hresultMessage(status.result)+L"/"+
                                hresultMessage(status.nativeResult)+L"/"+hresultMessage(status.cleanupResult)+
                                L"; actual manager epochs(requested/active/creator/latched)="+
                                std::to_wstring(status.requestedEpoch)+L"/"+std::to_wstring(status.activeEpoch)+L"/"+
                                std::to_wstring(paneApp->previewEpoch_)+L"/"+std::to_wstring(observedPreviewEpoch)+
                                L"; worker(started/exited/desktop/ready/pending)="+
                                std::to_wstring(status.workerStarted)+L"/"+std::to_wstring(status.workerExited)+L"/"+
                                std::to_wstring(status.desktopMatchesCreator)+L"/"+std::to_wstring(status.ready)+L"/"+
                                std::to_wstring(status.pending)+L"; initialization="+
                                std::to_wstring(static_cast<unsigned>(status.initialization));
                            const auto& visuals=status.visuals;
                            observations[index].detail+=L"; optional visuals revisions(requested/attempted/epoch/pending)="+
                                std::to_wstring(visuals.requestedRevision)+L"/"+std::to_wstring(visuals.attemptedRevision)+L"/"+
                                std::to_wstring(visuals.attemptedEpoch)+L"/"+std::to_wstring(visuals.pending)+
                                L"; optional native attempts(query/background/text/font)="+
                                std::to_wstring(visuals.queryAttempted)+L"/"+std::to_wstring(visuals.backgroundAttempted)+L"/"+
                                std::to_wstring(visuals.textAttempted)+L"/"+std::to_wstring(visuals.fontAttempted)+
                                L"; optional native HRESULTs(result/query/background/text/font)="+hresultMessage(visuals.result)+L"/"+
                                hresultMessage(visuals.queryResult)+L"/"+hresultMessage(visuals.backgroundResult)+L"/"+
                                hresultMessage(visuals.textResult)+L"/"+hresultMessage(visuals.fontResult);
                            const auto& cleanup=status.cleanup;
                            observations[index].detail+=L"; cleanup(epoch/stage/result/pending)="+
                                std::to_wstring(cleanup.epoch)+L"/"+std::to_wstring(static_cast<unsigned>(cleanup.stage))+L"/"+
                                hresultMessage(cleanup.result)+L"/"+std::to_wstring(cleanup.pending)+
                                L"; cleanup attempts(Unload/siteClear/windowDestroy)="+
                                std::to_wstring(cleanup.unloadAttempted)+L"/"+std::to_wstring(cleanup.siteClearAttempted)+L"/"+
                                std::to_wstring(cleanup.windowDestroyAttempted)+
                                L"; cleanup raw HRESULTs(Unload/siteClear/windowDestroy)="+
                                hresultMessage(cleanup.unloadResult)+L"/"+hresultMessage(cleanup.siteClearResult)+L"/"+
                                hresultMessage(cleanup.windowDestroyResult);
                        }
                        prints[index] = (!preview || previewSourceReady() == S_OK) ? capturePane(paneApp->window_, content, observations[index], preview ? (index ? L"preview-b" : L"preview-a") :
                            (index ? L"details-b" : L"details-a"), captures[index],preview?paneApp->previewPane_:nullptr)
                            : E_ABORT;
                        if (preview && previewSourceReady() != S_OK) {
                            prints[index] = E_ABORT; observations[index].matched = false;
                        }
                        if (preview && index == 1) {
                            // These production focus commands share the original
                            // content observation's deadline. Queue acceptance is
                            // separate from actual native SetFocus/QueryFocus.
                            const auto paneFocusDeadline = observations[index].observationDeadline;
                            const auto paneFocusEpoch = observedPreviewEpoch;
                            const auto paneFocusHost = paneApp->previewHost_.get();
                            const auto paneFocusAdmission = observations[index].rendererAdmission;
                            const auto paneFocusDesktop = PrivateDesktop::current();
                            const auto paneFocusOriginalWindow = GetFocus();
                            const bool paneFocusCaptureAccepted = observations[index].matched && observations[index].privacyChecked &&
                                observations[index].privateWindows && prints[index] == S_OK && captures[index].inspectionUniqueColors >= 2 &&
                                captures[index].inspectionInkFraction > 0.002;
                            std::set<DWORD> paneFocusProcesses{GetCurrentProcessId()};
                            if (paneFocusAdmission)
                                for (size_t paneFocusWindow = 0; paneFocusWindow < paneFocusAdmission->count; ++paneFocusWindow)
                                    paneFocusProcesses.insert(paneFocusAdmission->windows[paneFocusWindow].process);
                            HRESULT paneFocusInputRead = E_PENDING;
                            const auto paneFocusInputCurrent = [&] {
                                if (!paneFocusDesktop || GetTickCount64() >= paneFocusDeadline) return false;
                                bool paneFocusOwnVisible = false;
                                paneFocusInputRead = paneFocusDesktop->visibleWindowsOnInputDesktop(paneFocusOwnVisible);
                                if (paneFocusInputRead != S_OK || paneFocusOwnVisible || GetTickCount64() >= paneFocusDeadline) return false;
                                struct PaneFocusInputHandle { HDESK value; ~PaneFocusInputHandle() { if (value) CloseDesktop(value); } } paneFocusInput{
                                    OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS | DESKTOP_ENUMERATE)};
                                if (!paneFocusInput.value) {
                                    const auto paneFocusInputError = GetLastError();
                                    paneFocusInputRead = HRESULT_FROM_WIN32(paneFocusInputError ? paneFocusInputError : ERROR_GEN_FAILURE); return false;
                                }
                                std::array<wchar_t, 512> paneFocusInputName;
                                paneFocusInputName.fill(L'\xffff');
                                constexpr DWORD paneFocusInputCapacity = 512 * sizeof(wchar_t);
                                DWORD paneFocusInputNameBytes = 0;
                                SetLastError(ERROR_SUCCESS);
                                const auto paneFocusNameRead = GetUserObjectInformationW(paneFocusInput.value, UOI_NAME,
                                    paneFocusInputName.data(), paneFocusInputCapacity, &paneFocusInputNameBytes);
                                const auto paneFocusNameError = GetLastError();
                                if (!paneFocusNameRead) {
                                    paneFocusInputRead = HRESULT_FROM_WIN32(paneFocusNameError ? paneFocusNameError : ERROR_GEN_FAILURE); return false;
                                }
                                if (paneFocusInputNameBytes < 2 * sizeof(wchar_t) || paneFocusInputNameBytes > paneFocusInputCapacity ||
                                    paneFocusInputNameBytes % sizeof(wchar_t) != 0) {
                                    paneFocusInputRead = HRESULT_FROM_WIN32(ERROR_INVALID_DATA); return false;
                                }
                                const auto paneFocusInputNameUnits = static_cast<size_t>(paneFocusInputNameBytes / sizeof(wchar_t));
                                const auto paneFocusInputTerminator = paneFocusInputName.begin() + paneFocusInputNameUnits - 1;
                                if (*paneFocusInputTerminator != L'\0' ||
                                    std::find(paneFocusInputName.begin(), paneFocusInputTerminator, L'\0') != paneFocusInputTerminator) {
                                    paneFocusInputRead = HRESULT_FROM_WIN32(ERROR_INVALID_DATA); return false;
                                }
                                if (std::wstring_view(paneFocusInputName.data(), paneFocusInputNameUnits - 1) !=
                                    std::wstring_view(paneFocusDesktop->originalInputName())) { paneFocusInputRead = E_ACCESSDENIED; return false; }
                                struct PaneFocusInputWindows {
                                    const std::set<DWORD>* processes;
                                    ULONGLONG deadline;
                                    unsigned visited = 0;
                                    bool visible = false, bounded = true;
                                } paneFocusInputWindows{&paneFocusProcesses, paneFocusDeadline};
                                SetLastError(ERROR_SUCCESS);
                                const auto paneFocusEnumerated = EnumDesktopWindows(paneFocusInput.value, [](HWND paneFocusCandidate, LPARAM paneFocusContext) -> BOOL {
                                    auto& paneFocusWindows = *reinterpret_cast<PaneFocusInputWindows*>(paneFocusContext);
                                    if (++paneFocusWindows.visited > 4096 || GetTickCount64() >= paneFocusWindows.deadline) {
                                        paneFocusWindows.bounded = false; SetLastError(ERROR_TIMEOUT); return FALSE;
                                    }
                                    DWORD paneFocusProcess = 0;
                                    if (!GetWindowThreadProcessId(paneFocusCandidate, &paneFocusProcess) || !paneFocusProcess) {
                                        paneFocusWindows.bounded = false; SetLastError(ERROR_INVALID_DATA); return FALSE;
                                    }
                                    if (paneFocusWindows.processes->contains(paneFocusProcess) && IsWindowVisible(paneFocusCandidate)) paneFocusWindows.visible = true;
                                    return TRUE;
                                }, reinterpret_cast<LPARAM>(&paneFocusInputWindows));
                                const auto paneFocusEnumError = GetLastError();
                                paneFocusInputRead = paneFocusEnumerated ? S_OK : HRESULT_FROM_WIN32(paneFocusEnumError ? paneFocusEnumError : ERROR_GEN_FAILURE);
                                return paneFocusInputRead == S_OK && paneFocusInputWindows.bounded && !paneFocusInputWindows.visible &&
                                    paneFocusDesktop->verifyIsolation() == S_OK && GetTickCount64() < paneFocusDeadline;
                            };
                            std::vector<Pidl> paneFocusHistory;
                            bool paneFocusHistoryCopied = true;
                            for (const auto& paneFocusEntry : paneApp->history_) {
                                Pidl paneFocusCopy(paneFocusEntry ? ILCloneFull(paneFocusEntry.get()) : nullptr);
                                paneFocusHistoryCopied = paneFocusHistoryCopied && paneFocusCopy != nullptr;
                                paneFocusHistory.push_back(std::move(paneFocusCopy));
                            }
                            const auto paneFocusTarget = [&] {
                                if (!paneFocusCaptureAccepted || !paneFocusDeadline || GetTickCount64() >= paneFocusDeadline || !caseUnchanged() ||
                                    !paneFocusHistoryCopied || paneApp->history_.size() != paneFocusHistory.size() ||
                                    paneApp->previewHost_.get() != paneFocusHost || !paneFocusHost ||
                                    paneApp->previewEpoch_ != paneFocusEpoch || !paneFocusEpoch || !paneFocusAdmission ||
                                    !paneFocusDesktop || paneFocusDesktop->verifyIsolation() != S_OK) return false;
                                for (size_t paneFocusEntry = 0; paneFocusEntry < paneFocusHistory.size(); ++paneFocusEntry) {
                                    const auto paneFocusSaved = paneFocusHistory[paneFocusEntry].get();
                                    const auto paneFocusCurrent = paneApp->history_[paneFocusEntry].get();
                                    if (!paneFocusSaved || !paneFocusCurrent || ILGetSize(paneFocusSaved) != ILGetSize(paneFocusCurrent) ||
                                        std::memcmp(paneFocusSaved, paneFocusCurrent, ILGetSize(paneFocusSaved)) != 0) return false;
                                }
                                return paneRenderersCurrent(paneApp->previewPane_, observations[index].bounds, paneFocusDesktop->name(), *paneFocusAdmission) &&
                                    paneFocusInputCurrent() && caseUnchanged() && paneApp->previewEpoch_ == paneFocusEpoch;
                            };
                            const auto paneFocusReadBasic = [](const std::filesystem::path& paneFocusPath, FILE_BASIC_INFO& paneFocusBasic) {
                                struct PaneFocusHandle { HANDLE value; ~PaneFocusHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } } paneFocusHandle{
                                    CreateFileW(paneFocusPath.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL, nullptr)};
                                return paneFocusHandle.value != INVALID_HANDLE_VALUE &&
                                    GetFileInformationByHandleEx(paneFocusHandle.value, FileBasicInfo, &paneFocusBasic, sizeof(paneFocusBasic));
                            };
                            std::array<FILE_BASIC_INFO, 4> paneFocusBasicBefore{};
                            bool paneFocusBasicRead = paneFocusTarget();
                            for (size_t paneFocusSource = 0; paneFocusBasicRead && paneFocusSource < paths.size(); ++paneFocusSource)
                                paneFocusBasicRead = paneFocusTarget() && paneFocusReadBasic(paths[paneFocusSource], paneFocusBasicBefore[paneFocusSource]);
                            const auto paneFocusSources = [&] {
                                if (!paneFocusTarget() || !paneFocusBasicRead || !originalPaneSourcesCurrent()) return false;
                                for (size_t paneFocusSource = 0; paneFocusSource < paths.size(); ++paneFocusSource) {
                                    FILE_BASIC_INFO paneFocusBasicAfter{};
                                    const auto& paneFocusOriginal = paneFocusBasicBefore[paneFocusSource];
                                    if (!paneFocusReadBasic(paths[paneFocusSource], paneFocusBasicAfter) ||
                                        paneFocusBasicAfter.FileAttributes != paneFocusOriginal.FileAttributes ||
                                        paneFocusBasicAfter.CreationTime.QuadPart != paneFocusOriginal.CreationTime.QuadPart ||
                                        paneFocusBasicAfter.LastWriteTime.QuadPart != paneFocusOriginal.LastWriteTime.QuadPart ||
                                        paneFocusBasicAfter.ChangeTime.QuadPart != paneFocusOriginal.ChangeTime.QuadPart) return false;
                                }
                                return paneFocusTarget();
                            };
                            const auto paneFocusSamePresentation = [](const SearchViewPresentation& paneFocusFirst, const SearchViewPresentation& paneFocusSecond) {
                                if (paneFocusFirst.mode != paneFocusSecond.mode || paneFocusFirst.iconSize != paneFocusSecond.iconSize ||
                                    paneFocusFirst.visibleColumns != paneFocusSecond.visibleColumns ||
                                    paneFocusFirst.groupBy.has_value() != paneFocusSecond.groupBy.has_value() ||
                                    paneFocusFirst.sort.has_value() != paneFocusSecond.sort.has_value()) return false;
                                if (paneFocusFirst.groupBy && (paneFocusFirst.groupBy->property != paneFocusSecond.groupBy->property ||
                                    paneFocusFirst.groupBy->direction != paneFocusSecond.groupBy->direction)) return false;
                                if (paneFocusFirst.sort) {
                                    if (paneFocusFirst.sort->size() != paneFocusSecond.sort->size()) return false;
                                    for (size_t paneFocusOrder = 0; paneFocusOrder < paneFocusFirst.sort->size(); ++paneFocusOrder)
                                        if ((*paneFocusFirst.sort)[paneFocusOrder].property != (*paneFocusSecond.sort)[paneFocusOrder].property ||
                                            (*paneFocusFirst.sort)[paneFocusOrder].direction != (*paneFocusSecond.sort)[paneFocusOrder].direction) return false;
                                }
                                return true;
                            };
                            struct PaneFocusState {
                                std::set<NativeFileIdentity> members, selection;
                                NativeFileIdentity focused;
                                DWORD focusFlags = 0, folderFlags = 0;
                                SearchViewPresentation presentation;
                            } paneFocusBefore;
                            const auto paneFocusReadState = [&](PaneFocusState& paneFocusState) -> HRESULT {
                                if (!paneFocusTarget()) return E_ABORT;
                                ComPtr<IShellItemArray> paneFocusAll, paneFocusSelected;
                                auto paneFocusRead = retainedFolder->Items(SVGIO_ALLVIEW, IID_PPV_ARGS(&paneFocusAll));
                                if (paneFocusRead == S_OK) paneFocusRead = paneFocusAll ? nativeArrayIdentities(paneFocusAll.Get(), paneFocusState.members) : E_UNEXPECTED;
                                if (paneFocusRead == S_OK) paneFocusRead = retainedFolder->GetSelection(FALSE, &paneFocusSelected);
                                if (paneFocusRead == S_OK) paneFocusRead = paneFocusSelected ? nativeArrayIdentities(paneFocusSelected.Get(), paneFocusState.selection) : E_UNEXPECTED;
                                int paneFocusItemIndex = -1;
                                if (paneFocusRead == S_OK) paneFocusRead = retainedFolder->GetFocusedItem(&paneFocusItemIndex);
                                ComPtr<IShellItem> paneFocusItem;
                                if (paneFocusRead == S_OK) paneFocusRead = paneFocusItemIndex >= 0 ? retainedFolder->GetItem(paneFocusItemIndex, IID_PPV_ARGS(&paneFocusItem)) : E_UNEXPECTED;
                                FILE_ID_INFO paneFocusItemId{};
                                if (paneFocusRead == S_OK) paneFocusRead = paneFocusItem ? nativeFileIdentity(itemName(paneFocusItem.Get(), SIGDN_FILESYSPATH), paneFocusItemId) : E_UNEXPECTED;
                                if (paneFocusRead == S_OK) paneFocusState.focused = identity(paneFocusItemId);
                                PITEMID_CHILD paneFocusRawChild = nullptr;
                                if (paneFocusRead == S_OK) paneFocusRead = retainedFolder->Item(paneFocusItemIndex, &paneFocusRawChild);
                                Pidl paneFocusChild(paneFocusRawChild);
                                if (paneFocusRead == S_OK) paneFocusRead = paneFocusChild ? retainedFolder->GetSelectionState(paneFocusChild.get(), &paneFocusState.focusFlags) : E_UNEXPECTED;
                                if (paneFocusRead == S_OK) paneFocusRead = retainedFolder->GetCurrentFolderFlags(&paneFocusState.folderFlags);
                                if (paneFocusRead == S_OK) paneFocusRead = captureSearchViewPresentation(retainedFolder.Get(), &paneFocusState.presentation);
                                return paneFocusRead == S_OK && !paneFocusTarget() ? E_ABORT : paneFocusRead;
                            };
                            struct PaneFocusKeyboard {
                                std::array<BYTE, 256> original{}, expected{};
                                std::wstring& error;
                                std::wstring originalError;
                                bool ready = false, restored = false;
                                explicit PaneFocusKeyboard(std::wstring& paneFocusError) : error(paneFocusError), originalError(paneFocusError) {
                                    ready = GetKeyboardState(original.data()) != FALSE;
                                    expected = original;
                                }
                                bool apply(bool paneFocusReverse) {
                                    if (!ready) return false;
                                    expected = original;
                                    for (const UINT paneFocusKey : {VK_CONTROL, VK_LCONTROL, VK_RCONTROL, VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_MENU, VK_LMENU, VK_RMENU})
                                        expected[paneFocusKey] &= 0x7F;
                                    if (paneFocusReverse) { expected[VK_SHIFT] |= 0x80; expected[VK_LSHIFT] |= 0x80; }
                                    return SetKeyboardState(expected.data()) && exact() && !(GetKeyState(VK_CONTROL) & 0x8000) &&
                                        !(GetKeyState(VK_MENU) & 0x8000) && ((GetKeyState(VK_SHIFT) & 0x8000) != 0) == paneFocusReverse &&
                                        ((GetKeyState(VK_LSHIFT) & 0x8000) != 0) == paneFocusReverse && !(GetKeyState(VK_RSHIFT) & 0x8000);
                                }
                                bool exact() const {
                                    std::array<BYTE, 256> paneFocusActual{};
                                    return ready && GetKeyboardState(paneFocusActual.data()) && paneFocusActual == expected;
                                }
                                bool restore() {
                                    std::array<BYTE, 256> paneFocusActual{};
                                    restored = ready && SetKeyboardState(original.data()) && GetKeyboardState(paneFocusActual.data()) && paneFocusActual == original;
                                    error = originalError;
                                    return restored && error == originalError;
                                }
                                ~PaneFocusKeyboard() { if (!restored) restore(); }
                            } paneFocusKeyboard(paneApp->lastError_);
                            const auto paneFocusRead = paneFocusReadState(paneFocusBefore);
                            const std::array paneFocusWindows{paneApp->window_, paneApp->previewPane_, paneApp->previewRender_, content, paneApp->previewGrip_};
                            std::array<std::pair<LONG_PTR, LONG_PTR>, 5> paneFocusStyles{};
                            for (size_t paneFocusWindow = 0; paneFocusWindow < paneFocusWindows.size(); ++paneFocusWindow)
                                paneFocusStyles[paneFocusWindow] = {GetWindowLongPtrW(paneFocusWindows[paneFocusWindow], GWL_STYLE), GetWindowLongPtrW(paneFocusWindows[paneFocusWindow], GWL_EXSTYLE)};
                            const auto paneFocusPreferences = std::tuple{paneApp->preferences_.previewPane, paneApp->preferences_.detailsPane,
                                paneApp->preferences_.navigationPane, paneApp->preferences_.previewWidth, paneApp->preferences_.searchWidth};
                            const auto paneFocusPreserved = [&] {
                                PaneFocusState paneFocusAfter;
                                if (!paneFocusTarget() || !paneFocusKeyboard.exact() || !paneFocusSources() || previewSourceReady() != S_OK ||
                                    paneFocusReadState(paneFocusAfter) != S_OK || paneFocusAfter.members != paneFocusBefore.members ||
                                    paneFocusAfter.selection != paneFocusBefore.selection || paneFocusAfter.focused != paneFocusBefore.focused ||
                                    paneFocusAfter.focusFlags != paneFocusBefore.focusFlags || paneFocusAfter.folderFlags != paneFocusBefore.folderFlags ||
                                    !paneFocusSamePresentation(paneFocusAfter.presentation, paneFocusBefore.presentation) ||
                                    std::tuple{paneApp->preferences_.previewPane, paneApp->preferences_.detailsPane, paneApp->preferences_.navigationPane,
                                        paneApp->preferences_.previewWidth, paneApp->preferences_.searchWidth} != paneFocusPreferences) return false;
                                for (size_t paneFocusWindow = 0; paneFocusWindow < paneFocusWindows.size(); ++paneFocusWindow)
                                    if (std::pair{GetWindowLongPtrW(paneFocusWindows[paneFocusWindow], GWL_STYLE), GetWindowLongPtrW(paneFocusWindows[paneFocusWindow], GWL_EXSTYLE)} != paneFocusStyles[paneFocusWindow]) return false;
                                return paneFocusTarget() && paneFocusKeyboard.exact();
                            };
                            const auto paneFocusStart = paneFocusTarget() ? paneApp->currentFocusRegion() : std::optional<FocusRegion>{};
                            bool paneFocusPassed = paneFocusCaptureAccepted &&
                                paneFocusRead == S_OK && paneFocusBefore.members == std::set<NativeFileIdentity>{identity(before[0]), identity(before[1]), identity(before[2]), identity(before[3])} &&
                                paneFocusBefore.selection == std::set<NativeFileIdentity>{identity(before[3])} &&
                                paneFocusBefore.focused == identity(before[3]) && paneFocusStart == FocusRegion::FolderView &&
                                paneFocusOriginalWindow && (paneFocusOriginalWindow == content || IsChild(content, paneFocusOriginalWindow)) &&
                                paneFocusKeyboard.ready && paneFocusKeyboard.apply(false) && paneFocusPreserved();
                            std::wstring paneFocusTrace;
                            unsigned paneFocusDispatches = 0;
                            HWND paneFocusLastNativeWindow = nullptr;
                            DWORD paneFocusLastNativeThread = 0;
                            const std::array<FocusRegion, 4> paneFocusExpected{FocusRegion::Preview, FocusRegion::Sorting, FocusRegion::Preview, FocusRegion::FolderView};
                            for (size_t paneFocusAction = 0; paneFocusAction < paneFocusExpected.size(); ++paneFocusAction) {
                                const bool paneFocusReverse = paneFocusAction >= 2;
                                if (!paneFocusPassed || !paneFocusPreserved() ||
                                    (paneFocusAction == 2 && (!paneFocusKeyboard.apply(true) || !paneFocusPreserved()))) { paneFocusPassed = false; break; }
                                const auto paneFocusOldSequence = paneFocusHost->status().focus.requestedSequence;
                                const auto paneFocusStarted = GetTickCount64();
                                const auto paneFocusCommand = paneApp->execute(paneFocusReverse ? FocusPrevious : FocusNext);
                                ++paneFocusDispatches;
                                bool paneFocusCompleted = false, paneFocusGuiExact = false;
                                HRESULT paneFocusGuiRead = E_PENDING;
                                GUITHREADINFO paneFocusGui{sizeof(paneFocusGui)};
                                PreviewFocusStatus paneFocusReceipt = paneFocusTarget() ? paneFocusHost->status().focus : PreviewFocusStatus{};
                                const bool paneFocusIsPreview = paneFocusExpected[paneFocusAction] == FocusRegion::Preview;
                                if (paneFocusIsPreview && (paneFocusCommand == S_OK || paneFocusCommand == S_FALSE) && paneFocusPreserved() &&
                                    paneFocusReceipt.requestedSequence == paneFocusOldSequence + 1 && paneFocusReceipt.requestedEpoch == paneFocusEpoch &&
                                    paneFocusReceipt.requestedReverse == paneFocusReverse) {
                                    const auto paneFocusPoll = [&] {
                                        if (!paneFocusPreserved()) return true;
                                        paneFocusReceipt = paneFocusHost->status().focus;
                                        return !paneFocusReceipt.pending && paneFocusReceipt.completedSequence == paneFocusOldSequence + 1 &&
                                            paneFocusReceipt.completedEpoch == paneFocusEpoch;
                                    };
                                    const auto paneFocusNow = GetTickCount64();
                                    if (paneFocusNow < paneFocusDeadline)
                                        paneFocusCompleted = pumpUntil(paneFocusPoll, static_cast<DWORD>(paneFocusDeadline - paneFocusNow));
                                    paneFocusReceipt = paneFocusTarget() ? paneFocusHost->status().focus : PreviewFocusStatus{};
                                    if (paneFocusCompleted && paneFocusPreserved() && !paneFocusReceipt.pending &&
                                        paneFocusReceipt.requestedSequence == paneFocusOldSequence + 1 && paneFocusReceipt.completedSequence == paneFocusOldSequence + 1 &&
                                        paneFocusReceipt.requestedEpoch == paneFocusEpoch && paneFocusReceipt.completedEpoch == paneFocusEpoch &&
                                        paneFocusReceipt.requestedReverse == paneFocusReverse && paneFocusReceipt.completedReverse == paneFocusReverse &&
                                        paneFocusReceipt.workerShiftRead && paneFocusReceipt.workerShiftDown == paneFocusReverse &&
                                        paneFocusReceipt.setAttempted && paneFocusReceipt.queryAttempted && paneFocusReceipt.setResult == S_OK &&
                                        paneFocusReceipt.queryResult == S_OK && paneFocusReceipt.result == S_OK && paneFocusReceipt.queriedWindow) {
                                        const auto paneFocusStatus = paneFocusHost->status();
                                        const auto paneFocusFound = std::find_if(paneFocusAdmission->windows.begin(), paneFocusAdmission->windows.begin() + paneFocusAdmission->count,
                                            [&](const auto& paneFocusNative) { return paneFocusNative.window == paneFocusReceipt.queriedWindow; });
                                        PrivateWindowSnapshot paneFocusFresh;
                                        if (paneFocusStatus.ready && paneFocusStatus.activeEpoch == paneFocusEpoch && paneFocusStatus.sessionWindow &&
                                            (paneFocusReceipt.queriedWindow == paneFocusStatus.sessionWindow || IsChild(paneFocusStatus.sessionWindow, paneFocusReceipt.queriedWindow)) &&
                                            paneFocusFound != paneFocusAdmission->windows.begin() + paneFocusAdmission->count &&
                                            paneWindowSnapshot(paneFocusReceipt.queriedWindow, paneFocusFresh) == S_OK && samePaneWindow(*paneFocusFound, paneFocusFresh) && paneFocusFresh.thread) {
                                            SetLastError(ERROR_SUCCESS);
                                            const auto paneFocusGuiResult = GetGUIThreadInfo(paneFocusFresh.thread, &paneFocusGui);
                                            const auto paneFocusGuiError = GetLastError();
                                            paneFocusGuiRead = paneFocusGuiResult ? S_OK : HRESULT_FROM_WIN32(paneFocusGuiError ? paneFocusGuiError : ERROR_GEN_FAILURE);
                                            paneFocusGuiExact = paneFocusGuiRead == S_OK && paneFocusGui.hwndFocus == paneFocusReceipt.queriedWindow;
                                            if (paneFocusGuiExact) { paneFocusLastNativeWindow = paneFocusReceipt.queriedWindow; paneFocusLastNativeThread = paneFocusFresh.thread; }
                                        }
                                    }
                                } else if (!paneFocusIsPreview && paneFocusCommand == S_OK && paneFocusPreserved() && paneFocusLastNativeThread) {
                                    SetLastError(ERROR_SUCCESS);
                                    const auto paneFocusGuiResult = GetGUIThreadInfo(paneFocusLastNativeThread, &paneFocusGui);
                                    const auto paneFocusGuiError = GetLastError();
                                    paneFocusGuiRead = paneFocusGuiResult ? S_OK : HRESULT_FROM_WIN32(paneFocusGuiError ? paneFocusGuiError : ERROR_GEN_FAILURE);
                                    const auto paneFocusStatus = paneFocusHost->status();
                                    paneFocusGuiExact = paneFocusGuiRead == S_OK && paneFocusStatus.sessionWindow &&
                                        paneFocusGui.hwndFocus != paneFocusStatus.sessionWindow && !IsChild(paneFocusStatus.sessionWindow, paneFocusGui.hwndFocus);
                                    paneFocusCompleted = paneFocusReceipt.requestedSequence == paneFocusOldSequence;
                                }
                                const auto paneFocusRegion = paneFocusTarget() ? paneApp->currentFocusRegion() : std::optional<FocusRegion>{};
                                const bool paneFocusNativePreserved = paneFocusPreserved();
                                if (paneFocusGuiExact && paneFocusNativePreserved && paneFocusLastNativeThread) {
                                    const auto paneFocusStatus = paneFocusHost->status();
                                    GUITHREADINFO paneFocusFinalGui{sizeof(paneFocusFinalGui)};
                                    const bool paneFocusFinalRead = GetGUIThreadInfo(paneFocusLastNativeThread, &paneFocusFinalGui) != FALSE;
                                    paneFocusGuiExact = paneFocusFinalRead && paneFocusStatus.ready && paneFocusStatus.activeEpoch == paneFocusEpoch &&
                                        paneFocusStatus.sessionWindow && paneFocusStatus.focus.requestedSequence == paneFocusReceipt.requestedSequence &&
                                        paneFocusStatus.focus.completedSequence == paneFocusReceipt.completedSequence &&
                                        (paneFocusIsPreview ? paneFocusFinalGui.hwndFocus == paneFocusReceipt.queriedWindow :
                                            (paneFocusFinalGui.hwndFocus != paneFocusStatus.sessionWindow && !IsChild(paneFocusStatus.sessionWindow, paneFocusFinalGui.hwndFocus)));
                                    paneFocusGui.hwndFocus = paneFocusFinalGui.hwndFocus;
                                }
                                paneFocusPassed = paneFocusCompleted && paneFocusGuiExact && paneFocusRegion == paneFocusExpected[paneFocusAction] &&
                                    paneFocusNativePreserved && (paneFocusAction != 3 || GetFocus() == paneFocusOriginalWindow) && paneFocusTarget();
                                paneFocusTrace += L"; action=" + std::to_wstring(paneFocusAction) + L" dispatch=" + hresultMessage(paneFocusCommand) +
                                    L" elapsed=" + std::to_wstring(GetTickCount64() - paneFocusStarted) + L" seq requested/completed=" +
                                    std::to_wstring(paneFocusReceipt.requestedSequence) + L"/" + std::to_wstring(paneFocusReceipt.completedSequence) +
                                    L" epochs=" + std::to_wstring(paneFocusReceipt.requestedEpoch) + L"/" + std::to_wstring(paneFocusReceipt.completedEpoch) +
                                    L" reverse(requested/completed)/workerShift(read/down)=" + std::to_wstring(paneFocusReceipt.requestedReverse) + L"/" +
                                    std::to_wstring(paneFocusReceipt.completedReverse) + L"/" + std::to_wstring(paneFocusReceipt.workerShiftRead) + L"/" + std::to_wstring(paneFocusReceipt.workerShiftDown) +
                                    L" native Set/Query=" + hresultMessage(paneFocusReceipt.setResult) + L"/" + hresultMessage(paneFocusReceipt.queryResult) +
                                    L" result/pending=" + hresultMessage(paneFocusReceipt.result) + L"/" + std::to_wstring(paneFocusReceipt.pending) +
                                    L" native/GUI HWND=" + std::to_wstring(reinterpret_cast<UINT_PTR>(paneFocusReceipt.queriedWindow)) + L"/" +
                                    std::to_wstring(reinterpret_cast<UINT_PTR>(paneFocusGui.hwndFocus)) + L" GUI=" + hresultMessage(paneFocusGuiRead) +
                                    L" region=" + std::to_wstring(paneFocusRegion ? static_cast<int>(*paneFocusRegion) : -1) +
                                    L" completed/preserved/256=" + std::to_wstring(paneFocusCompleted) + L"/" + std::to_wstring(paneFocusNativePreserved) + L"/" + std::to_wstring(paneFocusKeyboard.exact());
                                if (!paneFocusPassed) break;
                            }
                            const bool paneFocusFinalPreserved = paneFocusPreserved();
                            const bool paneFocusKeysRestored = paneFocusKeyboard.restore();
                            check("native_preview_source_b_four_production_focus_transitions", paneFocusPassed && paneFocusDispatches == 4 &&
                                paneFocusFinalPreserved && paneFocusKeysRestored,
                                L"original observation deadline=" + std::to_wstring(paneFocusDeadline) + L"; current ticks=" + std::to_wstring(GetTickCount64()) +
                                L"; initial region=" + std::to_wstring(paneFocusStart ? static_cast<int>(*paneFocusStart) : -1) +
                                L"; native state=" + hresultMessage(paneFocusRead) + L"; actual dispatches=" + std::to_wstring(paneFocusDispatches) +
                                L"; last native focus HWND=" + std::to_wstring(reinterpret_cast<UINT_PTR>(paneFocusLastNativeWindow)) +
                                L"; current owned/renderer input-window read=" + hresultMessage(paneFocusInputRead) +
                                L"; original256/error restored=" + std::to_wstring(paneFocusKeysRestored) + paneFocusTrace);
                        }
                        if (preview && index == 0 && selected && nativeHandler && observations[index].privacyChecked && observations[index].privateWindows) {
                            // The original semantic/pixel readback above is
                            // complete before this diagnostic touches a fresh
                            // handler. It cannot rescue a blank native pane.
                            ComPtr<IShellItemArray> actualSelection;
                            ComPtr<IShellItem> actualSelected;
                            DWORD selectedCount = 0;
                            auto selectedRead = retainedFolder->GetSelection(FALSE, &actualSelection);
                            if (SUCCEEDED(selectedRead)) selectedRead = actualSelection ? actualSelection->GetCount(&selectedCount) : E_UNEXPECTED;
                            if (SUCCEEDED(selectedRead) && selectedCount != 1) selectedRead = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                            if (SUCCEEDED(selectedRead)) selectedRead = actualSelection->GetItemAt(0, &actualSelected);
                            FILE_ID_INFO actualIdentity{};
                            if (SUCCEEDED(selectedRead)) {
                                const auto actualPath = itemName(actualSelected.Get(), SIGDN_FILESYSPATH);
                                selectedRead = actualPath.empty() ? E_UNEXPECTED : nativeFileIdentity(actualPath, actualIdentity);
                            }
                            if (SUCCEEDED(selectedRead) && identity(actualIdentity) != identity(before[2])) selectedRead = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                            // The native selection data object exposes the item
                            // handed to Preview. Read it after the original
                            // render observation; this cannot supply a handler
                            // or rescue its blank surface.
                            const auto handoffGeneration = paneApp->namespaceGeneration_;
                            const auto handoffCurrent = [&] {
                                if (paneApp->closing_ || paneApp->navigating_ || paneApp->view_.Get() != retainedView.Get() ||
                                    paneApp->folderView_.Get() != retainedFolder.Get() || paneApp->namespaceGeneration_ != handoffGeneration)
                                    return false;
                                return exactSelected(retainedFolder.Get(), file) && focusedItem(retainedFolder.Get(), file).exact &&
                                    location && paneApp->currentPidl_ && ILIsEqual(location.get(), paneApp->currentPidl_.get()) &&
                                    paneApp->history_.size() == historyCount && paneApp->historyIndex_ == historyIndex &&
                                    paneApp->navigationCount_ == navigation && paneApp->activeQuery_ == query &&
                                    paneApp->searchInteractionRevision_ == revision && paneApp->namespaceGeneration_ == handoffGeneration &&
                                    paneApp->view_.Get() == retainedView.Get() && paneApp->folderView_.Get() == retainedFolder.Get() &&
                                    !paneApp->closing_ && !paneApp->navigating_;
                            };
                            ComPtr<IDataObject> nativeData;
                            ComPtr<IPreviewItem> previewTarget;
                            ComPtr<IShellItem> relatedItem;
                            HRESULT dataRead = E_PENDING, previewRead = E_PENDING, relatedRead = E_PENDING;
                            HRESULT relatedPidlRead = E_PENDING, selectedPidlRead = E_PENDING, itemPidlRead = E_PENDING;
                            HRESULT targetPathRead = E_PENDING, targetIdentityRead = E_PENDING;
                            bool targetPidlMatches = false, targetFileMatches = false;
                            const bool handoffBefore = SUCCEEDED(selectedRead) && handoffCurrent();
                            if (handoffBefore) dataRead = retainedView->GetItemObject(SVGIO_SELECTION, IID_PPV_ARGS(&nativeData));
                            if (SUCCEEDED(dataRead) && nativeData && handoffCurrent()) previewRead = nativeData.As(&previewTarget);
                            if (SUCCEEDED(previewRead) && previewTarget && handoffCurrent()) relatedRead = previewTarget->GetItem(&relatedItem);
                            PIDLIST_ABSOLUTE rawRelated = nullptr, rawSelected = nullptr, rawItem = nullptr;
                            if (SUCCEEDED(relatedRead) && relatedItem && handoffCurrent()) relatedPidlRead = previewTarget->GetItemIDList(&rawRelated);
                            Pidl relatedPidl(rawRelated);
                            if (SUCCEEDED(relatedPidlRead) && relatedPidl && handoffCurrent()) selectedPidlRead = SHGetIDListFromObject(actualSelected.Get(), &rawSelected);
                            Pidl selectedPidl(rawSelected);
                            if (SUCCEEDED(selectedPidlRead) && selectedPidl && handoffCurrent()) itemPidlRead = SHGetIDListFromObject(relatedItem.Get(), &rawItem);
                            Pidl itemPidl(rawItem);
                            if (SUCCEEDED(itemPidlRead) && itemPidl && handoffCurrent())
                                targetPidlMatches = ILIsEqual(relatedPidl.get(), selectedPidl.get()) && ILIsEqual(itemPidl.get(), selectedPidl.get());
                            PWSTR targetPath = nullptr;
                            if (SUCCEEDED(relatedRead) && relatedItem && handoffCurrent()) targetPathRead = relatedItem->GetDisplayName(SIGDN_FILESYSPATH, &targetPath);
                            FILE_ID_INFO targetIdentity{};
                            if (SUCCEEDED(targetPathRead) && targetPath && handoffCurrent()) targetIdentityRead = nativeFileIdentity(targetPath, targetIdentity);
                            CoTaskMemFree(targetPath);
                            targetFileMatches = SUCCEEDED(targetIdentityRead) && identity(targetIdentity) == identity(before[2]);
                            const bool handoffAfter = handoffBefore && handoffCurrent();
                            previewTargetDiagnostic = L"native Preview data-object target HRESULT data/QI/item/relatedPIDL/selectedPIDL/itemPIDL/path/FileID=" +
                                hresultMessage(dataRead) + L"/" + hresultMessage(previewRead) + L"/" + hresultMessage(relatedRead) + L"/" +
                                hresultMessage(relatedPidlRead) + L"/" + hresultMessage(selectedPidlRead) + L"/" + hresultMessage(itemPidlRead) + L"/" +
                                hresultMessage(targetPathRead) + L"/" + hresultMessage(targetIdentityRead) +
                                L"; actual interfaces data/preview/item=" + std::to_wstring(nativeData != nullptr) + L"/" +
                                std::to_wstring(previewTarget != nullptr) + L"/" + std::to_wstring(relatedItem != nullptr) +
                                L"; original target PIDL/FileID matches=" + std::to_wstring(targetPidlMatches) + L"/" + std::to_wstring(targetFileMatches) +
                                L"; original selection/view/generation/history before/after=" + std::to_wstring(handoffBefore) + L"/" + std::to_wstring(handoffAfter);
                            std::cerr << "headless-preview-data-object-after-original-pane-capture " << jsonString(previewTargetDiagnostic) << std::endl;
                            if (!handoffAfter) selectedRead = HRESULT_FROM_WIN32(ERROR_RETRY);
                            ComPtr<IQueryAssociations> selectedAssociation;
                            std::array<wchar_t, 128> selectedHandler{};
                            DWORD selectedHandlerCharacters = static_cast<DWORD>(selectedHandler.size());
                            GUID selectedClass{};
                            if (SUCCEEDED(selectedRead)) selectedRead = actualSelected->BindToHandler(nullptr, BHID_AssociationArray, IID_PPV_ARGS(&selectedAssociation));
                            if (SUCCEEDED(selectedRead)) selectedRead = selectedAssociation->GetString(ASSOCF_NOTRUNCATE, ASSOCSTR_SHELLEXTENSION,
                                L"{8895b1c6-b41f-4c1c-a562-0d564250836f}", selectedHandler.data(), &selectedHandlerCharacters);
                            if (SUCCEEDED(selectedRead)) selectedRead = CLSIDFromString(selectedHandler.data(), &selectedClass);
                            if (SUCCEEDED(selectedRead) && !IsEqualGUID(selectedClass, handler)) selectedRead = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                            previewActivationDiagnostic = SUCCEEDED(selectedRead) ?
                                nativePreviewActivationDiagnostic(selectedClass, actualSelected.Get(), before[2], originalPreviewBasic, originalBytes[2]) :
                                L"actual original Preview selection/FileID/association guard=" + hresultMessage(selectedRead) + L"; factory calls=0";
                            std::cerr << "headless-preview-no-ui-after-original-pane-capture " << jsonString(previewActivationDiagnostic) << std::endl;
                        }
                        focusedItems[index] = focusedItem(retainedFolder.Get(), file);
                        ComPtr<IUnknown> afterSite;
                        const bool siteStable = located && SUCCEEDED(located->GetSite(IID_PPV_ARGS(&afterSite))) && afterSite.Get() == viewSite.Get();
                        const bool stable = !paneApp->closing_ && !paneApp->navigating_ && paneApp->view_.Get() == retainedView.Get() &&
                            paneApp->folderView_.Get() == retainedFolder.Get() && paneApp->navigationCount_ == navigation &&
                            paneApp->history_.size() == historyCount && paneApp->historyIndex_ == historyIndex && location &&
                            paneApp->activeQuery_ == query && paneApp->searchInteractionRevision_ == revision &&
                            paneApp->currentPidl_ && ILIsEqual(location.get(), paneApp->currentPidl_.get()) && siteStable &&
                            exactSelected(retainedFolder.Get(), file) && focusedItems[index].exact;
                        previewSourcesPreserved[index] = !preview || previewSourceReady() == S_OK;
                        retainedStates[index] = stable && previewSourcesPreserved[index];
                        passed = selected && observations[index].matched && SUCCEEDED(prints[index]) && retainedStates[index] &&
                            captures[index].inspectionUniqueColors >= 2 && captures[index].inspectionInkFraction > 0.002 && passed;
                    }
                    const bool pixelsChanged = EqualRect(&captures[0].pixelInspectionBounds, &captures[1].pixelInspectionBounds) &&
                        captures[0].inspectionPixelHash != captures[1].inspectionPixelHash;
                    const auto appHostingRequests = paneHostingRequestsJson(paneApp->headlessPaneHostingRequests_);
                    check(preview ? "native_preview_app_host_requests_bounded" : "native_details_app_host_requests_bounded",
                        !paneApp->headlessPaneHostingRequests_.overflow, appHostingRequests);
                    std::wstring diagnostic = L"created=" + hresultMessage(created) + L"; actual ready/presentation/case ready=" +
                        std::to_wstring(actualReady) + L"/" + std::to_wstring(presentation.ready) + L"/" + std::to_wstring(caseReady) +
                        L"; native view activation=" + hresultMessage(activation) + L"; view window=" + hresultMessage(windowRead) + L"; original App pane site=" +
                        std::to_wstring(originalSite) + L"/" + hresultMessage(siteRead) + L"; pane pixels changed=" + std::to_wstring(pixelsChanged);
                    diagnostic += L"; original App incoming native hosting requests=" + appHostingRequests;
                    {
                        // Read the public browser controls only after the original
                        // semantic/pixel observation. This cannot repair a pane.
                        const size_t footerSelection = selections[1] ? 1 : 0;
                        const size_t footerFile = preview ? footerSelection + 2 : footerSelection;
                        const auto footerEpoch = paneApp->previewEpoch_;
                        const auto footerAcceptedEpoch = preview ? observedPreviewEpochs[footerSelection] : 0;
                        const auto footerGeneration = paneApp->namespaceGeneration_;
                        const auto footerRevision = paneApp->commandSourceRevision_;
                        std::vector<Pidl> footerHistory;
                        bool footerHistoryCopied = true;
                        for (const auto& entry : paneApp->history_) {
                            Pidl copy(entry ? ILCloneFull(entry.get()) : nullptr);
                            footerHistoryCopied = footerHistoryCopied && copy != nullptr;
                            footerHistory.push_back(std::move(copy));
                        }
                        const auto footerCurrent = [&] {
                            if (!caseUnchanged() || !footerHistoryCopied ||
                                paneApp->namespaceGeneration_ != footerGeneration ||
                                paneApp->commandSourceRevision_ != footerRevision ||
                                paneApp->history_.size() != footerHistory.size() ||
                                (preview && (paneApp->previewEpoch_ != footerEpoch ||
                                 (footerAcceptedEpoch && footerEpoch != footerAcceptedEpoch)))) return false;
                            for (size_t entry = 0; entry < footerHistory.size(); ++entry) {
                                const auto retained = footerHistory[entry].get();
                                const auto current = paneApp->history_[entry].get();
                                if (!retained || !current || ILGetSize(retained) != ILGetSize(current) ||
                                    std::memcmp(retained, current, ILGetSize(retained)) != 0) return false;
                            }
                            return caseUnchanged() && paneApp->namespaceGeneration_ == footerGeneration &&
                                paneApp->commandSourceRevision_ == footerRevision &&
                                (!preview || paneApp->previewEpoch_ == footerEpoch);
                        };
                        const auto footerSourceCurrent = [&] {
                            const auto footerDesktop = PrivateDesktop::current();
                            return selections[footerSelection] && footerCurrent() &&
                                exactSelected(retainedFolder.Get(), footerFile) &&
                                focusedItem(retainedFolder.Get(), footerFile).exact && originalPaneSourcesCurrent() &&
                                footerDesktop && SUCCEEDED(footerDesktop->verifyIsolation()) &&
                                (!footerAcceptedEpoch || paneApp->previewSourceCurrent(true)) && footerCurrent();
                        };
                        ComPtr<IShellBrowser> footerBrowser;
                        HWND footerFrame = nullptr, footerStatus = nullptr, footerContent = nullptr;
                        HRESULT footerServiceRead = E_PENDING, footerFrameRead = E_PENDING;
                        HRESULT footerStatusRead = E_PENDING, footerContentRead = E_PENDING;
                        const bool footerBefore = footerSourceCurrent();
                        bool footerCallsCurrent = footerBefore;
                        if (footerCallsCurrent) {
                            footerServiceRead = IUnknown_QueryService(retainedView.Get(), SID_STopLevelBrowser, IID_PPV_ARGS(&footerBrowser));
                            footerCallsCurrent = footerCurrent();
                        }
                        if (footerCallsCurrent && footerServiceRead == S_OK && footerBrowser) {
                            footerFrameRead = footerBrowser->GetWindow(&footerFrame);
                            footerCallsCurrent = footerCurrent();
                        }
                        if (footerCallsCurrent && footerServiceRead == S_OK && footerBrowser) {
                            footerStatusRead = footerBrowser->GetControlWindow(FCW_STATUS, &footerStatus);
                            footerCallsCurrent = footerCurrent();
                        }
                        if (footerCallsCurrent) {
                            footerContentRead = retainedView->GetWindow(&footerContent);
                            footerCallsCurrent = footerCurrent();
                        }
                        const auto rectangle = [](const RECT& value) {
                            return std::to_wstring(value.left) + L"," + std::to_wstring(value.top) + L"," +
                                std::to_wstring(value.right) + L"," + std::to_wstring(value.bottom);
                        };
                        const auto footerGeometry = [&](const wchar_t* role, HWND target, HRESULT handleRead) {
                            DWORD targetProcess = 0;
                            const auto targetThread = target ? GetWindowThreadProcessId(target, &targetProcess) : 0;
                            const bool root = target && target == paneApp->window_;
                            const bool child = target && IsChild(paneApp->window_, target);
                            const bool owned = targetThread == GetCurrentThreadId() && targetProcess == GetCurrentProcessId() && (root || child);
                            RECT screen{}, client{};
                            DWORD boundsError = ERROR_SUCCESS;
                            HRESULT boundsRead = E_PENDING, clientRead = E_PENDING;
                            if (handleRead == S_OK && owned && footerCallsCurrent && footerCurrent()) {
                                if (GetWindowRect(target, &screen)) {
                                    boundsRead = S_OK;
                                    clientRead = mapUiRect(nullptr, paneApp->window_, screen, &client);
                                } else {
                                    boundsError = GetLastError();
                                    boundsRead = HRESULT_FROM_WIN32(boundsError ? boundsError : ERROR_GEN_FAILURE);
                                }
                                footerCallsCurrent = footerCurrent();
                            }
                            return L"; public " + std::wstring(role) + L" HWND/PID/TID/root/child/owned=" +
                                std::to_wstring(reinterpret_cast<UINT_PTR>(target)) + L"/" + std::to_wstring(targetProcess) +
                                L"/" + std::to_wstring(targetThread) + L"/" + std::to_wstring(root) + L"/" +
                                std::to_wstring(child) + L"/" + std::to_wstring(owned) +
                                L"; bounds HRESULT/nativeError/clientMap=" + hresultMessage(boundsRead) + L"/" +
                                std::to_wstring(boundsError) + L"/" + hresultMessage(clientRead) +
                                L"; normalized physical screen/App-client=" + rectangle(screen) + L"/" + rectangle(client);
                        };
                        std::wstring footerDiagnostic = L"public footer geometry raw service/frame/status/view HRESULTs=" +
                            hresultMessage(footerServiceRead) + L"/" + hresultMessage(footerFrameRead) + L"/" +
                            hresultMessage(footerStatusRead) + L"/" + hresultMessage(footerContentRead) +
                            L"; status NULL=" + std::to_wstring(footerStatus == nullptr) +
                            L"; captured Preview epoch/accepted=" + std::to_wstring(footerEpoch) + L"/" + std::to_wstring(footerAcceptedEpoch);
                        footerDiagnostic += footerGeometry(L"browser", footerFrame, footerFrameRead);
                        footerDiagnostic += footerGeometry(L"status", footerStatus, footerStatusRead);
                        footerDiagnostic += footerGeometry(L"view", footerContent, footerContentRead);
                        footerBrowser.Reset(); // Include native Release reentry in the final source fence.
                        footerCallsCurrent = footerCallsCurrent && footerCurrent();
                        const bool footerAfter = footerCallsCurrent && footerSourceCurrent();
                        footerDiagnostic += L"; exact source before/calls/after=" + std::to_wstring(footerBefore) + L"/" +
                            std::to_wstring(footerCallsCurrent) + L"/" + std::to_wstring(footerAfter) +
                            L"; read-only diagnostic; native layout mutations=0";
                        diagnostic += L"; " + footerDiagnostic;
                        std::cerr << "headless-native-footer-after-original-pane-observation " << jsonString(footerDiagnostic) << std::endl;
                        const auto renderedGeometry = readNativePaneGeometry(retainedView.Get(), paneApp->window_, paneApp->previewPane_,
                            paneApp->previewSplitter_, preview, observations[footerSelection].observationDeadline, footerCurrent, paneApp->previewGrip_);
                        check(preview ? "native_preview_rendered_file_pane_partitions_content_and_leaves_full_footer" :
                            "native_details_rendered_file_view_keeps_full_native_footer",
                            footerAfter && renderedGeometry.read == S_OK && renderedGeometry.footerFull && renderedGeometry.partition &&
                            footerSourceCurrent(), paneGeometryFacts(renderedGeometry));
                    }
                    if (preview) diagnostic += L"; " + previewTargetDiagnostic + L"; " + previewActivationDiagnostic;
                    for (size_t index = 0; index < observations.size(); ++index) diagnostic += L"; " + std::to_wstring(index) + L": " +
                        L"exact selected/retained state=" + std::to_wstring(selections[index]) + L"/" + std::to_wstring(retainedStates[index]) +
                        L"; native focused item HRESULT/index/exactFileID=" + hresultMessage(focusedItems[index].read) + L"/" +
                        std::to_wstring(focusedItems[index].index) + L"/" + std::to_wstring(focusedItems[index].exact) +
                        L"; observation HRESULT=" + hresultMessage(observations[index].read) + L"; " + observations[index].detail +
                        L"; print=" + hresultMessage(prints[index]) + L"; colors=" +
                        std::to_wstring(captures[index].inspectionUniqueColors) + L"; hash=" + std::to_wstring(captures[index].inspectionPixelHash) +
                        L"; source HWND=" + std::to_wstring(captures[index].printSourceWindow) + L"; native frame HWND=" +
                        std::to_wstring(captures[index].printTargetWindow) + L"; native flags=" + std::to_wstring(captures[index].printWindowFlags) +
                        L"; source image=" + captures[index].nativeClientCropSourceImage.wstring();
                    const bool isolationFailed = std::any_of(observations.begin(), observations.end(), [](const auto& observed) {
                        return observed.privacyChecked && !observed.privateWindows;
                    });
                    paneIsolationPreserved = !isolationFailed && paneIsolationPreserved;
                    if (!passed || !pixelsChanged) {
                        std::cerr << "headless native " << (preview ? "Preview" : "Details") <<
                            " pane before independent reference: " << jsonString(diagnostic) << std::endl;
                    }
                    if ((!passed || !pixelsChanged) && !isolationFailed) {
                        ComPtr<IShellItem> folder;
                        auto referenceRead = SHCreateItemFromParsingName(paneRoot.c_str(), nullptr, IID_PPV_ARGS(&folder));
                        NativePaneReference reference;
                        if (SUCCEEDED(referenceRead)) referenceRead = reference.create(instance_, folder.Get(), preview);
                        PaneObservation observed;
                        HRESULT referenceActivation = E_PENDING;
                        FocusedPaneItem referenceFocused;
                        if (SUCCEEDED(referenceRead)) {
                            PrivatePresentation referencePresentation(reference.owner, true);
                            HWND referenceContent = nullptr;
                            referenceRead = reference.view->GetWindow(&referenceContent);
                            if (SUCCEEDED(referenceRead) && referencePresentation.ready)
                                referenceActivation = reference.view->UIActivate(SVUIA_ACTIVATE_FOCUS);
                            if (SUCCEEDED(referenceRead) && referencePresentation.ready && SUCCEEDED(referenceActivation) &&
                                selectOnce(reference.view.Get(), reference.folderView.Get(), preview ? 2 : 0))
                                observed = observeNativePane(reference.owner, referenceContent,
                                    preview ? std::vector<std::wstring>{previewTokens[0]} : detailsTokens[0], L"", preview);
                            referenceFocused = focusedItem(reference.folderView.Get(), preview ? 2 : 0);
                        }
                        diagnostic += L"; independent native reference=" + hresultMessage(referenceRead) + L"/semantic=" +
                            std::to_wstring(observed.matched) + L"/" + observed.detail + L"; reference activation=" + hresultMessage(referenceActivation) +
                            L"; reference focused item HRESULT/index/exactFileID=" + hresultMessage(referenceFocused.read) + L"/" +
                            std::to_wstring(referenceFocused.index) + L"/" + std::to_wstring(referenceFocused.exact);
                        if (reference.site) {
                            const auto referenceHostingRequests = paneHostingRequestsJson(reference.site->requests);
                            diagnostic += L"; independent reference incoming native hosting requests=" + referenceHostingRequests;
                            check(preview ? "native_preview_reference_host_requests_bounded" : "native_details_reference_host_requests_bounded",
                                !reference.site->requests.overflow, referenceHostingRequests);
                        }
                    }
                    check(preview ? "native_preview_rtf_content_and_pixels_follow_exact_selected_file" :
                        "native_details_property_content_and_pixels_follow_exact_selected_file", passed && pixelsChanged, diagnostic);
                    return passed && pixelsChanged;
                };
                const bool detailsRendered = verifyCase(false);
                if (nativeHandler && paneIsolationPreserved) {
                    // Creator-only calibration precedes every foreign UIA or
                    // pixel observation. Both directly owned controls exit
                    // before the separate real Preview startup below.
                    const auto currentPrivate = PrivateDesktop::current();
                    paneCalibration = currentPrivate ? runPrivateDesktopMessageControls(*currentPrivate, &paneMessageControls) : E_ACCESSDENIED;
                    std::wstring controlFacts = L"creator calibration=" + hresultMessage(paneCalibration) +
                        L"; PID/TID=" + std::to_wstring(paneMessageControls.creatorProcess) + L"/" + std::to_wstring(paneMessageControls.creatorThread) +
                        L"; calibrated/overflow=" + std::to_wstring(paneMessageControls.calibrated) + L"/" + std::to_wstring(paneMessageControls.overflow);
                    for (size_t controlIndex = 0; controlIndex < paneMessageControls.controls.size(); ++controlIndex) {
                        const auto& control = paneMessageControls.controls[controlIndex];
                        controlFacts += L"; owned control=" + std::to_wstring(controlIndex) + L"/deliveryMatchesExpected=" + std::to_wstring(control.expectedDelivery) +
                            L"/WM_NULLdelta=" + std::to_wstring(control.final.observedBeforeLocal-control.initial.nullCount) +
                            L"/actualMessageDelivered=" + std::to_wstring(control.message.delivered()) +
                            L"/kernelExited=" + std::to_wstring(control.kernelExited) + L"/drain=" + hresultMessage(control.drain) +
                            L"/exitRead=" + hresultMessage(control.exitRead) + L"/exit=" + std::to_wstring(control.exitCode);
                    }
                    check("native_preview_creator_private_window_controls", paneCalibration == S_OK && paneMessageControls.calibrated &&
                        !paneMessageControls.overflow, controlFacts);
                    // A separate real startup with Preview already enabled
                    // cannot accidentally preview the earlier BMP selection
                    // through an unrelated third-party image association.
                    paneApp.reset(new ExplorerApp(instance_, true, requestedRibbonLayout_));
                    paneApp->headlessPaneHostingTrace_ = true;
                    paneApp->preferences_.navigationPane = false;
                    paneApp->preferences_.detailsPane = false;
                    paneApp->preferences_.previewPane = true;
                    created = SUCCEEDED(stage) ? paneApp->create(paneRoot.wstring()) : stage;
                    actualReady = SUCCEEDED(created) && pumpUntil(paneReady, 5000);
                    verifyCase(true);
                } else {
                    // No alternate association or native substitute is installed.
                    // This records unavailable coverage without a render PASS.
                    check("native_preview_unavailable_original_handler_not_activated", SUCCEEDED(stage) && paneIsolationPreserved,
                        L"effective handler=" + hresultMessage(handlerRead) + L"/" + originalHandler + L"; effective64bitserver=" +
                        hresultMessage(moduleRead) + L"; no preview selected/invoked");
                }
                paneApp.reset();
                bool sourcesPreserved = true;
                for (size_t index = 0; index < paths.size(); ++index) {
                    FILE_ID_INFO after{};
                    sourcesPreserved = SUCCEEDED(nativeFileIdentity(paths[index], after)) && identity(after) == identity(before[index]) &&
                        readBytes(paths[index]) == originalBytes[index] && sourcesPreserved;
                }
                FILE_BASIC_INFO previewBasicAfter{};
                {
                    struct SourceMetadataHandle {
                        HANDLE value = INVALID_HANDLE_VALUE;
                        ~SourceMetadataHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
                    } source{CreateFileW(paths[2].c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL, nullptr)};
                    sourcesPreserved = SUCCEEDED(originalPreviewBasicRead) && source.value != INVALID_HANDLE_VALUE &&
                        GetFileInformationByHandleEx(source.value, FileBasicInfo, &previewBasicAfter, sizeof(previewBasicAfter)) &&
                        previewBasicAfter.FileAttributes == originalPreviewBasic.FileAttributes &&
                        previewBasicAfter.CreationTime.QuadPart == originalPreviewBasic.CreationTime.QuadPart &&
                        previewBasicAfter.LastWriteTime.QuadPart == originalPreviewBasic.LastWriteTime.QuadPart &&
                        previewBasicAfter.ChangeTime.QuadPart == originalPreviewBasic.ChangeTime.QuadPart && sourcesPreserved;
                }
                check("native_pane_fixture_preserves_sources_and_original_app_view", sourcesPreserved && detailsRendered &&
                    view_.Get() == mainView.Get() && navigationCount_ == mainNavigation && history_.size() == mainHistory &&
                    historyIndex_ == mainHistoryIndex && activeQuery_ == mainQuery && currentPidl_ && mainLocation && ILIsEqual(currentPidl_.get(), mainLocation.get()));
            }
            {
                // Only the owned folder is selected: splitter geometry must
                // not activate an unrelated file's Preview handler. These are
                // real HWND DPI/mouse paths, with no synthetic WM_DPICHANGED.
                const auto splitterDeadline = GetTickCount64() + 10000;
                const auto splitterRoot = fixture / L"Subfolder" / L"Native Preview splitter";
                const auto splitterFolder = splitterRoot / L"Owned folder";
                const auto splitterMarker = splitterFolder / L"Owned marker.bin";
                std::filesystem::create_directories(splitterFolder);
                const std::string splitterMarkerBytes = "owned Preview splitter source stays unchanged";
                { std::ofstream splitterOutput(splitterMarker, std::ios::binary); splitterOutput << splitterMarkerBytes; }
                struct SplitterSource {
                    FILE_ID_INFO identity{};
                    FILE_BASIC_INFO basic{};
                };
                const auto splitterIdentity = [](const FILE_ID_INFO& splitterValue) {
                    std::array<BYTE, 16> splitterBytes{};
                    std::copy(std::begin(splitterValue.FileId.Identifier), std::end(splitterValue.FileId.Identifier), splitterBytes.begin());
                    return NativeFileIdentity{splitterValue.VolumeSerialNumber, splitterBytes};
                };
                const auto splitterReadSource = [](const std::filesystem::path& splitterPath, SplitterSource& splitterValue) {
                    struct SplitterHandle {
                        HANDLE value = INVALID_HANDLE_VALUE;
                        ~SplitterHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
                    } splitterHandle{CreateFileW(splitterPath.c_str(), FILE_READ_ATTRIBUTES,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL, nullptr)};
                    return splitterHandle.value != INVALID_HANDLE_VALUE &&
                        GetFileInformationByHandleEx(splitterHandle.value, FileIdInfo, &splitterValue.identity, sizeof(splitterValue.identity)) &&
                        GetFileInformationByHandleEx(splitterHandle.value, FileBasicInfo, &splitterValue.basic, sizeof(splitterValue.basic));
                };
                const std::array splitterPaths{splitterRoot, splitterFolder, splitterMarker};
                std::array<SplitterSource, 3> splitterSources{};
                bool splitterSourcesRead = true;
                for (size_t splitterSourceIndex = 0; splitterSourceIndex < splitterPaths.size(); ++splitterSourceIndex)
                    splitterSourcesRead = splitterReadSource(splitterPaths[splitterSourceIndex], splitterSources[splitterSourceIndex]) && splitterSourcesRead;
                const auto splitterSourcesCurrent = [&] {
                    for (size_t splitterSourceIndex = 0; splitterSourceIndex < splitterPaths.size(); ++splitterSourceIndex) {
                        SplitterSource splitterAfter;
                        if (!splitterReadSource(splitterPaths[splitterSourceIndex], splitterAfter) ||
                            splitterIdentity(splitterAfter.identity) != splitterIdentity(splitterSources[splitterSourceIndex].identity) ||
                            splitterAfter.basic.FileAttributes != splitterSources[splitterSourceIndex].basic.FileAttributes ||
                            splitterAfter.basic.CreationTime.QuadPart != splitterSources[splitterSourceIndex].basic.CreationTime.QuadPart ||
                            splitterAfter.basic.LastWriteTime.QuadPart != splitterSources[splitterSourceIndex].basic.LastWriteTime.QuadPart ||
                            splitterAfter.basic.ChangeTime.QuadPart != splitterSources[splitterSourceIndex].basic.ChangeTime.QuadPart) return false;
                    }
                    std::ifstream splitterInput(splitterMarker, std::ios::binary);
                    return std::string((std::istreambuf_iterator<char>(splitterInput)), std::istreambuf_iterator<char>()) == splitterMarkerBytes;
                };
                const auto splitterMainView = view_;
                const auto splitterMainFolder = folderView_;
                const auto splitterMainNavigation = navigationCount_;
                const auto splitterMainHistory = history_.size();
                const auto splitterMainHistoryIndex = historyIndex_;
                const auto splitterMainWidth = preferences_.previewWidth;
                Pidl splitterMainLocation(currentPidl_ ? ILCloneFull(currentPidl_.get()) : nullptr);
                const auto splitterSameBytes = [](PCIDLIST_ABSOLUTE splitterFirst, PCIDLIST_ABSOLUTE splitterSecond) {
                    if (!splitterFirst || !splitterSecond) return splitterFirst == splitterSecond;
                    const auto splitterSize = ILGetSize(splitterFirst);
                    return splitterSize == ILGetSize(splitterSecond) && std::memcmp(splitterFirst, splitterSecond, splitterSize) == 0;
                };
                const auto splitterSamePresentation = [](const SearchViewPresentation& splitterFirst, const SearchViewPresentation& splitterSecond) {
                    if (splitterFirst.mode != splitterSecond.mode || splitterFirst.iconSize != splitterSecond.iconSize ||
                        splitterFirst.visibleColumns != splitterSecond.visibleColumns ||
                        splitterFirst.groupBy.has_value() != splitterSecond.groupBy.has_value() ||
                        splitterFirst.sort.has_value() != splitterSecond.sort.has_value()) return false;
                    if (splitterFirst.groupBy && (splitterFirst.groupBy->property != splitterSecond.groupBy->property ||
                        splitterFirst.groupBy->direction != splitterSecond.groupBy->direction)) return false;
                    if (splitterFirst.sort) {
                        if (splitterFirst.sort->size() != splitterSecond.sort->size()) return false;
                        for (size_t splitterOrder = 0; splitterOrder < splitterFirst.sort->size(); ++splitterOrder)
                            if ((*splitterFirst.sort)[splitterOrder].property != (*splitterSecond.sort)[splitterOrder].property ||
                                (*splitterFirst.sort)[splitterOrder].direction != (*splitterSecond.sort)[splitterOrder].direction) return false;
                    }
                    return true;
                };
                auto releaseSplitterApp = [](ExplorerApp* splitterValue) {
                    if (splitterValue->window_ && IsWindow(splitterValue->window_)) SendMessageW(splitterValue->window_, WM_CLOSE, 0, 0);
                    if (FAILED(splitterValue->shutdownStatus_)) {
                        std::fprintf(stderr, "headless splitter child drain failed HRESULT=0x%08lX; owned sources retained\n",
                            static_cast<unsigned long>(splitterValue->shutdownStatus_)); std::fflush(stderr);
                        if (!TerminateProcess(GetCurrentProcess(), 9)) std::_Exit(9);
                        std::_Exit(9);
                    }
                    splitterValue->Release();
                };
                bool splitterMayCreate = splitterSourcesRead;
                for (const bool splitterRtl : {false, true}) {
                    std::unique_ptr<ExplorerApp, decltype(releaseSplitterApp)> splitterApp(
                        new ExplorerApp(instance_, true, requestedRibbonLayout_), releaseSplitterApp);
                    splitterApp->headlessDirectionOverride_ = splitterRtl;
                    splitterApp->preferences_.navigationPane = false;
                    splitterApp->preferences_.detailsPane = false;
                    splitterApp->preferences_.previewPane = true;
                    splitterApp->preferences_.previewWidth = 300;
                    HRESULT splitterStage = splitterMayCreate && splitterSourcesCurrent() && GetTickCount64() < splitterDeadline ?
                        splitterApp->create(splitterRoot.wstring()) : E_ABORT;
                    const auto splitterWait = [&](const auto& splitterPredicate) {
                        const auto splitterNow = GetTickCount64();
                        return splitterNow < splitterDeadline && pumpUntil(splitterPredicate, static_cast<DWORD>(splitterDeadline - splitterNow));
                    };
                    const bool splitterNavigated = splitterStage == S_OK && splitterWait([&] {
                        if (splitterApp->navigating_ || !splitterApp->folderView_) return false;
                        ComPtr<IShellItemArray> splitterItems;
                        std::set<NativeFileIdentity> splitterIds;
                        return splitterApp->folderView_->Items(SVGIO_ALLVIEW, IID_PPV_ARGS(&splitterItems)) == S_OK && splitterItems &&
                            nativeArrayIdentities(splitterItems.Get(), splitterIds) == S_OK &&
                            splitterIds == std::set<NativeFileIdentity>{splitterIdentity(splitterSources[1].identity)};
                    });
                    PrivatePresentation splitterPresentation(splitterNavigated ? splitterApp->window_ : nullptr, true);
                    Pidl splitterFolderPidl;
                    ComPtr<IShellItem> splitterFolderItem;
                    if (splitterNavigated && splitterPresentation.ready)
                        splitterStage = SHCreateItemFromParsingName(splitterFolder.c_str(), nullptr, IID_PPV_ARGS(&splitterFolderItem));
                    else splitterStage = E_ABORT;
                    PIDLIST_ABSOLUTE splitterRawFolder = nullptr;
                    if (splitterStage == S_OK) splitterStage = SHGetIDListFromObject(splitterFolderItem.Get(), &splitterRawFolder);
                    splitterFolderPidl.reset(splitterRawFolder);
                    if (splitterStage == S_OK && splitterFolderPidl && GetTickCount64() < splitterDeadline)
                        splitterStage = splitterApp->view_->SelectItem(ILFindLastID(splitterFolderPidl.get()), SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_FOCUSED);
                    else splitterStage = E_ABORT;
                    const bool splitterSelected = splitterStage == S_OK && splitterWait([&] {
                        int splitterCount = 0;
                        return !splitterApp->navigating_ && !splitterApp->selectionStateDirty_ && !splitterApp->namespaceDirty_ &&
                            splitterApp->folderView_->ItemCount(SVGIO_SELECTION, &splitterCount) == S_OK && splitterCount == 1;
                    });
                    const auto splitterView = splitterApp->view_;
                    const auto splitterFolderView = splitterApp->folderView_;
                    const auto splitterNavigation = splitterApp->navigationCount_;
                    const auto splitterHistoryIndex = splitterApp->historyIndex_;
                    Pidl splitterLocation(splitterApp->currentPidl_ ? ILCloneFull(splitterApp->currentPidl_.get()) : nullptr);
                    std::vector<Pidl> splitterHistory;
                    bool splitterHistoryCopied = true;
                    for (const auto& splitterEntry : splitterApp->history_) {
                        Pidl splitterCopy(splitterEntry ? ILCloneFull(splitterEntry.get()) : nullptr);
                        splitterHistoryCopied = splitterHistoryCopied && splitterCopy != nullptr;
                        splitterHistory.push_back(std::move(splitterCopy));
                    }
                    const auto splitterCurrent = [&] {
                        if (GetTickCount64() >= splitterDeadline || !splitterSelected || !splitterLocation || !splitterHistoryCopied ||
                            splitterApp->closing_ || splitterApp->navigating_ || splitterApp->view_.Get() != splitterView.Get() ||
                            splitterApp->folderView_.Get() != splitterFolderView.Get() || splitterApp->navigationCount_ != splitterNavigation ||
                            splitterApp->historyIndex_ != splitterHistoryIndex || splitterApp->history_.size() != splitterHistory.size() ||
                            !splitterSameBytes(splitterLocation.get(), splitterApp->currentPidl_.get())) return false;
                        for (size_t splitterEntry = 0; splitterEntry < splitterHistory.size(); ++splitterEntry)
                            if (!splitterSameBytes(splitterHistory[splitterEntry].get(), splitterApp->history_[splitterEntry].get())) return false;
                        return true;
                    };
                    struct SplitterState {
                        std::set<NativeFileIdentity> members, selection;
                        NativeFileIdentity focused;
                        DWORD focusFlags = 0, folderFlags = 0;
                        SearchViewPresentation presentation;
                    } splitterBefore;
                    const auto splitterReadState = [&](SplitterState& splitterState) -> HRESULT {
                        if (!splitterCurrent()) return E_ABORT;
                        ComPtr<IShellItemArray> splitterAll, splitterSelection;
                        auto splitterRead = splitterFolderView->Items(SVGIO_ALLVIEW, IID_PPV_ARGS(&splitterAll));
                        if (splitterRead == S_OK) splitterRead = splitterAll ? nativeArrayIdentities(splitterAll.Get(), splitterState.members) : E_UNEXPECTED;
                        if (splitterRead == S_OK) splitterRead = splitterFolderView->GetSelection(FALSE, &splitterSelection);
                        if (splitterRead == S_OK) splitterRead = splitterSelection ? nativeArrayIdentities(splitterSelection.Get(), splitterState.selection) : E_UNEXPECTED;
                        int splitterFocusedIndex = -1;
                        if (splitterRead == S_OK) splitterRead = splitterFolderView->GetFocusedItem(&splitterFocusedIndex);
                        ComPtr<IShellItem> splitterFocusedItem;
                        if (splitterRead == S_OK) splitterRead = splitterFocusedIndex >= 0 ?
                            splitterFolderView->GetItem(splitterFocusedIndex, IID_PPV_ARGS(&splitterFocusedItem)) : E_UNEXPECTED;
                        FILE_ID_INFO splitterFocusedId{};
                        if (splitterRead == S_OK) splitterRead = splitterFocusedItem ?
                            nativeFileIdentity(itemName(splitterFocusedItem.Get(), SIGDN_FILESYSPATH), splitterFocusedId) : E_UNEXPECTED;
                        if (splitterRead == S_OK) splitterState.focused = splitterIdentity(splitterFocusedId);
                        PITEMID_CHILD splitterRawFocus = nullptr;
                        if (splitterRead == S_OK) splitterRead = splitterFolderView->Item(splitterFocusedIndex, &splitterRawFocus);
                        Pidl splitterFocusPidl(splitterRawFocus);
                        if (splitterRead == S_OK) splitterRead = splitterFocusPidl ?
                            splitterFolderView->GetSelectionState(splitterFocusPidl.get(), &splitterState.focusFlags) : E_UNEXPECTED;
                        if (splitterRead == S_OK) splitterRead = splitterFolderView->GetCurrentFolderFlags(&splitterState.folderFlags);
                        if (splitterRead == S_OK) splitterRead = captureSearchViewPresentation(splitterFolderView.Get(), &splitterState.presentation);
                        return splitterRead == S_OK && !splitterCurrent() ? E_ABORT : splitterRead;
                    };
                    const auto splitterPersistence = [&] {
                        const auto& splitterReceipt = splitterApp->persistenceStatus_;
                        return std::tuple{splitterReceipt.searchHistory, splitterReceipt.addressHistory, splitterReceipt.windowPlacement,
                            splitterReceipt.ribbonState, splitterReceipt.preferences, splitterReceipt.ribbonSettings, splitterReceipt.result,
                            splitterApp->searchHistorySaveStatus_, splitterApp->pendingSearchHistorySaveError_, splitterApp->addressHistoryStatus_, splitterApp->lastError_};
                    };
                    const auto splitterReceipts = splitterPersistence();
                    HWND splitterViewWindow = nullptr;
                    if (splitterSelected) splitterStage = splitterView->GetWindow(&splitterViewWindow);
                    if (splitterStage == S_OK) splitterStage = splitterReadState(splitterBefore);
                    const std::array splitterWindows{splitterApp->window_, splitterApp->previewPane_, splitterApp->previewRender_, splitterViewWindow};
                    std::array<std::pair<LONG_PTR, LONG_PTR>, 4> splitterStyles{};
                    bool splitterOwned = splitterStage == S_OK;
                    for (size_t splitterWindow = 0; splitterWindow < splitterWindows.size(); ++splitterWindow) {
                        DWORD splitterProcess = 0;
                        splitterOwned = splitterOwned && splitterWindows[splitterWindow] &&
                            GetWindowThreadProcessId(splitterWindows[splitterWindow], &splitterProcess) == GetCurrentThreadId() && splitterProcess == GetCurrentProcessId() &&
                            (splitterWindow == 0 || IsChild(splitterApp->window_, splitterWindows[splitterWindow]));
                        splitterStyles[splitterWindow] = {GetWindowLongPtrW(splitterWindows[splitterWindow], GWL_STYLE), GetWindowLongPtrW(splitterWindows[splitterWindow], GWL_EXSTYLE)};
                    }
                    const auto splitterDpi = GetDpiForWindow(splitterApp->window_);
                    bool splitterDirection = !splitterRtl;
                    const bool splitterSetup = splitterOwned && splitterSourcesCurrent() && splitterDpi && splitterDpi == splitterApp->dpi_ &&
                        GetDpiForWindow(splitterApp->previewPane_) == splitterDpi && GetDpiForWindow(splitterApp->previewRender_) == splitterDpi &&
                        windowUiDirection(splitterApp->window_, &splitterDirection) == S_OK && splitterDirection == splitterRtl &&
                        splitterBefore.members == std::set<NativeFileIdentity>{splitterIdentity(splitterSources[1].identity)} &&
                        splitterBefore.selection == splitterBefore.members && splitterBefore.focused == splitterIdentity(splitterSources[1].identity) && GetCapture() == nullptr;
                    const auto splitterIntact = [&](bool splitterPaneHidden = false) {
                        SplitterState splitterAfter;
                        if (!splitterSetup || splitterReadState(splitterAfter) != S_OK || !splitterSourcesCurrent() ||
                            splitterAfter.members != splitterBefore.members || splitterAfter.selection != splitterBefore.selection ||
                            splitterAfter.focused != splitterBefore.focused || splitterAfter.focusFlags != splitterBefore.focusFlags ||
                            splitterAfter.folderFlags != splitterBefore.folderFlags || !splitterSamePresentation(splitterAfter.presentation, splitterBefore.presentation) ||
                            splitterPersistence() != splitterReceipts) return false;
                        for (size_t splitterWindow = 0; splitterWindow < splitterWindows.size(); ++splitterWindow) {
                            auto expectedStyles = splitterStyles[splitterWindow];
                            if (splitterPaneHidden && splitterWindow == 1) expectedStyles.first &= ~static_cast<LONG_PTR>(WS_VISIBLE);
                            if (std::pair{GetWindowLongPtrW(splitterWindows[splitterWindow], GWL_STYLE), GetWindowLongPtrW(splitterWindows[splitterWindow], GWL_EXSTYLE)} != expectedStyles) return false;
                        }
                        const auto splitterDesktop = PrivateDesktop::current();
                        return splitterCurrent() && splitterDesktop && splitterDesktop->verifyIsolation() == S_OK;
                    };
                    const auto splitterOriginalGeometry = readNativePaneGeometry(splitterView.Get(), splitterApp->window_,
                        splitterApp->previewPane_, splitterApp->previewSplitter_, true, splitterDeadline, splitterCurrent, splitterApp->previewGrip_);
                    bool splitterGeometryPassed = splitterOriginalGeometry.read == S_OK && splitterOriginalGeometry.footerFull &&
                        splitterOriginalGeometry.partition && splitterIntact();
                    std::wstring splitterGeometryTrace = L"initial=" + paneGeometryFacts(splitterOriginalGeometry);
                    const auto splitterGeometryIntact = [&] {
                        const auto sampled = readNativePaneGeometry(splitterView.Get(), splitterApp->window_,
                            splitterApp->previewPane_, splitterApp->previewSplitter_, true, splitterDeadline, splitterCurrent, splitterApp->previewGrip_);
                        const bool geometryPreserved = sampled.read == S_OK && sampled.footerFull && sampled.partition &&
                            EqualRect(&sampled.frameClient, &splitterOriginalGeometry.frameClient) &&
                            EqualRect(&sampled.footer, &splitterOriginalGeometry.footer) && splitterIntact();
                        if (!geometryPreserved) splitterGeometryTrace += L"; failure=" + paneGeometryFacts(sampled);
                        splitterGeometryPassed = geometryPreserved && splitterGeometryPassed;
                        return geometryPreserved;
                    };
                    const auto splitterBounds = [&](HWND splitterWindow, RECT& splitterResult) {
                        RECT splitterScreen{};
                        return GetWindowRect(splitterWindow, &splitterScreen) && mapUiRect(nullptr, splitterApp->window_, splitterScreen, &splitterResult) == S_OK;
                    };
                    struct SplitterCaptureGuard {
                        HWND window;
                        ~SplitterCaptureGuard() { if (window && GetCapture() == window) ReleaseCapture(); }
                    } splitterCaptureGuard{splitterApp->window_};
                    bool splitterDragsPassed = splitterSetup, splitterBoundsPassed = splitterSetup;
                    unsigned splitterCompletedGrips = 0;
                    std::wstring splitterTrace;
                    const auto splitterDownAtActualTarget = [&](int x, int y) {
                        if (!splitterIntact() || GetTickCount64() >= splitterDeadline) return false;
                        const auto root = splitterApp->window_, expected = splitterApp->previewGrip_;
                        const auto logicalGrip = splitterApp->previewSplitter_;
                        RECT physicalGrip{}, actualGrip{};
                        const POINT requested{x,y}; POINT physical{}, rootLocal{}, targetLocal{};
                        auto mapped = mapUiRect(root, nullptr, logicalGrip, &physicalGrip);
                        if (mapped == S_OK) mapped = mapUiPoint(root, nullptr, requested, &physical);
                        if (mapped == S_OK) mapped = mapUiPoint(nullptr, root, physical, &rootLocal);
                        SetLastError(ERROR_SUCCESS);
                        const auto target = mapped == S_OK ? ChildWindowFromPointEx(root, rootLocal, CWP_SKIPINVISIBLE) : nullptr;
                        const auto hitError = GetLastError();
                        const bool exact = expected && expected == splitterOriginalGeometry.expectedGrip && target == expected && paneGeometryOwned(expected, root) &&
                            GetAncestor(expected, GA_PARENT) == root && IsWindowVisible(expected) && PtInRect(&physicalGrip, physical) &&
                            !(GetWindowLongPtrW(expected, GWL_STYLE) & WS_TABSTOP) && GetDpiForWindow(expected) == splitterDpi &&
                            GetWindowRect(expected, &actualGrip) && EqualRect(&physicalGrip, &actualGrip) &&
                            rootLocal.x == requested.x && rootLocal.y == requested.y;
                        if (mapped == S_OK && exact) mapped = mapUiPoint(root, target, requested, &targetLocal);
                        const bool current = exact && mapped == S_OK && splitterCurrent() && GetTickCount64() < splitterDeadline &&
                            root == splitterApp->window_ && expected == splitterApp->previewGrip_ &&
                            EqualRect(&logicalGrip, &splitterApp->previewSplitter_) &&
                            targetLocal.x >= std::numeric_limits<short>::min() && targetLocal.x <= std::numeric_limits<short>::max() &&
                            targetLocal.y >= std::numeric_limits<short>::min() && targetLocal.y <= std::numeric_limits<short>::max();
                        splitterTrace += L"; actual down target/expected="+std::to_wstring(reinterpret_cast<UINT_PTR>(target))+L"/"+
                            std::to_wstring(reinterpret_cast<UINT_PTR>(expected))+L"; mapped/error/exact/source="+hresultMessage(mapped)+L"/"+
                            std::to_wstring(hitError)+L"/"+std::to_wstring(exact)+L"/"+std::to_wstring(current)+L"; target client="+
                            std::to_wstring(targetLocal.x)+L","+std::to_wstring(targetLocal.y);
                        if (!current) return false;
                        // One original press reaches the real hit child. Its
                        // production forwarding establishes normal root capture.
                        SendMessageW(target, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(targetLocal.x, targetLocal.y));
                        return true;
                    };
                    const auto splitterMoveToCapturedRoot = [&](int x, int y) {
                        const auto target = GetCapture();
                        if (target != splitterApp->window_ || !splitterCurrent() || GetTickCount64() >= splitterDeadline ||
                            !paneGeometryOwned(target, target)) return false;
                        SendMessageW(target, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x,y));
                        return true;
                    };
                    RECT splitterOriginalPane{}, splitterOriginalView{};
                    const RECT splitterOriginalGrip = splitterApp->previewSplitter_;
                    splitterDragsPassed = splitterDragsPassed && splitterBounds(splitterApp->previewPane_, splitterOriginalPane) && splitterBounds(splitterViewWindow, splitterOriginalView);
                    for (const bool splitterFarEdge : {false, true}) {
                        if (!splitterDragsPassed || !splitterIntact()) { splitterDragsPassed = false; break; }
                        const auto splitterGrip = splitterApp->previewSplitter_;
                        const int splitterX = splitterFarEdge ? splitterGrip.right - 1 : splitterGrip.left + 1;
                        const int splitterY = (splitterGrip.top + splitterGrip.bottom) / 2;
                        const int splitterWidthBefore = splitterApp->preferences_.previewWidth;
                        const bool splitterDispatched = splitterDownAtActualTarget(splitterX, splitterY);
                        const bool splitterCaptured = splitterDispatched && splitterApp->previewResizing_ && GetCapture() == splitterApp->window_;
                        bool splitterNoJump = false, splitterMoved = false, splitterLimits = false, splitterReturned = false;
                        if (splitterCaptured && splitterIntact()) {
                            const bool splitterStillSent = splitterMoveToCapturedRoot(splitterX, splitterY);
                            RECT splitterPaneStill{}, splitterViewStill{};
                            splitterNoJump = splitterStillSent && splitterIntact() && splitterBounds(splitterApp->previewPane_, splitterPaneStill) && splitterBounds(splitterViewWindow, splitterViewStill) &&
                                EqualRect(&splitterPaneStill, &splitterOriginalPane) && EqualRect(&splitterViewStill, &splitterOriginalView) &&
                                EqualRect(&splitterGrip, &splitterApp->previewSplitter_) && splitterApp->preferences_.previewWidth == splitterWidthBefore;
                        }
                        if (splitterNoJump) {
                            const bool splitterMoveSent = splitterMoveToCapturedRoot(splitterX - splitterApp->px(40), splitterY);
                            RECT splitterPaneMoved{}, splitterViewMoved{};
                            splitterMoved = splitterMoveSent && splitterIntact() && splitterBounds(splitterApp->previewPane_, splitterPaneMoved) && splitterBounds(splitterViewWindow, splitterViewMoved) &&
                                splitterApp->preferences_.previewWidth == splitterWidthBefore + 40 && splitterPaneMoved.right == splitterOriginalPane.right &&
                                splitterPaneMoved.left == splitterOriginalPane.left - splitterApp->px(40) && splitterViewMoved.left == splitterOriginalView.left &&
                                splitterViewMoved.right == splitterOriginalView.right - splitterApp->px(40);
                            if (splitterMoved) splitterMoved = splitterGeometryIntact();
                        }
                        if (splitterMoved) {
                            const bool splitterMinimumSent = splitterMoveToCapturedRoot(32760, splitterY);
                            RECT splitterMinimum{};
                            const bool splitterMinimumPassed = splitterMinimumSent && splitterIntact() && splitterBounds(splitterApp->previewPane_, splitterMinimum) &&
                                splitterApp->preferences_.previewWidth == 120 && splitterMinimum.right - splitterMinimum.left == splitterApp->px(120);
                            if (splitterMinimumPassed) {
                                const bool splitterMaximumSent = splitterMoveToCapturedRoot(-32760, splitterY);
                                RECT splitterMaximum{};
                                const auto splitterMaximumGeometry = readNativePaneGeometry(splitterView.Get(), splitterApp->window_,
                                    splitterApp->previewPane_, splitterApp->previewSplitter_, true, splitterDeadline, splitterCurrent, splitterApp->previewGrip_);
                                splitterLimits = splitterMaximumSent && splitterIntact() && splitterMaximumGeometry.read == S_OK && splitterMaximumGeometry.footerFull &&
                                    splitterMaximumGeometry.partition &&
                                    splitterBounds(splitterApp->previewPane_, splitterMaximum) && splitterApp->preferences_.previewWidth == 4096 &&
                                    splitterMaximum.right - splitterMaximum.left == std::max(splitterApp->px(120),
                                        static_cast<int>((splitterMaximumGeometry.parentClient.right - splitterMaximumGeometry.parentClient.left) / 2));
                                splitterGeometryPassed = splitterLimits && splitterGeometryPassed;
                                if (!splitterLimits) splitterGeometryTrace += L"; maximum=" + paneGeometryFacts(splitterMaximumGeometry);
                            }
                        }
                        if (splitterLimits) {
                            const bool splitterReturnSent = splitterMoveToCapturedRoot(splitterX, splitterY);
                            RECT splitterPaneReturned{}, splitterViewReturned{};
                            splitterReturned = splitterReturnSent && splitterIntact() && splitterBounds(splitterApp->previewPane_, splitterPaneReturned) && splitterBounds(splitterViewWindow, splitterViewReturned) &&
                                splitterApp->preferences_.previewWidth == splitterWidthBefore && EqualRect(&splitterPaneReturned, &splitterOriginalPane) &&
                                EqualRect(&splitterViewReturned, &splitterOriginalView) && EqualRect(&splitterGrip, &splitterApp->previewSplitter_);
                            if (splitterReturned) splitterReturned = splitterGeometryIntact();
                        }
                        // Always release this exact owned capture before any
                        // native view or source can unwind after a failed arm.
                        if (const auto capturedRoot = GetCapture(); capturedRoot == splitterApp->window_)
                            SendMessageW(capturedRoot, WM_LBUTTONUP, 0, MAKELPARAM(splitterX, splitterY));
                        const bool splitterReleased = !splitterApp->previewResizing_ && GetCapture() == nullptr;
                        splitterTrace += L"; grip=" + std::to_wstring(splitterFarEdge) + L" capture/nojump/move/limits/return/release=" +
                            std::to_wstring(splitterCaptured) + L"/" + std::to_wstring(splitterNoJump) + L"/" + std::to_wstring(splitterMoved) + L"/" +
                            std::to_wstring(splitterLimits) + L"/" + std::to_wstring(splitterReturned) + L"/" + std::to_wstring(splitterReleased);
                        splitterBoundsPassed = splitterLimits && splitterBoundsPassed;
                        splitterDragsPassed = splitterCaptured && splitterNoJump && splitterMoved && splitterLimits && splitterReturned && splitterReleased && splitterIntact();
                        if (splitterDragsPassed) ++splitterCompletedGrips;
                    }
                    bool splitterCancellation = false;
                    if (splitterDragsPassed && splitterIntact()) {
                        const int splitterCancelX = splitterApp->previewSplitter_.left + 1;
                        const int splitterCancelY = (splitterApp->previewSplitter_.top + splitterApp->previewSplitter_.bottom) / 2;
                        const bool splitterCancelDispatched = splitterDownAtActualTarget(splitterCancelX, splitterCancelY);
                        const bool splitterCancelCaptured = splitterCancelDispatched && splitterApp->previewResizing_ && GetCapture() == splitterApp->window_;
                        const auto splitterCancelWidth = splitterApp->preferences_.previewWidth;
                        if (splitterCancelCaptured && splitterIntact()) {
                            const bool splitterCancelReleased = ReleaseCapture() != FALSE;
                            const bool splitterCancelObserved = !splitterApp->previewResizing_ && GetCapture() == nullptr;
                            if (splitterCancelReleased && splitterCancelObserved && splitterIntact()) {
                                SendMessageW(splitterApp->window_, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(splitterCancelX - splitterApp->px(40), splitterCancelY));
                                splitterCancellation = !splitterApp->previewResizing_ && GetCapture() == nullptr &&
                                    splitterApp->preferences_.previewWidth == splitterCancelWidth && splitterIntact();
                            }
                        }
                    }
                    const bool splitterRestored = splitterDragsPassed && splitterCancellation && splitterIntact() &&
                        splitterApp->preferences_.previewWidth == 300 && EqualRect(&splitterOriginalGrip, &splitterApp->previewSplitter_) && GetCapture() == nullptr;
                    const auto splitterFacts = L"create/seed/snapshot=" + hresultMessage(splitterStage) + L"; actual HWND DPI=" +
                        std::to_wstring(splitterDpi) + L"; native96=" + std::to_wstring(splitterDpi == 96) + L"; RTL=" + std::to_wstring(splitterRtl) +
                        L"; original full selection/focus=" + std::to_wstring(splitterBefore.selection.size()) + L"/" + std::to_wstring(splitterBefore.focused == splitterIdentity(splitterSources[1].identity)) +
                        L"; completed grips=" + std::to_wstring(splitterCompletedGrips) + L"; capture-loss inert=" + std::to_wstring(splitterCancellation) +
                        L"; source/view/presentation/history/styles/persistence restored=" + std::to_wstring(splitterRestored) + splitterTrace;
                    check(splitterRtl ? "native_preview_splitter_rtl_exact_grab_bounds_and_source_preservation" :
                        "native_preview_splitter_ltr_exact_grab_bounds_and_source_preservation",
                        splitterRestored && splitterBoundsPassed && splitterCompletedGrips == 2, splitterFacts);
                    const auto splitterFrameInsets = [](const RECT& root, const RECT& frame) {
                        return RECT{frame.left-root.left, frame.top-root.top, root.right-frame.right, root.bottom-frame.bottom};
                    };
                    const auto sameFrameInsets = [&](const NativePaneGeometry& geometry) {
                        const auto original = splitterFrameInsets(splitterOriginalGeometry.rootClient, splitterOriginalGeometry.frameClient);
                        const auto actual = splitterFrameInsets(geometry.rootClient, geometry.frameClient);
                        return EqualRect(&original, &actual);
                    };
                    const auto splitterRectangleFacts = [](const RECT& value) {
                        return std::to_wstring(value.left)+L","+std::to_wstring(value.top)+L","+
                            std::to_wstring(value.right)+L","+std::to_wstring(value.bottom);
                    };
                    bool splitterOffRestored = false, splitterNoopRestored = false, splitterResizePassed = false, splitterFinalGeometry = false;
                    if (splitterRestored && splitterGeometryPassed && splitterIntact()) {
                        // Same current native view: prove OFF restores its full
                        // independent parent slot even when SetRect is a no-op.
                        splitterApp->preferences_.previewPane = false; splitterApp->layout();
                        const auto off = readNativePaneGeometry(splitterView.Get(), splitterApp->window_, splitterApp->previewPane_,
                            splitterApp->previewSplitter_, false, splitterDeadline, splitterCurrent, splitterApp->previewGrip_);
                        splitterOffRestored = off.read == S_OK && off.footerFull && off.partition && sameFrameInsets(off) &&
                            EqualRect(&off.footer, &splitterOriginalGeometry.footer) && splitterIntact(true);
                        if (splitterOffRestored) splitterApp->layout();
                        const auto noop = readNativePaneGeometry(splitterView.Get(), splitterApp->window_, splitterApp->previewPane_,
                            splitterApp->previewSplitter_, false, splitterDeadline, splitterCurrent, splitterApp->previewGrip_);
                        splitterNoopRestored = splitterOffRestored && noop.read == S_OK && noop.footerFull && noop.partition &&
                            EqualRect(&off.content, &noop.content) && EqualRect(&off.footer, &noop.footer) && splitterIntact(true);
                        splitterGeometryTrace += L"; off=" + paneGeometryFacts(off) + L"; noop=" + paneGeometryFacts(noop);
                        if (splitterNoopRestored && splitterIntact(true)) { splitterApp->preferences_.previewPane = true; splitterApp->layout(); }
                        RECT originalRoot{};
                        if (splitterNoopRestored && GetWindowRect(splitterApp->window_, &originalRoot) && splitterGeometryIntact()) {
                            const LONG resizeWidthDelta = -40, resizeHeightDelta = -20;
                            const LONG originalRootWidth = originalRoot.right-originalRoot.left;
                            const LONG originalRootHeight = originalRoot.bottom-originalRoot.top;
                            const LONG requestedRootWidth = originalRootWidth+resizeWidthDelta;
                            const LONG requestedRootHeight = originalRootHeight+resizeHeightDelta;
                            // Keep the shrink above the production tracking minimum.
                            const bool resizeFeasible = requestedRootWidth >= splitterApp->px(600) &&
                                requestedRootHeight >= splitterApp->px(320);
                            SetLastError(ERROR_SUCCESS);
                            const BOOL resized = resizeFeasible && SetWindowPos(splitterApp->window_, nullptr, 0, 0,
                                requestedRootWidth, requestedRootHeight,
                                SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER|SWP_NOOWNERZORDER);
                            const auto resizeRead = resizeFeasible ? (resized ? S_OK : paneGeometryError()) : E_ABORT;
                            RECT actualResizedRoot{};
                            HRESULT actualResizedRootRead = E_ABORT;
                            if (splitterCurrent()) {
                                SetLastError(ERROR_SUCCESS);
                                const auto actualRootRead = GetWindowRect(splitterApp->window_, &actualResizedRoot);
                                actualResizedRootRead = actualRootRead ? S_OK : paneGeometryError();
                            }
                            const LONG actualRootWidth = actualResizedRoot.right-actualResizedRoot.left;
                            const LONG actualRootHeight = actualResizedRoot.bottom-actualResizedRoot.top;
                            const LONG actualRootWidthDelta = actualRootWidth-originalRootWidth;
                            const LONG actualRootHeightDelta = actualRootHeight-originalRootHeight;
                            const bool actualRootResizeMatches = actualResizedRootRead == S_OK &&
                                actualRootWidth == requestedRootWidth && actualRootHeight == requestedRootHeight &&
                                actualRootWidthDelta != 0 && actualRootHeightDelta != 0 &&
                                actualRootWidthDelta == resizeWidthDelta && actualRootHeightDelta == resizeHeightDelta;
                            const auto resizedGeometry = readNativePaneGeometry(splitterView.Get(), splitterApp->window_, splitterApp->previewPane_,
                                splitterApp->previewSplitter_, true, splitterDeadline, splitterCurrent, splitterApp->previewGrip_);
                            splitterResizePassed = resizeFeasible && resized && actualRootResizeMatches &&
                                resizedGeometry.read == S_OK && resizedGeometry.footerFull && resizedGeometry.partition &&
                                sameFrameInsets(resizedGeometry) && resizedGeometry.frameClient.right-resizedGeometry.frameClient.left ==
                                splitterOriginalGeometry.frameClient.right-splitterOriginalGeometry.frameClient.left+resizeWidthDelta && splitterIntact();
                            HRESULT rootRestoreRead = E_ABORT;
                            if (splitterCurrent()) {
                                SetLastError(ERROR_SUCCESS);
                                const auto restored = SetWindowPos(splitterApp->window_, nullptr, 0, 0,
                                    originalRoot.right-originalRoot.left, originalRoot.bottom-originalRoot.top,
                                    SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER|SWP_NOOWNERZORDER);
                                rootRestoreRead = restored ? S_OK : paneGeometryError();
                            }
                            splitterFinalGeometry = rootRestoreRead == S_OK && splitterGeometryIntact() && splitterIntact();
                            splitterGeometryTrace += L"; root resize/restore HRESULT=" + hresultMessage(resizeRead) + L"/" +
                                hresultMessage(rootRestoreRead) + L"; requested resize width/height=" +
                                std::to_wstring(requestedRootWidth) + L"/" +
                                std::to_wstring(requestedRootHeight) +
                                L"; feasible resize/actual nonzero root size matches=" + std::to_wstring(resizeFeasible) + L"/" +
                                std::to_wstring(actualRootResizeMatches) + L"; actual/requested root width/height deltas=" +
                                std::to_wstring(actualRootWidthDelta) + L"/" + std::to_wstring(actualRootHeightDelta) + L"/" +
                                std::to_wstring(resizeWidthDelta) + L"/" + std::to_wstring(resizeHeightDelta) +
                                L"; actual resized root HRESULT/rectangle=" + hresultMessage(actualResizedRootRead) + L"/" +
                                splitterRectangleFacts(actualResizedRoot) + L"; original root rectangle=" + splitterRectangleFacts(originalRoot) +
                                L"; original/resized native frame insets=" +
                                splitterRectangleFacts(splitterFrameInsets(splitterOriginalGeometry.rootClient, splitterOriginalGeometry.frameClient)) + L"/" +
                                splitterRectangleFacts(splitterFrameInsets(resizedGeometry.rootClient, resizedGeometry.frameClient)) +
                                L"; resized frame actual/expected width=" +
                                std::to_wstring(resizedGeometry.frameClient.right-resizedGeometry.frameClient.left) + L"/" +
                                std::to_wstring(splitterOriginalGeometry.frameClient.right-splitterOriginalGeometry.frameClient.left+resizeWidthDelta) +
                                L"; pure resized frame insets/width matches=" + std::to_wstring(sameFrameInsets(resizedGeometry)) + L"/" +
                                std::to_wstring(resizedGeometry.frameClient.right-resizedGeometry.frameClient.left ==
                                    splitterOriginalGeometry.frameClient.right-splitterOriginalGeometry.frameClient.left+resizeWidthDelta) +
                                L"; resized=" + paneGeometryFacts(resizedGeometry);
                        }
                    }
                    splitterGeometryTrace += L"; exact accepted predicates geometry/drag/off/noop/resize/final=" +
                        std::to_wstring(splitterGeometryPassed) + L"/" + std::to_wstring(splitterRestored) + L"/" +
                        std::to_wstring(splitterOffRestored) + L"/" +
                        std::to_wstring(splitterNoopRestored) + L"/" + std::to_wstring(splitterResizePassed) + L"/" +
                        std::to_wstring(splitterFinalGeometry);
                    check(splitterRtl ? "native_preview_rtl_full_footer_partition_off_noop_and_resize" :
                        "native_preview_ltr_full_footer_partition_off_noop_and_resize",
                        splitterGeometryPassed && splitterOffRestored && splitterNoopRestored && splitterResizePassed && splitterFinalGeometry,
                        splitterGeometryTrace);
                    // A failed source/state arm must not be overwritten by
                    // selecting the same source in a second native browser.
                    splitterMayCreate = splitterRestored && splitterGeometryPassed && splitterOffRestored && splitterNoopRestored &&
                        splitterResizePassed && splitterFinalGeometry && splitterSourcesCurrent();
                }
                const auto splitterDesktop = PrivateDesktop::current();
                const bool splitterMainPreserved = splitterSourcesRead && splitterSourcesCurrent() && view_.Get() == splitterMainView.Get() &&
                    folderView_.Get() == splitterMainFolder.Get() && navigationCount_ == splitterMainNavigation && history_.size() == splitterMainHistory &&
                    historyIndex_ == splitterMainHistoryIndex && preferences_.previewWidth == splitterMainWidth &&
                    splitterMainLocation && splitterSameBytes(splitterMainLocation.get(), currentPidl_.get()) &&
                    splitterDesktop && splitterDesktop->verifyIsolation() == S_OK && !IsWindowVisible(window_) && GetCapture() == nullptr;
                std::error_code splitterRemoveError;
                std::filesystem::remove_all(splitterRoot, splitterRemoveError);
                check("native_preview_splitter_teardown_preserves_original_app_and_owned_sources", splitterMainPreserved && !splitterRemoveError &&
                    !std::filesystem::exists(splitterRoot), L"fixture deadline ms=10000; no input injection/profile writes; cleanup error=" + std::to_wstring(splitterRemoveError.value()));
            }
            check("invalid_location_returns_error", FAILED(navigate((fixture / L"does-not-exist").wstring())));
            auto previousCount = navigationCount_;
            Pidl originalSearchScope(ILCloneFull(currentPidl_.get()));
            SetWindowTextW(search_, L"filename:file-99");
            hr = execute(Search);
            const bool searchReady = SUCCEEDED(hr) && pumpUntil([&] { return !navigating_ && navigationCount_ > previousCount; }, 10000);
            check("search_folder_navigation", searchReady, currentLocation_);
            DWORD flags = 0; GetViewFlags(&flags);
            check("background_search_filtering_flag", searchReady && (flags & CDB2GVF_NOINCLUDEITEM) != 0);
            check("search_context_commands", searchReady && contextAvailable(RibbonSearchContext, true) &&
                  commandRegistered(CloseSearch) && commandRegistered(SaveSearch) && commandRegistered(SearchCurrent));
            {
                updateNamespace();
                NamespaceCommandState nativeDateParent;
                const auto parentRead = backgroundActions_.queryCommandState(L"Windows.SearchFilterDate", &nativeDateParent,
                    NamespaceMenuScope::Background);
                std::unique_ptr<NativeNamespaceCommandChildren> nativeDates;
                const auto childRead = backgroundActions_.queryCommandChildren(L"Windows.SearchFilterDate", &nativeDates,
                    NamespaceMenuScope::Background);
                std::vector<NamespaceSubcommandMetadata> independentDates;
                const auto independentRead = namespaceCommandChildren(L"Windows.SearchFilterDate", nullptr, view_.Get(), &independentDates);
                const auto hostDates = ribbonItems(SearchDateMenu);
                const auto clipboardBefore = GetClipboardSequenceNumber();
                bool exactPresets = SUCCEEDED(childRead) && nativeDates && nativeDates->entries().size() == 8 &&
                    SUCCEEDED(independentRead) && independentDates.size() == 8 && hostDates.size() == 8;
                bool guarded = exactPresets;
                std::wstring titles;
                for (size_t index = 0; exactPresets && index < 8; ++index) {
                    const auto& native = nativeDates->entries()[index];
                    const auto& independent = independentDates[index];
                    const auto& host = hostDates[index];
                    exactPresets = !native.label.empty() && native.label == independent.label && host.label == native.label &&
                        host.command == index && native.flags == ECF_DEFAULT && native.children.empty() &&
                        independent.flags == ECF_DEFAULT && independent.children.empty() &&
                        SUCCEEDED(native.stateStatus) && !(native.state & (ECS_DISABLED | ECS_HIDDEN)) &&
                        SUCCEEDED(independent.stateStatus) && independent.state == native.state &&
                        IsEqualGUID(native.canonicalName, independent.canonicalName);
                    guarded = guarded && nativeDates->invoke(index, true) == E_ACCESSDENIED;
                    titles += (titles.empty() ? L"" : L" | ") + native.label;
                }
                check("date_modified_native_parent_and_exact_eight_localized_presets", searchReady &&
                    SUCCEEDED(parentRead) && nativeDateParent.explorerCommand && nativeDateParent.enabled() &&
                    ribbonState(SearchDateMenu).enabled && exactPresets,
                    L"parent=" + hresultMessage(parentRead) + L"; children=" + hresultMessage(childRead) +
                    L"; independent=" + hresultMessage(independentRead) + L"; titles=" + titles);
                check("date_modified_native_children_reject_headless_invocation", guarded &&
                    GetClipboardSequenceNumber() == clipboardBefore,
                    L"retained native commands; no provider Invoke or custom calendar command");
            }
            execute(SelectNone);
            check("open_file_location_requires_one_result", FAILED(execute(OpenFileLocation)) &&
                  searchActive_ && ILIsEqual(searchScope_.get(), originalSearchScope.get()));
            previousCount = navigationCount_;
            hr = execute(SearchCurrent);
            ready = SUCCEEDED(hr) && pumpUntil([&] { return !navigating_ && navigationCount_ > previousCount; }, 10000);
            check("search_current_folder_retains_origin", ready && !searchRecursive_ && searchScope_ &&
                  ILIsEqual(searchScope_.get(), originalSearchScope.get()) && activeQuery_ == L"filename:file-99" &&
                  commandChecked(SearchCurrent, true) && commandChecked(SearchSubfolders, false));
            previousCount = navigationCount_;
            hr = execute(SearchSubfolders);
            ready = SUCCEEDED(hr) && pumpUntil([&] { return !navigating_ && navigationCount_ > previousCount; }, 10000);
            check("search_subfolders_retains_origin", ready && searchRecursive_ && searchScope_ &&
                  ILIsEqual(searchScope_.get(), originalSearchScope.get()) && recentSearches_.size() == 1 &&
                  commandChecked(SearchCurrent, false) && commandChecked(SearchSubfolders, true));
            const auto committedQuery = activeQuery_;
            OnNavigationPending(originalSearchScope.get());
            OnNavigationFailed(originalSearchScope.get());
            flags = 0; GetViewFlags(&flags);
            check("failed_navigation_preserves_committed_search", searchActive_ && searchBackground_ && searchRecursive_ &&
                  ILIsEqual(searchScope_.get(), originalSearchScope.get()) && activeQuery_ == committedQuery &&
                  (flags & CDB2GVF_NOINCLUDEITEM));
            previousCount = navigationCount_;
            hr = startSearch(activeQuery_, true, 2, L"System.Size:System.Size#Tiny");
            ready = SUCCEEDED(hr) && pumpUntil([&] { return !navigating_ && navigationCount_ > previousCount; }, 10000);
            previousCount = navigationCount_;
            hr = startSearch(activeQuery_, true, 2, L"System.Size:System.Size#Empty");
            ready = ready && SUCCEEDED(hr) && pumpUntil([&] { return !navigating_ && navigationCount_ > previousCount; }, 10000);
            check("search_filter_replaces_same_category", ready && searchBase_ == committedQuery &&
                  searchFilters_[2] == L"System.Size:System.Size#Empty" && activeQuery_.find(L"#Tiny") == std::wstring::npos &&
                  activeQuery_.find(L"#Empty") != std::wstring::npos && ILIsEqual(searchScope_.get(), originalSearchScope.get()));
            hr = execute(CloseSearch);
            ready = SUCCEEDED(hr) && pumpUntil([&] { return !navigating_ && atLocation(fixture); }, 5000);
            check("close_search_restores_origin_and_context", ready && !searchActive_ && contextAvailable(RibbonSearchContext, false));
            ready = pumpUntil([&] { int total = 0; return !navigating_ && folderView_ && atLocation(fixture) && SUCCEEDED(folderView_->ItemCount(SVGIO_ALLVIEW, &total)) && total == 1003; }, 5000);
            check("protected_files_remain_hidden", ready);
            execute(HiddenItems);
            ready = pumpUntil([&] { int total = 0; return !navigating_ && folderView_ && SUCCEEDED(folderView_->ItemCount(SVGIO_ALLVIEW, &total)) && total == 1002; }, 5000);
            check("hide_hidden_after_search", ready && !searchActive_);
            ComPtr<IShellItem> savedScope;
            hr = SHCreateItemFromIDList(originalSearchScope.get(), IID_PPV_ARGS(&savedScope));
            const auto savedPath = fixture / L"Subfolder" / L"Fixture.search-ms";
            if (SUCCEEDED(hr)) hr = explorer::saveSearch(L"System.FileName:=\"file-99.txt\"", savedScope.Get(), false, savedPath);
            if (SUCCEEDED(hr)) hr = navigate(savedPath.wstring());
            ready = SUCCEEDED(hr) && pumpUntil([&] { return !navigating_ && isExternalSearch(currentPidl_.get()); }, 10000);
            flags = 0; GetViewFlags(&flags);
            check("saved_search_reopens_in_hidden_host", ready && searchBackground_ && (flags & CDB2GVF_NOINCLUDEITEM), hresultMessage(hr));
            check("saved_search_restores_host_query_scope", ready && searchActive_ && searchScope_ &&
                ILIsEqual(searchScope_.get(), originalSearchScope.get()) && !searchRecursive_ && !activeQuery_.empty() &&
                textOf(search_) == activeQuery_ && contextAvailable(RibbonSearchContext, true) && commandRegistered(SaveSearch));
            previousCount = navigationCount_;
            hr = execute(SearchSubfolders);
            ready = ready && SUCCEEDED(hr) && pumpUntil([&] { return !navigating_ && navigationCount_ > previousCount; }, 5000);
            check("saved_search_context_refines_original_scope", ready && searchActive_ && searchRecursive_ && searchScope_ &&
                ILIsEqual(searchScope_.get(), originalSearchScope.get()));
            HRESULT searchCountResult = E_PENDING;
            ready = ready && pumpUntil([&] {
                int results = 0;
                if (folderView_) searchCountResult = folderView_->ItemCount(SVGIO_ALLVIEW, &results);
                return folderView_ && SUCCEEDED(searchCountResult) && results == 1;
            }, 5000);
            const bool resultEnumerated = ready;
            if (ready) hr = folderView_->SelectItem(0, SVSI_SELECT | SVSI_DESELECTOTHERS);
            if (ready && SUCCEEDED(hr)) hr = execute(OpenFileLocation);
            ready = ready && SUCCEEDED(hr) && pumpUntil([&] {
                if (navigating_ || !atLocation(fixture)) return false;
                ComPtr<IShellItemArray> chosen;
                DWORD selectedCount = 0;
                ComPtr<IShellItem> chosenItem;
                if (FAILED(selection(chosen)) || !chosen || FAILED(chosen->GetCount(&selectedCount)) ||
                    selectedCount != 1 || FAILED(chosen->GetItemAt(0, &chosenItem))) return false;
                std::error_code error;
                return std::filesystem::equivalent(std::filesystem::path(itemName(chosenItem.Get(), SIGDN_FILESYSPATH)),
                    fixture / L"file-99.txt", error) && !error;
            }, 5000);
            ComPtr<IShellItemArray> locationSelection;
            ComPtr<IShellItem> locationItem;
            DWORD locationCount = 0;
            int locationTotal = 0;
            if (folderView_) folderView_->ItemCount(SVGIO_ALLVIEW, &locationTotal);
            if (SUCCEEDED(selection(locationSelection)) && locationSelection) locationSelection->GetCount(&locationCount);
            if (locationCount == 1) locationSelection->GetItemAt(0, &locationItem);
            check("open_file_location_selects_exact_result", ready && !searchBackground_ && !selectionChild_,
                  hresultMessage(hr) + L"; location=" + currentLocation_ + L"; selected=" + std::to_wstring(locationCount) +
                  L"; item=" + (locationItem ? itemName(locationItem.Get(), SIGDN_FILESYSPATH) : L"<none>") +
                  L"; total=" + std::to_wstring(locationTotal) +
                  L"; enumerated=" + std::to_wstring(resultEnumerated) + L"; countHr=" + hresultMessage(searchCountResult) +
                  L"; ready=" + std::to_wstring(ready) + L"; nativeFolderIdentity=" + std::to_wstring(atLocation(fixture)) +
                  L"; navigating=" + std::to_wstring(navigating_) + L"; searchBackground=" + std::to_wstring(searchBackground_) +
                  L"; pending=" + std::to_wstring(selectionChild_ != nullptr));
            // Exercise the native ItemsView, rather than only round-tripping
            // metadata: one recursive include, one shallow include and a child
            // exclusion must survive import, refinement and history.
            const auto scopeA = fixture / L"Subfolder" / L"Scope A";
            const auto scopeB = fixture / L"Subfolder" / L"Scope B";
            const auto excludedScope = scopeA / L"Excluded";
            std::filesystem::create_directories(scopeA / L"Nested");
            std::filesystem::create_directories(excludedScope);
            std::filesystem::create_directories(scopeB / L"Nested");
            constexpr auto memberName = L"scope-member.txt";
            const std::array<std::filesystem::path, 5> scopeMembers{
                scopeA / memberName, scopeA / L"Nested" / memberName,
                excludedScope / memberName, scopeB / memberName,
                scopeB / L"Nested" / memberName};
            for (const auto& member : scopeMembers) std::ofstream(member) << "owned scope fixture";
            ComPtr<IShellItem> scopeAItem, scopeBItem, excludedScopeItem;
            hr = SHCreateItemFromParsingName(scopeA.c_str(), nullptr, IID_PPV_ARGS(&scopeAItem));
            if (SUCCEEDED(hr)) hr = SHCreateItemFromParsingName(scopeB.c_str(), nullptr, IID_PPV_ARGS(&scopeBItem));
            if (SUCCEEDED(hr)) hr = SHCreateItemFromParsingName(excludedScope.c_str(), nullptr, IID_PPV_ARGS(&excludedScopeItem));
            const std::vector<SearchScopeRule> originalRules{
                {scopeAItem, true, false}, {scopeBItem, false, false}, {excludedScopeItem, true, true}};
            auto sameRules = [&](const std::vector<SearchScopeRule>& observed) {
                if (observed.size() != originalRules.size()) return false;
                for (size_t i = 0; i < observed.size(); ++i) {
                    int order = 1;
                    if (!observed[i].folder || !originalRules[i].folder ||
                        observed[i].recursive != originalRules[i].recursive || observed[i].excluded != originalRules[i].excluded ||
                        FAILED(observed[i].folder->Compare(originalRules[i].folder.Get(), SICHINT_CANONICAL, &order)) || order != 0) return false;
                }
                return true;
            };
            std::array<FILE_ID_INFO, 3> expectedMembers{};
            const std::array<size_t, 3> expectedIndices{0, 1, 3};
            for (size_t i = 0; SUCCEEDED(hr) && i < expectedMembers.size(); ++i)
                hr = nativeFileIdentity(scopeMembers[expectedIndices[i]], expectedMembers[i]);
            DWORD observedMembers = 0;
            HRESULT membershipResult = E_PENDING;
            auto exactScopeMembership = [&] {
                if (navigating_ || !searchActive_ || !folderView_) return false;
                ComPtr<IShellItemArray> members;
                membershipResult = folderView_->Items(SVGIO_ALLVIEW, IID_PPV_ARGS(&members));
                if (FAILED(membershipResult) || !members || FAILED(members->GetCount(&observedMembers)) ||
                    observedMembers != expectedMembers.size()) return false;
                std::array<bool, 3> found{};
                for (DWORD i = 0; i < observedMembers; ++i) {
                    ComPtr<IShellItem> member;
                    if (FAILED(members->GetItemAt(i, &member)) || !member) return false;
                    const auto nativePath = itemName(member.Get(), SIGDN_FILESYSPATH);
                    FILE_ID_INFO memberIdentity{};
                    membershipResult = nativePath.empty() ? E_UNEXPECTED : nativeFileIdentity(nativePath, memberIdentity);
                    if (FAILED(membershipResult)) return false;
                    bool matches = false;
                    for (size_t j = 0; j < expectedMembers.size(); ++j) {
                        // Search results can have a search-namespace PIDL that
                        // wraps the physical item. Match its actual volume and
                        // 128-bit File ID, independently of paths/PIDL wrappers.
                        const bool sameIdentity = memberIdentity.VolumeSerialNumber == expectedMembers[j].VolumeSerialNumber &&
                            std::equal(std::begin(memberIdentity.FileId.Identifier), std::end(memberIdentity.FileId.Identifier),
                                std::begin(expectedMembers[j].FileId.Identifier));
                        if (sameIdentity) {
                            if (found[j]) return false;
                            found[j] = true; matches = true; break;
                        }
                    }
                    if (!matches) return false;
                }
                return std::all_of(found.begin(), found.end(), [](bool present) { return present; });
            };
            auto scopeDetail = [&](HRESULT operation) {
                return hresultMessage(operation) + L"; actual members=" + std::to_wstring(observedMembers) +
                    L"; expected members=3; Items=" + hresultMessage(membershipResult) +
                    L"; rules=" + std::to_wstring(searchScopeRules_.size());
            };
            const auto mixedSearchPath = fixture / L"Subfolder" / L"Mixed scope.search-ms";
            SearchViewPresentation importedPresentation;
            importedPresentation.mode = SearchViewMode::Details;
            importedPresentation.iconSize = 16;
            importedPresentation.visibleColumns = std::vector<std::wstring>{
                L"System.ItemNameDisplay", L"System.Size", L"System.ItemTypeText"};
            importedPresentation.groupBy = SearchViewOrder{L"System.ItemTypeText", SORT_ASCENDING};
            importedPresentation.sort = std::vector<SearchViewOrder>{{L"System.Size", SORT_DESCENDING}};
            SearchFileProperties importedProperties;
            importedProperties.author=L"  Owned 作者 🚀  "; importedProperties.kind=L"searchfolder";
            importedProperties.description=L"Owned & <metadata>\t\r\nretained"; importedProperties.tags=L"owned;資料";
            if (SUCCEEDED(hr)) hr = saveSearchForScopeRules(L"System.FileName:=\"scope-member.txt\"", originalRules,
                mixedSearchPath, SearchSaveMode::CreateNew, &importedPresentation, &importedProperties);
            if (SUCCEEDED(hr)) hr = navigate(mixedSearchPath.wstring());
            const bool mixedImported = SUCCEEDED(hr) && pumpUntil(exactScopeMembership, 5000);
            check("saved_search_import_preserves_file_properties",mixedImported&&searchFileProperties_&&*searchFileProperties_==importedProperties,
                L"All four exact owned text values imported="+std::to_wstring(searchFileProperties_&&*searchFileProperties_==importedProperties));
            check("mixed_scope_saved_search_imports_native_membership", mixedImported && sameRules(searchScopeRules_) &&
                isExternalSearch(currentPidl_.get()) && textOf(search_) == activeQuery_, scopeDetail(hr) +
                L"; membership=" + std::to_wstring(mixedImported) + L"; rules match=" + std::to_wstring(sameRules(searchScopeRules_)) +
                L"; external=" + std::to_wstring(isExternalSearch(currentPidl_.get())) +
                L"; query edit match=" + std::to_wstring(textOf(search_) == activeQuery_));
            SearchViewPresentation importedActual;
            HRESULT presentationRead = E_PENDING;
            const bool importedViewApplied = mixedImported && pumpUntil([&] {
                if (searchPresentationPending_ || FAILED(searchPresentationStatus_)) return false;
                presentationRead = captureSearchViewPresentation(folderView_.Get(), &importedActual);
                return SUCCEEDED(presentationRead) && importedActual.mode == importedPresentation.mode &&
                    importedActual.iconSize == importedPresentation.iconSize &&
                    importedActual.visibleColumns == importedPresentation.visibleColumns &&
                    importedActual.groupBy && importedActual.groupBy->property == importedPresentation.groupBy->property &&
                    importedActual.groupBy->direction == importedPresentation.groupBy->direction &&
                    importedActual.sort && importedActual.sort->size() == 1 &&
                    (*importedActual.sort)[0].property == L"System.Size" &&
                    (*importedActual.sort)[0].direction == SORT_DESCENDING;
            }, 2000);
            check("saved_search_import_applies_native_presentation", importedViewApplied,
                L"Deferred status=" + hresultMessage(searchPresentationStatus_) + L"; actual view read=" +
                hresultMessage(presentationRead) + L"; native Details16/columns/group/sort=" +
                std::to_wstring(importedViewApplied ? 1 : 0));
            {
                // A real view edit after import must survive browser recreation;
                // the imported XML remains an independent, older presentation.
                const auto paneQuery=activeQuery_,paneBase=searchBase_;
                const auto paneFilters=searchFilters_;
                const auto paneRecent=recentSearches_;
                const auto paneRecursive=searchRecursive_;
                const auto paneHistoryIndex=historyIndex_;
                const auto paneNavigation=preferences_.navigationPane;
                const auto panePreview=preferences_.previewPane;
                const auto paneDetails=preferences_.detailsPane;
                const auto paneHidden=preferences_.showHidden;
                const auto readPaneBytes=[](const std::filesystem::path& path) {
                    std::ifstream input(path,std::ios::binary);
                    return input?std::string(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()):std::string{};
                };
                const auto paneSourceBytes=readPaneBytes(mixedSearchPath);
                FILE_ID_INFO paneSourceId{};
                const auto paneSourceRead=nativeFileIdentity(mixedSearchPath,paneSourceId);
                Pidl paneIdentity(mixedImported?ILCloneFull(currentPidl_.get()):nullptr);
                Pidl paneScope(searchScope_?ILCloneFull(searchScope_.get()):nullptr);
                std::vector<Pidl> paneHistory;
                bool paneHistoryCopied=true;
                for(const auto& entry:history_) {
                    Pidl copy(entry?ILCloneFull(entry.get()):nullptr);
                    if(entry&&!copy){paneHistoryCopied=false;break;}
                    paneHistory.push_back(std::move(copy));
                }
                const auto samePanePresentation=[](const SearchViewPresentation& first,const SearchViewPresentation& second) {
                    const auto sameOrder=[](const SearchViewOrder& a,const SearchViewOrder& b) {
                        return a.property==b.property&&a.direction==b.direction;
                    };
                    if(first.mode!=second.mode||first.iconSize!=second.iconSize||first.visibleColumns!=second.visibleColumns||
                       first.groupBy.has_value()!=second.groupBy.has_value()||first.sort.has_value()!=second.sort.has_value())return false;
                    if(first.groupBy&&!sameOrder(*first.groupBy,*second.groupBy))return false;
                    if(first.sort) {
                        if(first.sort->size()!=second.sort->size())return false;
                        for(size_t index=0;index<first.sort->size();++index)
                            if(!sameOrder((*first.sort)[index],(*second.sort)[index]))return false;
                    }
                    return true;
                };
                const auto samePaneHistory=[&] {
                    if(!paneHistoryCopied||historyIndex_!=paneHistoryIndex||history_.size()!=paneHistory.size())return false;
                    for(size_t index=0;index<paneHistory.size();++index)
                        if(!ILIsEqual(history_[index].get(),paneHistory[index].get()))return false;
                    return true;
                };
                SearchViewPresentation paneDesired=importedPresentation;
                paneDesired.mode=SearchViewMode::Content;paneDesired.iconSize=32;
                paneDesired.visibleColumns=std::vector<std::wstring>{L"System.ItemNameDisplay",L"System.DateModified",L"System.Size"};
                paneDesired.groupBy=SearchViewOrder{L"System.ItemNameDisplay",SORT_DESCENDING};
                paneDesired.sort=std::vector<SearchViewOrder>{{L"System.DateModified",SORT_ASCENDING}};
                auto paneSetup=importedViewApplied?applySearchViewPresentation(folderView_.Get(),paneDesired):E_UNEXPECTED;
                SearchViewPresentation paneActual;
                if(SUCCEEDED(paneSetup))paneSetup=captureSearchViewPresentation(folderView_.Get(),&paneActual);
                bool paneSetupReady=SUCCEEDED(paneSetup)&&samePanePresentation(paneActual,paneDesired)&&
                    !samePanePresentation(paneActual,importedActual)&&paneIdentity&&paneScope&&paneHistoryCopied&&!paneSourceBytes.empty();
                check("search_pane_recreation_native_presentation_setup",paneSetupReady,hresultMessage(paneSetup));
                // Failed native capture must precede every preference mutation.
                const auto beforeCapturePresentation=searchPresentation_;
                const auto beforeCaptureContexts=searchLocations_.size(),beforeCaptureViews=searchPresentationLocations_.size();
                const auto retainedFolderView=folderView_;
                folderView_.Reset();
                const auto paneCaptureFailure=paneSetupReady?execute(PreviewPane):E_UNEXPECTED;
                folderView_=retainedFolderView;
                check("search_pane_capture_failure_preserves_view_and_preferences",paneSetupReady&&
                    paneCaptureFailure==E_UNEXPECTED&&preferences_.navigationPane==paneNavigation&&
                    preferences_.previewPane==panePreview&&preferences_.detailsPane==paneDetails&&preferences_.showHidden==paneHidden&&
                    currentPidl_&&ILIsEqual(currentPidl_.get(),paneIdentity.get())&&samePaneHistory()&&
                    searchLocations_.size()==beforeCaptureContexts&&searchPresentationLocations_.size()==beforeCaptureViews&&
                    searchPresentation_.has_value()==beforeCapturePresentation.has_value()&&
                    (!searchPresentation_||samePanePresentation(*searchPresentation_,*beforeCapturePresentation)),hresultMessage(paneCaptureFailure));
                for(const auto paneCommand:std::array<UINT,4>{NavigationPane,PreviewPane,DetailsPane,HiddenItems}) {
                    const auto paneGeometryDeadline=GetTickCount64()+5000;
                    const auto beforePaneGeometry=sampleCurrentPaneGeometry(paneGeometryDeadline);
                    const auto beforePaneNavigation=navigationCount_;
                    const auto paneOperation=paneSetupReady?execute(paneCommand):E_UNEXPECTED;
                    SearchViewPresentation recreatedPresentation;
                    HRESULT recreatedRead=E_PENDING;
                    const auto paneGeometryNow=GetTickCount64();
                    const bool paneReady=SUCCEEDED(paneOperation)&&paneGeometryNow<paneGeometryDeadline&&pumpUntil([&] {
                        if(navigating_||navigationCount_<=beforePaneNavigation||!folderView_||searchPresentationPending_||
                           FAILED(searchPresentationStatus_)||!exactScopeMembership())return false;
                        recreatedRead=captureSearchViewPresentation(folderView_.Get(),&recreatedPresentation);
                        return SUCCEEDED(recreatedRead)&&samePanePresentation(recreatedPresentation,paneActual);
                    },static_cast<DWORD>(paneGeometryDeadline-paneGeometryNow));
                    const auto paneCheckName="search_pane_recreation_preserves_native_view_"+std::to_string(paneCommand);
                    check(paneCheckName.c_str(),paneReady&&
                        currentPidl_&&ILIsEqual(currentPidl_.get(),paneIdentity.get())&&searchScope_&&ILIsEqual(searchScope_.get(),paneScope.get())&&
                        activeQuery_==paneQuery&&searchBase_==paneBase&&searchFilters_==paneFilters&&searchRecursive_==paneRecursive&&
                        sameRules(searchScopeRules_)&&searchFileProperties_&&*searchFileProperties_==importedProperties&&
                        recentSearches_==paneRecent&&samePaneHistory(),hresultMessage(paneOperation)+L"; view="+hresultMessage(recreatedRead));
                    const auto recreatedGeometry=sampleCurrentPaneGeometry(paneGeometryDeadline);
                    const auto paneGeometryCheck="search_native_pane_recreation_preserves_full_footer_and_content_slot_"+std::to_string(paneCommand);
                    check(paneGeometryCheck.c_str(),paneReady&&beforePaneGeometry.read==S_OK&&beforePaneGeometry.footerFull&&beforePaneGeometry.partition&&
                        recreatedGeometry.read==S_OK&&recreatedGeometry.footerFull&&recreatedGeometry.partition&&
                        EqualRect(&beforePaneGeometry.frameClient,&recreatedGeometry.frameClient)&&
                        EqualRect(&beforePaneGeometry.footer,&recreatedGeometry.footer)&&currentPidl_&&ILIsEqual(currentPidl_.get(),paneIdentity.get())&&
                        exactScopeMembership()&&samePaneHistory(),L"before="+paneGeometryFacts(beforePaneGeometry)+L"; after="+paneGeometryFacts(recreatedGeometry));
                    paneSetupReady=paneSetupReady&&paneReady;
                }
                // Restore only the preferences this fixture changed, through the
                // same actual commands. Details/Preview are mutually exclusive.
                std::vector<UINT> restorePaneCommands;
                if(preferences_.navigationPane!=paneNavigation)restorePaneCommands.push_back(NavigationPane);
                if(preferences_.showHidden!=paneHidden)restorePaneCommands.push_back(HiddenItems);
                if(preferences_.previewPane!=panePreview||preferences_.detailsPane!=paneDetails)
                    restorePaneCommands.push_back(panePreview?PreviewPane:paneDetails?DetailsPane:
                        preferences_.previewPane?PreviewPane:DetailsPane);
                bool paneRestored=paneSetupReady;
                for(const auto paneCommand:restorePaneCommands) {
                    const auto beforeRestore=navigationCount_;
                    const auto restored=execute(paneCommand);
                    paneRestored=SUCCEEDED(restored)&&pumpUntil([&] {
                        return !navigating_&&navigationCount_>beforeRestore&&folderView_&&!searchPresentationPending_&&exactScopeMembership();
                    },5000)&&paneRestored;
                }
                auto originalLayout=paneRestored?applySearchViewPresentation(folderView_.Get(),importedActual):E_UNEXPECTED;
                if(SUCCEEDED(originalLayout))originalLayout=captureActiveSearchPresentation();
                const bool immediatePaneMembership=exactScopeMembership();
                SearchViewPresentation restoredPanePresentation;
                HRESULT restoredPaneRead=E_PENDING;
                const bool originalPaneReady=SUCCEEDED(originalLayout)&&pumpUntil([&] {
                    if(navigating_||!folderView_||searchPresentationPending_||FAILED(searchPresentationStatus_))return false;
                    restoredPaneRead=captureSearchViewPresentation(folderView_.Get(),&restoredPanePresentation);
                    return SUCCEEDED(restoredPaneRead)&&samePanePresentation(restoredPanePresentation,importedActual)&&exactScopeMembership();
                },5000);
                FILE_ID_INFO paneSourceAfter{};
                const auto paneSourceAfterRead=nativeFileIdentity(mixedSearchPath,paneSourceAfter);
                const bool restoredPanePreferences=preferences_.navigationPane==paneNavigation&&preferences_.previewPane==panePreview&&
                    preferences_.detailsPane==paneDetails&&preferences_.showHidden==paneHidden;
                const bool restoredPaneHistory=samePaneHistory();
                const bool restoredPaneSourceIdentity=SUCCEEDED(paneSourceRead)&&SUCCEEDED(paneSourceAfterRead)&&
                    paneSourceId.VolumeSerialNumber==paneSourceAfter.VolumeSerialNumber&&
                    std::equal(std::begin(paneSourceId.FileId.Identifier),std::end(paneSourceId.FileId.Identifier),std::begin(paneSourceAfter.FileId.Identifier));
                const bool restoredPaneSourceBytes=readPaneBytes(mixedSearchPath)==paneSourceBytes;
                const bool restoredPaneQuery=activeQuery_==paneQuery&&searchBase_==paneBase&&searchFilters_==paneFilters&&searchRecursive_==paneRecursive;
                const bool restoredPaneRules=sameRules(searchScopeRules_);
                const bool restoredPaneProperties=searchFileProperties_&&*searchFileProperties_==importedProperties;
                const bool restoredPaneRecent=recentSearches_==paneRecent;
                const bool restoredPaneIdentity=currentPidl_&&ILIsEqual(currentPidl_.get(),paneIdentity.get())&&
                    searchScope_&&ILIsEqual(searchScope_.get(),paneScope.get());
                const bool restoredPaneMembers=exactScopeMembership();
                check("search_pane_recreation_restores_fixture_and_preserves_source",paneRestored&&originalPaneReady&&
                    restoredPanePreferences&&restoredPaneHistory&&restoredPaneSourceIdentity&&restoredPaneSourceBytes&&
                    restoredPaneQuery&&restoredPaneRules&&restoredPaneProperties&&restoredPaneRecent&&restoredPaneIdentity&&restoredPaneMembers,
                    hresultMessage(originalLayout)+L"; native presentation read="+hresultMessage(restoredPaneRead)+
                    L"; restored commands="+std::to_wstring(paneRestored)+L"; original layout="+std::to_wstring(originalPaneReady)+
                    L"; preferences="+std::to_wstring(restoredPanePreferences)+L"; history="+std::to_wstring(restoredPaneHistory)+
                    L"; source ID="+std::to_wstring(restoredPaneSourceIdentity)+L"; source bytes="+std::to_wstring(restoredPaneSourceBytes)+
                    L"; query="+std::to_wstring(restoredPaneQuery)+L"; rules="+std::to_wstring(restoredPaneRules)+
                    L"; properties="+std::to_wstring(restoredPaneProperties)+L"; MRU="+std::to_wstring(restoredPaneRecent)+
                    L"; current/scope identity="+std::to_wstring(restoredPaneIdentity)+
                    L"; immediate members="+std::to_wstring(immediatePaneMembership)+L"; settled members="+std::to_wstring(restoredPaneMembers));
            }
            Pidl importedMixedPidl(mixedImported ? ILCloneFull(currentPidl_.get()) : nullptr);
            const auto contentChanged = mixedImported ? setView(ViewMode::Content) : E_UNEXPECTED;
            auto nativeContent32 = [&] {
                FOLDERVIEWMODE mode = FVM_AUTO; int size = 0;
                return !searchPresentationPending_ && SUCCEEDED(searchPresentationStatus_) && folderView_ &&
                    SUCCEEDED(folderView_->GetViewModeAndIconSize(&mode, &size)) && mode == FVM_CONTENT && size == 32;
            };
            const bool contentSelected = SUCCEEDED(contentChanged) && pumpUntil(nativeContent32, 2000);
            const auto externalPresentationCaptured = contentSelected ? captureActiveSearchPresentation() : E_UNEXPECTED;
            const auto externalCachePresentation = searchPresentation_.value_or(SearchViewPresentation{});
            previousCount = navigationCount_;
            hr = mixedImported ? startSearch(activeQuery_, searchRecursive_, 2, L"System.Size:System.Size#Tiny") : E_UNEXPECTED;
            const bool mixedRefined = SUCCEEDED(hr) && pumpUntil([&] {
                return navigationCount_ > previousCount && exactScopeMembership();
            }, 5000);
            const bool propertiesRefined=mixedRefined&&searchFileProperties_&&*searchFileProperties_==importedProperties;
            check("mixed_scope_refinement_preserves_native_membership", mixedRefined && sameRules(searchScopeRules_) &&
                activeQuery_.find(L"#Tiny") != std::wstring::npos, scopeDetail(hr));
            const bool contentRefined = mixedRefined && pumpUntil(nativeContent32, 2000);
            const auto refinedMixedQuery = activeQuery_;
            Pidl refinedMixedPidl(ILCloneFull(currentPidl_.get()));
            Pidl refinedMixedScope(ILCloneFull(searchScope_.get()));
            const auto preparedCacheResult = contentRefined ? captureActiveSearchPresentation() : E_UNEXPECTED;
            hr = navigate(fixture.wstring());
            const bool leftMixedSearch = SUCCEEDED(hr) && pumpUntil([&] { return !navigating_ && atLocation(fixture); }, 5000);
            const bool propertiesCleared=leftMixedSearch&&!searchFileProperties_;
            {
                // A live query replaces one history slot while its factory
                // contexts accumulate. Exercise the real native identities
                // without opening/enumerating 120 additional ItemsViews.
                const auto cacheNavigationCount = navigationCount_;
                const auto cacheHistorySize = history_.size();
                const auto cacheHistoryIndex = historyIndex_;
                HRESULT cacheResult = leftMixedSearch ? preparedCacheResult : E_UNEXPECTED;
                Pidl cacheCurrentLocation(ILCloneFull(currentPidl_.get()));
                const auto exactBytes = [](PCIDLIST_ABSOLUTE left, PCIDLIST_ABSOLUTE right) {
                    if (!left || !right) return false;
                    const auto bytes = ILGetSize(left);
                    return bytes == ILGetSize(right) && std::memcmp(left, right, bytes) == 0;
                };
                const auto originalContext = std::find_if(searchLocations_.rbegin(), searchLocations_.rend(),
                    [&](const SearchLocation& entry) { return refinedMixedPidl && ILIsEqual(entry.location.get(), refinedMixedPidl.get()); });
                Pidl originalFactoryIdentity(originalContext != searchLocations_.rend() ? ILCloneFull(originalContext->location.get()) : nullptr);
                const bool factoryCanonicalMatch = originalFactoryIdentity && refinedMixedPidl &&
                    ILIsEqual(originalFactoryIdentity.get(), refinedMixedPidl.get());
                const bool factoryBytesDiffer = factoryCanonicalMatch && !exactBytes(originalFactoryIdentity.get(), refinedMixedPidl.get());
                std::fprintf(stderr, "headless-cache-native factory_bytes=%u completed_bytes=%u canonical_equal=%u bytes_differ=%u\n",
                    ILGetSize(originalFactoryIdentity.get()), ILGetSize(refinedMixedPidl.get()), factoryCanonicalMatch ? 1U : 0U,
                    factoryBytesDiffer ? 1U : 0U);
                std::fflush(stderr);
                PIDLIST_ABSOLUTE scopeRaw = nullptr;
                if (SUCCEEDED(cacheResult)) cacheResult = SHGetIDListFromObject(scopeAItem.Get(), &scopeRaw);
                Pidl cacheScope(scopeRaw), firstFactory, latestFactory;
                ComPtr<IShellItemArray> cacheScopes;
                if (SUCCEEDED(cacheResult)) cacheResult = SHCreateShellItemArrayFromShellItem(scopeAItem.Get(), IID_PPV_ARGS(&cacheScopes));
                ComPtr<IConditionFactory2> cacheConditionFactory;
                if (SUCCEEDED(cacheResult)) cacheResult = CoCreateInstance(__uuidof(ConditionFactory), nullptr, CLSCTX_INPROC_SERVER,
                    IID_PPV_ARGS(&cacheConditionFactory));
                ComPtr<ISearchFolderItemFactory> cacheFolderFactory;
                if (SUCCEEDED(cacheResult)) cacheResult = CoCreateInstance(CLSID_SearchFolderItemFactory, nullptr, CLSCTX_INPROC_SERVER,
                    IID_PPV_ARGS(&cacheFolderFactory));
                if (SUCCEEDED(cacheResult)) cacheResult = cacheFolderFactory->SetScope(cacheScopes.Get());
                if (SUCCEEDED(cacheResult)) cacheResult = cacheFolderFactory->SetFolderTypeID(FOLDERTYPEID_GenericSearchResults);
                if (SUCCEEDED(cacheResult)) cacheResult = cacheFolderFactory->SetFolderLogicalViewMode(FLVM_DETAILS);
                const std::vector<SearchScopeRule> cacheRules{{scopeAItem, true, false}};
                size_t createdFactories = 0;
                bool distinctFactories = true;
                std::vector<Pidl> factoryIdentities;
                const auto cacheFactoryStarted = GetTickCount64();
                for (size_t index = 0; SUCCEEDED(cacheResult) && index < 120; ++index) {
                    const auto cacheQuery = L"System.Size:=" + std::to_wstring(1000 + index);
                    ComPtr<ICondition> cacheCondition;
                    cacheResult = cacheConditionFactory->CreateIntegerLeaf(PKEY_Size, COP_EQUAL, static_cast<INT32>(1000 + index),
                        CONDITION_CREATION_DEFAULT, IID_PPV_ARGS(&cacheCondition));
                    if (SUCCEEDED(cacheResult)) cacheResult = cacheFolderFactory->SetCondition(cacheCondition.Get());
                    if (SUCCEEDED(cacheResult)) cacheResult = cacheFolderFactory->SetDisplayName(cacheQuery.c_str());
                    ComPtr<IShellItem> cacheResults;
                    if (SUCCEEDED(cacheResult)) cacheResult = cacheFolderFactory->GetShellItem(IID_PPV_ARGS(&cacheResults));
                    PIDLIST_ABSOLUTE raw = nullptr;
                    if (SUCCEEDED(cacheResult)) cacheResult = SHGetIDListFromObject(cacheResults.Get(), &raw);
                    Pidl location(raw);
                    if (FAILED(cacheResult)) break;
                    Pidl scopeCopy(ILCloneFull(cacheScope.get())), identityCopy(ILCloneFull(location.get()));
                    Pidl presentationLocation(ILCloneFull(location.get()));
                    if (!location || !scopeCopy || !identityCopy || !presentationLocation) { cacheResult = E_OUTOFMEMORY; break; }
                    distinctFactories = distinctFactories && std::none_of(factoryIdentities.begin(), factoryIdentities.end(),
                        [&](const Pidl& identity) { return ILIsEqual(identity.get(), location.get()); });
                    factoryIdentities.push_back(std::move(identityCopy));
                    if (index == 0) firstFactory.reset(ILCloneFull(location.get()));
                    if (index == 119) latestFactory.reset(ILCloneFull(location.get()));
                    SearchFileProperties properties; properties.description = L"earlier owned factory metadata";
                    searchLocations_.push_back({std::move(location), std::move(scopeCopy), cacheQuery, true, cacheQuery, {},
                        cacheScopes, cacheRules, {}, properties, false, false});
                    searchPresentationLocations_.push_back({std::move(presentationLocation), externalCachePresentation});
                    pruneSearchCaches();
                    ++createdFactories;
                }
                const auto cacheFactoryDuration = GetTickCount64() - cacheFactoryStarted;
                const auto retained = std::find_if(searchLocations_.rbegin(), searchLocations_.rend(),
                    [&](const SearchLocation& context) { return refinedMixedPidl && ILIsEqual(context.location.get(), refinedMixedPidl.get()); });
                const bool retainedMetadata = retained != searchLocations_.rend() && retained->query == refinedMixedQuery &&
                    retained->scope && refinedMixedScope && ILIsEqual(retained->scope.get(), refinedMixedScope.get()) &&
                    sameRules(retained->scopeRules) && retained->fileProperties && *retained->fileProperties == importedProperties &&
                    retained->presentation && retained->presentation->mode == SearchViewMode::Content && retained->presentation->iconSize == 32;
                const bool nativeCompletionAliasRetained = retained != searchLocations_.rend() &&
                    exactBytes(retained->location.get(), originalFactoryIdentity.get()) &&
                    exactBytes(retained->completedLocation.get(), refinedMixedPidl.get()) &&
                    std::any_of(history_.begin(), history_.end(), [&](const Pidl& entry) {
                        return exactBytes(retained->historyLocation.get(), entry.get());
                    });
                const bool oldestObsoleteRemoved = firstFactory && std::none_of(searchLocations_.begin(), searchLocations_.end(),
                    [&](const SearchLocation& context) { return ILIsEqual(context.location.get(), firstFactory.get()); });
                check("live_search_cache_prunes_obsolete_factories_and_retains_history_metadata", SUCCEEDED(cacheResult) &&
                    createdFactories == 120 && distinctFactories && retainedMetadata && oldestObsoleteRemoved &&
                    searchLocations_.size() == 100 && navigationCount_ == cacheNavigationCount &&
                    history_.size() == cacheHistorySize && historyIndex_ == cacheHistoryIndex &&
                    currentPidl_ && cacheCurrentLocation && ILIsEqual(currentPidl_.get(), cacheCurrentLocation.get()),
                    L"Native factory creation=" + hresultMessage(cacheResult) + L"; distinct factories=" + std::to_wstring(createdFactories) +
                    L"; native factory elapsed ms=" + std::to_wstring(cacheFactoryDuration) +
                    L"; retained contexts=" + std::to_wstring(searchLocations_.size()) + L"; pinned scope/rules/properties/presentation=" +
                    std::to_wstring(retainedMetadata));
                check("live_search_native_completion_alias_pins_canonical_factory_context", factoryCanonicalMatch &&
                    nativeCompletionAliasRetained && retainedMetadata,
                    L"Canonical native equality=" + std::to_wstring(factoryCanonicalMatch) +
                    L"; factory/completed bytes differ=" + std::to_wstring(factoryBytesDiffer) +
                    L"; exact completed and travel aliases retained=" + std::to_wstring(nativeCompletionAliasRetained));
                const auto externalPresentation = std::find_if(searchPresentationLocations_.rbegin(), searchPresentationLocations_.rend(),
                    [&](const SearchPresentationLocation& entry) { return importedMixedPidl && ILIsEqual(entry.location.get(), importedMixedPidl.get()); });
                const bool externalHistoryOwned = importedMixedPidl && std::any_of(history_.begin(), history_.end(),
                    [&](const Pidl& entry) { return ILIsEqual(entry.get(), importedMixedPidl.get()); });
                const bool externalHasNoFactoryContext = importedMixedPidl && std::none_of(searchLocations_.begin(), searchLocations_.end(),
                    [&](const SearchLocation& entry) { return ILIsEqual(entry.location.get(), importedMixedPidl.get()); });
                bool externalSnapshotRetained = externalPresentation != searchPresentationLocations_.rend();
                if (externalSnapshotRetained) {
                    const auto& actual = externalPresentation->presentation;
                    externalSnapshotRetained = actual.mode == externalCachePresentation.mode && actual.iconSize == externalCachePresentation.iconSize &&
                        actual.visibleColumns == externalCachePresentation.visibleColumns && actual.groupBy.has_value() == externalCachePresentation.groupBy.has_value() &&
                        actual.sort.has_value() == externalCachePresentation.sort.has_value();
                    if (externalSnapshotRetained && actual.groupBy) externalSnapshotRetained =
                        actual.groupBy->property == externalCachePresentation.groupBy->property && actual.groupBy->direction == externalCachePresentation.groupBy->direction;
                    if (externalSnapshotRetained && actual.sort) {
                        externalSnapshotRetained = actual.sort->size() == externalCachePresentation.sort->size();
                        for (size_t index = 0; externalSnapshotRetained && index < actual.sort->size(); ++index)
                            externalSnapshotRetained = (*actual.sort)[index].property == (*externalCachePresentation.sort)[index].property &&
                                (*actual.sort)[index].direction == (*externalCachePresentation.sort)[index].direction;
                    }
                }
                check("live_search_cache_retains_external_saved_view_presentation_by_history_identity", SUCCEEDED(cacheResult) &&
                    SUCCEEDED(externalPresentationCaptured) && createdFactories == 120 && externalHistoryOwned && externalHasNoFactoryContext &&
                    externalSnapshotRetained && externalCachePresentation.mode == SearchViewMode::Content && externalCachePresentation.iconSize == 32 &&
                    searchPresentationLocations_.size() == 100 && history_.size() == cacheHistorySize && historyIndex_ == cacheHistoryIndex,
                    L"Actual external Content32 capture=" + hresultMessage(externalPresentationCaptured) +
                    L"; exact owned history identity=" + std::to_wstring(externalHistoryOwned) +
                    L"; presentation contexts=" + std::to_wstring(searchPresentationLocations_.size()));
                bool latestMetadataRetained = false;
                if (SUCCEEDED(cacheResult) && latestFactory && !searchLocations_.empty() &&
                    ILIsEqual(searchLocations_.back().location.get(), latestFactory.get())) {
                    const auto& latest = searchLocations_.back();
                    SearchFileProperties properties; properties.description = L"latest owned factory metadata";
                    SearchLocation replacement{Pidl(ILCloneFull(latest.location.get())), Pidl(ILCloneFull(latest.scope.get())),
                        latest.query, latest.recursive, latest.base, latest.filters, latest.scopes, latest.scopeRules,
                        latest.presentation, properties, false, false};
                    if (replacement.location && replacement.scope) {
                        searchLocations_.push_back(std::move(replacement));
                        pruneSearchCaches();
                        const auto duplicates = std::count_if(searchLocations_.begin(), searchLocations_.end(),
                            [&](const SearchLocation& context) { return ILIsEqual(context.location.get(), latestFactory.get()); });
                        latestMetadataRetained = duplicates == 1 && searchLocations_.back().fileProperties &&
                            *searchLocations_.back().fileProperties == properties;
                    }
                }
                check("live_search_cache_deduplicates_exact_native_identity_with_latest_metadata", latestMetadataRetained &&
                    searchLocations_.size() == 100 && navigationCount_ == cacheNavigationCount);
            }
            const auto backExactBytes=[](PCIDLIST_ABSOLUTE left,PCIDLIST_ABSOLUTE right) {
                if(!left||!right)return false;
                const auto length=ILGetSize(left);
                return length==ILGetSize(right)&&std::memcmp(left,right,length)==0;
            };
            hr = mixedRefined && leftMixedSearch ? execute(Back) : E_UNEXPECTED;
            const bool mixedRestored = SUCCEEDED(hr) && pumpUntil(exactScopeMembership, 5000);
            check("saved_search_file_properties_refinement_leave_and_history",propertiesRefined&&propertiesCleared&&mixedRestored&&
                searchFileProperties_&&*searchFileProperties_==importedProperties,
                L"Exact metadata retained on refinement, cleared on physical navigation and restored by Back");
            check("mixed_scope_history_restores_query_rules_and_membership", mixedRestored &&
                sameRules(searchScopeRules_) && activeQuery_ == refinedMixedQuery &&
                ILIsEqual(currentPidl_.get(), refinedMixedPidl.get()), scopeDetail(hr)+
                    L"; navigating="+std::to_wstring(navigating_)+L"; search active/background="+
                    std::to_wstring(searchActive_)+L"/"+std::to_wstring(searchBackground_)+
                    L"; pending/history index="+std::to_wstring(pendingHistory_)+L"/"+std::to_wstring(historyIndex_)+
                    L"; current/refined native identity="+std::to_wstring(currentPidl_&&refinedMixedPidl&&
                        ILIsEqual(currentPidl_.get(),refinedMixedPidl.get()))+
                    L"; retained exact factory/completed/history alias counts="+
                    std::to_wstring(std::count_if(searchLocations_.begin(),searchLocations_.end(),[&](const SearchLocation& value){
                        return backExactBytes(value.location.get(),currentPidl_.get());}))+L"/"+
                    std::to_wstring(std::count_if(searchLocations_.begin(),searchLocations_.end(),[&](const SearchLocation& value){
                        return backExactBytes(value.completedLocation.get(),currentPidl_.get());}))+L"/"+
                    std::to_wstring(std::count_if(searchLocations_.begin(),searchLocations_.end(),[&](const SearchLocation& value){
                        return backExactBytes(value.historyLocation.get(),currentPidl_.get());})));
            const bool contentRestored = mixedRestored && pumpUntil(nativeContent32, 2000);
            check("mixed_scope_content_view_survives_refinement_and_history", contentSelected && contentRefined && contentRestored &&
                searchPresentation_ && searchPresentation_->mode == SearchViewMode::Content && searchPresentation_->iconSize == 32,
                L"Content change=" + hresultMessage(contentChanged) + L"; native Content32 selected/refined/restored=" +
                std::to_wstring(contentSelected ? 1 : 0) + L"/" + std::to_wstring(contentRefined ? 1 : 0) + L"/" +
                std::to_wstring(contentRestored ? 1 : 0));
            const auto mixedRoundtripPath = fixture / L"Subfolder" / L"Mixed scope roundtrip.search-ms";
            const auto capturedPresentation = mixedRestored ? captureActiveSearchPresentation() : E_UNEXPECTED;
            SearchViewPresentation projectedPresentation;
            const auto projectionResult = SUCCEEDED(capturedPresentation) && searchPresentation_ ?
                nativeSearchViewPresentation(*searchPresentation_, &projectedPresentation) : E_UNEXPECTED;
            hr = SUCCEEDED(projectionResult) ? saveSearchForScopeRules(activeQuery_, searchScopeRules_, mixedRoundtripPath,
                SearchSaveMode::CreateNew, &projectedPresentation,searchFileProperties_?&*searchFileProperties_:nullptr) : projectionResult;
            SavedSearchMetadata mixedMetadata;
            if (SUCCEEDED(hr)) hr = readSavedSearch(mixedRoundtripPath, &mixedMetadata);
            const bool savedRules = SUCCEEDED(hr) && sameRules(mixedMetadata.scopeRules) && !mixedMetadata.query.empty();
            if (savedRules) hr = navigate(mixedRoundtripPath.wstring());
            const bool roundtripReady = savedRules && SUCCEEDED(hr) && pumpUntil(exactScopeMembership, 5000);
            check("saved_search_file_properties_native_roundtrip",roundtripReady&&mixedMetadata.fileProperties&&
                *mixedMetadata.fileProperties==importedProperties&&searchFileProperties_&&*searchFileProperties_==importedProperties,
                L"Refined saved file and recreated native host preserve all original file property text");
            check("mixed_scope_save_reopens_with_native_membership", roundtripReady && sameRules(searchScopeRules_) &&
                isExternalSearch(currentPidl_.get()), scopeDetail(hr));
            check("saved_search_public_projection_retains_native_results", SUCCEEDED(projectionResult) &&
                !projectedPresentation.mode && projectedPresentation.iconSize == 32 && savedRules &&
                mixedMetadata.presentation && !mixedMetadata.presentation->mode && mixedMetadata.presentation->iconSize == 32 &&
                roundtripReady && activeQuery_ == mixedMetadata.query && exactScopeMembership(),
                L"Projection=" + hresultMessage(projectionResult) + L"; public mode omitted=" +
                std::to_wstring(!projectedPresentation.mode ? 1 : 0) + L"; native query/File IDs preserved=" +
                std::to_wstring(roundtripReady ? 1 : 0));
            // Keep actual mixed-scope FileIDs, metadata and native presentation
            // when the installed saved-folder loader rejects a long filename.
            auto longSavedDirectory=fixture/L"Subfolder";
            for(unsigned index=0;index<5;++index)
                longSavedDirectory/=L"Owned saved 日本語 🚀 " + std::to_wstring(index) + std::wstring(28,L'x');
            const auto extended=[](const std::filesystem::path& value) {
                const auto& name=value.native();
                return std::filesystem::path(name.starts_with(L"\\\\?\\")?name:
                    name.starts_with(L"\\\\")?L"\\\\?\\UNC\\"+name.substr(2):L"\\\\?\\"+name);
            };
            const auto longSavedPath=longSavedDirectory/L"Owned saved query 資料.search-ms";
            const auto samePresentation=[](const SearchViewPresentation& first,const SearchViewPresentation& second) {
                const auto sameOrder=[](const SearchViewOrder& a,const SearchViewOrder& b) {
                    return a.property==b.property&&a.direction==b.direction;
                };
                if(first.mode!=second.mode||first.iconSize!=second.iconSize||first.visibleColumns!=second.visibleColumns||
                   first.groupBy.has_value()!=second.groupBy.has_value()||first.sort.has_value()!=second.sort.has_value())return false;
                if(first.groupBy&&!sameOrder(*first.groupBy,*second.groupBy))return false;
                if(first.sort) {
                    if(first.sort->size()!=second.sort->size())return false;
                    for(size_t index=0;index<first.sort->size();++index)
                        if(!sameOrder((*first.sort)[index],(*second.sort)[index]))return false;
                }
                return true;
            };
            const auto longUnsupportedPath=longSavedDirectory/L"Unsupported owned query.search-ms";
            std::error_code longSavedError;
            std::filesystem::create_directories(extended(longSavedDirectory),longSavedError);
            auto longOpen=longSavedError?HRESULT_FROM_WIN32(static_cast<DWORD>(longSavedError.value())):
                saveSearchForScopeRules(L"System.FileName:=\"scope-member.txt\"",originalRules,extended(longSavedPath),
                    SearchSaveMode::CreateNew,&importedPresentation,&importedProperties);
            const auto readLongBytes=[&](const std::filesystem::path& value) {
                std::ifstream input(extended(value),std::ios::binary);
                return input?std::string(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()):std::string{};
            };
            const auto longSavedBytes=SUCCEEDED(longOpen)?readLongBytes(longSavedPath):std::string{};
            FILE_ID_INFO longSavedId{};
            if(SUCCEEDED(longOpen))longOpen=nativeFileIdentity(extended(longSavedPath),longSavedId);
            ComPtr<IShellItem> longSavedItem;
            if(SUCCEEDED(longOpen))longOpen=SHCreateItemFromParsingName(longSavedPath.c_str(),nullptr,IID_PPV_ARGS(&longSavedItem));
            ComPtr<IShellFolder> nativeLongSavedFolder;
            const auto longNativeBind=longSavedItem?longSavedItem->BindToHandler(nullptr,BHID_SFObject,IID_PPV_ARGS(&nativeLongSavedFolder)):E_UNEXPECTED;
            const auto originalAddresses=typedAddresses_;
            const bool originalHistoryAllowed=typedAddressHistoryAllowed_;
            typedAddressHistoryAllowed_=true;
            previousCount=navigationCount_;
            if(SUCCEEDED(longOpen))longOpen=navigate(longSavedPath.wstring(),true);
            const bool longAddressOpened=SUCCEEDED(longOpen)&&pumpUntil([&]{return navigationCount_>previousCount&&exactScopeMembership();},5000);
            SearchViewPresentation longActual;
            const bool longPresentation=longAddressOpened&&pumpUntil([&] {
                return !searchPresentationPending_&&SUCCEEDED(searchPresentationStatus_)&&
                    SUCCEEDED(captureSearchViewPresentation(folderView_.Get(),&longActual))&&samePresentation(longActual,importedPresentation);
            },2000);
            check("long_saved_address_recreates_native_scope_metadata_view_and_mru",longSavedPath.native().size()>MAX_PATH&&
                longNativeBind==HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)&&!nativeLongSavedFolder&&longAddressOpened&&
                sameRules(searchScopeRules_)&&searchFileProperties_&&*searchFileProperties_==importedProperties&&longPresentation&&
                !typedAddresses_.empty()&&typedAddresses_.front()==longSavedPath.wstring(),hresultMessage(longOpen));
            const auto longReuseCount=navigationCount_;const auto longReuseHistory=historyIndex_;
            const auto longReuseContexts=searchLocations_.size();
            Pidl longReusePidl(ILCloneFull(currentPidl_.get()));
            const auto longReuse=longAddressOpened?navigate(longSavedPath.wstring(),true):E_UNEXPECTED;
            const auto sameReuseNavigation=[&] {
                return !navigating_&&navigationCount_==longReuseCount&&historyIndex_==longReuseHistory&&
                    searchLocations_.size()==longReuseContexts&&currentPidl_&&longReusePidl&&
                    ILIsEqual(currentPidl_.get(),longReusePidl.get());
            };
            bool longReuseStable=sameReuseNavigation(),longReuseMembership=false,longReusePresentation=false;
            // Reusing the native view posts presentation work rather than a
            // browse. Columns/group/sort can temporarily invalidate Items;
            // wait for actual presentation/membership while retaining the
            // original native PIDL, travel history and navigation count.
            const bool longReuseReady=longReuse==S_OK&&pumpUntil([&] {
                longReuseStable=longReuseStable&&sameReuseNavigation();
                if(!longReuseStable)return true;
                if(searchPresentationPending_||FAILED(searchPresentationStatus_))return false;
                longReusePresentation=SUCCEEDED(captureSearchViewPresentation(folderView_.Get(),&longActual))&&
                    samePresentation(longActual,importedPresentation);
                longReuseStable=longReuseStable&&sameReuseNavigation();
                if(!longReuseStable)return true;
                longReuseMembership=longReusePresentation&&exactScopeMembership();
                longReuseStable=longReuseStable&&sameReuseNavigation();
                return !longReuseStable||(longReusePresentation&&longReuseMembership);
            },2000)&&longReuseStable;
            const bool longReuseSamePidl=currentPidl_&&longReusePidl&&ILIsEqual(currentPidl_.get(),longReusePidl.get());
            const bool longReuseRules=sameRules(searchScopeRules_);
            const bool longReuseProperties=searchFileProperties_&&*searchFileProperties_==importedProperties;
            check("long_saved_same_native_query_reuse_preserves_history_without_pending_browse",longReuse==S_OK&&!navigating_&&
                navigationCount_==longReuseCount&&historyIndex_==longReuseHistory&&searchLocations_.size()==longReuseContexts&&
                longReuseSamePidl&&longReuseRules&&longReuseProperties&&longReuseMembership&&longReuseReady&&longReusePresentation,
                hresultMessage(longReuse)+L"; same PIDL="+std::to_wstring(longReuseSamePidl)+
                L"; navigating="+std::to_wstring(navigating_)+L"; navigation delta="+
                std::to_wstring(static_cast<long long>(navigationCount_)-static_cast<long long>(longReuseCount))+
                L"; history delta="+std::to_wstring(historyIndex_-longReuseHistory)+L"; cache delta="+
                std::to_wstring(static_cast<long long>(searchLocations_.size())-static_cast<long long>(longReuseContexts))+
                L"; rules="+std::to_wstring(longReuseRules)+L"; properties="+std::to_wstring(longReuseProperties)+
                L"; membership="+std::to_wstring(longReuseMembership)+L"; native presentation="+
                std::to_wstring(longReusePresentation)+L"; unchanged throughout wait="+std::to_wstring(longReuseStable));
            previousCount=navigationCount_;
            const auto longRefine=longAddressOpened?startSearch(activeQuery_,searchRecursive_,2,L"System.Size:System.Size#Tiny"):E_UNEXPECTED;
            const bool longRefined=SUCCEEDED(longRefine)&&pumpUntil([&]{return navigationCount_>previousCount&&exactScopeMembership();},5000);
            Pidl longRefinedPidl(longRefined?ILCloneFull(currentPidl_.get()):nullptr);
            const auto longLeave=navigate(fixture.wstring());
            const bool longLeft=SUCCEEDED(longLeave)&&pumpUntil([&]{return !navigating_&&atLocation(fixture);},5000);
            const auto longBack=longRefined&&longLeft?execute(Back):E_UNEXPECTED;
            const bool longRestored=SUCCEEDED(longBack)&&pumpUntil(exactScopeMembership,5000);
            check("long_saved_refinement_and_native_back_preserve_properties_rules_and_fileids",longRefined&&longRestored&&
                ILIsEqual(currentPidl_.get(),longRefinedPidl.get())&&sameRules(searchScopeRules_)&&
                searchFileProperties_&&*searchFileProperties_==importedProperties,hresultMessage(longBack));
            auto unsupportedBytes=longSavedBytes;
            const auto propertyEnd=unsupportedBytes.find("</persistedQuery>");
            if(propertyEnd!=std::string::npos)unsupportedBytes.insert(propertyEnd,"<unsupportedOwnedField/>");
            {std::ofstream output(extended(longUnsupportedPath),std::ios::binary);output.write(unsupportedBytes.data(),static_cast<std::streamsize>(unsupportedBytes.size()));}
            Pidl beforeUnsupported(ILCloneFull(currentPidl_.get()));
            const auto beforeUnsupportedHistory=historyIndex_;const auto beforeUnsupportedCount=navigationCount_;
            const auto beforeUnsupportedContexts=searchLocations_.size();const auto beforeUnsupportedAddresses=typedAddresses_;
            const auto beforeUnsupportedProperties=searchFileProperties_;const auto beforeUnsupportedView=searchPresentation_;
            const auto unsupportedOpen=navigate(longUnsupportedPath.wstring(),true);
            check("long_saved_unsupported_external_shape_preserves_native_error_and_current_state",
                unsupportedOpen==HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)&&!navigating_&&
                ILIsEqual(currentPidl_.get(),beforeUnsupported.get())&&historyIndex_==beforeUnsupportedHistory&&
                navigationCount_==beforeUnsupportedCount&&searchLocations_.size()==beforeUnsupportedContexts&&
                typedAddresses_==beforeUnsupportedAddresses&&searchFileProperties_==beforeUnsupportedProperties&&
                searchPresentation_.has_value()==beforeUnsupportedView.has_value()&&
                (!searchPresentation_||samePresentation(*searchPresentation_,*beforeUnsupportedView))&&exactScopeMembership(),hresultMessage(unsupportedOpen));
            const auto longVariantPath=longSavedDirectory/L"Different metadata same query.search-ms";
            auto variantProperties=importedProperties;variantProperties.author=L"Different owned 作者 🚀";
            auto variantPresentation=importedPresentation;variantPresentation.mode=SearchViewMode::Icons;variantPresentation.iconSize=64;
            auto variantOpen=saveSearchForScopeRules(L"System.FileName:=\"scope-member.txt\"",originalRules,extended(longVariantPath),
                SearchSaveMode::CreateNew,&variantPresentation,&variantProperties);
            if(SUCCEEDED(variantOpen))variantOpen=navigate(fixture.wstring());
            const bool variantLeft=SUCCEEDED(variantOpen)&&pumpUntil([&]{return !navigating_&&atLocation(fixture);},5000);
            previousCount=navigationCount_;
            variantOpen=variantLeft?navigate(longVariantPath.wstring(),true):E_UNEXPECTED;
            const bool variantOpened=SUCCEEDED(variantOpen)&&pumpUntil([&]{return navigationCount_>previousCount&&exactScopeMembership();},5000);
            SearchViewPresentation variantActual;
            const bool variantApplied=variantOpened&&pumpUntil([&] {
                return !searchPresentationPending_&&SUCCEEDED(searchPresentationStatus_)&&
                    SUCCEEDED(captureSearchViewPresentation(folderView_.Get(),&variantActual))&&samePresentation(variantActual,variantPresentation);
            },2000);
            check("long_saved_leave_reopen_same_native_query_uses_new_source_metadata_and_presentation",variantOpened&&variantApplied&&
                ILIsEqual(currentPidl_.get(),longReusePidl.get())&&searchFileProperties_&&*searchFileProperties_==variantProperties&&
                sameRules(searchScopeRules_),hresultMessage(variantOpen));
            {
                PrivatePresentation longSelectionPresentation(window_,true);
                const auto longDeadline=GetTickCount64()+10000;
                const auto waitForLongSelection=[&](const std::function<bool()>& predicate) {
                    const auto now=GetTickCount64();
                    return now<longDeadline&&pumpUntil(predicate,static_cast<DWORD>(longDeadline-now));
                };
                // The subject here is default activation of the enumerated
                // saved item, not a second long-text parent parsing operation.
                ComPtr<IShellItem> longOriginalParent;
                const auto longParentItemRead=longSavedItem?longSavedItem->GetParent(&longOriginalParent):E_PENDING;
                FILE_ID_INFO longParentId{};
                const auto longParentIdentityRead=nativeFileIdentity(extended(longSavedDirectory),longParentId);
                const auto longParent=SUCCEEDED(longParentItemRead)&&longOriginalParent&&SUCCEEDED(longParentIdentityRead)?
                    browser_->BrowseToObject(longOriginalParent.Get(),SBSP_ABSOLUTE):
                    FAILED(longParentItemRead)?longParentItemRead:FAILED(longParentIdentityRead)?longParentIdentityRead:E_PENDING;
                HRESULT longCurrentParentRead=E_PENDING,longParentCompare=E_PENDING,longParentPathRead=E_PENDING,longCurrentParentIdentityRead=E_PENDING;
                int longParentOrder=1;
                const bool longParentReady=SUCCEEDED(longParent)&&waitForLongSelection([&] {
                    if(navigating_||!view_||!folderView_)return false;
                    ComPtr<IShellItem> actualParent;longCurrentParentRead=currentFolder(actualParent);
                    if(FAILED(longCurrentParentRead)||!actualParent)return false;
                    longParentOrder=1;longParentCompare=actualParent->Compare(longOriginalParent.Get(),SICHINT_CANONICAL,&longParentOrder);
                    if(FAILED(longParentCompare)||longParentOrder!=0)return false;
                    PWSTR path=nullptr;longParentPathRead=actualParent->GetDisplayName(SIGDN_FILESYSPATH,&path);
                    FILE_ID_INFO actual{};
                    longCurrentParentIdentityRead=SUCCEEDED(longParentPathRead)&&path?
                        nativeFileIdentity(extended(std::filesystem::path(path)),actual):E_PENDING;
                    CoTaskMemFree(path);
                    return SUCCEEDED(longCurrentParentIdentityRead)&&actual.VolumeSerialNumber==longParentId.VolumeSerialNumber&&
                        std::equal(std::begin(actual.FileId.Identifier),std::end(actual.FileId.Identifier),std::begin(longParentId.FileId.Identifier));
                });
                ComPtr<IShellView> longParentView=longParentReady?view_:nullptr;
                ComPtr<IFolderView2> longParentFolderView=longParentReady?folderView_:nullptr;
                const auto longViewActivation=longParentReady&&longSelectionPresentation.ready?
                    longParentView->UIActivate(SVUIA_ACTIVATE_NOFOCUS):E_PENDING;
                ComPtr<IShellItemArray> longParentItems;ComPtr<IShellItem> enumeratedLong;
                HRESULT longItemsRead=E_PENDING,longItemsCountRead=E_PENDING,longEnumeratedRead=E_PENDING,longEnumeratedCompare=E_PENDING;
                HRESULT longPathRead=E_PENDING,longIdentityRead=E_PENDING;
                DWORD longParentCount=0;int longEnumeratedOrder=1;
                const auto isOriginalLongFile=[&](IShellItem* item) {
                    PWSTR path=nullptr;longPathRead=item?item->GetDisplayName(SIGDN_FILESYSPATH,&path):E_POINTER;
                    FILE_ID_INFO actual{};
                    longIdentityRead=SUCCEEDED(longPathRead)&&path?nativeFileIdentity(extended(std::filesystem::path(path)),actual):E_PENDING;
                    CoTaskMemFree(path);
                    return SUCCEEDED(longIdentityRead)&&actual.VolumeSerialNumber==longSavedId.VolumeSerialNumber&&
                        std::equal(std::begin(actual.FileId.Identifier),std::end(actual.FileId.Identifier),std::begin(longSavedId.FileId.Identifier));
                };
                // A completed browse does not guarantee enumeration or realized
                // selection. Wait for this actual source item, retaining its native
                // parent array/view and preserving canonical comparison plus FileID.
                const bool longItemReady=SUCCEEDED(longViewActivation)&&longSavedItem&&waitForLongSelection([&] {
                    if(view_.Get()!=longParentView.Get()||folderView_.Get()!=longParentFolderView.Get()||navigating_)return false;
                    longItemsRead=longParentFolderView->Items(SVGIO_ALLVIEW,IID_PPV_ARGS(&longParentItems));
                    if(FAILED(longItemsRead)||!longParentItems)return false;
                    longItemsCountRead=longParentItems->GetCount(&longParentCount);
                    if(FAILED(longItemsCountRead))return false;
                    for(DWORD index=0;index<longParentCount;++index) {
                        ComPtr<IShellItem> candidate;longEnumeratedRead=longParentItems->GetItemAt(index,&candidate);
                        if(FAILED(longEnumeratedRead)||!candidate)return false;
                        longEnumeratedOrder=1;longEnumeratedCompare=candidate->Compare(longSavedItem.Get(),SICHINT_CANONICAL,&longEnumeratedOrder);
                        if(SUCCEEDED(longEnumeratedCompare)&&longEnumeratedOrder==0&&isOriginalLongFile(candidate.Get())) {
                            enumeratedLong=candidate;return true;
                        }
                    }
                    return false;
                });
                PIDLIST_ABSOLUTE longRaw=nullptr;
                const auto longPidlRead=longItemReady?SHGetIDListFromObject(enumeratedLong.Get(),&longRaw):E_PENDING;
                Pidl longSelected(longRaw);
                const auto longSelect=SUCCEEDED(longPidlRead)&&longSelected?
                    longParentView->SelectItem(ILFindLastID(longSelected.get()),SVSI_SELECT|SVSI_DESELECTOTHERS|SVSI_NOTAKEFOCUS):E_PENDING;
                ComPtr<IShellItemArray> longSelection;ComPtr<IShellItem> selectedLong;DWORD selectedLongCount=0;int selectedLongOrder=1;
                HRESULT longSelectionRead=E_PENDING,longSelectionCountRead=E_PENDING,longSelectedItemRead=E_PENDING,longSelectedCompare=E_PENDING;
                const bool longSelectedExactly=SUCCEEDED(longSelect)&&waitForLongSelection([&] {
                    if(view_.Get()!=longParentView.Get()||folderView_.Get()!=longParentFolderView.Get()||navigating_)return false;
                    longSelectionRead=longParentFolderView->GetSelection(FALSE,&longSelection);
                    if(FAILED(longSelectionRead)||!longSelection)return false;
                    longSelectionCountRead=longSelection->GetCount(&selectedLongCount);
                    if(FAILED(longSelectionCountRead)||selectedLongCount!=1)return false;
                    longSelectedItemRead=longSelection->GetItemAt(0,&selectedLong);
                    if(FAILED(longSelectedItemRead)||!selectedLong)return false;
                    selectedLongOrder=1;longSelectedCompare=selectedLong->Compare(longSavedItem.Get(),SICHINT_CANONICAL,&selectedLongOrder);
                    return SUCCEEDED(longSelectedCompare)&&selectedLongOrder==0&&isOriginalLongFile(selectedLong.Get());
                });
                const bool originalNewWindow=newWindowMode_;newWindowMode_=false;
                previousCount=navigationCount_;
                const auto longActivate=longSelectedExactly?OnDefaultCommand(longParentView.Get()):E_PENDING;
                const bool longActivated=longActivate==S_OK&&waitForLongSelection([&]{return navigationCount_>previousCount&&exactScopeMembership();});
                const bool longActivationPresentation=longActivated&&waitForLongSelection([&] {
                    return !searchPresentationPending_&&SUCCEEDED(searchPresentationStatus_)&&
                        SUCCEEDED(captureSearchViewPresentation(folderView_.Get(),&longActual))&&samePresentation(longActual,importedPresentation);
                });
                newWindowMode_=originalNewWindow;
                check("long_saved_actual_native_selection_default_command_opens_reconstructed_query",longSelectedExactly&&longActivate==S_OK&&
                    longActivated&&longActivationPresentation&&sameRules(searchScopeRules_)&&
                    searchFileProperties_&&*searchFileProperties_==importedProperties,
                    L"original parent="+hresultMessage(longParentItemRead)+L"; original parent FileID="+hresultMessage(longParentIdentityRead)+
                    L"; parent browse="+hresultMessage(longParent)+L"/"+std::to_wstring(longParentReady)+L"; current parent="+
                    hresultMessage(longCurrentParentRead)+L"; parent canonical="+hresultMessage(longParentCompare)+L"/"+
                    std::to_wstring(longParentOrder)+L"; parent path="+hresultMessage(longParentPathRead)+L"; current parent FileID="+
                    hresultMessage(longCurrentParentIdentityRead)+L"; presentation="+
                    std::to_wstring(longSelectionPresentation.ready)+L"; view activation="+hresultMessage(longViewActivation)+
                    L"; Items="+hresultMessage(longItemsRead)+L"; Items count="+hresultMessage(longItemsCountRead)+L"/"+std::to_wstring(longParentCount)+
                    L"; enumerated item="+hresultMessage(longEnumeratedRead)+L"; enumerated canonical="+hresultMessage(longEnumeratedCompare)+
                    L"/"+std::to_wstring(longEnumeratedOrder)+L"; PIDL="+hresultMessage(longPidlRead)+L"; select="+hresultMessage(longSelect)+
                    L"; selection="+hresultMessage(longSelectionRead)+L"; selected count="+hresultMessage(longSelectionCountRead)+L"/"+
                    std::to_wstring(selectedLongCount)+L"; selected item="+hresultMessage(longSelectedItemRead)+L"; selected canonical="+
                    hresultMessage(longSelectedCompare)+L"/"+std::to_wstring(selectedLongOrder)+L"; selected path="+hresultMessage(longPathRead)+
                    L"; selected FileID="+hresultMessage(longIdentityRead)+L"; default command="+hresultMessage(longActivate)+
                    L"; native query/presentation="+std::to_wstring(longActivated)+L"/"+std::to_wstring(longActivationPresentation)+
                    L"; common deadline="+hresultMessage(GetTickCount64()>=longDeadline?HRESULT_FROM_WIN32(ERROR_TIMEOUT):S_OK));
            }
            FILE_ID_INFO longFinalId{};
            check("long_saved_fallback_preserves_owned_source_bytes_and_file_identity",!longSavedBytes.empty()&&
                readLongBytes(longSavedPath)==longSavedBytes&&SUCCEEDED(nativeFileIdentity(extended(longSavedPath),longFinalId))&&
                longFinalId.VolumeSerialNumber==longSavedId.VolumeSerialNumber&&
                std::equal(std::begin(longFinalId.FileId.Identifier),std::end(longFinalId.FileId.Identifier),std::begin(longSavedId.FileId.Identifier)));
            typedAddresses_=originalAddresses;typedAddressHistoryAllowed_=originalHistoryAllowed;
            pendingTypedAddress_.clear();pendingTypedAddressTarget_.reset();
            const auto longReturn=navigate(fixture.wstring());
            const bool longReturned=SUCCEEDED(longReturn)&&pumpUntil([&]{return !navigating_&&atLocation(fixture);},5000);
            check("long_saved_fixture_returns_to_native_origin",longReturned,hresultMessage(longReturn));
            if(longReturned)std::filesystem::remove_all(extended(longSavedDirectory),longSavedError);
            check("long_saved_owned_fixture_cleanup",longReturned&&!longSavedError);
            {
                // Query the same real one-file selection through both native
                // sites. Do not infer sharing policy from an HRESULT or invoke
                // either registered handler, menu leaf or recipient interface.
                const auto pairedStarted = GetTickCount64(), pairedDeadline = pairedStarted + 5000;
                const auto pumpPair = [&](const std::function<bool()>& complete) {
                    const auto now = GetTickCount64();
                    return now < pairedDeadline && pumpUntil(complete, static_cast<DWORD>(pairedDeadline - now));
                };
                const auto pairedClipboard = GetClipboardSequenceNumber();
                const auto pairedNavigation = navigationCount_;
                const auto pairedHistory = history_.size();
                const auto pairedHistoryIndex = historyIndex_;
                const auto originalView = view_;
                const auto originalFolderView = folderView_;
                Pidl originalFolderId(currentPidl_ ? ILCloneFull(currentPidl_.get()) : nullptr);
                const auto shareSource = fixture / L"file-0.txt";
                FILE_ID_INFO originalSourceId{}, originalFolderFileId{};
                auto pairedRead = nativeFileIdentity(shareSource, originalSourceId);
                if (SUCCEEDED(pairedRead)) pairedRead = nativeFileIdentity(fixture, originalFolderFileId);
                const auto bytesOf = [&](const std::filesystem::path& path) {
                    std::ifstream input(path, std::ios::binary);
                    return input.good() ? std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()) : std::string{};
                };
                const auto originalSourceBytes = bytesOf(shareSource);
                const auto securityOf = [](const std::filesystem::path& path, std::vector<BYTE>& bytes) {
                    constexpr SECURITY_INFORMATION fields = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
                    DWORD required = 0; SetLastError(ERROR_SUCCESS);
                    const auto sized = GetFileSecurityW(path.c_str(), fields, nullptr, 0, &required);
                    const auto error = GetLastError();
                    if (sized || error != ERROR_INSUFFICIENT_BUFFER || !required || required > 65536)
                        return HRESULT_FROM_WIN32(error ? error : ERROR_INVALID_DATA);
                    bytes.resize(required);
                    if (!GetFileSecurityW(path.c_str(), fields, bytes.data(), required, &required))
                        return HRESULT_FROM_WIN32(GetLastError());
                    bytes.resize(required); return S_OK;
                };
                std::vector<BYTE> sourceSecurity, folderSecurity;
                if (SUCCEEDED(pairedRead)) pairedRead = securityOf(shareSource, sourceSecurity);
                if (SUCCEEDED(pairedRead)) pairedRead = securityOf(fixture, folderSecurity);
                ComPtr<IShellItem> pairedFolder;
                if (SUCCEEDED(pairedRead)) pairedRead = currentFolder(pairedFolder);
                PIDLIST_ABSOLUTE fileRaw = nullptr;
                if (SUCCEEDED(pairedRead)) pairedRead = SHParseDisplayName(shareSource.c_str(), nullptr, &fileRaw, 0, nullptr);
                Pidl selectedId(fileRaw);
                if (SUCCEEDED(pairedRead) && originalView && selectedId)
                    pairedRead = originalView->SelectItem(ILFindLastID(selectedId.get()),
                        SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_NOTAKEFOCUS);
                else if (SUCCEEDED(pairedRead)) pairedRead = E_UNEXPECTED;
                if (SUCCEEDED(pairedRead)) pairedRead = OnStateChange(originalView.Get(), CDBOSC_SELCHANGE);
                if (SUCCEEDED(pairedRead)) updateCommands();
                ComPtr<IShellItemArray> appSelected;
                const bool appPairReady = SUCCEEDED(pairedRead) && pumpPair([&] {
                    DWORD countSelected = 0;
                    return originalView.Get() == view_.Get() && !navigating_ && !selectionStateDirty_ && !namespaceDirty_ &&
                        !commandRefreshActive_ && !commandRefreshPending_ && SUCCEEDED(selection(appSelected)) && appSelected &&
                        SUCCEEDED(appSelected->GetCount(&countSelected)) && countSelected == 1;
                });
                if (SUCCEEDED(pairedRead) && !appPairReady) pairedRead = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                std::set<NativeFileIdentity> appIdentities, referenceIdentities;
                if (SUCCEEDED(pairedRead)) pairedRead = nativeArrayIdentities(appSelected.Get(), appIdentities);
                std::array<BYTE, 16> expectedId{};
                std::copy(std::begin(originalSourceId.FileId.Identifier), std::end(originalSourceId.FileId.Identifier), expectedId.begin());
                const std::set<NativeFileIdentity> expectedSelection{{originalSourceId.VolumeSerialNumber, expectedId}};
                bool pairEquivalent = SUCCEEDED(pairedRead) && appIdentities == expectedSelection && namespaceActions_.facts().selectionCount == 1u;
                std::wstring pairedDetail = L"App ready=" + std::to_wstring(appPairReady) + L"; input=" + hresultMessage(pairedRead);
                {
                    ShareSiteReference reference;
                    EXPLORER_BROWSER_OPTIONS options = EBO_NONE;
                    FOLDERSETTINGS nativeSettings{static_cast<UINT>(FVM_AUTO), 0}; int nativeIconSize = 0;
                    FOLDERVIEWMODE nativeMode = FVM_AUTO; DWORD nativeFlags = 0;
                    auto referenceRead = SUCCEEDED(pairedRead) ? browser_->GetOptions(&options) : pairedRead;
                    if (SUCCEEDED(referenceRead)) referenceRead = originalFolderView->GetViewModeAndIconSize(&nativeMode, &nativeIconSize);
                    if (SUCCEEDED(referenceRead)) referenceRead = originalFolderView->GetCurrentFolderFlags(&nativeFlags);
                    nativeSettings.ViewMode = static_cast<UINT>(nativeMode);
                    nativeSettings.fFlags = static_cast<UINT>(nativeFlags);
                    if (SUCCEEDED(referenceRead)) referenceRead = reference.create(instance_, pairedFolder.Get(), options, nativeSettings);
                    const bool referenceReady = SUCCEEDED(referenceRead) && pumpPair([&] {
                        return reference.navigation && reference.navigation->finished;
                    });
                    if (SUCCEEDED(referenceRead)) referenceRead = referenceReady ? reference.navigation->status : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                    if (SUCCEEDED(referenceRead)) referenceRead = reference.browser->GetCurrentView(IID_PPV_ARGS(&reference.view));
                    ComPtr<IFolderView2> referenceFolderView;
                    if (SUCCEEDED(referenceRead)) referenceRead = reference.view.As(&referenceFolderView);
                    if (SUCCEEDED(referenceRead)) referenceRead = referenceFolderView->SetViewModeAndIconSize(
                        static_cast<FOLDERVIEWMODE>(nativeSettings.ViewMode), nativeIconSize);
                    if (SUCCEEDED(referenceRead)) referenceRead = reference.view->SelectItem(ILFindLastID(selectedId.get()),
                        SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_NOTAKEFOCUS);
                    ComPtr<IShellItemArray> referenceSelected;
                    if (SUCCEEDED(referenceRead)) referenceRead = referenceFolderView->Items(SVGIO_SELECTION, IID_PPV_ARGS(&referenceSelected));
                    DWORD referenceCount = 0;
                    if (SUCCEEDED(referenceRead)) referenceRead = referenceSelected->GetCount(&referenceCount);
                    if (SUCCEEDED(referenceRead)) referenceRead = nativeArrayIdentities(referenceSelected.Get(), referenceIdentities);
                    ComPtr<IUnknown> appIdentity, referenceIdentity;
                    const bool distinctSites = originalView && reference.view && SUCCEEDED(originalView.As(&appIdentity)) &&
                        SUCCEEDED(reference.view.As(&referenceIdentity)) && appIdentity.Get() != referenceIdentity.Get();
                    const bool folderSame = reference.navigation && reference.navigation->completed && originalFolderId &&
                        ILIsEqual(reference.navigation->completed.get(), originalFolderId.get());
                    pairEquivalent = pairEquivalent && SUCCEEDED(referenceRead) && referenceCount == 1 &&
                        referenceIdentities == expectedSelection && distinctSites && folderSame && reference.owner && !IsWindowVisible(reference.owner);
                    pairedDetail += L"; independent=" + hresultMessage(referenceRead) + L"; exact FileIDs=" +
                        std::to_wstring(appIdentities == expectedSelection) + L"/" + std::to_wstring(referenceIdentities == expectedSelection) +
                        L"; folder=" + std::to_wstring(folderSame) + L"; distinct sites=" + std::to_wstring(distinctSites);
                    if (SUCCEEDED(referenceRead) && pairEquivalent) {
                        NativeNamespaceActions independentActions;
                        referenceRead = independentActions.initialize(reference.owner, {pairedFolder, referenceSelected, reference.view});
                        ComPtr<IShellFolder> nativeParent; PCUITEMID_CHILD nativeChild = nullptr;
                        if (SUCCEEDED(referenceRead)) referenceRead = SHBindToParent(selectedId.get(), IID_PPV_ARGS(&nativeParent), &nativeChild);
                        Pidl nativeParentId(ILCloneFull(selectedId.get()));
                        if (SUCCEEDED(referenceRead) && (!nativeParentId || !ILRemoveLastID(nativeParentId.get()))) referenceRead = E_UNEXPECTED;
                        const auto appContext = commandContext();
                        const auto declaredServices = [&](IShellView* site, const wchar_t* side) {
                            ComPtr<ICommDlgBrowser2> frame; DWORD flags = 0;
                            const auto frameRead = IUnknown_QueryService(site, SID_SExplorerBrowserFrame, IID_PPV_ARGS(&frame));
                            const auto flagsRead = SUCCEEDED(frameRead) ? frame->GetViewFlags(&flags) : frameRead;
                            ComPtr<IShellBrowser> topLevel;
                            const auto topRead = IUnknown_QueryService(site, SID_STopLevelBrowser, IID_PPV_ARGS(&topLevel));
                            HWND topWindow = nullptr;
                            const auto windowRead = SUCCEEDED(topRead) ? topLevel->GetWindow(&topWindow) : topRead;
                            DWORD process = 0;
                            const auto thread = topWindow ? GetWindowThreadProcessId(topWindow, &process) : 0;
                            pairedDetail += L"; " + std::wstring(side) + L" frame/flags=" + hresultMessage(frameRead) + L"/" +
                                hresultMessage(flagsRead) + L"/" + std::to_wstring(flags) + L" top/window=" + hresultMessage(topRead) + L"/" +
                                hresultMessage(windowRead) + L" ownedSTA=" + std::to_wstring(thread == GetCurrentThreadId() && process == GetCurrentProcessId());
                        };
                        declaredServices(originalView.Get(), L"App"); declaredServices(reference.view.Get(), L"independent");
                        ComPtr<ICommDlgBrowser2> appFrame; DWORD appFrameFlags = 0;
                        const auto appFrameRead = QueryService(SID_SExplorerBrowserFrame, IID_PPV_ARGS(&appFrame));
                        const auto appFlagsRead = SUCCEEDED(appFrameRead) ? appFrame->GetViewFlags(&appFrameFlags) : appFrameRead;
                        pairedDetail += L"; declared host frame=" + hresultMessage(appFrameRead) + L"/" + hresultMessage(appFlagsRead) + L"/" + std::to_wstring(appFrameFlags);
                        struct Readback {
                            HRESULT providerRead = E_PENDING, catalogRead = E_PENDING, menuRead = E_PENDING;
                            NamespaceCommandState provider; AppCommandCapability catalog;
                            UINT leaves = 0, nativeState = 0, nativeType = 0, menuCommands = 0; bool submenu = false;
                        };
                        const auto readSide = [&](NativeNamespaceActions& actions, IShellItemArray* items, IShellView* site,
                                                  HWND owner, UINT command, std::wstring_view key) {
                            Readback result;
                            result.providerRead = namespaceCommandState(key, items, site, &result.provider);
                            result.catalogRead = queryAppCommand(actions, command, appContext, &result.catalog);
                            struct Key { HKEY value = nullptr; ~Key() { if (value) RegCloseKey(value); } } registry;
                            // aKeys names class/root keys, whose shell subkeys
                            // provide the verbs. Match the production native
                            // CommandStore root, then read only this exact leaf.
                            constexpr auto path = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\CommandStore";
                            result.menuRead = HRESULT_FROM_WIN32(RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &registry.value));
                            DEFCONTEXTMENU definition{}; definition.hwnd = owner;
                            definition.pidlFolder = nativeParentId.get(); definition.psf = nativeParent.Get();
                            definition.cidl = 1; definition.apidl = &nativeChild; definition.cKeys = 1; definition.aKeys = &registry.value;
                            ComPtr<IContextMenu> context;
                            if (SUCCEEDED(result.menuRead)) result.menuRead = SHCreateDefaultContextMenu(&definition, IID_PPV_ARGS(&context));
                            NativeContextMenu menu;
                            if (SUCCEEDED(result.menuRead)) result.menuRead = menu.createLeafState(context.Get(), site, CMF_EXTENDEDVERBS);
                            if (SUCCEEDED(result.menuRead)) result.menuCommands = menu.commandCount();
                            std::vector<ContextMenuEntry> entries;
                            if (SUCCEEDED(result.menuRead)) result.menuRead = menu.enumerate(entries, false);
                            const std::wstring canonical(key);
                            for (const auto& entry : entries) if (!entry.separator() && _wcsicmp(entry.canonicalVerb.c_str(), canonical.c_str()) == 0) {
                                ++result.leaves; result.nativeState = entry.state; result.nativeType = entry.type; result.submenu = entry.submenu;
                            }
                            menu.reset(); return result;
                        };
                        constexpr std::array<std::pair<UINT, std::wstring_view>, 3> shareCases{{
                            {RibbonSpecificPeople, L"Windows.ShareSpecificUsers"}, {RibbonStopSharing, L"Windows.SharePrivate"}, {Sharing, L"Windows.ModernShare"}}};
                        for (const auto& [command, key] : shareCases) {
                            if (FAILED(referenceRead) || GetTickCount64() >= pairedDeadline) { pairEquivalent = false; break; }
                            const auto appRead = readSide(namespaceActions_, appSelected.Get(), originalView.Get(), window_, command, key);
                            const auto independentRead = readSide(independentActions, referenceSelected.Get(), reference.view.Get(), reference.owner, command, key);
                            const bool providersSame = appRead.providerRead == independentRead.providerRead &&
                                (FAILED(appRead.providerRead) || (appRead.provider.state == independentRead.provider.state &&
                                 appRead.provider.handler == independentRead.provider.handler && appRead.provider.initialized == independentRead.provider.initialized &&
                                 appRead.provider.siteAttached == independentRead.provider.siteAttached));
                            const bool catalogsSame = SUCCEEDED(appRead.catalogRead) && SUCCEEDED(independentRead.catalogRead) &&
                                appRead.catalog.status == independentRead.catalog.status && appRead.catalog.enabled == independentRead.catalog.enabled &&
                                appRead.catalog.checked == independentRead.catalog.checked &&
                                (FAILED(appRead.catalog.status) || appRead.catalog.native.state == independentRead.catalog.native.state);
                            const bool menusSame = appRead.menuRead == independentRead.menuRead && SUCCEEDED(appRead.menuRead) &&
                                appRead.menuCommands > 0u && independentRead.menuCommands > 0u &&
                                appRead.leaves <= 1u && appRead.leaves == independentRead.leaves && appRead.nativeState == independentRead.nativeState &&
                                appRead.nativeType == independentRead.nativeType && appRead.submenu == independentRead.submenu;
                            pairEquivalent = pairEquivalent && providersSame && catalogsSame && menusSame;
                            const auto summary = [&](const Readback& read) {
                                // Native output is atomic on failure; default
                                // fields then describe no returned provenance.
                                return hresultMessage(read.providerRead) + L"/" + std::to_wstring(read.provider.state) + L" valid=" +
                                    std::to_wstring(SUCCEEDED(read.providerRead)) + L" init/site=" +
                                    std::to_wstring(read.provider.initialized) + L"/" + std::to_wstring(read.provider.siteAttached) +
                                    L" catalog=" + hresultMessage(read.catalogRead) + L"/" + hresultMessage(read.catalog.status) + L"/" +
                                    std::to_wstring(read.catalog.enabled) + L" menu=" + hresultMessage(read.menuRead) + L"/" +
                                    std::to_wstring(read.leaves) + L"/" + std::to_wstring(read.nativeState) + L"/" + std::to_wstring(read.submenu) +
                                    L" actual root commands=" + std::to_wstring(read.menuCommands);
                            };
                            pairedDetail += L"; " + std::wstring(key) + L" App{" + summary(appRead) + L"} independent{" + summary(independentRead) +
                                L"} equal=" + std::to_wstring(providersSame) + L"/" + std::to_wstring(catalogsSame) + L"/" + std::to_wstring(menusSame);
                        }
                        independentActions.reset();
                        pairEquivalent = pairEquivalent && SUCCEEDED(referenceRead);
                    }
                }
                FILE_ID_INFO finalSourceId{}, finalFolderFileId{};
                std::vector<BYTE> finalSourceSecurity, finalFolderSecurity;
                auto preservedRead = nativeFileIdentity(shareSource, finalSourceId);
                if (SUCCEEDED(preservedRead)) preservedRead = nativeFileIdentity(fixture, finalFolderFileId);
                if (SUCCEEDED(preservedRead)) preservedRead = securityOf(shareSource, finalSourceSecurity);
                if (SUCCEEDED(preservedRead)) preservedRead = securityOf(fixture, finalFolderSecurity);
                std::set<NativeFileIdentity> finalSelection;
                ComPtr<IShellItemArray> finalSelected;
                if (SUCCEEDED(preservedRead)) preservedRead = selection(finalSelected);
                if (SUCCEEDED(preservedRead)) preservedRead = nativeArrayIdentities(finalSelected.Get(), finalSelection);
                const bool originalViewRetained = view_.Get() == originalView.Get() && folderView_.Get() == originalFolderView.Get() &&
                    !navigating_ && currentPidl_ && originalFolderId && ILIsEqual(currentPidl_.get(), originalFolderId.get()) &&
                    navigationCount_ == pairedNavigation && history_.size() == pairedHistory && historyIndex_ == pairedHistoryIndex;
                const bool sourcesPreserved = SUCCEEDED(preservedRead) && !originalSourceBytes.empty() && bytesOf(shareSource) == originalSourceBytes &&
                    originalSourceId.VolumeSerialNumber == finalSourceId.VolumeSerialNumber && originalFolderFileId.VolumeSerialNumber == finalFolderFileId.VolumeSerialNumber &&
                    std::memcmp(originalSourceId.FileId.Identifier, finalSourceId.FileId.Identifier, sizeof(originalSourceId.FileId.Identifier)) == 0 &&
                    std::memcmp(originalFolderFileId.FileId.Identifier, finalFolderFileId.FileId.Identifier, sizeof(originalFolderFileId.FileId.Identifier)) == 0 &&
                    sourceSecurity == finalSourceSecurity && folderSecurity == finalFolderSecurity && finalSelection == expectedSelection;
                bool inputVisible = true, inputUnchanged = false;
                const auto privateDesktop = PrivateDesktop::current();
                const bool isolated = privateDesktop && SUCCEEDED(privateDesktop->verifyIsolation(&inputUnchanged)) && inputUnchanged &&
                    SUCCEEDED(privateDesktop->visibleWindowsOnInputDesktop(inputVisible)) && !inputVisible &&
                    pairedClipboard == GetClipboardSequenceNumber() && !IsWindowVisible(window_);
                pairedDetail += L"; original view/history=" + std::to_wstring(originalViewRetained) + L"; source/ACL/selection=" +
                    std::to_wstring(sourcesPreserved) + L"; isolation=" + std::to_wstring(isolated) + L"; ms=" + std::to_wstring(GetTickCount64() - pairedStarted);
                check("share_single_file_original_app_site_matches_independent_native_site", pairEquivalent && originalViewRetained && sourcesPreserved && isolated &&
                    GetTickCount64() < pairedDeadline, pairedDetail);
            }
            selectNativeTab(RibbonShareTab);
            PIDLIST_ABSOLUTE directoryRaw = nullptr;
            hr = SHParseDisplayName((fixture / L"Subfolder").c_str(), nullptr, &directoryRaw, 0, nullptr);
            Pidl directoryIdentifier(directoryRaw);
            if (SUCCEEDED(hr)) hr = view_->SelectItem(ILFindLastID(directoryIdentifier.get()), SVSI_SELECT | SVSI_DESELECTOTHERS);
            ready = SUCCEEDED(hr) && pumpUntil([&] {
                ComPtr<IShellItemArray> chosen; DWORD chosenCount = 0;
                return SUCCEEDED(selection(chosen)) && chosen && SUCCEEDED(chosen->GetCount(&chosenCount)) && chosenCount == 1 &&
                    !selectionStateDirty_ && commandDisabled(Sharing);
            }, 5000);
            check("share_disables_directory_selection", ready);
            const auto shareZip = fixture / L"Subfolder" / L"Share fixture.zip";
            {
                constexpr char emptyZip[] = {'P', 'K', 5, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
                std::ofstream archive(shareZip, std::ios::binary); archive.write(emptyZip, sizeof(emptyZip));
            }
            hr = navigate((fixture / L"Subfolder").wstring());
            ready = SUCCEEDED(hr) && pumpUntil([&] { return !navigating_ && atLocation(fixture / L"Subfolder"); }, 5000);
            PIDLIST_ABSOLUTE zipRaw = nullptr;
            if (ready) hr = SHParseDisplayName(shareZip.c_str(), nullptr, &zipRaw, 0, nullptr);
            Pidl zipIdentifier(zipRaw);
            if (ready && SUCCEEDED(hr)) hr = view_->SelectItem(ILFindLastID(zipIdentifier.get()), SVSI_SELECT | SVSI_DESELECTOTHERS);
            ready = ready && SUCCEEDED(hr) && pumpUntil([&] { return !selectionStateDirty_ && commandEnabled(Sharing); }, 5000);
            check("share_accepts_zip_files_despite_shell_folder_attribute", ready);
            selectNativeTab(RibbonHomeTab);
            hr = navigate(shareZip.wstring());
            ready = SUCCEEDED(hr) && pumpUntil([&] { return !navigating_ && atLocation(shareZip); }, 5000);
            for (const auto command : {PasteShortcut, Terminal}) quickAccessModel_.add(command);
            rebuildQuickAccess();
            bool physicalCommandsDisabled = true;
            std::wstring archiveCommandDiagnostic;
            for (const auto command : std::array<UINT, 4>{NewFolder, RibbonNewMenu, PasteShortcut, Terminal}) {
                const bool disabled = commandDisabled(command);
                physicalCommandsDisabled = physicalCommandsDisabled && disabled;
                archiveCommandDiagnostic += L"; command " + std::to_wstring(command) + L" disabled=" + std::to_wstring(disabled ? 1 : 0);
            }
            for (const auto command : {NewText, NewShortcut}) {
                const bool disabled = !ribbonState(command).enabled;
                physicalCommandsDisabled = physicalCommandsDisabled && disabled;
                archiveCommandDiagnostic += L"; host command " + std::to_wstring(command) + L" disabled=" + std::to_wstring(disabled ? 1 : 0);
            }
            check("zip_namespace_disables_physical_directory_commands", ready && !physicalDirectory_ && physicalCommandsDisabled,
                hresultMessage(hr) + L"; physical directory=" + std::to_wstring(physicalDirectory_ ? 1 : 0) + archiveCommandDiagnostic);
            execute(QuickAccessReset);
            navigate(fixture.wstring()); pumpUntil([&] { return !navigating_ && atLocation(fixture); }, 5000);
            HWND nativeView = nullptr; view_->GetWindow(&nativeView);
            RECT expandedRect{}, collapsedRect{};
            GetWindowRect(nativeView, &expandedRect);
            const auto expandedHeight = ribbon_.height();
            execute(Collapse);
            pumpUntil([&] { return ribbon_.height() < expandedHeight; }, 2000);
            GetWindowRect(nativeView, &collapsedRect);
            bool minimized = false;
            check("collapsed_ribbon_reclaims_space", expandedRect.top - collapsedRect.top >= px(80) &&
                SUCCEEDED(ribbon_.minimized(minimized)) && minimized && ribbon_.height() < expandedHeight);
            execute(Collapse);
            pumpUntil([&] { return ribbon_.height() == expandedHeight; }, 2000);
            {
                PrivatePresentation presentation(window_, true);
                UiString minimiseName, minimiseTip, expandName, expandTip;
                const bool collapseStringsReady = SUCCEEDED(loadUiString(UiText::MinimiseRibbon, &minimiseName)) &&
                    SUCCEEDED(loadUiString(UiText::MinimiseRibbonTooltip, &minimiseTip)) &&
                    SUCCEEDED(loadUiString(UiText::ExpandRibbon, &expandName)) &&
                    SUCCEEDED(loadUiString(UiText::ExpandRibbonTooltip, &expandTip));
                check("native_collapse_resource_names", collapseStringsReady);
                const auto desktop = GetThreadDesktop(GetCurrentThreadId());
                auto buttonAccessibility = std::async(std::launch::async,
                    [desktop, host = window_, button = ribbonCollapse_, ready = presentation.ready,
                        caption = minimiseName.text] {
                    AccessibleResult result;
                    struct Apartment {
                        HRESULT status = E_ACCESSDENIED;
                        explicit Apartment(HDESK target) {
                            if (SetThreadDesktop(target)) status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                        }
                        ~Apartment() { if (SUCCEEDED(status)) CoUninitialize(); }
                    } apartment(desktop);
                    ComPtr<IUIAutomation> automation;
                    if (ready && SUCCEEDED(apartment.status))
                        CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation));
                    ComPtr<IUIAutomation2> timeout;
                    if (automation && SUCCEEDED(automation.As(&timeout))) {
                        timeout->put_ConnectionTimeout(1000); timeout->put_TransactionTimeout(1000);
                        timeout->put_AutoSetFocus(FALSE);
                    }
                    result = accessibleElement(automation.Get(), nullptr, button, caption.c_str(), UIA_ButtonControlTypeId);
                    ComPtr<IUIAutomationElement> root;
                    if (automation) automation->ElementFromHandle(host, &root);
                    VARIANT type{}; type.vt = VT_I4; type.lVal = UIA_ButtonControlTypeId;
                    ComPtr<IUIAutomationCondition> condition;
                    ComPtr<IUIAutomationElementArray> buttons;
                    if (automation && SUCCEEDED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, type, &condition)) && root)
                        root->FindAll(TreeScope_Descendants, condition.Get(), &buttons);
                    int count = 0; if (buttons) buttons->get_Length(&count);
                    RECT collapseBounds{}, helpBounds{}; bool helpFound = false;
                    if (result.element) result.element->get_CurrentBoundingRectangle(&collapseBounds);
                    for (int index = 0; buttons && index < std::min(count, 128); ++index) {
                        ComPtr<IUIAutomationElement> candidate; BSTR label = nullptr;
                        if (FAILED(buttons->GetElement(index, &candidate)) || !candidate) continue;
                        candidate->get_CurrentName(&label);
                        if (label && (wcscmp(label, L"Help") == 0 || wcscmp(label, L"Help (F1)") == 0)) {
                            BOOL offscreen = TRUE;
                            candidate->get_CurrentIsOffscreen(&offscreen);
                            helpFound = !offscreen && SUCCEEDED(candidate->get_CurrentBoundingRectangle(&helpBounds)) &&
                                helpBounds.right > helpBounds.left && helpBounds.bottom > helpBounds.top;
                        }
                        SysFreeString(label);
                        if (helpFound) break;
                    }
                    RECT overlap{};
                    const bool overlaps = IntersectRect(&overlap, &collapseBounds, &helpBounds) != FALSE;
                    result.passed = result.passed && helpFound && !overlaps && collapseBounds.right <= helpBounds.left &&
                        collapseBounds.right - collapseBounds.left == MulDiv(22, static_cast<int>(GetDpiForWindow(host)), 96);
                    result.detail += L"; native Help found=" + std::to_wstring(helpFound) + L"; intersects Help=" +
                        std::to_wstring(overlaps) + L"; collapse width=" + std::to_wstring(collapseBounds.right - collapseBounds.left);
                    result.element.Reset(); // Release the provider on its MTA, before CoUninitialize.
                    return result;
                });
                const bool accessibleReady = pumpUntil([&] {
                    return buttonAccessibility.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
                }, 5000);
                const auto accessibleButton = buttonAccessibility.get();
                check("native_collapse_button_accessible_and_unobstructed", accessibleReady && accessibleButton.passed,
                    accessibleButton.detail);
                VisualCaptureOptions caretOptions;
                caretOptions.includeFrame = false;
                caretOptions.requireVisibleChildren = false;
                caretOptions.layoutDpi = dpi_;
                // A single monochrome caret needs foreground and background,
                // rather than the twelve colors required of a complete frame.
                caretOptions.minimumUniqueColors = 2;
                caretOptions.pixelInspectionBounds = {px(4),px(4),px(18),px(20)};
                const bool caretPainted=presentation.ready&&RedrawWindow(window_,nullptr,nullptr,
                    RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_ALLCHILDREN|RDW_UPDATENOW)&&GdiFlush();
                VisualCaptureReport caretCapture;
                const auto caretRead = caretPainted && PrivateDesktop::current() ?
                    captureWindowPng(*PrivateDesktop::current(), ribbonCollapse_,
                        fixture / L"native-collapse-caret.png", caretOptions, caretCapture) : E_ACCESSDENIED;
                RECT frameCaret{};GetWindowRect(ribbonCollapse_,&frameCaret);
                MapWindowPoints(nullptr,window_,reinterpret_cast<POINT*>(&frameCaret),2);
                InflateRect(&frameCaret,-px(4),-px(4));
                VisualCaptureOptions frameCaretOptions;frameCaretOptions.includeFrame=false;
                frameCaretOptions.pixelInspectionBounds=frameCaret;
                VisualCaptureReport frameCaretCapture;
                const auto frameCaretRead=presentation.ready&&PrivateDesktop::current() ?
                    captureWindowPng(*PrivateDesktop::current(),window_,fixture/L"native-collapse-frame.png",
                        frameCaretOptions,frameCaretCapture):E_ACCESSDENIED;
                check("native_collapse_button_prints_real_caret", SUCCEEDED(caretRead) &&
                    caretCapture.width == static_cast<unsigned>(px(22)) &&
                    caretCapture.height == static_cast<unsigned>(px(24)) && caretCapture.uniqueColors >= 2 &&
                    caretCapture.inkFraction >= caretOptions.minimumInkFraction && caretCapture.printWindowSucceeded &&
                    caretCapture.inspectionUniqueColors >= 2 && caretCapture.inspectionInkFraction >= 0.01 &&
                    SUCCEEDED(frameCaretRead)&&frameCaretCapture.inspectionUniqueColors>=2&&
                    frameCaretCapture.inspectionInkFraction>=0.01,
                    L"Native PrintWindow=" + hresultMessage(caretRead) + L"; colors=" +
                    std::to_wstring(caretCapture.uniqueColors) + L"; ink=" + std::to_wstring(caretCapture.inkFraction) +
                    L"; central colors=" + std::to_wstring(caretCapture.inspectionUniqueColors) +
                    L"; central ink=" + std::to_wstring(caretCapture.inspectionInkFraction)+
                    L"; whole-frame caret="+hresultMessage(frameCaretRead)+L"/"+
                    std::to_wstring(frameCaretCapture.inspectionUniqueColors)+L"/"+
                    std::to_wstring(frameCaretCapture.inspectionInkFraction));
                auto tooltipText = [&] {
                    std::wstring buffer(std::max(minimiseTip.text.size(), expandTip.text.size()) + 1, L'\0');
                    TOOLINFOW tool{sizeof(tool)}; tool.hwnd = window_;
                    tool.uId = reinterpret_cast<UINT_PTR>(ribbonCollapse_); tool.lpszText = buffer.data();
                    if (ribbonCollapseTooltip_) SendMessageW(ribbonCollapseTooltip_, TTM_GETTEXTW, buffer.size(), reinterpret_cast<LPARAM>(&tool));
                    return std::wstring(buffer.c_str());
                };
                const bool initialButton = collapseStringsReady && presentation.ready && GetDlgCtrlID(ribbonCollapse_) == Collapse &&
                    textOf(ribbonCollapse_) == minimiseName.text && tooltipText() == minimiseTip.text;
                if (presentation.ready) SendMessageW(ribbonCollapse_, BM_CLICK, 0, 0);
                bool collapsed = false;
                const bool clickedMinimize = pumpUntil([&] {
                    return SUCCEEDED(ribbon_.minimized(collapsed)) && collapsed && ribbon_.height() < expandedHeight;
                }, 2000);
                const bool minimizedButton = textOf(ribbonCollapse_) == expandName.text &&
                    tooltipText() == expandTip.text && preferences_.ribbonCollapsed;
                if (presentation.ready) SendMessageW(ribbonCollapse_, BM_CLICK, 0, 0);
                const bool clickedExpand = pumpUntil([&] {
                    return SUCCEEDED(ribbon_.minimized(collapsed)) && !collapsed && ribbon_.height() == expandedHeight;
                }, 2000);
                const bool expandedButton = textOf(ribbonCollapse_) == minimiseName.text &&
                    tooltipText() == minimiseTip.text && !preferences_.ribbonCollapsed;
                check("native_collapse_button_click_and_tooltip", initialButton && clickedMinimize && minimizedButton &&
                    clickedExpand && expandedButton,
                    L"Initial label/tooltip=" + std::to_wstring(initialButton) + L"; native minimize=" +
                    std::to_wstring(clickedMinimize) + L"; minimized label/tooltip=" + std::to_wstring(minimizedButton) +
                    L"; native expand=" + std::to_wstring(clickedExpand) + L"; expanded label/tooltip=" + std::to_wstring(expandedButton));
            }
            {
                const bool initiallyIsolated = headless_ && !typedAddressHistoryLoaded_ && typedAddresses_.empty();
                const auto originalSessionIndex = historyIndex_;
                const auto typedFolder = fixture / L"Typed-&-\u65e5\u672c\u8a9e";
                std::filesystem::create_directory(typedFolder);
                const auto environmentName = L"WINDOWS_EXPLORER_SMOKE_TYPED_" + std::to_wstring(GetCurrentProcessId()) +
                    L"_" + std::to_wstring(GetTickCount64());
                struct OwnedEnvironment {
                    std::wstring name;
                    bool set = false;
                    ~OwnedEnvironment() { if (set) SetEnvironmentVariableW(name.c_str(), nullptr); }
                } environment{environmentName, SetEnvironmentVariableW(environmentName.c_str(), fixture.c_str()) != FALSE};
                const auto originalSpelling = L"%" + environmentName + L"%\\Typed-&-\u65e5\u672c\u8a9e";
                const auto typedRead = environment.set ? navigate(originalSpelling, true) : HRESULT_FROM_WIN32(GetLastError());
                const bool typedReady = SUCCEEDED(typedRead) && pumpUntil([&] {
                    return !navigating_ && atLocation(typedFolder) && pendingTypedAddress_.empty() && !pendingTypedAddressTarget_;
                }, 5000);
                Pidl typedHistoryTarget(typedReady ? ILCloneFull(currentPidl_.get()) : nullptr);
                const auto typedHistoryIndex = historyIndex_;
                const std::vector<std::wstring> expected{originalSpelling};
                check("typed_address_preserves_original_environment_and_unicode", initiallyIsolated && typedReady &&
                    typedAddresses_ == expected && !typedAddressHistoryLoaded_,
                    L"Owned successful native navigation=" + hresultMessage(typedRead) +
                    L"; original environment syntax/Unicode/ampersand retained=" + std::to_wstring(typedAddresses_ == expected) +
                    L"; shared history imported=0");
                // An existing owned file is not a folder. Its typed activation
                // is denied headlessly, with no pending entry or navigation.
                const auto failedRead = navigate((fixture / L"file-0.txt").wstring(), true);
                check("typed_address_failed_navigation_does_not_add_history", FAILED(failedRead) &&
                    typedAddresses_ == expected && pendingTypedAddress_.empty() && !pendingTypedAddressTarget_ &&
                    atLocation(typedFolder), hresultMessage(failedRead));
                const auto ordinaryRead = navigate(fixture.wstring());
                const bool ordinaryReady = SUCCEEDED(ordinaryRead) && pumpUntil([&] {
                    return !navigating_ && atLocation(fixture);
                }, 5000);
                struct OwnedMenu { HMENU value = nullptr; ~OwnedMenu() { if (value) DestroyMenu(value); } } menu{createTypedAddressMenu()};
                MENUITEMINFOW entry{sizeof(entry)}; entry.fMask = MIIM_STRING | MIIM_ID;
                auto menuRead = menu.value && GetMenuItemInfoW(menu.value, 0, TRUE, &entry);
                std::wstring nativeLabel(static_cast<size_t>(entry.cch) + 1, L'\0');
                if (menuRead) {
                    entry.dwTypeData = nativeLabel.data(); entry.cch = static_cast<UINT>(nativeLabel.size());
                    menuRead = GetMenuItemInfoW(menu.value, 0, TRUE, &entry) != FALSE;
                    nativeLabel.resize(entry.cch);
                }
                std::wstring expectedLabel;
                for (const auto character : originalSpelling) {
                    expectedLabel += character;
                    if (character == L'&') expectedLabel += character;
                }
                check("typed_address_native_menu_is_distinct_from_session_history", ordinaryReady &&
                    typedAddresses_ == expected && historyIndex_ >= originalSessionIndex + 2 &&
                    history_.size() > typedAddresses_.size() &&
                    menu.value && GetMenuItemCount(menu.value) == 1 && menuRead && entry.wID != 0 &&
                    nativeLabel == expectedLabel,
                    L"Actual HMENU rows=" + std::to_wstring(menu.value ? GetMenuItemCount(menu.value) : -1) +
                    L"; native original-spelling label=" + std::to_wstring(menuRead && nativeLabel == expectedLabel) +
                    L"; non-typed navigation added to typed MRU=" + std::to_wstring(typedAddresses_ != expected));
                {
                    AddressContextSnapshot snapshot;
                    const auto snapshotRead = addressContextSnapshot(&snapshot);
                    OwnedMenu addressMenu;
                    const auto addressMenuRead = createAddressContextMenu(&addressMenu.value);
                    wchar_t system[MAX_PATH]{}; GetSystemDirectoryW(system, static_cast<UINT>(std::size(system)));
                    const auto module = LoadLibraryExW((std::filesystem::path(system) / L"explorerframe.dll").c_str(),
                        nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
                    OwnedMenu reference{module ? LoadMenuW(module, MAKEINTRESOURCEW(272)) : nullptr};
                    const auto referencePopup = reference.value ? GetSubMenu(reference.value, 0) : nullptr;
                    bool exactNativeMenu = SUCCEEDED(addressMenuRead) && referencePopup &&
                        GetMenuItemCount(addressMenu.value) == 4 && GetMenuItemCount(referencePopup) == 4;
                    constexpr std::array nativeCommands{AddressContextCommand::Copy, AddressContextCommand::CopyText,
                        AddressContextCommand::Edit, AddressContextCommand::DeleteHistory};
                    for (UINT index = 0; exactNativeMenu && index < nativeCommands.size(); ++index) {
                        wchar_t actual[256]{}, native[256]{};
                        GetMenuStringW(addressMenu.value, index, actual, static_cast<int>(std::size(actual)), MF_BYPOSITION);
                        GetMenuStringW(referencePopup, index, native, static_cast<int>(std::size(native)), MF_BYPOSITION);
                        exactNativeMenu = GetMenuItemID(addressMenu.value, index) == static_cast<UINT>(nativeCommands[index]) &&
                            GetMenuItemID(referencePopup, index) == static_cast<UINT>(nativeCommands[index]) &&
                            *native && std::wstring(actual) == native &&
                            !(GetMenuState(addressMenu.value, index, MF_BYPOSITION) & MF_GRAYED);
                    }
                    if (module) FreeLibrary(module);
                    check("address_context_menu_matches_actual_native_resource_commands_and_labels", exactNativeMenu &&
                        SUCCEEDED(snapshotRead) && snapshot.location && ILIsEqual(snapshot.location.get(), currentPidl_.get()) &&
                        snapshot.text == fixture.wstring(), L"Installed explorerframe.dll RT_MENU 272; native actions=4; retained PIDL/text=1");
                    const auto beforeClipboard = GetClipboardSequenceNumber();
                    const auto beforeHistory = typedAddresses_;
                    const auto copyGuard = executeAddressContext(AddressContextCommand::Copy, snapshot);
                    const auto textGuard = executeAddressContext(AddressContextCommand::CopyText, snapshot);
                    const auto historyGuard = executeAddressContext(AddressContextCommand::DeleteHistory, snapshot);
                    const auto popupGuard = showAddressContextMenu({-1, -1});
                    check("address_context_headless_guards_preserve_clipboard_and_history", copyGuard == E_ACCESSDENIED &&
                        textGuard == E_ACCESSDENIED && historyGuard == E_ACCESSDENIED && popupGuard == E_ACCESSDENIED &&
                        GetClipboardSequenceNumber() == beforeClipboard && typedAddresses_ == beforeHistory &&
                        !typedAddressHistoryLoaded_ && atLocation(fixture));
                    PrivatePresentation presentation(window_, true);
                    const auto edit = executeAddressContext(AddressContextCommand::Edit, snapshot);
                    check("address_context_edit_routes_to_owned_address_control", presentation.ready && SUCCEEDED(edit) &&
                        GetFocus() == address_ && addressEditing_ && textOf(address_) == snapshot.text);
                    finishAddress(false);
                    bool inputUnchanged = false, visible = true;
                    check("address_context_checks_preserve_private_desktop_isolation", presentation.desktop &&
                        SUCCEEDED(presentation.desktop->verifyIsolation(&inputUnchanged)) && inputUnchanged &&
                        SUCCEEDED(presentation.desktop->visibleWindowsOnInputDesktop(visible)) && !visible);
                }
                {
                    const auto firstBack = execute(Back);
                    bool replacedIndex = SUCCEEDED(firstBack) && pumpUntil([&] {
                        return !navigating_ && atLocation(typedFolder);
                    }, 5000);
                    const auto secondBack = execute(Back);
                    replacedIndex = replacedIndex && SUCCEEDED(secondBack) && pumpUntil([&] {
                        return !navigating_ && atLocation(fixture);
                    }, 5000);
                    const auto branch = navigate((fixture / L"Subfolder").wstring());
                    replacedIndex = replacedIndex && SUCCEEDED(branch) && pumpUntil([&] {
                        return !navigating_ && atLocation(fixture / L"Subfolder");
                    }, 5000) && typedHistoryTarget && typedHistoryIndex >= 0 && typedHistoryIndex < static_cast<int>(history_.size()) &&
                        !ILIsEqual(history_[typedHistoryIndex].get(), typedHistoryTarget.get());
                    const auto snapshotBrowse = typedHistoryTarget ? browseHistoryLocation(typedHistoryTarget.get(), typedHistoryIndex) : E_UNEXPECTED;
                    const bool restoredTarget = SUCCEEDED(snapshotBrowse) && pumpUntil([&] {
                        return !navigating_ && atLocation(typedFolder);
                    }, 5000);
                    check("history_popup_snapshot_retains_exact_target_after_real_forward_truncation", replacedIndex &&
                        restoredTarget && currentPidl_ && ILIsEqual(currentPidl_.get(), typedHistoryTarget.get()) &&
                        pendingHistory_ == -1 && typedAddresses_ == expected);
                    const auto restore = navigate(fixture.wstring());
                    check("history_snapshot_fixture_returns_to_origin", SUCCEEDED(restore) && pumpUntil([&] {
                        return !navigating_ && atLocation(fixture);
                    }, 5000));
                }
                const auto commandRead = navigate(L"cmd.exe /c exit 0", true);
                const auto uriRead = navigate(L"native-explorer-smoke-no-handler://owned", true);
                check("typed_address_headless_blocks_command_and_uri_launch", commandRead == E_ACCESSDENIED &&
                    uriRead == E_ACCESSDENIED && typedAddresses_ == expected && pendingTypedAddress_.empty() &&
                    !pendingTypedAddressTarget_ && atLocation(fixture),
                    L"Command=" + hresultMessage(commandRead) + L"; URI=" + hresultMessage(uriRead) +
                    L"; native command/URI invocation=0");
            }
            {
                // Arbitrary dates travel through the real EDIT/Enter route and
                // the native ItemsView. Exact local-day 100ns boundaries and a
                // matching file outside the scope expose parser/scope mistakes.
                const auto dateFolder = fixture / L"Subfolder" / L"Typed date search";
                std::filesystem::create_directory(dateFolder);
                HRESULT dateFixtureRead = S_OK;
                auto localTicks = [&](WORD month, WORD day) {
                    SYSTEMTIME local{}, utc{}; FILETIME native{};
                    local.wYear = 2024; local.wMonth = month; local.wDay = day;
                    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc) || !SystemTimeToFileTime(&utc, &native)) {
                        dateFixtureRead = HRESULT_FROM_WIN32(GetLastError()); return ULONGLONG{};
                    }
                    return (static_cast<ULONGLONG>(native.dwHighDateTime) << 32) | native.dwLowDateTime;
                };
                const auto leapStart = localTicks(2, 29);
                const auto marchStart = localTicks(3, 1);
                const auto rangeEnd = localTicks(3, 3);
                struct DateMember { std::filesystem::path path; ULONGLONG ticks; NativeFileIdentity identity; };
                std::array<DateMember, 6> members{{
                    {dateFolder / L"before.bin", leapStart - 1, {}},
                    {dateFolder / L"day-start.bin", leapStart, {}},
                    {dateFolder / L"day-end.bin", marchStart - 1, {}},
                    {dateFolder / L"range-end.bin", rangeEnd - 1, {}},
                    {dateFolder / L"after.bin", rangeEnd, {}},
                    {fixture / L"Subfolder" / L"date-outside.bin", leapStart, {}}}};
                constexpr char dateContents[] = "owned typed-date fixture";
                std::set<NativeFileIdentity> allDates, dayDates, rangeDates;
                for (size_t index = 0; index < members.size(); ++index) {
                    auto& member = members[index];
                    { std::ofstream output(member.path, std::ios::binary); output << dateContents; }
                    const auto file = CreateFileW(member.path.c_str(), FILE_WRITE_ATTRIBUTES | FILE_READ_ATTRIBUTES,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                    HRESULT stampRead = file == INVALID_HANDLE_VALUE ? HRESULT_FROM_WIN32(GetLastError()) : S_OK;
                    FILETIME actual{};
                    const FILETIME requested{static_cast<DWORD>(member.ticks), static_cast<DWORD>(member.ticks >> 32)};
                    if (SUCCEEDED(stampRead) && (!SetFileTime(file, nullptr, nullptr, &requested) ||
                        !GetFileTime(file, nullptr, nullptr, &actual))) stampRead = HRESULT_FROM_WIN32(GetLastError());
                    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
                    if (SUCCEEDED(stampRead) && (actual.dwHighDateTime != requested.dwHighDateTime ||
                        actual.dwLowDateTime != requested.dwLowDateTime)) stampRead = E_UNEXPECTED;
                    FILE_ID_INFO identity{};
                    if (SUCCEEDED(stampRead)) stampRead = nativeFileIdentity(member.path, identity);
                    if (FAILED(stampRead)) dateFixtureRead = stampRead;
                    member.identity.first = identity.VolumeSerialNumber;
                    std::copy(std::begin(identity.FileId.Identifier), std::end(identity.FileId.Identifier), member.identity.second.begin());
                    if (index < 5) allDates.insert(member.identity);
                    if (index == 1 || index == 2) dayDates.insert(member.identity);
                    if (index >= 1 && index <= 3) rangeDates.insert(member.identity);
                }
                HRESULT dateViewRead = E_PENDING; DWORD dateCount = 0;
                auto exactDateView = [&](const std::set<NativeFileIdentity>& expected) {
                    if (navigating_ || !folderView_) return false;
                    ComPtr<IShellItemArray> items; std::set<NativeFileIdentity> identities;
                    dateViewRead = folderView_->Items(SVGIO_ALLVIEW, IID_PPV_ARGS(&items));
                    if (SUCCEEDED(dateViewRead) && items) dateViewRead = items->GetCount(&dateCount);
                    if (SUCCEEDED(dateViewRead)) dateViewRead = nativeArrayIdentities(items.Get(), identities);
                    return SUCCEEDED(dateViewRead) && dateCount == expected.size() && identities == expected;
                };
                const auto dateNavigation = navigate(dateFolder.wstring());
                const bool dateOriginReady = SUCCEEDED(dateFixtureRead) && SUCCEEDED(dateNavigation) && pumpUntil([&] {
                    return atLocation(dateFolder) && exactDateView(allDates);
                }, 5000);
                Pidl dateScope(ILCloneFull(currentPidl_.get()));
                const std::wstring dayQuery = L"System.DateModified:=2024-02-29";
                SetWindowTextW(search_, dayQuery.c_str()); SendMessageW(search_, WM_KEYDOWN, VK_RETURN, 0);
                const bool dayReady = dateOriginReady && pumpUntil([&] {
                    return searchActive_ && activeQuery_ == dayQuery && textOf(search_) == dayQuery &&
                        !pendingLiveSearch_ && !liveSearchPolicy_.waiting() && exactDateView(dayDates);
                }, 5000);
                check("typed_date_enter_returns_exact_leap_day_boundaries", dayReady && searchScope_ &&
                    ILIsEqual(searchScope_.get(), dateScope.get()) && std::count(recentSearches_.begin(), recentSearches_.end(), dayQuery) == 1,
                    L"fixture=" + hresultMessage(dateFixtureRead) + L"; view=" + hresultMessage(dateViewRead) +
                    L"; exact native files=" + std::to_wstring(dateCount));
                const std::wstring rangeQuery = L"System.DateModified:2024-02-29..2024-03-02";
                SetWindowTextW(search_, rangeQuery.c_str()); SendMessageW(search_, WM_KEYDOWN, VK_RETURN, 0);
                const bool rangeReady = dayReady && pumpUntil([&] {
                    return searchActive_ && activeQuery_ == rangeQuery && textOf(search_) == rangeQuery &&
                        !pendingLiveSearch_ && !liveSearchPolicy_.waiting() && exactDateView(rangeDates);
                }, 5000);
                check("typed_date_range_enter_is_inclusive_and_keeps_exact_scope", rangeReady && searchScope_ &&
                    ILIsEqual(searchScope_.get(), dateScope.get()) && searchBase_ == rangeQuery &&
                    std::all_of(searchFilters_.begin(), searchFilters_.end(), [](const auto& filter) { return filter.empty(); }) &&
                    std::count(recentSearches_.begin(), recentSearches_.end(), rangeQuery) == 1,
                    L"view=" + hresultMessage(dateViewRead) + L"; exact native files=" + std::to_wstring(dateCount) +
                    L"; matching outside-scope identity excluded");
                const auto shownRecentQueries = ribbonItems(RecentSearches);
                const std::wstring newerRecentQuery = L"System.FileName:=\"before.bin\"";
                // The real commit hook invalidates the collection, but the
                // currently displayed row can still be selected before its
                // replacement ItemsSource callback runs on this same STA.
                rememberQuery(newerRecentQuery);
                const bool movedRecentIndex = !shownRecentQueries.empty() && shownRecentQueries.front().label == rangeQuery &&
                    !recentSearches_.empty() && recentSearches_.front() == newerRecentQuery;
                Pidl displayedDateResult(ILCloneFull(currentPidl_.get()));
                const auto dateResultHistory = historyIndex_;
                const auto dateResultHistorySize = history_.size();
                const auto dateResultNavigationCount = navigationCount_;
                const auto recentSelected = executeRibbonItem(RecentSearches, 0);
                check("recent_search_ribbon_selection_retains_displayed_literal_after_mru_reorder", movedRecentIndex &&
                    SUCCEEDED(recentSelected) && pumpUntil([&] {
                        return searchActive_ && activeQuery_ == rangeQuery && textOf(search_) == rangeQuery && exactDateView(rangeDates);
                    }, 5000) && currentPidl_ && ILIsEqual(currentPidl_.get(), displayedDateResult.get()) &&
                    historyIndex_ == dateResultHistory && history_.size() == dateResultHistorySize &&
                    navigationCount_ == dateResultNavigationCount && !pendingDirectSearchTarget_,
                    L"displayed query=" + rangeQuery + L"; newer first MRU=" + newerRecentQuery +
                    L"; selection=" + hresultMessage(recentSelected));
                const auto dateClosed = execute(CloseSearch);
                check("typed_date_close_restores_exact_owned_files", SUCCEEDED(dateClosed) && pumpUntil([&] {
                    return !searchActive_ && atLocation(dateFolder) && exactDateView(allDates);
                }, 5000), L"Close=" + hresultMessage(dateClosed) + L"; view=" + hresultMessage(dateViewRead) +
                    L"; actual files=" + std::to_wstring(dateCount) + L"; navigating=" + std::to_wstring(navigating_) +
                    L"; current=" + currentLocation_);
                bool dateFilesPreserved = true;
                for (const auto& member : members) {
                    FILE_ID_INFO identity{}; FILETIME actual{};
                    const auto file = CreateFileW(member.path.c_str(), FILE_READ_ATTRIBUTES,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                    const bool read = file != INVALID_HANDLE_VALUE && GetFileTime(file, nullptr, nullptr, &actual);
                    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
                    std::ifstream input(member.path, std::ios::binary);
                    const std::string contents{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
                    dateFilesPreserved = dateFilesPreserved && read && SUCCEEDED(nativeFileIdentity(member.path, identity)) &&
                        identity.VolumeSerialNumber == member.identity.first &&
                        std::equal(std::begin(identity.FileId.Identifier), std::end(identity.FileId.Identifier), member.identity.second.begin()) &&
                        ((static_cast<ULONGLONG>(actual.dwHighDateTime) << 32) | actual.dwLowDateTime) == member.ticks &&
                        contents == dateContents;
                }
                check("typed_date_search_preserves_owned_identities_contents_and_timestamps", dateFilesPreserved);
                const auto dateRestore = navigate(fixture.wstring());
                check("typed_date_fixture_returns_to_origin", SUCCEEDED(dateRestore) && pumpUntil([&] {
                    return !navigating_ && atLocation(fixture);
                }, 5000), L"Browse=" + hresultMessage(dateRestore) + L"; navigating=" + std::to_wstring(navigating_) +
                    L"; current=" + currentLocation_);
            }
            {
                // Real EDIT notifications, timer dispatch and native results:
                // late-created files keep the earlier 1,002-item fixture exact.
                const auto liveFolder=fixture/L"Subfolder"/L"Live search";
                std::filesystem::create_directory(liveFolder);
                const std::array<std::wstring,3> liveNames{
                    L"one-\u65e5\u672c\u8a9e.txt",L"two-\u00e9.txt",L"three-\u03a9.txt"};
                std::array<std::set<NativeFileIdentity>,3> liveIdentities;
                std::set<NativeFileIdentity> allLiveIdentities;
                HRESULT liveFixtureRead=S_OK;
                for(size_t index=0;index<liveNames.size();++index) {
                    const auto file=liveFolder/liveNames[index];std::ofstream(file)<<"owned live-search fixture";
                    FILE_ID_INFO identity{};const auto identityRead=nativeFileIdentity(file,identity);
                    if(FAILED(identityRead))liveFixtureRead=identityRead;
                    std::array<BYTE,16> bytes{};std::copy(std::begin(identity.FileId.Identifier),std::end(identity.FileId.Identifier),bytes.begin());
                    liveIdentities[index].insert({identity.VolumeSerialNumber,bytes});
                    allLiveIdentities.insert({identity.VolumeSerialNumber,bytes});
                }
                auto queryFor=[&](size_t index){return L"System.FileName:=\""+liveNames[index]+L"\"";};
                HRESULT liveViewRead=E_PENDING;DWORD liveItems=0;
                auto exactLiveView=[&](const std::set<NativeFileIdentity>& expected) {
                    if(navigating_||!folderView_)return false;
                    ComPtr<IShellItemArray> items;std::set<NativeFileIdentity> identities;
                    liveViewRead=folderView_->Items(SVGIO_ALLVIEW,IID_PPV_ARGS(&items));
                    if(SUCCEEDED(liveViewRead)&&items)liveViewRead=items->GetCount(&liveItems);
                    if(SUCCEEDED(liveViewRead))liveViewRead=nativeArrayIdentities(items.Get(),identities);
                    return SUCCEEDED(liveViewRead)&&liveItems==expected.size()&&identities==expected;
                };
                auto liveDetail=[&] {
                    return L"factory="+hresultMessage(liveSearchStatus_)+L"; view="+hresultMessage(liveViewRead)+
                        L"; actualItems="+std::to_wstring(liveItems)+L"; pending="+std::to_wstring(pendingLiveSearch_.has_value())+
                        L"; waiting="+std::to_wstring(liveSearchPolicy_.waiting())+L"; dispatch="+
                        std::to_wstring(liveSearchDispatchActive_)+L"; historyIndex="+std::to_wstring(historyIndex_);
                };
                auto liveCheck=[&](const char* name,bool passed) {
                    std::fprintf(stderr,"headless-live check=%s passed=%u factory=0x%08lX view=0x%08lX items=%lu pending=%u waiting=%u dispatch=%u history_index=%d history_size=%zu recent=%zu\n",
                        name,passed?1u:0u,static_cast<unsigned long>(liveSearchStatus_),static_cast<unsigned long>(liveViewRead),liveItems,
                        pendingLiveSearch_?1u:0u,liveSearchPolicy_.waiting()?1u:0u,liveSearchDispatchActive_?1u:0u,
                        historyIndex_,history_.size(),recentSearches_.size());std::fflush(stderr);
                    check(name,passed,liveDetail());
                };
                auto liveNavigation=navigate(liveFolder.wstring());
                const bool liveOriginReady=SUCCEEDED(liveFixtureRead)&&SUCCEEDED(liveNavigation)&&pumpUntil([&] {
                    return atLocation(liveFolder)&&exactLiveView(allLiveIdentities);
                },5000);
                Pidl liveOrigin(ILCloneFull(currentPidl_.get()));
                const auto originHistoryIndex=historyIndex_;
                const auto recentBeforeLive=recentSearches_;
                const auto beforeLiveNavigation=navigationCount_;
                SetWindowTextW(search_,queryFor(0).c_str());SetWindowTextW(search_,queryFor(1).c_str());
                SetWindowTextW(search_,queryFor(2).c_str());
                const bool reallyDebounced=liveSearchPolicy_.waiting()&&liveSearchPolicy_.deadline()&&
                    *liveSearchPolicy_.deadline()>GetTickCount64()&&!pendingLiveSearch_&&navigationCount_==beforeLiveNavigation;
                const bool automaticReady=liveOriginReady&&pumpUntil([&] {
                    return searchActive_&&activeQuery_==queryFor(2)&&textOf(search_)==queryFor(2)&&
                        !liveSearchPolicy_.waiting()&&!pendingLiveSearch_&&exactLiveView(liveIdentities[2]);
                },5000);
                const auto liveHistorySize=history_.size();
                liveCheck("live_search_edit_burst_debounces_to_exact_unicode_file",reallyDebounced&&automaticReady&&
                    recentSearches_==recentBeforeLive&&historyIndex_==originHistoryIndex+1&&
                    liveHistorySize==static_cast<size_t>(originHistoryIndex+2)&&liveSearchOrigin_&&
                    ILIsEqual(liveSearchOrigin_.get(),liveOrigin.get()));

                const auto beforeGuardedEnter=navigationCount_;
                const auto guardedEnterHistory=history_.size();
                const auto guardedEnterError=lastError_;
                const auto guardedTypedAddresses=typedAddresses_;
                const auto guardedQueryContexts=std::count_if(searchLocations_.begin(),searchLocations_.end(),
                    [&](const SearchLocation& value){return value.query==queryFor(0);});
                bool guardedEnterQueued=false,guardedEnterHeld=false;
                {
                    CommandRefreshScope refresh(*this);
                    SetWindowTextW(search_,queryFor(0).c_str());
                    const auto automaticDeadline=liveSearchPolicy_.deadline();
                    const auto guardedEnterResult=execute(Search);
                    showError(guardedEnterResult,L"Search");
                    const auto explicitDeadline=liveSearchPolicy_.deadline();
                    guardedEnterQueued=SUCCEEDED(guardedEnterResult)&&liveSearchPolicy_.waiting()&&
                        automaticDeadline&&*automaticDeadline>GetTickCount64()&&explicitDeadline&&
                        *explicitDeadline<=GetTickCount64()&&liveSearchPolicy_.literal()==queryFor(0);
                    const auto guardedUntil=GetTickCount64()+300;
                    bool remainedBlocked=true;
                    const bool observedGuard=pumpUntil([&] {
                        remainedBlocked=remainedBlocked&&commandRefreshActive_&&liveSearchPolicy_.waiting()&&
                            !navigating_&&!pendingLiveSearch_&&!pendingLiveSearchTarget_&&!pendingDirectSearchTarget_&&
                            navigationCount_==beforeGuardedEnter&&activeQuery_==queryFor(2)&&
                            history_.size()==guardedEnterHistory&&recentSearches_==recentBeforeLive&&
                            typedAddresses_==guardedTypedAddresses&&lastError_==guardedEnterError;
                        return GetTickCount64()>=guardedUntil;
                    },500);
                    guardedEnterHeld=observedGuard&&remainedBlocked&&exactLiveView(liveIdentities[2])&&
                        std::count_if(searchLocations_.begin(),searchLocations_.end(),
                            [&](const SearchLocation& value){return value.query==queryFor(0);})==guardedQueryContexts;
                }
                const bool enterWasPending=pendingLiveSearch_&&pendingLiveSearch_->explicitSubmit;
                const bool uncommittedUntilCompletion=!enterWasPending||recentSearches_==recentBeforeLive;
                const bool explicitReady=pumpUntil([&] {
                    return activeQuery_==queryFor(0)&&textOf(search_)==queryFor(0)&&!pendingLiveSearch_&&
                        !liveSearchPolicy_.waiting()&&exactLiveView(liveIdentities[0]);
                },5000);
                liveCheck("live_search_enter_during_command_refresh_keeps_explicit_intent_without_nested_navigation",
                    automaticReady&&guardedEnterQueued&&guardedEnterHeld&&explicitReady&&
                    liveSearchPolicy_.committedLiteral()==queryFor(0)&&
                    recentSearches_.size()==recentBeforeLive.size()+1&&
                    std::count(recentSearches_.begin(),recentSearches_.end(),queryFor(0))==1&&
                    history_.size()==liveHistorySize&&typedAddresses_==guardedTypedAddresses&&lastError_==guardedEnterError);
                const auto afterExplicit=recentSearches_;
                const auto beforeIdenticalEnter=navigationCount_;
                SendMessageW(search_,WM_KEYDOWN,VK_RETURN,0);
                const bool resubmitReady=pumpUntil([&] {return !navigating_&&!pendingLiveSearch_&&!liveSearchPolicy_.waiting();},5000);
                bool identicalEnterStayedCurrent=resubmitReady;
                const auto identicalEnterDeadline=GetTickCount64()+200;
                const bool identicalEnterObserved=pumpUntil([&] {
                    identicalEnterStayedCurrent=identicalEnterStayedCurrent&&!navigating_&&!pendingLiveSearch_&&
                        !pendingLiveSearchTarget_&&!pendingDirectSearchTarget_&&navigationCount_==beforeIdenticalEnter;
                    return GetTickCount64()>=identicalEnterDeadline;
                },500);
                liveCheck("live_search_enter_commits_once_after_native_completion",uncommittedUntilCompletion&&explicitReady&&
                    liveSearchPolicy_.committedLiteral()==queryFor(0)&&resubmitReady&&recentSearches_==afterExplicit&&
                    std::count(recentSearches_.begin(),recentSearches_.end(),queryFor(0))==1&&
                    afterExplicit.size()==recentBeforeLive.size()+1&&history_.size()==liveHistorySize);
                liveCheck("live_search_identical_enter_reuses_actual_current_query_without_queued_navigation",
                    explicitReady&&identicalEnterObserved&&identicalEnterStayedCurrent&&
                    activeQuery_==queryFor(0)&&textOf(search_)==queryFor(0)&&exactLiveView(liveIdentities[0])&&
                    recentSearches_==afterExplicit&&history_.size()==liveHistorySize&&
                    liveSearchPolicy_.committedLiteral()==queryFor(0));

                // Different literal spelling may describe the very same
                // genuine native condition/scope. Commit its intent before
                // issuing Browse, retaining the actual view and Back identity.
                const auto equivalentQuery=L"system.filename:=\""+liveNames[0]+L"\"";
                const auto equivalentNavigation=navigationCount_;
                const auto equivalentHistory=history_.size();
                const auto equivalentHistoryIndex=historyIndex_;
                const auto equivalentRecent=recentSearches_;
                Pidl equivalentCurrent(ILCloneFull(currentPidl_.get())),equivalentScope(ILCloneFull(searchScope_.get()));
                const auto equivalentScopes=searchScopes_;
                const auto equivalentRules=searchScopeRules_;
                FOLDERVIEWMODE equivalentMode{};int equivalentSize=0;
                const auto equivalentViewRead=folderView_->GetViewModeAndIconSize(&equivalentMode,&equivalentSize);
                ComPtr<IShellItem> equivalentResults;
                auto equivalentRead=equivalentRules.empty()
                    ?createSearchFolderForScopes(equivalentQuery,equivalentScopes.Get(),&equivalentResults,searchRecursive_)
                    :createSearchFolderForScopeRules(equivalentQuery,equivalentRules,&equivalentResults);
                PIDLIST_ABSOLUTE equivalentRaw=nullptr;
                if(SUCCEEDED(equivalentRead))equivalentRead=SHGetIDListFromObject(equivalentResults.Get(),&equivalentRaw);
                Pidl equivalentFactory(equivalentRaw);
                const bool genuinelyEquivalent=SUCCEEDED(equivalentRead)&&equivalentFactory&&equivalentCurrent&&
                    ILIsEqual(equivalentFactory.get(),equivalentCurrent.get());
                const auto equivalentStarted=genuinelyEquivalent?startSearch(equivalentQuery,searchRecursive_):E_UNEXPECTED;
                bool equivalentStayedCurrent=SUCCEEDED(equivalentStarted);
                const auto equivalentDeadline=GetTickCount64()+200;
                const bool equivalentObserved=pumpUntil([&] {
                    equivalentStayedCurrent=equivalentStayedCurrent&&!navigating_&&!pendingLiveSearchTarget_&&
                        !pendingDirectSearchTarget_&&navigationCount_==equivalentNavigation;
                    return GetTickCount64()>=equivalentDeadline;
                },500);
                FOLDERVIEWMODE equivalentAfterMode{};int equivalentAfterSize=0;
                const auto equivalentAfterView=folderView_->GetViewModeAndIconSize(&equivalentAfterMode,&equivalentAfterSize);
                const auto equivalentContext=std::find_if(searchLocations_.rbegin(),searchLocations_.rend(),
                    [&](const SearchLocation& value){return value.query==equivalentQuery&&value.completedLocation&&
                        ILIsEqual(value.completedLocation.get(),currentPidl_.get());});
                const bool equivalentRuleMetadata=searchScopeRules_.size()==equivalentRules.size()&&
                    std::equal(searchScopeRules_.begin(),searchScopeRules_.end(),equivalentRules.begin(),[](const auto& left,const auto& right){
                        return left.folder.Get()==right.folder.Get()&&left.recursive==right.recursive&&left.excluded==right.excluded;
                    });
                const bool equivalentContextMetadata=equivalentContext!=searchLocations_.rend()&&
                    equivalentContext->base==equivalentQuery&&equivalentContext->remembered&&
                    equivalentContext->presentation.has_value()&&equivalentContext->historyLocation&&
                    historyIndex_>=0&&historyIndex_<static_cast<int>(history_.size())&&
                    ILIsEqual(equivalentContext->historyLocation.get(),history_[static_cast<size_t>(historyIndex_)].get());
                liveCheck("live_search_equivalent_direct_literal_commits_without_queued_native_navigation",
                    genuinelyEquivalent&&equivalentObserved&&equivalentStayedCurrent&&SUCCEEDED(equivalentViewRead)&&
                    SUCCEEDED(equivalentAfterView)&&equivalentMode==equivalentAfterMode&&equivalentSize==equivalentAfterSize&&
                    currentPidl_&&ILIsEqual(currentPidl_.get(),equivalentCurrent.get())&&exactLiveView(liveIdentities[0])&&
                    activeQuery_==equivalentQuery&&searchBase_==equivalentQuery&&textOf(search_)==equivalentQuery&&
                    std::all_of(searchFilters_.begin(),searchFilters_.end(),[](const auto& value){return value.empty();})&&
                    searchScope_&&ILIsEqual(searchScope_.get(),equivalentScope.get())&&searchScopes_.Get()==equivalentScopes.Get()&&
                    equivalentRuleMetadata&&history_.size()==equivalentHistory&&historyIndex_==equivalentHistoryIndex&&
                    recentSearches_.size()==equivalentRecent.size()+1&&recentSearches_.front()==equivalentQuery&&
                    std::equal(equivalentRecent.begin(),equivalentRecent.end(),recentSearches_.begin()+1)&&
                    equivalentContextMetadata);

                const auto recentAtOverlap=recentSearches_;
                const auto errorBeforeOverlap=lastError_;
                const auto olderHistorySlot=historyIndex_;
                const auto olderLiveSlot=liveSearchHistoryIndex_;
                const bool olderAcceptedSlot=searchBackground_&&currentPidl_&&olderHistorySlot>=0&&
                    olderHistorySlot<static_cast<int>(history_.size())&&
                    ILIsEqual(history_[static_cast<size_t>(olderHistorySlot)].get(),currentPidl_.get());
                const auto olderAcceptedHistory=history_.size();
                unsigned olderProbeCalls=0;
                bool olderPendingObserved=false,olderIsStale=false,newerEditAccepted=false;
                bool olderOverwroteEdit=false;
                unsigned overlapObservationSamples=0,overlapBeforeEditSamples=0;
                unsigned firstUnexpectedPhase=0,firstUnexpectedText=0,firstUnexpectedPolicy=0,firstUnexpectedActive=0;
                unsigned firstUnexpectedNavigation=0,firstUnexpectedProbeCalls=0;
                bool firstUnexpectedNavigating=false,firstUnexpectedLiveTarget=false,firstUnexpectedDirectTarget=false;
                std::uint64_t firstUnexpectedGeneration=0;
                // Observe only after the actual pending-navigation callback has
                // delivered the newer EN_CHANGE. Before that callback the old
                // literal is still the intended edit, even if it takes several
                // bounded message batches for the native provider to call us.
                const auto overlapLiteralCategory=[&](const std::wstring& text) -> unsigned {
                    if(text==queryFor(2))return 0;
                    if(text==queryFor(1))return 1;
                    if(text==queryFor(0))return 2;
                    return text.empty()?3u:4u;
                };
                const auto observeOverlapEdit=[&](unsigned phase) {
                    if(olderProbeCalls!=1||!newerEditAccepted){++overlapBeforeEditSamples;return;}
                    ++overlapObservationSamples;
                    const auto category=overlapLiteralCategory(textOf(search_));
                    if(!category)return;
                    if(!olderOverwroteEdit){
                        firstUnexpectedPhase=phase;firstUnexpectedText=category;
                        firstUnexpectedPolicy=overlapLiteralCategory(liveSearchPolicy_.literal());
                        firstUnexpectedActive=overlapLiteralCategory(activeQuery_);
                        firstUnexpectedNavigation=navigationCount_;firstUnexpectedProbeCalls=olderProbeCalls;
                        firstUnexpectedNavigating=navigating_;firstUnexpectedLiveTarget=static_cast<bool>(pendingLiveSearchTarget_);
                        firstUnexpectedDirectTarget=static_cast<bool>(pendingDirectSearchTarget_);
                        firstUnexpectedGeneration=pendingLiveSearch_?pendingLiveSearch_->generation:0;
                    }
                    olderOverwroteEdit=true;
                };
                struct ClearLiveNavigationProbe {
                    std::function<void()>& probe;
                    ~ClearLiveNavigationProbe(){probe={};}
                } clearLiveNavigationProbe{headlessLiveNavigationProbe_};
                unsigned busyBrowseCalls=0;
                bool busyIntentPreserved=false;
                struct ClearBusyBrowseProbe {
                    std::function<HRESULT()>& probe;
                    ~ClearBusyBrowseProbe(){probe={};}
                } clearBusyBrowseProbe{headlessLiveBrowseProbe_};
                headlessLiveBrowseProbe_=[&] {
                    ++busyBrowseCalls;
                    busyIntentPreserved=pendingLiveSearch_&&pendingLiveSearch_->explicitSubmit&&
                        pendingLiveSearch_->literal==queryFor(1)&&liveSearchPolicy_.current(*pendingLiveSearch_)&&
                        textOf(search_)==queryFor(1)&&recentSearches_==recentAtOverlap&&history_.size()==olderAcceptedHistory;
                    return HRESULT_FROM_WIN32(ERROR_BUSY);
                };
                headlessLiveNavigationProbe_=[&] {
                    ++olderProbeCalls;
                    const auto olderPending=pendingLiveSearch_;
                    olderPendingObserved=navigating_&&olderPending&&olderPending->explicitSubmit&&
                        pendingPidl_&&pendingLiveSearchTarget_&&ILIsEqual(pendingPidl_.get(),pendingLiveSearchTarget_.get());
                    newerEditAccepted=SetWindowTextW(search_,queryFor(2).c_str())!=FALSE;
                    olderIsStale=olderPending&&!liveSearchPolicy_.current(*olderPending);
                    observeOverlapEdit(1);
                };
                SetWindowTextW(search_,queryFor(1).c_str());SendMessageW(search_,WM_KEYDOWN,VK_RETURN,0);
                const bool newerReady=pumpUntil([&] {
                    observeOverlapEdit(2);
                    return olderProbeCalls==1&&newerEditAccepted&&activeQuery_==queryFor(2)&&!pendingLiveSearch_&&!liveSearchPolicy_.waiting()&&
                        exactLiveView(liveIdentities[2]);
                },5000);
                const bool olderMruPreserved=recentSearches_==recentAtOverlap;
                const bool olderHistoryPreserved=olderAcceptedSlot&&history_.size()==olderAcceptedHistory&&
                    olderAcceptedHistory==liveHistorySize&&historyIndex_==olderHistorySlot&&liveSearchHistoryIndex_==olderHistorySlot;
                std::fprintf(stderr,"headless-live-overlap calls=%u actual_pending=%u old_stale=%u edit_accepted=%u newer_ready=%u old_overwrite=%u mru_preserved=%u history_preserved=%u accepted_slot_valid=%u old_slot=%d old_live_slot=%d final_slot=%d final_index=%d before_history=%zu final_history=%zu\n",
                    olderProbeCalls,olderPendingObserved?1u:0u,olderIsStale?1u:0u,newerEditAccepted?1u:0u,newerReady?1u:0u,
                    olderOverwroteEdit?1u:0u,olderMruPreserved?1u:0u,olderHistoryPreserved?1u:0u,olderAcceptedSlot?1u:0u,olderHistorySlot,olderLiveSlot,
                    liveSearchHistoryIndex_,historyIndex_,olderAcceptedHistory,history_.size());std::fflush(stderr);
                std::fprintf(stderr,"headless-live-overlap-observation samples=%u before_edit_samples=%u first_phase=%u first_text=%u first_policy=%u first_active=%u first_navigation=%u first_probe_calls=%u first_navigating=%u first_live_target=%u first_direct_target=%u first_generation=%llu\n",
                    overlapObservationSamples,overlapBeforeEditSamples,firstUnexpectedPhase,firstUnexpectedText,firstUnexpectedPolicy,
                    firstUnexpectedActive,firstUnexpectedNavigation,firstUnexpectedProbeCalls,firstUnexpectedNavigating?1u:0u,
                    firstUnexpectedLiveTarget?1u:0u,firstUnexpectedDirectTarget?1u:0u,static_cast<unsigned long long>(firstUnexpectedGeneration));std::fflush(stderr);
                liveCheck("live_search_older_navigation_cannot_reset_newer_edit",olderProbeCalls==1&&olderPendingObserved&&
                    newerEditAccepted&&olderIsStale&&newerReady&&!olderOverwroteEdit&&olderMruPreserved&&olderHistoryPreserved&&
                    liveSearchStatus_==S_OK&&!headlessLiveNavigationProbe_);
                liveCheck("live_search_busy_navigation_retries_current_intent",busyBrowseCalls==1&&busyIntentPreserved&&
                    !headlessLiveBrowseProbe_&&newerReady&&olderPendingObserved&&olderHistoryPreserved&&olderMruPreserved&&
                    lastError_==errorBeforeOverlap);

                const auto beforeProgrammatic=navigationCount_;
                SetWindowTextW(search_,queryFor(1).c_str());const bool pendingEdit=liveSearchPolicy_.waiting();
                setSearchText(queryFor(0));
                const auto cancelledDeadline=GetTickCount64()+350;
                pumpUntil([&] {return GetTickCount64()>=cancelledDeadline;},500);
                liveCheck("live_search_programmatic_edit_cancels_without_navigation",pendingEdit&&
                    textOf(search_)==queryFor(0)&&liveSearchPolicy_.literal()==queryFor(0)&&
                    !liveSearchPolicy_.waiting()&&!pendingLiveSearch_&&!liveSearchOrigin_&&
                    navigationCount_==beforeProgrammatic&&activeQuery_==queryFor(2)&&exactLiveView(liveIdentities[2]));

                SetWindowTextW(search_,queryFor(0).c_str());
                liveNavigation=navigate(liveFolder.wstring());
                const bool externallyCancelled=SUCCEEDED(liveNavigation)&&pumpUntil([&] {
                    return atLocation(liveFolder)&&!pendingLiveSearch_&&!liveSearchPolicy_.waiting()&&exactLiveView(allLiveIdentities);
                },5000);
                const auto afterExternalNavigation=navigationCount_;
                const auto externalDeadline=GetTickCount64()+350;
                pumpUntil([&] {return GetTickCount64()>=externalDeadline;},500);
                liveCheck("live_search_external_navigation_cancels_edit_timer",externallyCancelled&&!searchActive_&&
                    textOf(search_).empty()&&navigationCount_==afterExternalNavigation&&recentSearches_==recentAtOverlap);

                SetWindowTextW(search_,queryFor(2).c_str());
                const bool beforeClearReady=pumpUntil([&] {return activeQuery_==queryFor(2)&&!pendingLiveSearch_&&exactLiveView(liveIdentities[2]);},5000);
                SetWindowTextW(search_,L"");
                const bool clearedReady=pumpUntil([&] {
                    return !searchActive_&&currentPidl_&&ILIsEqual(currentPidl_.get(),liveOrigin.get())&&
                        !pendingLiveSearch_&&!liveSearchPolicy_.waiting()&&exactLiveView(allLiveIdentities);
                },5000);
                liveCheck("live_search_clear_returns_exact_native_origin",beforeClearReady&&clearedReady&&textOf(search_).empty()&&
                    !liveSearchOrigin_&&recentSearches_==recentAtOverlap);

                setSearchText(queryFor(0));
                const auto beforeDirectNavigation=navigationCount_;
                const auto beforeDirectHistory=history_.size();
                const auto beforeDirectRevision=searchInteractionRevision_;
                unsigned directProbeCalls=0;
                struct ClearSearchProbe {
                    std::function<void()>& probe;
                    ~ClearSearchProbe(){probe={};}
                } clearSearchProbe{headlessSearchFactoryReentryProbe_};
                headlessSearchFactoryReentryProbe_=[&] {
                    ++directProbeCalls;
                    // Same final text, different actual edit generation.
                    SetWindowTextW(search_,queryFor(1).c_str());SetWindowTextW(search_,queryFor(0).c_str());
                };
                const auto directFactory=startSearch(queryFor(0),false);
                const bool directAborted=directFactory==S_FALSE&&directProbeCalls==1&&
                    searchInteractionRevision_>beforeDirectRevision+1&&navigationCount_==beforeDirectNavigation&&
                    history_.size()==beforeDirectHistory&&liveSearchPolicy_.waiting()&&!pendingDirectSearchTarget_&&
                    textOf(search_)==queryFor(0)&&exactLiveView(allLiveIdentities);
                const bool afterDirectReady=pumpUntil([&] {
                    return searchActive_&&activeQuery_==queryFor(0)&&!pendingLiveSearch_&&!liveSearchPolicy_.waiting()&&
                        exactLiveView(liveIdentities[0]);
                },5000);
                SendMessageW(search_,WM_KEYDOWN,VK_ESCAPE,0);
                const bool afterDirectOrigin=pumpUntil([&] {
                    return !searchActive_&&currentPidl_&&ILIsEqual(currentPidl_.get(),liveOrigin.get())&&
                        exactLiveView(allLiveIdentities);
                },5000);
                liveCheck("live_search_direct_factory_respects_same_text_new_generation",directAborted&&afterDirectReady&&
                    afterDirectOrigin&&recentSearches_==recentAtOverlap&&headlessSearchFactoryReentryProbe_==nullptr);

                setSearchText(queryFor(1));
                const auto enterIntentRevision=searchInteractionRevision_;
                const auto enterIntentNavigation=navigationCount_;
                const auto enterIntentHistory=history_.size();
                const auto recentBeforeEnterIntent=recentSearches_;
                unsigned enterProbeCalls=0;
                headlessSearchFactoryReentryProbe_=[&] {++enterProbeCalls;SendMessageW(search_,WM_KEYDOWN,VK_RETURN,0);};
                const auto enterDirectFactory=startSearch(queryFor(1),false);
                const bool enterDirectAborted=enterDirectFactory==S_FALSE&&enterProbeCalls==1&&
                    searchInteractionRevision_>enterIntentRevision+1&&navigationCount_==enterIntentNavigation&&
                    history_.size()==enterIntentHistory&&liveSearchPolicy_.waiting()&&!pendingDirectSearchTarget_&&
                    recentSearches_==recentBeforeEnterIntent&&textOf(search_)==queryFor(1)&&exactLiveView(allLiveIdentities);
                const bool enterIntentReady=pumpUntil([&] {
                    return searchActive_&&activeQuery_==queryFor(1)&&!pendingLiveSearch_&&!liveSearchPolicy_.waiting()&&
                        exactLiveView(liveIdentities[1]);
                },5000);
                liveCheck("live_search_enter_intent_supersedes_direct_factory",enterDirectAborted&&enterIntentReady&&
                    liveSearchPolicy_.committedLiteral()==queryFor(1)&&recentSearches_.size()==recentBeforeEnterIntent.size()+1&&
                    std::count(recentSearches_.begin(),recentSearches_.end(),queryFor(1))==1);

                const auto escapeIntentRevision=searchInteractionRevision_;
                const auto escapeIntentNavigation=navigationCount_;
                const auto escapeIntentHistory=history_.size();
                const auto recentBeforeEscapeIntent=recentSearches_;
                unsigned escapeProbeCalls=0;
                headlessSearchFactoryReentryProbe_=[&] {++escapeProbeCalls;SendMessageW(search_,WM_KEYDOWN,VK_ESCAPE,0);};
                const auto escapeDirectFactory=startSearch(queryFor(2),false);
                const bool escapeDirectAborted=escapeDirectFactory==S_FALSE&&escapeProbeCalls==1&&
                    searchInteractionRevision_>escapeIntentRevision+1&&navigationCount_==escapeIntentNavigation&&
                    history_.size()==escapeIntentHistory&&liveSearchPolicy_.waiting()&&!pendingDirectSearchTarget_&&
                    recentSearches_==recentBeforeEscapeIntent&&textOf(search_).empty()&&exactLiveView(liveIdentities[1]);
                const bool escapeIntentReady=pumpUntil([&] {
                    return !searchActive_&&currentPidl_&&ILIsEqual(currentPidl_.get(),liveOrigin.get())&&
                        !pendingLiveSearch_&&!liveSearchPolicy_.waiting()&&exactLiveView(allLiveIdentities);
                },5000);
                liveCheck("live_search_escape_intent_supersedes_direct_factory",escapeDirectAborted&&escapeIntentReady&&
                    textOf(search_).empty()&&recentSearches_==recentBeforeEscapeIntent&&headlessSearchFactoryReentryProbe_==nullptr);

                ComPtr<IShellItem> liveScope;
                liveNavigation=SHCreateItemFromIDList(liveOrigin.get(),IID_PPV_ARGS(&liveScope));
                const auto liveSavedPath=fixture/L"Subfolder"/L"Live origin.search-ms";
                if(SUCCEEDED(liveNavigation))liveNavigation=explorer::saveSearch(queryFor(0),liveScope.Get(),false,liveSavedPath);
                if(SUCCEEDED(liveNavigation))liveNavigation=navigate(liveSavedPath.wstring());
                const bool importedLiveReady=SUCCEEDED(liveNavigation)&&pumpUntil([&] {
                    return searchActive_&&isExternalSearch(currentPidl_.get())&&exactLiveView(liveIdentities[0]);
                },5000);
                SetWindowTextW(search_,queryFor(1).c_str());
                const bool importedRefinedReady=importedLiveReady&&pumpUntil([&] {
                    return activeQuery_==queryFor(1)&&!pendingLiveSearch_&&!liveSearchPolicy_.waiting()&&
                        exactLiveView(liveIdentities[1]);
                },5000);
                const bool savedOriginMatched=liveSearchOrigin_&&ILIsEqual(liveSearchOrigin_.get(),liveOrigin.get());
                SendMessageW(search_,WM_KEYDOWN,VK_ESCAPE,0);
                const bool escapedReady=pumpUntil([&] {
                    return !searchActive_&&currentPidl_&&ILIsEqual(currentPidl_.get(),liveOrigin.get())&&
                        !pendingLiveSearch_&&!liveSearchPolicy_.waiting()&&exactLiveView(allLiveIdentities);
                },5000);
                liveCheck("live_search_saved_search_edit_escape_returns_scope",importedRefinedReady&&savedOriginMatched&&escapedReady&&
                    textOf(search_).empty()&&!liveSearchOrigin_&&recentSearches_==recentBeforeEscapeIntent);
            }
            {
                // Existing host-preset mapping fixture follows the independent
                // imported/typed full-query replacement checks above.
                const auto refinementFolder = fixture / L"Subfolder" / L"Native refinements";
                std::filesystem::create_directory(refinementFolder);
                const std::array<std::filesystem::path, 4> refinementMembers{
                    refinementFolder / L"target-today.txt", refinementFolder / L"target-month.txt",
                    refinementFolder / L"target-empty.txt", refinementFolder / L"Folder"};
                HRESULT refinementRead = S_OK;
                SYSTEMTIME localNow{}; GetLocalTime(&localNow);
                SYSTEMTIME lastMonth = localNow; lastMonth.wDay = 15; lastMonth.wHour = 12;
                lastMonth.wMinute = lastMonth.wSecond = lastMonth.wMilliseconds = 0;
                if (lastMonth.wMonth == 1) { --lastMonth.wYear; lastMonth.wMonth = 12; } else --lastMonth.wMonth;
                SYSTEMTIME utcMonth{}; FILETIME monthTime{};
                if (!TzSpecificLocalTimeToSystemTime(nullptr, &lastMonth, &utcMonth) || !SystemTimeToFileTime(&utcMonth, &monthTime))
                    refinementRead = HRESULT_FROM_WIN32(GetLastError());
                std::array<NativeFileIdentity, 4> refinementIds{};
                for (size_t index = 0; index < refinementMembers.size(); ++index) {
                    if (index == 3) std::filesystem::create_directory(refinementMembers[index]);
                    else { std::ofstream output(refinementMembers[index], std::ios::binary); if (index != 2) output << "owned refinement"; }
                    if (index == 1) {
                        const auto file = CreateFileW(refinementMembers[index].c_str(), FILE_WRITE_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                        if (file == INVALID_HANDLE_VALUE) refinementRead = HRESULT_FROM_WIN32(GetLastError());
                        else { if (!SetFileTime(file, nullptr, nullptr, &monthTime)) refinementRead = HRESULT_FROM_WIN32(GetLastError()); CloseHandle(file); }
                    }
                    FILE_ID_INFO fileId{};
                    const auto read = nativeFileIdentity(refinementMembers[index], fileId);
                    if (FAILED(read)) refinementRead = read;
                    refinementIds[index].first = fileId.VolumeSerialNumber;
                    std::copy(std::begin(fileId.FileId.Identifier), std::end(fileId.FileId.Identifier), refinementIds[index].second.begin());
                }
                size_t refinementExpectedCount = 0, refinementActualCount = 0, refinementMissingCount = 0, refinementUnexpectedCount = 0;
                auto exactRefinementView = [&](const std::set<NativeFileIdentity>& expected) {
                    refinementExpectedCount = expected.size();
                    if (navigating_ || !searchActive_ || !folderView_) return false;
                    ComPtr<IShellItemArray> items; std::set<NativeFileIdentity> actual;
                    refinementRead = folderView_->Items(SVGIO_ALLVIEW, IID_PPV_ARGS(&items));
                    if (SUCCEEDED(refinementRead)) refinementRead = nativeArrayIdentities(items.Get(), actual);
                    refinementActualCount = actual.size();
                    refinementMissingCount = static_cast<size_t>(std::count_if(expected.begin(), expected.end(), [&](const auto& identity) { return !actual.contains(identity); }));
                    refinementUnexpectedCount = static_cast<size_t>(std::count_if(actual.begin(), actual.end(), [&](const auto& identity) { return !expected.contains(identity); }));
                    return SUCCEEDED(refinementRead) && actual == expected;
                };
                struct PresetObservation {
                    UINT command = 0, expected = UI_COLLECTION_INVALIDINDEX, host = UI_COLLECTION_INVALIDINDEX;
                    UINT native = UI_COLLECTION_INVALIDINDEX, variant = VT_EMPTY, nativeCount = 0;
                    UINT beforeNative = UI_COLLECTION_INVALIDINDEX;
                    size_t rows = 0, checked = 0;
                    bool rowChecked = false, inspected = false, queryMatches = false, expanded = false;
                    HRESULT cache = E_PENDING, selectedRead = E_PENDING, sourceRead = E_PENDING, beforeRead = E_PENDING, afterRead = E_PENDING;
                    HRESULT parentRead = E_PENDING, flushRead = E_PENDING;
                    HRESULT beforeSelectedRead = E_PENDING;
                    RibbonCollectionReadback before, after;
                } presetObservation;
                auto presetDetail = [&] {
                    const auto& observed = presetObservation;
                    return L"; expected IDs=" + std::to_wstring(refinementExpectedCount) + L"/actual IDs=" +
                        std::to_wstring(refinementActualCount) + L"/missing=" + std::to_wstring(refinementMissingCount) +
                        L"/unexpected=" + std::to_wstring(refinementUnexpectedCount) + L"; native view=" + hresultMessage(refinementRead) +
                        L"; command=" + std::to_wstring(observed.command) + L"/expected index=" + std::to_wstring(observed.expected) +
                        L"/host index=" + std::to_wstring(observed.host) + L"/cache=" + hresultMessage(observed.cache) +
                        L"/inspected=" + std::to_wstring(observed.inspected) + L"/query matches=" + std::to_wstring(observed.queryMatches) +
                        L"/rows=" + std::to_wstring(observed.rows) + L"/checked count=" + std::to_wstring(observed.checked) +
                        L"/selected row checked=" + std::to_wstring(observed.rowChecked) + L"; native SelectedItem=" +
                        hresultMessage(observed.selectedRead) + L"/vt=" + std::to_wstring(observed.variant) + L"/index=" +
                        std::to_wstring(observed.native) + L"; native source=" + hresultMessage(observed.sourceRead) +
                        L"/count=" + std::to_wstring(observed.nativeCount) + L"; parent read=" + hresultMessage(observed.parentRead) +
                        L"/expanded=" + std::to_wstring(observed.expanded) + L"/flush=" + hresultMessage(observed.flushRead) +
                        L"; before native SelectedItem=" + hresultMessage(observed.beforeSelectedRead) + L"/index=" + std::to_wstring(observed.beforeNative) +
                        L"; before collection=" + hresultMessage(observed.beforeRead) + L"/source requests=" +
                        std::to_wstring(observed.before.sourceRequests) + L"/published=" + std::to_wstring(observed.before.publishedItems) +
                        L"/selected requests=" + std::to_wstring(observed.before.selectedRequests) + L"/index=" +
                        std::to_wstring(observed.before.lastSelectedIndex) + L"; after collection=" + hresultMessage(observed.afterRead) +
                        L"/source requests=" + std::to_wstring(observed.after.sourceRequests) + L"/published=" +
                        std::to_wstring(observed.after.publishedItems) + L"/selected requests=" +
                        std::to_wstring(observed.after.selectedRequests) + L"/index=" + std::to_wstring(observed.after.lastSelectedIndex) +
                        L"/pending invalidations=" + std::to_wstring(observed.after.pendingInvalidations);
                };
                auto actualPreset = [&](UINT command, UINT index) {
                    presetObservation.command = command; presetObservation.expected = index;
                    presetObservation.host = ribbonState(command).selectedIndex;
                    presetObservation.cache = searchRefinementStatus_; presetObservation.inspected = searchRefinementInspected_;
                    presetObservation.queryMatches = searchRefinementQuery_ == activeQuery_;
                    const auto rows = ribbonItems(command);
                    presetObservation.rows = rows.size();
                    presetObservation.checked = static_cast<size_t>(std::count_if(rows.begin(), rows.end(), [](const auto& row) { return row.checked; }));
                    presetObservation.rowChecked = index < rows.size() && rows[index].checked;
                    PROPVARIANT selected{};
                    const auto framework = ribbon_.nativeFramework();
                    presetObservation.selectedRead = framework ? framework->GetUICommandProperty(ribbon_.nativeCommandId(command), UI_PKEY_SelectedItem, &selected) : E_NOINTERFACE;
                    presetObservation.variant = selected.vt;
                    presetObservation.native = selected.vt == VT_UI4 ? selected.ulVal : UI_COLLECTION_INVALIDINDEX;
                    PropVariantClear(&selected);
                    PROPVARIANT source{}; ComPtr<IUICollection> collection;
                    presetObservation.sourceRead = framework ? framework->GetUICommandProperty(ribbon_.nativeCommandId(command), UI_PKEY_ItemsSource, &source) : E_NOINTERFACE;
                    if (SUCCEEDED(presetObservation.sourceRead)) presetObservation.sourceRead = source.vt == VT_UNKNOWN && source.punkVal ?
                        source.punkVal->QueryInterface(IID_PPV_ARGS(&collection)) : E_NOINTERFACE;
                    presetObservation.nativeCount = 0;
                    if (SUCCEEDED(presetObservation.sourceRead)) presetObservation.sourceRead = collection->GetCount(&presetObservation.nativeCount);
                    PropVariantClear(&source);
                    presetObservation.afterRead = ribbon_.collectionReadback(command, presetObservation.after);
                    return presetObservation.host == index && presetObservation.rowChecked && presetObservation.checked == 1 &&
                        SUCCEEDED(presetObservation.selectedRead) && presetObservation.variant == VT_UI4 && presetObservation.native == index &&
                        SUCCEEDED(presetObservation.sourceRead) && presetObservation.nativeCount == rows.size();
                };
                auto readyPreset = [&](const std::set<NativeFileIdentity>& expected, UINT command, UINT index) {
                    presetObservation = {}; presetObservation.command = command; presetObservation.expected = index;
                    if (!pumpUntil([&] { return exactRefinementView(expected) && !searchPresentationPending_; }, 5000)) {
                        actualPreset(command, index); return false;
                    }
                    updateCommands();
                    actualPreset(command, index);
                    presetObservation.beforeSelectedRead = presetObservation.selectedRead;
                    presetObservation.beforeNative = presetObservation.native;
                    // The native item gallery publishes its lazy ItemsSource
                    // when its real parent expands. Read state only after that
                    // public provider route, without selecting/invoking a leaf.
                    presetObservation.beforeRead = ribbon_.collectionReadback(command, presetObservation.before);
                    PrivatePresentation presentation(window_, true);
                    auto expandRead = presentation.ready ? ribbon_.selectTab(RibbonSearchTab) : E_ACCESSDENIED;
                    std::wstring label;
                    if (SUCCEEDED(expandRead)) expandRead = ribbon_.commandLabel(command, label);
                    presetObservation.parentRead = expandRead;
                    HWND ribbonWindow = nullptr;
                    EnumChildWindows(window_, [](HWND child, LPARAM context) -> BOOL {
                        wchar_t name[64]{}; GetClassNameW(child, name, static_cast<int>(std::size(name)));
                        if (wcscmp(name, L"UIRibbonCommandBar") == 0) { *reinterpret_cast<HWND*>(context) = child; return FALSE; }
                        return TRUE;
                    }, reinterpret_cast<LPARAM>(&ribbonWindow));
                    const auto desktop = GetThreadDesktop(GetCurrentThreadId());
                    if (SUCCEEDED(expandRead) && ribbonWindow) {
                        auto worker = std::async(std::launch::async, [desktop, ribbonWindow, host = window_, label] {
                            struct Apartment {
                                HRESULT status = E_ACCESSDENIED;
                                explicit Apartment(HDESK target) { if (SetThreadDesktop(target)) status = CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
                                ~Apartment() { if (SUCCEEDED(status)) CoUninitialize(); }
                            } apartment(desktop);
                            ComPtr<IUIAutomation> automation; ComPtr<IUIAutomationElement> root;
                            auto read = apartment.status;
                            if (SUCCEEDED(read)) read = CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation));
                            ComPtr<IUIAutomation2> timeouts;
                            if (automation && SUCCEEDED(automation.As(&timeouts))) {
                                timeouts->put_ConnectionTimeout(1000); timeouts->put_TransactionTimeout(1000); timeouts->put_AutoSetFocus(FALSE);
                            }
                            if (SUCCEEDED(read)) read = automation->ElementFromHandle(ribbonWindow, &root);
                            return SUCCEEDED(read) && accessibleExpandedMenu(automation.Get(), root.Get(), host, label.c_str(), 1, {}, true).passed;
                        });
                        pumpUntil([&] { return worker.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; }, 12000);
                        presetObservation.expanded = worker.get();
                    }
                    presetObservation.flushRead = ribbon_.flush();
                    const bool matched = pumpUntil([&] { return exactRefinementView(expected) && actualPreset(command, index); }, 5000);
                    return presetObservation.expanded && matched;
                };
                ComPtr<IShellItem> refinementScope;
                auto operation = SHCreateItemFromParsingName(refinementFolder.c_str(), nullptr, IID_PPV_ARGS(&refinementScope));
                const std::vector<SearchScopeRule> refinementRules{{refinementScope, false, false}};
                SearchFileProperties refinementProperties; refinementProperties.author = L"Owned 作者";
                refinementProperties.kind = L"searchfolder"; refinementProperties.description = L"literal & metadata";
                refinementProperties.tags = L"owned;exact";
                SearchViewPresentation refinementPresentation; refinementPresentation.mode = SearchViewMode::Details;
                refinementPresentation.iconSize = 16; refinementPresentation.visibleColumns = std::vector<std::wstring>{L"System.ItemNameDisplay", L"System.Size"};
                refinementPresentation.sort = std::vector<SearchViewOrder>{{L"System.ItemNameDisplay", SORT_DESCENDING}};
                const std::wstring todayCondition = L"System.DateModified:System.StructuredQueryType.DateTime#Today";
                const std::wstring todayQuery = L"System.FileName:~\"target-*.txt\" AND NOT System.FileName:=\"target-empty.txt\" AND " + todayCondition;
                const auto refinementSource = fixture / L"Subfolder" / L"Today refinement.search-ms";
                if (SUCCEEDED(operation)) operation = saveSearchForScopeRules(todayQuery, refinementRules, refinementSource,
                    SearchSaveMode::CreateNew, &refinementPresentation, &refinementProperties);
                if (SUCCEEDED(operation)) operation = navigate(refinementSource.wstring());
                const bool imported = SUCCEEDED(operation) && readyPreset({refinementIds[0]}, SearchDateMenu, 0);
                const auto importedPresetDetail = presetDetail();
                operation = imported ? executeRibbonItem(SearchDateMenu, 5) : E_UNEXPECTED;
                const bool dateReplaced = SUCCEEDED(operation) && readyPreset({refinementIds[1]}, SearchDateMenu, 5);
                const auto refinedFullQuery = activeQuery_;
                Pidl refinedLocation(ILCloneFull(currentPidl_.get()));
                const auto refinedDateHistory = historyIndex_;
                const auto propertiesPreserved = [&] {
                    return searchFileProperties_ && *searchFileProperties_ == refinementProperties && searchScopeRules_.size() == 1 &&
                        !searchScopeRules_.front().recursive && !searchScopeRules_.front().excluded && searchPresentation_ &&
                        searchPresentation_->mode == SearchViewMode::Details && searchPresentation_->visibleColumns == refinementPresentation.visibleColumns;
                };
                check("imported_today_date_replacement_native_ids_selected_item_and_metadata", imported && dateReplaced && propertiesPreserved(),
                    L"operation=" + hresultMessage(operation) + L"; imported Today" + importedPresetDetail + L"; replaced LastMonth" + presetDetail());
                operation = dateReplaced ? navigate(refinementFolder.wstring()) : E_UNEXPECTED;
                const bool leftRefinement = SUCCEEDED(operation) && pumpUntil([&] { return !navigating_ && atLocation(refinementFolder); }, 5000);
                operation = leftRefinement ? execute(Back) : E_UNEXPECTED;
                const bool restoredRefinement = SUCCEEDED(operation) && readyPreset({refinementIds[1]}, SearchDateMenu, 5) && activeQuery_ == refinedFullQuery;
                check("refined_import_history_restores_exact_category_and_native_metadata", restoredRefinement && propertiesPreserved() &&
                    historyIndex_ == refinedDateHistory && currentPidl_ && ILIsEqual(currentPidl_.get(), refinedLocation.get()),
                    L"operation=" + hresultMessage(operation) + presetDetail());
                const auto refinementRoundtrip = fixture / L"Subfolder" / L"Last month refinement.search-ms";
                operation = restoredRefinement && propertiesPreserved() ? saveSearchForScopeRules(activeQuery_, searchScopeRules_, refinementRoundtrip,
                    SearchSaveMode::CreateNew, &*searchPresentation_, &*searchFileProperties_) : E_UNEXPECTED;
                SavedSearchMetadata refinementMetadata;
                if (SUCCEEDED(operation)) operation = readSavedSearch(refinementRoundtrip, &refinementMetadata);
                if (SUCCEEDED(operation)) operation = navigate(refinementRoundtrip.wstring());
                const bool reopenedRefinement = SUCCEEDED(operation) && readyPreset({refinementIds[1]}, SearchDateMenu, 5);
                check("refined_category_resave_import_keeps_exact_native_ids_and_all_file_properties", reopenedRefinement &&
                    refinementMetadata.fileProperties && *refinementMetadata.fileProperties == refinementProperties && propertiesPreserved(),
                    L"operation=" + hresultMessage(operation) + presetDetail());
                operation = startSearch(L"System.FileName:~\"target-*.txt\" AND System.Size:>0", false);
                const bool typedSizeReady = SUCCEEDED(operation) && pumpUntil([&] {
                    return exactRefinementView({refinementIds[0], refinementIds[1]});
                }, 5000);
                operation = typedSizeReady ? executeRibbonItem(SearchSizeMenu, 0) : E_UNEXPECTED;
                const bool replacedSize = SUCCEEDED(operation) && readyPreset({refinementIds[2]}, SearchSizeMenu, 0);
                check("typed_size_base_replacement_returns_exact_empty_file_and_selected_item", replacedSize && propertiesPreserved(),
                    L"operation=" + hresultMessage(operation) + presetDetail());
                struct RefinementMenu { HMENU value = CreatePopupMenu(); ~RefinementMenu() { if (value) DestroyMenu(value); } } kindMenu;
                std::vector<std::wstring> kindExpressions;
                operation = appendSearchRefinementMenu(SearchKindMenu, kindMenu.value, &kindExpressions);
                std::vector<std::wstring> nativeFolderExpression;
                if (SUCCEEDED(operation)) operation = NativeSearchRefinements::kindPresets({L"folder"}, &nativeFolderExpression);
                const auto folderChoice = nativeFolderExpression.size() == 1 ? std::find(kindExpressions.begin(), kindExpressions.end(), nativeFolderExpression.front()) : kindExpressions.end();
                const UINT folderIndex = folderChoice == kindExpressions.end() ? UI_COLLECTION_INVALIDINDEX : static_cast<UINT>(folderChoice - kindExpressions.begin());
                if (SUCCEEDED(operation)) operation = startSearch(L"System.Kind:=System.Kind#Document", false);
                const bool typedKindReady = SUCCEEDED(operation) && pumpUntil([&] {
                    return exactRefinementView({refinementIds[0], refinementIds[1], refinementIds[2]});
                }, 5000);
                operation = typedKindReady && folderIndex != UI_COLLECTION_INVALIDINDEX ? executeRibbonItem(SearchKindMenu, folderIndex) : E_UNEXPECTED;
                const bool replacedKind = SUCCEEDED(operation) && readyPreset({refinementIds[3]}, SearchKindMenu, folderIndex);
                check("typed_kind_base_replacement_returns_exact_owned_folder_and_selected_item", replacedKind && propertiesPreserved(),
                    L"operation=" + hresultMessage(operation) + presetDetail());
                const auto entangledQuery = L"(" + todayCondition + L" AND System.FileName:=\"target-today.txt\") OR System.FileName:=\"target-month.txt\"";
                operation = startSearch(entangledQuery, false);
                const bool entangledReady = SUCCEEDED(operation) && pumpUntil([&] {
                    return exactRefinementView({refinementIds[0], refinementIds[1]});
                }, 5000);
                const auto previousQuery = activeQuery_, previousBase = searchBase_;
                const auto previousFilters = searchFilters_; const auto previousRevision = searchInteractionRevision_;
                const auto previousHistory = historyIndex_; const auto previousNavigation = navigationCount_;
                Pidl previousLocation(ILCloneFull(currentPidl_.get()));
                operation = entangledReady ? executeRibbonItem(SearchDateMenu, 5) : E_UNEXPECTED;
                check("entangled_category_rejection_preserves_native_query_history_intent_and_metadata", entangledReady &&
                    operation == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) && activeQuery_ == previousQuery && searchBase_ == previousBase &&
                    searchFilters_ == previousFilters && searchInteractionRevision_ == previousRevision && historyIndex_ == previousHistory &&
                    navigationCount_ == previousNavigation && currentPidl_ && ILIsEqual(currentPidl_.get(), previousLocation.get()) &&
                    ribbonState(SearchDateMenu).selectedIndex == UI_COLLECTION_INVALIDINDEX && propertiesPreserved() &&
                    exactRefinementView({refinementIds[0], refinementIds[1]}));
                operation = execute(CloseSearch);
                check("native_refinement_fixture_returns_to_owned_scope", SUCCEEDED(operation) && pumpUntil([&] {
                    return !navigating_ && !searchActive_ && atLocation(refinementFolder);
                }, 5000));
                operation = navigate(fixture.wstring());
                check("native_refinement_fixture_returns_to_original_test_origin", SUCCEEDED(operation) && pumpUntil([&] {
                    return !navigating_ && atLocation(fixture);
                }, 5000));
            }
            {
                // Exercise the same packet consumer and pre-create native
                // factory preparation as a launched child. These extra App
                // instances remain hidden on this never-switched desktop.
                const auto handoffRoot=fixture/L"Search window handoff";
                const auto firstPath=handoffRoot/L"first",secondPath=handoffRoot/L"second",nestedPath=firstPath/L"nested";
                std::filesystem::create_directories(nestedPath);std::filesystem::create_directory(secondPath);
                const std::array files{firstPath/L"target.txt",secondPath/L"target.txt",nestedPath/L"target.txt"};
                for(const auto& file:files){std::ofstream output(file,std::ios::binary);output<<"owned handoff source";}
                const auto nativeIdentity=[](const FILE_ID_INFO& value){
                    std::array<BYTE,16> bytes{};std::copy(std::begin(value.FileId.Identifier),std::end(value.FileId.Identifier),bytes.begin());
                    return NativeFileIdentity{value.VolumeSerialNumber,bytes};
                };
                std::array<FILE_ID_INFO,3> originalIds{};bool sourceReads=true;
                for(size_t index=0;index<files.size();++index)sourceReads=SUCCEEDED(nativeFileIdentity(files[index],originalIds[index]))&&sourceReads;
                const std::set<NativeFileIdentity> expected{nativeIdentity(originalIds[0]),nativeIdentity(originalIds[1])};
                ComPtr<IShellItem> firstScope,secondScope,nestedScope;
                auto stage=SHCreateItemFromParsingName(firstPath.c_str(),nullptr,IID_PPV_ARGS(&firstScope));
                if(SUCCEEDED(stage))stage=SHCreateItemFromParsingName(secondPath.c_str(),nullptr,IID_PPV_ARGS(&secondScope));
                if(SUCCEEDED(stage))stage=SHCreateItemFromParsingName(nestedPath.c_str(),nullptr,IID_PPV_ARGS(&nestedScope));
                PIDLIST_ABSOLUTE firstRaw=nullptr,secondRaw=nullptr;
                if(SUCCEEDED(stage))stage=SHGetIDListFromObject(firstScope.Get(),&firstRaw);Pidl firstId(firstRaw);
                if(SUCCEEDED(stage))stage=SHGetIDListFromObject(secondScope.Get(),&secondRaw);Pidl secondId(secondRaw);
                PCIDLIST_ABSOLUTE scopeIds[]{firstId.get(),secondId.get()};ComPtr<IShellItemArray> scopes;
                if(SUCCEEDED(stage))stage=SHCreateShellItemArrayFromIDLists(2,scopeIds,&scopes);
                const auto mainHistory=history_.size();const auto mainHistoryIndex=historyIndex_;
                const auto mainNavigation=navigationCount_;const auto mainQuery=activeQuery_;const auto mainView=view_;
                Pidl mainLocation(currentPidl_?ILCloneFull(currentPidl_.get()):nullptr);
                const auto sameItem=[](IShellItem* left,IShellItem* right){int comparison=1;
                    return left&&right&&SUCCEEDED(left->Compare(right,SICHINT_CANONICAL,&comparison))&&comparison==0;};
                const auto sameHandoffPresentation=[](const SearchViewPresentation& left,const SearchViewPresentation& right){
                    const auto sameOrder=[](const SearchViewOrder& a,const SearchViewOrder& b){return a.property==b.property&&a.direction==b.direction;};
                    return left.mode==right.mode&&left.iconSize==right.iconSize&&left.visibleColumns==right.visibleColumns&&
                        left.groupBy.has_value()==right.groupBy.has_value()&&(!left.groupBy||sameOrder(*left.groupBy,*right.groupBy))&&
                        left.sort.has_value()==right.sort.has_value()&&(!left.sort||(left.sort->size()==right.sort->size()&&
                            std::equal(left.sort->begin(),left.sort->end(),right.sort->begin(),sameOrder)));
                };
                HRESULT membershipRead=E_PENDING;size_t handoffObservedMembers=0;
                const auto members=[&](ExplorerApp& app){ComPtr<IShellItemArray> items;std::set<NativeFileIdentity> actual;
                    const auto view=app.folderView_;const auto navigation=app.navigationCount_;const auto revision=app.searchInteractionRevision_;
                    if(app.navigating_||!view){membershipRead=E_PENDING;handoffObservedMembers=0;return false;}
                    membershipRead=view->Items(SVGIO_ALLVIEW,IID_PPV_ARGS(&items));
                    if(SUCCEEDED(membershipRead))membershipRead=nativeArrayIdentities(items.Get(),actual);
                    handoffObservedMembers=actual.size();
                    return SUCCEEDED(membershipRead)&&actual==expected&&!app.navigating_&&app.folderView_.Get()==view.Get()&&
                        app.navigationCount_==navigation&&app.searchInteractionRevision_==revision;};
                const auto atOrigin=[&](ExplorerApp& app){ComPtr<IShellItem> current;
                    const auto navigation=app.navigationCount_;Pidl location(app.currentPidl_?ILCloneFull(app.currentPidl_.get()):nullptr);
                    return !app.navigating_&&!app.searchActive_&&location&&
                        SUCCEEDED(SHCreateItemFromIDList(location.get(),IID_PPV_ARGS(&current)))&&sameItem(current.Get(),secondScope.Get())&&
                        !app.navigating_&&!app.searchActive_&&app.navigationCount_==navigation&&app.currentPidl_&&ILIsEqual(location.get(),app.currentPidl_.get());};
                const auto releaseApp=[](ExplorerApp* app){if(app->window_&&IsWindow(app->window_))SendMessageW(app->window_,WM_CLOSE,0,0);app->Release();};
                const auto datePublication=[&](ExplorerApp& app,std::wstring& detail){
                    const auto isolation=PrivateDesktop::current();
                    if(!headless_||!app.headless_||!isolation||FAILED(isolation->verifyIsolation())){detail=L"private owner verification failed";return false;}
                    PrivatePresentation presentation(app.window_,true);
                    auto read=presentation.ready?app.ribbon_.selectTab(RibbonSearchTab):E_ACCESSDENIED;std::wstring label;
                    if(SUCCEEDED(read))read=app.ribbon_.commandLabel(SearchDateMenu,label);
                    HWND ribbonWindow=nullptr;
                    EnumChildWindows(app.window_,[](HWND child,LPARAM context)->BOOL{wchar_t name[64]{};
                        GetClassNameW(child,name,static_cast<int>(std::size(name)));
                        if(wcscmp(name,L"UIRibbonCommandBar")==0){*reinterpret_cast<HWND*>(context)=child;return FALSE;}return TRUE;},
                        reinterpret_cast<LPARAM>(&ribbonWindow));
                    if(FAILED(read)||!ribbonWindow){detail=L"parent="+hresultMessage(read)+L"; ribbon="+std::to_wstring(ribbonWindow!=nullptr);return false;}
                    const auto desktop=GetThreadDesktop(GetCurrentThreadId());
                    const auto deadline=GetTickCount64()+12000;
                    const auto cancellation=std::make_shared<std::atomic_bool>(false);
                    struct DateExpansion { bool expanded=false;HRESULT setup=E_PENDING,expired=E_PENDING,cancelled=E_PENDING; };
                    auto worker=std::async(std::launch::async,[desktop,ribbonWindow,host=app.window_,label,deadline,cancellation]{
                        struct Apartment{HRESULT status=E_ACCESSDENIED;explicit Apartment(HDESK target){if(SetThreadDesktop(target))status=CoInitializeEx(nullptr,COINIT_MULTITHREADED);}
                            ~Apartment(){if(SUCCEEDED(status))CoUninitialize();}}apartment(desktop);
                        ComPtr<IUIAutomation> automation;ComPtr<IUIAutomationElement> root;auto result=apartment.status;
                        if(SUCCEEDED(result))result=CoCreateInstance(CLSID_CUIAutomation8,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&automation));
                        ComPtr<IUIAutomation2> timeouts;
                        if(SUCCEEDED(result))result=automation.As(&timeouts);
                        if(SUCCEEDED(result))result=timeouts->put_ConnectionTimeout(1000);
                        if(SUCCEEDED(result))result=timeouts->put_TransactionTimeout(1000);
                        if(SUCCEEDED(result))result=timeouts->put_AutoSetFocus(FALSE);
                        if(SUCCEEDED(result)&&!cancellation->load()&&GetTickCount64()<deadline)result=automation->ElementFromHandle(ribbonWindow,&root);
                        DateExpansion proof;proof.setup=result;
                        if(FAILED(result)||!root)return proof;
                        const auto expired=accessibleExpandedMenu(automation.Get(),root.Get(),host,label.c_str(),1,{},true,1,nullptr);
                        proof.expired=expired.budgetStatus;
                        const std::atomic_bool alreadyCancelled(true);
                        const auto cancelled=accessibleExpandedMenu(automation.Get(),root.Get(),host,label.c_str(),1,{},true,deadline,&alreadyCancelled);
                        proof.cancelled=cancelled.budgetStatus;
                        if(expired.passed||expired.element||proof.expired!=HRESULT_FROM_WIN32(ERROR_TIMEOUT)||
                            cancelled.passed||cancelled.element||proof.cancelled!=HRESULT_FROM_WIN32(ERROR_CANCELLED))return proof;
                        proof.expanded=accessibleExpandedMenu(automation.Get(),root.Get(),host,label.c_str(),1,{},true,deadline,cancellation.get()).passed;
                        return proof;
                    });
                    const auto ready=[&]{return worker.wait_for(std::chrono::milliseconds(0))==std::future_status::ready;};
                    const bool withinDeadline=pumpUntil(ready,12000);
                    if(!withinDeadline){
                        cancellation->store(true);
                        const auto joinDeadline=GetTickCount64()+5000;
                        while(!ready()&&GetTickCount64()<joinDeadline)pumpUntil(ready,25);
                        if(!ready()){
                            std::fprintf(stderr,"headless NewWindow Date UIA deadline exceeded; worker did not join after cancellation; private_owner=true\n");
                            std::fflush(stderr);
                            // Keep borrowed native sites/HWNDs alive until this
                            // verified private test process stops its workers.
                            if (!TerminateProcess(GetCurrentProcess(), 9)) std::_Exit(9);
                            std::_Exit(9);
                        }
                    }
                    const auto proof=worker.get();const bool expanded=proof.expanded;read=app.ribbon_.flush();
                    const auto budgets=L"; setup="+hresultMessage(proof.setup)+L"; expired="+hresultMessage(proof.expired)+L"; cancelled="+hresultMessage(proof.cancelled);
                    if(!withinDeadline||!expanded||FAILED(read)){detail=L"deadline="+std::to_wstring(withinDeadline)+L"; expanded="+
                        std::to_wstring(expanded)+L"; flush="+hresultMessage(read)+budgets;return false;}
                    const auto native=app.ribbon_.nativeCommandId(SearchDateMenu);
                    HRESULT selectedRead=E_PENDING,sourceRead=E_PENDING;UINT count=0,index=UI_COLLECTION_INVALIDINDEX;VARTYPE type=VT_EMPTY;
                    const bool matched=pumpUntil([&]{
                        PROPVARIANT selected{},source{};auto framework=app.ribbon_.nativeFramework();
                        selectedRead=framework?framework->GetUICommandProperty(native,UI_PKEY_SelectedItem,&selected):E_UNEXPECTED;
                        type=selected.vt;index=type==VT_UI4?selected.ulVal:UI_COLLECTION_INVALIDINDEX;PropVariantClear(&selected);
                        sourceRead=framework?framework->GetUICommandProperty(native,UI_PKEY_ItemsSource,&source):E_UNEXPECTED;
                        ComPtr<IUICollection> collection;count=0;
                        if(SUCCEEDED(sourceRead))sourceRead=source.vt==VT_UNKNOWN&&source.punkVal?source.punkVal->QueryInterface(IID_PPV_ARGS(&collection)):E_NOINTERFACE;
                        if(SUCCEEDED(sourceRead))sourceRead=collection->GetCount(&count);PropVariantClear(&source);
                        const auto rows=app.ribbonItems(SearchDateMenu);
                        return SUCCEEDED(selectedRead)&&type==VT_UI4&&index==0&&SUCCEEDED(sourceRead)&&count==8&&rows.size()==8&&rows[0].checked&&
                            std::count_if(rows.begin(),rows.end(),[](const auto& row){return row.checked;})==1&&
                            app.ribbonState(SearchDateMenu).selectedIndex==0&&app.searchRefinementInspected_&&
                            SUCCEEDED(app.searchRefinementStatus_)&&app.searchRefinementQuery_==app.activeQuery_;
                    },5000);
                    detail=L"expanded="+std::to_wstring(expanded)+L"; selected="+hresultMessage(selectedRead)+L"/"+std::to_wstring(type)+L"/"+
                        std::to_wstring(index)+L"; source="+hresultMessage(sourceRead)+L"/"+std::to_wstring(count)+L"; cache="+
                        hresultMessage(app.searchRefinementStatus_)+L"; inspected="+std::to_wstring(app.searchRefinementInspected_)+budgets;
                    return matched;
                };
                const std::wstring handoffQuery=L"System.FileName:=\"target.txt\" AND System.DateModified:System.StructuredQueryType.DateTime#Today";
                ComPtr<IShellItem> baseline;ComPtr<IShellFolder> baselineFolder;ComPtr<IEnumIDList> baselineEnumeration;
                auto baselineRead=SUCCEEDED(stage)?createSearchFolderForScopes(handoffQuery,scopes.Get(),&baseline,false):stage;
                if(SUCCEEDED(baselineRead))baselineRead=baseline->BindToHandler(nullptr,BHID_SFObject,IID_PPV_ARGS(&baselineFolder));
                if(SUCCEEDED(baselineRead))baselineRead=baselineFolder->EnumObjects(nullptr,SHCONTF_NONFOLDERS,&baselineEnumeration);
                std::set<NativeFileIdentity> baselineIds;
                while(SUCCEEDED(baselineRead)&&baselineEnumeration){PITEMID_CHILD entry=nullptr;
                    const auto next=baselineEnumeration->Next(1,&entry,nullptr);if(next==S_FALSE)break;
                    baselineRead=next;ComPtr<IShellItem> value;
                    if(SUCCEEDED(baselineRead))baselineRead=SHCreateItemWithParent(nullptr,baselineFolder.Get(),entry,IID_PPV_ARGS(&value));CoTaskMemFree(entry);
                    FILE_ID_INFO info{};if(SUCCEEDED(baselineRead))baselineRead=nativeFileIdentity(itemName(value.Get(),SIGDN_FILESYSPATH),info);
                    if(SUCCEEDED(baselineRead)&&(!baselineIds.insert(nativeIdentity(info)).second||baselineIds.size()>3))baselineRead=E_UNEXPECTED;
                }
                const bool baselineMatches=SUCCEEDED(baselineRead)&&baselineIds==expected;
                check("search_new_window_original_query_native_factory_baseline",baselineMatches&&sourceReads,
                    L"read="+hresultMessage(baselineRead)+L"; expected="+std::to_wstring(expected.size())+L"; actual="+std::to_wstring(baselineIds.size()));
                SearchFileProperties properties;properties.author=L"資料 🚀 & %1";properties.kind=L"";
                properties.description=L"original\r\nlines\ttabs";properties.tags=L"owned handoff";
                for(const bool explicitRules:{false,true}){
                    SearchWindowContext seed;seed.query=handoffQuery;seed.primaryScope=firstScope;seed.closeOrigin=secondScope;seed.scopes=scopes;seed.recursive=false;
                    if(explicitRules)seed.rules={{firstScope,false,false},{secondScope,false,false},{nestedScope,true,true}};
                    SearchViewPresentation full;full.mode=explicitRules?SearchViewMode::List:SearchViewMode::Content;full.iconSize=explicitRules?16:32;
                    full.visibleColumns=std::vector<std::wstring>{L"System.ItemNameDisplay",L"System.Size",L"System.DateModified"};
                    full.groupBy=SearchViewOrder{L"System.Kind",SORT_ASCENDING};
                    full.sort=std::vector<SearchViewOrder>{{L"System.DateModified",SORT_DESCENDING},{L"System.ItemNameDisplay",SORT_ASCENDING}};
                    seed.presentation=full;seed.fileProperties=properties;
                    std::unique_ptr<ExplorerApp,decltype(releaseApp)> parent(new ExplorerApp(instance_,true,requestedRibbonLayout_),releaseApp);
                    const auto prepared=SUCCEEDED(stage)?parent->prepareSearchWindowContext(seed):stage;
                    const auto created=SUCCEEDED(prepared)?parent->create(L""):prepared;
                    auto operation=created;
                    const bool parentReady=SUCCEEDED(created)&&pumpUntil([&]{return parent->searchActive_&&!parent->searchPresentationPending_&&
                        SUCCEEDED(parent->searchPresentationStatus_)&&members(*parent);},5000);
                    const auto parentMembershipRead=membershipRead;const auto parentMembers=handoffObservedMembers;
                    const auto parentPresentationRead=parent->searchPresentationStatus_;
                    const auto parentNavigations=parent->navigationCount_;
                    SearchWindowContext transferred;
                    const auto captured=parentReady?parent->currentSearchWindowContext(&transferred):HRESULT_FROM_WIN32(ERROR_NOT_READY);
                    operation=captured;
                    const auto parentHistory=parent->history_.size();const auto parentHistoryIndex=parent->historyIndex_;
                    const auto parentNavigation=parent->navigationCount_;const auto parentView=parent->view_;
                    Pidl parentLocation(parent->currentPidl_?ILCloneFull(parent->currentPidl_.get()):nullptr);
                    SearchWindowMapping mapping;if(SUCCEEDED(operation))operation=SearchWindowMapping::create(transferred,&mapping);
                    HANDLE inherited=nullptr;
                    if(SUCCEEDED(operation)&&!DuplicateHandle(GetCurrentProcess(),mapping.handle(),GetCurrentProcess(),&inherited,0,TRUE,DUPLICATE_SAME_ACCESS))
                        operation=HRESULT_FROM_WIN32(GetLastError());
                    SearchWindowContext loaded;
                    if(SUCCEEDED(operation))operation=consumeSearchWindowContext(reinterpret_cast<ULONG_PTR>(inherited),&loaded);
                    if(FAILED(operation)&&inherited)CloseHandle(inherited);
                    const auto consumed=operation;
                    std::unique_ptr<ExplorerApp,decltype(releaseApp)> child(new ExplorerApp(instance_,true,requestedRibbonLayout_),releaseApp);
                    if(SUCCEEDED(operation))operation=child->prepareSearchWindowContext(loaded);
                    const auto childPrepared=operation;
                    if(SUCCEEDED(operation))operation=child->create(L"");
                    const auto childCreated=operation;
                    const bool childReady=SUCCEEDED(operation)&&pumpUntil([&]{return child->searchActive_&&!child->searchPresentationPending_&&
                        SUCCEEDED(child->searchPresentationStatus_)&&members(*child);},5000);
                    const auto childMembershipRead=membershipRead;const auto childMembers=handoffObservedMembers;
                    const auto childFolderView=child->folderView_;const auto childScopes=child->searchScopes_;
                    const auto childRules=child->searchScopeRules_;const auto childOrigin=child->searchWindowOrigin_;
                    const auto childProperties=child->searchFileProperties_;const auto childQuery=child->activeQuery_;
                    const auto childNavigation=child->navigationCount_;const auto childRevision=child->searchInteractionRevision_;
                    Pidl childLocation(child->currentPidl_?ILCloneFull(child->currentPidl_.get()):nullptr);
                    FOLDERVIEWMODE actualMode{};int actualSize=0;SearchViewPresentation actualPresentation;
                    auto viewRead=childReady?childFolderView->GetViewModeAndIconSize(&actualMode,&actualSize):E_UNEXPECTED;
                    if(SUCCEEDED(viewRead))viewRead=captureSearchViewPresentation(childFolderView.Get(),&actualPresentation);
                    DWORD scopeCount=0;bool scopeMetadata=childReady&&childScopes&&SUCCEEDED(childScopes->GetCount(&scopeCount))&&scopeCount==2;
                    for(DWORD index=0;scopeMetadata&&index<scopeCount;++index){ComPtr<IShellItem> value;scopeMetadata=SUCCEEDED(childScopes->GetItemAt(index,&value))&&
                        sameItem(value.Get(),index?secondScope.Get():firstScope.Get());}
                    scopeMetadata=scopeMetadata&&childRules.size()==seed.rules.size();
                    for(size_t index=0;scopeMetadata&&index<seed.rules.size();++index){const auto rule=childRules[index];
                        scopeMetadata=sameItem(rule.folder.Get(),seed.rules[index].folder.Get())&&rule.recursive==seed.rules[index].recursive&&rule.excluded==seed.rules[index].excluded;}
                    const bool metadata=childReady&&childProperties&&*childProperties==properties&&
                        sameItem(childOrigin.Get(),secondScope.Get())&&childQuery==handoffQuery&&textOf(child->search_)==handoffQuery;
                    const bool stableChild=childReady&&!child->navigating_&&child->searchActive_&&child->folderView_.Get()==childFolderView.Get()&&
                        child->navigationCount_==childNavigation&&child->searchInteractionRevision_==childRevision&&child->activeQuery_==childQuery&&
                        childLocation&&child->currentPidl_&&ILIsEqual(childLocation.get(),child->currentPidl_.get());
                    check(explicitRules?"search_new_window_list_complete_native_context":"search_new_window_content_complete_native_context",
                        parentReady&&stableChild&&SUCCEEDED(viewRead)&&actualMode==(explicitRules?FVM_LIST:FVM_CONTENT)&&actualSize==(explicitRules?16:32)&&
                        transferred.presentation&&sameHandoffPresentation(actualPresentation,*transferred.presentation)&&scopeMetadata&&metadata&&
                        child->execute(NewWindow)==E_ACCESSDENIED,
                        L"variant="+std::wstring(explicitRules?L"List":L"Content")+L"; stage="+hresultMessage(operation)+L"; view="+hresultMessage(viewRead)+
                        L"; mode="+std::to_wstring(actualMode)+L"; size="+std::to_wstring(actualSize)+L"; scopes="+std::to_wstring(scopeCount)+
                        L"; prepare="+hresultMessage(prepared)+L"; create="+hresultMessage(created)+L"; parentReady="+std::to_wstring(parentReady)+
                        L"; parentNavigation="+std::to_wstring(parentNavigations)+L"; parentPresentation="+hresultMessage(parentPresentationRead)+
                        L"; parentItems="+hresultMessage(parentMembershipRead)+L"/"+std::to_wstring(parentMembers)+L"; capture="+hresultMessage(captured)+
                        L"; consume="+hresultMessage(consumed)+L"; childPrepare="+hresultMessage(childPrepared)+L"; childCreate="+hresultMessage(childCreated)+
                        L"; childReady="+std::to_wstring(childReady)+L"; childItems="+hresultMessage(childMembershipRead)+L"/"+std::to_wstring(childMembers)+
                        L"; exact metadata/scope/stableChild/presentation="+std::to_wstring(metadata)+L"/"+std::to_wstring(scopeMetadata)+L"/"+
                        std::to_wstring(stableChild)+L"/"+std::to_wstring(transferred.presentation&&sameHandoffPresentation(actualPresentation,*transferred.presentation))+
                        L"; actual/original explicit rule counts="+std::to_wstring(childRules.size())+L"/"+std::to_wstring(seed.rules.size()));
                    std::wstring dateDetail;
                    const bool nativeDate=childReady&&datePublication(*child,dateDetail);
                    check(explicitRules?"search_new_window_list_native_date_selected_item":"search_new_window_content_native_date_selected_item",nativeDate,dateDetail);
                    auto originOperation=childReady?child->execute(CloseSearch):E_UNEXPECTED;
                    const bool closed=SUCCEEDED(originOperation)&&pumpUntil([&]{return atOrigin(*child);},5000);
                    if(closed)originOperation=child->execute(Back);else originOperation=E_UNEXPECTED;
                    const bool historyRestored=SUCCEEDED(originOperation)&&pumpUntil([&]{return child->activeQuery_==handoffQuery&&
                        !child->searchPresentationPending_&&members(*child);},5000);
                    SearchViewPresentation restoredPresentation;
                    const auto restoredView=child->folderView_;const auto restoredOrigin=child->searchWindowOrigin_;
                    const auto restoredProperties=child->searchFileProperties_;const auto restoredNavigation=child->navigationCount_;
                    const auto restoredRevision=child->searchInteractionRevision_;
                    bool originPreserved=historyRestored&&restoredProperties&&*restoredProperties==properties&&
                        sameItem(restoredOrigin.Get(),secondScope.Get())&&transferred.presentation&&
                        SUCCEEDED(captureSearchViewPresentation(restoredView.Get(),&restoredPresentation))&&
                        sameHandoffPresentation(restoredPresentation,*transferred.presentation)&&!child->navigating_&&
                        child->searchActive_&&child->activeQuery_==handoffQuery&&child->folderView_.Get()==restoredView.Get()&&
                        child->navigationCount_==restoredNavigation&&child->searchInteractionRevision_==restoredRevision;
                    if(historyRestored){
                        if(explicitRules)SetWindowTextW(child->search_,L"");
                        else{
                            const auto refined=handoffQuery+L" AND System.Size:>0";SetWindowTextW(child->search_,refined.c_str());
                            originPreserved=originPreserved&&pumpUntil([&]{return child->activeQuery_==refined&&!child->pendingLiveSearch_&&
                                !child->liveSearchPolicy_.waiting()&&members(*child);},5000);
                            SendMessageW(child->search_,WM_KEYDOWN,VK_ESCAPE,0);
                        }
                        originPreserved=originPreserved&&pumpUntil([&]{return atOrigin(*child);},5000);
                    }
                    check(explicitRules?"search_new_window_clear_and_close_use_transferred_origin":"search_new_window_escape_close_and_history_use_transferred_origin",
                        closed&&historyRestored&&originPreserved,L"variant="+std::wstring(explicitRules?L"List":L"Content")+
                        L"; close="+std::to_wstring(closed)+L"; history="+std::to_wstring(historyRestored)+L"; return="+std::to_wstring(originPreserved));
                    check(explicitRules?"search_new_window_list_parent_state_unchanged":"search_new_window_content_parent_state_unchanged",
                        parentReady&&members(*parent)&&parent->view_.Get()==parentView.Get()&&parent->navigationCount_==parentNavigation&&
                        parent->history_.size()==parentHistory&&parent->historyIndex_==parentHistoryIndex&&parent->currentPidl_&&parentLocation&&
                        ILIsEqual(parent->currentPidl_.get(),parentLocation.get())&&parent->activeQuery_==handoffQuery&&
                        parent->searchFileProperties_&&*parent->searchFileProperties_==properties);
                }
                {
                    // A real native browse to a deleted, exclusively owned
                    // empty origin must not erase a still-active query.
                    const auto unavailablePath=handoffRoot/L"Unavailable Close origin";
                    std::filesystem::create_directory(unavailablePath);
                    ComPtr<IShellItem> unavailableOrigin;
                    auto unavailableRead=SHCreateItemFromParsingName(unavailablePath.c_str(),nullptr,IID_PPV_ARGS(&unavailableOrigin));
                    FILE_ID_INFO unavailableId{};
                    if(SUCCEEDED(unavailableRead))unavailableRead=nativeFileIdentity(unavailablePath,unavailableId);
                    SearchWindowContext seed;seed.query=handoffQuery;seed.primaryScope=firstScope;seed.closeOrigin=unavailableOrigin;
                    seed.scopes=scopes;seed.recursive=false;
                    seed.rules={{firstScope,false,false},{secondScope,false,false},{nestedScope,true,true}};
                    SearchViewPresentation full;full.mode=SearchViewMode::Content;full.iconSize=32;
                    full.visibleColumns=std::vector<std::wstring>{L"System.ItemNameDisplay",L"System.Size",L"System.DateModified"};
                    full.groupBy=SearchViewOrder{L"System.Kind",SORT_ASCENDING};
                    full.sort=std::vector<SearchViewOrder>{{L"System.DateModified",SORT_DESCENDING},{L"System.ItemNameDisplay",SORT_ASCENDING}};
                    seed.presentation=full;seed.fileProperties=properties;
                    std::unique_ptr<ExplorerApp,decltype(releaseApp)> child(new ExplorerApp(instance_,true,requestedRibbonLayout_),releaseApp);
                    const auto prepared=SUCCEEDED(stage)&&SUCCEEDED(unavailableRead)?child->prepareSearchWindowContext(seed):E_UNEXPECTED;
                    const auto created=SUCCEEDED(prepared)?child->create(L""):prepared;
                    const bool childReady=SUCCEEDED(created)&&pumpUntil([&]{return child->searchActive_&&!child->searchPresentationPending_&&
                        SUCCEEDED(child->searchPresentationStatus_)&&members(*child);},5000);
                    SearchWindowContext before;
                    const auto captured=childReady?child->currentSearchWindowContext(&before):E_UNEXPECTED;
                    const auto literal=textOf(child->search_);const auto base=child->searchBase_;const auto filters=child->searchFilters_;
                    const auto failedCloseView=child->view_;const auto nativeFolder=child->folderView_;
                    const auto failedCloseNavigation=child->navigationCount_;const auto historyIndex=child->historyIndex_;
                    const auto recent=child->recentSearches_;const auto addresses=child->typedAddresses_;
                    const auto errorBefore=child->lastError_;
                    Pidl location(child->currentPidl_?ILCloneFull(child->currentPidl_.get()):nullptr);
                    Pidl scope(child->searchScope_?ILCloneFull(child->searchScope_.get()):nullptr);
                    std::vector<Pidl> history;bool historyCopied=true;
                    for(const auto& entry:child->history_){Pidl copy(entry?ILCloneFull(entry.get()):nullptr);
                        historyCopied=historyCopied&&copy!=nullptr;history.push_back(std::move(copy));}
                    const bool failedCloseReady=childReady&&captured==S_OK&&before.presentation&&before.fileProperties&&
                        *before.fileProperties==properties&&literal==handoffQuery&&location&&scope&&historyCopied&&
                        child->searchWindowOrigin_&&std::filesystem::is_empty(unavailablePath);
                    const BOOL removed=failedCloseReady?RemoveDirectoryW(unavailablePath.c_str()):FALSE;
                    const DWORD removeError=removed?ERROR_SUCCESS:failedCloseReady?GetLastError():ERROR_NOT_READY;
                    const auto close=removed?child->execute(CloseSearch):E_UNEXPECTED;
                    const bool failedCallback=pumpUntil([&]{return !child->navigating_&&(FAILED(close)||
                        (child->lastError_!=errorBefore&&child->lastError_==
                            L"This location could not be opened. Check its availability and your permissions."));},5000);
                    SearchViewPresentation afterPresentation;
                    const auto failedClosePresentationRead=failedCallback&&child->folderView_?
                        captureSearchViewPresentation(child->folderView_.Get(),&afterPresentation):E_UNEXPECTED;
                    const bool presentationPreserved=failedClosePresentationRead==S_OK&&before.presentation&&
                        sameHandoffPresentation(afterPresentation,*before.presentation);
                    bool rulesPreserved=child->searchScopeRules_.size()==before.rules.size();
                    for(size_t index=0;rulesPreserved&&index<before.rules.size();++index)
                        rulesPreserved=child->searchScopeRules_[index].folder.Get()==before.rules[index].folder.Get()&&
                            child->searchScopeRules_[index].recursive==before.rules[index].recursive&&
                            child->searchScopeRules_[index].excluded==before.rules[index].excluded;
                    const auto sameBytes=[](PCIDLIST_ABSOLUTE left,PCIDLIST_ABSOLUTE right){return left&&right&&
                        ILGetSize(left)==ILGetSize(right)&&std::memcmp(left,right,ILGetSize(left))==0;};
                    bool historyPreserved=historyCopied&&child->history_.size()==history.size()&&child->historyIndex_==historyIndex;
                    for(size_t index=0;historyPreserved&&index<history.size();++index)
                        historyPreserved=sameBytes(history[index].get(),child->history_[index].get());
                    const bool resultsPreserved=failedCallback&&members(*child);
                    const auto desktop=PrivateDesktop::current();
                    const bool literalPreserved=textOf(child->search_)==literal&&child->activeQuery_==before.query&&
                        child->searchBase_==base&&child->searchFilters_==filters;
                    const bool metadataPreserved=rulesPreserved&&child->searchScopes_.Get()==before.scopes.Get()&&
                        child->searchRecursive_==before.recursive&&child->searchFileProperties_==before.fileProperties&&
                        child->searchWindowOrigin_.Get()==before.closeOrigin.Get()&&sameBytes(scope.get(),child->searchScope_.get());
                    const bool hostPreserved=historyPreserved&&child->navigationCount_==failedCloseNavigation&&child->recentSearches_==recent&&
                        child->typedAddresses_==addresses&&sameBytes(location.get(),child->currentPidl_.get())&&
                        child->view_.Get()==failedCloseView.Get()&&child->folderView_.Get()==nativeFolder.Get()&&
                        !child->navigating_&&!child->closing_&&child->searchActive_&&!IsWindowVisible(child->window_)&&
                        desktop&&SUCCEEDED(desktop->verifyIsolation());
                    check("close_search_unavailable_native_origin_preserves_literal_results_metadata_and_history",
                        failedCloseReady&&removed&&failedCallback&&literalPreserved&&metadataPreserved&&presentationPreserved&&resultsPreserved&&hostPreserved,
                        L"prepare/create/capture="+hresultMessage(prepared)+L"/"+hresultMessage(created)+L"/"+hresultMessage(captured)+
                        L"; ready/removed/error="+std::to_wstring(failedCloseReady)+L"/"+std::to_wstring(removed!=FALSE)+L"/"+std::to_wstring(removeError)+
                        L"; native close="+hresultMessage(close)+L"; failure settled="+std::to_wstring(failedCallback)+
                        L"; actual failure receipt="+std::to_wstring(child->lastError_!=errorBefore)+
                        L"; literal/metadata/presentation/results/host="+std::to_wstring(literalPreserved)+L"/"+
                        std::to_wstring(metadataPreserved)+L"/"+std::to_wstring(presentationPreserved)+L"/"+
                        std::to_wstring(resultsPreserved)+L"/"+std::to_wstring(hostPreserved));
                }
                bool sourcesPreserved=sourceReads;
                for(size_t index=0;index<files.size();++index){FILE_ID_INFO after{};std::ifstream file(files[index],std::ios::binary);
                    const std::string bytes((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
                    sourcesPreserved=sourcesPreserved&&SUCCEEDED(nativeFileIdentity(files[index],after))&&nativeIdentity(after)==nativeIdentity(originalIds[index])&&
                        bytes=="owned handoff source";}
                const auto desktop=PrivateDesktop::current();
                check("search_new_window_owned_sources_main_host_and_private_desktop_unchanged",sourcesPreserved&&desktop&&SUCCEEDED(desktop->verifyIsolation())&&
                    history_.size()==mainHistory&&historyIndex_==mainHistoryIndex&&navigationCount_==mainNavigation&&activeQuery_==mainQuery&&
                    view_.Get()==mainView.Get()&&mainLocation&&currentPidl_&&ILIsEqual(mainLocation.get(),currentPidl_.get())&&!IsWindowVisible(window_));
            }
            {
                constexpr std::array<const wchar_t*, 8> relativeDates{
                    L"Today", L"Yesterday", L"ThisWeek", L"LastWeek", L"ThisMonth", L"LastMonth", L"ThisYear", L"LastYear"};
                const std::wstring presetBase = L"System.Size:>0";
                Pidl presetScope(ILCloneFull(currentPidl_.get()));
                auto choiceRead = startSearch(presetBase, true, 2, L"System.Size:System.Size#Tiny");
                bool mappedPresets = SUCCEEDED(choiceRead) && pumpUntil([&] {
                    return !navigating_ && searchActive_ && searchBase_ == presetBase;
                }, 5000);
                std::vector<NamespaceSubcommandMetadata> nativeDateChoices;
                const auto nativeDateRead = namespaceCommandChildren(L"Windows.SearchFilterDate", nullptr, view_.Get(), &nativeDateChoices);
                std::vector<NamespaceSubcommandMetadata> nativeSizeChoices;
                const auto nativeSizeRead = namespaceCommandChildren(L"Windows.SearchFilterSize", nullptr, view_.Get(), &nativeSizeChoices);
                bool exactHostMenus = SUCCEEDED(nativeDateRead) && nativeDateChoices.size() == relativeDates.size() &&
                    SUCCEEDED(nativeSizeRead) && nativeSizeChoices.size() == 7 &&
                    std::all_of(nativeSizeChoices.begin(), nativeSizeChoices.end(), [](const auto& entry) {
                        return !entry.label.empty() && entry.flags == ECF_DEFAULT && entry.children.empty();
                    });
                const auto originalPresetQuery = activeQuery_;
                const auto originalPresetFilters = searchFilters_;
                const auto originalPresetHistory = historyIndex_;
                const auto originalPresetNavigation = navigationCount_;
                for (const auto command : {SearchDateMenu, SearchKindMenu, SearchSizeMenu}) {
                    struct SearchMenuOwner { HMENU value = CreatePopupMenu(); ~SearchMenuOwner() { if (value) DestroyMenu(value); } } menu;
                    std::vector<std::wstring> expressions;
                    const auto menuRead = appendSearchRefinementMenu(command, menu.value, &expressions);
                    const auto gallery = ribbonItems(command);
                    bool exact = SUCCEEDED(menuRead) && GetMenuItemCount(menu.value) == static_cast<int>(gallery.size()) &&
                        expressions.size() == gallery.size() && (command == SearchDateMenu ? gallery.size() == 8 :
                            command == SearchSizeMenu ? gallery.size() == 7 : gallery.size() > 6);
                    for (size_t index = 0; exact && index < gallery.size(); ++index) {
                        MENUITEMINFOW item{sizeof(item)}; item.fMask = MIIM_ID | MIIM_STATE | MIIM_STRING;
                        exact = GetMenuItemInfoW(menu.value, static_cast<UINT>(index), TRUE, &item) != FALSE;
                        std::wstring label(static_cast<size_t>(item.cch) + 1, L'\0');
                        if (exact) {
                            item.dwTypeData = label.data(); item.cch = static_cast<UINT>(label.size());
                            exact = GetMenuItemInfoW(menu.value, static_cast<UINT>(index), TRUE, &item) != FALSE;
                            label.resize(item.cch);
                        }
                        std::wstring escaped;
                        for (const auto character : gallery[index].label) { escaped += character; if (character == L'&') escaped += character; }
                        exact = exact && item.wID == 28000 + index && !(item.fState & (MFS_DISABLED | MFS_GRAYED)) &&
                            label == escaped && !expressions[index].empty();
                        if (command == SearchDateMenu) exact = exact && index < nativeDateChoices.size() &&
                            gallery[index].label == nativeDateChoices[index].label && expressions[index] ==
                            std::wstring(L"System.DateModified:System.StructuredQueryType.DateTime#") + relativeDates[index];
                        if (command == SearchSizeMenu) exact = exact && index < nativeSizeChoices.size() &&
                            gallery[index].label == nativeSizeChoices[index].label;
                    }
                    exactHostMenus = exactHostMenus && exact;
                }
                check("search_refinement_host_popup_snapshots_keep_all_native_gallery_choices", exactHostMenus &&
                    activeQuery_ == originalPresetQuery && searchFilters_ == originalPresetFilters &&
                    historyIndex_ == originalPresetHistory && navigationCount_ == originalPresetNavigation &&
                    searchScope_ && ILIsEqual(searchScope_.get(), presetScope.get()),
                    L"Actual owned HMENU Date8/Kind-property-list/Size7 matches gallery and native Date/Size providers; no menu displayed");
                for (UINT index = 0; mappedPresets && index < relativeDates.size(); ++index) {
                    previousCount = navigationCount_;
                    choiceRead = executeRibbonItem(SearchDateMenu, index);
                    const auto expected = std::wstring(L"System.DateModified:System.StructuredQueryType.DateTime#") + relativeDates[index];
                    mappedPresets = SUCCEEDED(choiceRead) && pumpUntil([&] {
                        return !navigating_ && navigationCount_ > previousCount && searchFilters_[1] == expected;
                    }, 5000) && searchBase_ == presetBase && searchFilters_[2] == L"System.Size:System.Size#Tiny" &&
                        searchScope_ && ILIsEqual(searchScope_.get(), presetScope.get()) &&
                        activeQuery_.find(expected) != std::wstring::npos;
                }
                check("date_modified_host_presets_replace_only_date_category", mappedPresets &&
                    executeRibbonItem(SearchDateMenu, static_cast<UINT>(relativeDates.size())) == E_INVALIDARG,
                    L"eight native positions map to canonical DateTime tokens; last=" + hresultMessage(choiceRead));
                const auto closed = execute(CloseSearch);
                check("date_modified_preset_search_returns_to_owned_scope", SUCCEEDED(closed) && pumpUntil([&] {
                    return !navigating_ && !searchActive_ && currentPidl_ && ILIsEqual(currentPidl_.get(), presetScope.get());
                }, 5000));
            }
            {
                // The native ancestry model must survive both deep paths and
                // narrow presentation. Use actual folder objects/FileIDs;
                // no Shell input, personal locations or Drop is invoked.
                const auto breadcrumbRoot = fixture / L"Subfolder" / L"Breadcrumb identity";
                std::vector<std::filesystem::path> ownedAncestors{breadcrumbRoot};
                std::filesystem::create_directory(breadcrumbRoot);
                for (unsigned depth = 1; depth <= 12; ++depth) {
                    ownedAncestors.push_back(ownedAncestors.back() / (L"\u968e-" + std::to_wstring(depth) + L"-&"));
                    std::filesystem::create_directory(ownedAncestors.back());
                }
                const auto marker = ownedAncestors.back() / L"exact-owned-leaf.txt";
                std::ofstream(marker) << "breadcrumb source stays unchanged";
                const auto readBreadcrumbIdentity = [](const std::filesystem::path& path, NativeFileIdentity& result) {
                    FILE_ID_INFO identity{};
                    const auto hr = nativeFileIdentity(path, identity);
                    if (SUCCEEDED(hr)) {
                        std::array<BYTE, 16> id{};
                        std::copy(std::begin(identity.FileId.Identifier), std::end(identity.FileId.Identifier), id.begin());
                        result = {identity.VolumeSerialNumber, id};
                    }
                    return hr;
                };
                std::vector<NativeFileIdentity> originalAncestorIds(ownedAncestors.size());
                bool originalIdentities = true;
                for (size_t index = 0; index < ownedAncestors.size(); ++index)
                    originalIdentities = SUCCEEDED(readBreadcrumbIdentity(ownedAncestors[index], originalAncestorIds[index])) && originalIdentities;
                NativeFileIdentity markerId{};
                originalIdentities = SUCCEEDED(readBreadcrumbIdentity(marker, markerId)) && originalIdentities;
                auto opened = navigate(ownedAncestors.back().wstring());
                const bool deepReady = SUCCEEDED(opened) && pumpUntil([&] {
                    return !navigating_ && folderView_ && atLocation(ownedAncestors.back());
                }, 5000);
                ComPtr<IShellItem> emptyDesktop;
                ITEMIDLIST desktopId{};
                auto nativeRead = SHCreateItemFromIDList(&desktopId, IID_PPV_ARGS(&emptyDesktop));
                std::vector<ComPtr<IShellItem>> independentAncestors;
                ComPtr<IShellItem> nativeAncestor;
                if (SUCCEEDED(nativeRead)) nativeRead = currentFolder(nativeAncestor);
                bool reachedDesktop = false;
                while (SUCCEEDED(nativeRead) && nativeAncestor && independentAncestors.size() < 128) {
                    independentAncestors.push_back(nativeAncestor);
                    int comparison = 1;
                    nativeRead = nativeAncestor->Compare(emptyDesktop.Get(), SICHINT_CANONICAL, &comparison);
                    if (FAILED(nativeRead)) break;
                    if (!comparison) { reachedDesktop = true; break; }
                    ComPtr<IShellItem> parent;
                    nativeRead = nativeAncestor->GetParent(&parent); nativeAncestor = std::move(parent);
                }
                std::reverse(independentAncestors.begin(), independentAncestors.end());
                bool completeModel = deepReady && originalIdentities && reachedDesktop &&
                    breadcrumbsPidls_.size() == independentAncestors.size() && breadcrumbsPidls_.size() > 12 &&
                    breadcrumbLabels_.size() == breadcrumbsPidls_.size();
                for (size_t index = 0; completeModel && index < independentAncestors.size(); ++index) {
                    ComPtr<IShellItem> actual;
                    int comparison = 1;
                    nativeRead = SHCreateItemFromIDList(breadcrumbsPidls_[index].get(), IID_PPV_ARGS(&actual));
                    if (SUCCEEDED(nativeRead)) nativeRead = actual->Compare(independentAncestors[index].Get(), SICHINT_CANONICAL, &comparison);
                    completeModel = SUCCEEDED(nativeRead) && !comparison;
                }
                check("deep_breadcrumb_retains_complete_native_parent_chain", completeModel,
                    L"model=" + std::to_wstring(breadcrumbsPidls_.size()) + L"; native=" + std::to_wstring(independentAncestors.size()) +
                    L"; Desktop reached=" + std::to_wstring(reachedDesktop) + L"; native read=" + hresultMessage(nativeRead));
                RECT originalBounds{}; GetWindowRect(breadcrumbs_, &originalBounds);
                MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&originalBounds), 2);
                const auto applyWidth = [&](int width) {
                    SetWindowPos(breadcrumbs_, nullptr, 0, 0, width, originalBounds.bottom - originalBounds.top,
                        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                    fitBreadcrumbs(width);
                };
                applyWidth(px(220));
                const auto overflowGeneration = navigationCount_;
                struct BreadcrumbMenuOwner { HMENU value = nullptr; ~BreadcrumbMenuOwner() { if (value) DestroyMenu(value); } } overflowMenu;
                std::vector<Pidl> overflowTargets;
                const auto overflowRead = createBreadcrumbOverflowMenu(&overflowMenu.value, &overflowTargets);
                bool exactOverflow = completeModel && overflowRead == S_OK && !breadcrumbHiddenAncestors_.empty() &&
                    overflowTargets.size() == breadcrumbHiddenAncestors_.size() &&
                    GetMenuItemCount(overflowMenu.value) == static_cast<int>(overflowTargets.size()) &&
                    breadcrumbButtons_.front() == 0 && breadcrumbButtons_.back() == breadcrumbsPidls_.size() - 1 &&
                    SendMessageW(breadcrumbs_, TB_COMMANDTOINDEX, BreadcrumbOverflow, 0) >= 0;
                size_t ownedTarget = overflowTargets.size();
                for (size_t row = 0; exactOverflow && row < overflowTargets.size(); ++row) {
                    const auto index = breadcrumbHiddenAncestors_[row];
                    ComPtr<IShellItem> actual;
                    int comparison = 1;
                    nativeRead = SHCreateItemFromIDList(overflowTargets[row].get(), IID_PPV_ARGS(&actual));
                    if (SUCCEEDED(nativeRead)) nativeRead = actual->Compare(independentAncestors[index].Get(), SICHINT_CANONICAL, &comparison);
                    MENUITEMINFOW entry{sizeof(entry)}; entry.fMask = MIIM_STRING | MIIM_ID | MIIM_STATE;
                    bool readMenu = GetMenuItemInfoW(overflowMenu.value, static_cast<UINT>(row), TRUE, &entry) != FALSE;
                    std::wstring title(static_cast<size_t>(entry.cch) + 1, L'\0');
                    if (readMenu) {
                        entry.dwTypeData = title.data(); entry.cch = static_cast<UINT>(title.size());
                        readMenu = GetMenuItemInfoW(overflowMenu.value, static_cast<UINT>(row), TRUE, &entry) != FALSE; title.resize(entry.cch);
                    }
                    std::wstring escaped;
                    const auto nativeTitle = itemName(independentAncestors[index].Get(), SIGDN_NORMALDISPLAY);
                    for (const auto character : nativeTitle) { escaped += character; if (character == L'&') escaped += character; }
                    exactOverflow = SUCCEEDED(nativeRead) && !comparison && readMenu && entry.wID == row + 1 &&
                        !(entry.fState & (MFS_DISABLED | MFS_GRAYED)) && title == escaped;
                    NativeFileIdentity candidate{};
                    const auto candidatePath = itemName(actual.Get(), SIGDN_FILESYSPATH);
                    if (!candidatePath.empty() && SUCCEEDED(readBreadcrumbIdentity(candidatePath, candidate)) &&
                        candidate == originalAncestorIds[2]) ownedTarget = row;
                }
                check("deep_breadcrumb_overflow_exact_native_targets_labels_and_order", exactOverflow && ownedTarget < overflowTargets.size(),
                    L"hidden=" + std::to_wstring(breadcrumbHiddenAncestors_.size()) + L"; native HMENU rows=" +
                    std::to_wstring(overflowMenu.value ? GetMenuItemCount(overflowMenu.value) : -1) + L"; owned target present=" +
                    std::to_wstring(ownedTarget < overflowTargets.size()));
                bool nativeBounds = exactOverflow;
                for (size_t slot = 0; nativeBounds && slot < breadcrumbButtons_.size(); ++slot) {
                    RECT buttonBounds{};
                    nativeBounds = SendMessageW(breadcrumbs_, TB_GETRECT, BreadcrumbFirst + slot, reinterpret_cast<LPARAM>(&buttonBounds)) &&
                        buttonBounds.right > buttonBounds.left && buttonBounds.left >= 0 && buttonBounds.right <= px(220);
                }
                check("deep_breadcrumb_narrow_native_buttons_remain_inside_owned_control", nativeBounds);
                ComPtr<IShellItem> rootParent, rootSelected, actualRootParent;
                const auto rootRead = breadcrumbDropdownTarget(BreadcrumbFirst, &rootParent, &rootSelected);
                int rootComparison = 1, selectedComparison = 1, parentComparison = 1;
                const bool rootDropdown = completeModel && rootRead == S_OK && rootSelected &&
                    SUCCEEDED(rootParent->Compare(emptyDesktop.Get(), SICHINT_CANONICAL, &rootComparison)) && !rootComparison &&
                    SUCCEEDED(rootSelected->Compare(independentAncestors[1].Get(), SICHINT_CANONICAL, &selectedComparison)) && !selectedComparison &&
                    SUCCEEDED(rootSelected->GetParent(&actualRootParent)) &&
                    SUCCEEDED(actualRootParent->Compare(rootParent.Get(), SICHINT_CANONICAL, &parentComparison)) && !parentComparison;
                check("deep_breadcrumb_desktop_dropdown_keeps_actual_adjacent_child", rootDropdown, hresultMessage(rootRead));
                applyWidth(px(65));
                BreadcrumbMenuOwner minimumMenu;
                std::vector<Pidl> minimumTargets;
                const auto minimumRead = createBreadcrumbOverflowMenu(&minimumMenu.value, &minimumTargets);
                bool minimumModel = completeModel && minimumRead == S_OK && breadcrumbButtons_.size() == 1 &&
                    breadcrumbButtons_.front() == breadcrumbsPidls_.size() - 1 &&
                    breadcrumbHiddenAncestors_.size() + 1 == breadcrumbsPidls_.size() &&
                    minimumTargets.size() == breadcrumbHiddenAncestors_.size() &&
                    GetMenuItemCount(minimumMenu.value) == static_cast<int>(minimumTargets.size()) &&
                    navigationCount_ == overflowGeneration;
                for (size_t row = 0; minimumModel && row < minimumTargets.size(); ++row) {
                    ComPtr<IShellItem> actual;
                    int comparison = 1;
                    nativeRead = SHCreateItemFromIDList(minimumTargets[row].get(), IID_PPV_ARGS(&actual));
                    if (SUCCEEDED(nativeRead)) nativeRead = actual->Compare(independentAncestors[row].Get(), SICHINT_CANONICAL, &comparison);
                    minimumModel = breadcrumbHiddenAncestors_[row] == row && SUCCEEDED(nativeRead) && !comparison;
                }
                for (const UINT command : {static_cast<UINT>(BreadcrumbFirst), static_cast<UINT>(BreadcrumbOverflow), static_cast<UINT>(AddressList)}) {
                    RECT buttonBounds{};
                    minimumModel = SendMessageW(breadcrumbs_, TB_GETRECT, command, reinterpret_cast<LPARAM>(&buttonBounds)) &&
                        buttonBounds.right > buttonBounds.left && buttonBounds.left >= 0 && buttonBounds.right <= px(65) && minimumModel;
                }
                check("deep_breadcrumb_minimum_width_keeps_desktop_and_every_hidden_identity", minimumModel,
                    L"visible=" + std::to_wstring(breadcrumbButtons_.size()) + L"; hidden=" +
                    std::to_wstring(breadcrumbHiddenAncestors_.size()) + L"; native menu=" + hresultMessage(minimumRead));
                applyWidth(px(8192));
                bool expandedModel = completeModel && breadcrumbHiddenAncestors_.empty() &&
                    breadcrumbButtons_.size() == breadcrumbsPidls_.size() &&
                    SendMessageW(breadcrumbs_, TB_COMMANDTOINDEX, BreadcrumbOverflow, 0) < 0;
                std::optional<size_t> ownedSlot;
                for (size_t slot = 0; expandedModel && slot < breadcrumbButtons_.size(); ++slot) {
                    const auto index = breadcrumbButtons_[slot];
                    expandedModel = index == slot;
                    NativeFileIdentity candidate{};
                    const auto candidatePath = itemName(independentAncestors[index].Get(), SIGDN_FILESYSPATH);
                    if (!candidatePath.empty() && SUCCEEDED(readBreadcrumbIdentity(candidatePath, candidate)) &&
                        candidate == originalAncestorIds[2]) ownedSlot = slot;
                }
                ComPtr<IShellItem> ownedParent, ownedChild;
                BreadcrumbSnapshot childSnapshot;
                auto childRead = ownedSlot ? breadcrumbDropdownTarget(BreadcrumbFirst + static_cast<UINT>(*ownedSlot), &ownedParent, &ownedChild) : E_FAIL;
                if (SUCCEEDED(childRead)) childRead = enumerateBreadcrumbChildren(ownedParent.Get(), ownedChild.Get(), {}, &childSnapshot);
                NativeFileIdentity childId{};
                const bool exactChild = expandedModel && childRead == S_OK && childSnapshot.complete && childSnapshot.children.size() == 1 &&
                    childSnapshot.children.front().selected &&
                    SUCCEEDED(readBreadcrumbIdentity(itemName(childSnapshot.children.front().item.Get(), SIGDN_FILESYSPATH), childId)) &&
                    childId == originalAncestorIds[3];
                check("deep_breadcrumb_wide_restores_every_node_and_native_current_child", exactChild,
                    L"visible=" + std::to_wstring(breadcrumbButtons_.size()) + L"; native children=" +
                    std::to_wstring(childSnapshot.children.size()) + L"; read=" + hresultMessage(childRead));
                applyWidth(px(220));
                const auto beforeOverflowNavigation = navigationCount_;
                const auto overflowNavigate = ownedTarget < overflowTargets.size() ?
                    browseBreadcrumbTarget(overflowTargets[ownedTarget].get(), overflowGeneration) : E_FAIL;
                const bool overflowNavigationReady = SUCCEEDED(overflowNavigate) && pumpUntil([&] {
                    return !navigating_ && atLocation(ownedAncestors[2]);
                }, 5000);
                const auto beforeStaleNavigation = navigationCount_;
                const auto beforeStaleHistory = historyIndex_;
                const auto staleNavigate = ownedTarget < overflowTargets.size() ?
                    browseBreadcrumbTarget(overflowTargets[ownedTarget].get(), overflowGeneration) : E_FAIL;
                check("deep_breadcrumb_overflow_snapshot_native_navigation_and_stale_rejection", overflowNavigationReady &&
                    navigationCount_ == beforeOverflowNavigation + 1 && staleNavigate == E_ABORT &&
                    navigationCount_ == beforeStaleNavigation && historyIndex_ == beforeStaleHistory && atLocation(ownedAncestors[2]),
                    L"browse=" + hresultMessage(overflowNavigate) + L"; stale=" + hresultMessage(staleNavigate));
                opened = navigate(ownedAncestors.back().wstring());
                const bool returnedDeep = SUCCEEDED(opened) && pumpUntil([&] {
                    return !navigating_ && atLocation(ownedAncestors.back());
                }, 5000);
                applyWidth(px(8192));
                std::optional<size_t> routeSlot;
                for (size_t slot = 0; returnedDeep && slot < breadcrumbButtons_.size(); ++slot) {
                    ComPtr<IShellItem> target;
                    NativeFileIdentity candidate{};
                    if (SUCCEEDED(SHCreateItemFromIDList(breadcrumbsPidls_[breadcrumbButtons_[slot]].get(), IID_PPV_ARGS(&target)))) {
                        const auto candidatePath = itemName(target.Get(), SIGDN_FILESYSPATH);
                        if (!candidatePath.empty() && SUCCEEDED(readBreadcrumbIdentity(candidatePath, candidate)) &&
                            candidate == originalAncestorIds[4]) routeSlot = slot;
                    }
                }
                if (routeSlot) SendMessageW(window_, WM_COMMAND, MAKEWPARAM(BreadcrumbFirst + *routeSlot, BN_CLICKED), reinterpret_cast<LPARAM>(breadcrumbs_));
                check("deep_breadcrumb_real_toolbar_slot_routes_exact_native_ancestor", routeSlot && pumpUntil([&] {
                    return !navigating_ && atLocation(ownedAncestors[4]);
                }, 5000));
                bool sourcesUnchanged = originalIdentities;
                for (size_t index = 0; index < ownedAncestors.size(); ++index) {
                    NativeFileIdentity after{};
                    sourcesUnchanged = SUCCEEDED(readBreadcrumbIdentity(ownedAncestors[index], after)) && after == originalAncestorIds[index] && sourcesUnchanged;
                }
                NativeFileIdentity markerAfter{}; std::ifstream markerInput(marker);
                const std::string markerText((std::istreambuf_iterator<char>(markerInput)), std::istreambuf_iterator<char>());
                sourcesUnchanged = SUCCEEDED(readBreadcrumbIdentity(marker, markerAfter)) && markerAfter == markerId &&
                    markerText == "breadcrumb source stays unchanged" && sourcesUnchanged;
                layout();
                const auto desktop = PrivateDesktop::current();
                check("deep_breadcrumb_navigation_preserves_owned_sources_and_private_desktop", sourcesUnchanged && desktop &&
                    SUCCEEDED(desktop->verifyIsolation()) && !IsWindowVisible(window_));
            }
            {
                const auto cueFolder = fixture / L"Host cue-\u65e5\u672c\u8a9e-\U0001f680-100%-%s-%1-%n";
                std::filesystem::create_directory(cueFolder);
                const auto cueMarker = cueFolder / L"literal marker.txt";
                { std::ofstream cueOutput(cueMarker); cueOutput << "host cue source stays unchanged"; }
                auto cueIdentity = [](const FILE_ID_INFO& value) {
                    std::array<BYTE, 16> bytes{};
                    std::copy(std::begin(value.FileId.Identifier), std::end(value.FileId.Identifier), bytes.begin());
                    return NativeFileIdentity{value.VolumeSerialNumber, bytes};
                };
                FILE_ID_INFO cueFolderBefore{}, cueMarkerBefore{};
                const auto cueFolderIdentityRead = nativeFileIdentity(cueFolder, cueFolderBefore);
                const auto cueMarkerIdentityRead = nativeFileIdentity(cueMarker, cueMarkerBefore);
                const std::set<NativeFileIdentity> expectedCueItems{cueIdentity(cueMarkerBefore)};
                const auto cueOpened = navigate(cueFolder.wstring());
                HRESULT cueItemsRead = E_PENDING;
                const bool cueViewReady = SUCCEEDED(cueOpened) && pumpUntil([&] {
                    if (navigating_ || !folderView_ || !atLocation(cueFolder)) return false;
                    ComPtr<IShellItemArray> cueItems;
                    cueItemsRead = folderView_->Items(SVGIO_ALLVIEW, IID_PPV_ARGS(&cueItems));
                    std::set<NativeFileIdentity> actualCueItems;
                    if (SUCCEEDED(cueItemsRead)) cueItemsRead = nativeArrayIdentities(cueItems.Get(), actualCueItems);
                    return SUCCEEDED(cueItemsRead) && actualCueItems == expectedCueItems;
                }, 5000);
                ComPtr<IShellItem> actualCueFolder;
                const auto cueFolderRead = currentFolder(actualCueFolder);
                const auto cueDisplayName = itemName(actualCueFolder.Get(), SIGDN_NORMALDISPLAY);
                const auto cueNativePath = itemName(actualCueFolder.Get(), SIGDN_FILESYSPATH);
                FILE_ID_INFO cueNativeIdentity{};
                const auto cueNativeRead = nativeFileIdentity(cueNativePath, cueNativeIdentity);
                UiString cueTemplate;
                const auto cueTemplateRead = loadUiString(UiText::SearchCue, &cueTemplate);
                const auto cueSlot = cueTemplate.text.find(L"%1");
                const bool cueSlotExact = cueSlot != std::wstring::npos && cueTemplate.text.find(L'%', cueSlot + 2) == std::wstring::npos &&
                    cueTemplate.text.find(L'%') == cueSlot;
                // Assemble the independent resource template literally. The
                // folder's percent tokens must survive the real Edit cue.
                const auto expectedCue = cueSlotExact ? cueTemplate.text.substr(0, cueSlot) + cueDisplayName +
                    cueTemplate.text.substr(cueSlot + 2) : std::wstring{};
                std::wstring actualCue(expectedCue.size() + 1, L'\0');
                const bool cueRead = SendMessageW(search_, EM_GETCUEBANNER, reinterpret_cast<WPARAM>(actualCue.data()),
                    static_cast<LPARAM>(actualCue.size())) != FALSE;
                actualCue.resize(wcsnlen_s(actualCue.data(), actualCue.size()));
                const bool cueNativeFolder = SUCCEEDED(cueFolderIdentityRead) && SUCCEEDED(cueMarkerIdentityRead) &&
                    cueViewReady && SUCCEEDED(cueFolderRead) && SUCCEEDED(cueNativeRead) &&
                    cueIdentity(cueNativeIdentity) == cueIdentity(cueFolderBefore) && cueDisplayName == cueFolder.filename().wstring();
                check("native_localized_search_cue_preserves_unicode_and_literal_percent_folder", cueNativeFolder &&
                    SUCCEEDED(cueTemplateRead) && cueSlotExact && cueRead && actualCue == expectedCue,
                    L"native folder=" + std::to_wstring(cueNativeFolder) + L"; open=" + hresultMessage(cueOpened) +
                    L"; native membership=" + hresultMessage(cueItemsRead) + L"; cue read=" + std::to_wstring(cueRead) +
                    L"; exact cue=" + std::to_wstring(actualCue == expectedCue) + L"; resource=" + std::to_wstring(cueTemplate.resourceId));
                auto tooltipNotification = [&](UINT command, wchar_t* buffer, int capacity) {
                    NMTBGETINFOTIPW info{};
                    info.hdr.hwndFrom = command == Refresh ? addressActions_ : nav_;
                    info.hdr.idFrom = static_cast<UINT_PTR>(GetDlgCtrlID(info.hdr.hwndFrom));
                    info.hdr.code = TBN_GETINFOTIPW;
                    info.iItem = static_cast<int>(command); info.pszText = buffer; info.cchTextMax = capacity;
                    SendMessageW(window_, WM_NOTIFY, info.hdr.idFrom, reinterpret_cast<LPARAM>(&info));
                };
                constexpr wchar_t tooltipGuard = 0x25a1;
                bool fullTooltipsExact = true;
                std::wstring expectedRefreshTip;
                for (const auto& [command, key] : std::array<std::pair<UINT, UiText>, 5>{{
                    {Back, UiText::BackTooltip}, {Forward, UiText::ForwardTooltip},
                    {HistoryMenu, UiText::RecentLocations}, {Up, UiText::UpTooltip}, {Refresh, UiText::RefreshTooltip}}}) {
                    UiString expectedTipResource;
                    const auto expectedTipRead = loadUiString(key, &expectedTipResource);
                    auto expectedTip = expectedTipResource.text;
                    bool expectedTipValid = SUCCEEDED(expectedTipRead);
                    if (command == Refresh) {
                        const auto refreshSlot = expectedTip.find(L"%s");
                        expectedTipValid = expectedTipValid && refreshSlot != std::wstring::npos &&
                            expectedTip.find(L'%') == refreshSlot && expectedTip.find(L'%', refreshSlot + 2) == std::wstring::npos;
                        if (expectedTipValid) expectedTip = expectedTip.substr(0, refreshSlot) + cueDisplayName + expectedTip.substr(refreshSlot + 2);
                        expectedRefreshTip = expectedTip;
                    }
                    const auto tipCapacity = static_cast<int>(expectedTip.size() + 1);
                    std::vector<wchar_t> tipBuffer(static_cast<size_t>(tipCapacity) + 2, tooltipGuard);
                    tooltipNotification(command, tipBuffer.data() + 1, tipCapacity);
                    const bool tipTerminated = tipBuffer[static_cast<size_t>(tipCapacity)] == L'\0';
                    fullTooltipsExact = expectedTipValid && tipBuffer.front() == tooltipGuard && tipBuffer.back() == tooltipGuard &&
                        tipTerminated && std::wstring(tipBuffer.data() + 1, expectedTip.size()) == expectedTip && fullTooltipsExact;
                }
                check("native_host_tooltip_notifications_preserve_full_resource_text", cueNativeFolder && fullTooltipsExact);
                std::array<wchar_t, 3> emptyTipBuffer{tooltipGuard, tooltipGuard, tooltipGuard};
                const auto untouchedTipBuffer = emptyTipBuffer;
                tooltipNotification(Refresh, emptyTipBuffer.data() + 1, 0);
                const bool zeroTipUnchanged = emptyTipBuffer == untouchedTipBuffer;
                tooltipNotification(Refresh, nullptr, 1);
                const bool nullTipUnchanged = emptyTipBuffer == untouchedTipBuffer;
                tooltipNotification(Refresh, emptyTipBuffer.data() + 1, 1);
                const bool oneTipTerminated = emptyTipBuffer.front() == tooltipGuard && emptyTipBuffer[1] == L'\0' &&
                    emptyTipBuffer.back() == tooltipGuard;
                const auto tipSurrogate = expectedRefreshTip.find(L"\U0001f680");
                const bool hasTipSurrogate = tipSurrogate != std::wstring::npos && tipSurrogate + 1 < expectedRefreshTip.size() &&
                    expectedRefreshTip[tipSurrogate] >= 0xd800 && expectedRefreshTip[tipSurrogate] <= 0xdbff &&
                    expectedRefreshTip[tipSurrogate + 1] >= 0xdc00 && expectedRefreshTip[tipSurrogate + 1] <= 0xdfff;
                const auto shortTipCapacity = hasTipSurrogate ? static_cast<int>(tipSurrogate + 2) : 1;
                std::vector<wchar_t> shortTipBuffer(static_cast<size_t>(shortTipCapacity) + 2, tooltipGuard);
                tooltipNotification(Refresh, shortTipBuffer.data() + 1, shortTipCapacity);
                const bool shortTipExact = hasTipSurrogate && shortTipBuffer.front() == tooltipGuard && shortTipBuffer.back() == tooltipGuard &&
                    shortTipBuffer[tipSurrogate + 1] == L'\0' && shortTipBuffer[tipSurrogate + 2] == tooltipGuard &&
                    std::wstring(shortTipBuffer.data() + 1, tipSurrogate) == expectedRefreshTip.substr(0, tipSurrogate);
                check("native_host_tooltip_notification_buffer_edges_keep_complete_utf16", zeroTipUnchanged && nullTipUnchanged &&
                    oneTipTerminated && shortTipExact,
                    L"zero=" + std::to_wstring(zeroTipUnchanged) + L"; null=" + std::to_wstring(nullTipUnchanged) +
                    L"; one=" + std::to_wstring(oneTipTerminated) + L"; surrogate=" + std::to_wstring(hasTipSurrogate) +
                    L"; bounded prefix=" + std::to_wstring(shortTipExact));
                FILE_ID_INFO cueFolderAfter{}, cueMarkerAfter{};
                std::ifstream cueInput(cueMarker);
                const std::string cueSourceText((std::istreambuf_iterator<char>(cueInput)), std::istreambuf_iterator<char>());
                const auto cueDesktop = PrivateDesktop::current();
                check("native_localized_search_cue_preserves_owned_sources_and_private_desktop",
                    SUCCEEDED(nativeFileIdentity(cueFolder, cueFolderAfter)) && SUCCEEDED(nativeFileIdentity(cueMarker, cueMarkerAfter)) &&
                    cueIdentity(cueFolderAfter) == cueIdentity(cueFolderBefore) && cueIdentity(cueMarkerAfter) == cueIdentity(cueMarkerBefore) &&
                    cueSourceText == "host cue source stays unchanged" && cueDesktop &&
                    SUCCEEDED(cueDesktop->verifyIsolation()) && !IsWindowVisible(window_));
            }
            {
                const auto rtlFolder = fixture / L"RTL-\u0627\u0644\u0645\u0644\u0641-\u05ea\u05d9\u05e7\u05d9\u05d4-Latin-%1";
                const auto rtlChild = rtlFolder / L"child-\u062a\u062c\u0631\u0628\u0629-\u05d9\u05dc\u05d3";
                std::filesystem::create_directories(rtlChild);
                const auto rtlMarker = rtlFolder / L"marker-\u062d\u0631\u0641-\u05d0.txt";
                { std::ofstream markerOutput(rtlMarker); markerOutput << "private RTL source stays unchanged"; }
                auto rtlIdentity = [](const FILE_ID_INFO& value) {
                    std::array<BYTE, 16> bytes{};
                    std::copy(std::begin(value.FileId.Identifier), std::end(value.FileId.Identifier), bytes.begin());
                    return NativeFileIdentity{value.VolumeSerialNumber, bytes};
                };
                FILE_ID_INFO rtlFolderBefore{}, rtlChildBefore{}, rtlMarkerBefore{};
                const bool rtlSourcesRead = SUCCEEDED(nativeFileIdentity(rtlFolder, rtlFolderBefore)) &&
                    SUCCEEDED(nativeFileIdentity(rtlChild, rtlChildBefore)) && SUCCEEDED(nativeFileIdentity(rtlMarker, rtlMarkerBefore));
                const auto rtlThreadLanguageBefore = GetThreadUILanguage();
                UiDirectionPolicy rtlLanguagePolicyBefore;
                const auto rtlLanguagePolicyRead = loadThreadUiDirection(&rtlLanguagePolicyBefore);
                const std::set<NativeFileIdentity> rtlExpectedItems{rtlIdentity(rtlChildBefore), rtlIdentity(rtlMarkerBefore)};
                auto releaseRtlApp = [](ExplorerApp* value) {
                    if (value->window_ && IsWindow(value->window_)) SendMessageW(value->window_, WM_CLOSE, 0, 0);
                    value->Release();
                };
                std::unique_ptr<ExplorerApp, decltype(releaseRtlApp)> rtlApp(
                    new ExplorerApp(instance_, true, requestedRibbonLayout_), releaseRtlApp);
                // This per-instance declaration is accepted only before HWND
                // creation on the verified never-switched private desktop.
                // Neither the thread's language nor process layout is changed.
                rtlApp->headlessDirectionOverride_ = true;
                rtlApp->showAllFolders_ = true; rtlApp->expandCurrent_ = true;
                const auto rtlCreated = rtlApp->create(rtlFolder.wstring());
                HRESULT rtlMembershipRead = E_PENDING;
                auto rtlMembership = [&] {
                    if (rtlApp->navigating_ || !rtlApp->folderView_) return false;
                    ComPtr<IShellItem> folder;
                    FILE_ID_INFO nativeFolderId{};
                    auto status = rtlApp->currentFolder(folder);
                    if (SUCCEEDED(status)) status = nativeFileIdentity(itemName(folder.Get(), SIGDN_FILESYSPATH), nativeFolderId);
                    if (FAILED(status) || rtlIdentity(nativeFolderId) != rtlIdentity(rtlFolderBefore)) return false;
                    ComPtr<IShellItemArray> items;
                    rtlMembershipRead = rtlApp->folderView_->Items(SVGIO_ALLVIEW, IID_PPV_ARGS(&items));
                    std::set<NativeFileIdentity> actual;
                    if (SUCCEEDED(rtlMembershipRead)) rtlMembershipRead = nativeArrayIdentities(items.Get(), actual);
                    return SUCCEEDED(rtlMembershipRead) && actual == rtlExpectedItems;
                };
                const bool rtlReady = rtlSourcesRead && SUCCEEDED(rtlCreated) && pumpUntil(rtlMembership, 5000);
                check("private_rtl_app_opens_exact_native_unicode_source", rtlReady,
                    L"create=" + hresultMessage(rtlCreated) + L"; membership=" + hresultMessage(rtlMembershipRead));
                if (rtlReady) {
                    MONITORINFO rtlMonitor{sizeof(rtlMonitor)};
                    RECT rtlOriginalWindow{};
                    const bool rtlPositioned = GetWindowRect(rtlApp->window_, &rtlOriginalWindow) &&
                        GetMonitorInfoW(MonitorFromWindow(rtlApp->window_, MONITOR_DEFAULTTONEAREST), &rtlMonitor) &&
                        SetWindowPos(rtlApp->window_, nullptr, rtlMonitor.rcWork.left + rtlApp->px(20),
                            rtlMonitor.rcWork.top + rtlApp->px(20),
                            std::min<LONG>(rtlOriginalWindow.right - rtlOriginalWindow.left,
                                rtlMonitor.rcWork.right - rtlMonitor.rcWork.left - rtlApp->px(40)),
                            std::min<LONG>(rtlOriginalWindow.bottom - rtlOriginalWindow.top,
                                rtlMonitor.rcWork.bottom - rtlMonitor.rcWork.top - rtlApp->px(40)),
                            SWP_NOZORDER | SWP_NOACTIVATE);
                    {
                        PrivatePresentation rtlPresentation(rtlApp->window_, true);
                        bool rtlNativeDc = rtlPositioned && rtlPresentation.ready;
                        // The stock EDIT consumes inherited LAYOUTRTL into its
                        // classic reading/alignment styles. Compare that class
                        // with a fresh native control without host subclasses;
                        // never force its DC to satisfy a generic mirror test.
                        struct RtlEditReference {
                            HWND value;
                            ~RtlEditReference() { if (value) DestroyWindow(value); }
                        } rtlEditReference{CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
                            0, 0, 160, 30, rtlApp->window_, nullptr, instance_, nullptr)};
                        constexpr LONG_PTR rtlEditDirectionMask = WS_EX_LAYOUTRTL | WS_EX_RIGHT | WS_EX_RTLREADING | WS_EX_LEFTSCROLLBAR;
                        constexpr LONG_PTR rtlEditNativeDirection = WS_EX_RIGHT | WS_EX_RTLREADING | WS_EX_LEFTSCROLLBAR;
                        const auto rtlEditReferenceStyle = GetWindowLongPtrW(rtlEditReference.value, GWL_EXSTYLE);
                        const auto rtlEditReferenceClassStyle = GetClassLongPtrW(rtlEditReference.value, GCL_STYLE);
                        HDC rtlEditReferenceDc = rtlEditReference.value ? GetDC(rtlEditReference.value) : nullptr;
                        const auto rtlEditReferenceLayout = rtlEditReferenceDc ? GetLayout(rtlEditReferenceDc) : GDI_ERROR;
                        const auto rtlEditReferenceMapMode = rtlEditReferenceDc ? GetMapMode(rtlEditReferenceDc) : 0;
                        if (rtlEditReferenceDc) ReleaseDC(rtlEditReference.value, rtlEditReferenceDc);
                        const bool rtlEditReferenceValid = rtlEditReference.value &&
                            (rtlEditReferenceStyle & rtlEditDirectionMask) == rtlEditNativeDirection &&
                            (rtlEditReferenceClassStyle & CS_PARENTDC) && rtlEditReferenceLayout == 0u && rtlEditReferenceMapMode == MM_TEXT;
                        unsigned rtlNativeControls = 0;
                        std::wstring rtlControlDiagnostic;
                        for (const auto& [control, role] : std::array<std::pair<HWND, const wchar_t*>, 7>{{
                             {rtlApp->window_, L"host"}, {rtlApp->nav_, L"navigation"}, {rtlApp->breadcrumbs_, L"breadcrumbs"},
                             {rtlApp->address_, L"address Edit"}, {rtlApp->addressActions_, L"refresh"},
                             {rtlApp->search_, L"search Edit"}, {rtlApp->ribbonCollapse_, L"collapse"}}}) {
                            bool mirrored = false;
                            HDC dc = GetDC(control);
                            const DWORD dcLayout = dc ? GetLayout(dc) : GDI_ERROR;
                            const auto dcMapMode = dc ? GetMapMode(dc) : 0;
                            if (dc) ReleaseDC(control, dc);
                            wchar_t controlClass[64]{};
                            GetClassNameW(control, controlClass, static_cast<int>(std::size(controlClass)));
                            const auto controlStyle = GetWindowLongPtrW(control, GWL_EXSTYLE);
                            const auto classStyle = GetClassLongPtrW(control, GCL_STYLE);
                            const bool nativeEdit = control == rtlApp->address_ || control == rtlApp->search_;
                            const bool native = SUCCEEDED(windowUiDirection(control, &mirrored)) &&
                                (nativeEdit ? rtlEditReferenceValid && !mirrored && _wcsicmp(controlClass, L"Edit") == 0 &&
                                    (controlStyle & rtlEditDirectionMask) == (rtlEditReferenceStyle & rtlEditDirectionMask) &&
                                    classStyle == rtlEditReferenceClassStyle && dcLayout == rtlEditReferenceLayout && dcMapMode == rtlEditReferenceMapMode :
                                    mirrored && !(controlStyle & WS_EX_RTLREADING) && dcLayout != GDI_ERROR && (dcLayout & LAYOUT_RTL));
                            rtlNativeDc = native && rtlNativeDc;
                            if (native) ++rtlNativeControls;
                            rtlControlDiagnostic += std::wstring(role) + L"(" + controlClass + L"): exStyle=" +
                                std::to_wstring(controlStyle) + L"; classStyle=" + std::to_wstring(classStyle) +
                                L"; dcLayout=" + std::to_wstring(dcLayout) + L"; mapMode=" + std::to_wstring(dcMapMode) +
                                L"; native=" + std::to_wstring(native) + L" | ";
                        }
                        bool rtlPopupStyles = true;
                        for (const auto toolbar : {rtlApp->nav_, rtlApp->breadcrumbs_, rtlApp->addressActions_}) {
                            const auto tooltip = reinterpret_cast<HWND>(SendMessageW(toolbar, TB_GETTOOLTIPS, 0, 0));
                            bool mirrored = false;
                            rtlPopupStyles = tooltip && SUCCEEDED(windowUiDirection(tooltip, &mirrored)) && mirrored && rtlPopupStyles;
                        }
                        bool collapseTipMirrored = false;
                        rtlPopupStyles = SUCCEEDED(windowUiDirection(rtlApp->ribbonCollapseTooltip_, &collapseTipMirrored)) &&
                            collapseTipMirrored && rtlPopupStyles;
                        check("private_rtl_host_inherits_native_layout_and_owned_popup_direction", rtlNativeDc && rtlPopupStyles,
                            L"native class/DC controls=" + std::to_wstring(rtlNativeControls) + L"/7; independent Edit=" +
                            std::to_wstring(rtlEditReferenceValid) + L"; tooltips=" + std::to_wstring(rtlPopupStyles) +
                            L"; " + rtlControlDiagnostic);
                        auto rtlBounds = [&](HWND control, RECT& screen, RECT& logical) {
                            return GetWindowRect(control, &screen) && SUCCEEDED(mapUiRect(nullptr, rtlApp->window_, screen, &logical));
                        };
                        RECT rtlNavScreen{}, rtlNavLogical{}, rtlBreadcrumbScreen{}, rtlBreadcrumbLogical{};
                        RECT rtlSearchScreen{}, rtlSearchLogical{}, rtlAddressScreen{}, rtlAddressLogical{};
                        const bool rtlHostBounds = rtlBounds(rtlApp->nav_, rtlNavScreen, rtlNavLogical) &&
                            rtlBounds(rtlApp->breadcrumbs_, rtlBreadcrumbScreen, rtlBreadcrumbLogical) &&
                            rtlBounds(rtlApp->search_, rtlSearchScreen, rtlSearchLogical) &&
                            rtlBounds(rtlApp->address_, rtlAddressScreen, rtlAddressLogical);
                        HWND rtlViewWindow = nullptr;
                        RECT rtlViewScreen{}, rtlViewLogical{}, rtlClient{};
                        const bool rtlViewBounds = rtlApp->view_ && SUCCEEDED(rtlApp->view_->GetWindow(&rtlViewWindow)) &&
                            rtlBounds(rtlViewWindow, rtlViewScreen, rtlViewLogical) && GetClientRect(rtlApp->window_, &rtlClient);
                        check("private_rtl_native_physical_geometry_preserves_logical_layout", rtlHostBounds && rtlViewBounds &&
                            rtlNavScreen.left >= rtlBreadcrumbScreen.right && rtlSearchScreen.right < rtlBreadcrumbScreen.left &&
                            rtlNavLogical.left == rtlApp->px(3) && rtlBreadcrumbLogical.left == rtlApp->px(106) &&
                            rtlSearchLogical.left == rtlAddressLogical.right + rtlApp->px(12) &&
                            rtlViewLogical.left >= rtlClient.left && rtlViewLogical.right <= rtlClient.right &&
                            rtlViewLogical.top >= static_cast<LONG>(rtlApp->ribbon_.height()) + rtlApp->px(41) &&
                            rtlViewLogical.bottom <= rtlClient.bottom,
                            L"host bounds=" + std::to_wstring(rtlHostBounds) + L"; native view=" + std::to_wstring(rtlViewBounds));
                        const auto rtlSlot = rtlApp->breadcrumbButtons_.empty() ? size_t{0} : rtlApp->breadcrumbButtons_.size() - 1;
                        const UINT rtlCommand = BreadcrumbFirst + static_cast<UINT>(rtlSlot);
                        RECT rtlButtonLogical{}, rtlButtonScreen{};
                        POINT rtlHit{};
                        const auto rtlButtonIndex = SendMessageW(rtlApp->breadcrumbs_, TB_COMMANDTOINDEX, rtlCommand, 0);
                        bool rtlBreadcrumbHit = !rtlApp->breadcrumbButtons_.empty() && rtlButtonIndex >= 0 &&
                            SendMessageW(rtlApp->breadcrumbs_, TB_GETRECT, rtlCommand, reinterpret_cast<LPARAM>(&rtlButtonLogical)) &&
                            SUCCEEDED(mapUiRect(rtlApp->breadcrumbs_, nullptr, rtlButtonLogical, &rtlButtonScreen));
                        rtlHit = {rtlButtonScreen.left + (rtlButtonScreen.right - rtlButtonScreen.left) / 2,
                            rtlButtonScreen.top + (rtlButtonScreen.bottom - rtlButtonScreen.top) / 2};
                        rtlBreadcrumbHit = rtlBreadcrumbHit && SUCCEEDED(mapUiPoint(nullptr, rtlApp->breadcrumbs_, rtlHit, &rtlHit)) &&
                            SendMessageW(rtlApp->breadcrumbs_, TB_HITTEST, 0, reinterpret_cast<LPARAM>(&rtlHit)) == rtlButtonIndex;
                        ComPtr<IShellItem> rtlBreadcrumbTarget;
                        FILE_ID_INFO rtlBreadcrumbIdentity{};
                        const auto rtlAncestor = rtlApp->breadcrumbAncestor(rtlCommand);
                        const bool rtlBreadcrumbIdentityRead = rtlAncestor &&
                            SUCCEEDED(SHCreateItemFromIDList(rtlApp->breadcrumbsPidls_[*rtlAncestor].get(), IID_PPV_ARGS(&rtlBreadcrumbTarget))) &&
                            SUCCEEDED(nativeFileIdentity(itemName(rtlBreadcrumbTarget.Get(), SIGDN_FILESYSPATH), rtlBreadcrumbIdentity)) &&
                            rtlIdentity(rtlBreadcrumbIdentity) == rtlIdentity(rtlFolderBefore);
                        const auto rtlMargins = SendMessageW(rtlApp->search_, EM_GETMARGINS, 0, 0);
                        if (rtlEditReference.value) {
                            SendMessageW(rtlEditReference.value, WM_SETFONT, reinterpret_cast<WPARAM>(rtlApp->font_), FALSE);
                            SendMessageW(rtlEditReference.value, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                                MAKELPARAM(EC_USEFONTINFO, EC_USEFONTINFO));
                        }
                        const auto rtlFontMargins = SendMessageW(rtlEditReference.value, EM_GETMARGINS, 0, 0);
                        check("private_rtl_native_breadcrumb_hit_preserves_exact_identity_and_leading_margin",
                            rtlBreadcrumbHit && rtlBreadcrumbIdentityRead && rtlEditReferenceValid &&
                            HIWORD(rtlMargins) == rtlApp->px(40) && LOWORD(rtlMargins) == LOWORD(rtlFontMargins),
                            L"hit=" + std::to_wstring(rtlBreadcrumbHit) + L"; identity=" + std::to_wstring(rtlBreadcrumbIdentityRead) +
                            L"; physical right margin=" + std::to_wstring(HIWORD(rtlMargins)) +
                            L"; physical left/native font margin=" + std::to_wstring(LOWORD(rtlMargins)) +
                            L"/" + std::to_wstring(LOWORD(rtlFontMargins)));
                        auto searchChromeProof = [&](ExplorerApp& candidate, bool expectedRtl) {
                            std::wstring diagnostic;
                            const auto desktop = PrivateDesktop::current();
                            DWORD process = 0;
                            bool ownerRtl = !expectedRtl;
                            const bool owned = desktop && SUCCEEDED(desktop->verifyIsolation()) &&
                                GetWindowThreadProcessId(candidate.search_, &process) == GetCurrentThreadId() &&
                                process == GetCurrentProcessId() && IsChild(candidate.window_, candidate.search_) &&
                                SUCCEEDED(windowUiDirection(candidate.window_, &ownerRtl)) && ownerRtl == expectedRtl;
                            PrivatePresentation presentation(owned ? candidate.window_ : nullptr, true);
                            const auto originalText = textOf(candidate.search_);
                            struct RestoreText {
                                std::function<void()> action;
                                bool active = false;
                                ~RestoreText() { if (active) action(); }
                            } restore{};
                            restore.action = [&] { candidate.setSearchText(originalText, false); };
                            Pidl originalLocation(candidate.currentPidl_ ? ILCloneFull(candidate.currentPidl_.get()) : nullptr);
                            const auto originalNavigation = candidate.navigationCount_;
                            const auto originalHistory = candidate.history_.size();
                            const auto originalRecent = candidate.recentSearches_;
                            const auto originalFactories = candidate.searchLocations_.size();
                            const auto originalQuery = candidate.activeQuery_;
                            const auto originalRevision = candidate.searchInteractionRevision_;
                            RECT searchOuter{};
                            const bool outerRead = GetWindowRect(candidate.search_, &searchOuter);
                            struct NativeEdit {
                                HWND value;
                                ~NativeEdit() { if (value) DestroyWindow(value); }
                            } reference{owned && outerRead ? CreateWindowExW(0, L"EDIT", L"",
                                static_cast<DWORD>(GetWindowLongPtrW(candidate.search_,GWL_STYLE)),0,0,1,1,
                                candidate.window_, nullptr, instance_, nullptr) : nullptr};
                            // Read each native initialization stage without
                            // changing the actual control under assertion. The
                            // original mixed request is documented for EDIT;
                            // its readback distinguishes rejection at 1x1 from
                            // a later native frame/size reset on this platform.
                            std::wstring marginLifecycle;
                            auto observeMargins = [&](const wchar_t* stage) {
                                RECT bounds{}, format{};
                                const bool boundsRead = GetClientRect(reference.value, &bounds) != FALSE;
                                SendMessageW(reference.value, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&format));
                                const auto value = SendMessageW(reference.value, EM_GETMARGINS, 0, 0);
                                marginLifecycle += std::wstring(stage) + L"(client=" + std::to_wstring(boundsRead) + L":" +
                                    std::to_wstring(bounds.left) + L"," + std::to_wstring(bounds.top) + L"," +
                                    std::to_wstring(bounds.right) + L"," + std::to_wstring(bounds.bottom) + L"; format=" +
                                    std::to_wstring(format.left) + L"," + std::to_wstring(format.top) + L"," +
                                    std::to_wstring(format.right) + L"," + std::to_wstring(format.bottom) + L"; style=" +
                                    std::to_wstring(GetWindowLongPtrW(reference.value, GWL_STYLE)) + L"; margins=" +
                                    std::to_wstring(LOWORD(value)) + L"/" + std::to_wstring(HIWORD(value)) + L") ";
                            };
                            auto requestLeadingMargin = [&] {
                                if (expectedRtl) SendMessageW(reference.value, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                                    MAKELPARAM(EC_USEFONTINFO, candidate.px(40)));
                                else SendMessageW(reference.value, EM_SETMARGINS, EC_LEFTMARGIN, MAKELPARAM(candidate.px(40), 0));
                            };
                            observeMargins(L"created-1x1");
                            if (reference.value)
                                SendMessageW(reference.value, WM_SETFONT, reinterpret_cast<WPARAM>(candidate.font_), TRUE);
                            observeMargins(L"font-at-1x1");
                            const auto referenceTheme=reference.value?applyWindowTheme(reference.value):E_UNEXPECTED;
                            const auto nativeDefaultMargins = SendMessageW(reference.value, EM_GETMARGINS, 0, 0);
                            observeMargins(L"theme-at-1x1");
                            if (reference.value) {
                                requestLeadingMargin();
                                observeMargins(L"original-request-before-frame");
                                // The native Explorer theme can remove WS_BORDER.
                                // Production restores it in applyChrome before
                                // layout; reproduce that native frame sequence.
                                SetWindowLongPtrW(reference.value, GWL_STYLE,
                                    GetWindowLongPtrW(candidate.search_, GWL_STYLE));
                                SetWindowPos(reference.value, nullptr, 0, 0, 0, 0,
                                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
                                observeMargins(L"frame-at-1x1");
                                SetWindowPos(reference.value,nullptr,0,0,
                                    searchOuter.right-searchOuter.left,searchOuter.bottom-searchOuter.top,SWP_NOZORDER|SWP_NOACTIVATE);
                                observeMargins(L"resized-without-retry");
                                requestLeadingMargin();
                                observeMargins(L"same-request-after-size");
                                // Calculate the opposite font margin at the
                                // actual final native client size.
                                SendMessageW(reference.value, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                                    MAKELPARAM(EC_USEFONTINFO, EC_USEFONTINFO));
                                observeMargins(L"native-font-baseline-after-size");
                            }
                            const auto nativeMargins = SendMessageW(reference.value, EM_GETMARGINS, 0, 0);
                            const auto actualMargins = SendMessageW(candidate.search_, EM_GETMARGINS, 0, 0);
                            const bool margins = reference.value && (expectedRtl ?
                                HIWORD(actualMargins) == candidate.px(40) && LOWORD(actualMargins) == LOWORD(nativeMargins) :
                                LOWORD(actualMargins) == candidate.px(40) && HIWORD(actualMargins) == HIWORD(nativeDefaultMargins));
                            const std::wstring mixed = L"ABC - \u05d0\u05d1\u05d2 - XYZ";
                            if (owned && presentation.ready) {
                                restore.active = true;
                                candidate.setSearchText(mixed, false);
                                SendMessageW(candidate.search_, EM_SETSEL, 0, 0);
                                SetFocus(candidate.search_);
                            }
                            const bool actualFocused = GetFocus() == candidate.search_;
                            RECT client{}, formatting{};
                            const bool clientRead = GetClientRect(candidate.search_, &client) &&
                                client.right > candidate.px(40) && client.bottom > candidate.px(4);
                            SendMessageW(candidate.search_, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&formatting));
                            RECT leading = client;
                            if (expectedRtl) leading.left = leading.right - candidate.px(40);
                            else leading.right = leading.left + candidate.px(40);
                            const bool textBounds = clientRead && formatting.left >= client.left && formatting.right <= client.right &&
                                formatting.right > formatting.left && formatting.bottom > formatting.top &&
                                (expectedRtl ? formatting.right <= leading.left : formatting.left >= leading.right);
                            const auto editStyle = GetWindowLongPtrW(candidate.search_, GWL_EXSTYLE);
                            const auto editClassStyle = GetClassLongPtrW(candidate.search_, GCL_STYLE);
                            const auto editFont = reinterpret_cast<HFONT>(SendMessageW(candidate.search_, WM_GETFONT, 0, 0));
                            LOGFONTW editFontInfo{};
                            const bool fontRead = editFont && GetObjectW(editFont, sizeof(editFontInfo), &editFontInfo) == sizeof(editFontInfo);
                            HDC editDc = GetDC(candidate.search_);
                            const auto editLayout = editDc ? GetLayout(editDc) : GDI_ERROR;
                            const auto editMapMode = editDc ? GetMapMode(editDc) : 0;
                            const auto editTextAlign = editDc ? GetTextAlign(editDc) : GDI_ERROR;
                            if (editDc) ReleaseDC(candidate.search_, editDc);
                            diagnostic += L"owner HWND="+std::to_wstring(reinterpret_cast<UINT_PTR>(candidate.window_))+
                                L"; search HWND="+std::to_wstring(reinterpret_cast<UINT_PTR>(candidate.search_))+
                                L"; owner RTL=" + std::to_wstring(expectedRtl) + L"; edit style="+
                                std::to_wstring(GetWindowLongPtrW(candidate.search_,GWL_STYLE))+L"; edit exStyle=" +
                                std::to_wstring(editStyle) + L"; classStyle=" + std::to_wstring(editClassStyle) +
                                L"; dcLayout=" + std::to_wstring(editLayout) + L"; mapMode=" + std::to_wstring(editMapMode) +
                                L"; textAlign=" + std::to_wstring(editTextAlign) + L"; font=" + std::to_wstring(fontRead) +
                                L"/" + editFontInfo.lfFaceName + L"/" + std::to_wstring(editFontInfo.lfHeight) +
                                L"/" + std::to_wstring(editFontInfo.lfCharSet) + L"; format=" +
                                std::to_wstring(formatting.left) + L"," + std::to_wstring(formatting.top) + L"," +
                                std::to_wstring(formatting.right) + L"," + std::to_wstring(formatting.bottom) + L"; positions=";
                            std::vector<POINT> positions;
                            bool positionsRead = owned && presentation.ready && textOf(candidate.search_) == mixed;
                            for (size_t index = 0; owned && presentation.ready && index < mixed.size(); ++index) {
                                const auto read = SendMessageW(candidate.search_, EM_POSFROMCHAR, index, 0);
                                const POINT point{static_cast<short>(LOWORD(read)), static_cast<short>(HIWORD(read))};
                                const bool within = read != -1 && point.x >= formatting.left && point.x <= formatting.right &&
                                    point.y >= client.top && point.y < client.bottom;
                                positionsRead = within && positionsRead;
                                positions.push_back(point);
                                diagnostic += std::to_wstring(index) + L":" + std::to_wstring(read) + L"(" +
                                    std::to_wstring(point.x) + L"," + std::to_wstring(point.y) + L"; within=" +
                                    std::to_wstring(within) + L") ";
                            }
                            const auto hebrew = mixed.find(L'\u05d0'), lastLatin = mixed.find(L"XYZ");
                            bool sameNativePositions = reference.value && positionsRead && positions.size() == mixed.size();
                            RECT nativeFormatting{};
                            if (reference.value) {
                                SendMessageW(reference.value, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                                    MAKELPARAM(LOWORD(actualMargins), HIWORD(actualMargins)));
                                SetWindowTextW(reference.value, mixed.c_str());
                                SendMessageW(reference.value, EM_SETSEL, 0, 0);
                                ShowWindow(reference.value, SW_SHOWNA); SetFocus(reference.value);
                                sameNativePositions = GetFocus() == reference.value && textOf(reference.value) == mixed && sameNativePositions;
                                SendMessageW(reference.value, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&nativeFormatting));
                                RECT nativeClient{};
                                const bool nativeGeometry=GetClientRect(reference.value,&nativeClient)&&EqualRect(&client,&nativeClient)&&
                                    EqualRect(&formatting,&nativeFormatting);
                                sameNativePositions = SUCCEEDED(referenceTheme)&&nativeGeometry&&
                                    GetWindowLongPtrW(reference.value,GWL_STYLE)==GetWindowLongPtrW(candidate.search_,GWL_STYLE)&&
                                    GetWindowLongPtrW(reference.value,GWL_EXSTYLE)==editStyle&&sameNativePositions;
                                diagnostic+=L"; actual focused="+std::to_wstring(actualFocused)+L"; reference focus="+
                                    std::to_wstring(GetFocus()==reference.value)+L"; reference theme="+hresultMessage(referenceTheme)+
                                    L"; native geometry equal="+std::to_wstring(nativeGeometry)+L"; reference style="+
                                    std::to_wstring(GetWindowLongPtrW(reference.value,GWL_STYLE))+L"; exStyle="+
                                    std::to_wstring(GetWindowLongPtrW(reference.value,GWL_EXSTYLE))+L"; reference client="+
                                    std::to_wstring(nativeClient.left)+L","+std::to_wstring(nativeClient.top)+L","+
                                    std::to_wstring(nativeClient.right)+L","+std::to_wstring(nativeClient.bottom)+L"; reference format="+
                                    std::to_wstring(nativeFormatting.left)+L","+std::to_wstring(nativeFormatting.top)+L","+
                                    std::to_wstring(nativeFormatting.right)+L","+std::to_wstring(nativeFormatting.bottom);
                                diagnostic += L"; native reference positions=";
                                for (size_t index = 0; index < mixed.size(); ++index) {
                                    const auto read = SendMessageW(reference.value, EM_POSFROMCHAR, index, 0);
                                    const POINT point{static_cast<short>(LOWORD(read)), static_cast<short>(HIWORD(read))};
                                    const bool same = read != -1 && index < positions.size() &&
                                        point.x == positions[index].x && point.y == positions[index].y;
                                    sameNativePositions = same && sameNativePositions;
                                    diagnostic += std::to_wstring(index) + L":" + std::to_wstring(read) + L"(" +
                                        std::to_wstring(point.x) + L"," + std::to_wstring(point.y) + L"; same=" + std::to_wstring(same) + L") ";
                                }
                                ShowWindow(reference.value, SW_HIDE);
                            }
                            const bool reading = actualFocused && sameNativePositions && positions[0].x < positions[1].x &&
                                (expectedRtl ? positions[hebrew].x > positions[hebrew + 1].x && positions[0].x > positions[lastLatin].x :
                                    positions[0].x < positions[lastLatin].x);
                            // Remove native caret/focus ink from both captures;
                            // the text itself must account for their difference.
                            if (owned) SetFocus(candidate.nav_);
                            struct NativePrintObservation {
                                struct State { POINT viewport{}, origin{}; RECT clip{}; DWORD layout=GDI_ERROR; int map=0, clipKind=0; };
                                struct Record { UINT message=0; UINT_PTR dc=0; State before{}, after{}; };
                                HWND window=nullptr;
                                bool ready=false;
                                std::array<Record,16> records{};
                                size_t count=0;
                                static State state(HDC dc) {
                                    State value;
                                    GetViewportOrgEx(dc,&value.viewport);GetWindowOrgEx(dc,&value.origin);
                                    value.layout=GetLayout(dc);value.map=GetMapMode(dc);value.clipKind=GetClipBox(dc,&value.clip);
                                    return value;
                                }
                                static LRESULT CALLBACK observe(HWND window,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR,DWORD_PTR data) {
                                    auto& owner=*reinterpret_cast<NativePrintObservation*>(data);
                                    if((message==WM_PRINT||message==WM_PRINTCLIENT)&&wparam) {
                                        Record record;record.message=message;record.dc=static_cast<UINT_PTR>(wparam);
                                        record.before=state(reinterpret_cast<HDC>(wparam));
                                        const auto result=DefSubclassProc(window,message,wparam,lparam);
                                        record.after=state(reinterpret_cast<HDC>(wparam));
                                        if(owner.count<owner.records.size())owner.records[owner.count++]=record;
                                        return result;
                                    }
                                    return DefSubclassProc(window,message,wparam,lparam);
                                }
                                explicit NativePrintObservation(HWND target):window(target) {
                                    ready=SetWindowSubclass(window,observe,0x53505249,reinterpret_cast<DWORD_PTR>(this))!=FALSE;
                                }
                                ~NativePrintObservation(){if(ready)RemoveWindowSubclass(window,observe,0x53505249);}
                            } printObservation(owned ? candidate.search_ : nullptr);
                            const bool painted = owned && presentation.ready && RedrawWindow(candidate.search_, nullptr, nullptr,
                                RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW) && GdiFlush();
                            const auto captureRoot = report.parent_path().empty() ? fixture : std::filesystem::absolute(report).parent_path();
                            const auto captureId = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
                            // Preserve native print evidence even if a reading
                            // assertion fails; final acceptance still requires
                            // that complete character-position contract.
                            const bool captureSafe=painted&&owned&&presentation.ready&&clientRead&&printObservation.ready;
                            // A memory DC does not inherit the native root's
                            // direction. The measured RTL fixture needs its
                            // actual mirrored layout and ordinary PrintWindow;
                            // flags2/DC0 reflected the root and cropped navigation.
                            const DWORD printLayout=expectedRtl?LAYOUT_RTL:0;
                            const int printMap=expectedRtl?MM_ANISOTROPIC:MM_TEXT;
                            bool captured = true;
                            std::array<uint64_t,2> mixedHashes{},shortHashes{};
                            for (const bool shortText : {false,true}) {
                                bool phasePainted=painted;
                                if (shortText && owned) {
                                    candidate.setSearchText(L"XYZ",false);
                                    SendMessageW(candidate.search_,EM_SETSEL,0,0);
                                    phasePainted=RedrawWindow(candidate.search_,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_UPDATENOW)&&GdiFlush();
                                }
                            for (size_t regionIndex=0;regionIndex<2;++regionIndex) {
                                const auto* region=regionIndex==0?L"glyph":L"text";
                                VisualCaptureOptions options;
                                options.includeFrame = false; options.requireVisibleChildren = false;
                                options.nativeClientCropSource = candidate.window_;
                                options.nativeClientCropPrintLayout = printLayout;
                                options.nativeClientCropPrintFlags = 0;
                                options.layoutDpi = candidate.dpi_; options.minimumUniqueColors = 2;
                                options.pixelInspectionBackground = themePalette().address;
                                options.pixelInspectionBounds = wcscmp(region, L"glyph") == 0 ? leading : formatting;
                                // Exclude client edges, so border ink cannot
                                // stand in for either the real icon or text.
                                if (wcscmp(region, L"glyph") == 0)
                                    InflateRect(&options.pixelInspectionBounds, -candidate.px(6), -candidate.px(2));
                                VisualCaptureReport rendered;
                                const auto path = captureRoot / (report.stem().wstring() + L"-search-" +
                                    (expectedRtl ? L"rtl-" : L"ltr-") + region + L"-dc"+std::to_wstring(printLayout)+L"-flags0"+
                                    (shortText?L"-short-":L"-mixed-") + captureId + L".png");
                                options.nativeClientCropSourceImage=path.parent_path()/(path.stem().wstring()+L"-root-source.png");
                                const auto& inspected=options.pixelInspectionBounds;
                                diagnostic+=L"requested=root-PrintWindow-client-crop sourceHWND="+
                                    std::to_wstring(reinterpret_cast<UINT_PTR>(candidate.window_))+L" targetHWND="+
                                    std::to_wstring(reinterpret_cast<UINT_PTR>(candidate.search_))+L" dcLayout="+
                                    std::to_wstring(printLayout)+L" flags=0 ROI="+
                                    std::to_wstring(inspected.left)+L","+std::to_wstring(inspected.top)+L","+
                                    std::to_wstring(inspected.right)+L","+std::to_wstring(inspected.bottom)+L"; ";
                                const std::wstring expectedText=shortText?L"XYZ":mixed;
                                const bool beforeText=textOf(candidate.search_)==expectedText;
                                const auto printed = captureSafe&&phasePainted&&beforeText ? captureWindowPng(*desktop, candidate.search_, path, options, rendered) : E_UNEXPECTED;
                                const bool afterText=textOf(candidate.search_)==expectedText;
                                captured = SUCCEEDED(printed) && rendered.printWindowSucceeded &&
                                    rendered.nativeClientCropped&&rendered.printSourceWindow==reinterpret_cast<UINT_PTR>(candidate.window_)&&
                                    rendered.printTargetWindow==reinterpret_cast<UINT_PTR>(candidate.search_)&&
                                    rendered.printWindowFlags==0&&rendered.printSourceDcLayout==printLayout&&
                                    rendered.printSourceDcMapMode==printMap&&rendered.printMemoryDcBeforeLayout==printLayout&&
                                    rendered.printMemoryDcAfterLayout==printLayout&&rendered.printMemoryDcBeforeMapMode==printMap&&
                                    rendered.printMemoryDcAfterMapMode==printMap&&beforeText&&afterText&&
                                    rendered.width == static_cast<unsigned>(client.right) && rendered.height == static_cast<unsigned>(client.bottom) &&
                                    rendered.inspectionUniqueColors >= 2 && rendered.inspectionInkFraction >= options.minimumInkFraction && captured;
                                (shortText?shortHashes:mixedHashes)[regionIndex]=rendered.inspectionPixelHash;
                                diagnostic += std::wstring(region) + L"=" + hresultMessage(printed) + L"/" +
                                    std::to_wstring(rendered.inspectionUniqueColors) + L" colors/" +
                                    std::to_wstring(rendered.inspectionInkFraction) + L" contrast/flags=" +
                                    std::to_wstring(rendered.printWindowFlags) + L"/hash=" + std::to_wstring(rendered.inspectionPixelHash) +
                                    L"; source DC layout/map="+std::to_wstring(rendered.printSourceDcLayout)+L"/"+
                                    std::to_wstring(rendered.printSourceDcMapMode)+L"; memory DC initial/before/after="+
                                    std::to_wstring(rendered.printMemoryDcInitialLayout)+L"/"+std::to_wstring(rendered.printMemoryDcInitialMapMode)+L","+
                                    std::to_wstring(rendered.printMemoryDcBeforeLayout)+L"/"+std::to_wstring(rendered.printMemoryDcBeforeMapMode)+L","+
                                    std::to_wstring(rendered.printMemoryDcAfterLayout)+L"/"+std::to_wstring(rendered.printMemoryDcAfterMapMode)+
                                    L"; exact native text before/after="+std::to_wstring(beforeText)+L"/"+std::to_wstring(afterText)+
                                    L"; source bounds="+std::to_wstring(rendered.printSourceBounds.left)+L","+
                                    std::to_wstring(rendered.printSourceBounds.top)+L","+std::to_wstring(rendered.printSourceBounds.right)+L","+
                                    std::to_wstring(rendered.printSourceBounds.bottom)+L"; client bounds="+
                                    std::to_wstring(rendered.printTargetClientBounds.left)+L","+std::to_wstring(rendered.printTargetClientBounds.top)+L","+
                                    std::to_wstring(rendered.printTargetClientBounds.right)+L","+std::to_wstring(rendered.printTargetClientBounds.bottom)+
                                    L"; "+path.wstring()+L"; full source="+options.nativeClientCropSourceImage.wstring()+L"; ";
                            }
                            }
                            const bool textChanged=mixedHashes[1]&&shortHashes[1]&&mixedHashes[1]!=shortHashes[1];
                            const bool glyphStable=mixedHashes[0]&&mixedHashes[0]==shortHashes[0];
                            diagnostic+=L"print dispatch=";
                            for(size_t index=0;index<printObservation.count;++index) {
                                const auto& value=printObservation.records[index];
                                diagnostic+=std::to_wstring(value.message)+L"/dc="+std::to_wstring(value.dc);
                                for(const auto& state:{value.before,value.after})
                                    diagnostic+=L"[layout="+std::to_wstring(state.layout)+L"; map="+std::to_wstring(state.map)+
                                        L"; viewport="+std::to_wstring(state.viewport.x)+L","+std::to_wstring(state.viewport.y)+
                                        L"; origin="+std::to_wstring(state.origin.x)+L","+std::to_wstring(state.origin.y)+
                                        L"; clip="+std::to_wstring(state.clipKind)+L":"+std::to_wstring(state.clip.left)+L","+
                                        std::to_wstring(state.clip.top)+L","+std::to_wstring(state.clip.right)+L","+std::to_wstring(state.clip.bottom)+L"]";
                                diagnostic+=L" ";
                            }
                            diagnostic+=L"; native text changed="+std::to_wstring(textChanged)+L"; glyph stable="+std::to_wstring(glyphStable)+L"; ";
                            if (restore.active) {
                                candidate.setSearchText(originalText, false); restore.active = false;
                            }
                            const bool unchanged = textOf(candidate.search_) == originalText && originalLocation && candidate.currentPidl_ &&
                                ILIsEqual(originalLocation.get(), candidate.currentPidl_.get()) &&
                                candidate.navigationCount_ == originalNavigation && candidate.history_.size() == originalHistory &&
                                candidate.recentSearches_ == originalRecent && candidate.searchLocations_.size() == originalFactories &&
                                candidate.activeQuery_ == originalQuery && candidate.searchInteractionRevision_ == originalRevision &&
                                desktop && SUCCEEDED(desktop->verifyIsolation());
                            diagnostic += L"margins L/R=" + std::to_wstring(LOWORD(actualMargins)) + L"/" +
                                std::to_wstring(HIWORD(actualMargins)) + L"; native default L/R=" +
                                std::to_wstring(LOWORD(nativeDefaultMargins)) + L"/" + std::to_wstring(HIWORD(nativeDefaultMargins)) +
                                L"; native calculated font L/R=" +
                                std::to_wstring(LOWORD(nativeMargins)) + L"/" + std::to_wstring(HIWORD(nativeMargins)) +
                                L"; text bounds=" + std::to_wstring(textBounds) + L"; native positions=" + std::to_wstring(reading) +
                                L"; unchanged=" + std::to_wstring(unchanged) + L"; native margin lifecycle=" + marginLifecycle;
                            return std::pair{owned && presentation.ready && captured && margins&&textBounds&&reading&&unchanged&&
                                textChanged&&glyphStable, diagnostic};
                        };
                        const auto ltrSearchProof = searchChromeProof(*this, false);
                        check("private_ltr_search_native_text_and_icon_use_physical_leading_strip", ltrSearchProof.first, ltrSearchProof.second);
                        const auto rtlSearchProof = searchChromeProof(*rtlApp, true);
                        check("private_rtl_search_native_text_and_icon_use_physical_leading_strip", rtlSearchProof.first, rtlSearchProof.second);
                        ComPtr<INameSpaceTreeControl2> rtlTree;
                        auto rtlTreeRead = IUnknown_QueryService(rtlApp->browser_.Get(), SID_SNavigationPane, IID_PPV_ARGS(&rtlTree));
                        if (FAILED(rtlTreeRead)) rtlTreeRead = IUnknown_QueryService(rtlApp->view_.Get(), SID_SNavigationPane, IID_PPV_ARGS(&rtlTree));
                        if (FAILED(rtlTreeRead)) {
                            ComPtr<IObjectWithSite> rtlTreeSite;
                            ComPtr<IServiceProvider> rtlTreeFrame;
                            if (SUCCEEDED(rtlApp->view_.As(&rtlTreeSite)) && SUCCEEDED(rtlTreeSite->GetSite(IID_PPV_ARGS(&rtlTreeFrame))))
                                rtlTreeRead = rtlTreeFrame->QueryService(SID_SNavigationPane, IID_PPV_ARGS(&rtlTree));
                        }
                        const auto rtlTreeServiceRead=rtlTreeRead;
                        ComPtr<IOleWindow> rtlLocatedTree;
                        HWND rtlTreeWindow = nullptr;
                        auto rtlTreeWindowRead=rtlTreeRead;
                        if (SUCCEEDED(rtlTreeWindowRead)) rtlTreeWindowRead = rtlTree.As(&rtlLocatedTree);
                        if (SUCCEEDED(rtlTreeWindowRead)) rtlTreeWindowRead = rtlLocatedTree->GetWindow(&rtlTreeWindow);
                        DWORD rtlTreeProcess = 0;
                        const bool rtlTreeOwned = rtlBreadcrumbIdentityRead && SUCCEEDED(rtlTreeWindowRead) && rtlTreeWindow &&
                            GetWindowThreadProcessId(rtlTreeWindow, &rtlTreeProcess) == GetCurrentThreadId() &&
                            rtlTreeProcess == GetCurrentProcessId() && IsChild(rtlApp->window_, rtlTreeWindow);
                        const auto rtlTreeView=rtlApp->view_;
                        const auto rtlTreeNavigation=rtlApp->navigationCount_;
                        Pidl rtlTreeLocation(rtlApp->currentPidl_?ILCloneFull(rtlApp->currentPidl_.get()):nullptr);
                        const auto rtlTreeStable=[&] {
                            return !rtlApp->closing_&&!rtlApp->navigating_&&rtlApp->view_.Get()==rtlTreeView.Get()&&
                                rtlApp->navigationCount_==rtlTreeNavigation&&rtlTreeLocation&&rtlApp->currentPidl_&&
                                ILIsEqual(rtlTreeLocation.get(),rtlApp->currentPidl_.get());
                        };
                        const bool rtlTreeAncestorsReady=rtlTreeOwned&&pumpUntil([&] {
                            return rtlTreeStable()&&rtlApp->navigationExpansion_.empty();
                        },2500);
                        RECT rtlTreeItemBounds{};
                        const auto rtlTreeInitialBoundsRead=rtlTreeAncestorsReady?
                            rtlTree->GetItemRect(rtlBreadcrumbTarget.Get(),&rtlTreeItemBounds):E_UNEXPECTED;
                        // GetItemRect requires a realized item. Once the
                        // original native ancestor work has completed, ask
                        // that owned tree to make this exact folder visible.
                        // The following reads must establish its actual bounds.
                        const auto rtlTreeEnsureRead=rtlTreeAncestorsReady&&rtlTreeStable()?
                            rtlTree->EnsureItemVisible(rtlBreadcrumbTarget.Get()):E_UNEXPECTED;
                        auto rtlTreeBoundsRead=E_PENDING;
                        const bool rtlTreeReady=SUCCEEDED(rtlTreeEnsureRead)&&pumpUntil([&] {
                            if(!rtlTreeStable())return false;
                            rtlTreeBoundsRead=rtlTree->GetItemRect(rtlBreadcrumbTarget.Get(),&rtlTreeItemBounds);
                            return SUCCEEDED(rtlTreeBoundsRead)&&rtlTreeItemBounds.right>rtlTreeItemBounds.left&&
                                rtlTreeItemBounds.bottom>rtlTreeItemBounds.top&&rtlTreeStable();
                        },2500);
                        POINT rtlTreePoint{rtlTreeItemBounds.left + (rtlTreeItemBounds.right - rtlTreeItemBounds.left) / 2,
                            rtlTreeItemBounds.top + (rtlTreeItemBounds.bottom - rtlTreeItemBounds.top) / 2};
                        const auto rtlTreeMapRead=rtlTreeReady?
                            mapUiPoint(nullptr, rtlTreeWindow, rtlTreePoint, &rtlTreePoint):E_UNEXPECTED;
                        ComPtr<IShellItem> rtlTreeHit;
                        const auto rtlTreeHitRead=SUCCEEDED(rtlTreeMapRead)&&rtlTreeStable()?
                            rtlTree->HitTest(&rtlTreePoint,&rtlTreeHit):E_UNEXPECTED;
                        FILE_ID_INFO rtlTreeHitId{};
                        const bool rtlTreeExactHit = rtlTreeReady && SUCCEEDED(rtlTreeHitRead) && rtlTreeHit &&
                            SUCCEEDED(nativeFileIdentity(itemName(rtlTreeHit.Get(), SIGDN_FILESYSPATH), rtlTreeHitId)) &&
                            rtlIdentity(rtlTreeHitId) == rtlIdentity(rtlFolderBefore)&&rtlTreeStable();
                        auto rtlTreeExpanded = rtlTreeExactHit ? rtlTree->SetItemState(rtlBreadcrumbTarget.Get(), NSTCIS_EXPANDED, NSTCIS_NONE) : E_UNEXPECTED;
                        NSTCITEMSTATE rtlTreeState = NSTCIS_EXPANDED;
                        const bool rtlTreeCollapsed = SUCCEEDED(rtlTreeExpanded) && pumpUntil([&] {
                            rtlTreeExpanded = rtlTree->GetItemState(rtlBreadcrumbTarget.Get(), NSTCIS_EXPANDED, &rtlTreeState);
                            return SUCCEEDED(rtlTreeExpanded) && !(rtlTreeState & NSTCIS_EXPANDED);
                        }, 2000);
                        if (rtlTreeCollapsed) rtlTreeExpanded = expandNativeTreeItem(rtlTree.Get(), rtlBreadcrumbTarget.Get(), rtlApp->window_);
                        const bool rtlTreeExpansionConfirmed = rtlTreeCollapsed && (SUCCEEDED(rtlTreeExpanded) || rtlTreeExpanded == E_PENDING) &&
                            pumpUntil([&] {
                                rtlTreeExpanded = rtlTree->GetItemState(rtlBreadcrumbTarget.Get(), NSTCIS_EXPANDED, &rtlTreeState);
                                return SUCCEEDED(rtlTreeExpanded) && (rtlTreeState & NSTCIS_EXPANDED);
                            }, 3000);
                        check("private_rtl_native_tree_hit_and_expand_preserve_owned_folder_identity", rtlTreeExactHit &&
                            rtlTreeCollapsed && rtlTreeExpansionConfirmed && rtlTreeStable() && rtlMembership(),
                            L"service="+hresultMessage(rtlTreeServiceRead)+L"; window="+hresultMessage(rtlTreeWindowRead)+
                            L"; owned="+std::to_wstring(rtlTreeOwned)+L"; ancestors ready="+std::to_wstring(rtlTreeAncestorsReady)+
                            L"; initial bounds="+hresultMessage(rtlTreeInitialBoundsRead)+L"; ensure="+hresultMessage(rtlTreeEnsureRead)+
                            L"; bounds="+hresultMessage(rtlTreeBoundsRead)+L"/"+std::to_wstring(rtlTreeReady)+
                            L"; map="+hresultMessage(rtlTreeMapRead)+L"; hit="+hresultMessage(rtlTreeHitRead)+
                            L"; expansion chain/index/status="+std::to_wstring(rtlApp->navigationExpansion_.size())+L"/"+
                            std::to_wstring(rtlApp->navigationExpansionIndex_)+L"/"+hresultMessage(rtlApp->navigationExpansionStatus_)+
                            L"; stable="+std::to_wstring(rtlTreeStable())+L"; exact hit=" + std::to_wstring(rtlTreeExactHit) +
                            L"; collapsed=" + std::to_wstring(rtlTreeCollapsed) + L"; expanded=" + std::to_wstring(rtlTreeExpansionConfirmed) +
                            L"/" + hresultMessage(rtlTreeExpanded));
                        const int rtlOriginalSearchWidth = rtlApp->preferences_.searchWidth;
                        bool rtlResized = false;
                        RECT rtlSearchAfterScreen{}, rtlSearchAfterLogical{}, rtlAddressAfterScreen{}, rtlAddressAfterLogical{};
                        if (rtlPresentation.ready && rtlHostBounds) {
                            const int rtlDragX = rtlSearchLogical.left - rtlApp->px(4);
                            const int rtlDragY = (rtlSearchLogical.top + rtlSearchLogical.bottom) / 2;
                            SendMessageW(rtlApp->window_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(rtlDragX, rtlDragY));
                            const bool rtlCaptured = rtlApp->searchResizing_ && GetCapture() == rtlApp->window_;
                            SendMessageW(rtlApp->window_, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(rtlDragX - rtlApp->px(40), rtlDragY));
                            const bool rtlAfterRead = rtlBounds(rtlApp->search_, rtlSearchAfterScreen, rtlSearchAfterLogical) &&
                                rtlBounds(rtlApp->address_, rtlAddressAfterScreen, rtlAddressAfterLogical);
                            SendMessageW(rtlApp->window_, WM_LBUTTONUP, 0, MAKELPARAM(rtlDragX - rtlApp->px(40), rtlDragY));
                            rtlResized = rtlCaptured && rtlAfterRead && !rtlApp->searchResizing_ && GetCapture() != rtlApp->window_ &&
                                rtlApp->preferences_.searchWidth == rtlOriginalSearchWidth + 40 &&
                                rtlSearchAfterScreen.left == rtlSearchScreen.left &&
                                rtlSearchAfterScreen.right == rtlSearchScreen.right + rtlApp->px(40) &&
                                rtlAddressAfterScreen.right - rtlAddressAfterScreen.left == rtlAddressScreen.right - rtlAddressScreen.left - rtlApp->px(40);
                        }
                        rtlApp->preferences_.searchWidth = rtlOriginalSearchWidth; rtlApp->layout();
                        check("private_rtl_owned_splitter_messages_resize_actual_mirrored_fields", rtlResized,
                            L"search physical leading delta=" + std::to_wstring(rtlSearchAfterScreen.right - rtlSearchScreen.right));
                        Pidl rtlFocusLocation(rtlApp->currentPidl_ ? ILCloneFull(rtlApp->currentPidl_.get()) : nullptr);
                        const auto rtlHistoryCount = rtlApp->history_.size();
                        const auto rtlNavigationCount = rtlApp->navigationCount_;
                        const bool rtlSearchFocused = rtlApp->execute(FocusSearch) == S_OK && GetFocus() == rtlApp->search_;
                        const bool rtlAddressFocused = rtlApp->execute(Address) == S_OK && GetFocus() == rtlApp->address_;
                        const std::wstring rtlMixedText = L"ABC - \u05d0\u05d1\u05d2 - XYZ";
                        const bool rtlMixedSet = rtlAddressFocused && SetWindowTextW(rtlApp->address_, rtlMixedText.c_str());
                        SendMessageW(rtlApp->address_, EM_SETSEL, 0, 0);
                        auto rtlCharacterScreen = [&](size_t index, POINT& screen) {
                            const auto position = SendMessageW(rtlApp->address_, EM_POSFROMCHAR, index, 0);
                            if (position == -1) return false;
                            const POINT logical{static_cast<short>(LOWORD(position)), static_cast<short>(HIWORD(position))};
                            return SUCCEEDED(mapUiPoint(rtlApp->address_, nullptr, logical, &screen));
                        };
                        POINT rtlLatinFirst{}, rtlLatinNext{}, rtlLatinLast{}, rtlHebrewFirst{}, rtlHebrewNext{};
                        const auto rtlHebrewIndex = rtlMixedText.find(L'\u05d0');
                        const auto rtlLatinLastIndex = rtlMixedText.find(L"XYZ");
                        const bool rtlReadingPositions = rtlMixedSet && textOf(rtlApp->address_) == rtlMixedText &&
                            rtlCharacterScreen(0, rtlLatinFirst) && rtlCharacterScreen(1, rtlLatinNext) &&
                            rtlCharacterScreen(rtlLatinLastIndex, rtlLatinLast) &&
                            rtlCharacterScreen(rtlHebrewIndex, rtlHebrewFirst) && rtlCharacterScreen(rtlHebrewIndex + 1, rtlHebrewNext);
                        check("private_rtl_native_edit_preserves_rtl_paragraph_and_mixed_run_reading", rtlReadingPositions &&
                            rtlLatinFirst.x < rtlLatinNext.x && rtlHebrewFirst.x > rtlHebrewNext.x && rtlLatinFirst.x > rtlLatinLast.x,
                            L"native positions=" + std::to_wstring(rtlReadingPositions) + L"; Latin run=" +
                            std::to_wstring(rtlLatinNext.x - rtlLatinFirst.x) + L"; Hebrew run=" +
                            std::to_wstring(rtlHebrewNext.x - rtlHebrewFirst.x) + L"; paragraph=" +
                            std::to_wstring(rtlLatinFirst.x - rtlLatinLast.x));
                        const bool rtlRefreshFocused = rtlApp->cycleToolbarFocus(false) == S_OK && GetFocus() == rtlApp->addressActions_;
                        const bool rtlSearchTabFocused = rtlApp->cycleToolbarFocus(false) == S_OK && GetFocus() == rtlApp->search_;
                        check("private_rtl_native_focus_routes_preserve_view_and_history", rtlPresentation.ready && rtlSearchFocused &&
                            rtlAddressFocused && rtlRefreshFocused && rtlSearchTabFocused && rtlFocusLocation &&
                            ILIsEqual(rtlFocusLocation.get(), rtlApp->currentPidl_.get()) && rtlApp->history_.size() == rtlHistoryCount &&
                            rtlApp->navigationCount_ == rtlNavigationCount && rtlMembership());
                        rtlApp->addressEditing_ = false;
                        ShowWindow(rtlApp->address_, SW_HIDE); ShowWindow(rtlApp->breadcrumbs_, SW_SHOWNA);
                        const bool rtlPainted = RedrawWindow(rtlApp->window_, nullptr, nullptr,
                            RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW) && GdiFlush();
                        bool rtlPrinted = rtlPainted;
                        std::wstring rtlPrintDiagnostic;
                        const auto rtlCaptureId = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
                        const auto rtlCaptureRoot = report.parent_path().empty() ? fixture : std::filesystem::absolute(report).parent_path();
                        for (const auto& [control, name] : std::array<std::pair<HWND, const wchar_t*>, 3>{{
                            {rtlApp->nav_, L"navigation"}, {rtlApp->breadcrumbs_, L"breadcrumbs"}, {rtlApp->addressActions_, L"refresh"}}}) {
                            VisualCaptureOptions options;
                            options.includeFrame = false; options.requireVisibleChildren = false;
                            options.layoutDpi = rtlApp->dpi_; options.minimumUniqueColors = 2;
                            VisualCaptureReport captured;
                            const auto imagePath = rtlCaptureRoot / (report.stem().wstring() + L"-rtl-" + name + L"-" + rtlCaptureId + L".png");
                            const auto printed = rtlPainted && PrivateDesktop::current() ?
                                captureWindowPng(*PrivateDesktop::current(), control, imagePath, options, captured) : E_ACCESSDENIED;
                            RECT actualClient{};
                            const bool actualSize = GetClientRect(control, &actualClient) &&
                                captured.width == static_cast<unsigned>(actualClient.right) &&
                                captured.height == static_cast<unsigned>(actualClient.bottom);
                            rtlPrinted = SUCCEEDED(printed) && captured.printWindowSucceeded && actualSize &&
                                captured.uniqueColors >= 2 && captured.inkFraction >= options.minimumInkFraction && rtlPrinted;
                            rtlPrintDiagnostic += std::wstring(name) + L"=" + hresultMessage(printed) + L"/" +
                                std::to_wstring(captured.uniqueColors) + L" colors; " + imagePath.wstring() + L"; ";
                        }
                        // These images come from the real native HWND paint
                        // path and accompany geometry/DC checks. Their pixels
                        // remain reviewable; no synthetic glyph renderer or
                        // claim of pixel parity substitutes for native output.
                        check("private_rtl_native_prints_directional_controls_and_mixed_breadcrumb_text", rtlPrinted, rtlPrintDiagnostic);
                        struct RtlMenuOwner { HMENU value = CreatePopupMenu(); ~RtlMenuOwner() { if (value) DestroyMenu(value); } } rtlMenu;
                        UiPopupPlacement rtlPlacement;
                        RECT rtlAnchorBounds{};
                        const bool rtlMenuReady = rtlMenu.value &&
                            AppendMenuW(rtlMenu.value, MF_STRING, 1, L"\u0627\u0644\u0645\u0644\u0641 \u05ea\u05d9\u05e7\u05d9\u05d4 Latin") &&
                            AppendMenuW(rtlMenu.value, MF_STRING, 2, L"Owned second row") &&
                            GetWindowRect(rtlApp->breadcrumbs_, &rtlAnchorBounds) &&
                            // Retain native loop notifications for readback;
                            // TPM_RETURNCMD still prevents WM_COMMAND dispatch.
                            SUCCEEDED(popupUiPlacement(rtlApp->window_, rtlAnchorBounds, TPM_RETURNCMD, &rtlPlacement));
                        bool rtlMenuObserved = false;
                        std::wstring rtlMenuDiagnostic = L"menu preparation=" + std::to_wstring(rtlMenuReady);
                        if (rtlMenuReady) {
                            PrivateMenuObservation rtlObservation(rtlApp->window_, rtlMenu.value);
                            UINT rtlMenuSelection = 0;
                            if (rtlObservation.ready) rtlMenuSelection = TrackPopupMenuEx(rtlMenu.value, rtlPlacement.flags,
                                rtlPlacement.anchor.x, rtlPlacement.anchor.y, rtlApp->window_, nullptr);
                            rtlMenuObserved = rtlObservation.ready && rtlObservation.entered && rtlObservation.initialized &&
                                rtlObservation.exited && rtlObservation.ownerConfirmed && rtlObservation.cancelled &&
                                rtlObservation.visibleMenus == 1 && rtlMenuSelection == 0 && rtlPlacement.rightToLeft &&
                                !(rtlPlacement.flags & (TPM_RIGHTALIGN | TPM_CENTERALIGN)) && (rtlPlacement.flags & TPM_LAYOUTRTL) &&
                                rtlObservation.bounds.right == rtlPlacement.anchor.x && rtlObservation.bounds.top == rtlPlacement.anchor.y;
                            rtlMenuDiagnostic += L"; loop=" + std::to_wstring(rtlObservation.entered) + L"/" + std::to_wstring(rtlObservation.initialized) +
                                L"/" + std::to_wstring(rtlObservation.exited) + L"; owner=" + std::to_wstring(rtlObservation.ownerConfirmed) +
                                L"; menus=" + std::to_wstring(rtlObservation.visibleMenus) + L"; right/top deltas=" +
                                std::to_wstring(rtlObservation.bounds.right - rtlPlacement.anchor.x) + L"/" +
                                std::to_wstring(rtlObservation.bounds.top - rtlPlacement.anchor.y);
                        }
                        check("private_rtl_native_menu_realizes_at_owned_logical_leading_anchor", rtlMenuObserved, rtlMenuDiagnostic);
                    }
                    const HWND rtlClosedWindow = rtlApp->window_;
                    SendMessageW(rtlClosedWindow, WM_CLOSE, 0, 0);
                    check("private_rtl_app_owned_shutdown_destroys_controls_and_drains_workers",
                        !IsWindow(rtlClosedWindow) && !rtlApp->window_ && SUCCEEDED(rtlApp->shutdownStatus()));
                }
                rtlApp.reset();
                FILE_ID_INFO rtlFolderAfter{}, rtlChildAfter{}, rtlMarkerAfter{};
                std::ifstream rtlMarkerInput(rtlMarker);
                const std::string rtlMarkerText((std::istreambuf_iterator<char>(rtlMarkerInput)), std::istreambuf_iterator<char>());
                bool rtlInputUnchanged = false, rtlInputVisible = true;
                const auto rtlDesktop = PrivateDesktop::current();
                UiDirectionPolicy rtlLanguagePolicyAfter;
                const auto rtlLanguagePolicyReread = loadThreadUiDirection(&rtlLanguagePolicyAfter);
                check("private_rtl_fixture_preserves_sources_language_policy_and_input_desktop", rtlSourcesRead &&
                    SUCCEEDED(nativeFileIdentity(rtlFolder, rtlFolderAfter)) && SUCCEEDED(nativeFileIdentity(rtlChild, rtlChildAfter)) &&
                    SUCCEEDED(nativeFileIdentity(rtlMarker, rtlMarkerAfter)) && rtlIdentity(rtlFolderAfter) == rtlIdentity(rtlFolderBefore) &&
                    rtlIdentity(rtlChildAfter) == rtlIdentity(rtlChildBefore) && rtlIdentity(rtlMarkerAfter) == rtlIdentity(rtlMarkerBefore) &&
                    rtlMarkerText == "private RTL source stays unchanged" && rtlDesktop &&
                    GetThreadUILanguage() == rtlThreadLanguageBefore &&
                    SUCCEEDED(rtlLanguagePolicyRead) && SUCCEEDED(rtlLanguagePolicyReread) &&
                    rtlLanguagePolicyAfter.language == rtlLanguagePolicyBefore.language &&
                    rtlLanguagePolicyAfter.uiLanguages == rtlLanguagePolicyBefore.uiLanguages &&
                    rtlLanguagePolicyAfter.nativeReadingLayout == rtlLanguagePolicyBefore.nativeReadingLayout &&
                    SUCCEEDED(rtlDesktop->verifyIsolation(&rtlInputUnchanged)) && rtlInputUnchanged &&
                    SUCCEEDED(rtlDesktop->visibleWindowsOnInputDesktop(rtlInputVisible)) && !rtlInputVisible && !IsWindowVisible(window_));
            }
            {
                const auto historyDirectory = fixture / L"Clear search history";
                {
                    struct RestorePersistence {
                        PersistenceStatus& status; PersistenceStatus previous;
                        HRESULT& searchSave; HRESULT previousSearch;
                        HRESULT& pending; HRESULT previousPending;
                        Preferences& preferences; Preferences previousPreferences;
                        ~RestorePersistence() {
                            status=previous;searchSave=previousSearch;pending=previousPending;
                            preferences=std::move(previousPreferences);
                        }
                    } restore{persistenceStatus_,persistenceStatus_,searchHistorySaveStatus_,searchHistorySaveStatus_,
                        pendingSearchHistorySaveError_,pendingSearchHistorySaveError_,preferences_,preferences_};
                    // Nondefault retained values distinguish a true no-op from
                    // resetting receipts or clearing an unreported failure.
                    persistenceStatus_={E_ACCESSDENIED,E_ABORT,E_FAIL,E_UNEXPECTED,E_INVALIDARG,E_OUTOFMEMORY,
                        HRESULT_FROM_WIN32(ERROR_DISK_FULL)};
                    searchHistorySaveStatus_=HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION);
                    pendingSearchHistorySaveError_=HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
                    const auto statuses=[&] {
                        return std::tuple{persistenceStatus_.searchHistory,persistenceStatus_.addressHistory,
                            persistenceStatus_.windowPlacement,persistenceStatus_.ribbonState,persistenceStatus_.preferences,
                            persistenceStatus_.ribbonSettings,persistenceStatus_.result,searchHistorySaveStatus_,
                            pendingSearchHistorySaveError_,addressHistoryStatus_};
                    };
                    const auto preferences=[&] {
                        return std::tuple{preferences_.navigationPane,preferences_.previewPane,preferences_.detailsPane,
                            preferences_.expandToCurrent,preferences_.showAllFolders,preferences_.showLibraries,
                            preferences_.showHidden,preferences_.showExtensions,preferences_.ribbonCollapsed,preferences_.view,
                            preferences_.windowWidth,preferences_.windowHeight,preferences_.searchWidth,
                            preferences_.useWindowsStartup,preferences_.startupLocation,preferences_.previewWidth};
                    };
                    const auto beforeStatuses=statuses();const auto beforePreferences=preferences();
                    const auto beforeError=lastError_;
                    const auto skipped=persist();
                    check("headless_persist_skips_all_writers_and_preserves_retained_state",skipped==S_FALSE&&
                        statuses()==beforeStatuses&&preferences()==beforePreferences&&lastError_==beforeError);
                }
                std::filesystem::create_directory(historyDirectory);
                const auto ownedHistory = historyDirectory / L"search-history.dat";
                const auto historyBeforeClear = recentSearches_;
                const auto displayedBeforeClear = ribbonItems(RecentSearches);
                const auto viewBeforeClear = view_;
                const auto navigationBeforeClear = navigationCount_;
                const auto queryBeforeClear = activeQuery_;
                const auto nativeHistoryBeforeClear = history_.size();
                struct HistorySuggestionRestore {
                    ComPtr<SearchSuggestionList>& slot;
                    ComPtr<SearchSuggestionList> previous;
                    ~HistorySuggestionRestore() { slot = std::move(previous); }
                } historySuggestionRestore{searchSuggestions_, searchSuggestions_};
                // Normal hosts construct this actual IEnumString source; a
                // hidden host deliberately never attaches autocomplete UI.
                // Exercise the same source with accepted owned-query history.
                ComPtr<SearchSuggestionList> historySuggestionFixture;
                historySuggestionFixture.Attach(new SearchSuggestionList);
                const auto historySuggestionSetup = historySuggestionFixture->replace(historyBeforeClear);
                if (SUCCEEDED(historySuggestionSetup)) searchSuggestions_ = historySuggestionFixture;
                const auto sourceBeforeClear = searchSuggestions_;
                const auto readSuggestions = [](SearchSuggestionList* source, std::vector<std::wstring>& values) {
                    if (!source) return E_POINTER;
                    ComPtr<IEnumString> snapshot;
                    auto read = source->Clone(&snapshot);
                    if (SUCCEEDED(read)) read = snapshot->Reset();
                    for (size_t index = 0; SUCCEEDED(read) && index <= maximumRecentSearches; ++index) {
                        PWSTR raw = nullptr;
                        read = snapshot->Next(1, &raw, nullptr);
                        std::unique_ptr<WCHAR, decltype(&CoTaskMemFree)> text(raw, CoTaskMemFree);
                        if (read == S_FALSE) return raw ? E_UNEXPECTED : S_OK;
                        if (read != S_OK || !raw) return FAILED(read) ? read : E_UNEXPECTED;
                        values.emplace_back(raw);
                    }
                    return FAILED(read) ? read : E_UNEXPECTED;
                };
                std::vector<std::wstring> suggestionsBeforeClear;
                const auto suggestionsRead = readSuggestions(sourceBeforeClear.Get(), suggestionsBeforeClear);
                const auto originalHistorySave = saveSearchHistory(ownedHistory, historyBeforeClear);
                const auto directoryClear = clearSearchHistory(&historyDirectory);
                std::vector<std::wstring> suggestionsAfterFailure, persistedAfterFailure;
                const auto failedSuggestionsRead = readSuggestions(sourceBeforeClear.Get(), suggestionsAfterFailure);
                const auto failedHistoryRead = loadSearchHistory(ownedHistory, &persistedAfterFailure);
                const auto displayedAfterFailure = displayedRecentSearches_;
                bool displayedPreserved = displayedBeforeClear.size() == displayedAfterFailure.size();
                for (size_t index = 0; displayedPreserved && index < displayedBeforeClear.size(); ++index)
                    displayedPreserved = displayedBeforeClear[index].label == displayedAfterFailure[index];
                const bool failedNativeStatePreserved = view_.Get() == viewBeforeClear.Get() &&
                    navigationCount_ == navigationBeforeClear && activeQuery_ == queryBeforeClear && history_.size() == nativeHistoryBeforeClear;
                const auto failureClearDetail = L"save=" + hresultMessage(originalHistorySave) + L"; clear=" + hresultMessage(directoryClear) +
                    L"; sourceSetup=" + hresultMessage(historySuggestionSetup) + L"; source=" + std::to_wstring(sourceBeforeClear != nullptr) +
                    L"; beforeSource=" + hresultMessage(suggestionsRead) + L"; afterSource=" + hresultMessage(failedSuggestionsRead) +
                    L"; codec=" + hresultMessage(failedHistoryRead) + L"; MRU before/after=" + std::to_wstring(historyBeforeClear.size()) +
                    L"/" + std::to_wstring(recentSearches_.size()) + L"; suggestions before/after=" + std::to_wstring(suggestionsBeforeClear.size()) +
                    L"/" + std::to_wstring(suggestionsAfterFailure.size()) + L"; codecCount=" + std::to_wstring(persistedAfterFailure.size()) +
                    L"; sourceExact=" + std::to_wstring(suggestionsBeforeClear == historyBeforeClear && suggestionsAfterFailure == suggestionsBeforeClear) +
                    L"; codecExact=" + std::to_wstring(persistedAfterFailure == historyBeforeClear) +
                    L"; displayedExact=" + std::to_wstring(displayedPreserved) + L"; nativeState=" + std::to_wstring(failedNativeStatePreserved);
                check("clear_search_history_failed_owned_save_preserves_mru_suggestions_and_displayed_snapshot",
                    !historyBeforeClear.empty() && SUCCEEDED(originalHistorySave) && FAILED(directoryClear) &&
                    SUCCEEDED(historySuggestionSetup) &&
                    SUCCEEDED(suggestionsRead) && SUCCEEDED(failedSuggestionsRead) && suggestionsBeforeClear == historyBeforeClear &&
                    suggestionsAfterFailure == suggestionsBeforeClear && recentSearches_ == historyBeforeClear &&
                    SUCCEEDED(failedHistoryRead) && persistedAfterFailure == historyBeforeClear && displayedPreserved &&
                    failedNativeStatePreserved, failureClearDetail);
                const auto relativeHistory = std::filesystem::path(L"relative-history.dat");
                const auto invalidClear = clearSearchHistory(&relativeHistory);
                check("clear_search_history_rejects_relative_override_without_state_publication",
                    invalidClear == E_INVALIDARG && recentSearches_ == historyBeforeClear && displayedRecentSearches_ == displayedAfterFailure);
                const auto ownedClear = clearSearchHistory(&ownedHistory);
                std::vector<std::wstring> suggestionsAfterClear, persistedAfterClear{L"unchanged"};
                const auto clearedSuggestionsRead = readSuggestions(sourceBeforeClear.Get(), suggestionsAfterClear);
                const auto clearedHistoryRead = loadSearchHistory(ownedHistory, &persistedAfterClear);
                const auto clearedRows = ribbonItems(RecentSearches);
                const auto clearDesktop = PrivateDesktop::current();
                const bool clearedNativeStatePreserved = view_.Get() == viewBeforeClear.Get() &&
                    navigationCount_ == navigationBeforeClear && activeQuery_ == queryBeforeClear && history_.size() == nativeHistoryBeforeClear;
                const bool clearRowsDisabled = !ribbonState(RecentSearches).enabled && !ribbonState(RibbonClearSearchHistory).enabled;
                const bool clearIsolated = clearDesktop && SUCCEEDED(clearDesktop->verifyIsolation()) && !IsWindowVisible(window_);
                const auto successClearDetail = L"clear=" + hresultMessage(ownedClear) + L"; source=" + hresultMessage(clearedSuggestionsRead) +
                    L"; codec=" + hresultMessage(clearedHistoryRead) + L"; MRU/source/codec/rows=" + std::to_wstring(recentSearches_.size()) +
                    L"/" + std::to_wstring(suggestionsAfterClear.size()) + L"/" + std::to_wstring(persistedAfterClear.size()) +
                    L"/" + std::to_wstring(clearedRows.size()) + L"; rowsDisabled=" + std::to_wstring(clearRowsDisabled) +
                    L"; sameSource=" + std::to_wstring(sourceBeforeClear.Get() == searchSuggestions_.Get()) +
                    L"; nativeState=" + std::to_wstring(clearedNativeStatePreserved) + L"; private=" + std::to_wstring(clearIsolated);
                check("clear_search_history_owned_success_publishes_empty_codec_mru_and_suggestions",
                    SUCCEEDED(ownedClear) && SUCCEEDED(clearedSuggestionsRead) && suggestionsAfterClear.empty() &&
                    SUCCEEDED(clearedHistoryRead) && persistedAfterClear.empty() && recentSearches_.empty() && clearedRows.empty() &&
                    clearRowsDisabled && sourceBeforeClear.Get() == searchSuggestions_.Get() && clearedNativeStatePreserved &&
                    clearIsolated, successClearDetail);
            }
            hr = execute(ThisPC);
            check("this_pc_namespace", SUCCEEDED(hr) && pumpUntil([&] { return !navigating_ && currentLocation_.find(L"20D04FE0") != std::wstring::npos; }, 5000), currentLocation_);
            fullPathTitle_ = true;
            updateFrameTitle();
            ComPtr<IShellItem> captionFolder;
            const auto captionFolderRead = currentFolder(captionFolder);
            const auto nativeVirtualCaption = itemName(captionFolder.Get(), SIGDN_NORMALDISPLAY);
            check("full_path_title_virtual_namespace", SUCCEEDED(captionFolderRead) && !physicalDirectory_ &&
                !nativeVirtualCaption.empty() && textOf(window_) == nativeVirtualCaption,
                L"Native virtual namespace caption retains its normal display name");
            fullPathTitle_ = originalFullPathTitle;
            updateFrameTitle();
        }
        }
        check("no_visible_host_after_tests", !IsWindowVisible(window_));
        check("no_visible_input_desktop_process_windows_during_pump", !visibleWindowObserved);
        check("headless_mode_blocks_interactive_operations", execute(Delete) == E_ACCESSDENIED && execute(FolderOptions) == E_ACCESSDENIED &&
              execute(Extensions) == E_ACCESSDENIED && execute(HideSelected) == E_ACCESSDENIED && execute(SaveSearch) == E_ACCESSDENIED &&
              execute(NewItems) == E_ACCESSDENIED && execute(Sharing) == E_ACCESSDENIED &&
              execute(NewLibrary) == E_ACCESSDENIED && execute(IncludeLibraryFolder) == E_ACCESSDENIED);
    } catch (const std::exception& error) {
        const std::string detail = error.what();
        check("unexpected_exception", false, std::wstring(detail.begin(), detail.end()));
    }
    destroyBrowser();
    if (!libraryOnly || libraryFixtureOwned) std::filesystem::remove_all(fixture, filesystemError);
    check("fixture_cleanup", (!libraryOnly || libraryFixtureOwned) && !filesystemError,
        libraryOnly && !libraryFixtureOwned ? L"Unowned Library fixture path was not removed" : L"");
    auto failed = std::count_if(checks.begin(), checks.end(), [](const Check& value) { return !value.passed; });
    if (!report.parent_path().empty()) std::filesystem::create_directories(report.parent_path(), filesystemError);
    std::ofstream output(report);
    if (!output) return 9;
    const auto desktop = PrivateDesktop::current();
    bool inputUnchanged = false, inputVisible = true;
    const bool isolated = desktop && SUCCEEDED(desktop->verifyIsolation(&inputUnchanged)) &&
        SUCCEEDED(desktop->visibleWindowsOnInputDesktop(inputVisible));
    const auto nativeFeatures = ribbon_.features();
    output << "{\n  \"headless\": true,\n  \"smokeScope\": " << jsonString(libraryOnly ? L"Library" : L"General")
           << ",\n  \"privateDesktop\": " << (isolated ? "true" : "false")
           << ",\n  \"ribbonLayout\": " << jsonString(ribbon_.layout() == RibbonLayout::InstalledWindows10 ? L"InstalledWindows10" : L"Authored")
           << ",\n  \"installedRibbonStatus\": " << static_cast<long>(ribbon_.installedLayoutStatus())
           << ",\n  \"installedFeatures\": {\"bitLocker\":" << (nativeFeatures.bitLocker ? "true" : "false")
           << ",\"editionHresult\":" << static_cast<long>(nativeFeatures.editionStatus)
           << ",\"mediaFoundation\":" << (nativeFeatures.mediaFoundation ? "true" : "false")
           << ",\"mediaFoundationHresult\":" << static_cast<long>(nativeFeatures.mediaFoundationStatus)
           << ",\"discBurning\":" << (nativeFeatures.discBurning ? "true" : "false")
           << ",\"discBurningHresult\":" << static_cast<long>(nativeFeatures.discBurningStatus)
           << ",\"diskCleanup\":" << (nativeFeatures.diskCleanup ? "true" : "false")
           << ",\"diskCleanupHresult\":" << static_cast<long>(nativeFeatures.diskCleanupStatus) << "}"
           << ",\n  \"nativeRibbonContexts\":[";
    const std::array<RibbonContext, 11> reportContexts{RibbonContext::Picture, RibbonContext::Drive,
        RibbonContext::Compressed, RibbonContext::Search, RibbonContext::Library, RibbonContext::Recycle,
        RibbonContext::Application, RibbonContext::Music, RibbonContext::Video, RibbonContext::DiscImage,
        RibbonContext::Shortcut};
    for (size_t i = 0; i < reportContexts.size(); ++i) {
        UINT identifier = 0, availability = UI_CONTEXTAVAILABILITY_NOTAVAILABLE;
        const auto read = ribbon_.contextAvailable(reportContexts[i], identifier, availability);
        output << (i ? "," : "") << "{\"logicalContext\":" << static_cast<UINT>(reportContexts[i])
            << ",\"nativeIdentifier\":" << identifier << ",\"availability\":" << availability
            << ",\"readHresult\":" << static_cast<long>(read) << '}';
    }
    output << "]"
           << ",\n  \"inputDesktopUnchanged\": " << (isolated && inputUnchanged ? "true" : "false")
           << ",\n  \"visibleInputDesktopWindows\": " << (inputVisible ? "true" : "false")
           << ",\n  \"passed\": " << (failed == 0 ? "true" : "false")
           << ",\n  \"checks\": " << checks.size() << ",\n  \"failed\": " << failed
           << ",\n  \"elapsed_ms\": " << GetTickCount64() - started << ",\n  \"results\": [\n";
    for (size_t i = 0; i < checks.size(); ++i) {
        const auto& result = checks[i];
        output << "    {\"name\": \"" << result.name << "\", \"passed\": " << (result.passed ? "true" : "false")
               << ", \"detail\": " << jsonString(result.detail) << ", \"milliseconds\": " << result.milliseconds << "}";
        output << (i + 1 < checks.size() ? ",\n" : "\n");
    }
    output << "  ]\n}\n";
    return failed == 0 ? 0 : 1;
}
}
