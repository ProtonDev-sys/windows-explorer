// Read-only diagnosis, never UI-parity or stock-Explorer-owner proof.
#include "explorer/headless_visual.hpp"
#include "explorer/namespace_actions.hpp"
#include "explorer/ribbon.hpp"
#include "explorer/theme.hpp"
#include "explorer/native_apartment.hpp"
#include "explorer/worker_sta.hpp"
#include "stock_image_observer.hpp"
#include <shlobj.h>
#include <shlwapi.h>
#include <propvarutil.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <UIRibbonPropertyHelpers.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {
using Microsoft::WRL::ComPtr;
using installed_image_contract_draft::RawDib;
constexpr wchar_t hostClass[]=L"WindowsExplorerOwnedNativeResourceOracle";
ULONGLONG deadline=0;
bool teardownFailed=false;
struct Window;
std::array<Window*,8> ownedWindows{};
std::atomic<std::uint64_t> nextWindowToken{0};
std::uint64_t ownedWindowToken(HWND owner) noexcept;
struct Unavailable {std::string message;};
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
[[noreturn]] void failClosedRetirement(const char* source,HRESULT actual)noexcept{
    std::fprintf(stderr,"FAIL: own diagnostic retirement %s actualHRESULT=%lu; native owners retained until process termination\n",source,static_cast<unsigned long>(actual));
    std::fflush(stderr);std::terminate();
}
void exact(HRESULT status,const char* message){if(status!=S_OK)throw std::runtime_error(std::string(message)+" HRESULT="+std::to_string(static_cast<ULONG>(status)));}
void printHr(const char* field,HRESULT status){std::cout<<' '<<field<<'='<<static_cast<ULONG>(status);}
void printText(const char* field,const wchar_t* value){
    // Escaped UTF-16 code units preserve exact spelling without console/MUI
    // conversion or private Shell-item display names. Only command resources.
    std::cout<<' '<<field<<"=\"";
    if(value)for(std::size_t i=0;value[i]&&i<32768;++i){
        const auto ch=static_cast<unsigned>(value[i]);
        if(ch>=0x20&&ch<0x7f&&ch!='\\'&&ch!='"')std::cout<<static_cast<char>(ch);
        else std::cout<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<ch<<std::dec;
    }
    std::cout<<'"';
}
void guard(HWND owner=nullptr){
    require(GetTickCount64()<deadline,"Native resource oracle exceeded its 65-second admission bound");
    const auto* desktop=explorer::PrivateDesktop::current();
    require(desktop!=nullptr,"Native resource oracle lacks its private desktop");
    exact(desktop->verifyIsolation(),"Read exact private desktop identity");
    bool visible=true;exact(desktop->visibleWindowsOnInputDesktop(visible),"Read input desktop isolation");
    require(!visible,"Native resource oracle exposed an input-desktop window");
    if(owner){DWORD process=0;require(IsWindow(owner)&&GetWindowThreadProcessId(owner,&process)==GetCurrentThreadId()&&
        process==GetCurrentProcessId()&&ownedWindowToken(owner)!=0,"Native resource oracle refuses an unowned/replaced HWND");
        if(GetDpiForWindow(owner)!=96)throw Unavailable{"actual owned HWND DPI is not 96"};}
}
void pump(HWND owner,ULONGLONG until){
    do{guard(owner);MSG message{};unsigned dispatched=0;
        while(dispatched++<32&&GetTickCount64()<deadline&&PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){
            require(message.message!=WM_QUIT,"Unexpected WM_QUIT during read-only resource measurement");
            TranslateMessage(&message);DispatchMessageW(&message);
        }
        if(GetTickCount64()<until)MsgWaitForMultipleObjectsEx(0,nullptr,2,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
    }while(GetTickCount64()<until);
    guard(owner);
}
struct Window {
    HWND value=nullptr;
    const DWORD creator=GetCurrentThreadId();
    const std::uint64_t token=nextWindowToken.fetch_add(1,std::memory_order_relaxed)+1;
    std::size_t registrySlot=ownedWindows.size();
    bool destroyed=false;
    static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wParam,LPARAM lParam) noexcept {
        auto* owner=reinterpret_cast<Window*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(message==WM_NCCREATE){const auto* create=reinterpret_cast<const CREATESTRUCTW*>(lParam);
            owner=static_cast<Window*>(create->lpCreateParams);if(!owner||owner->creator!=GetCurrentThreadId())return FALSE;
            SetLastError(ERROR_SUCCESS);if(!SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(owner))&&GetLastError())return FALSE;}
        if(message==WM_NCDESTROY&&owner){owner->destroyed=true;SetWindowLongPtrW(window,GWLP_USERDATA,0);}
        return DefWindowProcW(window,message,wParam,lParam);
    }
    Window(){for(std::size_t i=0;i<ownedWindows.size();++i)if(!ownedWindows[i]){registrySlot=i;ownedWindows[i]=this;break;}
        require(registrySlot<ownedWindows.size(),"Owned window registry exceeded its fixed eight-entry bound");value=CreateWindowExW(0,hostClass,L"Owned read-only native resource oracle",WS_OVERLAPPEDWINDOW,
        0,0,1000,700,nullptr,nullptr,GetModuleHandleW(nullptr),this);
        if(!value){ownedWindows[registrySlot]=nullptr;throw std::runtime_error("Create owned private HWND failed");}}
    ~Window(){if(value&&!destroyed){if(ownedWindowToken(value)!=token||!DestroyWindow(value))std::terminate();}
        if(registrySlot<ownedWindows.size())ownedWindows[registrySlot]=nullptr;}
    Window(const Window&)=delete;Window& operator=(const Window&)=delete;
    void realize(){guard(value);require(SetWindowPos(value,nullptr,0,0,0,0,
        SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_SHOWWINDOW)!=FALSE,"Realize only the owned private HWND");
        require(IsWindowVisible(value)!=FALSE,"Owned HWND did not become visible on its private desktop");UpdateWindow(value);pump(value,GetTickCount64()+35);}
};
std::uint64_t ownedWindowToken(HWND window) noexcept {
    if(!window||!IsWindow(window))return 0;DWORD process=0;
    if(GetWindowThreadProcessId(window,&process)!=GetCurrentThreadId()||process!=GetCurrentProcessId())return 0;
    const auto actual=GetWindowLongPtrW(window,GWLP_USERDATA);
    for(const auto* owner:ownedWindows)if(owner&&owner->value==window&&!owner->destroyed&&owner->creator==GetCurrentThreadId()&&
        actual==reinterpret_cast<LONG_PTR>(owner))return owner->token;
    return 0;
}
struct PidlDelete{using pointer=LPITEMIDLIST;void operator()(pointer p)const noexcept{CoTaskMemFree(p);}};
using Pidl=std::unique_ptr<ITEMIDLIST,PidlDelete>;
Pidl objectPidl(IUnknown* object){require(object!=nullptr,"Actual source object is null");PIDLIST_ABSOLUTE value=nullptr;
    const auto status=SHGetIDListFromObject(object,&value);Pidl owned(value);exact(status,"Read actual full native PIDL");
    require(owned!=nullptr,"Successful source read omitted its PIDL");return owned;}
std::vector<BYTE> pidlBytes(PCIDLIST_ABSOLUTE value){require(value!=nullptr,"Native full PIDL is null");const auto bytes=ILGetSize(value);require(bytes>=sizeof(USHORT)&&bytes<=65536,"Native PIDL exceeded the bounded diagnostic");
    return {reinterpret_cast<const BYTE*>(value),reinterpret_cast<const BYTE*>(value)+bytes};}
ComPtr<IUnknown> identity(IUnknown* value){require(value!=nullptr,"Canonical identity input is null");ComPtr<IUnknown> result;
    exact(value->QueryInterface(IID_PPV_ARGS(&result)),"Read canonical native interface identity");
    require(result!=nullptr,"Successful canonical identity query omitted its object");return result;}

class BrowserEvents final:public IExplorerBrowserEvents {
    std::atomic<ULONG> references_{1};
public:
    const DWORD creator=GetCurrentThreadId();std::uint64_t epoch=0;bool pending=false,failed=false;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** output)override{
        if(!output)return E_POINTER;*output=nullptr;
        if(iid!=IID_IUnknown&&iid!=__uuidof(IExplorerBrowserEvents))return E_NOINTERFACE;
        *output=static_cast<IExplorerBrowserEvents*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return ++references_;}
    ULONG STDMETHODCALLTYPE Release()override{const auto left=--references_;if(!left)delete this;return left;}
    HRESULT STDMETHODCALLTYPE OnNavigationPending(PCIDLIST_ABSOLUTE)override{if(GetCurrentThreadId()!=creator)return RPC_E_WRONG_THREAD;++epoch;pending=true;return S_OK;}
    HRESULT STDMETHODCALLTYPE OnViewCreated(IShellView*)override{if(GetCurrentThreadId()!=creator)return RPC_E_WRONG_THREAD;++epoch;return S_OK;}
    HRESULT STDMETHODCALLTYPE OnNavigationComplete(PCIDLIST_ABSOLUTE)override{if(GetCurrentThreadId()!=creator)return RPC_E_WRONG_THREAD;++epoch;pending=false;return S_OK;}
    HRESULT STDMETHODCALLTYPE OnNavigationFailed(PCIDLIST_ABSOLUTE)override{if(GetCurrentThreadId()!=creator)return RPC_E_WRONG_THREAD;++epoch;pending=false;failed=true;return S_OK;}
};

struct OwnedBrowser {
    explorer::NativeApartmentClient apartmentClient; // Last native release/client retirement.
    HWND owner=nullptr;ComPtr<IExplorerBrowser> browser;ComPtr<BrowserEvents> events;DWORD cookie=0;
    ComPtr<IShellView> view;ComPtr<IFolderView2> folderView;ComPtr<IUnknown> viewIdentity;
    ComPtr<IShellItem> folder;ComPtr<IShellItemArray> originalSelection,background;
    std::vector<BYTE> folderIdentity;std::uint64_t acceptedEpoch=0;DWORD selectedCount=0;
    HRESULT rawSelection=E_PENDING;bool initialized=false,advised=false,admitted=false;
    ~OwnedBrowser(){retire();}
    void retire()noexcept{
        apartmentClient.beforeNativeRelease();
        if(advised&&browser){const auto status=browser->Unadvise(cookie);if(FAILED(status))failClosedRetirement("browser Unadvise",status);advised=false;}
        if(initialized&&browser){const auto status=browser->Destroy();if(FAILED(status))failClosedRetirement("browser Destroy",status);initialized=false;}
        apartmentClient.beforeNativeRelease();
        originalSelection.Reset();background.Reset();folder.Reset();viewIdentity.Reset();folderView.Reset();view.Reset();browser.Reset();events.Reset();
        admitted=false;apartmentClient.beforeNativeRelease();
    }
    void initialize(HWND window,REFKNOWNFOLDERID known){
        owner=window;guard(owner);exact(apartmentClient.acquire(),"Reserve actual owned browser apartment client");
        exact(SHGetKnownFolderItem(known,KF_FLAG_DEFAULT,nullptr,IID_PPV_ARGS(&folder)),"Read exact native scene folder");
        const auto expected=objectPidl(folder.Get());folderIdentity=pidlBytes(expected.get());
        exact(CoCreateInstance(CLSID_ExplorerBrowser,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&browser)),"Create owned native ExplorerBrowser");
        exact(apartmentClient.observeLoadedCode(),"Observe only native-loaded browser executable lifetime");
        require(browser!=nullptr,"Successful browser activation omitted its object");
        exact(browser->SetOptions(static_cast<EXPLORER_BROWSER_OPTIONS>(EBO_NOTRAVELLOG|EBO_NOBORDER)),"Disable only this owned browser travel log and border");
        RECT area{0,170,1000,660};FOLDERSETTINGS settings{FVM_DETAILS,FWF_NOHEADERINALLVIEWS};
        exact(browser->Initialize(owner,&area,&settings),"Initialize owned native Shell view");initialized=true;
        events.Attach(new BrowserEvents());exact(browser->Advise(events.Get(),&cookie),"Observe only owned browser navigation");advised=true;
        exact(browser->BrowseToObject(folder.Get(),SBSP_ABSOLUTE),"Browse the exact native read-only scene folder");
        const auto until=std::min(deadline,GetTickCount64()+5000);
        while(GetTickCount64()<until){guard(owner);if(events->failed)throw Unavailable{"actual owned browser navigation failed"};
            if(!events->pending){ComPtr<IFolderView2> candidate;const auto read=browser->GetCurrentView(IID_PPV_ARGS(&candidate));
                if(read==S_OK&&candidate){ComPtr<IPersistFolder2> persisted;const auto native=candidate->GetFolder(IID_PPV_ARGS(&persisted));
                    PIDLIST_ABSOLUTE raw=nullptr;const auto current=native==S_OK&&persisted?persisted->GetCurFolder(&raw):native==S_OK?E_UNEXPECTED:native;Pidl actual(raw);
                    if(current==S_OK&&actual&&pidlBytes(actual.get())==folderIdentity){folderView=std::move(candidate);break;}}}
            pump(owner,std::min(until,GetTickCount64()+10));}
        if(!folderView)throw Unavailable{"actual owned native scene view did not arrive within five seconds"};
        exact(browser->GetCurrentView(IID_PPV_ARGS(&view)),"Retain actual native view site");viewIdentity=identity(view.Get());
        rawSelection=folderView->GetSelection(FALSE,&originalSelection);
        if(rawSelection==S_OK||rawSelection==S_FALSE){require(originalSelection!=nullptr,"Successful native selection has no array");exact(originalSelection->GetCount(&selectedCount),"Read original native selection count");}
        else if(rawSelection==HRESULT_FROM_WIN32(ERROR_NOT_FOUND))require(originalSelection==nullptr,"Failed native selection unexpectedly returned an unchecked array");
        else throw Unavailable{"actual native empty selection returned HRESULT="+std::to_string(static_cast<ULONG>(rawSelection))};
        require(selectedCount==0,"Read-only scene unexpectedly selected an unowned native child");
        // This is the actual view's documented none-implies-folder array,
        // not a manufactured selection or an inferred first selected item.
        exact(folderView->GetSelection(TRUE,&background),"Read actual view-owned background folder array");
        require(background!=nullptr,"Successful native background read omitted its array");
        DWORD count=0;exact(background->GetCount(&count),"Read original background count");require(count==1,"Native background did not name exactly its folder");
        ComPtr<IShellItem> item;exact(background->GetItemAt(0,&item),"Read actual background folder identity");
        const auto actual=objectPidl(item.Get());require(pidlBytes(actual.get())==folderIdentity,"Native background folder disagrees with the viewed folder");
        exact(apartmentClient.observeLoadedCode(),"Observe actual browser/view native activation");
        item.Reset(); // Last temporary native item Release precedes source admission.
        acceptedEpoch=events->epoch;admitted=true;verify();
        std::cout<<"SOURCE selectionCount="<<selectedCount<<" backgroundCount="<<count<<" navigationEpoch="<<acceptedEpoch;
        printHr("GetSelectionFALSE",rawSelection);std::cout<<" fullFolderPidlVerified=1 originalViewSite=1\n";
    }
    void verify(){
        guard(owner);require(admitted&&events&&!events->pending&&!events->failed&&events->epoch==acceptedEpoch,"Provider read changed the original native navigation source");
        ComPtr<IShellView> current;exact(browser->GetCurrentView(IID_PPV_ARGS(&current)),"Re-read actual current view");
        auto canonical=identity(current.Get());require(canonical.Get()==viewIdentity.Get(),"Provider read replaced the actual view site");
        ComPtr<IPersistFolder2> persisted;exact(folderView->GetFolder(IID_PPV_ARGS(&persisted)),"Re-read original native folder");
        require(persisted!=nullptr,"Successful folder query omitted its persistence interface");
        PIDLIST_ABSOLUTE raw=nullptr;const auto folderRead=persisted->GetCurFolder(&raw);Pidl actual(raw);
        exact(folderRead,"Re-read full native folder PIDL");
        require(pidlBytes(actual.get())==folderIdentity,"Provider read changed original full folder identity");
        ComPtr<IShellItemArray> selected;const auto read=folderView->GetSelection(FALSE,&selected);DWORD count=0;
        if(read==S_OK||read==S_FALSE){require(selected!=nullptr,"Re-read native selection omitted its array");exact(selected->GetCount(&count),"Re-read full native selection count");}
        else require(read==HRESULT_FROM_WIN32(ERROR_NOT_FOUND)&&selected==nullptr,"Re-read native selection failed differently or returned an unchecked array");
        require(count==selectedCount&&count==0,"Provider read changed the original native empty selection");
        DWORD backgroundCount=0;exact(background->GetCount(&backgroundCount),"Re-read original whole background array");
        require(backgroundCount==1,"Original background array changed cardinality");
        ComPtr<IShellItem> actualBackground;exact(background->GetItemAt(0,&actualBackground),"Re-read original background item");
        const auto backgroundPidl=objectPidl(actualBackground.Get());
        require(pidlBytes(backgroundPidl.get())==folderIdentity,"Original background full PIDL changed");
        // Local actual interface Releases can pump. Complete them before the
        // final scalar/window fence, retaining original site/array throughout.
        actualBackground.Reset();selected.Reset();persisted.Reset();canonical.Reset();current.Reset();
        exact(apartmentClient.observeLoadedCode(),"Observe actual source-read native code lifetime");
        guard(owner);require(events->epoch==acceptedEpoch&&!events->pending&&!events->failed,"Source changed during external identity readback");
    }
};

struct Target{UINT native,application;const char* name;};
constexpr std::array targets{
    Target{13634,explorer::RibbonAddNetworkLocation,"AddNetworkLocation"},
    Target{13648,explorer::RibbonOpenSettings,"OpenSettings"},
    Target{13586,explorer::RibbonNetworkSharingCenter,"NetworkAndSharing"},
    Target{14593,explorer::RibbonRecycleProperties,"RecycleProperties"}
};
const Target* target(UINT native){for(const auto& t:targets)if(t.native==native)return &t;return nullptr;}
bool observedTextId(UINT native){
    if(target(native))return true;
    // Only previously source-mapped Computer/Network/Recycle group IDs. Their
    // actual OnCreateUICommand type is independently recorded; no guessed label.
    constexpr std::array groups{0x2500U,0x2501U,0x2502U,0x2503U,0x2510U,0x2512U,0x2513U,0x2515U,0x2900U,0x2901U,0x2910U,0x2911U};
    return std::find(groups.begin(),groups.end(),native)!=groups.end();
}
enum class Property{SmallImage,LargeImage,Label,Tooltip};
const char* propertyName(Property p){switch(p){case Property::SmallImage:return "SmallImage";case Property::LargeImage:return "LargeImage";case Property::Label:return "Label";case Property::Tooltip:return "TooltipTitle";}return "unknown";}
const PROPERTYKEY& propertyKey(Property p){switch(p){case Property::SmallImage:return UI_PKEY_SmallImage;case Property::LargeImage:return UI_PKEY_LargeImage;case Property::Label:return UI_PKEY_Label;case Property::Tooltip:return UI_PKEY_TooltipTitle;}return UI_PKEY_Label;}
struct CurrentRow{
    UINT native=0;Property property=Property::Label;std::uint64_t ordinal=0,windowGeneration=0;
    HWND window=nullptr;DWORD creator=0;UINT dpi=0;bool present=false,stringPresent=false,truncated=false,stable=false;
    VARTYPE type=VT_EMPTY;HRESULT imageQuery=E_PENDING,returned=E_PENDING;std::array<wchar_t,1024> text{};ComPtr<IUIImage> image;
};
class FirstCurrentHandler final:public IUIApplication,public IUICommandHandler{
    std::atomic<ULONG> references_{1};
public:
    explicit FirstCurrentHandler(HWND owner):window(owner),windowToken(ownedWindowToken(owner)){}
    const DWORD creator=GetCurrentThreadId();const HWND window;const std::uint64_t windowToken;
    bool retired=false,overflow=false;
    struct Registration{UINT native=0;UI_COMMANDTYPE type=UI_COMMANDTYPE_UNKNOWN;};
    std::array<Registration,1024> registrations{};UINT registeredCount=0;
    std::array<CurrentRow,64> rows{};UINT count=0;std::uint64_t ordinal=0,requests=0;
    bool registered(UINT native)const noexcept{for(UINT i=0;i<registeredCount;++i)if(registrations[i].native==native)return true;return false;}
    bool binding()const noexcept{DWORD process=0;return !retired&&GetCurrentThreadId()==creator&&IsWindow(window)&&
        GetWindowThreadProcessId(window,&process)==creator&&process==GetCurrentProcessId()&&GetDpiForWindow(window)==96&&
        windowToken!=0&&ownedWindowToken(window)==windowToken;}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** output)override{
        if(!output)return E_POINTER;*output=nullptr;if(iid==IID_IUnknown||iid==__uuidof(IUIApplication))*output=static_cast<IUIApplication*>(this);
        else if(iid==__uuidof(IUICommandHandler))*output=static_cast<IUICommandHandler*>(this);else return E_NOINTERFACE;AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return ++references_;}
    ULONG STDMETHODCALLTYPE Release()override{const auto left=--references_;if(!left)delete this;return left;}
    HRESULT STDMETHODCALLTYPE OnViewChanged(UINT32,UI_VIEWTYPE,IUnknown*,UI_VIEWVERB verb,INT32 reason)override{return verb==UI_VIEWVERB_ERROR?reason:S_OK;}
    HRESULT STDMETHODCALLTYPE OnCreateUICommand(UINT32 command,UI_COMMANDTYPE type,IUICommandHandler** output)override{
        if(!output)return E_POINTER;*output=nullptr;if(!binding())return RPC_E_WRONG_THREAD;
        if(!registered(command)){if(registeredCount>=registrations.size()){overflow=true;return HRESULT_FROM_WIN32(ERROR_MORE_DATA);}
            registrations[registeredCount++]={command,type};}
        *output=static_cast<IUICommandHandler*>(this);AddRef();return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDestroyUICommand(UINT32,UI_COMMANDTYPE,IUICommandHandler*)override{return GetCurrentThreadId()==creator?S_OK:RPC_E_WRONG_THREAD;}
    HRESULT STDMETHODCALLTYPE Execute(UINT32,UI_EXECUTIONVERB,const PROPERTYKEY*,const PROPVARIANT*,IUISimplePropertySet*)override{return E_ACCESSDENIED;}
    HRESULT STDMETHODCALLTYPE UpdateProperty(UINT32 native,REFPROPERTYKEY key,const PROPVARIANT* current,PROPVARIANT* output)override{
        if(!output)return E_POINTER;PropVariantInit(output);if(!binding())return RPC_E_WRONG_THREAD;
        bool eligible=false;Property selected=Property::Label;
        for(const auto p:{Property::SmallImage,Property::LargeImage,Property::Label,Property::Tooltip})if(IsEqualPropertyKey(key,propertyKey(p))){selected=p;
            eligible=(p==Property::SmallImage||p==Property::LargeImage)?target(native)!=nullptr:observedTextId(native);break;}
        CurrentRow* saved=nullptr;
        if(eligible){++requests;++ordinal;bool seen=false;for(UINT i=0;i<count;++i)if(rows[i].native==native&&rows[i].property==selected)seen=true;
            if(!seen){if(count>=rows.size()){overflow=true;return E_OUTOFMEMORY;}
                // Reserve before any external QI/AddRef: null-first and nested
                // completion order cannot be relabelled by later host output.
                saved=&rows[count++];saved->native=native;saved->property=selected;saved->ordinal=ordinal;
                saved->window=window;saved->creator=creator;saved->dpi=GetDpiForWindow(window);saved->windowGeneration=windowToken;
                saved->present=current!=nullptr;saved->type=current?current->vt:static_cast<VARTYPE>(VT_EMPTY);
                if(current&&current->vt==VT_LPWSTR&&current->pwszVal){saved->stringPresent=true;const auto length=wcsnlen_s(current->pwszVal,saved->text.size());
                    saved->truncated=length==saved->text.size();const auto copied=std::min(length,saved->text.size()-1);
                    if(copied)std::memcpy(saved->text.data(),current->pwszVal,copied*sizeof(wchar_t));saved->text[copied]=L'\0';}
                if(selected==Property::SmallImage||selected==Property::LargeImage)saved->imageQuery=current&&current->vt==VT_UNKNOWN&&current->punkVal?
                    current->punkVal->QueryInterface(IID_PPV_ARGS(&saved->image)):E_NOINTERFACE;
            }}
        HRESULT result=E_NOTIMPL;
        // Independent reference preserves only a genuine supplied current.
        // No provider icon, state, label or color is supplied to this framework.
        const bool textProperty=IsEqualPropertyKey(key,UI_PKEY_Label)||IsEqualPropertyKey(key,UI_PKEY_TooltipTitle);
        const bool imageProperty=IsEqualPropertyKey(key,UI_PKEY_SmallImage)||IsEqualPropertyKey(key,UI_PKEY_LargeImage);
        ComPtr<IUIImage> repeatedImage;HRESULT repeatedQuery=E_NOINTERFACE;
        if(imageProperty&&target(native)&&!saved&&current&&current->vt==VT_UNKNOWN&&current->punkVal)
            repeatedQuery=current->punkVal->QueryInterface(IID_PPV_ARGS(&repeatedImage));
        const bool actualImage=saved?SUCCEEDED(saved->imageQuery)&&saved->image.Get()!=nullptr:SUCCEEDED(repeatedQuery)&&repeatedImage.Get()!=nullptr;
        if(current&&((textProperty&&current->vt==VT_LPWSTR&&current->pwszVal&&*current->pwszVal)||
            (imageProperty&&current->vt==VT_UNKNOWN&&current->punkVal&&target(native)&&actualImage)))result=PropVariantCopy(output,current);
        repeatedImage.Reset(); // External Release precedes the original binding fence below.
        if(!binding()){PropVariantClear(output);PropVariantInit(output);result=HRESULT_FROM_WIN32(ERROR_RETRY);}
        if(saved){saved->returned=result;saved->stable=binding();}return result;
    }
};
struct Independent{
    Window window;
    explorer::NativeApartmentClient apartmentClient; // Before all native COM fields.
    HMODULE module=nullptr;ComPtr<FirstCurrentHandler> handler;ComPtr<IUIFramework> framework;bool initialized=false;
    ~Independent(){retire();}
    void retire()noexcept{if(handler)handler->retired=true;apartmentClient.beforeNativeRelease();
        if(initialized&&framework){const auto status=framework->Destroy();if(FAILED(status))failClosedRetirement("independent framework Destroy",status);initialized=false;}
        apartmentClient.beforeNativeRelease();framework.Reset();handler.Reset();
        if(module){if(!FreeLibrary(module)){const auto error=GetLastError();failClosedRetirement("owned data resource FreeLibrary",error?HRESULT_FROM_WIN32(error):E_FAIL);}module=nullptr;}apartmentClient.beforeNativeRelease();}
    void initialize(UINT32 modes){guard(window.value);module=LoadLibraryExW(L"ExplorerFrame.dll",nullptr,
        LOAD_LIBRARY_SEARCH_SYSTEM32|LOAD_LIBRARY_AS_DATAFILE|LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        if(!module)throw Unavailable{"installed ExplorerFrame resource module unavailable"};
        exact(apartmentClient.acquire(),"Reserve actual independent raw-framework apartment client");
        handler.Attach(new FirstCurrentHandler(window.value));exact(CoCreateInstance(CLSID_UIRibbonFramework,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&framework)),"Create independent native framework");
        require(framework!=nullptr,"Successful independent framework activation omitted its object");
        exact(framework->Initialize(window.value,handler.Get()),"Initialize independent owned native framework");initialized=true;
        exact(framework->LoadUI(module,L"EXPLORER_RIBBON"),"Load actual unmodified installed Ribbon resource");
        exact(apartmentClient.observeLoadedCode(),"Observe only native-loaded independent executable lifetime");
        exact(framework->SetModes(modes),"Set source-matched independent resource mode");
        for(UINT i=0;i<handler->registeredCount;++i)if(handler->registrations[i].type==UI_COMMANDTYPE_CONTEXT){PROPVARIANT hidden{};
            exact(InitPropVariantFromUInt32(UI_CONTEXTAVAILABILITY_NOTAVAILABLE,&hidden),"Create context baseline value");
            const auto result=framework->SetUICommandProperty(handler->registrations[i].native,UI_PKEY_ContextAvailable,hidden);PropVariantClear(&hidden);exact(result,"Match original context suppression");}
    }
};
struct ImageRow{explorer::RibbonImageObservation metadata;ComPtr<IUIImage> current,returned;};
struct Collector{
    std::array<ImageRow,64> rows{};UINT count=0;bool retired=false,overflow=false;std::uint64_t targetRequests=0;
    void observe(const explorer::RibbonImageObservation& event)noexcept{
        if(retired||!target(event.nativeCommand))return;++targetRequests;
        if(count>=rows.size()){overflow=true;return;}
        // No framework, pixel, provider, metadata or theme calls in delivery.
        ImageRow local;local.metadata=event;local.current=event.currentImage;local.returned=event.returnedImage;rows[count++]=std::move(local);
    }
};

std::uint64_t hash(const std::vector<BYTE>& bytes)noexcept{std::uint64_t value=14695981039346656037ULL;for(const auto byte:bytes){value^=byte;value*=1099511628211ULL;}return value;}
std::vector<BYTE> topDown(const RawDib& raw){const auto rows=raw.header.biHeight<0?-static_cast<std::int64_t>(raw.header.biHeight):raw.header.biHeight;
    require(raw.width>0&&raw.width<=128&&rows>0&&rows<=128&&raw.stride>=raw.width*4,"Actual icon raster dimensions exceed the diagnostic bound");
    std::vector<BYTE> result(static_cast<std::size_t>(rows)*static_cast<std::size_t>(raw.width)*4);
    for(std::int64_t y=0;y<rows;++y){const auto original=raw.header.biHeight<0?y:rows-y-1;
        std::memcpy(result.data()+static_cast<std::size_t>(y)*static_cast<std::size_t>(raw.width)*4,
            raw.storedBytes.data()+static_cast<std::size_t>(original)*static_cast<std::size_t>(raw.stride),static_cast<std::size_t>(raw.width)*4);}return result;}
struct Pixels{HRESULT status=E_PENDING;LONG width=0,height=0;std::vector<BYTE> bgra;};
Pixels actualPixels(IUIImage* image,HWND owner){guard(owner);Pixels result;RawDib raw;
    result.status=installed_image_contract_draft::copyActualRawDib(image,GetCurrentThreadId(),raw);guard(owner);
    if(result.status==S_OK){result.width=raw.width;result.height=raw.header.biHeight<0?-raw.header.biHeight:raw.header.biHeight;result.bgra=topDown(raw);
        result.status=installed_image_contract_draft::verifyActualRawDib(image,GetCurrentThreadId(),raw);guard(owner);if(FAILED(result.status))result.bgra.clear();}
    return result;}
struct Icon{HICON value=nullptr;~Icon(){if(value)DestroyIcon(value);}};
struct Raster{HBITMAP bitmap=nullptr;HDC dc=nullptr;HGDIOBJ previous=nullptr;~Raster(){if(previous&&dc)SelectObject(dc,previous);if(dc)DeleteDC(dc);if(bitmap)DeleteObject(bitmap);}};
Pixels providerPixels(const std::wstring& specification,UINT pixels,HWND owner){
    guard(owner);Pixels result;if(specification.empty()){result.status=E_NOTIMPL;return result;}
    if(specification.find(L":IMAGE:")!=std::wstring::npos){result.status=HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);return result;}
    auto location=specification;const auto comma=location.rfind(L',');if(comma==std::wstring::npos){result.status=E_NOTIMPL;return result;}
    wchar_t* end=nullptr;const long index=wcstol(location.c_str()+comma+1,&end,10);
    if(!end||*end||index<std::numeric_limits<int>::min()||index>std::numeric_limits<int>::max()){result.status=E_INVALIDARG;return result;}
    location.resize(comma);if(location.size()>1&&location.front()==L'"'&&location.back()==L'"')location=location.substr(1,location.size()-2);
    if(!location.empty()&&location.front()==L'@')location.erase(location.begin());std::array<wchar_t,32768> expanded{};
    const auto characters=ExpandEnvironmentStringsW(location.c_str(),expanded.data(),static_cast<DWORD>(expanded.size()));
    if(!characters||characters>expanded.size()){result.status=E_INVALIDARG;return result;}
    Icon icon;result.status=SHDefExtractIconW(expanded.data(),static_cast<int>(index),0,&icon.value,nullptr,MAKELONG(pixels,0));guard(owner);
    std::cout<<"EXTRACT pixels="<<pixels<<" nativeIndex="<<index;printText("actualExpandedSource",expanded.data());printHr("SHDefExtractIcon",result.status);std::cout<<" iconPresent="<<(icon.value!=nullptr)<<'\n';
    if(FAILED(result.status)||!icon.value){if(SUCCEEDED(result.status))result.status=E_FAIL;return result;}
    Raster raster;BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=static_cast<LONG>(pixels);
    info.bmiHeader.biHeight=-static_cast<LONG>(pixels);info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;void* bits=nullptr;
    raster.bitmap=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&bits,nullptr,0);require(raster.bitmap&&bits,"Create owned independent provider icon DIB");
    raster.dc=CreateCompatibleDC(nullptr);require(raster.dc!=nullptr,"Create owned independent provider icon DC");raster.previous=SelectObject(raster.dc,raster.bitmap);
    require(raster.previous&&raster.previous!=HGDI_ERROR,"Select only owned provider icon DIB");std::memset(bits,0,static_cast<std::size_t>(pixels)*pixels*4);
    require(DrawIconEx(raster.dc,0,0,icon.value,static_cast<int>(pixels),static_cast<int>(pixels),0,nullptr,DI_NORMAL)!=FALSE,"Render only the native returned provider resource");
    require(GdiFlush()!=FALSE,"Complete native provider icon rendering");const auto restored=SelectObject(raster.dc,raster.previous);
    require(restored&&restored!=HGDI_ERROR,"Restore owned provider drawing DC");raster.previous=nullptr;
    result.width=static_cast<LONG>(pixels);result.height=static_cast<LONG>(pixels);const auto* bytes=static_cast<const BYTE*>(bits);
    result.bgra.assign(bytes,bytes+static_cast<std::size_t>(pixels)*pixels*4);result.status=S_OK;guard(owner);return result;
}
void printPixels(const char* source,const Pixels& pixels){std::cout<<"PIXELS source="<<source;printHr("status",pixels.status);
    std::cout<<" width="<<pixels.width<<" height="<<pixels.height<<" storedBgraBytes="<<pixels.bgra.size()<<" bgraHash="<<hash(pixels.bgra)<<'\n';}
void compare(const char* left,const Pixels& a,const char* right,const Pixels& b){std::cout<<"COMPARE left="<<left<<" right="<<right;
    if(a.status!=S_OK||b.status!=S_OK||a.bgra.empty()||b.bgra.empty())std::cout<<" evidence=UNAVAILABLE";
    else std::cout<<" evidence="<<(a.width==b.width&&a.height==b.height&&a.bgra==b.bgra?"SAME_ALL_FOUR_BYTES":"DIFFERENT");std::cout<<'\n';}
struct GlobalColors{HRESULT query=E_PENDING;std::array<HRESULT,3> reads{E_PENDING,E_PENDING,E_PENDING};std::array<VARTYPE,3> types{};std::array<ULONG,3> hsb{};};
GlobalColors readColors(IUIFramework* framework,HWND owner){guard(owner);require(framework!=nullptr,"Actual color source framework is null");GlobalColors result;ComPtr<IPropertyStore> store;result.query=framework->QueryInterface(IID_PPV_ARGS(&store));
    if(SUCCEEDED(result.query))require(result.query==S_OK&&store!=nullptr,"Successful color-property query omitted its real native store");
    if(result.query==S_OK)for(std::size_t i=0;i<result.reads.size();++i){const std::array keys{&UI_PKEY_GlobalBackgroundColor,&UI_PKEY_GlobalHighlightColor,&UI_PKEY_GlobalTextColor};PROPVARIANT value{};
        result.reads[i]=store->GetValue(*keys[i],&value);result.types[i]=value.vt;if(value.vt==VT_UI4)result.hsb[i]=value.ulVal;PropVariantClear(&value);guard(owner);}
    store.Reset();guard(owner);return result;}
void printColors(const char* source,const GlobalColors& value){std::cout<<"GLOBAL_COLORS source="<<source;printHr("QIPropertyStore",value.query);
    constexpr std::array names{"background","highlight","text"};for(std::size_t i=0;i<names.size();++i){printHr(names[i],value.reads[i]);std::cout<<' '<<names[i]<<"VT="<<value.types[i]<<' '<<names[i]<<"PackedHSB="<<value.hsb[i];}std::cout<<" valuesAreHSBNotRgb=1\n";}
void nativeThemeFacts(HWND owner){
    guard(owner);std::array<wchar_t,32768> path{};std::array<wchar_t,1024> scheme{},size{};
    const auto style=GetCurrentThemeName(path.data(),static_cast<int>(path.size()),scheme.data(),static_cast<int>(scheme.size()),size.data(),static_cast<int>(size.size()));
    std::cout<<"NATIVE_VISUAL_STYLE";printHr("actualThemeName",style);
    if(style==S_OK){printText("actualPath",path.data());printText("actualColorScheme",scheme.data());printText("actualSize",size.data());}
    DWORD argb=0;BOOL opaque=FALSE;const auto dwm=DwmGetColorizationColor(&argb,&opaque);guard(owner);
    std::cout<<" actualWindowThemeHandle="<<reinterpret_cast<std::uintptr_t>(GetWindowTheme(owner));printHr("actualDwmColorization",dwm);
    std::cout<<" actualDwmArgb="<<argb<<" actualDwmOpaque="<<opaque<<" hostSpecificExplorerPalette=UNAVAILABLE\n";
    const auto policy=explorer::themePalette();
    std::cout<<"APP_COLOR_POLICY ribbonCOLORREF="<<policy.ribbon<<" captionCOLORREF="<<policy.caption<<" textCOLORREF="<<policy.text
        <<" controlTextCOLORREF="<<policy.controlText<<" selectionCOLORREF="<<policy.selection<<" selectionTextCOLORREF="<<policy.selectionText<<'\n';
    constexpr std::array codes{COLOR_WINDOW,COLOR_WINDOWTEXT,COLOR_BTNFACE,COLOR_BTNTEXT,COLOR_ACTIVECAPTION,COLOR_CAPTIONTEXT,COLOR_HIGHLIGHT,COLOR_HIGHLIGHTTEXT};
    for(const auto code:codes)std::cout<<"NATIVE_SYSTEM_COLOR index="<<code<<" actualCOLORREF="<<GetSysColor(code)
        <<" actualBorrowedSystemBrushPresent="<<(GetSysColorBrush(code)!=nullptr)<<" zeroDoesNotProvePlatformSupport=1 sharedSystemFactNotExplorerHostPalette=1\n";
    guard(owner);
}
void languageFacts(){
    ULONG count=0,characters=0;constexpr DWORD flags=MUI_LANGUAGE_NAME|MUI_UI_FALLBACK;
    SetLastError(ERROR_SUCCESS);const bool sized=GetThreadPreferredUILanguages(flags,&count,nullptr,&characters)!=FALSE;const auto error=GetLastError();
    std::cout<<"MUI flags="<<flags<<" sizeSucceeded="<<sized<<" sizeError="<<error<<" languages="<<count<<" characters="<<characters<<'\n';
    if(!sized||characters<2||characters>32768)return;std::vector<wchar_t> values(characters,L'\0');
    SetLastError(ERROR_SUCCESS);const bool read=GetThreadPreferredUILanguages(flags,&count,values.data(),&characters)!=FALSE;
    const auto readError=GetLastError();std::cout<<"MUI readSucceeded="<<read<<" readError="<<readError<<" languages="<<count<<'\n';
    if(!read)return;std::size_t position=0;for(ULONG i=0;i<count&&position<values.size()&&values[position];++i){const auto length=wcsnlen_s(values.data()+position,values.size()-position);
        require(length<values.size()-position,"Native MUI list is not bounded/multi-terminated");std::cout<<"MUI_LANGUAGE ordinal="<<i;printText("name",values.data()+position);std::cout<<'\n';position+=length+1;}
    std::array<wchar_t,32768> system{};const auto size=GetSystemDirectoryW(system.data(),static_cast<UINT>(system.size()));require(size&&size<system.size(),"Resolve actual system resource root");
    const auto path=std::wstring(system.data())+L"\\ExplorerFrame.dll";ULONGLONG enumerator=0;bool terminal=false;
    for(unsigned i=0;i<16;++i){std::array<wchar_t,LOCALE_NAME_MAX_LENGTH> language{};std::array<wchar_t,32768> mui{};
        ULONG languageChars=static_cast<ULONG>(language.size()),pathChars=static_cast<ULONG>(mui.size());SetLastError(ERROR_SUCCESS);
        const bool found=GetFileMUIPath(MUI_LANGUAGE_NAME|MUI_USER_PREFERRED_UI_LANGUAGES,path.c_str(),language.data(),&languageChars,mui.data(),&pathChars,&enumerator)!=FALSE;
        const auto nativeError=GetLastError();std::cout<<"MUI_FILE ordinal="<<i<<" found="<<found<<" nativeError="<<nativeError;
        if(found){printText("language",language.data());printText("actualExistingFallbackFile",mui.data());}std::cout<<" inventoryNotSelectedLoaderProof=1\n";
        if(!found){terminal=true;break;}}
    require(terminal,"ExplorerFrame fallback-file inventory exceeded the fixed 16-entry diagnostic bound");
}
void printMetadata(const char* route,const explorer::NamespaceCommandMetadata& metadata,const explorer::NamespaceCommandMetadataDiagnostics& d){
    std::cout<<"METADATA route="<<route<<" creator="<<d.creatorThread<<" initialized="<<d.initialized<<" siteSupplied="<<d.siteSupplied<<" siteAttached="<<d.siteAttached<<" diagnosticException="<<d.diagnosticException;
    printText("command",metadata.command.c_str());printText("resultLabel",metadata.label.c_str());printText("resultIcon",metadata.icon.c_str());
    printHr("metadataRegistryOpen",d.metadataRegistryOpen);printHr("providerRegistryOpen",d.providerRegistryOpen);printHr("handlerRead",d.handlerRead);printHr("handlerParse",d.handlerParse);printHr("providerCreate",d.providerCreate);printHr("nativeProviderCreate",d.nativeProviderCreate);printHr("objectQI",d.objectQuery);
    printHr("initializerQI",d.initializerQuery);printHr("propertyBagOpen",d.propertyBagOpen);printHr("Initialize",d.initialize);printHr("nativeInitialize",d.nativeInitialize);printHr("siteQI",d.siteQuery);printHr("SetSite",d.siteAttach);printHr("nativeSetSite",d.nativeSiteAttach);printHr("DetachSite",d.siteDetach);printHr("nativeDetachSite",d.nativeSiteDetach);printHr("providerLoad",d.providerLoad);printHr("returned",d.returned);std::cout<<'\n';
    const std::array<const explorer::NamespaceMetadataTextReceipt*,8> fields{&d.muiVerb,&d.defaultVerb,&d.description,&d.icon,&d.handlerText,&d.providerFields[0],&d.providerFields[1],&d.providerFields[2]};
    constexpr std::array names{"registryMUIVerb","registryDefault","registryDescription","registryIcon","registeredHandler","GetTitle","GetIcon","GetToolTip"};
    for(std::size_t i=0;i<fields.size();++i){std::cout<<"METADATA_FIELD route="<<route<<" field="<<names[i];printHr("status",fields[i]->status);
        printHr("wrappedReturn",fields[i]->returned);std::cout<<" attempted="<<fields[i]->attempted<<" present="<<fields[i]->present<<" truncated="<<fields[i]->truncated;printText("actualRawText",fields[i]->text.data());std::cout<<'\n';}
}

// One teardown owner spans every immutable/retained diagnostic interface.
// Its destructor runs the same retirement sequence on success or exception:
// frameworks -> retained image references -> original browser/site -> HWNDs.
struct SceneOwners {
    Window productionWindow;OwnedBrowser browser;Collector collector;
    explorer::NativeRibbon production;Independent independent;
    std::array<ImageRow,64> productionRows{};
    std::array<CurrentRow,64> independentRows{};
    std::shared_ptr<explorer::RibbonTextDiagnostics> text=std::make_shared<explorer::RibbonTextDiagnostics>();
    bool retired=false;
    ~SceneOwners(){retire();}
    void cleanupFence()noexcept{
        try{guard(productionWindow.value);guard(independent.window.value);if(browser.admitted)browser.verify();}
        catch(...){teardownFailed=true;}
    }
    void retire()noexcept{
        if(retired)return;retired=true;collector.retired=true;cleanupFence();
        explorer::forgetRibbonTheme(production.framework());production.reset();independent.retire();cleanupFence();
        for(auto& row:productionRows){releaseAndFence(row.current);releaseAndFence(row.returned);}
        for(auto& row:independentRows)releaseAndFence(row.image);
        for(auto& row:collector.rows){releaseAndFence(row.current);releaseAndFence(row.returned);}
        browser.retire();cleanupFence();
    }
    template<class T>void releaseAndFence(ComPtr<T>& value)noexcept{if(value){value.Reset();cleanupFence();}}
};

std::wstring protocolAssociation(ASSOCSTR field,const char* name){
    constexpr auto flags=static_cast<ASSOCF>(ASSOCF_IS_PROTOCOL|ASSOCF_NOTRUNCATE|ASSOCF_NOFIXUPS);
    DWORD characters=0;const auto sizeStatus=AssocQueryStringW(flags,field,L"ms-settings",nullptr,nullptr,&characters);
    std::cout<<"PROTOCOL_ASSOCIATION protocol=ms-settings field="<<name;printHr("actualSizeRead",sizeStatus);
    std::cout<<" nativeCharacters="<<characters<<" actualAppLaunchProtocol=1 stockExplorerIconAuthority=UNAVAILABLE\n";
    if(sizeStatus!=S_FALSE||characters<1||characters>32768)return {};
    std::vector<wchar_t> text(characters,L'\0');const auto readStatus=AssocQueryStringW(flags,field,L"ms-settings",nullptr,text.data(),&characters);
    std::cout<<"PROTOCOL_ASSOCIATION field="<<name;printHr("actualRead",readStatus);
    if(readStatus!=S_OK||!characters||characters>text.size()) {std::cout<<" evidence=UNAVAILABLE\n";return {};}
    const auto length=wcsnlen_s(text.data(),characters);if(length>=characters){std::cout<<" evidence=UNAVAILABLE_MALFORMED_OUTPUT\n";return {};}
    printText("actualNativeResource",text.data());std::cout<<'\n';return std::wstring(text.data(),length);
}
enum class Scene{Computer,Network,Recycle};
const char* sceneName(Scene value){return value==Scene::Computer?"Computer":value==Scene::Network?"Network":"Recycle";}
REFKNOWNFOLDERID sceneFolder(Scene value){return value==Scene::Computer?FOLDERID_ComputerFolder:value==Scene::Network?FOLDERID_NetworkFolder:FOLDERID_RecycleBinFolder;}
void run(Scene scene){
    std::cout<<"SCENE name="<<sceneName(scene)<<" diagnosticOnly=1\n";SceneOwners owners;
    auto& productionWindow=owners.productionWindow;auto& collector=owners.collector;auto& production=owners.production;
    auto& independent=owners.independent;auto& browser=owners.browser;guard(productionWindow.value);
    constexpr std::array observedIds{13634U,13648U,13586U,14593U,0x2500U,0x2501U,0x2502U,0x2503U,0x2510U,0x2512U,0x2513U,0x2515U,0x2900U,0x2901U,0x2910U,0x2911U};
    owners.text->filterCount=static_cast<UINT>(observedIds.size());owners.text->nativeCommands=observedIds;
    explorer::RibbonCallbacks callbacks;callbacks.observeImageRequest=[&](const explorer::RibbonImageObservation& event){collector.observe(event);};
    callbacks.textDiagnostics=owners.text;
    exact(production.initialize(productionWindow.value,GetModuleHandleW(nullptr),std::move(callbacks),explorer::RibbonLayout::InstalledWindows10),"Initialize actual production resource route");
    if(production.layout()!=explorer::RibbonLayout::InstalledWindows10)throw Unavailable{"installed 19045 resource admission is unavailable"};
    const auto features=production.features();UINT32 modes=0xa1|(features.discBurning?0x20000U:0x40000U);
    if(scene==Scene::Computer){exact(production.setComputerMode(true),"Set actual Computer template");modes=4|(features.mediaFoundation?0x2000U:0x4000U);}
    else if(scene==Scene::Network){exact(production.setNetworkMode(true,true),"Set actual Network template");modes=0x202;}
    independent.initialize(modes);
    if(scene==Scene::Recycle){exact(production.setContexts(explorer::RibbonContext::Recycle,true),"Activate actual Recycle context");UINT native=0,availability=0;
        exact(production.contextAvailable(explorer::RibbonContext::Recycle,native,availability),"Read actual Recycle context identifier");
        require(independent.handler->registered(native)&&availability==UI_CONTEXTAVAILABILITY_ACTIVE,"Independent native Recycle context is not registered/active");
        PROPVARIANT active{};exact(InitPropVariantFromUInt32(UI_CONTEXTAVAILABILITY_ACTIVE,&active),"Create exact context availability value");
        const auto set=independent.framework->SetUICommandProperty(native,UI_PKEY_ContextAvailable,active);PropVariantClear(&active);exact(set,"Activate source-matched independent context");}
    productionWindow.realize();independent.window.realize();auto* native=production.nativeFramework();require(native!=nullptr,"Production framework is missing");
    const auto epoch=production.callbackEntryEpoch();const auto factory=readColors(independent.framework.Get(),independent.window.value);
    const auto before=readColors(production.framework(),productionWindow.value);const auto themeApplied=explorer::applyRibbonTheme(production.framework());
    const auto windowThemeApplied=explorer::applyWindowTheme(productionWindow.value);guard(productionWindow.value);
    const auto after=readColors(production.framework(),productionWindow.value);printColors("independentFactory",factory);printColors("productionBeforeAppTheme",before);printColors("productionAfterAppTheme",after);printHr("applyRibbonTheme",themeApplied);printHr("applyWindowTheme",windowThemeApplied);std::cout<<" actualAppInitializeOrder=RibbonThenOwnedWindow\n";
    nativeThemeFacts(productionWindow.value);
    const auto theme=explorer::themeState();std::cout<<"THEME requested="<<static_cast<int>(theme.requested)<<" actual="<<static_cast<int>(theme.actual)<<" highContrast="<<theme.highContrast<<" appsUseLight="<<theme.appsUseLightTheme<<" nativeDarkAvailable="<<theme.nativeDarkAvailable<<" activeOwned="<<(GetActiveWindow()==productionWindow.value)<<" dpi="<<GetDpiForWindow(productionWindow.value)<<" IsThemeActive="<<IsThemeActive()<<" IsAppThemed="<<IsAppThemed()<<'\n';
    browser.initialize(productionWindow.value,sceneFolder(scene));browser.verify();
    std::cout<<"RESOURCE_MODE actualModes="<<modes<<" modeDerivedFromActualFeatures=1 contextualAvailabilityRead="<<(scene==Scene::Recycle)
        <<" actualTabSelectionWitness=UNAVAILABLE protocol=registered-native-property-invalidation noRenderedTabClaim=1\n";
    for(UINT i=0;i<independent.handler->registeredCount;++i){const auto id=independent.handler->registrations[i].native;
        if(!observedTextId(id))continue;for(const auto p:{Property::SmallImage,Property::LargeImage,Property::Label,Property::Tooltip}){
            if((p==Property::SmallImage||p==Property::LargeImage)&&!target(id))continue;const auto& key=propertyKey(p);
            const auto stock=independent.framework->InvalidateUICommand(id,UI_INVALIDATIONS_PROPERTY,&key);
            const auto app=native->InvalidateUICommand(id,UI_INVALIDATIONS_PROPERTY,&key);std::cout<<"REQUEST native="<<id<<" property="<<propertyName(p);
            printHr("independent",stock);printHr("production",app);std::cout<<'\n';}}
    exact(independent.framework->FlushPendingInvalidations(),"Return from all independent real invalidations");exact(production.flush(),"Return from all production real invalidations");
    independent.window.realize();productionWindow.realize();browser.verify();require(!collector.overflow&&!independent.handler->overflow,"Fixed resource receipt bounds were exceeded");
    explorer::RibbonImageObservationStats stats;exact(production.imageObservationStats(stats),"Read actual image callback coverage");
    require(stats.dropped==0&&stats.reentrant==0,"Actual production image evidence was dropped/reentrant");
    // Freeze by move, not AddRef. Native callback collectors remain alive and
    // empty while external provider, image and metadata reads may pump.
    auto& productionRows=owners.productionRows;for(UINT i=0;i<collector.count;++i)productionRows[i]=std::move(collector.rows[i]);const UINT productionCount=collector.count;collector.count=0;
    auto& independentRows=owners.independentRows;const UINT independentCount=independent.handler->count;
    for(UINT i=0;i<independentCount;++i)independentRows[i]=std::move(independent.handler->rows[i]);
    // Independent reservation remains occupied by its scalar native/key/first
    // markers. Moving interfaces never creates another first-current slot.
    const auto independentRequests=independent.handler->requests;const auto targetRequests=collector.targetRequests;
    require(!owners.text->overflow,"Plain first-current/returned label receipts exceeded the fixed bound");
    const auto textRequests=owners.text->requests;const auto textCount=owners.text->count;
    const auto textRows=owners.text->receipts; // Plain values, no native retention/API call.
    const auto coherent=[&]{browser.verify();guard(independent.window.value);require(production.nativeFramework()==native&&production.callbackEntryEpoch()==epoch,
        "Actual production binding changed during external readback");explorer::RibbonImageObservationStats current;exact(production.imageObservationStats(current),"Re-read real image coverage after native calls");
        require(current.requests==stats.requests&&current.delivered==stats.delivered&&current.dropped==0&&current.reentrant==0&&collector.count==0&&
            collector.targetRequests==targetRequests&&independent.handler->requests==independentRequests&&
            owners.text->requests==textRequests&&owners.text->count==textCount&&!owners.text->overflow,
            "Late native callbacks invalidated immutable resource evidence");
        exact(independent.apartmentClient.observeLoadedCode(),"Observe actual independent readback code lifetime");};
    for(UINT i=0;i<textCount;++i){const auto& row=textRows[i];
        require(row.bindingStable&&row.creatorThread==GetCurrentThreadId()&&row.window==productionWindow.value&&
            !row.current.truncated&&!row.returned.truncated,"Actual production text receipt lost source or exact bounded text");
        std::cout<<"PRODUCTION_FIRST_TEXT native="<<row.nativeCommand<<" application="<<row.applicationCommand<<" property="<<(row.label?"Label":"TooltipTitle")
            <<" ordinal="<<row.ordinal<<" currentPresent="<<row.current.present<<" currentVT="<<row.current.type<<" returnedVT="<<row.returned.type;
        printHr("normalResult",row.normalResult);std::cout<<" currentStringPresent="<<row.current.stringPresent<<" returnedStringPresent="<<row.returned.stringPresent;
        printText("actualFirstCurrent",row.current.text.data());printText("actualNormalReturned",row.returned.text.data());
        std::cout<<" entryEpoch="<<row.entryEpoch<<" exitEpoch="<<row.exitEpoch<<" bindingStable="<<row.bindingStable<<" normalPrecedenceUnchanged=1\n";
    }
    for(UINT i=0;i<independentCount;++i){const auto& row=independentRows[i];require(row.stable&&!row.truncated,"Actual independent first current was truncated or lost its owned binding");
        std::cout<<"FIRST_CURRENT native="<<row.native<<" property="<<propertyName(row.property)<<" ordinal="<<row.ordinal<<" currentPresent="<<row.present<<" currentVT="<<row.type;
        std::cout<<" creator="<<row.creator<<" window="<<reinterpret_cast<std::uintptr_t>(row.window)<<" windowGeneration="<<row.windowGeneration<<" actualDpi="<<row.dpi;
        printHr("currentQI",row.imageQuery);printHr("returned",row.returned);printText("actualFirstText",row.text.data());std::cout<<" firstOnly=1\n";
        if(row.image){const auto pixels=actualPixels(row.image.Get(),independent.window.value);printPixels("independentGenuineFirst",pixels);coherent();}}
    // Each group and command text row compares both genuine first inputs and
    // normal returned output. Different inputs prohibit a displacement claim.
    for(const auto id:observedIds)for(const auto p:{Property::Label,Property::Tooltip}){
        const CurrentRow* first=nullptr;const explorer::RibbonTextReceipt* returned=nullptr;
        for(UINT i=0;i<independentCount;++i)if(independentRows[i].native==id&&independentRows[i].property==p)first=&independentRows[i];
        for(UINT i=0;i<textCount;++i)if(textRows[i].nativeCommand==id&&textRows[i].label==(p==Property::Label))returned=&textRows[i];
        std::cout<<"TEXT_SOURCE_COMPARE native="<<id<<" property="<<propertyName(p)<<" independentlyRegistered="<<independent.handler->registered(id)
            <<" independentCallbackPresent="<<(first!=nullptr)<<" productionCallbackPresent="<<(returned!=nullptr);
        const bool independentText=first&&first->type==VT_LPWSTR&&first->stringPresent;
        const bool productionInput=returned&&returned->current.type==VT_LPWSTR&&returned->current.stringPresent;
        const bool productionOutput=returned&&returned->normalResult==S_OK&&returned->returned.type==VT_LPWSTR&&returned->returned.stringPresent;
        std::cout<<" genuineInputs="<<(!independentText||!productionInput?"UNAVAILABLE":wcscmp(first->text.data(),returned->current.text.data())==0?"SAME":"DIFFERENT");
        std::cout<<" normalReturnAgainstIndependent="<<(!independentText||!productionOutput?"UNAVAILABLE":wcscmp(first->text.data(),returned->returned.text.data())==0?"SAME":"DIFFERENT")
            <<" noDisplacementClaimWithoutEquivalentInputs=1\n";
    }
    for(const auto& command:targets){const bool intended=(scene==Scene::Computer&&(command.native==13634||command.native==13648))||
            (scene==Scene::Network&&command.native==13586)||(scene==Scene::Recycle&&command.native==14593);if(!intended)continue;
        const auto mapped=production.nativeCommandId(command.application);std::cout<<"COMMAND name="<<command.name<<" native="<<command.native<<" application="<<command.application<<" actualMappedNative="<<mapped<<" actualIndependentRegistered="<<independent.handler->registered(command.native)<<'\n';
        require(mapped==command.native,"Actual production command mapping differs from the recorded physical command");
        for(const auto p:{Property::SmallImage,Property::LargeImage,Property::Label,Property::Tooltip}){
            const CurrentRow* actualFirst=nullptr;for(UINT i=0;i<independentCount;++i)if(independentRows[i].native==command.native&&independentRows[i].property==p)actualFirst=&independentRows[i];
            std::cout<<"FIRST_CURRENT_COVERAGE native="<<command.native<<" property="<<propertyName(p)<<" actualRegistered="<<independent.handler->registered(command.native)
                <<" actualCallbackPresent="<<(actualFirst!=nullptr)<<" evidence="<<(!actualFirst?"UNAVAILABLE_CALLBACK":
                (p==Property::SmallImage||p==Property::LargeImage)?(actualFirst->image?"GENUINE_FIRST_IMAGE":"UNAVAILABLE_GENUINE_FIRST_IMAGE"):
                (actualFirst->type==VT_LPWSTR&&actualFirst->stringPresent?(actualFirst->text.front()?"GENUINE_FIRST_TEXT":"GENUINE_FIRST_EMPTY_TEXT"):"UNAVAILABLE_GENUINE_FIRST_TEXT"))<<'\n';
            if(p==Property::Label||p==Property::Tooltip){const explorer::RibbonTextReceipt* actualReturn=nullptr;
                for(UINT i=0;i<textCount;++i)if(textRows[i].nativeCommand==command.native&&textRows[i].label==(p==Property::Label))actualReturn=&textRows[i];
                std::cout<<"TEXT_COMPARE native="<<command.native<<" property="<<propertyName(p)<<" actualProductionCallbackPresent="<<(actualReturn!=nullptr);
                if(!actualFirst||!actualReturn||actualFirst->type!=VT_LPWSTR||!actualFirst->stringPresent||actualReturn->returned.type!=VT_LPWSTR||!actualReturn->returned.stringPresent||actualReturn->normalResult!=S_OK)
                    std::cout<<" evidence=UNAVAILABLE";
                else std::cout<<" evidence="<<(wcscmp(actualFirst->text.data(),actualReturn->returned.text.data())==0?"SAME":"DIFFERENT");std::cout<<'\n';}
        }
        const auto key=explorer::ribbonCommandStoreName(command.application);
        explorer::NamespaceCommandMetadata baseline,scoped;explorer::NamespaceCommandMetadataDiagnostics baselineReceipt,scopedReceipt;
        HRESULT baselineStatus=HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),scopedStatus=HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        std::wstring protocolDefaultIcon,protocolAppIcon;
        if(!key.empty()){
            baselineStatus=explorer::namespaceCommandMetadataWithDiagnostics(key,&baseline,nullptr,nullptr,&baselineReceipt);coherent();printMetadata("noSelectionNoSite",baseline,baselineReceipt);
            explorer::NamespaceCommandMetadata emptySelection;explorer::NamespaceCommandMetadataDiagnostics emptyReceipt;
            if(browser.originalSelection){const auto actualEmptyStatus=explorer::namespaceCommandMetadataWithDiagnostics(key,&emptySelection,browser.originalSelection.Get(),browser.view.Get(),&emptyReceipt);
                coherent();printMetadata("actualOriginalEmptySelectionAndSite",emptySelection,emptyReceipt);printHr("actualEmptySelectionStatus",actualEmptyStatus);std::cout<<'\n';}
            else std::cout<<"METADATA route=actualOriginalEmptySelectionAndSite evidence=UNAVAILABLE_NATIVE_EMPTY_ARRAY noManufacturedArray=1\n";
            scopedStatus=explorer::namespaceCommandMetadataWithDiagnostics(key,&scoped,browser.background.Get(),browser.view.Get(),&scopedReceipt);coherent();printMetadata("actualViewBackgroundArrayAndSite",scoped,scopedReceipt);
        }else{
            // Open Settings is an actual ms-settings: protocol launch in App,
            // with no entry in production's CommandStore map. Never fabricate
            // Windows.OpenSettings or claim a provider GetIcon occurred.
            std::cout<<"METADATA native="<<command.native<<" evidence=UNAVAILABLE_PRODUCTION_COMMANDSTORE_NAME noGuessedProvider=1\n";
            if(command.application==explorer::RibbonOpenSettings){protocolDefaultIcon=protocolAssociation(ASSOCSTR_DEFAULTICON,"DefaultIcon");coherent();
                protocolAppIcon=protocolAssociation(ASSOCSTR_APPICONREFERENCE,"AppIconReference");coherent();}
        }
        std::cout<<"ROUTE_AUTHORITY onlyDocumentedNativeScope=1 actualStockExplorerInvocationRoute=UNAVAILABLE noStateOrEligibilityOverride=1\n";
        std::wstring label;const auto labelStatus=production.commandLabel(command.application,label);coherent();std::cout<<"PRODUCTION_LABEL native="<<command.native;
        printHr("publicRead",labelStatus);printText("actualReadText",label.c_str());std::cout<<" unsupportedIsNotTextProof=1\n";
        for(const bool large:{false,true}){const UINT pixels=large?32U:16U;const ImageRow* first=nullptr;
            for(UINT i=0;i<productionCount;++i){const auto& row=productionRows[i];if(row.metadata.nativeCommand==command.native&&row.metadata.large==large&&(!first||row.metadata.ordinal<first->metadata.ordinal))first=&row;}
            std::cout<<"PRODUCTION_IMAGE native="<<command.native<<" large="<<large<<" actualCallbackPresent="<<(first!=nullptr);
            if(first){require(first->metadata.applicationCommand==command.application&&first->metadata.completion&&first->metadata.completion->bindingStable&&!first->metadata.completion->observerThrew&&
                    first->metadata.completion->result==first->metadata.normalUpdateResult,"Production callback lost its true source/completion");
                std::cout<<" path="<<static_cast<int>(first->metadata.path)<<" genuineFirst="<<first->metadata.nativeFirstObservation<<" nativeImageCached="<<first->metadata.nativeImageCached<<" suppliedVT="<<first->metadata.currentType;
                printHr("normalReturn",first->metadata.normalUpdateResult);printHr("suppliedQI",first->metadata.currentImageQuery);}
            std::cout<<'\n';Pixels actual;if(first&&first->returned)actual=actualPixels(first->returned.Get(),productionWindow.value);else actual.status=E_NOINTERFACE;coherent();
            ComPtr<IUIImage> fallback;const auto fallbackStatus=production.commandImage(command.application,large,&fallback);coherent();Pixels explicitFallback;
            if(fallbackStatus==S_OK&&fallback)explicitFallback=actualPixels(fallback.Get(),productionWindow.value);else explicitFallback.status=fallbackStatus;coherent();
            const auto fromBaseline=baselineStatus==S_OK?providerPixels(baseline.icon,pixels,productionWindow.value):Pixels{baselineStatus,0,0,{}};coherent();
            const auto fromScoped=scopedStatus==S_OK?providerPixels(scoped.icon,pixels,productionWindow.value):Pixels{scopedStatus,0,0,{}};coherent();
            if(command.application==explorer::RibbonOpenSettings){
                const auto fromProtocolDefault=providerPixels(protocolDefaultIcon,pixels,productionWindow.value);coherent();
                const auto fromProtocolApp=providerPixels(protocolAppIcon,pixels,productionWindow.value);coherent();
                printPixels("actual-ms-settings-DefaultIconAssociation",fromProtocolDefault);printPixels("actual-ms-settings-AppIconReference",fromProtocolApp);
                compare("actualUpdatePropertyReturned",actual,"actual-ms-settings-DefaultIconAssociation",fromProtocolDefault);
                compare("actualUpdatePropertyReturned",actual,"actual-ms-settings-AppIconReference",fromProtocolApp);
            }
            printPixels("actualUpdatePropertyReturned",actual);printPixels("actualCommandFallbackApi",explicitFallback);printPixels("baselineProviderResource",fromBaseline);printPixels("scopedProviderResource",fromScoped);
            compare("actualUpdatePropertyReturned",actual,"actualCommandFallbackApi",explicitFallback);compare("actualUpdatePropertyReturned",actual,"baselineProviderResource",fromBaseline);compare("actualUpdatePropertyReturned",actual,"scopedProviderResource",fromScoped);
            fallback.Reset();coherent(); // Fence actual native Release before next result is admitted.
        }}
    coherent();std::cout<<"SCENE_DIAGNOSTIC_COMPLETE name="<<sceneName(scene)<<" productionTargetCallbacks="<<productionCount<<" independentFirstRows="<<independentCount<<" allSourceFences=1 parityClaim=0\n";
    // Retire owned native frameworks while all original native sources and
    // retained callback image interfaces remain alive on their creator STA.
    owners.retire();require(!teardownFailed,"Native source/frame/image retirement failed its final source guard");
    guard(productionWindow.value);guard(independent.window.value);
}
} // namespace

int wmain(int count,wchar_t** arguments){
    // Unknown/missing arguments are rejected before private desktop, COM,
    // process theme policy, HWND, registry provider or module initialization.
    if(count!=2||!arguments||!arguments[1]||wcscmp(arguments[1],L"--owned-read-only")!=0){std::cerr<<"Usage: native_resource_oracle --owned-read-only\n";return 2;}
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOOPENFILEERRORBOX);SetLastError(ERROR_SUCCESS);
    const auto dpiSet=SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);const auto dpiError=GetLastError();
    const auto dpiContext=GetThreadDpiAwarenessContext();const auto dpiAwareness=GetAwarenessFromDpiAwarenessContext(dpiContext);
    const auto actualV2=AreDpiAwarenessContextsEqual(dpiContext,DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    std::cout<<"DPI_ADMISSION actualSetResult="<<dpiSet<<" actualSetError="<<dpiError<<" actualAwareness="<<dpiAwareness
        <<" actualContext="<<reinterpret_cast<std::uintptr_t>(dpiContext)<<" actualPerMonitorV2="<<actualV2<<'\n';
    if(dpiAwareness!=DPI_AWARENESS_PER_MONITOR_AWARE||!actualV2){std::cout<<"UNAVAILABLE: actual process PMv2 DPI-awareness admission\n";return 77;}
    explorer::PrivateDesktop desktop;const auto isolated=desktop.initialize();if(FAILED(isolated)){std::cerr<<"FAIL: private desktop HRESULT="<<static_cast<ULONG>(isolated)<<'\n';return 3;}
    explorer::NativeApartmentOwner nativeApartment;const auto initialized=nativeApartment.initializeOle();if(FAILED(initialized)){std::cerr<<"FAIL: creator STA HRESULT="<<static_cast<ULONG>(initialized)<<'\n';return 4;}
    int result=0;bool completed=false;deadline=GetTickCount64()+65000;
    try{APTTYPE apartment{};APTTYPEQUALIFIER qualifier{};exact(CoGetApartmentType(&apartment,&qualifier),"Read actual creator COM apartment");
        require(apartment==APTTYPE_STA||apartment==APTTYPE_MAINSTA,"Native resource oracle creator is not STA");
        INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES|ICC_WIN95_CLASSES};require(InitCommonControlsEx(&controls)!=FALSE,"Initialize native owned controls");
        exact(explorer::initializeProcessTheme(explorer::ThemeMode::Auto),"Initialize actual App process-local theme policy");
        WNDCLASSW type{};type.hInstance=GetModuleHandleW(nullptr);type.lpfnWndProc=Window::procedure;type.lpszClassName=hostClass;type.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
        require(RegisterClassW(&type)!=0,"Register only native resource oracle owned HWND class");languageFacts();
        for(const auto scene:{Scene::Computer,Scene::Network,Scene::Recycle})run(scene);guard();completed=true;
    }catch(const Unavailable& missing){std::cout<<"UNAVAILABLE: "<<missing.message<<" parityClaim=0\n";result=77;}
    catch(const std::exception& failure){std::cerr<<"FAIL: "<<failure.what()<<'\n';result=1;}
    catch(...){std::cerr<<"FAIL: unexpected exception retired through owned RAII\n";result=1;}
    // All scene-local native clients/interfaces/windows have completed their
    // original creator-STA retirement, including exception paths.
    const auto now=GetTickCount64();const DWORD budget=now<deadline?static_cast<DWORD>(std::min<ULONGLONG>(2000,deadline-now)):0;
    const auto drained=explorer::drainStaWorkers(budget);if(drained!=S_OK||explorer::pendingStaWorkers()){
        std::cerr<<"FAIL: original creator native worker drain HRESULT="<<static_cast<ULONG>(drained)<<'\n';result=1;}
    const auto beforeFinish=nativeApartment.readback();
    if(beforeFinish.clients){std::cerr<<"FAIL: native apartment clients survived owned retirement\n";result=1;}
    nativeApartment.finishOrTerminate();const auto finished=nativeApartment.readback();
    if(finished.active||finished.depth||finished.clients||finished.heldCode||finished.codeAcquisitions!=finished.codeReleases||FAILED(finished.failure)){
        std::cerr<<"FAIL: paired native apartment/code owner final readback\n";result=1;}
    std::cout<<"APARTMENT_FINAL active="<<finished.active<<" depth="<<finished.depth<<" clients="<<finished.clients
        <<" codeAcquisitions="<<finished.codeAcquisitions<<" codeReleases="<<finished.codeReleases<<" postApartmentHRESULT="<<static_cast<ULONG>(finished.postApartmentRead)<<'\n';
    if(teardownFailed){std::cerr<<"FAIL: native browser/framework/window retirement failed\n";result=1;}
    if(!result&&completed){try{guard();std::cout<<"DIAGNOSTIC_COMPLETE: all native objects/windows retired before this receipt; missing-current/route/color facts remain explicit; parityClaim=0\n";}
        catch(const std::exception& failure){std::cerr<<"FAIL: final isolation/deadline "<<failure.what()<<'\n';result=1;}}
    return result;
}
