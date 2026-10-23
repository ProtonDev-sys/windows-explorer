#include "explorer/modern_window.hpp"
#include "explorer/theme.hpp"
#include <dwmapi.h>
#include <algorithm>
#include <new>
#include <cstdlib>

namespace explorer {
namespace {
constexpr wchar_t FrameClass[]=L"WindowsExplorer.ModernFrame";
constexpr size_t MaxTabs=32;
void frameTheme(HWND window) {
    applyWindowTheme(window);
    const auto theme=themeState();
    if(theme.windowsBuild>=22621) {
        const DWORD backdrop=theme.highContrast?DWMSBT_NONE:DWMSBT_TABBEDWINDOW;
        DwmSetWindowAttribute(window,DWMWA_SYSTEMBACKDROP_TYPE,&backdrop,sizeof(backdrop));
    }
}
}
ModernExplorerWindow::ModernExplorerWindow(HINSTANCE instance,bool headless) noexcept
    :instance_(instance),headless_(headless) {}

ModernExplorerWindow::~ModernExplorerWindow() {
    if(FAILED(shutdown())) {
        // A native callback may still own its creator apartment. Match the
        // existing host's fail-closed lifetime contract instead of freeing it.
        TerminateProcess(GetCurrentProcess(),8);std::_Exit(8);
    }
    tabs_.reset();if(font_)DeleteObject(font_);
}

HRESULT ModernExplorerWindow::create(const std::wstring& location,const SearchWindowContext* search) {
    if(window_)return E_UNEXPECTED;
    if(headless_) {
        const auto desktop=PrivateDesktop::current();
        if(!desktop||!desktop->ready()||FAILED(desktop->verifyIsolation()))return E_ACCESSDENIED;
    }
    WNDCLASSEXW type{sizeof(type)};type.hInstance=instance_;type.lpfnWndProc=windowProc;
    type.lpszClassName=FrameClass;type.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    type.hbrBackground=GetSysColorBrush(COLOR_WINDOW);type.hIcon=LoadIconW(nullptr,IDI_APPLICATION);
    if(!RegisterClassExW(&type)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return HRESULT_FROM_WIN32(GetLastError());
    const auto preferences=headless_?Preferences{}:loadPreferences(preferencesPath());
    window_=CreateWindowExW(WS_EX_CONTROLPARENT,FrameClass,L"File Explorer",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
        CW_USEDEFAULT,CW_USEDEFAULT,std::max(640,preferences.windowWidth),std::max(400,preferences.windowHeight),
        nullptr,nullptr,instance_,this);
    if(!window_)return HRESULT_FROM_WIN32(GetLastError());
    dpi_=GetDpiForWindow(window_);refreshFont();
    auto hr=tabs_.create(window_,instance_);tabs_.setFont(font_);
    frameTheme(window_);
    if(SUCCEEDED(hr))hr=addTab(location,nullptr,search);
    if(FAILED(hr)) {shutdown();return hr;}
    layout();return S_OK;
}

HRESULT ModernExplorerWindow::addTab(const std::wstring& location,TabId* result,const SearchWindowContext* search) {
    if(result)*result=BrowserTabs::NoTab;
    if(!window_||closing_)return E_ABORT;
    if(busy_)return HRESULT_FROM_WIN32(ERROR_BUSY);
    if(entries_.size()>=MaxTabs)return HRESULT_FROM_WIN32(ERROR_TOO_MANY_OPEN_FILES);
    busy_=true;
    struct End {ModernExplorerWindow& host;~End(){host.busy_=false;host.collectRetired();if(host.closeRequested_&&host.window_)PostMessageW(host.window_,WM_CLOSE,0,0);}} end{*this};
    try {
        ComPtr<ExplorerApp> app;app.Attach(new ExplorerApp(instance_,headless_,RibbonLayout::Authored,ExplorerChrome::Compact));
        auto hr=search?app->prepareSearchWindowContext(*search):S_OK;
        if(FAILED(hr))return hr;
        entries_.reserve(entries_.size()+1);
        TabId id=0;hr=tabs_.add(L"New tab",&id);if(FAILED(hr))return hr;
        entries_.push_back({id,app,nullptr,false});
        hr=app->createEmbedded(window_,location);
        auto found=std::find_if(entries_.begin(),entries_.end(),[&](const Entry& row){return row.id==id;});
        if(FAILED(hr)) {
            if(found!=entries_.end())entries_.erase(found);
            tabs_.remove(id);return hr;
        }
        if(found==entries_.end())return E_ABORT;
        found->child=app->window();
        tabs_.setLabel(id,app->currentTitle().empty()?L"Loading…":app->currentTitle());
        layout();
        hr=selectTab(id);if(SUCCEEDED(hr)&&result)*result=id;
        return hr;
    }catch(const std::bad_alloc&) {return E_OUTOFMEMORY;}
}

ExplorerApp* ModernExplorerWindow::activeBrowser() const noexcept {
    const auto found=std::find_if(entries_.begin(),entries_.end(),[&](const Entry& row){return row.id==active_;});
    return found==entries_.end()?nullptr:found->app.Get();
}

HRESULT ModernExplorerWindow::selectTab(TabId id) {
    const auto found=std::find_if(entries_.begin(),entries_.end(),[&](const Entry& row){return row.id==id;});
    if(found==entries_.end()||found->closing||closing_)return E_INVALIDARG;
    const auto next=found->app;
    const auto hr=tabs_.select(id);if(FAILED(hr))return hr;
    if(active_!=id)if(auto previous=activeBrowser())previous->setEmbeddedActive(false);
    active_=id;next->setEmbeddedActive(true);layout();updateTitle();return S_OK;
}

HRESULT ModernExplorerWindow::closeTab(TabId id) {
    if(busy_)return HRESULT_FROM_WIN32(ERROR_BUSY);
    const auto found=std::find_if(entries_.begin(),entries_.end(),[&](const Entry& row){return row.id==id;});
    if(found==entries_.end())return E_INVALIDARG;
    if(found->closing)return S_FALSE;
    found->closing=true;
    if(found->child&&IsWindow(found->child)) {
        if(!PostMessageW(found->child,WM_CLOSE,0,0)) {found->closing=false;return HRESULT_FROM_WIN32(GetLastError());}
    }else collectDestroyed(found->child,found->app.Get());
    return S_OK;
}

void ModernExplorerWindow::collectDestroyed(HWND child,ExplorerApp* app) {
    if(busy_)return; // Failed construction is reconciled by addTab itself.
    const auto found=std::find_if(entries_.begin(),entries_.end(),[&](const Entry& row){return row.child==child&&row.app.Get()==app;});
    if(found==entries_.end()||found->app->window())return;
    const auto retained=found->app;const auto id=found->id;
    const bool wasActive=id==active_;
    const auto status=retained->shutdownStatus();if(FAILED(status))shutdownStatus_=status;
    entries_.erase(found);tabs_.remove(id);
    if(entries_.empty()) {
        active_=BrowserTabs::NoTab;
        if(window_)DestroyWindow(window_);
    }else if(!closing_&&wasActive) {
        auto selected=tabs_.selected();
        if(selected==BrowserTabs::NoTab)selected=entries_.front().id;
        selectTab(selected);
    }else if(!closing_) {
        layout();updateTitle();
    }
}

void ModernExplorerWindow::collectRetired() {
    while(!busy_) {
        const auto found=std::find_if(entries_.begin(),entries_.end(),[](const Entry& row){return row.child&&!row.app->window();});
        if(found==entries_.end())return;
        const auto retained=found->app;const auto child=found->child;
        collectDestroyed(child,retained.Get());
    }
}

void ModernExplorerWindow::refreshFont() {
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    if(!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0,dpi_))return;
    const auto next=CreateFontIndirectW(&metrics.lfMessageFont);if(!next)return;
    const auto previous=font_;font_=next;tabs_.setFont(font_);if(previous)DeleteObject(previous);
}

void ModernExplorerWindow::layout() {
    if(!window_)return;
    RECT client{};GetClientRect(window_,&client);
    const auto top=BrowserTabs::height(dpi_);
    tabs_.layout({0,0,client.right,top},dpi_);
    const auto snapshot=entries_;const auto frame=window_;
    for(const auto& row:snapshot) {
        if(window_!=frame)break;
        const auto current=std::find_if(entries_.begin(),entries_.end(),[&](const Entry& entry){return entry.id==row.id&&entry.app.Get()==row.app.Get()&&entry.child==row.child;});
        if(current!=entries_.end()&&row.child&&row.app->window()==row.child&&IsWindow(row.child))
            SetWindowPos(row.child,nullptr,0,top,client.right,std::max(0L,client.bottom-top),SWP_NOACTIVATE|SWP_NOZORDER);
    }
}

void ModernExplorerWindow::updateTitle() {
    if(!window_)return;
    const auto app=activeBrowser();
    const auto title=app&&!app->currentTitle().empty()?app->currentTitle()+L" — File Explorer":L"File Explorer";
    SetWindowTextW(window_,title.c_str());
}

void ModernExplorerWindow::showFailure(HRESULT status) {
    if(FAILED(status)&&!headless_&&window_)
        MessageBoxW(window_,hresultMessage(status).c_str(),L"File Explorer",MB_OK|MB_ICONERROR);
}

bool ModernExplorerWindow::preprocess(MSG& message) {
    if(!window_||!message.hwnd||(message.hwnd!=window_&&!IsChild(window_,message.hwnd)))return false;
    if(message.message==WM_KEYDOWN||message.message==WM_SYSKEYDOWN) {
        const bool control=(GetKeyState(VK_CONTROL)&0x8000)!=0;
        const bool shift=(GetKeyState(VK_SHIFT)&0x8000)!=0;
        const bool alt=(GetKeyState(VK_MENU)&0x8000)!=0;
        if(alt&&message.wParam==VK_F4) {PostMessageW(window_,WM_CLOSE,0,0);return true;}
        if(!alt&&control&&message.wParam==L'T'&&!shift) {showFailure(addTab(L""));return true;}
        if(!alt&&control&&message.wParam==L'W'&&!shift) {showFailure(closeTab(active_));return true;}
        if(!alt&&control&&message.wParam==VK_TAB) {
            const auto& ids=tabs_.ids();const auto index=tabs_.indexOf(active_);
            if(!ids.empty()&&index>=0) {
                const auto count=static_cast<int>(ids.size());
                showFailure(selectTab(ids[(index+(shift?count-1:1))%count]));
            }
            return true;
        }
        if(!alt&&control&&message.wParam>=L'1'&&message.wParam<=L'9') {
            const auto& ids=tabs_.ids();const auto index=message.wParam==L'9'&&!ids.empty()?ids.size()-1:static_cast<size_t>(message.wParam-L'1');
            if(index<ids.size())showFailure(selectTab(ids[index]));return true;
        }
        if(!alt&&message.wParam==VK_F11) {SendMessageW(window_,ToggleFullscreen,0,0);return true;}
        if(!alt&&!control&&message.wParam==VK_F6) {
            const auto focus=GetFocus();
            if(focus==tabs_.window()||focus==tabs_.newButton()||focus==tabs_.closeButton()||
               (focus&&IsChild(tabs_.window(),focus))) {
                if(auto app=activeBrowser()) {const ComPtr<ExplorerApp> retained=app;retained->execute(shift?FocusPrevious:FocusNext);}
                return true;
            }
        }
    }
    if(auto app=activeBrowser())if(app->preprocess(message))return true;
    if(message.message==WM_KEYDOWN&&message.wParam==VK_TAB&&!(GetKeyState(VK_CONTROL)&0x8000))
        return IsDialogMessageW(window_,&message)!=FALSE;
    return false;
}

int ModernExplorerWindow::run(int showCommand) {
    ShowWindow(window_,showCommand);UpdateWindow(window_);
    if(auto app=activeBrowser())app->setEmbeddedActive(true);
    MSG message{};BOOL read=0;
    while((read=GetMessageW(&message,nullptr,0,0))>0) {
        if(preprocess(message))continue;TranslateMessage(&message);DispatchMessageW(&message);
    }
    return read<0?7:static_cast<int>(message.wParam);
}

HRESULT ModernExplorerWindow::shutdown(DWORD timeout) noexcept {
    if(!window_)return shutdownStatus_;
    try {
        SendMessageW(window_,WM_CLOSE,0,0);
        const auto deadline=GetTickCount64()+timeout;
        while(window_&&GetTickCount64()<deadline) {
            MSG message{};
            while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
                if(message.message==WM_QUIT)continue;
                TranslateMessage(&message);DispatchMessageW(&message);
            }
            if(window_)MsgWaitForMultipleObjectsEx(0,nullptr,10,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
        }
        return window_?HRESULT_FROM_WIN32(ERROR_TIMEOUT):shutdownStatus_;
    }catch(...) {return E_FAIL;}
}

LRESULT CALLBACK ModernExplorerWindow::windowProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    auto host=reinterpret_cast<ModernExplorerWindow*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE) {
        host=static_cast<ModernExplorerWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        host->window_=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(host));
    }
    if(host)try{return host->onMessage(message,wparam,lparam);}catch(...){host->showFailure(E_FAIL);return 0;}
    return DefWindowProcW(window,message,wparam,lparam);
}

LRESULT ModernExplorerWindow::onMessage(UINT message,WPARAM wparam,LPARAM lparam) {
    switch(message) {
    case WM_SIZE:layout();return 0;
    case WM_SETFOCUS:if(auto app=activeBrowser())app->setEmbeddedActive(true);return 0;
    case WM_COMMAND:
        if(LOWORD(wparam)==BrowserTabs::NewTabCommand) {showFailure(addTab(L""));return 0;}
        if(LOWORD(wparam)==BrowserTabs::CloseTabCommand) {showFailure(closeTab(active_));return 0;}
        break;
    case WM_NOTIFY: {
        const auto info=reinterpret_cast<NMHDR*>(lparam);
        if(info&&info->hwndFrom==tabs_.window()&&info->code==TCN_SELCHANGE) {showFailure(selectTab(tabs_.selected()));return 0;}
        break;
    }
    case ExplorerApp::EmbeddedTitleChanged:
        for(const auto& row:entries_)if(row.app.Get()==reinterpret_cast<ExplorerApp*>(lparam)&&row.app->window()==reinterpret_cast<HWND>(wparam))
            tabs_.setLabel(row.id,row.app->currentTitle().empty()?L"Home":row.app->currentTitle());
        updateTitle();return 0;
    case ExplorerApp::EmbeddedDestroyed:
        collectDestroyed(reinterpret_cast<HWND>(wparam),reinterpret_cast<ExplorerApp*>(lparam));return 0;
    case ExplorerApp::EmbeddedFocusTabs:SetFocus(tabs_.window());return 0;
    case WM_GETMINMAXINFO:reinterpret_cast<MINMAXINFO*>(lparam)->ptMinTrackSize={MulDiv(640,dpi_,96),MulDiv(400,dpi_,96)};return 0;
    case WM_DPICHANGED: {
        dpi_=HIWORD(wparam);refreshFont();const auto bounds=reinterpret_cast<RECT*>(lparam);
        SetWindowPos(window_,nullptr,bounds->left,bounds->top,bounds->right-bounds->left,bounds->bottom-bounds->top,SWP_NOZORDER|SWP_NOACTIVATE);
        {const auto snapshot=entries_;const auto frame=window_;
        for(const auto& row:snapshot) {
            if(window_!=frame)break;
            const auto current=std::find_if(entries_.begin(),entries_.end(),[&](const Entry& entry){return entry.id==row.id&&entry.app.Get()==row.app.Get()&&entry.child==row.child;});
            if(current!=entries_.end()&&row.child&&row.app->window()==row.child)SendMessageW(row.child,WM_DPICHANGED,wparam,lparam);
        }}
        layout();return 0;
    }
    case WM_THEMECHANGED:case WM_SETTINGCHANGE:case WM_SYSCOLORCHANGE: {
        refreshProcessTheme();frameTheme(window_);tabs_.applyTheme();
        std::vector<ComPtr<ExplorerApp>> retained;for(const auto& row:entries_)retained.push_back(row.app);
        for(const auto& app:retained)if(app->window())SendMessageW(app->window(),message,wparam,lparam);
        return 0;
    }
    case ToggleFullscreen:
        if(!fullscreen_) {
            MONITORINFO monitor{sizeof(monitor)};
            if(!GetWindowPlacement(window_,&placement_)||!GetMonitorInfoW(MonitorFromWindow(window_,MONITOR_DEFAULTTONEAREST),&monitor))return 0;
            windowStyle_=GetWindowLongPtrW(window_,GWL_STYLE);SetWindowLongPtrW(window_,GWL_STYLE,windowStyle_&~WS_OVERLAPPEDWINDOW);
            const auto& bounds=monitor.rcMonitor;SetWindowPos(window_,HWND_TOP,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,SWP_FRAMECHANGED);
            fullscreen_=true;
        }else {
            SetWindowLongPtrW(window_,GWL_STYLE,windowStyle_);SetWindowPlacement(window_,&placement_);
            SetWindowPos(window_,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);fullscreen_=false;
        }
        return 0;
    case WM_CLOSE:
        if(busy_) {closeRequested_=true;return 0;}
        closing_=true;closeRequested_=false;
        if(entries_.empty()) {DestroyWindow(window_);return 0;}
        {const auto ids=tabs_.ids();for(const auto id:ids)closeTab(id);}return 0;
    case WM_DESTROY:if(!headless_)PostQuitMessage(FAILED(shutdownStatus_)?8:0);return 0;
    case WM_NCDESTROY: {
        const auto old=window_;SetWindowLongPtrW(old,GWLP_USERDATA,0);window_=nullptr;
        return DefWindowProcW(old,message,wparam,lparam);
    }
    }
    return DefWindowProcW(window_,message,wparam,lparam);
}
}
