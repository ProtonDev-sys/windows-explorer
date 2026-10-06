#include "explorer/app.hpp"
#include "explorer/ui_direction.hpp"
#include "explorer/ui_strings.hpp"
#include "explorer/theme.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

namespace explorer {
namespace {
bool samePreviewPidl(PCIDLIST_ABSOLUTE first, PCIDLIST_ABSOLUTE second) noexcept {
    if (!first || !second) return first == second;
    const auto bytes = ILGetSize(first);
    return bytes == ILGetSize(second) && std::memcmp(first, second, bytes) == 0;
}
LRESULT CALLBACK previewSurfaceProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam,
                                    UINT_PTR id,DWORD_PTR) {
    if(message==WM_CTLCOLORSTATIC) {
        const auto child=reinterpret_cast<HWND>(lparam);
        if(GetParent(child)==window&&GetWindowThreadProcessId(child,nullptr)==GetCurrentThreadId()) {
            const auto palette=themePalette();const auto dc=reinterpret_cast<HDC>(wparam);
            SetTextColor(dc,palette.foreground(ThemeSurface::Content));
            SetBkColor(dc,palette.surface(ThemeSurface::Content));SetBkMode(dc,TRANSPARENT);
            return reinterpret_cast<LRESULT>(themeBrush(ThemeSurface::Content));
        }
    }
    if(message==WM_PAINT) {
        PAINTSTRUCT paint{};const auto dc=BeginPaint(window,&paint);
        if(dc)FillRect(dc,&paint.rcPaint,themeBrush(ThemeSurface::Content));
        EndPaint(window,&paint);return 0;
    }
    if(message==WM_ERASEBKGND||message==WM_PRINTCLIENT) {
        RECT bounds{};
        if(GetClientRect(window,&bounds))FillRect(reinterpret_cast<HDC>(wparam),&bounds,themeBrush(ThemeSurface::Content));
        return message==WM_ERASEBKGND?TRUE:0;
    }
    if(message==WM_NCDESTROY)RemoveWindowSubclass(window,previewSurfaceProc,id);
    return DefSubclassProc(window,message,wparam,lparam);
}
HRESULT initializePreviewSurface(HWND window) {
    if(!SetWindowSubclass(window,previewSurfaceProc,0x57505256,0)) {
        const auto error=GetLastError();return HRESULT_FROM_WIN32(error?error:ERROR_GEN_FAILURE);
    }
    return applyWindowTheme(window);
}
}

ExplorerApp::PreviewCallScope::PreviewCallScope(ExplorerApp& value) noexcept : owner(value) {
    if(!owner.destroying_) {owner.AddRef();retained=true;}
    ++owner.previewCallsActive_;
}
ExplorerApp::PreviewCallScope::~PreviewCallScope() {
    if(!--owner.previewCallsActive_&&owner.previewClosePending_&&!owner.destroying_&&owner.window_&&
       IsWindow(owner.window_)&&GetWindowLongPtrW(owner.window_,GWLP_USERDATA)==reinterpret_cast<LONG_PTR>(&owner)) {
        if(!PostMessageW(owner.window_,WM_CLOSE,0,0)) {
            const auto error=GetLastError();
            owner.shutdownStatus_=HRESULT_FROM_WIN32(error?error:ERROR_GEN_FAILURE);
            PostQuitMessage(8);
        }
    }
    if(retained)owner.Release();
}

HRESULT ExplorerApp::createPreviewPane() {
    if (previewPane_&&previewRender_&&previewText_) return S_OK;
    PreviewCallScope lifetime(*this);
    if(previewPaneCreating_)return HRESULT_FROM_WIN32(ERROR_BUSY);
    previewPaneCreating_=true;
    struct Creating {bool& value;~Creating(){value=false;}} creating{previewPaneCreating_};
    UiString name,select,unavailable;
    auto loaded=loadUiString(UiText::PreviewPaneName,&name);
    if(SUCCEEDED(loaded))loaded=loadUiString(UiText::PreviewSelectFile,&select);
    if(SUCCEEDED(loaded))loaded=loadUiString(UiText::PreviewUnavailable,&unavailable);
    if(FAILED(loaded))return loaded;
    previewSelectText_=std::move(select.text);previewUnavailableText_=std::move(unavailable.text);
    if(!previewPane_) {
        previewPane_ = CreateWindowExW(0, L"STATIC", name.text.c_str(),
        WS_CHILD | WS_CLIPCHILDREN | SS_WHITERECT, 0, 0, 1, 1, window_,
        nullptr, instance_, nullptr);
        if (!previewPane_) {const auto error=GetLastError();return HRESULT_FROM_WIN32(error?error:ERROR_GEN_FAILURE);}
        const auto surface=initializePreviewSurface(previewPane_);
        if(FAILED(surface)){const auto failed=previewPane_;previewPane_=nullptr;DestroyWindow(failed);return surface;}
    }
    if(closing_)return E_ABORT;
    if(!previewRender_) {
        previewRender_ = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_CLIPCHILDREN | SS_WHITERECT, 0, 0, 1, 1, previewPane_,
        nullptr, instance_, nullptr);
        if(!previewRender_) {const auto error=GetLastError();return HRESULT_FROM_WIN32(error?error:ERROR_GEN_FAILURE);}
        const auto surface=initializePreviewSurface(previewRender_);
        if(FAILED(surface)){const auto failed=previewRender_;previewRender_=nullptr;DestroyWindow(failed);return surface;}
    }
    if(closing_)return E_ABORT;
    if(!previewText_)previewText_ = CreateWindowExW(0, L"STATIC", previewSelectText_.c_str(),
        WS_CHILD | WS_VISIBLE | SS_CENTER, 0, 0, 1, 1, previewPane_,
        nullptr, instance_, nullptr);
    if (!previewText_) {const auto error=GetLastError();return HRESULT_FROM_WIN32(error?error:ERROR_GEN_FAILURE);}
    applyWindowTheme(previewText_);
    SendMessageW(previewText_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    return S_OK;
}

void ExplorerApp::layoutPreviewPane(RECT& browserBounds) {
    previewSplitter_ = {};
    if (!preferences_.previewPane) {
        if (previewPane_) ShowWindow(previewPane_, SW_HIDE);
        return;
    }
    if (FAILED(createPreviewPane())) return;
    const int available = static_cast<int>(browserBounds.right - browserBounds.left);
    const auto paneWidth = std::clamp(px(preferences_.previewWidth), px(120), std::max(px(120), available / 2));
    const auto left = browserBounds.right - paneWidth;
    const int height = std::max(1, static_cast<int>(browserBounds.bottom - browserBounds.top));
    previewSplitter_ = {left - px(4), browserBounds.top, left, browserBounds.bottom};
    browserBounds.right = std::max(browserBounds.left + 1, previewSplitter_.left);
    MoveWindow(previewPane_, left, browserBounds.top, paneWidth, height, TRUE);
    MoveWindow(previewRender_, 0, 0, paneWidth, height, TRUE);
    MoveWindow(previewText_, px(12), std::max(0, static_cast<int>(height / 2) - px(10)),
        std::max(1, paneWidth - px(24)), px(40), TRUE);
    ShowWindow(previewPane_, SW_SHOWNA);
    if (previewHost_) {
        updatePreviewVisuals();
        RECT client{};
        if (GetClientRect(previewRender_, &client)) previewHost_->resize(previewRender_, client);
    }
}

HRESULT ExplorerApp::updatePreviewVisuals() {
    if(!previewHost_||closing_)return S_FALSE;
    PreviewCallScope lifetime(*this);
    const auto palette=themePalette();
    PreviewVisualSuggestions values;
    values.background=palette.surface(ThemeSurface::Content);values.backgroundPresent=true;
    values.text=palette.foreground(ThemeSurface::Content);values.textPresent=true;
    values.fontPresent=font_&&GetObjectW(font_,sizeof(values.font),&values.font)==sizeof(values.font);
    return previewHost_->suggestVisuals(values);
}

std::uint64_t ExplorerApp::invalidatePreview(PreviewEmptyReason reason) noexcept {
    PreviewCallScope lifetime(*this);
    previewDirty_ = true;
    // Fence first: ShowWindow and COM disposal may dispatch newer callbacks.
    if (previewEpoch_ != std::numeric_limits<std::uint64_t>::max()) ++previewEpoch_;
    const auto epoch=previewEpoch_;
    previewTicket_.epoch=0;
    auto retired=std::move(previewTicket_);
    previewTicket_ = {};
    if (previewHost_) previewHost_->clear(previewEpoch_, reason);
    else if (previewRender_) ShowWindow(previewRender_, SW_HIDE);
    if (!closing_&&previewEpoch_==epoch&&previewText_) {
        SetWindowTextW(previewText_, reason == PreviewEmptyReason::None || reason == PreviewEmptyReason::Disabled ?
            previewSelectText_.c_str() : previewUnavailableText_.c_str());
        if(previewEpoch_==epoch)ShowWindow(previewText_, SW_SHOWNA);
    }
    return epoch;
}

bool ExplorerApp::previewSourceCurrent(bool readNative) {
    const auto epoch = previewTicket_.epoch;
    const auto basic = [&] {
        return epoch && epoch == previewEpoch_ && previewTicket_.epoch == epoch &&
            !closing_ && !navigating_ && preferences_.previewPane &&
            previewTicket_.view.Get() == view_.Get() && previewTicket_.folderView.Get() == folderView_.Get() &&
            previewTicket_.navigation == navigationCount_ && previewTicket_.revision == commandSourceRevision_ &&
            samePreviewPidl(previewTicket_.location.get(), currentPidl_.get());
    };
    if (!basic()) return false;
    if (!readNative) return true;
    // Keep the accepted native view and PIDL alive through reentrant reads.
    const auto nativeView = previewTicket_.folderView;
    Pidl item(ILCloneFull(previewTicket_.item.get()));
    if (!nativeView || !item) return false;
    ComPtr<IShellItemArray> selected;
    DWORD count = 0;
    if (nativeView->GetSelection(FALSE, &selected) != S_OK || !selected || !basic()) return false;
    if (selected->GetCount(&count) != S_OK || count != 1 || !basic()) return false;
    ComPtr<IShellItem> actual;
    if (selected->GetItemAt(0, &actual) != S_OK || !actual || !basic()) return false;
    PIDLIST_ABSOLUTE raw = nullptr;
    const auto read = SHGetIDListFromObject(actual.Get(), &raw);
    Pidl actualId(raw);
    return read == S_OK && actualId && basic() && samePreviewPidl(item.get(), actualId.get());
}

void ExplorerApp::updatePreviewTarget() {
    if (!previewDirty_ || closing_ || navigating_ || namespaceDirty_ || selectionStateDirty_) return;
    if (!preferences_.previewPane) { previewDirty_ = false; return; }
    PreviewCallScope lifetime(*this);
    if (!commandSelectionIdentities_ || commandSelectionView_ != view_.Get()) return;
    if (commandSelectionIdentities_->size() != 1 || (commandSelectionAttributes_ & SFGAO_FOLDER)) {
        const auto reason = commandSelectionIdentities_->empty() ? PreviewEmptyReason::None :
            commandSelectionIdentities_->size() > 1 ? PreviewEmptyReason::MultipleSelection : PreviewEmptyReason::Folder;
        const auto cleared=invalidatePreview(reason);
        if(previewEpoch_==cleared)previewDirty_ = false;
        return;
    }
    try {
        if (FAILED(createPreviewPane())) return;
        if(closing_||navigating_||namespaceDirty_||selectionStateDirty_||!preferences_.previewPane||
           !commandSelectionIdentities_||commandSelectionIdentities_->size()!=1||commandSelectionView_!=view_.Get())return;
        PreviewTicket ticket;
        ticket.view = view_; ticket.folderView = folderView_;
        ticket.location.reset(ILCloneFull(currentPidl_.get()));
        ticket.item.reset(ILCloneFull(commandSelectionIdentities_->front().get()));
        ticket.navigation = navigationCount_; ticket.revision = commandSourceRevision_;
        if (!ticket.location || !ticket.item) return;
        if (previewEpoch_ == std::numeric_limits<std::uint64_t>::max()) return;
        ticket.epoch = ++previewEpoch_;
        previewTicket_.epoch=0;
        auto retired=std::move(previewTicket_);
        previewTicket_={};
        previewTicket_ = std::move(ticket);
        const auto epoch = previewEpoch_;
        if (!previewSourceCurrent(true)||previewEpoch_!=epoch) return;
        if (!previewHost_) {
            PreviewHostCallbacks callbacks;
            callbacks.notifyWindow = window_; callbacks.notifyMessage = PreviewResult;
            callbacks.translateAccelerator = [this](std::uint64_t accepted, const MSG& message) {
                return previewAccelerator(accepted, message);
            };
            std::unique_ptr<NativePreviewHost> host;
            const auto created = NativePreviewHost::create(previewRender_, std::move(callbacks), &host);
            if(previewEpoch_!=epoch||!previewSourceCurrent(false)) {
                if(host) {
                    const auto stopped=host->drain(5000);
                    if(FAILED(stopped)) {previewHost_=std::move(host);shutdownStatus_=stopped;closing_=true;PostQuitMessage(8);}
                }
                return;
            }
            previewHost_=std::move(host);
            if (FAILED(created)) {
                const auto cleared=invalidatePreview(PreviewEmptyReason::NoAssociation);
                if(previewEpoch_==cleared)previewDirty_ = false;
                return;
            }
        }
        if (previewEpoch_ != epoch || !previewSourceCurrent(true)||previewEpoch_!=epoch) return;
        RECT bounds{};
        if (!GetClientRect(previewRender_, &bounds)) return;
        previewHost_->resize(previewRender_, bounds);
        updatePreviewVisuals();
        if (previewEpoch_ != epoch || !previewSourceCurrent(false)) return;
        Pidl source(ILCloneFull(previewTicket_.item.get()));
        if(!source)return;
        const auto accepted = previewHost_->update(source.get(), epoch);
        if (previewEpoch_ != epoch) return;
        previewDirty_ = false;
        if (SUCCEEDED(accepted) && previewText_) { SetWindowTextW(previewText_, L""); ShowWindow(previewText_, SW_SHOWNA); }
        else if(previewText_) {SetWindowTextW(previewText_,previewUnavailableText_.c_str());ShowWindow(previewText_,SW_SHOWNA);}
    } catch (const std::bad_alloc&) {
        const auto cleared=invalidatePreview(PreviewEmptyReason::NoAssociation);
        if(previewEpoch_==cleared)previewDirty_ = false;
    }
}

void ExplorerApp::pollPreview() {
    if (!previewHost_ || closing_) return;
    PreviewCallScope lifetime(*this);
    const auto status = previewHost_->status();
    if (status.requestedEpoch != previewEpoch_ || !previewSourceCurrent(true) || status.requestedEpoch != previewEpoch_) return;
    const auto epoch = previewEpoch_;
    if (status.ready && status.activeEpoch == epoch) {
        if (previewHost_->showCurrent(epoch) == S_OK && previewEpoch_ == epoch && previewSourceCurrent(true)&&previewEpoch_==epoch)
            ShowWindow(previewText_, SW_HIDE);
        else if (previewEpoch_ == epoch) invalidatePreview();
    } else if (!status.pending && (status.stage == PreviewHostStage::Failed ||
        (status.stage == PreviewHostStage::Stopped && FAILED(status.result)) || status.emptyReason != PreviewEmptyReason::None)) {
        SetWindowTextW(previewText_, previewUnavailableText_.c_str()); ShowWindow(previewText_, SW_SHOWNA);
    }
}

HRESULT ExplorerApp::previewAccelerator(std::uint64_t epoch, const MSG& message) {
    PreviewCallScope lifetime(*this);
    if (epoch != previewEpoch_ || !previewSourceCurrent(true) || epoch != previewEpoch_) return E_ABORT;
    const auto status=previewHost_?previewHost_->status():PreviewHostStatus{};
    if(!status.ready||status.activeEpoch!=epoch||!status.sessionWindow||
       !message.hwnd||(message.hwnd!=status.sessionWindow&&!IsChild(status.sessionWindow,message.hwnd)))return S_FALSE;
    if (message.message != WM_KEYDOWN && message.message != WM_SYSKEYDOWN) return S_FALSE;
    if (message.wParam == VK_F6 && !(GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000))
        return cycleFocus((GetKeyState(VK_SHIFT) & 0x8000) != 0);
    // Preview-local text chords remain with the native handler. Creator
    // GetFocus cannot classify controls on the surrogate's message queue.
    const auto command=shortcutCommand(static_cast<UINT>(message.wParam),
        (GetKeyState(VK_CONTROL)&0x8000)!=0,(GetKeyState(VK_SHIFT)&0x8000)!=0,
        (GetKeyState(VK_MENU)&0x8000)!=0,true);
    if(!command||(*command!=Back&&*command!=Forward&&*command!=Up&&*command!=Address&&
        *command!=FocusSearch&&*command!=FocusNext&&*command!=FocusPrevious&&*command!=Close&&
        *command!=Refresh&&*command!=Collapse&&*command!=Fullscreen&&*command!=NewWindow&&
        *command!=PreviewPane&&*command!=DetailsPane&&*command!=AddressList))return S_FALSE;
    if(epoch!=previewEpoch_||!previewSourceCurrent(false))return E_ABORT;
    const auto result=execute(*command);
    return SUCCEEDED(result)?S_OK:result;
}

bool ExplorerApp::previewHasFocus() const {
    if(!previewHost_||!preferences_.previewPane||closing_)return false;
    const auto status=previewHost_->status();
    if(!status.ready||status.activeEpoch!=previewEpoch_||!status.sessionWindow||
       !IsChild(previewRender_,status.sessionWindow))return false;
    struct FocusRead {HWND parent;bool found=false;} read{status.sessionWindow};
    const auto observe=[](HWND window,FocusRead& result) {
        const auto thread=GetWindowThreadProcessId(window,nullptr);
        GUITHREADINFO gui{sizeof(gui)};
        if(thread&&GetGUIThreadInfo(thread,&gui)&&gui.hwndFocus&&
           (gui.hwndFocus==result.parent||IsChild(result.parent,gui.hwndFocus)))result.found=true;
    };
    observe(status.sessionWindow,read);
    EnumChildWindows(status.sessionWindow,[](HWND child,LPARAM parameter)->BOOL {
        auto& result=*reinterpret_cast<FocusRead*>(parameter);
        const auto thread=GetWindowThreadProcessId(child,nullptr);GUITHREADINFO gui{sizeof(gui)};
        if(thread&&GetGUIThreadInfo(thread,&gui)&&gui.hwndFocus&&
           (gui.hwndFocus==result.parent||IsChild(result.parent,gui.hwndFocus)))result.found=true;
        return !result.found;
    },reinterpret_cast<LPARAM>(&read));
    return read.found;
}

void ExplorerApp::registerPreviewChanges() {
    if (previewChangeCookie_) { SHChangeNotifyDeregister(previewChangeCookie_); previewChangeCookie_ = 0; }
    if (closing_ || !preferences_.previewPane || !window_ || !currentPidl_) return;
    SHChangeNotifyEntry entry{currentPidl_.get(), FALSE};
    previewChangeCookie_ = SHChangeNotifyRegister(window_, SHCNRF_ShellLevel | SHCNRF_InterruptLevel | SHCNRF_NewDelivery,
        SHCNE_RENAMEITEM | SHCNE_DELETE | SHCNE_UPDATEITEM | SHCNE_ATTRIBUTES | SHCNE_ASSOCCHANGED |
        SHCNE_UPDATEDIR | SHCNE_RMDIR | SHCNE_RENAMEFOLDER, PreviewChange, 1, &entry);
}

void ExplorerApp::previewChanged(WPARAM wparam, LPARAM lparam) {
    PIDLIST_ABSOLUTE* identities = nullptr; LONG event = 0;
    const auto lock = SHChangeNotification_Lock(reinterpret_cast<HANDLE>(wparam), static_cast<DWORD>(lparam), &identities, &event);
    if (!lock) return;
    // Copy while locked; no callback uses notification-owned pointers later.
    Pidl first(identities && identities[0] ? ILCloneFull(identities[0]) : nullptr);
    Pidl second(identities && identities[1] ? ILCloneFull(identities[1]) : nullptr);
    SHChangeNotification_Unlock(lock);
    if (closing_ || !preferences_.previewPane) return;
    const auto related = [&](PCIDLIST_ABSOLUTE id) {
        return id && currentPidl_ && (ILIsEqual(id, currentPidl_.get()) || ILIsParent(currentPidl_.get(), id, TRUE));
    };
    if ((event & SHCNE_ASSOCCHANGED) || related(first.get()) || related(second.get())) {
        ++commandSourceRevision_; invalidatePreview();
        namespaceDirty_ = selectionStateDirty_ = true; scheduleDeferredUpdate();
    }
}

HRESULT ExplorerApp::shutdownPreview() noexcept {
    if(previewCallsActive_) {shutdownStatus_=HRESULT_FROM_WIN32(ERROR_BUSY);return shutdownStatus_;}
    closing_=true;
    if (previewChangeCookie_) { SHChangeNotifyDeregister(previewChangeCookie_); previewChangeCookie_ = 0; }
    invalidatePreview(PreviewEmptyReason::Disabled);
    if (!previewHost_) return S_OK;
    const auto stopped = previewHost_->drain(5000);
    if (FAILED(stopped)) { shutdownStatus_ = stopped; return stopped; }
    previewHost_.reset();
    return S_OK;
}
} // namespace explorer
