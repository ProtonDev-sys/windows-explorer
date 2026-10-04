#include "explorer/selection.hpp"
#include "explorer/namespace_actions.hpp"
#include "explorer/headless_visual.hpp"
#include <docobj.h>
#include <algorithm>
#include <shlobj.h>
#include <wrl/client.h>
#include <memory>
#include <new>
#include <vector>

namespace explorer {
namespace {
using Microsoft::WRL::ComPtr;
struct ChildDeleter {
    using pointer = PITEMID_CHILD;
    void operator()(pointer value) const noexcept { ILFree(value); }
};
using ChildPidl = std::unique_ptr<ITEMIDLIST, ChildDeleter>;
class RedrawScope {
public:
    explicit RedrawScope(IFolderView2* view) noexcept : view_(view), status_(view->SetRedraw(FALSE)) {}
    ~RedrawScope() { if (!finished_ && SUCCEEDED(status_)) view_->SetRedraw(TRUE); }
    HRESULT status() const noexcept { return status_; }
    HRESULT finish(HRESULT operation) noexcept {
        finished_ = true;
        if (FAILED(status_)) return status_;
        const auto restored = view_->SetRedraw(TRUE);
        return FAILED(operation) ? operation : restored;
    }
private:
    IFolderView2* view_;
    HRESULT status_;
    bool finished_ = false;
};

bool unavailableCommand(HRESULT hr) noexcept {
    return hr == E_NOINTERFACE || hr == E_NOTIMPL || hr == OLECMDERR_E_NOTSUPPORTED ||
           hr == OLECMDERR_E_UNKNOWNGROUP;
}
}

HRESULT changeShellSelection(IFolderView2* folderView, IShellView* shellView,
                             SelectionAction action, NativeNamespaceActions* nativeActions,
                             bool headless) noexcept {
    if (!folderView || !shellView) return E_POINTER;
    if (action != SelectionAction::All && action != SelectionAction::None &&
        action != SelectionAction::Invert) return E_INVALIDARG;
    ComPtr<IUnknown> folderIdentity, viewIdentity;
    auto identityResult = folderView->QueryInterface(IID_PPV_ARGS(&folderIdentity));
    if (SUCCEEDED(identityResult)) identityResult = shellView->QueryInterface(IID_PPV_ARGS(&viewIdentity));
    if (FAILED(identityResult)) return identityResult;
    if (folderIdentity.Get() != viewIdentity.Get()) return E_INVALIDARG;
    if (headless) {
        const auto desktop = PrivateDesktop::current();
        if (!desktop || !desktop->ready() || FAILED(desktop->verifyIsolation())) return E_ACCESSDENIED;
        HWND viewWindow = nullptr;
        DWORD process = 0;
        const auto ownedWindow = shellView->GetWindow(&viewWindow);
        if (FAILED(ownedWindow)) return ownedWindow;
        if (!IsWindow(viewWindow) || GetWindowThreadProcessId(viewWindow, &process) != GetCurrentThreadId() ||
            process != GetCurrentProcessId()) return E_ACCESSDENIED;
    }
    try {
        int total = 0;
        auto hr = folderView->ItemCount(SVGIO_ALLVIEW, &total);
        if (FAILED(hr)) return hr;
        if (total < 0) return E_UNEXPECTED;
        // Empty-view shortcuts are ordinary no-ops. The registered command
        // may correctly be disabled; that should not produce an error dialog.
        if (!total) return S_OK;
        if (action == SelectionAction::None || action == SelectionAction::Invert) {
            int selectedCount = 0;
            hr = folderView->ItemCount(SVGIO_SELECTION, &selectedCount);
            if (FAILED(hr)) return hr;
            if (selectedCount < 0 || selectedCount > total) return E_UNEXPECTED;
            if (action == SelectionAction::None && !selectedCount) return S_OK;
            // Clearing is one documented native view operation. Constructing
            // a registered menu adds hundreds of milliseconds to a large
            // selection without changing its empty complement or focus.
            if (action == SelectionAction::None || selectedCount == total)
                return shellView->SelectItem(nullptr, SVSI_DESELECTOTHERS);
        }
        if (nativeActions) {
            const auto name = action == SelectionAction::All ? L"Windows.SelectAll" :
                action == SelectionAction::None ? L"Windows.SelectNone" : L"Windows.InvertSelection";
            const auto native = nativeActions->invokeViewSelection(name, shellView, headless);
            if (!unavailableCommand(native) && native != HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) &&
                native != HRESULT_FROM_WIN32(ERROR_NOT_FOUND) && native != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) &&
                native != E_PENDING && native != HRESULT_FROM_WIN32(ERROR_BUSY)) return native;
        }
        if (action == SelectionAction::None) return shellView->SelectItem(nullptr, SVSI_DESELECTOTHERS);
        // A registered selection provider above is the preferred native route.
        // The generic document OLE Select All command can report success while
        // doing nothing in a Shell view without document focus. Fall back to
        // the documented bulk operation on this exact folder view instead.

        std::vector<bool> selected;
        int selectedCount = 0;
        if (action == SelectionAction::Invert) {
            hr = folderView->ItemCount(SVGIO_SELECTION, &selectedCount);
            if (FAILED(hr)) return hr;
            if (selectedCount < 0 || selectedCount > total) return E_UNEXPECTED;
            if (selectedCount == total) return shellView->SelectItem(nullptr, SVSI_DESELECTOTHERS);
            selected.resize(static_cast<size_t>(total), false);
            int start = 0, found = 0;
            bool usableIndices = true;
            while (start < total) {
                int index = -1;
                hr = folderView->GetSelectedItem(start, &index);
                if (hr == S_FALSE) break;
                if (FAILED(hr)) return hr;
                if (index < start || index >= total) { usableIndices = false; break; }
                selected[static_cast<size_t>(index)] = true;
                ++found;
                start = index + 1;
            }
            if (!usableIndices || found != selectedCount) {
                // Some native views repeatedly return their first selected
                // index, ignoring iStart. Reconstruct the entire snapshot from
                // actual per-item state; never accept that incomplete list.
                std::fill(selected.begin(), selected.end(), false);
                found = 0;
                for (int index = 0; index < total; ++index) {
                    PITEMID_CHILD raw = nullptr;
                    hr = folderView->Item(index, &raw);
                    ChildPidl child(raw);
                    if (FAILED(hr)) return hr;
                    if (!child || ILIsEmpty(child.get())) return E_UNEXPECTED;
                    DWORD state = 0;
                    hr = folderView->GetSelectionState(child.get(), &state);
                    if (FAILED(hr)) return hr;
                    const bool isSelected = (state & SVSI_SELECT) != 0;
                    selected[static_cast<size_t>(index)] = isSelected;
                    if (isSelected) ++found;
                }
                if (found != selectedCount) return HRESULT_FROM_WIN32(ERROR_RETRY);
            }
        }

        // Preflight every target before changing the selection. A single bulk
        // call avoids repeatedly searching the view by PIDL and issuing a
        // selection-change notification for every individual item.
        std::vector<ChildPidl> owned;
        std::vector<PCUITEMID_CHILD> children;
        const auto targetCount = static_cast<size_t>(total - selectedCount);
        owned.reserve(targetCount);
        children.reserve(targetCount);
        for (int index = 0; index < total; ++index) {
            if (!selected.empty() && selected[static_cast<size_t>(index)]) continue;
            PITEMID_CHILD raw = nullptr;
            hr = folderView->Item(index, &raw);
            ChildPidl child(raw);
            if (FAILED(hr)) return hr;
            if (!child || ILIsEmpty(child.get())) return E_UNEXPECTED;
            children.push_back(reinterpret_cast<PCUITEMID_CHILD>(child.get()));
            owned.push_back(std::move(child));
        }
        int currentTotal = 0, currentSelected = 0;
        hr = folderView->ItemCount(SVGIO_ALLVIEW, &currentTotal);
        if (SUCCEEDED(hr) && action == SelectionAction::Invert)
            hr = folderView->ItemCount(SVGIO_SELECTION, &currentSelected);
        if (FAILED(hr)) return hr;
        if (currentTotal != total || (action == SelectionAction::Invert && currentSelected != selectedCount))
            return HRESULT_FROM_WIN32(ERROR_RETRY);
        RedrawScope redraw(folderView);
        if (FAILED(redraw.status())) return redraw.status();
        // DESELECTOTHERS is applied per item by some native views. Clear once
        // before the batch, otherwise only its last item would stay selected.
        if (action == SelectionAction::Invert)
            hr = shellView->SelectItem(nullptr, SVSI_DESELECTOTHERS);
        if (SUCCEEDED(hr))
            hr = folderView->SelectAndPositionItems(static_cast<UINT>(children.size()), children.data(), nullptr,
                SVSI_SELECT | SVSI_NOTAKEFOCUS);
        hr = redraw.finish(hr);
        if (FAILED(hr)) return hr;
        int after = -1;
        hr = folderView->ItemCount(SVGIO_SELECTION,&after);
        if (FAILED(hr)) return hr;
        if (after == static_cast<int>(children.size())) return S_OK;
        // Some ItemsView revisions acknowledge the bulk call without changing
        // selection. Keep the preflighted complement and use the original
        // Shell view's PIDL operation, preserving item/window focus.
        for (const auto child : children) {
            hr = shellView->SelectItem(child,SVSI_SELECT | SVSI_NOTAKEFOCUS);
            if (FAILED(hr)) return hr;
        }
        return hr;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}
}
