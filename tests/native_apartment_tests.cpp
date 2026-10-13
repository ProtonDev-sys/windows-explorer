#include "explorer/native_apartment.hpp"
#include "explorer/headless_visual.hpp"
#include "explorer/namespace_actions.hpp"
#include "explorer/ribbon.hpp"
#include <commctrl.h>
#include <shlobj.h>
#include <objbase.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <string>
#include <thread>

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {
using Microsoft::WRL::ComPtr;
ULONGLONG deadline=0;
void require(bool value,const char* text){if(!value)throw std::runtime_error(text);}
void exact(HRESULT value,const char* text){if(value!=S_OK)throw std::runtime_error(std::string(text)+" HRESULT="+std::to_string(static_cast<ULONG>(value)));}
void apartmentStillSta(){APTTYPE type{};APTTYPEQUALIFIER qualifier{};exact(CoGetApartmentType(&type,&qualifier),"Read actual remaining STA");require(type==APTTYPE_STA||type==APTTYPE_MAINSTA,"Rejected retirement changed native STA");}
void isolate(const explorer::PrivateDesktop& desktop){bool visible=true;exact(desktop.verifyIsolation(),"Observe original private desktop");exact(desktop.visibleWindowsOnInputDesktop(visible),"Observe input-desktop windows");require(!visible&&GetTickCount64()<deadline,"Apartment fixture crossed visibility/deadline bound");}

template<class Body> void worker(Body body) {
    std::exception_ptr failure;
    std::thread owned([&]{try{explorer::PrivateDesktop desktop;exact(desktop.initialize(),"Create worker-owned private desktop");body(desktop);isolate(desktop);}catch(...){failure=std::current_exception();}});
    HANDLE terminated=nullptr;
    if(!DuplicateHandle(GetCurrentProcess(),owned.native_handle(),GetCurrentProcess(),&terminated,SYNCHRONIZE,FALSE,0)){
        TerminateProcess(GetCurrentProcess(),8);std::_Exit(8);
    }
    for(;;) {
        const auto signalled=WaitForSingleObject(terminated,0);
        if(signalled==WAIT_OBJECT_0)break;
        if(signalled!=WAIT_TIMEOUT){TerminateProcess(GetCurrentProcess(),8);std::_Exit(8);}
        if(GetTickCount64()>=deadline){TerminateProcess(GetCurrentProcess(),8);std::_Exit(8);}
        DWORD index=0;
        const auto waited=CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS|COWAIT_DISPATCH_WINDOW_MESSAGES|COWAIT_INPUTAVAILABLE,
            20,1,&terminated,&index);
        if(waited!=RPC_S_CALLPENDING&&FAILED(waited)){TerminateProcess(GetCurrentProcess(),8);std::_Exit(8);}
    }
    owned.join();require(CloseHandle(terminated)!=FALSE,"Close exact fixture thread readback handle");
    if(failure)std::rethrow_exception(failure);
}

void basicContract(explorer::NativeApartmentOwner& outer) {
    const auto before=outer.readback();require(before.active&&before.depth==1&&before.clients==0,"Initial outer owner admission");
    {
        explorer::NativeApartmentOwner nestedCo,nestedOle;
        require(nestedCo.initializeSta()==S_FALSE&&nestedCo.nativeInitializationResult()==S_FALSE,"Nested real CoInitialize S_FALSE lost");
        require(nestedOle.initializeOle()==S_FALSE&&nestedOle.nativeInitializationResult()==S_FALSE,"Nested real OleInitialize S_FALSE lost");
        require(outer.readback().depth==3,"Nested apartment owner did not share outer state");
        require(outer.finish()==HRESULT_FROM_WIN32(ERROR_INVALID_STATE),"Out-of-order outer finish was admitted");
        apartmentStillSta();exact(nestedOle.finish(),"Balance nested Ole call");exact(nestedCo.finish(),"Balance nested Co call");
    }
    {
        explorer::NativeApartmentClient nativeClient;
        exact(nativeClient.acquire(),"Reserve real owner native client");
        require(outer.finish()==E_PENDING&&outer.readback().clients==1,"Outer teardown accepted a live native client");
        apartmentStillSta();
    }
    require(outer.readback().clients==0&&outer.readback().depth==1,"Native client/nested scope retirement count");
    struct ExpectedUnwind {};
    bool unwound=false;
    try {
        explorer::NativeApartmentOwner nested;
        require(nested.initializeSta()==S_FALSE,"Initialize genuine nested exception control");
        throw ExpectedUnwind{};
    }catch(const ExpectedUnwind&){unwound=true;}
    apartmentStillSta();
    require(unwound&&outer.readback().active&&outer.readback().depth==1&&outer.readback().codeReleases==0,
        "Nested exception unwinding lost the original apartment/module ownership");
    explorer::NativeApartmentOwner originalThread;
    worker([&](const explorer::PrivateDesktop&) {
        require(originalThread.initializeOle()==RPC_E_WRONG_THREAD&&originalThread.finish()==RPC_E_WRONG_THREAD,
            "Wrong-thread owner call reached native initialization/teardown");
    });
    require(originalThread.nativeInitializationResult()==E_PENDING&&!originalThread.readback().active,"Wrong-thread call changed original state");
    worker([&](const explorer::PrivateDesktop&) {
        exact(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"Actual MTA negative-control initialization");
        struct ExternalCount {~ExternalCount(){CoUninitialize();}} originalCount;
        explorer::NativeApartmentOwner incompatible;
        require(incompatible.initializeOle()==RPC_E_CHANGED_MODE&&incompatible.nativeInitializationResult()==RPC_E_CHANGED_MODE,
            "Real incompatible initialization status was replaced");
        APTTYPE type{};APTTYPEQUALIFIER qualifier{};exact(CoGetApartmentType(&type,&qualifier),"Read original MTA after failed Ole call");
        require(type==APTTYPE_MTA&&!incompatible.readback().active,"Failed Ole initialization decremented original MTA");
    });
    worker([&](const explorer::PrivateDesktop&) {
        explorer::NativeApartmentOwner coOuter;
        exact(coOuter.initializeSta(),"Initialize actual outer Co STA");
        explorer::NativeApartmentOwner firstOle;
        exact(firstOle.initializeOle(),"Admit first actual OleInitialize within owned Co STA");
        require(firstOle.nativeInitializationResult()==S_OK&&coOuter.readback().depth==2,
            "Mixed owned Co-to-first-Ole initialization status lost");
        exact(firstOle.finish(),"Balance first inner Ole call");apartmentStillSta();
        exact(coOuter.finish(),"Balance original outer Co call");
    });
    worker([&](const explorer::PrivateDesktop&) {
        exact(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED),"Actual external unowned STA initialization");
        struct ExternalCount {~ExternalCount(){CoUninitialize();}} originalCount;
        explorer::NativeApartmentOwner rejected;
        require(rejected.initializeSta()==HRESULT_FROM_WIN32(ERROR_INVALID_STATE)&&rejected.nativeInitializationResult()==S_FALSE,
            "Unowned pre-existing apartment was adopted as a final owner");
        apartmentStillSta();require(!rejected.readback().active,"Rejected owner still owns an apartment call");
        explorer::NativeApartmentOwner firstOle;
        require(firstOle.initializeOle()==HRESULT_FROM_WIN32(ERROR_INVALID_STATE)&&firstOle.nativeInitializationResult()==S_OK,
            "Unowned Co STA was adopted after first successful Ole call");
        apartmentStillSta();require(!firstOle.readback().active,"Rejected first Ole wrapper retained an owned count");
    });
    const auto current=outer.readback();
    require(current.active&&current.depth==1&&current.clients==0&&SUCCEEDED(current.failure)&&
        current.codeAcquisitions>=before.codeAcquisitions&&current.codeAcquisitions-before.codeAcquisitions<=1&&
        current.codeAcquisitions<=1&&current.codeReleases==before.codeReleases&&current.codeReleases==0,
        "Basic ownership scope displaced or prematurely released a bounded normal code reference");
    if(current.codeAcquisitions) {
        MEMORY_BASIC_INFORMATION memory{};
        require(current.heldCode&&current.verifiedCode&&
            VirtualQuery(current.heldCode,&memory,sizeof(memory))==sizeof(memory)&&memory.State==MEM_COMMIT&&
            memory.Type==MEM_IMAGE&&memory.AllocationBase==current.heldCode,
            "Already-loaded executable reference lacks actual verified held mapping");
    }else require(!current.heldCode&&!current.verifiedCode,
        "Basic ownership reports executable state without an acquired normal reference");
}

struct OwnedFile {
    std::filesystem::path path;bool created=false;
    OwnedFile(){try{std::array<wchar_t,MAX_PATH> root{};const auto length=GetTempPathW(static_cast<DWORD>(root.size()),root.data());
        require(length&&length<root.size(),"Read bounded owned fixture temp root");
        path=std::filesystem::path(root.data())/(L"ExplorerApartment-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L".txt");
        require(path.is_absolute()&&path.native().size()<MAX_PATH,"Owned module fixture path bound");
        const auto file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY,nullptr);
        require(file!=INVALID_HANDLE_VALUE,"Exclusively create owned module selection file");created=true;
        constexpr char content[]="Owned read-only module lifetime selection";DWORD written=0;
        const bool wrote=WriteFile(file,content,sizeof(content)-1,&written,nullptr)&&written==sizeof(content)-1;
        const bool closed=CloseHandle(file)!=FALSE;require(wrote&&closed,"Write/close exclusively owned module fixture file");}
        catch(...){if(created)DeleteFileW(path.c_str());throw;}}
    ~OwnedFile(){if(created)DeleteFileW(path.c_str());}
};
void actualMetadata(const OwnedFile& file) {
    ComPtr<IShellItem> item;ComPtr<IShellItemArray> selection;
    exact(SHCreateItemFromParsingName(file.path.c_str(),nullptr,IID_PPV_ARGS(&item)),"Resolve actual owned native selection");
    exact(SHCreateShellItemArrayFromShellItem(item.Get(),IID_PPV_ARGS(&selection)),"Retain actual owned native array");
    explorer::NamespaceCommandMetadata metadata;
    exact(explorer::namespaceCommandMetadata(L"Windows.open",&metadata,selection.Get()),"Ask actual Open provider for read-only metadata");
}
// Use the same compiled APPLICATION_RIBBON resource scaffold as the real
// app and installed-Ribbon tests. Only production NativeRibbon loads native
// resources/activates registered providers; this fixture never loads or pins
// executable code, invents a provider or invokes a command.
struct OwnedRibbonWindow {
    HINSTANCE instance=GetModuleHandleW(nullptr);
    std::wstring className=L"ExplorerApartmentModuleHost-"+std::to_wstring(GetCurrentThreadId());
    ATOM atom=0;HWND handle=nullptr;
    OwnedRibbonWindow() {
        INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES|ICC_WIN95_CLASSES};
        require(InitCommonControlsEx(&controls)!=FALSE,"Initialize actual module-host common controls");
        WNDCLASSW type{};type.lpfnWndProc=DefWindowProcW;type.hInstance=instance;
        type.lpszClassName=className.c_str();type.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
        atom=RegisterClassW(&type);require(atom!=0,"Register creator-owned module-host class");
        handle=CreateWindowExW(0,className.c_str(),L"Owned module lifetime Ribbon",WS_OVERLAPPEDWINDOW,
            0,0,1000,700,nullptr,nullptr,instance,nullptr);
        if(!handle){UnregisterClassW(className.c_str(),instance);atom=0;require(false,"Create creator-owned private module-host HWND");}
    }
    ~OwnedRibbonWindow() {
        if(handle&&!DestroyWindow(handle))std::terminate();
        if(atom&&!UnregisterClassW(className.c_str(),instance))std::terminate();
    }
    void destroy() {
        require(DestroyWindow(handle)!=FALSE,"Destroy exact original module-host HWND");handle=nullptr;
    }
};
bool actualRibbonMetadata(const OwnedFile& file,explorer::NativeApartmentOwner& owner,
                          const explorer::PrivateDesktop& desktop) {
    isolate(desktop);
    OwnedRibbonWindow window;bool installed=false;
    {
        explorer::NativeRibbon ribbon;
        exact(ribbon.initialize(window.handle,window.instance,{},explorer::RibbonLayout::InstalledWindows10),
            "Load genuine production native Ribbon with compiled resource scaffold");
        require(ribbon.valid()&&ribbon.nativeFramework(),"Actual production LoadUI supplied no native Ribbon view");
        installed=ribbon.layout()==explorer::RibbonLayout::InstalledWindows10&&ribbon.installedLayoutStatus()==S_OK;
        DWORD process=0;
        require(GetWindowThreadProcessId(window.handle,&process)==GetCurrentThreadId()&&process==GetCurrentProcessId()&&
            !IsWindowVisible(window.handle),"Actual Ribbon escaped its original hidden creator-owned HWND");
        actualMetadata(file);isolate(desktop);
        const auto live=owner.readback();
        require(live.active&&live.depth==1&&live.clients==1&&owner.finish()==E_PENDING,
            "Creator retirement admitted the actual live NativeRibbon client");
        apartmentStillSta();
        std::cout<<"ACTUAL_NATIVE_RIBBON creator="<<live.creator<<" installed="<<installed
            <<" clients="<<live.clients<<" codeAcquisitions="<<live.codeAcquisitions<<'\n';
        ribbon.reset();
        require(!ribbon.valid(),"Production native Ribbon reset did not retire its publication");
        window.destroy();ribbon.finishOwnerWindowRetirement();
    }
    isolate(desktop);
    require(owner.readback().clients==0,"Actual Ribbon/provider interfaces survived original HWND retirement");
    if(!installed)std::cout<<"UNAVAILABLE: actual installed EXPLORER_RIBBON binding unavailable; module coverage zero\n";
    return installed;
}
bool creatorActuallyRetired(const explorer::NativeApartmentReadback& state) noexcept {
    return state.postApartmentRead==CO_E_NOTINITIALIZED||
        (state.postApartmentRead==S_OK&&state.postApartmentType==APTTYPE_MTA&&
         state.postApartmentQualifier==APTTYPEQUALIFIER_IMPLICIT_MTA);
}
void heldAcrossNested(explorer::NativeApartmentOwner& owner) {
    const auto held=owner.readback();
    explorer::NativeApartmentOwner nestedCo,nestedOle;
    require(nestedCo.initializeSta()==S_FALSE&&nestedOle.initializeOle()==S_FALSE,"Actual module-held nested native counts missing");
    require(owner.readback().depth==3&&owner.readback().heldCode==held.heldCode&&owner.readback().codeReleases==0,
        "Nested admission displaced original held code");
    exact(nestedOle.finish(),"Retire actual module-held nested Ole scope");
    require(owner.readback().depth==2&&owner.readback().heldCode==held.heldCode&&owner.readback().codeReleases==0,
        "Nested OleUninitialize released outer held code");
    exact(nestedCo.finish(),"Retire actual module-held nested Co scope");apartmentStillSta();
    const auto remaining=owner.readback();
    require(remaining.active&&remaining.depth==1&&remaining.heldCode==held.heldCode&&
        remaining.codeAcquisitions==1&&remaining.codeReleases==0,"Nested CoUninitialize released outer held code");
}
bool moduleContract(explorer::NativeApartmentOwner& outer,const explorer::PrivateDesktop& desktop) {
    OwnedFile file;if(!actualRibbonMetadata(file,outer,desktop))return false;
    const auto captured=outer.readback();
    if(!captured.heldCode){std::cout<<"UNAVAILABLE: actual production Ribbon/provider supplied no loaded ExplorerFrame code; module coverage zero\n";return false;}
    require(captured.verifiedCode&&captured.codeAcquisitions==1&&captured.codeReleases==0&&captured.clients==0,
        "Actual parent provider did not retain one verified executable reference");
    heldAcrossNested(outer);
    explorer::NativeApartmentReadback afterWorker;bool workerAvailable=true;
    worker([&](const explorer::PrivateDesktop& desktop) {
        explorer::NativeApartmentOwner independent;
        exact(independent.initializeOle(),"Initialize actual independent provider STA");
        workerAvailable=actualRibbonMetadata(file,independent,desktop);isolate(desktop);
        const auto held=independent.readback();
        require(held.creator!=captured.creator&&held.heldCode==captured.heldCode&&held.verifiedCode&&held.codeAcquisitions==1,
            "Independent actual provider STA did not own a separate normal code reference");
        heldAcrossNested(independent);
        exact(independent.finish(),"Complete worker actual Ole teardown before one module release");
        afterWorker=independent.readback();
        require(!afterWorker.active&&!afterWorker.finishing&&!afterWorker.heldCode&&!afterWorker.clients&&!afterWorker.depth&&
            afterWorker.codeAcquisitions==1&&afterWorker.codeReleases==1&&creatorActuallyRetired(afterWorker),
            "Worker owned reference was not released exactly once after actual final OLE teardown");
    });
    const auto remaining=outer.readback();
    MEMORY_BASIC_INFORMATION memory{};
    require(remaining.heldCode==captured.heldCode&&remaining.codeAcquisitions==1&&remaining.codeReleases==0&&
        VirtualQuery(remaining.heldCode,&memory,sizeof(memory))==sizeof(memory)&&memory.Type==MEM_IMAGE&&memory.AllocationBase==remaining.heldCode,
        "One worker retirement displaced another active apartment's executable reference");
    std::cout<<"ACTUAL_MODULE parentCreator="<<captured.creator<<" workerCreator="<<afterWorker.creator
        <<" parentAcquisitions="<<remaining.codeAcquisitions<<" workerReleases="<<afterWorker.codeReleases
        <<" workerPostApartmentRead="<<static_cast<ULONG>(afterWorker.postApartmentRead)
        <<" workerPostApartmentQualifier="<<afterWorker.postApartmentQualifier<<'\n';
    return workerAvailable;
}
}

int main(int argc,char** argv) {
    const bool module=argc==2&&argv&&argv[1]&&std::string_view(argv[1])=="--module";
    if(argc!=1&&!module){std::cerr<<"FAIL: native_apartment_tests [--module]\n";return 2;}
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOOPENFILEERRORBOX);
    explorer::PrivateDesktop desktop;const auto isolated=desktop.initialize();if(FAILED(isolated))return 2;
    explorer::NativeApartmentOwner outer;const auto initialized=outer.initializeOle();if(initialized!=S_OK)return 3;
    deadline=GetTickCount64()+20000;int result=0;
    try {basicContract(outer);if(module&&!moduleContract(outer,desktop))result=77;isolate(desktop);}
    catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';result=1;}
    const auto finished=outer.finish();const auto retired=outer.readback();
    if(FAILED(finished)||retired.active||retired.heldCode||retired.clients||retired.depth||retired.codeAcquisitions!=retired.codeReleases)result=1;
    try{isolate(desktop);}catch(const std::exception& error){std::cerr<<"FAIL: teardown: "<<error.what()<<'\n';result=1;}
    if(module&&!result) {
        if(retired.codeAcquisitions!=1||retired.codeReleases!=1||!creatorActuallyRetired(retired))result=1;
        else std::cout<<"ACTUAL_MODULE_FINAL parentCreator="<<retired.creator<<" parentReleases="<<retired.codeReleases
            <<" parentPostApartmentRead="<<static_cast<ULONG>(retired.postApartmentRead)
            <<" parentPostApartmentQualifier="<<retired.postApartmentQualifier<<'\n';
    }
    if(!result)std::cout<<"PASS: actual owned nested, failure, wrong-thread and post-uninitialize module ownership boundaries; no foreign-consumer claim\n";
    return result;
}
