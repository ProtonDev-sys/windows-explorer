#pragma once

#include <windows.h>
#include <shobjidl.h>
#include <oleidl.h>
#include <wrl/client.h>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace explorer {

struct BreadcrumbChild {
    Microsoft::WRL::ComPtr<IShellItem> item;
    std::wstring label;
    bool selected = false;
};

struct BreadcrumbEnumerationOptions {
    bool showHidden = false;
    unsigned maximumEntries = 4096;
    // Checked between provider calls. A provider blocked inside a COM method
    // cannot be interrupted; use the asynchronous task for network namespaces.
    DWORD timeBudgetMilliseconds = 5000;
    std::shared_ptr<std::atomic<bool>> cancelled;
};

struct BreadcrumbSnapshot {
    std::vector<BreadcrumbChild> children;
    bool complete = true;
    HRESULT stopReason = S_OK;
};

// Native folders and virtual namespaces, localized names and CompareIDs order.
// EnumObjects receives nullptr HWND so credential/media prompts silently fail.
// Enumerate only when a breadcrumb's dropdown is requested, never on navigation.
// S_FALSE returns a bounded partial snapshot; failure leaves output unchanged.
HRESULT enumerateBreadcrumbChildren(IShellItem* parent, IShellItem* selected,
                                    const BreadcrumbEnumerationOptions& options,
                                    BreadcrumbSnapshot* result);

// Expand the actual embedded native tree node through the public TreeView
// notification path. Some Windows 10 ExplorerBrowser trees accept SetItemState
// while leaving a folder with more than 100 children collapsed. Canonical native
// HitTest, mapped rectangles and HWND process/thread/ancestry checks establish
// the exact node before TVM_EXPAND. No focus/selection/input or roots are changed.
// S_OK means native GetItemState confirms expansion; E_PENDING means dispatch
// occurred but expansion is not yet confirmed. The caller polls its original
// native tree, with its navigation generation/budget; this function never waits.
// Hidden/private-desktop app-owned HWNDs are valid for headless UI fixtures.
HRESULT expandNativeTreeItem(INameSpaceTreeControl* tree, IShellItem* item, HWND owner);

// Worker thread owns its own STA and provider interfaces. Only PIDL bytes and
// strings cross threads; poll materializes Shell items on the caller's STA.
// Destructor cancels without waiting on a blocked network/provider call.
// Worker owns its creator-desktop handle and attaches before COM/provider calls.
// Shutdown drains workers before the creator apartment ends; normal navigation
// cancellation remains nonblocking.
class BreadcrumbEnumerationTask final {
public:
    ~BreadcrumbEnumerationTask();
    BreadcrumbEnumerationTask(const BreadcrumbEnumerationTask&) = delete;
    BreadcrumbEnumerationTask& operator=(const BreadcrumbEnumerationTask&) = delete;
    static HRESULT start(IShellItem* parent, IShellItem* selected,
                         const BreadcrumbEnumerationOptions& options,
                         std::unique_ptr<BreadcrumbEnumerationTask>* result);
    HRESULT poll(BreadcrumbSnapshot* result);
    void cancel() noexcept;
private:
    struct State;
    explicit BreadcrumbEnumerationTask(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
    DWORD callerThread_ = 0;
};

using BreadcrumbHitTest = std::function<HRESULT(POINTL screenPoint, IShellItem** result)>;
using BreadcrumbTargetBinder = std::function<HRESULT(IShellItem* item, IDropTarget** result)>;

struct BreadcrumbDropOptions {
    bool headless = false;
    BreadcrumbHitTest hitTest;
    Microsoft::WRL::ComPtr<IUnknown> site;
    // Optional provider dependency, primarily for isolated tests. Default binds
    // the actual item with BHID_SFUIObject, then BHID_SFViewObject when necessary.
    BreadcrumbTargetBinder bindTarget;
};

class BreadcrumbDropTarget final : public IDropTarget {
public:
    static HRESULT create(HWND owner, const BreadcrumbDropOptions& options,
                          BreadcrumbDropTarget** result);
    HRESULT registerWindow();
    HRESULT revokeWindow();
    bool registered() const noexcept { return registered_; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD keys, POINTL point, DWORD* effect) override;
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL point, DWORD* effect) override;
    HRESULT STDMETHODCALLTYPE DragLeave() override;
    // Always rejects headless mode. No test must call a real provider's Drop.
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keys, POINTL point, DWORD* effect) override;

private:
    BreadcrumbDropTarget(HWND owner, BreadcrumbDropOptions options);
    ~BreadcrumbDropTarget();
    HRESULT checkThread() const noexcept;
    HRESULT route(DWORD keys, POINTL point, DWORD* effect, bool dropping);
    HRESULT leaveTarget() noexcept;
    void clearData() noexcept;
    HRESULT nativeTarget(IShellItem* item, IDropTarget** result);
    void limitEffect(DWORD* effect, DWORD source) const noexcept;

    std::atomic<ULONG> references_{1};
    HWND owner_ = nullptr;
    DWORD thread_ = 0;
    bool registered_ = false;
    BreadcrumbDropOptions options_;
    Microsoft::WRL::ComPtr<IDataObject> data_;
    Microsoft::WRL::ComPtr<IShellItem> item_;
    Microsoft::WRL::ComPtr<IDropTarget> target_;
    Microsoft::WRL::ComPtr<IObjectWithSite> targetSite_;
    bool siteAttached_ = false;
    DWORD sourceEffects_ = DROPEFFECT_NONE;
};

} // namespace explorer
