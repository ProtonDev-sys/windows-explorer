#include "explorer/breadcrumb.hpp"
#include "explorer/worker_sta.hpp"

#include <shlobj.h>
#include <commctrl.h>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>
#include <utility>

namespace explorer {
namespace {
using Microsoft::WRL::ComPtr;
constexpr unsigned maximumEntries = 4096;
constexpr size_t maximumPidl = 65536;
constexpr size_t maximumSnapshotBytes = 16 * 1024 * 1024;
constexpr DWORD actionEffects = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;

struct TreeControls {
    HWND owner = nullptr;
    DWORD thread = 0;
    DWORD process = 0;
    unsigned visited = 0;
    std::vector<HWND> windows;
};

BOOL CALLBACK collectTreeControls(HWND window, LPARAM parameter) {
    auto& controls = *reinterpret_cast<TreeControls*>(parameter);
    if (++controls.visited > 512 || controls.windows.size() >= 32) return FALSE;
    DWORD process = 0;
    if (GetWindowThreadProcessId(window,&process) != controls.thread || process != controls.process ||
        !IsChild(controls.owner,window)) return TRUE;
    wchar_t name[64]{};
    if (GetClassNameW(window,name,static_cast<int>(std::size(name))) && lstrcmpW(name,WC_TREEVIEWW) == 0)
        controls.windows.push_back(window);
    return TRUE;
}

struct PidlDelete {
    using pointer = PIDLIST_ABSOLUTE;
    void operator()(PIDLIST_ABSOLUTE value) const noexcept { CoTaskMemFree(value); }
};
using OwnedPidl = std::unique_ptr<ITEMIDLIST_ABSOLUTE,PidlDelete>;
struct ChildPidlDelete {
    using pointer = PITEMID_CHILD;
    void operator()(PITEMID_CHILD value) const noexcept { CoTaskMemFree(value); }
};
using OwnedChildPidl = std::unique_ptr<ITEMID_CHILD,ChildPidlDelete>;
struct TextDelete { void operator()(wchar_t* value) const noexcept { CoTaskMemFree(value); } };
struct ProviderFailure { HRESULT value; };

HRESULT validOptions(const BreadcrumbEnumerationOptions& options) {
    if (!options.maximumEntries || options.maximumEntries > maximumEntries ||
        !options.timeBudgetMilliseconds || options.timeBudgetMilliseconds > 60000) return E_INVALIDARG;
    return S_OK;
}

HRESULT sta() {
    APTTYPE apartment{};
    APTTYPEQUALIFIER qualifier{};
    const HRESULT hr = CoGetApartmentType(&apartment,&qualifier);
    if (FAILED(hr)) return hr;
    return apartment == APTTYPE_STA || apartment == APTTYPE_MAINSTA ? S_OK : RPC_E_WRONG_THREAD;
}

bool cancelled(const BreadcrumbEnumerationOptions& options) {
    return options.cancelled && options.cancelled->load();
}

HRESULT snapshotFolder(IShellItem* parent, IShellItem* selected,
                       const BreadcrumbEnumerationOptions& options,
                       BreadcrumbSnapshot& result) {
    ComPtr<IShellFolder> folder;
    HRESULT hr = parent->BindToHandler(nullptr,BHID_SFObject,IID_PPV_ARGS(&folder));
    if (FAILED(hr)) return hr;
    PIDLIST_ABSOLUTE rawParent = nullptr;
    hr = SHGetIDListFromObject(parent,&rawParent);
    OwnedPidl parentId(rawParent);
    if (FAILED(hr)) return hr;
    if (!parentId) return E_UNEXPECTED;
    ComPtr<IEnumIDList> enumerator;
    DWORD flags = SHCONTF_FOLDERS | SHCONTF_INIT_ON_FIRST_NEXT;
    if (options.showHidden) flags |= SHCONTF_INCLUDEHIDDEN;
    // A null owner is documented to suppress authentication/media prompts.
    hr = folder->EnumObjects(nullptr,static_cast<SHCONTF>(flags),&enumerator);
    if (hr == S_FALSE && !enumerator) { result = {}; return S_OK; }
    if (FAILED(hr)) return hr;
    if (!enumerator) return E_UNEXPECTED;
    struct PendingChild {
        OwnedChildPidl id;
        BreadcrumbChild child;
    };
    std::vector<PendingChild> pending;
    pending.reserve(std::min(options.maximumEntries,128u));
    const ULONGLONG started = GetTickCount64();
    size_t bytes = 0;
    BreadcrumbSnapshot snapshot;
    for (;;) {
        if (cancelled(options)) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        if (GetTickCount64() - started >= options.timeBudgetMilliseconds) {
            snapshot.complete = false;
            snapshot.stopReason = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            break;
        }
        PITEMID_CHILD raw = nullptr;
        ULONG fetched = 0;
        hr = enumerator->Next(1,&raw,&fetched);
        OwnedChildPidl childId(raw);
        if (hr == S_FALSE && !fetched) break;
        if (FAILED(hr)) return hr;
        if (fetched != 1 || !raw) return E_UNEXPECTED;
        SFGAOF attributes = SFGAO_FOLDER | SFGAO_HIDDEN;
        PCUITEMID_CHILD relative = raw;
        hr = folder->GetAttributesOf(1,&relative,&attributes);
        if (FAILED(hr)) return hr;
        // Providers are allowed to interpret enumeration flags; retain the
        // explicit folder/hidden policy before constructing menu destinations.
        if (!(attributes & SFGAO_FOLDER) || (!options.showHidden && (attributes & SFGAO_HIDDEN))) continue;
        if (pending.size() == options.maximumEntries) {
            snapshot.complete = false;
            snapshot.stopReason = HRESULT_FROM_WIN32(ERROR_MORE_DATA);
            break;
        }
        const UINT size = ILGetSize(raw);
        if (!size || size > maximumPidl) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        ComPtr<IShellItem> item;
        hr = SHCreateItemWithParent(parentId.get(),folder.Get(),raw,IID_PPV_ARGS(&item));
        if (FAILED(hr)) return hr;
        PWSTR text = nullptr;
        hr = item->GetDisplayName(SIGDN_NORMALDISPLAY,&text);
        std::unique_ptr<wchar_t,TextDelete> label(text);
        if (FAILED(hr)) return hr;
        if (!label) return E_UNEXPECTED;
        const size_t length = wcsnlen_s(label.get(),32769);
        if (length > 32768) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        bytes += size + (length + 1) * sizeof(wchar_t);
        if (bytes > maximumSnapshotBytes) {
            snapshot.complete = false;
            snapshot.stopReason = HRESULT_FROM_WIN32(ERROR_MORE_DATA);
            break;
        }
        BreadcrumbChild child;
        child.item = item;
        child.label = label.get();
        if (selected) {
            int order = 1;
            hr = item->Compare(selected,static_cast<SICHINTF>(SICHINT_CANONICAL | SICHINT_TEST_FILESYSPATH_IF_NOT_EQUAL),&order);
            if (FAILED(hr)) return hr;
            child.selected = order == 0;
        }
        pending.push_back({std::move(childId),std::move(child)});
    }
    if (cancelled(options)) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    // Abort immediately if the provider fails: changing a comparator to always
    // return false after failure would violate stable_sort's ordering contract.
    try {
        std::stable_sort(pending.begin(),pending.end(),[&](const auto& left,const auto& right) {
            if (cancelled(options)) throw ProviderFailure{HRESULT_FROM_WIN32(ERROR_CANCELLED)};
            const HRESULT comparison = folder->CompareIDs(0,left.id.get(),right.id.get());
            if (FAILED(comparison)) throw ProviderFailure{comparison};
            return static_cast<short>(HRESULT_CODE(comparison)) < 0;
        });
    } catch (const ProviderFailure& failure) { return failure.value; }
    if (cancelled(options)) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    snapshot.children.reserve(pending.size());
    for (auto& item : pending) snapshot.children.push_back(std::move(item.child));
    result = std::move(snapshot);
    return result.complete ? S_OK : S_FALSE;
}

HRESULT pidlBytes(IShellItem* item, std::vector<unsigned char>& result) {
    if (!item) { result.clear(); return S_OK; }
    PIDLIST_ABSOLUTE raw = nullptr;
    const HRESULT hr = SHGetIDListFromObject(item,&raw);
    OwnedPidl owned(raw);
    if (FAILED(hr)) return hr;
    if (!raw) return E_UNEXPECTED;
    const UINT size = ILGetSize(raw);
    if (!size || size > maximumPidl) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    const auto first = reinterpret_cast<const unsigned char*>(raw);
    result.assign(first,first + size);
    return S_OK;
}

struct SelfReference {
    BreadcrumbDropTarget* value;
    explicit SelfReference(BreadcrumbDropTarget* target) : value(target) { value->AddRef(); }
    ~SelfReference() { value->Release(); }
};
} // namespace

HRESULT expandNativeTreeItem(INameSpaceTreeControl* tree, IShellItem* item, HWND owner) {
    if (!tree || !item) return E_POINTER;
    if (!owner || !IsWindow(owner)) return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
    DWORD process = 0;
    const auto thread = GetWindowThreadProcessId(owner,&process);
    if (process != GetCurrentProcessId()) return E_ACCESSDENIED;
    if (thread != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
    auto hr = sta();
    if (FAILED(hr)) return hr;
    ComPtr<IOleWindow> located;
    hr = tree->QueryInterface(IID_PPV_ARGS(&located));
    if (FAILED(hr)) return hr;
    HWND nativeWindow = nullptr;
    hr = located->GetWindow(&nativeWindow);
    if (FAILED(hr)) return hr;
    DWORD nativeProcess = 0;
    if (!nativeWindow || !IsWindow(nativeWindow) ||
        GetWindowThreadProcessId(nativeWindow,&nativeProcess) != thread || nativeProcess != process ||
        (nativeWindow != owner && !IsChild(owner,nativeWindow))) return E_ACCESSDENIED;
    NSTCITEMSTATE state = NSTCIS_NONE;
    hr = tree->GetItemState(item,static_cast<NSTCITEMSTATE>(NSTCIS_EXPANDED | NSTCIS_DISABLED),&state);
    if (FAILED(hr)) return hr;
    if (state & NSTCIS_DISABLED) return E_ACCESSDENIED;
    if (state & NSTCIS_EXPANDED) return S_OK;
    SFGAOF attributes = 0;
    hr = item->GetAttributes(SFGAO_FOLDER,&attributes);
    if (FAILED(hr)) return hr;
    if (!(attributes & SFGAO_FOLDER)) return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
    hr = tree->EnsureItemVisible(item);
    if (FAILED(hr)) return hr;
    RECT bounds{};
    hr = tree->GetItemRect(item,&bounds);
    if (FAILED(hr)) return hr;
    if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) return E_PENDING;
    POINT point{bounds.left + (bounds.right - bounds.left)/2,
                bounds.top + (bounds.bottom - bounds.top)/2};
    // GetItemRect returns screen coordinates; native HitTest receives client
    // coordinates. Keeping them distinct also handles a pane below the Ribbon
    // and a host positioned anywhere on any monitor.
    POINT nativePoint = point;
    if (!ScreenToClient(nativeWindow,&nativePoint)) return HRESULT_FROM_WIN32(GetLastError());
    ComPtr<IShellItem> hit;
    hr = tree->HitTest(&nativePoint,&hit);
    if (FAILED(hr)) return hr;
    if (!hit) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    int comparison = 1;
    hr = hit->Compare(item,SICHINT_CANONICAL,&comparison);
    if (FAILED(hr)) return hr;
    if (comparison) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    TreeControls controls{nativeWindow,thread,process,0,{}};
    wchar_t name[64]{};
    if (GetClassNameW(nativeWindow,name,static_cast<int>(std::size(name))) && lstrcmpW(name,WC_TREEVIEWW) == 0)
        controls.windows.push_back(nativeWindow);
    EnumChildWindows(nativeWindow,collectTreeControls,reinterpret_cast<LPARAM>(&controls));
    for (const auto control : controls.windows) {
        POINT mappedPoint = point;
        RECT mappedBounds = bounds;
        MapWindowPoints(HWND_DESKTOP,control,&mappedPoint,1);
        MapWindowPoints(HWND_DESKTOP,control,reinterpret_cast<POINT*>(&mappedBounds),2);
        TVHITTESTINFO info{};
        info.pt = mappedPoint;
        const auto node = TreeView_HitTest(control,&info);
        if (!node) continue;
        RECT actual{};
        if (!TreeView_GetItemRect(control,node,&actual,FALSE) ||
            actual.top != mappedBounds.top || actual.bottom != mappedBounds.bottom ||
            actual.bottom - actual.top != mappedBounds.bottom - mappedBounds.top ||
            mappedPoint.y < actual.top || mappedPoint.y >= actual.bottom) continue;
        // The native notification path can return FALSE while asynchronously
        // populating a large folder. Only native readback establishes success.
        SendMessageW(control,TVM_EXPAND,TVE_EXPAND,reinterpret_cast<LPARAM>(node));
        state = NSTCIS_NONE;
        hr = tree->GetItemState(item,NSTCIS_EXPANDED,&state);
        if (FAILED(hr)) return hr;
        return (state & NSTCIS_EXPANDED) ? S_OK : E_PENDING;
    }
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

HRESULT enumerateBreadcrumbChildren(IShellItem* parent, IShellItem* selected,
                                    const BreadcrumbEnumerationOptions& options,
                                    BreadcrumbSnapshot* result) {
    if (!result) return E_POINTER;
    if (!parent) return E_INVALIDARG;
    HRESULT hr = validOptions(options);
    if (FAILED(hr)) return hr;
    hr = sta();
    if (FAILED(hr)) return hr;
    if (cancelled(options)) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    try {
        BreadcrumbSnapshot snapshot;
        hr = snapshotFolder(parent,selected,options,snapshot);
        if (FAILED(hr)) return hr;
        *result = std::move(snapshot);
        return hr;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (...) { return E_FAIL; }
}

struct BreadcrumbEnumerationTask::State {
    struct Child { std::vector<unsigned char> id; std::wstring label; bool selected = false; };
    std::mutex mutex;
    BreadcrumbEnumerationOptions options;
    bool ready = false;
    bool complete = true;
    HRESULT result = E_PENDING;
    HRESULT stopReason = S_OK;
    std::vector<Child> children;
};

BreadcrumbEnumerationTask::BreadcrumbEnumerationTask(std::shared_ptr<State> state)
    : state_(std::move(state)),callerThread_(GetCurrentThreadId()) {}
BreadcrumbEnumerationTask::~BreadcrumbEnumerationTask() { cancel(); }

HRESULT BreadcrumbEnumerationTask::start(IShellItem* parent, IShellItem* selected,
                                         const BreadcrumbEnumerationOptions& options,
                                         std::unique_ptr<BreadcrumbEnumerationTask>* result) {
    if (!result) return E_POINTER;
    if (!parent) return E_INVALIDARG;
    HRESULT hr = validOptions(options);
    if (FAILED(hr)) return hr;
    hr = sta();
    if (FAILED(hr)) return hr;
    if (cancelled(options)) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    try {
        std::vector<unsigned char> parentId,selectedId;
        hr = pidlBytes(parent,parentId);
        if (SUCCEEDED(hr)) hr = pidlBytes(selected,selectedId);
        if (FAILED(hr)) return hr;
        auto state = std::make_shared<State>();
        state->options = options;
        if (!state->options.cancelled) state->options.cancelled = std::make_shared<std::atomic<bool>>(false);
        auto task = std::unique_ptr<BreadcrumbEnumerationTask>(new BreadcrumbEnumerationTask(state));
        std::unique_ptr<StaWorkerLease> lease;
        hr=StaWorkerLease::prepare(&lease);
        if(FAILED(hr))return hr;
        std::thread([state,lease=std::move(lease),parentId = std::move(parentId),selectedId = std::move(selectedId)]() {
            // The worker owns an exact creator-desktop handle and releases it
            // after all provider objects and COM, even after cancellation.
            HRESULT final = lease->attach();
            if (SUCCEEDED(final)) final = CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
            const bool initialized = SUCCEEDED(final);
            BreadcrumbSnapshot snapshot;
            std::vector<State::Child> children;
            try {
                if (initialized) {
                    ComPtr<IShellItem> parentItem,selectedItem;
                    final = SHCreateItemFromIDList(reinterpret_cast<PCIDLIST_ABSOLUTE>(parentId.data()),IID_PPV_ARGS(&parentItem));
                    if (SUCCEEDED(final) && !selectedId.empty())
                        final = SHCreateItemFromIDList(reinterpret_cast<PCIDLIST_ABSOLUTE>(selectedId.data()),IID_PPV_ARGS(&selectedItem));
                    if (SUCCEEDED(final)) final = enumerateBreadcrumbChildren(parentItem.Get(),selectedItem.Get(),state->options,&snapshot);
                    if (SUCCEEDED(final)) {
                        children.reserve(snapshot.children.size());
                        size_t bytes = 0;
                        for (auto& child : snapshot.children) {
                            State::Child record;
                            const HRESULT copied = pidlBytes(child.item.Get(),record.id);
                            if (FAILED(copied)) { final = copied; break; }
                            bytes += record.id.size() + (child.label.size() + 1) * sizeof(wchar_t);
                            if (bytes > maximumSnapshotBytes) {
                                snapshot.complete = false;
                                snapshot.stopReason = HRESULT_FROM_WIN32(ERROR_MORE_DATA);
                                final = S_FALSE;
                                break;
                            }
                            record.label = std::move(child.label);
                            record.selected = child.selected;
                            children.push_back(std::move(record));
                        }
                    }
                }
            } catch (const std::bad_alloc&) { final = E_OUTOFMEMORY; }
              catch (...) { final = E_FAIL; }
            // This also runs after a serialization/allocation exception. No
            // worker-apartment provider object may outlive COM initialization.
            snapshot.children.clear();
            if (initialized) CoUninitialize();
            const auto released=lease->finish();
            if(SUCCEEDED(final)&&FAILED(released))final=released;
            std::lock_guard lock(state->mutex);
            state->complete = snapshot.complete;
            state->stopReason = snapshot.stopReason;
            state->result = final;
            state->children = std::move(children);
            state->ready = true;
        }).detach();
        *result = std::move(task);
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (...) { return E_FAIL; }
}

HRESULT BreadcrumbEnumerationTask::poll(BreadcrumbSnapshot* result) {
    if (!result) return E_POINTER;
    if (callerThread_ != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
    HRESULT hr = sta();
    if (FAILED(hr)) return hr;
    if (state_->options.cancelled->load()) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    try {
        std::lock_guard lock(state_->mutex);
        if (!state_->ready) return E_PENDING;
        if (FAILED(state_->result)) return state_->result;
        BreadcrumbSnapshot snapshot;
        snapshot.complete = state_->complete;
        snapshot.stopReason = state_->stopReason;
        snapshot.children.reserve(state_->children.size());
        for (const auto& child : state_->children) {
            BreadcrumbChild item;
            hr = SHCreateItemFromIDList(reinterpret_cast<PCIDLIST_ABSOLUTE>(child.id.data()),IID_PPV_ARGS(&item.item));
            if (FAILED(hr)) return hr;
            item.label = child.label;
            item.selected = child.selected;
            snapshot.children.push_back(std::move(item));
        }
        *result = std::move(snapshot);
        return state_->result;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (...) { return E_FAIL; }
}

void BreadcrumbEnumerationTask::cancel() noexcept { state_->options.cancelled->store(true); }

BreadcrumbDropTarget::BreadcrumbDropTarget(HWND owner,BreadcrumbDropOptions options)
    : owner_(owner),thread_(GetCurrentThreadId()),options_(std::move(options)) {}

BreadcrumbDropTarget::~BreadcrumbDropTarget() {
    leaveTarget();
    clearData();
}

HRESULT BreadcrumbDropTarget::create(HWND owner,const BreadcrumbDropOptions& options,
                                     BreadcrumbDropTarget** result) {
    if (!result) return E_POINTER;
    *result = nullptr;
    if (!options.hitTest) return E_INVALIDARG;
    const HRESULT hr = sta();
    if (FAILED(hr)) return hr;
    try { *result = new BreadcrumbDropTarget(owner,options); return S_OK; }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}

HRESULT BreadcrumbDropTarget::checkThread() const noexcept {
    return thread_ == GetCurrentThreadId() ? S_OK : RPC_E_WRONG_THREAD;
}

HRESULT BreadcrumbDropTarget::registerWindow() {
    if (options_.headless) return E_ACCESSDENIED;
    HRESULT hr = checkThread();
    if (FAILED(hr)) return hr;
    if (registered_) return DRAGDROP_E_ALREADYREGISTERED;
    DWORD process = 0;
    const DWORD windowThread = owner_ ? GetWindowThreadProcessId(owner_,&process) : 0;
    if (!windowThread || process != GetCurrentProcessId() || windowThread != thread_) return DRAGDROP_E_INVALIDHWND;
    hr = RegisterDragDrop(owner_,this);
    if (SUCCEEDED(hr)) registered_ = true;
    return hr;
}

HRESULT BreadcrumbDropTarget::revokeWindow() {
    const HRESULT hr = checkThread();
    if (FAILED(hr)) return hr;
    SelfReference retained(this);
    leaveTarget();
    clearData();
    if (!registered_) return S_FALSE;
    const HRESULT revoked = RevokeDragDrop(owner_);
    if (SUCCEEDED(revoked) || revoked == DRAGDROP_E_NOTREGISTERED) registered_ = false;
    return revoked;
}

HRESULT BreadcrumbDropTarget::QueryInterface(REFIID iid,void** result) {
    if (!result) return E_POINTER;
    *result = nullptr;
    if (iid == IID_IUnknown || iid == IID_IDropTarget) {
        *result = static_cast<IDropTarget*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}
ULONG BreadcrumbDropTarget::AddRef() { return ++references_; }
ULONG BreadcrumbDropTarget::Release() { const ULONG remaining = --references_; if (!remaining) delete this; return remaining; }

HRESULT BreadcrumbDropTarget::nativeTarget(IShellItem* item,IDropTarget** result) {
    if (!result) return E_POINTER;
    *result = nullptr;
    ComPtr<IDropTarget> target;
    HRESULT hr = item->BindToHandler(nullptr,BHID_SFUIObject,IID_PPV_ARGS(&target));
    if (hr == E_NOINTERFACE || hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)) {
        target.Reset();
        hr = item->BindToHandler(nullptr,BHID_SFViewObject,IID_PPV_ARGS(&target));
    }
    if (FAILED(hr)) return hr;
    if (!target) return E_UNEXPECTED;
    *result = target.Detach();
    return hr;
}

HRESULT BreadcrumbDropTarget::leaveTarget() noexcept {
    HRESULT hr = S_OK;
    try { if (target_) hr = target_->DragLeave(); }
    catch (...) { hr = E_FAIL; }
    try {
        if (siteAttached_ && targetSite_) {
            const HRESULT detached = targetSite_->SetSite(nullptr);
            if (FAILED(detached) && SUCCEEDED(hr)) hr = detached;
        }
    } catch (...) { if (SUCCEEDED(hr)) hr = E_FAIL; }
    siteAttached_ = false;
    targetSite_.Reset();
    target_.Reset();
    item_.Reset();
    return hr;
}

void BreadcrumbDropTarget::clearData() noexcept { data_.Reset(); sourceEffects_ = DROPEFFECT_NONE; }

void BreadcrumbDropTarget::limitEffect(DWORD* effect,DWORD source) const noexcept {
    *effect &= (source & actionEffects) | DROPEFFECT_SCROLL;
    if (!(*effect & actionEffects)) *effect = DROPEFFECT_NONE;
}

HRESULT BreadcrumbDropTarget::route(DWORD keys,POINTL point,DWORD* effect,bool dropping) {
    ComPtr<IShellItem> destination;
    const HRESULT hit = options_.hitTest(point,&destination);
    if (FAILED(hit)) { leaveTarget(); *effect = DROPEFFECT_NONE; return hit; }
    if (hit == S_FALSE || !destination) { leaveTarget(); *effect = DROPEFFECT_NONE; return S_OK; }
    SFGAOF attributes = 0;
    HRESULT hr = destination->GetAttributes(SFGAO_FOLDER,&attributes);
    if (FAILED(hr)) { leaveTarget(); *effect = DROPEFFECT_NONE; return hr; }
    if (!(attributes & SFGAO_FOLDER)) { leaveTarget(); *effect = DROPEFFECT_NONE; return S_OK; }
    bool same = item_.Get() == destination.Get();
    if (!same && item_) {
        int order = 1;
        const HRESULT compared = item_->Compare(destination.Get(),static_cast<SICHINTF>(SICHINT_CANONICAL | SICHINT_TEST_FILESYSPATH_IF_NOT_EQUAL),&order);
        same = SUCCEEDED(compared) && order == 0;
    }
    if (!same || !target_) {
        leaveTarget();
        ComPtr<IDropTarget> provider;
        hr = options_.bindTarget ? options_.bindTarget(destination.Get(),&provider) : nativeTarget(destination.Get(),&provider);
        if (FAILED(hr)) { *effect = DROPEFFECT_NONE; return hr; }
        if (!provider) { *effect = DROPEFFECT_NONE; return hr == S_FALSE ? S_OK : E_UNEXPECTED; }
        ComPtr<IObjectWithSite> withSite;
        provider.As(&withSite);
        if (options_.site && withSite) {
            hr = withSite->SetSite(options_.site.Get());
            if (FAILED(hr)) { *effect = DROPEFFECT_NONE; return hr; }
        }
        item_ = destination;
        target_ = provider;
        targetSite_ = withSite;
        siteAttached_ = options_.site != nullptr && withSite != nullptr;
        *effect = sourceEffects_;
        hr = target_->DragEnter(data_.Get(),keys,point,effect);
        limitEffect(effect,sourceEffects_);
        if (FAILED(hr)) { leaveTarget(); *effect = DROPEFFECT_NONE; return hr; }
        if (!dropping) return hr;
    } else if (!dropping) {
        *effect = sourceEffects_;
        hr = target_->DragOver(keys,point,effect);
        limitEffect(effect,sourceEffects_);
        if (FAILED(hr)) { leaveTarget(); *effect = DROPEFFECT_NONE; }
        return hr;
    }
    *effect = sourceEffects_;
    hr = target_->Drop(data_.Get(),keys,point,effect);
    limitEffect(effect,sourceEffects_);
    if (FAILED(hr)) *effect = DROPEFFECT_NONE;
    // Drop itself owns feedback teardown. Calling DragLeave after Drop would
    // violate the native target's drag lifecycle; detach/release without it.
    if (siteAttached_ && targetSite_) targetSite_->SetSite(nullptr);
    siteAttached_ = false;
    targetSite_.Reset();
    target_.Reset();
    item_.Reset();
    return hr;
}

HRESULT BreadcrumbDropTarget::DragEnter(IDataObject* data,DWORD keys,POINTL point,DWORD* effect) {
    if (!effect) return E_POINTER;
    if (!data) { *effect = DROPEFFECT_NONE; return E_INVALIDARG; }
    const HRESULT thread = checkThread();
    if (FAILED(thread)) { *effect = DROPEFFECT_NONE; return thread; }
    if (options_.headless && !options_.bindTarget) { *effect = DROPEFFECT_NONE; return E_ACCESSDENIED; }
    SelfReference retained(this);
    try {
        leaveTarget();
        clearData();
        data_ = data;
        sourceEffects_ = *effect & actionEffects;
        return route(keys,point,effect,false);
    } catch (const std::bad_alloc&) { leaveTarget(); clearData(); *effect = DROPEFFECT_NONE; return E_OUTOFMEMORY; }
      catch (...) { leaveTarget(); clearData(); *effect = DROPEFFECT_NONE; return E_FAIL; }
}

HRESULT BreadcrumbDropTarget::DragOver(DWORD keys,POINTL point,DWORD* effect) {
    if (!effect) return E_POINTER;
    const HRESULT thread = checkThread();
    if (FAILED(thread)) { *effect = DROPEFFECT_NONE; return thread; }
    if (!data_) { *effect = DROPEFFECT_NONE; return S_OK; }
    SelfReference retained(this);
    try { return route(keys,point,effect,false); }
    catch (const std::bad_alloc&) { leaveTarget(); clearData(); *effect = DROPEFFECT_NONE; return E_OUTOFMEMORY; }
    catch (...) { leaveTarget(); clearData(); *effect = DROPEFFECT_NONE; return E_FAIL; }
}

HRESULT BreadcrumbDropTarget::DragLeave() {
    const HRESULT hr = checkThread();
    if (FAILED(hr)) return hr;
    SelfReference retained(this);
    const HRESULT result = leaveTarget();
    clearData();
    return result;
}

HRESULT BreadcrumbDropTarget::Drop(IDataObject* data,DWORD keys,POINTL point,DWORD* effect) {
    if (!effect) return E_POINTER;
    const HRESULT thread = checkThread();
    if (FAILED(thread)) { *effect = DROPEFFECT_NONE; return thread; }
    SelfReference retained(this);
    if (options_.headless || (!options_.bindTarget && (!registered_ || !IsWindow(owner_) || !IsWindowVisible(owner_)))) {
        leaveTarget(); clearData(); *effect = DROPEFFECT_NONE; return E_ACCESSDENIED;
    }
    if (!data || !data_) { leaveTarget(); clearData(); *effect = DROPEFFECT_NONE; return E_INVALIDARG; }
    try {
        ComPtr<IUnknown> originalIdentity,incomingIdentity;
        HRESULT hr = data_.As(&originalIdentity);
        if (SUCCEEDED(hr)) hr = data->QueryInterface(IID_PPV_ARGS(&incomingIdentity));
        if (FAILED(hr) || originalIdentity.Get() != incomingIdentity.Get()) {
            leaveTarget(); clearData(); *effect = DROPEFFECT_NONE;
            return FAILED(hr) ? hr : E_INVALIDARG;
        }
        hr = route(keys,point,effect,true);
        clearData();
        return hr;
    } catch (const std::bad_alloc&) { leaveTarget(); clearData(); *effect = DROPEFFECT_NONE; return E_OUTOFMEMORY; }
      catch (...) { leaveTarget(); clearData(); *effect = DROPEFFECT_NONE; return E_FAIL; }
}

} // namespace explorer
