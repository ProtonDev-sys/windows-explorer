#include "explorer/app.hpp"
#include "explorer/ui_direction.hpp"
#include "explorer/ui_strings.hpp"
#include "explorer/theme.hpp"
#include <windowsx.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

namespace explorer {
namespace {
constexpr wchar_t PreviewGripOwner[] = L"WindowsExplorer.PreviewGrip.Owner";
bool samePreviewPidl(PCIDLIST_ABSOLUTE first, PCIDLIST_ABSOLUTE second) noexcept {
    if (!first || !second) return first == second;
    const auto bytes = ILGetSize(first);
    return bytes == ILGetSize(second) && std::memcmp(first, second, bytes) == 0;
}
HRESULT previewLayoutError() noexcept {
    const auto error=GetLastError();return HRESULT_FROM_WIN32(error?error:ERROR_GEN_FAILURE);
}
bool previewOwnedWindow(HWND child,HWND root) noexcept {
    DWORD process=0;
    return child&&IsWindow(child)&&GetWindowThreadProcessId(child,&process)==GetCurrentThreadId()&&
        process==GetCurrentProcessId()&&(child==root||IsChild(root,child));
}
HRESULT previewScreenRect(HWND window,RECT& result) noexcept {
    SetLastError(ERROR_SUCCESS);return GetWindowRect(window,&result)?S_OK:previewLayoutError();
}
HRESULT previewClientScreen(HWND window,RECT& result) noexcept {
    RECT client{};SetLastError(ERROR_SUCCESS);
    if(!GetClientRect(window,&client))return previewLayoutError();
    return mapUiRect(window,nullptr,client,&result);
}
bool positivePreviewRect(const RECT& value) noexcept {return value.right>value.left&&value.bottom>value.top;}
bool containedPreviewRect(const RECT& child,const RECT& parent) noexcept {
    return positivePreviewRect(child)&&child.left>=parent.left&&child.top>=parent.top&&
        child.right<=parent.right&&child.bottom<=parent.bottom;
}
RECT previewInsets(const RECT& outer,const RECT& inner) noexcept {
    return {inner.left-outer.left,inner.top-outer.top,outer.right-inner.right,outer.bottom-inner.bottom};
}
RECT previewInsetRect(const RECT& outer,const RECT& insets) noexcept {
    return {outer.left+insets.left,outer.top+insets.top,outer.right-insets.right,outer.bottom-insets.bottom};
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
    if(message==WM_NCDESTROY) {
        // SetProp requires each application property to be removed before
        // returning from WM_NCDESTROY. This procedure retains no App pointer.
        RemovePropW(window,PreviewGripOwner);
        RemoveWindowSubclass(window,previewSurfaceProc,id);
    }
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
    if(previewGripRetiring_)return FAILED(previewGripRetirementStatus_)?previewGripRetirementStatus_:HRESULT_FROM_WIN32(ERROR_BUSY);
    if (previewPane_&&previewRender_&&previewText_&&previewGrip_) return S_OK;
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
    if(closing_)return E_ABORT;
    if(!previewGrip_) {
        const auto grip=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_CLIPSIBLINGS|SS_NOTIFY|SS_WHITERECT,
            0,0,1,1,window_,nullptr,instance_,nullptr);
        if(!grip)return previewLayoutError();
        const auto failedGrip=[&](HRESULT primary) {
            // Keep the exact provisional child until native destruction is
            // proved. No App callback has been installed on these failures.
            previewGrip_=grip;previewGripRetiring_=true;
            DWORD_PTR surface=0;
            if(!GetWindowSubclass(grip,previewSurfaceProc,0x57505256,&surface))RemovePropW(grip,PreviewGripOwner);
            SetLastError(ERROR_SUCCESS);
            const auto destroyed=DestroyWindow(grip);const auto cleanup=destroyed?S_OK:previewLayoutError();
            previewGripRetirementStatus_=cleanup;
            if(destroyed) {if(previewGrip_==grip)previewGrip_=nullptr;previewGripRetiring_=false;}
            else {shutdownStatus_=cleanup;if(SUCCEEDED(previewLayoutCleanupStatus_))previewLayoutCleanupStatus_=cleanup;}
            return primary;
        };
        SetLastError(ERROR_SUCCESS);
        if(!SetPropW(grip,PreviewGripOwner,reinterpret_cast<HANDLE>(this))) {
            const auto failed=previewLayoutError();return failedGrip(failed);
        }
        auto initialized=initializePreviewSurface(grip);
        if(SUCCEEDED(initialized)) {
            SetLastError(ERROR_SUCCESS);
            if(!SetWindowSubclass(grip,previewGripProc,reinterpret_cast<UINT_PTR>(this),reinterpret_cast<DWORD_PTR>(this)))
                initialized=previewLayoutError();
        }
        if(FAILED(initialized))return failedGrip(initialized);
        previewGrip_=grip;
    }
    return S_OK;
}

bool ExplorerApp::previewGripCurrent() const noexcept {
    const auto& record=previewLayout_;
    if(closing_||navigating_||previewLayoutActive_||previewGripRetiring_||!preferences_.previewPane||previewLayoutStatus_!=S_OK||
       !window_||!record.cropped||!record.view||record.view.Get()!=view_.Get()||!folderView_||
       record.navigation!=navigationCount_||record.dpi!=dpi_||record.navigationPane!=preferences_.navigationPane||
       record.detailsPane!=preferences_.detailsPane||!samePreviewPidl(record.location.get(),currentPidl_.get())||
       IsRectEmpty(&previewSplitter_)||GetWindowLongPtrW(window_,GWLP_USERDATA)!=reinterpret_cast<LONG_PTR>(this)||
       !previewOwnedWindow(previewGrip_,window_)||GetPropW(previewGrip_,PreviewGripOwner)!=reinterpret_cast<HANDLE>(const_cast<ExplorerApp*>(this))||
       GetAncestor(previewGrip_,GA_PARENT)!=window_||GetAncestor(previewGrip_,GA_ROOT)!=window_||!IsWindowVisible(previewGrip_)||
       !previewOwnedWindow(record.window,window_)||!previewOwnedWindow(record.parent,window_)||!previewOwnedWindow(record.frame,window_)||
       GetAncestor(record.window,GA_PARENT)!=record.parent||GetAncestor(record.frame,GA_PARENT)!=window_||
       !(record.parent==record.frame||IsChild(record.frame,record.parent)))return false;
    bool rtl=!record.rtl;RECT expected{},actual{},nativeBounds{},parentClient{};
    const POINT center{previewSplitter_.left+(previewSplitter_.right-previewSplitter_.left)/2,
        previewSplitter_.top+(previewSplitter_.bottom-previewSplitter_.top)/2};
    return windowUiDirection(window_,&rtl)==S_OK&&rtl==record.rtl&&
        mapUiRect(window_,nullptr,previewSplitter_,&expected)==S_OK&&GetWindowRect(previewGrip_,&actual)&&EqualRect(&expected,&actual)&&
        previewScreenRect(record.window,nativeBounds)==S_OK&&EqualRect(&nativeBounds,&record.crop)&&
        previewClientScreen(record.parent,parentClient)==S_OK&&EqualRect(&parentClient,&record.parentClient)&&
        ChildWindowFromPointEx(window_,center,CWP_SKIPINVISIBLE)==previewGrip_;
}

LRESULT CALLBACK ExplorerApp::previewGripProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam,
    UINT_PTR identifier,DWORD_PTR reference) {
    auto& app=*reinterpret_cast<ExplorerApp*>(reference);
    PreviewCallScope lifetime(app);
    if(message==WM_NCDESTROY) {
        RemoveWindowSubclass(window,previewGripProc,identifier);
        RemovePropW(window,PreviewGripOwner);
        if(app.previewGrip_==window) {
            const bool captured=app.previewResizing_&&GetCapture()==app.window_;
            app.previewGrip_=nullptr;app.previewSplitter_={};app.previewContentBounds_={};app.previewResizing_=false;
            if(captured)ReleaseCapture();
        }
        return DefSubclassProc(window,message,wparam,lparam);
    }
    const auto current=[&]{return window==app.previewGrip_&&app.previewGripCurrent();};
    if(message==WM_NCHITTEST)return window==app.previewGrip_&&previewOwnedWindow(window,app.window_)&&
        GetPropW(window,PreviewGripOwner)==reinterpret_cast<HANDLE>(&app)?HTCLIENT:HTTRANSPARENT;
    if(message==WM_SETCURSOR&&LOWORD(lparam)==HTCLIENT&&current()) {
        SetCursor(LoadCursorW(nullptr,IDC_SIZEWE));return TRUE;
    }
    if(message==WM_LBUTTONDOWN) {
        if(!current())return 0;
        const POINT local{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};POINT root{};
        if(mapUiPoint(window,app.window_,local,&root)!=S_OK||!current()||!PtInRect(&app.previewSplitter_,root)||
           root.x<std::numeric_limits<short>::min()||root.x>std::numeric_limits<short>::max()||
           root.y<std::numeric_limits<short>::min()||root.y>std::numeric_limits<short>::max())return 0;
        // Only the initial child click is forwarded. The existing root capture
        // then owns native move/up/cancel and the finished-drag persistence.
        return SendMessageW(app.window_,message,wparam,MAKELPARAM(root.x,root.y));
    }
    return DefSubclassProc(window,message,wparam,lparam);
}

HRESULT ExplorerApp::resetPreviewGrip() noexcept {
    PreviewCallScope lifetime(*this);
    if(previewGripRetiring_)return FAILED(previewGripRetirementStatus_)?previewGripRetirementStatus_:HRESULT_FROM_WIN32(ERROR_BUSY);
    const auto grip=previewGrip_,root=window_;
    previewSplitter_={};previewContentBounds_={};
    if(!grip)return S_FALSE;
    const auto owned=[&]{return previewGrip_==grip&&window_==root&&previewOwnedWindow(grip,root)&&
        GetAncestor(grip,GA_PARENT)==root&&GetAncestor(grip,GA_ROOT)==root&&
        GetPropW(grip,PreviewGripOwner)==reinterpret_cast<HANDLE>(this);};
    if(!owned())return E_UNEXPECTED;
    previewGripRetiring_=true;
    const auto failedRetirement=[&](HRESULT status) {
        previewGripRetirementStatus_=status;shutdownStatus_=status;
        if(SUCCEEDED(previewLayoutCleanupStatus_))previewLayoutCleanupStatus_=status;
        return status;
    };
    const bool releaseCapture=previewResizing_&&GetCapture()==root;
    previewResizing_=false;
    if(releaseCapture)ReleaseCapture();
    if(!owned())return failedRetirement(HRESULT_FROM_WIN32(ERROR_RETRY));
    ShowWindow(grip,SW_HIDE);
    if(!owned()||IsWindowVisible(grip))return failedRetirement(E_UNEXPECTED);
    DWORD_PTR reference=0;const auto identifier=reinterpret_cast<UINT_PTR>(this);
    if(!GetWindowSubclass(grip,previewGripProc,identifier,&reference)||reference!=reinterpret_cast<DWORD_PTR>(this))return failedRetirement(E_UNEXPECTED);
    SetLastError(ERROR_SUCCESS);
    if(!RemoveWindowSubclass(grip,previewGripProc,identifier))return failedRetirement(previewLayoutError());
    if(!owned())return failedRetirement(HRESULT_FROM_WIN32(ERROR_RETRY));
    // The App-free surface callback removes the owner property at native
    // WM_NCDESTROY. A failed DestroyWindow retains both the HWND and gate.
    SetLastError(ERROR_SUCCESS);
    if(!DestroyWindow(grip))return failedRetirement(previewLayoutError());
    if(previewGrip_==grip)previewGrip_=nullptr;
    previewGripRetiring_=false;previewGripRetirementStatus_=S_OK;
    return S_OK;
}

void ExplorerApp::queuePreviewLayout() noexcept {
    if(closing_||navigating_||!window_||!IsWindow(window_)||
       GetWindowLongPtrW(window_,GWLP_USERDATA)!=reinterpret_cast<LONG_PTR>(this))return;
    if(previewLayoutActive_){previewLayoutAgain_=true;return;}
    if(previewLayoutQueued_)return;
    previewLayoutQueued_=true;
    if(!PostMessageW(window_,PreviewLayout,0,0)){previewLayoutStatus_=previewLayoutError();previewLayoutQueued_=false;}
}

LRESULT CALLBACK ExplorerApp::previewLayoutProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam,
    UINT_PTR identifier,DWORD_PTR reference) {
    auto& app=*reinterpret_cast<ExplorerApp*>(reference);
    PreviewCallScope lifetime(app);
    if(message==WM_NCDESTROY) {
        RemoveWindowSubclass(window,previewLayoutProc,identifier);
        if(app.previewLayout_.window==window)app.previewLayout_.window=nullptr;
        if(app.previewLayout_.parent==window)app.previewLayout_.parent=nullptr;
    }
    const auto result=DefSubclassProc(window,message,wparam,lparam);
    // Observe public creator-owned windows only. Never change a WINDOWPOS,
    // native frame layout, or the original procedure's message parameters.
    if((message==WM_SIZE||message==WM_MOVE||message==WM_NCDESTROY)&&!app.previewLayoutActive_&&
       (window==app.previewLayout_.window||window==app.previewLayout_.parent||message==WM_NCDESTROY)&&
       app.preferences_.previewPane)app.queuePreviewLayout();
    return result;
}

HRESULT ExplorerApp::resetPreviewLayout() noexcept {
    PreviewCallScope lifetime(*this);
    const auto gripReset=resetPreviewGrip();if(FAILED(gripReset))return gripReset;
    const auto originalView=previewLayout_.view.Get();
    const auto originalWindow=previewLayout_.window,originalParent=previewLayout_.parent;
    const auto identifier=reinterpret_cast<UINT_PTR>(this);
    for(const auto native:{originalWindow,originalParent}) {
        if(!native||!IsWindow(native))continue;
        DWORD_PTR reference=0;
        if(GetWindowSubclass(native,previewLayoutProc,identifier,&reference)&&reference==reinterpret_cast<DWORD_PTR>(this)) {
            if(!previewOwnedWindow(native,window_))return E_UNEXPECTED;
            SetLastError(ERROR_SUCCESS);
            if(!RemoveWindowSubclass(native,previewLayoutProc,identifier))return previewLayoutError();
        }
    }
    if(originalView!=previewLayout_.view.Get()||originalWindow!=previewLayout_.window||
       originalParent!=previewLayout_.parent)return HRESULT_FROM_WIN32(ERROR_RETRY);
    auto retired=std::move(previewLayout_);previewLayout_={};
    previewSplitter_={};previewContentBounds_={};
    return S_OK;
}

HRESULT ExplorerApp::layoutPreviewPane() {
    PreviewCallScope lifetime(*this);
    if(previewGripRetiring_)return FAILED(previewGripRetirementStatus_)?previewGripRetirementStatus_:HRESULT_FROM_WIN32(ERROR_BUSY);
    previewSplitter_={};previewContentBounds_={};
    previewLayoutCleanupStatus_=S_FALSE;
    if(closing_||navigating_||!browser_||!view_||!folderView_||!currentPidl_||
       (!preferences_.previewPane&&!previewLayout_.view)) {
        if(!closing_&&previewPane_)ShowWindow(previewPane_,SW_HIDE);
        return S_FALSE;
    }
    const auto browser=browser_;const auto nativeView=view_;const auto folder=folderView_;
    const auto navigation=navigationCount_;const auto dpi=dpi_;
    const auto enabled=preferences_.previewPane,details=preferences_.detailsPane,nav=preferences_.navigationPane;
    const auto width=preferences_.previewWidth;
    Pidl location(ILCloneFull(currentPidl_.get()));if(!location)return E_OUTOFMEMORY;
    const auto current=[&] {
        return !closing_&&!navigating_&&window_&&IsWindow(window_)&&
            GetWindowLongPtrW(window_,GWLP_USERDATA)==reinterpret_cast<LONG_PTR>(this)&&
            browser.Get()==browser_.Get()&&nativeView.Get()==view_.Get()&&folder.Get()==folderView_.Get()&&
            navigation==navigationCount_&&dpi==dpi_&&enabled==preferences_.previewPane&&
            details==preferences_.detailsPane&&nav==preferences_.navigationPane&&width==preferences_.previewWidth&&
            samePreviewPidl(location.get(),currentPidl_.get());
    };
    struct FailureHide {
        ExplorerApp& owner;const decltype(current)& valid;bool committed=false;
        ~FailureHide(){if(!committed){
            if(valid()){owner.previewSplitter_={};owner.previewContentBounds_={};
                const auto gripReset=owner.resetPreviewGrip();
                if(FAILED(gripReset)&&SUCCEEDED(owner.previewLayoutCleanupStatus_))owner.previewLayoutCleanupStatus_=gripReset;
                if(valid()&&owner.previewPane_)ShowWindow(owner.previewPane_,SW_HIDE);}
            else owner.queuePreviewLayout();}}
    } failure{*this,current};
    const auto retry=HRESULT_FROM_WIN32(ERROR_RETRY);
    HWND native=nullptr;auto read=nativeView->GetWindow(&native);
    if(read!=S_OK)return read;if(!current())return retry;
    if(!previewOwnedWindow(native,window_)||native==window_)return E_UNEXPECTED;
    ComPtr<IUnknown> expected,actual;
    read=nativeView.As(&expected);if(read!=S_OK)return read;if(!current())return retry;
    read=browser->GetCurrentView(IID_PPV_ARGS(&actual));if(read!=S_OK)return read;
    if(!actual||!current()||expected.Get()!=actual.Get())return retry;
    const auto parent=GetAncestor(native,GA_PARENT);
    if(!previewOwnedWindow(parent,window_)||parent==window_)return E_UNEXPECTED;
    HWND frame=parent;unsigned ancestors=0;
    while(GetAncestor(frame,GA_PARENT)!=window_) {
        if(++ancestors>16)return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        frame=GetAncestor(frame,GA_PARENT);
        if(!previewOwnedWindow(frame,window_)||frame==window_)return E_UNEXPECTED;
    }
    bool rtl=false;read=windowUiDirection(window_,&rtl);if(read!=S_OK)return read;
    RECT parentClient{},frameClient{},nativeBounds{},rootClient{};
    read=previewClientScreen(parent,parentClient);if(read!=S_OK)return read;
    read=previewClientScreen(frame,frameClient);if(read!=S_OK)return read;
    read=previewScreenRect(native,nativeBounds);if(read!=S_OK)return read;
    read=previewClientScreen(window_,rootClient);if(read!=S_OK)return read;
    const auto tupleCurrent=[&] {
        bool direction=false;RECT actualParent{},actualFrame{},actualRoot{};
        return current()&&previewOwnedWindow(native,window_)&&previewOwnedWindow(parent,window_)&&
            previewOwnedWindow(frame,window_)&&GetAncestor(native,GA_PARENT)==parent&&
            GetAncestor(frame,GA_PARENT)==window_&&(parent==frame||IsChild(frame,parent))&&
            windowUiDirection(window_,&direction)==S_OK&&direction==rtl&&
            previewClientScreen(parent,actualParent)==S_OK&&EqualRect(&actualParent,&parentClient)&&
            previewClientScreen(frame,actualFrame)==S_OK&&EqualRect(&actualFrame,&frameClient)&&
            previewClientScreen(window_,actualRoot)==S_OK&&EqualRect(&actualRoot,&rootClient);
    };
    if(!tupleCurrent()||!containedPreviewRect(parentClient,frameClient))return retry;
    const bool sameWindow=previewLayout_.view.Get()==nativeView.Get()&&previewLayout_.window==native&&
        previewLayout_.parent==parent&&previewLayout_.frame==frame;
    const bool sameConfiguration=sameWindow&&previewLayout_.navigation==navigation&&previewLayout_.dpi==dpi&&
        previewLayout_.rtl==rtl&&previewLayout_.navigationPane==nav&&previewLayout_.detailsPane==details&&
        samePreviewPidl(previewLayout_.location.get(),location.get());
    RECT full=nativeBounds;
    if(sameWindow&&previewLayout_.cropped) {
        if(previewLayout_.parentSlot)full=parentClient;
        else {
            const auto expectedFull=previewInsetRect(parentClient,previewLayout_.nativeInsets);
            const bool unchangedSize=parentClient.right-parentClient.left==previewLayout_.parentClient.right-previewLayout_.parentClient.left&&
                parentClient.bottom-parentClient.top==previewLayout_.parentClient.bottom-previewLayout_.parentClient.top;
            if(unchangedSize||EqualRect(&nativeBounds,&expectedFull))full=expectedFull;
            else return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        }
        // Nonfilling providers may reuse measured insets only at unchanged
        // size or after the actual native view confirms the resized full rect.
    }
    if(!containedPreviewRect(full,parentClient))return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    const auto movePublic=[&](const RECT& screen) {
        if(!tupleCurrent())return retry;
        RECT local{};auto status=mapUiRect(nullptr,parent,screen,&local);if(status!=S_OK)return status;
        SetLastError(ERROR_SUCCESS);
        if(!SetWindowPos(native,nullptr,local.left,local.top,local.right-local.left,local.bottom-local.top,
            SWP_NOACTIVATE|SWP_NOZORDER|SWP_NOOWNERZORDER))return previewLayoutError();
        if(!tupleCurrent())return retry;
        RECT actualBounds{};status=previewScreenRect(native,actualBounds);
        return status==S_OK?(EqualRect(&actualBounds,&screen)?S_OK:E_UNEXPECTED):status;
    };
    bool needsRestore=sameWindow&&previewLayout_.cropped;
    bool observedNative=false,observedParent=false;
    const auto restoreFailure=[&] {
        if(failure.committed)return;
        if(!tupleCurrent()){previewLayoutCleanupStatus_=retry;queuePreviewLayout();return;}
        // Preserve the primary layout HRESULT. A failed pane/renderer update
        // must not leave this exact current public view beside an empty gutter.
        if(needsRestore) {
            previewLayoutCleanupStatus_=movePublic(full);
            if(!tupleCurrent()){queuePreviewLayout();return;}
            if(previewLayoutCleanupStatus_==S_OK&&previewLayout_.window==native&&previewLayout_.parent==parent&&
               previewLayout_.view.Get()==nativeView.Get())previewLayout_.cropped=false;
        }
        const auto identifier=reinterpret_cast<UINT_PTR>(this);
        for(const auto observed:{observedNative?native:nullptr,observedParent?parent:nullptr}) {
            if(!observed)continue;
            DWORD_PTR reference=0;
            if(GetWindowSubclass(observed,previewLayoutProc,identifier,&reference)&&
               reference==reinterpret_cast<DWORD_PTR>(this)) {
                if(!tupleCurrent()){queuePreviewLayout();return;}
                SetLastError(ERROR_SUCCESS);
                if(!RemoveWindowSubclass(observed,previewLayoutProc,identifier)) {
                    const auto removed=previewLayoutError();
                    if(SUCCEEDED(previewLayoutCleanupStatus_))previewLayoutCleanupStatus_=removed;
                    return;
                }
                if(previewLayoutCleanupStatus_==S_FALSE)previewLayoutCleanupStatus_=S_OK;
            }
        }
    };
    struct RestoreFailure {const decltype(restoreFailure)& cleanup;~RestoreFailure(){cleanup();}} restore{restoreFailure};
    if(!sameConfiguration) {
        read=resetPreviewLayout();if(read!=S_OK)return read;if(!tupleCurrent())return retry;
        PreviewLayoutRecord candidate;
        candidate.view=nativeView;candidate.window=native;candidate.parent=parent;candidate.frame=frame;
        candidate.location.reset(ILCloneFull(location.get()));if(!candidate.location)return E_OUTOFMEMORY;
        candidate.navigation=navigation;candidate.dpi=dpi;candidate.rtl=rtl;
        candidate.navigationPane=nav;candidate.detailsPane=details;
        candidate.parentSlot=EqualRect(&full,&parentClient)!=FALSE;
        candidate.nativeInsets=previewInsets(parentClient,full);candidate.parentClient=parentClient;
        candidate.uncropped=full;
        if(!tupleCurrent())return retry;
        previewLayout_=std::move(candidate);
    }
    if(!enabled) {
        read=movePublic(full);if(read!=S_OK)return read;
        previewLayout_.cropped=false;
        if(previewPane_)ShowWindow(previewPane_,SW_HIDE);
        if(!tupleCurrent())return retry;
        read=resetPreviewLayout();if(read==S_OK)failure.committed=true;
        return read;
    }
    read=createPreviewPane();if(read!=S_OK)return read;if(!tupleCurrent())return retry;
    RECT logical{};read=mapUiRect(nullptr,window_,full,&logical);if(read!=S_OK)return read;
    const int available=static_cast<int>(logical.right-logical.left);
    if(available<px(120)+px(4)+1)return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    const int paneWidth=std::clamp(px(width),px(120),std::max(px(120),available/2));
    const RECT pane{logical.right-paneWidth,logical.top,logical.right,logical.bottom};
    const RECT grip{pane.left-px(4),logical.top,pane.left,logical.bottom};
    const RECT content{logical.left,logical.top,grip.left,logical.bottom};
    RECT crop{};read=mapUiRect(window_,nullptr,content,&crop);if(read!=S_OK)return read;
    const auto identifier=reinterpret_cast<UINT_PTR>(this);
    for(const auto observed:{native,parent}) {
        SetLastError(ERROR_SUCCESS);
        if(!SetWindowSubclass(observed,previewLayoutProc,identifier,reinterpret_cast<DWORD_PTR>(this)))return previewLayoutError();
        if(observed==native)observedNative=true;else observedParent=true;
        if(!tupleCurrent())return retry;
    }
    needsRestore=true;
    read=movePublic(crop);if(read!=S_OK)return read;
    previewLayout_.parentClient=parentClient;previewLayout_.uncropped=full;previewLayout_.crop=crop;previewLayout_.cropped=true;
    const int height=static_cast<int>(pane.bottom-pane.top);
    const auto ownedPane=previewPane_,ownedGrip=previewGrip_,ownedRender=previewRender_,ownedText=previewText_;
    const auto ownedSurfaces=[&]{return tupleCurrent()&&ownedPane==previewPane_&&ownedGrip==previewGrip_&&
        ownedRender==previewRender_&&ownedText==previewText_&&
        previewOwnedWindow(ownedPane,window_)&&previewOwnedWindow(ownedGrip,window_)&&
        previewOwnedWindow(ownedRender,window_)&&previewOwnedWindow(ownedText,window_)&&
        GetAncestor(ownedPane,GA_PARENT)==window_&&GetAncestor(ownedGrip,GA_PARENT)==window_&&
        GetAncestor(ownedRender,GA_PARENT)==ownedPane&&GetAncestor(ownedText,GA_PARENT)==ownedPane&&
        GetAncestor(ownedPane,GA_ROOT)==window_&&GetAncestor(ownedGrip,GA_ROOT)==window_&&
        GetPropW(ownedGrip,PreviewGripOwner)==reinterpret_cast<HANDLE>(this);};
    if(!ownedSurfaces())return retry;
    SetLastError(ERROR_SUCCESS);
    if(!SetWindowPos(ownedPane,HWND_TOP,pane.left,pane.top,paneWidth,height,SWP_NOACTIVATE|SWP_NOOWNERZORDER))return previewLayoutError();
    if(!ownedSurfaces()||GetTopWindow(window_)!=ownedPane)return retry;
    SetLastError(ERROR_SUCCESS);
    if(!SetWindowPos(ownedGrip,HWND_TOP,grip.left,grip.top,grip.right-grip.left,height,
        SWP_NOACTIVATE|SWP_NOOWNERZORDER|SWP_SHOWWINDOW))return previewLayoutError();
    if(!ownedSurfaces()||GetTopWindow(window_)!=ownedGrip)return retry;
    if(!MoveWindow(ownedRender,0,0,paneWidth,height,TRUE))return previewLayoutError();
    if(!ownedSurfaces())return retry;
    if(!MoveWindow(ownedText,px(12),std::max(0,height/2-px(10)),std::max(1,paneWidth-px(24)),px(40),TRUE))return previewLayoutError();
    if(!ownedSurfaces())return retry;
    ShowWindow(ownedPane,SW_SHOWNA);if(!ownedSurfaces())return retry;
    const auto manager=previewHost_.get();const auto epoch=previewEpoch_;
    if(manager) {
        read=updatePreviewVisuals();if(!ownedSurfaces()||manager!=previewHost_.get()||epoch!=previewEpoch_)return retry;
        if(FAILED(read))return read;
        RECT client{};if(!GetClientRect(ownedRender,&client))return previewLayoutError();
        read=manager->resize(ownedRender,client);
        if(!ownedSurfaces()||manager!=previewHost_.get()||epoch!=previewEpoch_)return retry;
        if(FAILED(read))return read;
    }
    const POINT panePoint{pane.left+(pane.right-pane.left)/2,pane.top+(pane.bottom-pane.top)/2};
    const POINT gripPoint{grip.left+(grip.right-grip.left)/2,grip.top+(grip.bottom-grip.top)/2};
    if(!ownedSurfaces())return retry;
    if(ChildWindowFromPointEx(window_,panePoint,CWP_SKIPINVISIBLE)!=ownedPane||
       ChildWindowFromPointEx(window_,gripPoint,CWP_SKIPINVISIBLE)!=ownedGrip)return E_UNEXPECTED;
    previewContentBounds_=logical;previewSplitter_=grip;failure.committed=true;
    return S_OK;
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
    const auto detached=resetPreviewLayout();if(FAILED(detached)){shutdownStatus_=detached;return detached;}
    if (previewChangeCookie_) { SHChangeNotifyDeregister(previewChangeCookie_); previewChangeCookie_ = 0; }
    invalidatePreview(PreviewEmptyReason::Disabled);
    if (!previewHost_) return S_OK;
    const auto stopped = previewHost_->drain(5000);
    if (FAILED(stopped)) { shutdownStatus_ = stopped; return stopped; }
    previewHost_.reset();
    return S_OK;
}
} // namespace explorer
