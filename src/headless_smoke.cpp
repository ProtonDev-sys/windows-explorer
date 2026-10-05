#include "explorer/app.hpp"
#include "explorer/saved_search.hpp"
#include "explorer/search.hpp"
#include "explorer/ribbon_commands.hpp"
#include <propkey.h>
#include <shlwapi.h>
#include <propvarutil.h>
#include <commctrl.h>
#include <UIRibbonPropertyHelpers.h>
#include <uiautomation.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <set>
#include <sstream>

namespace explorer {
namespace {
bool visibleWindowObserved = false;
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
        MsgWaitForMultipleObjectsEx(0, nullptr, 15, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
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
                                       HWND host, const wchar_t* name, int minimumRows = 1) {
    AccessibleResult result;
    if (!automation || !ribbon) { result.detail = L"Native Ribbon accessibility scope unavailable"; return result; }
    VARIANT property{}; property.vt = VT_BSTR; property.bstrVal = SysAllocString(name);
    ComPtr<IUIAutomationCondition> condition;
    auto hr = property.bstrVal ? automation->CreatePropertyCondition(UIA_NamePropertyId, property, &condition) : E_OUTOFMEMORY;
    VariantClear(&property);
    ComPtr<IUIAutomationElementArray> candidates;
    if (SUCCEEDED(hr)) hr = ribbon->FindAll(TreeScope_Descendants, condition.Get(), &candidates);
    int count = 0;
    if (SUCCEEDED(hr)) hr = candidates ? candidates->get_Length(&count) : E_UNEXPECTED;
    ComPtr<IUIAutomationExpandCollapsePattern> expand;
    ComPtr<IUIAutomationInvokePattern> invokeParent;
    const bool safeParentInvoke = wcscmp(name, L"Sort by") == 0 || wcscmp(name, L"Group by") == 0 ||
        wcscmp(name, L"Add columns") == 0 || wcscmp(name, L"Copy to") == 0;
    std::wstring diagnostics;
    for (int index = 0; SUCCEEDED(hr) && index < std::min(count, 16); ++index) {
        ComPtr<IUIAutomationElement> candidate;
        if (FAILED(candidates->GetElement(index, &candidate)) || !candidate) continue;
        BOOL enabled = FALSE;
        if (FAILED(candidate->get_CurrentIsEnabled(&enabled)) || !enabled) continue;
        CONTROLTYPEID control = 0; BSTR id = nullptr, className = nullptr;
        candidate->get_CurrentControlType(&control); candidate->get_CurrentAutomationId(&id);
        candidate->get_CurrentClassName(&className);
        diagnostics += L"; candidate type=" + std::to_wstring(control) + L" id=" + (id ? id : L"") +
            L" class=" + (className ? className : L"");
        SysFreeString(id); SysFreeString(className);
        if (control != UIA_ButtonControlTypeId && control != UIA_SplitButtonControlTypeId &&
            control != UIA_MenuItemControlTypeId && control != UIA_ComboBoxControlTypeId) continue;
        if (wcscmp(name, L"Open") == 0 && control == UIA_SplitButtonControlTypeId) {
            // Windows exposes a split button's primary action and its arrow
            // as two same-name SplitButton siblings. The primary provider's
            // Expand can dispatch the default action. Require the narrow,
            // right-aligned arrow inside its actual same-name native Group.
            ComPtr<IUIAutomationTreeWalker> walker;
            ComPtr<IUIAutomationElement> parent;
            RECT bounds{}, parentBounds{}; CONTROLTYPEID parentType = 0;
            BSTR parentName = nullptr;
            const auto arrowRead = candidate->get_CurrentBoundingRectangle(&bounds);
            auto parentRead = automation->get_ControlViewWalker(&walker);
            if (SUCCEEDED(parentRead)) parentRead = walker ? walker->GetParentElement(candidate.Get(), &parent) : E_UNEXPECTED;
            if (SUCCEEDED(parentRead)) parentRead = parent ? parent->get_CurrentBoundingRectangle(&parentBounds) : E_UNEXPECTED;
            if (SUCCEEDED(parentRead)) parentRead = parent->get_CurrentControlType(&parentType);
            if (SUCCEEDED(parentRead)) parentRead = parent->get_CurrentName(&parentName);
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
        if (FAILED(candidate->GetCurrentPatternAs(UIA_ExpandCollapsePatternId, IID_PPV_ARGS(&pattern))) || !pattern) {
            if (safeParentInvoke) {
                ComPtr<IUIAutomationInvokePattern> parent;
                if (SUCCEEDED(candidate->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&parent))) && parent) {
                    result.element = candidate; invokeParent = parent; break;
                }
            }
            continue;
        }
        ExpandCollapseState state = ExpandCollapseState_LeafNode;
        if (SUCCEEDED(pattern->get_CurrentExpandCollapseState(&state)) && state != ExpandCollapseState_LeafNode) {
            result.element = candidate; expand = pattern; break;
        }
    }
    if (!expand && !invokeParent) {
        result.detail = std::wstring(L"Native enabled expandable control=") + name + L" not found; candidates=" + std::to_wstring(count) +
            L"; " + hresultMessage(hr) + diagnostics;
        return result;
    }
    // Expand/collapse only the native control on its owner's private desktop.
    // A small whitelist permits Invoke only for compiled dropdown parents
    // whose sole action opens a menu. Never Invoke Open's default action or a
    // menu leaf; those dispatch native filesystem/Shell commands.
    hr = expand ? expand->Expand() : invokeParent->Invoke();
    ExpandCollapseState expanded = ExpandCollapseState_LeafNode;
    if (SUCCEEDED(hr) && expand) hr = expand->get_CurrentExpandCollapseState(&expanded);
    VARIANT type{}; type.vt = VT_I4; type.lVal = UIA_MenuItemControlTypeId;
    ComPtr<IUIAutomationCondition> menus, lists, choices;
    if (SUCCEEDED(hr)) hr = automation->CreatePropertyCondition(UIA_ControlTypePropertyId, type, &menus);
    type.lVal = UIA_ListItemControlTypeId;
    if (SUCCEEDED(hr)) hr = automation->CreatePropertyCondition(UIA_ControlTypePropertyId, type, &lists);
    if (SUCCEEDED(hr)) hr = automation->CreateOrCondition(menus.Get(), lists.Get(), &choices);
    ComPtr<IUIAutomationElementArray> children;
    if (SUCCEEDED(hr)) hr = ribbon->FindAll(TreeScope_Descendants, choices.Get(), &children);
    int childCount = 0, visibleChildren = 0;
    if (SUCCEEDED(hr)) hr = children ? children->get_Length(&childCount) : E_UNEXPECTED;
    const auto end = GetTickCount64() + 2000;
    for (int index = 0; SUCCEEDED(hr) && index < std::min(childCount, 128) && GetTickCount64() < end; ++index) {
        ComPtr<IUIAutomationElement> child;
        BOOL offscreen = TRUE; RECT bounds{};
        if (SUCCEEDED(children->GetElement(index, &child)) && child &&
            SUCCEEDED(child->get_CurrentIsOffscreen(&offscreen)) && !offscreen &&
            SUCCEEDED(child->get_CurrentBoundingRectangle(&bounds)) && bounds.right > bounds.left && bounds.bottom > bounds.top)
            ++visibleChildren;
    }
    // Ribbon dropdowns are separate owned top-level HWNDs, not necessarily
    // accessibility descendants of their anchor. Inspect only such private
    // popups, never the desktop root or another process's windows.
    struct PopupEnumeration { HWND host; std::vector<HWND> windows; } popups{host, {}};
    const auto collectPopup = [](HWND popup, LPARAM context) -> BOOL {
        auto& result = *reinterpret_cast<PopupEnumeration*>(context);
        DWORD process = 0; GetWindowThreadProcessId(popup, &process);
        if (process == GetCurrentProcessId() && popup != result.host && IsWindowVisible(popup) &&
            GetAncestor(popup, GA_ROOTOWNER) == result.host) result.windows.push_back(popup);
        return TRUE;
    };
    const auto popupDeadline = GetTickCount64() + 2000;
    do {
        popups.windows.clear();
        EnumThreadWindows(GetWindowThreadProcessId(host, nullptr), collectPopup, reinterpret_cast<LPARAM>(&popups));
        if (!popups.windows.empty()) break;
        Sleep(5); // The owner's STA is pumped by probeNativeMenus, not this worker.
    } while (SUCCEEDED(hr) && GetTickCount64() < popupDeadline);
    std::wstring visibleStaticNames;
    const auto anchorVisibleChildren = visibleChildren;
    const auto rowsDeadline = GetTickCount64() + 2000;
    do {
        visibleChildren = anchorVisibleChildren;
        visibleStaticNames.clear();
        for (const auto popup : popups.windows) {
            ComPtr<IUIAutomationElement> popupRoot;
            if (FAILED(automation->ElementFromHandle(popup, &popupRoot)) || !popupRoot) continue;
            // Native command galleries expose action rows as Buttons as well as
            // MenuItems/ListItems; the separate popup ownership is essential.
            type.lVal = UIA_ButtonControlTypeId;
            ComPtr<IUIAutomationCondition> buttons, popupChoices;
            if (FAILED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, type, &buttons)) ||
                FAILED(automation->CreateOrCondition(choices.Get(), buttons.Get(), &popupChoices))) continue;
            ComPtr<IUIAutomationElementArray> rows;
            if (FAILED(popupRoot->FindAll(TreeScope_Descendants, popupChoices.Get(), &rows)) || !rows) continue;
            int rowsCount = 0; rows->get_Length(&rowsCount);
            for (int index = 0; index < std::min(rowsCount, 128); ++index) {
                ComPtr<IUIAutomationElement> row; BOOL offscreen = TRUE; RECT bounds{};
                if (FAILED(rows->GetElement(index, &row)) || !row || FAILED(row->get_CurrentIsOffscreen(&offscreen)) ||
                    offscreen || FAILED(row->get_CurrentBoundingRectangle(&bounds)) ||
                    bounds.right <= bounds.left || bounds.bottom <= bounds.top) continue;
                ++visibleChildren;
                BSTR label = nullptr;
                if (SUCCEEDED(row->get_CurrentName(&label)) && label) {
                    for (const auto* allowed : {L"Name", L"Date modified", L"Type", L"Size", L"Ascending", L"Descending"})
                        if (wcscmp(label, allowed) == 0) visibleStaticNames += std::wstring(L"[") + allowed + L"]";
                }
                SysFreeString(label);
            }
        }
        if (visibleChildren >= minimumRows) break;
        Sleep(5); // Popup HWND creation can precede its native accessibility rows.
    } while (SUCCEEDED(hr) && GetTickCount64() < rowsDeadline);
    if (visibleChildren < minimumRows) {
        // Record only provider structure and numeric geometry. Native MRU or
        // application names are deliberately absent from these diagnostics.
        ComPtr<IUIAutomationCondition> everything;
        automation->CreateTrueCondition(&everything);
        std::vector<HWND> diagnosticWindows{host};
        diagnosticWindows.insert(diagnosticWindows.end(), popups.windows.begin(), popups.windows.end());
        for (const auto popup : diagnosticWindows) {
            wchar_t className[80]{}; RECT popupBounds{};
            GetClassNameW(popup, className, static_cast<int>(std::size(className)));
            GetWindowRect(popup, &popupBounds);
            ComPtr<IUIAutomationElement> scope;
            const auto scopeRead = automation->ElementFromHandle(popup, &scope);
            ComPtr<IUIAutomationElementArray> descendants;
            const auto descendantsRead = scope && everything ?
                scope->FindAll(TreeScope_Descendants, everything.Get(), &descendants) : scopeRead;
            int length = 0;
            if (descendants) descendants->get_Length(&length);
            diagnostics += std::wstring(L"; popup structure class=") + className + L" bounds=[" +
                std::to_wstring(popupBounds.left) + L"," + std::to_wstring(popupBounds.top) + L"," +
                std::to_wstring(popupBounds.right) + L"," + std::to_wstring(popupBounds.bottom) +
                L"]/read=" + hresultMessage(descendantsRead) + L"/descendants=" + std::to_wstring(length);
            // Inspect native popup rows fully; the host inventory is bounded
            // to chrome and never records a Shell item's accessible name.
            for (int index = 0; descendants && index < std::min(length, popup == host ? 12 : 32); ++index) {
                ComPtr<IUIAutomationElement> row; CONTROLTYPEID rowType = 0;
                BOOL offscreen = TRUE; RECT bounds{};
                if (FAILED(descendants->GetElement(index, &row)) || !row) continue;
                row->get_CurrentControlType(&rowType); row->get_CurrentIsOffscreen(&offscreen);
                row->get_CurrentBoundingRectangle(&bounds);
                diagnostics += L" {" + std::to_wstring(rowType) + L"/off=" + std::to_wstring(offscreen) +
                    L"/rect=" + std::to_wstring(bounds.left) + L"," + std::to_wstring(bounds.top) + L"," +
                    std::to_wstring(bounds.right) + L"," + std::to_wstring(bounds.bottom) + L"}";
            }
        }
    }
    // Expansion posts native popup work. Read state after those real rows
    // materialize, instead of racing the immediate Expand return.
    if (SUCCEEDED(hr) && expand) hr = expand->get_CurrentExpandCollapseState(&expanded);
    const auto collapse = expand ? expand->Collapse() : invokeParent->Invoke();
    ExpandCollapseState collapsed = ExpandCollapseState_LeafNode;
    auto collapsedRead = expand ? expand->get_CurrentExpandCollapseState(&collapsed) : S_OK;
    for (const auto popup : popups.windows) if (IsWindow(popup) && IsWindowVisible(popup)) PostMessageW(popup, WM_CANCELMODE, 0, 0);
    const auto closeDeadline = GetTickCount64() + 500;
    bool popupsClosed = false;
    do {
        popupsClosed = std::none_of(popups.windows.begin(), popups.windows.end(), [](HWND popup) {
            return IsWindow(popup) && IsWindowVisible(popup);
        });
        if (!popupsClosed) Sleep(5);
    } while (!popupsClosed && GetTickCount64() < closeDeadline);
    // Native Collapse also posts its final accessibility state change. Read
    // it after the owned popup has actually disappeared.
    if (expand && popupsClosed) collapsedRead = expand->get_CurrentExpandCollapseState(&collapsed);
    result.passed = SUCCEEDED(hr) && (invokeParent || expanded == ExpandCollapseState_Expanded) && visibleChildren >= minimumRows &&
        SUCCEEDED(collapse) && SUCCEEDED(collapsedRead) && popupsClosed &&
        (invokeParent || collapsed == ExpandCollapseState_Collapsed);
    result.detail = std::wstring(L"Native dropdown=") + name + L"; expanded=" + std::to_wstring(expanded) +
        L"; visible menu/list items=" + std::to_wstring(visibleChildren) + L"; read=" + hresultMessage(hr) +
        L"; collapse=" + hresultMessage(collapse) + L"; collapsedState=" + std::to_wstring(collapsed) +
        L"; collapsedRead=" + hresultMessage(collapsedRead) + L"; owned popup count=" + std::to_wstring(popups.windows.size()) +
        L"; popups closed=" + std::to_wstring(popupsClosed ? 1 : 0) +
        L"; static row names=" + visibleStaticNames + L"; leaf commands invoked=0" + diagnostics;
    return result;
}

}

int ExplorerApp::headlessSmoke(const std::filesystem::path& report) {
    struct Check { std::string name; bool passed; std::wstring detail; ULONGLONG milliseconds; };
    std::vector<Check> checks;
    const auto started = GetTickCount64();
    auto check = [&](const char* name, bool passed, const std::wstring& detail = L"", ULONGLONG ms = 0) {
        checks.push_back({name, passed, detail, ms});
        std::cerr << "headless-check " << name << " passed=" << (passed ? "true" : "false")
                  << " elapsed_ms=" << GetTickCount64() - started << std::endl;
    };
    auto probeNativeMenus = [&](const std::vector<std::pair<const char*, const wchar_t*>>& requests, UINT tab = 0) {
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
        for (const UINT command : {static_cast<UINT>(RibbonOpenMenu), static_cast<UINT>(RibbonOpenWith)}) {
            const auto retained = ribbonCommandChildren_.find(command);
            nativeMenuProperties += L"; retained snapshot " + std::to_wstring(command) + L" present=" +
                std::to_wstring(retained != ribbonCommandChildren_.end()) + L"/object=" +
                std::to_wstring(retained != ribbonCommandChildren_.end() && retained->second != nullptr) + L"/count=" +
                std::to_wstring(retained != ribbonCommandChildren_.end() && retained->second ?
                    retained->second->entries().size() : 0);
            PROPVARIANT value{}; UINT count = 0;
            auto read = ribbon_.framework() ? ribbon_.framework()->GetUICommandProperty(command, UI_PKEY_ItemsSource, &value) : E_UNEXPECTED;
            ComPtr<IUICollection> collection;
            if (SUCCEEDED(read)) read = value.vt == VT_UNKNOWN && value.punkVal ?
                value.punkVal->QueryInterface(IID_PPV_ARGS(&collection)) : E_NOINTERFACE;
            if (SUCCEEDED(read)) read = collection ? collection->GetCount(&count) : E_UNEXPECTED;
            PropVariantClear(&value);
            nativeMenuProperties += L"; native collection " + std::to_wstring(command) + L"=" +
                hresultMessage(read) + L"/" + std::to_wstring(count);
            nativeMenuProperties += collectionDiagnostic(command, L"before expansion");
        }
        auto worker = std::async(std::launch::async, [desktop, ribbonWindow, requests, nativeMenuProperties,
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
                const auto result = accessibleExpandedMenu(automation.Get(), root.Get(), host, request.second, minimumRows);
                results.push_back({request.first, result.passed, result.detail + nativeMenuProperties, 0});
            }
            return results;
        });
        pumpUntil([&] { return worker.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; }, 12000);
        for (auto result : worker.get()) {
            if (result.name == "native_stock_open_with_dropdown_hierarchy") {
                const auto retained = ribbonCommandChildren_.find(RibbonOpenWith);
                result.detail += L"; after expansion retained OpenWith present=" +
                    std::to_wstring(retained != ribbonCommandChildren_.end()) + L"/object=" +
                    std::to_wstring(retained != ribbonCommandChildren_.end() && retained->second != nullptr) + L"/count=" +
                    std::to_wstring(retained != ribbonCommandChildren_.end() && retained->second ?
                        retained->second->entries().size() : 0);
                PROPVARIANT source{}; UINT count = 0;
                auto read = ribbon_.framework() ? ribbon_.framework()->GetUICommandProperty(
                    RibbonOpenWith, UI_PKEY_ItemsSource, &source) : E_UNEXPECTED;
                ComPtr<IUICollection> collection;
                if (SUCCEEDED(read)) read = source.vt == VT_UNKNOWN && source.punkVal ?
                    source.punkVal->QueryInterface(IID_PPV_ARGS(&collection)) : E_NOINTERFACE;
                if (SUCCEEDED(read)) read = collection ? collection->GetCount(&count) : E_UNEXPECTED;
                PropVariantClear(&source);
                result.detail += L"; after expansion native OpenWith=" + hresultMessage(read) + L"/" + std::to_wstring(count);
                result.detail += collectionDiagnostic(RibbonOpenWith, L"after expansion");
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
        PIDLIST_ABSOLUTE raw = nullptr;
        const auto hr = SHParseDisplayName(path.c_str(), nullptr, &raw, 0, nullptr);
        Pidl expected(raw);
        return SUCCEEDED(hr) && currentPidl_ && ILIsEqual(currentPidl_.get(), expected.get());
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
    try {
        check("host_stays_hidden", !IsWindowVisible(window_));
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
        check("breadcrumbs_and_history", breadcrumbsPidls_.size() >= 2 && historyIndex_ >= 1);
        check("native_view_interface", folderView_ && view_);
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
                const auto accessibilityDesktop = GetThreadDesktop(GetCurrentThreadId());
                auto accessibility = std::async(std::launch::async, [&, accessibilityFolderWindow, accessibilityDesktop] {
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
                accessibleCheck("native_accessibility_address", nullptr, address_, L"Address", UIA_EditControlTypeId);
                accessibleCheck("native_accessibility_search", nullptr, search_, L"Search", UIA_EditControlTypeId);
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
            auto checkboxResult = execute(Checkboxes);
            DWORD currentFlags = 0;
            check("checkbox_selection_flag", SUCCEEDED(checkboxResult) && SUCCEEDED(folderView_->GetCurrentFolderFlags(&currentFlags)) && (currentFlags & FWF_CHECKSELECT));
            execute(Checkboxes);
            ComPtr<IColumnManager> columns;
            UINT columnCount = 0;
            const bool columnsAvailable = SUCCEEDED(folderView_.As(&columns)) &&
                SUCCEEDED(columns->GetColumnCount(CM_ENUM_VISIBLE, &columnCount)) && columnCount >= 1;
            check("native_details_columns", columnsAvailable, std::to_wstring(columnCount));
            std::vector<PROPERTYKEY> originalColumns(columnCount);
            if (columns && columnCount) {
                auto columnsResult = columns->GetColumns(CM_ENUM_VISIBLE, originalColumns.data(), columnCount);
                auto contains = [&](const PROPERTYKEY& key) {
                    UINT total = 0;
                    if (FAILED(columns->GetColumnCount(CM_ENUM_VISIBLE, &total))) return false;
                    std::vector<PROPERTYKEY> visible(total);
                    if (FAILED(columns->GetColumns(CM_ENUM_VISIBLE, visible.data(), total))) return false;
                    return std::any_of(visible.begin(), visible.end(), [&](const PROPERTYKEY& value) { return IsEqualPropertyKey(key, value); });
                };
                const bool initiallyVisible = contains(PKEY_Size);
                auto toggleResult = toggleColumn(PKEY_Size);
                check("toggle_details_column", SUCCEEDED(columnsResult) && SUCCEEDED(toggleResult) && contains(PKEY_Size) != initiallyVisible);
                toggleResult = toggleColumn(PKEY_Size);
                check("restore_details_column", SUCCEEDED(toggleResult) && contains(PKEY_Size) == initiallyVisible);
                check("name_column_stays_visible", toggleColumn(PKEY_ItemNameDisplay) == E_ACCESSDENIED && contains(PKEY_ItemNameDisplay));
                auto sizing = sizeColumns();
                bool widthsValid = SUCCEEDED(sizing);
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
            execute(PreviewPane);
            ready = pumpUntil([&] { return !navigating_ && folderView_; }, 5000);
            EXPLORERPANESTATE pane = EPS_DONTCARE; GetPaneState(EP_PreviewPane, &pane);
            check("preview_pane_policy", ready && (pane & EPS_DEFAULT_ON));
            execute(DetailsPane);
            ready = pumpUntil([&] { return !navigating_ && folderView_; }, 5000);
            GetPaneState(EP_PreviewPane, &pane);
            check("panes_mutually_exclusive", ready && preferences_.detailsPane && !preferences_.previewPane && (pane & EPS_DEFAULT_OFF));
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
            if (SUCCEEDED(hr)) hr = saveSearchForScopeRules(L"System.FileName:=\"scope-member.txt\"", originalRules,
                mixedSearchPath, SearchSaveMode::CreateNew, &importedPresentation);
            if (SUCCEEDED(hr)) hr = navigate(mixedSearchPath.wstring());
            const bool mixedImported = SUCCEEDED(hr) && pumpUntil(exactScopeMembership, 5000);
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
            const auto contentChanged = mixedImported ? setView(ViewMode::Content) : E_UNEXPECTED;
            auto nativeContent32 = [&] {
                FOLDERVIEWMODE mode = FVM_AUTO; int size = 0;
                return !searchPresentationPending_ && SUCCEEDED(searchPresentationStatus_) && folderView_ &&
                    SUCCEEDED(folderView_->GetViewModeAndIconSize(&mode, &size)) && mode == FVM_CONTENT && size == 32;
            };
            const bool contentSelected = SUCCEEDED(contentChanged) && pumpUntil(nativeContent32, 2000);
            previousCount = navigationCount_;
            hr = mixedImported ? startSearch(activeQuery_, searchRecursive_, 2, L"System.Size:System.Size#Tiny") : E_UNEXPECTED;
            const bool mixedRefined = SUCCEEDED(hr) && pumpUntil([&] {
                return navigationCount_ > previousCount && exactScopeMembership();
            }, 5000);
            check("mixed_scope_refinement_preserves_native_membership", mixedRefined && sameRules(searchScopeRules_) &&
                activeQuery_.find(L"#Tiny") != std::wstring::npos, scopeDetail(hr));
            const bool contentRefined = mixedRefined && pumpUntil(nativeContent32, 2000);
            const auto refinedMixedQuery = activeQuery_;
            Pidl refinedMixedPidl(ILCloneFull(currentPidl_.get()));
            hr = navigate(fixture.wstring());
            const bool leftMixedSearch = SUCCEEDED(hr) && pumpUntil([&] { return !navigating_ && atLocation(fixture); }, 5000);
            hr = mixedRefined && leftMixedSearch ? execute(Back) : E_UNEXPECTED;
            const bool mixedRestored = SUCCEEDED(hr) && pumpUntil(exactScopeMembership, 5000);
            check("mixed_scope_history_restores_query_rules_and_membership", mixedRestored &&
                sameRules(searchScopeRules_) && activeQuery_ == refinedMixedQuery &&
                ILIsEqual(currentPidl_.get(), refinedMixedPidl.get()), scopeDetail(hr));
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
                SearchSaveMode::CreateNew, &projectedPresentation) : projectionResult;
            SavedSearchMetadata mixedMetadata;
            if (SUCCEEDED(hr)) hr = readSavedSearch(mixedRoundtripPath, &mixedMetadata);
            const bool savedRules = SUCCEEDED(hr) && sameRules(mixedMetadata.scopeRules) && !mixedMetadata.query.empty();
            if (savedRules) hr = navigate(mixedRoundtripPath.wstring());
            const bool roundtripReady = savedRules && SUCCEEDED(hr) && pumpUntil(exactScopeMembership, 5000);
            check("mixed_scope_save_reopens_with_native_membership", roundtripReady && sameRules(searchScopeRules_) &&
                isExternalSearch(currentPidl_.get()), scopeDetail(hr));
            check("saved_search_public_projection_retains_native_results", SUCCEEDED(projectionResult) &&
                !projectedPresentation.mode && projectedPresentation.iconSize == 32 && savedRules &&
                mixedMetadata.presentation && !mixedMetadata.presentation->mode && mixedMetadata.presentation->iconSize == 32 &&
                roundtripReady && activeQuery_ == mixedMetadata.query && exactScopeMembership(),
                L"Projection=" + hresultMessage(projectionResult) + L"; public mode omitted=" +
                std::to_wstring(!projectedPresentation.mode ? 1 : 0) + L"; native query/File IDs preserved=" +
                std::to_wstring(roundtripReady ? 1 : 0));
            const auto libraryFolder = fixture / L"Unicode-\u65e5\u672c\u8a9e";
            std::ofstream(libraryFolder / L"library-member.txt") << "owned library fixture";
            const std::array additionalLibraryFolders{fixture / L"Subfolder" / L"Library location A",
                fixture / L"Subfolder" / L"Library location B"};
            for (const auto& folder : additionalLibraryFolders) std::filesystem::create_directories(folder);
            ShellLibrary fixtureLibrary;
            ComPtr<IShellItem> fixtureLibraryItem;
            hr = ShellLibrary::create(fixtureLibrary);
            if (SUCCEEDED(hr)) hr = fixtureLibrary.addFolder(libraryFolder);
            for (const auto& folder : additionalLibraryFolders)
                if (SUCCEEDED(hr)) hr = fixtureLibrary.addFolder(folder);
            if (SUCCEEDED(hr)) hr = fixtureLibrary.optimize(LibraryKind::Documents);
            if (SUCCEEDED(hr)) hr = fixtureLibrary.setDefaultSaveFolder(libraryFolder);
            if (SUCCEEDED(hr)) hr = fixtureLibrary.save(fixture / L"Subfolder", L"Fixture library", fixtureLibraryItem);
            fixtureLibrary = ShellLibrary{};
            if (SUCCEEDED(hr)) hr = browser_->BrowseToObject(fixtureLibraryItem.Get(), SBSP_ABSOLUTE);
            ready = SUCCEEDED(hr) && pumpUntil([&] { return !navigating_ && library_.valid() && contextPage_ == ContextPage::Library; }, 5000);
            check("library_context_in_hidden_host", ready && contextAvailable(RibbonLibraryContext, true) && commandRegistered(LibraryLocations) &&
                commandRegistered(LibraryDefault) && commandRegistered(LibraryOptimize), hresultMessage(hr));
            std::vector<LibraryFolder> includedFolders;
            std::filesystem::path defaultLibraryPath;
            std::error_code libraryIdentityError;
            check("library_context_reads_included_default_location", ready && SUCCEEDED(library_.folders(includedFolders)) &&
                includedFolders.size() == 3 && SUCCEEDED(library_.defaultSavePath(defaultLibraryPath)) &&
                std::filesystem::equivalent(defaultLibraryPath, libraryFolder, libraryIdentityError) && !libraryIdentityError);
            if (ready && ribbon_.layout() == RibbonLayout::InstalledWindows10) {
                std::wstring defaultLabel, optimizeLabel;
                ribbon_.commandLabel(LibraryDefault, defaultLabel);
                ribbon_.commandLabel(RibbonLibraryOptimizeMenu, optimizeLabel);
                probeNativeMenus({{"native_stock_library_default_dropdown_hierarchy", defaultLabel.c_str()},
                    {"native_stock_library_optimize_dropdown_hierarchy", optimizeLabel.c_str()}}, RibbonLibraryTab);
            }
            const auto librariesNavigation = navigate(L"shell:Libraries");
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
            navigate(fixture.wstring());
            ready = pumpUntil([&] { return !navigating_ && atLocation(fixture); }, 5000);
            check("library_context_removed_after_navigation", ready && !library_.valid() && contextPage_ == ContextPage::None &&
                contextAvailable(RibbonLibraryContext, false));
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
                const auto desktop = GetThreadDesktop(GetCurrentThreadId());
                auto buttonAccessibility = std::async(std::launch::async,
                    [desktop, host = window_, button = ribbonCollapse_, ready = presentation.ready] {
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
                    result = accessibleElement(automation.Get(), nullptr, button, L"Minimize the Ribbon", UIA_ButtonControlTypeId);
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
                    wchar_t buffer[256]{};
                    TOOLINFOW tool{sizeof(tool)}; tool.hwnd = window_;
                    tool.uId = reinterpret_cast<UINT_PTR>(ribbonCollapse_); tool.lpszText = buffer;
                    if (ribbonCollapseTooltip_) SendMessageW(ribbonCollapseTooltip_, TTM_GETTEXTW, std::size(buffer), reinterpret_cast<LPARAM>(&tool));
                    return std::wstring(buffer);
                };
                const bool initialButton = presentation.ready && GetDlgCtrlID(ribbonCollapse_) == Collapse &&
                    textOf(ribbonCollapse_) == L"Minimize the Ribbon" && tooltipText() == L"Minimize the Ribbon (Ctrl+F1)";
                if (presentation.ready) SendMessageW(ribbonCollapse_, BM_CLICK, 0, 0);
                bool collapsed = false;
                const bool clickedMinimize = pumpUntil([&] {
                    return SUCCEEDED(ribbon_.minimized(collapsed)) && collapsed && ribbon_.height() < expandedHeight;
                }, 2000);
                const bool minimizedButton = textOf(ribbonCollapse_) == L"Expand the Ribbon" &&
                    tooltipText() == L"Expand the Ribbon (Ctrl+F1)" && preferences_.ribbonCollapsed;
                if (presentation.ready) SendMessageW(ribbonCollapse_, BM_CLICK, 0, 0);
                const bool clickedExpand = pumpUntil([&] {
                    return SUCCEEDED(ribbon_.minimized(collapsed)) && !collapsed && ribbon_.height() == expandedHeight;
                }, 2000);
                const bool expandedButton = textOf(ribbonCollapse_) == L"Minimize the Ribbon" &&
                    tooltipText() == L"Minimize the Ribbon (Ctrl+F1)" && !preferences_.ribbonCollapsed;
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
                const auto commandRead = navigate(L"cmd.exe /c exit 0", true);
                const auto uriRead = navigate(L"native-explorer-smoke-no-handler://owned", true);
                check("typed_address_headless_blocks_command_and_uri_launch", commandRead == E_ACCESSDENIED &&
                    uriRead == E_ACCESSDENIED && typedAddresses_ == expected && pendingTypedAddress_.empty() &&
                    !pendingTypedAddressTarget_ && atLocation(fixture),
                    L"Command=" + hresultMessage(commandRead) + L"; URI=" + hresultMessage(uriRead) +
                    L"; native command/URI invocation=0");
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

                SetWindowTextW(search_,queryFor(0).c_str());SendMessageW(search_,WM_KEYDOWN,VK_RETURN,0);
                const bool enterWasPending=pendingLiveSearch_&&pendingLiveSearch_->explicitSubmit;
                const bool uncommittedUntilCompletion=!enterWasPending||recentSearches_==recentBeforeLive;
                const bool explicitReady=pumpUntil([&] {
                    return activeQuery_==queryFor(0)&&textOf(search_)==queryFor(0)&&!pendingLiveSearch_&&
                        !liveSearchPolicy_.waiting()&&exactLiveView(liveIdentities[0]);
                },5000);
                const auto afterExplicit=recentSearches_;
                SendMessageW(search_,WM_KEYDOWN,VK_RETURN,0);
                const bool resubmitReady=pumpUntil([&] {return !navigating_&&!pendingLiveSearch_&&!liveSearchPolicy_.waiting();},5000);
                liveCheck("live_search_enter_commits_once_after_native_completion",uncommittedUntilCompletion&&explicitReady&&
                    liveSearchPolicy_.committedLiteral()==queryFor(0)&&resubmitReady&&recentSearches_==afterExplicit&&
                    std::count(recentSearches_.begin(),recentSearches_.end(),queryFor(0))==1&&
                    afterExplicit.size()==recentBeforeLive.size()+1&&history_.size()==liveHistorySize);

                const auto recentAtOverlap=recentSearches_;
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
                    !headlessLiveBrowseProbe_&&newerReady&&olderPendingObserved&&olderHistoryPreserved&&olderMruPreserved);

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
    std::filesystem::remove_all(fixture, filesystemError);
    check("fixture_cleanup", !filesystemError);
    auto failed = std::count_if(checks.begin(), checks.end(), [](const Check& value) { return !value.passed; });
    if (!report.parent_path().empty()) std::filesystem::create_directories(report.parent_path(), filesystemError);
    std::ofstream output(report);
    if (!output) return 9;
    const auto desktop = PrivateDesktop::current();
    bool inputUnchanged = false, inputVisible = true;
    const bool isolated = desktop && SUCCEEDED(desktop->verifyIsolation(&inputUnchanged)) &&
        SUCCEEDED(desktop->visibleWindowsOnInputDesktop(inputVisible));
    const auto nativeFeatures = ribbon_.features();
    output << "{\n  \"headless\": true,\n  \"privateDesktop\": " << (isolated ? "true" : "false")
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
