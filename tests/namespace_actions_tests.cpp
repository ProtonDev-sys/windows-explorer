#include "explorer/namespace_actions.hpp"
#include "explorer/headless_visual.hpp"
#include "explorer/worker_sta.hpp"
#include "../src/parent_attribute_snapshot.hpp"

#include <shlobj.h>
#include <objbase.h>
#include <winioctl.h>
#include <wrl/implements.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {
using Microsoft::WRL::ComPtr;
using explorer::NamespaceAction;
using explorer::NamespaceFacts;
using explorer::NamespaceInvocationPlan;
using explorer::NamespaceInvocationRoute;
namespace fs = std::filesystem;
unsigned assertions = 0;

void require(bool value, const char* message) {
    ++assertions;
    if (!value) throw std::runtime_error(message);
}

void succeeded(HRESULT value, const char* message) {
    ++assertions;
    if (FAILED(value)) {
        std::cerr << "HRESULT 0x" << std::hex << static_cast<unsigned long>(value) << std::dec << '\n';
        throw std::runtime_error(message);
    }
}

[[noreturn]] void namespaceFatal(const char* phase,HRESULT status) noexcept {
    std::fprintf(stderr,"FATAL: private namespace %s HRESULT=0x%08lX\n",phase,static_cast<unsigned long>(status));
    TerminateProcess(GetCurrentProcess(),1);std::_Exit(1);
}

void drainNamespaceWorkers(const char* phase) noexcept {
    const auto status=explorer::drainStaWorkers(10000);
    if(FAILED(status))namespaceFatal(phase,status);
}

struct NamespaceDrainGuard {
    ~NamespaceDrainGuard(){drainNamespaceWorkers("fixture worker drain before owned-resource teardown");}
};

void onPrivateNamespaceDesktop(const std::function<void()>& body) {
    std::exception_ptr failure;
    std::thread worker([&] {
        explorer::PrivateDesktop desktop;bool initialized=false;
        try {
            succeeded(desktop.initialize(),"Attach private namespace desktop before native initialization");
            succeeded(desktop.verifyIsolation(),"Verify private namespace desktop isolation");
            succeeded(OleInitialize(nullptr),"Initialize private namespace STA");initialized=true;
            body();
            bool visible=true;
            succeeded(desktop.visibleWindowsOnInputDesktop(visible),"Inspect namespace fixture input-desktop visibility");
            require(!visible,"Namespace fixture displayed an input-desktop window");
            succeeded(desktop.verifyIsolation(),"Verify input desktop remains unchanged after namespace fixture");
        } catch(...) { failure=std::current_exception(); }
        if(initialized) {
            drainNamespaceWorkers("worker drain before apartment/private-desktop shutdown");
            OleUninitialize();
        }
    });
    HANDLE kernel=nullptr;
    if(!DuplicateHandle(GetCurrentProcess(),worker.native_handle(),GetCurrentProcess(),&kernel,SYNCHRONIZE,FALSE,0))
        namespaceFatal("duplicate exact fixture thread",HRESULT_FROM_WIN32(GetLastError()));
    const auto desktop=explorer::PrivateDesktop::current();
    const auto deadline=GetTickCount64()+20000;
    for(;;) {
        if(desktop) {
            const auto isolated=desktop->verifyIsolation();if(FAILED(isolated))namespaceFatal("parent isolation during native wait",isolated);
        }
        const auto signaled=WaitForSingleObject(kernel,0);
        if(signaled==WAIT_OBJECT_0)break;
        if(signaled!=WAIT_TIMEOUT)namespaceFatal("inspect exact fixture thread",HRESULT_FROM_WIN32(GetLastError()));
        if(GetTickCount64()>=deadline)namespaceFatal("fixture kernel-exit deadline",HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        MSG message{};unsigned dispatched=0;
        while(dispatched++<32&&PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            if(message.message==WM_QUIT)namespaceFatal("unexpected parent quit",E_ABORT);
            TranslateMessage(&message);DispatchMessageW(&message);
        }
        DWORD index=0;
        const auto waited=CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS|COWAIT_DISPATCH_WINDOW_MESSAGES,10,1,&kernel,&index);
        if(FAILED(waited)&&waited!=RPC_S_CALLPENDING)namespaceFatal("dispatch fixture kernel wait",waited);
    }
    CloseHandle(kernel);
    worker.join();if(failure)std::rethrow_exception(failure);
}

explorer::ContextMenuEntry entry(UINT id, const wchar_t* verb, UINT state = MFS_ENABLED) {
    explorer::ContextMenuEntry result;
    result.id = id;
    result.canonicalVerb = verb;
    result.label = L"\u65E5\u672C\u8A9E \u03BB translated item";
    result.state = state;
    return result;
}

struct Fixture {
    fs::path root;
    fs::path image;
    fs::path program;
    fs::path disc;
    fs::path text;
    Fixture() {
        GUID guid{};
        succeeded(CoCreateGuid(&guid), "Generate namespace fixture identity");
        wchar_t formatted[40]{};
        require(StringFromGUID2(guid, formatted, 40) != 0, "Format namespace fixture identity");
        root = fs::temp_directory_path() / (std::wstring(L"WindowsExplorer-Namespace-") + formatted);
        require(fs::create_directory(root), "Create owned namespace fixture");
        image = root / L"\u65E5\u672C\u8A9E \u03BB.bmp";
        program = root / L"headless fixture.exe";
        disc = root / L"test image.iso";
        text = root / L"document.txt";
        // Real 2x2 24-bit BMP, with two DWORD-aligned rows. No decoder/plugin UI.
        const std::array<unsigned char, 70> bitmap{
            'B','M',70,0,0,0, 0,0,0,0, 54,0,0,0,
            40,0,0,0, 2,0,0,0, 2,0,0,0, 1,0,24,0,
            0,0,0,0, 16,0,0,0, 0,0,0,0, 0,0,0,0,
            0,0,0,0, 0,0,0,0,
            0,0,255, 0,255,0, 0,0, 255,0,0, 255,255,255, 0,0
        };
        std::ofstream output(image, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bitmap.data()), bitmap.size());
        require(output.good(), "Write valid owned bitmap fixture");
        output.close();
        std::array<wchar_t, 32768> executable{};
        const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        require(length && length < executable.size(), "Locate current console test executable");
        require(CopyFileW(executable.data(), program.c_str(), TRUE), "Copy owned executable without launching it");
        std::ofstream(disc, std::ios::binary) << "owned ISO-shaped fixture: never mounted or burned";
        std::ofstream(text, std::ios::binary) << "native namespace headless fixture stays intact";
    }
    ~Fixture() { std::error_code ignored; fs::remove_all(root, ignored); }
};

ComPtr<IShellItem> item(const fs::path& path) {
    ComPtr<IShellItem> result;
    succeeded(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&result)), "Create owned namespace Shell item");
    return result;
}

ComPtr<IShellItemArray> array(IShellItem* value) {
    ComPtr<IShellItemArray> result;
    succeeded(SHCreateShellItemArrayFromShellItem(value, IID_PPV_ARGS(&result)), "Create namespace selection array");
    return result;
}

std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "Read owned fixture for side-effect checks");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool pumpPrivateNamespaceUntil(const std::function<bool()>& complete,DWORD milliseconds) {
    struct Event {HANDLE value=CreateEventW(nullptr,TRUE,FALSE,nullptr);~Event(){if(value)CloseHandle(value);}}event;
    require(event.value!=nullptr,"Create owned namespace COM-dispatch event");
    const auto desktop=explorer::PrivateDesktop::current();
    require(desktop!=nullptr,"Native background fixture has no private desktop");
    const auto deadline=GetTickCount64()+milliseconds;
    do {
        succeeded(desktop->verifyIsolation(),"Keep native background query on its private desktop");
        if(complete())return true;
        MSG message{};unsigned dispatched=0;
        while(dispatched++<32&&PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            if(message.message==WM_QUIT){PostQuitMessage(static_cast<int>(message.wParam));return false;}
            TranslateMessage(&message);DispatchMessageW(&message);
        }
        DWORD index=0;
        const auto waited=CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS|COWAIT_DISPATCH_WINDOW_MESSAGES,5,1,&event.value,&index);
        if(waited!=RPC_S_CALLPENDING)succeeded(waited,"Dispatch native background provider's marshaled view calls");
    }while(GetTickCount64()<deadline);
    return complete();
}

struct LookupEvidence {
    HANDLE entered=CreateEventW(nullptr,TRUE,FALSE,nullptr),release=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::atomic<bool> block{false};
    std::atomic<unsigned> marshalClasses{0},registrationProbes{0},marshals{0},lookups{0},destroyed{0};
    std::atomic<DWORD> lookupThread{0};
    std::atomic<DWORD> classThread{0},classDestination{0},classFlags{0};
    std::atomic<HRESULT> lookupWait{E_PENDING},classStatus{E_PENDING},destructionApartment{E_PENDING};
    std::function<void()> onRegistration;
    ~LookupEvidence(){if(entered)CloseHandle(entered);if(release)CloseHandle(release);}
};

// Own public COM object using the actual free-threaded marshaler. The gate is
// inside the real standard GIT's foreign-apartment IUnknown lookup, rather
// than a replacement GIT or a simulated worker-completion seam.
class LookupSite final : public IServiceProvider,public IMarshal {
public:
    static HRESULT create(const std::shared_ptr<LookupEvidence>& evidence,ComPtr<IUnknown>* result) {
        auto* value=new(std::nothrow) LookupSite(evidence);if(!value)return E_OUTOFMEMORY;
        const auto status=CoCreateFreeThreadedMarshaler(static_cast<IServiceProvider*>(value),&value->marshaler_);
        if(FAILED(status)){value->Release();return status;}
        result->Attach(static_cast<IServiceProvider*>(value));return S_OK;
    }
    ULONG references() const noexcept{return references_.load();}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** result) override {
        if(!result)return E_POINTER;*result=nullptr;
        if(IsEqualIID(iid,IID_IUnknown)||IsEqualIID(iid,IID_IServiceProvider)) {
            if(IsEqualIID(iid,IID_IUnknown)&&GetCurrentThreadId()!=creator_&&evidence_->block.exchange(false)) {
                ++evidence_->lookups;evidence_->lookupThread=GetCurrentThreadId();SetEvent(evidence_->entered);
                evidence_->lookupWait=WaitForSingleObject(evidence_->release,5000)==WAIT_OBJECT_0?S_OK:HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                if(FAILED(evidence_->lookupWait.load()))return evidence_->lookupWait.load();
            }
            *result=static_cast<IServiceProvider*>(this);
        } else if(IsEqualIID(iid,IID_IMarshal))*result=static_cast<IMarshal*>(this);
        else return E_NOINTERFACE;
        AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++references_;}
    ULONG STDMETHODCALLTYPE Release() override{const auto remaining=--references_;if(!remaining)delete this;return remaining;}
    HRESULT STDMETHODCALLTYPE QueryService(REFGUID,REFIID,void** result) override {
        if(!result)return E_POINTER;*result=nullptr;return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetUnmarshalClass(REFIID iid,void* object,DWORD destination,void* context,DWORD flags,CLSID* result) override {
        ++evidence_->marshalClasses;evidence_->classThread=GetCurrentThreadId();
        evidence_->classDestination=destination;evidence_->classFlags=flags;
        if(GetCurrentThreadId()==creator_&&IsEqualIID(iid,IID_IUnknown)) {
            // The standard GIT may recognize the real FTM class and retain an
            // agile interface without calling MarshalInterface. Observe its
            // actual creator-side IUnknown registration-class probe instead.
            ++evidence_->registrationProbes;
            auto callback=std::move(evidence_->onRegistration);if(callback)callback();
        }
        const auto status=marshal([&](IMarshal* value){return value->GetUnmarshalClass(iid,object,destination,context,flags,result);});
        evidence_->classStatus=status;return status;
    }
    HRESULT STDMETHODCALLTYPE GetMarshalSizeMax(REFIID iid,void* object,DWORD destination,void* context,DWORD flags,DWORD* result) override {
        return marshal([&](IMarshal* value){return value->GetMarshalSizeMax(iid,object,destination,context,flags,result);});
    }
    HRESULT STDMETHODCALLTYPE MarshalInterface(IStream* stream,REFIID iid,void* object,DWORD destination,void* context,DWORD flags) override {
        ++evidence_->marshals;
        return marshal([&](IMarshal* value){return value->MarshalInterface(stream,iid,object,destination,context,flags);});
    }
    HRESULT STDMETHODCALLTYPE UnmarshalInterface(IStream* stream,REFIID iid,void** result) override {
        return marshal([&](IMarshal* value){return value->UnmarshalInterface(stream,iid,result);});
    }
    HRESULT STDMETHODCALLTYPE ReleaseMarshalData(IStream* stream) override {
        return marshal([&](IMarshal* value){return value->ReleaseMarshalData(stream);});
    }
    HRESULT STDMETHODCALLTYPE DisconnectObject(DWORD reserved) override {
        return marshal([&](IMarshal* value){return value->DisconnectObject(reserved);});
    }
private:
    explicit LookupSite(std::shared_ptr<LookupEvidence> evidence):evidence_(std::move(evidence)){}
    ~LookupSite(){APTTYPE type{};APTTYPEQUALIFIER qualifier{};evidence_->destructionApartment=CoGetApartmentType(&type,&qualifier);++evidence_->destroyed;}
    template<class Callback> HRESULT marshal(const Callback& callback) {
        ComPtr<IMarshal> value;const auto status=marshaler_.As(&value);return FAILED(status)?status:callback(value.Get());
    }
    std::atomic<ULONG> references_{1};
    DWORD creator_=GetCurrentThreadId();
    std::shared_ptr<LookupEvidence> evidence_;
    ComPtr<IUnknown> marshaler_;
};

void traceLookupEvidence(const char* stage,const LookupEvidence& evidence) {
    std::cerr<<"Native GIT stage="<<stage<<" classCalls="<<evidence.marshalClasses.load()
        <<" creatorIUnknownProbes="<<evidence.registrationProbes.load()<<" MarshalInterface="<<evidence.marshals.load()
        <<" foreignLookups="<<evidence.lookups.load()<<" lookupThread="<<evidence.lookupThread.load()
        <<" creatorThread="<<GetCurrentThreadId()<<" classThread="<<evidence.classThread.load()
        <<" destination="<<evidence.classDestination.load()<<" flags="<<evidence.classFlags.load()
        <<" classHr=0x"<<std::hex<<static_cast<unsigned long>(evidence.classStatus.load())<<std::dec<<'\n';
}

void realGitLookupCancellationLifetime() {
    Fixture fixture;const auto folder=item(fixture.root);const auto selected=array(folder.Get());
    const auto before=read(fixture.text);const auto clipboard=GetClipboardSequenceNumber();
    constexpr wchar_t command[]=L"Windows.WindowsExplorer_GIT_LifetimeFixture_8CF8E310";
    constexpr wchar_t keyPath[]=L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\CommandStore\\shell\\Windows.WindowsExplorer_GIT_LifetimeFixture_8CF8E310";
    HKEY missing=nullptr;const auto absent=RegOpenKeyExW(HKEY_LOCAL_MACHINE,keyPath,0,KEY_READ,&missing);
    if(missing)RegCloseKey(missing);
    require(absent==ERROR_FILE_NOT_FOUND||absent==ERROR_PATH_NOT_FOUND,
            "Controlled GIT lookup command is not genuinely absent from the installed registry");
    for(const bool facade:{false,true}) {
        const auto evidence=std::make_shared<LookupEvidence>();
        require(evidence->entered&&evidence->release,"Create owned actual-GIT lookup control events");
        ComPtr<IUnknown> site;succeeded(LookupSite::create(evidence,&site),"Create real free-threaded COM lookup site");
        auto* source=static_cast<LookupSite*>(static_cast<IServiceProvider*>(site.Get()));
        explorer::NativeNamespaceActions actions;
        if(facade)succeeded(actions.initialize(nullptr,{folder,selected,site}),"Initialize exact retained cancellation context");
        std::unique_ptr<explorer::NamespaceCommandStateTask> task;
        evidence->block=true;
        try {
            // No installed provider can perform a later QI: the only foreign
            // lookup before the missing-key failure is the actual GIT call.
            succeeded(facade?actions.startCommandStateTask(command,&task,explorer::NamespaceMenuScope::Background):
                explorer::NamespaceCommandStateTask::start(command,selected.Get(),site.Get(),true,&task),
                "Start actual standard-GIT lookup before cancellation");
            require(pumpPrivateNamespaceUntil([&]{return WaitForSingleObject(evidence->entered,0)==WAIT_OBJECT_0;},3000),
                    "Real standard-GIT foreign-apartment lookup did not enter the owned gate");
            if(evidence->lookups!=1||evidence->lookupThread==GetCurrentThreadId()||!evidence->registrationProbes)
                traceLookupEvidence("controlled-lookup",*evidence);
            require(evidence->lookups==1&&evidence->lookupThread!=GetCurrentThreadId()&&evidence->registrationProbes>0,
                    "Lookup control ran on the creator or bypassed real GIT free-threaded registration");
            const auto references=source->references();
            const auto started=GetTickCount64();task->cancel();
            explorer::NamespaceCommandState sentinel;sentinel.state=ECS_CHECKED;sentinel.selectionCount=41;
            require(task->poll(&sentinel)==HRESULT_FROM_WIN32(ERROR_CANCELLED)&&sentinel.state==ECS_CHECKED&&sentinel.selectionCount==41,
                    "Lookup cancellation accepted a stale result or changed output");
            task.reset();
            require(GetTickCount64()-started<250&&source->references()==references,
                    "Cancellation revoked a live GIT lookup or waited for the gated worker");
            actions.reset();
            // Keep a test anchor until the lookup has exited, so a broken
            // retention implementation fails an assertion rather than freeing
            // this owned COM object's method while it is executing.
            SetEvent(evidence->release);
            drainNamespaceWorkers("real canceled lookup and exact native worker exit");
            require(evidence->lookupWait==S_OK&&source->references()==1,
                    "Canceled lookup failed or left a registered/proxy site reference after worker exit");
            site.Reset();
            require(evidence->destroyed==1&&SUCCEEDED(evidence->destructionApartment.load()),
                    "Final GIT site release escaped an initialized apartment or retained the canceled object");
        } catch(...) {
            SetEvent(evidence->release);
            if(task)task->cancel();task.reset();actions.reset();
            drainNamespaceWorkers("failed controlled COM lookup before owned fixture teardown");
            throw;
        }
    }
    require(read(fixture.text)==before&&GetClipboardSequenceNumber()==clipboard,
            "Read-only GIT lifetime proof changed owned data or the clipboard");
}

void exactTargetRegistrationReuseAndReentry() {
    Fixture fixture;const auto folder=item(fixture.root),text=item(fixture.text),image=item(fixture.image);
    struct Identities {PIDLIST_ABSOLUTE first=nullptr,second=nullptr;~Identities(){CoTaskMemFree(first);CoTaskMemFree(second);}} identities;
    succeeded(SHGetIDListFromObject(text.Get(),&identities.first),"Capture first owned native registration target");
    succeeded(SHGetIDListFromObject(image.Get(),&identities.second),"Capture second owned native registration target");
    std::array<PCIDLIST_ABSOLUTE,2> ids{identities.first,identities.second};
    ComPtr<IShellItemArray> selected;succeeded(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(ids.size()),ids.data(),&selected),
                                             "Construct complete native registration-reuse selection");
    const auto evidence=std::make_shared<LookupEvidence>();ComPtr<IUnknown> site;
    succeeded(LookupSite::create(evidence,&site),"Create actual COM marshal-count provenance site");
    const auto replacement=std::make_shared<LookupEvidence>();ComPtr<IUnknown> replacementSite;
    succeeded(LookupSite::create(replacement,&replacementSite),"Create distinct replacement native context site");
    const auto replacementSelection=array(image.Get());
    HRESULT reinitialized=E_PENDING;
    const auto before=read(fixture.text);const auto clipboard=GetClipboardSequenceNumber();
    explorer::NativeNamespaceActions actions;succeeded(actions.initialize(nullptr,{folder,selected,site}),"Initialize exact full-array native context");
    NamespaceDrainGuard cleanup;
    const auto properties=[&](bool standalone=false) {
        std::unique_ptr<explorer::NamespaceCommandStateTask> task;
        succeeded(standalone?explorer::NamespaceCommandStateTask::startSelectionVerb(L"properties",selected.Get(),site.Get(),&task):
                            actions.startStaticVerbStateTask(L"properties",&task),"Start complete actual native properties state");
        require(pumpPrivateNamespaceUntil([&]{return task->completed();},5000),"Actual native registered context exceeded bounded wait");
        explorer::NamespaceCommandState state;succeeded(task->poll(&state),"Read actual complete-selection native properties state");
        require(state.contextMenu&&state.identitySnapshot&&state.selectionCount==2&&state.siteAttached,
                "Registration reuse replaced the full native selection, CIDA snapshot or original site");
        return state.state;
    };
    const auto expected=properties();const auto initial=evidence->registrationProbes.load();
    const auto repeated=properties();
    if(!initial||repeated!=expected||evidence->registrationProbes!=initial)traceLookupEvidence("same-object-reuse",*evidence);
    require(initial>0&&repeated==expected&&evidence->registrationProbes==initial,
            "Repeated same-object state created another GIT registration or changed native state");
    succeeded(actions.refresh(),"Refresh capabilities while exact full-array/site objects remain unchanged");
    require(properties()==expected&&evidence->registrationProbes==initial,"Menu refresh discarded an unchanged registration context");
    properties(true);
    const auto standaloneProbes=evidence->registrationProbes.load();
    require(standaloneProbes>initial,"Standalone public task reused a facade registration instead of owning its own");
    succeeded(actions.initialize(nullptr,{folder,selected,site}),"Advance native target generation with identical retained objects");
    require(properties()==expected&&evidence->registrationProbes>standaloneProbes,
            "Target generation replacement reused retired registrations or changed native membership state");
    const auto background=[&] {
        std::unique_ptr<explorer::NamespaceCommandStateTask> task;
        succeeded(actions.startCommandStateTask(L"Windows.undo",&task,explorer::NamespaceMenuScope::Background),
                  "Start read-only native background provider with exact current folder");
        require(pumpPrivateNamespaceUntil([&]{return task->completed();},5000),"Native background provider did not finish");
        explorer::NamespaceCommandState state;return std::pair{task->poll(&state),state.state};
    };
    const auto beforeBackground=evidence->registrationProbes.load();
    const auto backgroundState=background();const auto backgroundProbes=evidence->registrationProbes.load();
    require(backgroundProbes>beforeBackground&&background()==backgroundState&&evidence->registrationProbes==backgroundProbes,
            "Background jobs rebuilt/re-registered an unchanged folder context or merged selection targets");

    succeeded(actions.initialize(nullptr,{folder,selected,site}),"Prepare actual registration callback generation fence");
    evidence->onRegistration=[&]{reinitialized=actions.initialize(nullptr,{folder,replacementSelection,replacementSite});};
    std::unique_ptr<explorer::NamespaceCommandStateTask> untouched;
    require(actions.startStaticVerbStateTask(L"properties",&untouched)==HRESULT_FROM_WIN32(ERROR_BUSY)&&!untouched&&SUCCEEDED(reinitialized),
            "Native marshal reentry published an old-generation task or failed to preserve output");
    succeeded(actions.startStaticVerbStateTask(L"properties",&untouched),"Start only the actual replacement target generation");
    require(pumpPrivateNamespaceUntil([&]{return untouched->completed();},5000),"Replacement native provider did not complete");
    explorer::NamespaceCommandState actual;succeeded(untouched->poll(&actual),"Read genuine replacement-array native state");
    require(actual.contextMenu&&actual.identitySnapshot&&actual.selectionCount==1&&actual.siteAttached&&replacement->registrationProbes>0,
            "Reentrant replacement retained the old selection, site or stale registration");
    untouched.reset();actions.reset();
    drainNamespaceWorkers("every registration-reuse worker before owned data teardown");
    require(read(fixture.text)==before&&GetClipboardSequenceNumber()==clipboard,
            "Registration reuse/state reads changed owned contents or shared clipboard");
}

class BackgroundNavigationEvents final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IExplorerBrowserEvents> {
public:
    bool complete=false;
    HRESULT status=E_PENDING;
    HRESULT STDMETHODCALLTYPE OnNavigationPending(PCIDLIST_ABSOLUTE) override{return S_OK;}
    HRESULT STDMETHODCALLTYPE OnViewCreated(IShellView*) override{return S_OK;}
    HRESULT STDMETHODCALLTYPE OnNavigationComplete(PCIDLIST_ABSOLUTE) override{complete=true;status=S_OK;return S_OK;}
    HRESULT STDMETHODCALLTYPE OnNavigationFailed(PCIDLIST_ABSOLUTE) override{complete=true;status=E_FAIL;return S_OK;}
};

struct NativeBackgroundView {
    HWND owner=nullptr;
    DWORD cookie=0;
    ComPtr<IExplorerBrowser> browser;
    ComPtr<IShellView> view;
    ComPtr<BackgroundNavigationEvents> events;
    void initialize(IShellItem* folder) {
        const auto desktop=explorer::PrivateDesktop::current();
        require(desktop&&SUCCEEDED(desktop->verifyIsolation()),"Create native background view only on the owned private desktop");
        owner=CreateWindowExW(0,L"STATIC",L"owned read-only native background fixture",WS_POPUP,
            0,0,800,600,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        require(owner&&!IsWindowVisible(owner),"Create exclusively hidden native background owner");
        succeeded(CoCreateInstance(CLSID_ExplorerBrowser,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&browser)),
                  "Create read-only native background browser");
        succeeded(browser->SetOptions(EBO_NOPERSISTVIEWSTATE|EBO_NOTRAVELLOG),"Prohibit native background fixture view/history persistence");
        RECT bounds{0,0,800,600};FOLDERSETTINGS settings{FVM_DETAILS,0};
        succeeded(browser->Initialize(owner,&bounds,&settings),"Initialize native background browser without displaying it");
        events=Microsoft::WRL::Make<BackgroundNavigationEvents>();require(events!=nullptr,"Create native background navigation observer");
        succeeded(browser->Advise(events.Get(),&cookie),"Observe native background navigation completion");
        succeeded(browser->BrowseToObject(folder,SBSP_ABSOLUTE),"Browse native background namespace without invoking a command");
        require(pumpPrivateNamespaceUntil([&]{return events->complete;},5000),"Native background navigation exceeded bounded wait");
        succeeded(events->status,"Read actual native background navigation result");
        succeeded(browser->GetCurrentView(IID_PPV_ARGS(&view)),"Read actual native background view site");
        require(!IsWindowVisible(owner),"Native background browser displayed its owner");
    }
    ~NativeBackgroundView() {
        // Keep the native view/HWND alive through exact canceled-worker exit;
        // failed draining must stop this private process before teardown.
        drainNamespaceWorkers("native browser/view drain before teardown");
        view.Reset();
        if(browser){if(cookie)browser->Unadvise(cookie);browser->Destroy();browser.Reset();}
        events.Reset();if(owner)DestroyWindow(owner);
    }
};

void nativeRecyclePropertiesBackgroundState() {
    const auto clipboard=GetClipboardSequenceNumber();
    Fixture fixture;const auto before=read(fixture.text);
    ComPtr<IShellItem> bin;
    succeeded(SHGetKnownFolderItem(FOLDERID_RecycleBinFolder,KF_FLAG_DONT_VERIFY,nullptr,IID_PPV_ARGS(&bin)),
              "Get actual Recycle Bin namespace without deleting or restoring anything");
    NativeBackgroundView host;host.initialize(bin.Get());
    explorer::NativeNamespaceActions actions;
    succeeded(actions.initialize(host.owner,{bin,{},host.view}),"Retain actual Bin folder and original native view site");
    require(actions.facts().recycleBin&&actions.facts().selectionCount==0,"Native background fixture lost its Bin/no-selection context");
    constexpr auto command=L"Windows.RecycleBin.properties";
    explorer::NamespaceCommandState sentinel;sentinel.state=ECS_CHECKED;sentinel.delegatedCommand=L"unchanged";
    auto fast=sentinel;
    const auto status=actions.queryCommandState(command,&fast,explorer::NamespaceMenuScope::Background);
    require(status==E_PENDING&&fast.state==sentinel.state&&fast.delegatedCommand==sentinel.delegatedCommand,
            "Composite Bin fast query invented availability or changed output instead of deferring native menu work");
    std::unique_ptr<explorer::NamespaceCommandStateTask> task;
    const auto notFolder=item(fixture.text);const auto invalidBackground=array(notFolder.Get());
    succeeded(explorer::NamespaceCommandStateTask::startRegisteredMenu(command,invalidBackground.Get(),host.view.Get(),&task,
              explorer::NamespaceMenuScope::Background),"Start invalid owned file background to verify native folder validation");
    require(pumpPrivateNamespaceUntil([&]{return task->completed();},5000),"Invalid background folder validation exceeded bounded wait");
    auto invalidOutput=sentinel;
    require(task->poll(&invalidOutput)==E_INVALIDARG&&invalidOutput.state==sentinel.state&&
            invalidOutput.delegatedCommand==sentinel.delegatedCommand,
            "Nonfolder background was accepted or changed the caller's preserved state");
    task.reset();
    HRESULT started=HRESULT_FROM_WIN32(ERROR_BUSY);
    require(pumpPrivateNamespaceUntil([&]{
        if(started==HRESULT_FROM_WIN32(ERROR_BUSY))started=actions.startCommandStateTask(command,&task,explorer::NamespaceMenuScope::Background);
        return started!=HRESULT_FROM_WIN32(ERROR_BUSY);
    },5000),"Native Bin background worker could not reserve its own menu slot");
    succeeded(started,"Start exact native Bin background-menu state worker");
    require(task!=nullptr,"Native Bin background worker was not retained");
    require(pumpPrivateNamespaceUntil([&]{return task->completed();},10000),"Native Bin background-menu state worker exceeded bounded wait");
    explorer::NamespaceCommandState state;
    succeeded(task->poll(&state),"Read actual native Bin Properties background-menu state");
    require(state.contextMenu&&state.siteAttached&&!state.identitySnapshot&&state.selectionCount==0&&
            state.delegatedCommand==command&&state.enabled(),"Native Bin Properties did not resolve its enabled sited background leaf");
    explorer::NamespaceCommandStateTimings timing;
    succeeded(task->pollTimings(&timing),"Read completed native Bin menu-worker timings");
    task.reset();

    // Independently build the real public cidl=0 menu on the owner STA, with
    // the same original folder/site. No delayed submenus or verbs are invoked.
    ComPtr<IShellFolder> folder;succeeded(bin->BindToHandler(nullptr,BHID_SFObject,IID_PPV_ARGS(&folder)),"Bind actual native Bin background folder");
    struct Pidl {PIDLIST_ABSOLUTE value=nullptr;~Pidl(){CoTaskMemFree(value);}}pidl;
    succeeded(SHGetIDListFromObject(bin.Get(),&pidl.value),"Retain native Bin background identity");
    struct Key {HKEY value=nullptr;~Key(){if(value)RegCloseKey(value);}}key;
    succeeded(HRESULT_FROM_WIN32(RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\CommandStore",0,KEY_READ,&key.value)),
        "Open native background CommandStore read-only");
    DEFCONTEXTMENU definition{};definition.hwnd=host.owner;definition.pidlFolder=pidl.value;
    definition.psf=folder.Get();definition.cKeys=1;definition.aKeys=&key.value;
    ComPtr<IContextMenu> context;succeeded(SHCreateDefaultContextMenu(&definition,IID_PPV_ARGS(&context)),"Create independent native Bin background menu");
    explorer::NativeContextMenu menu;succeeded(menu.create(host.owner,context.Get(),host.view.Get(),CMF_EXTENDEDVERBS),"Attach original Bin view site to independent native menu");
    std::vector<explorer::ContextMenuEntry> entries;succeeded(menu.enumerate(entries,false),"Read native Bin Properties without opening or populating a submenu");
    unsigned matches=0;UINT ordinal=0;
    for(const auto& entry:entries)if(!entry.separator()&&!entry.submenu&&_wcsicmp(entry.canonicalVerb.c_str(),command)==0) {
        ++matches;ordinal=entry.id;
        require(state.enabled()==entry.enabled()&&state.checked()==((entry.state&MFS_CHECKED)!=0),
                "Deferred Bin Properties state differs from independent native background-menu authority");
    }
    require(matches==1&&ordinal!=0,"Independent native Bin menu has no exact unique Properties ordinal");
    require(actions.invokeCommandStore(command,true,{},explorer::NamespaceMenuScope::Background)==E_ACCESSDENIED&&
            actions.invokeCommandStore(command,false,{},explorer::NamespaceMenuScope::Background)==E_ACCESSDENIED,
            "Read-only Bin fixture weakened headless or hidden-owner invocation guards");
    bool visible=true;const auto desktop=explorer::PrivateDesktop::current();
    succeeded(desktop->visibleWindowsOnInputDesktop(visible),"Observe actual input-desktop windows after native Bin state readback");
    require(!visible&&!IsWindowVisible(host.owner)&&GetClipboardSequenceNumber()==clipboard&&read(fixture.text)==before,
            "Native Bin state fixture displayed UI or changed owned content/clipboard");
    std::cout<<"Bin Properties nativeBackground=1 enabled="<<state.enabled()<<" matchedOrdinals="<<matches
        <<" workerMicroseconds="<<timing.workerMicroseconds<<'\n';
}

NamespaceFacts selectedFile(const wchar_t* path) {
    NamespaceFacts facts;
    facts.selectionCount = 1;
    facts.filesystem = facts.physicalFiles = true;
    facts.singlePath = path;
    return facts;
}

void semanticPlanningAndCanonicalVerbs() {
    NamespaceFacts facts = selectedFile(L"C:\\owned\\picture.bmp");
    facts.images = true;
    auto native = entry(42, L"rotate90");
    auto stored = entry(501, L"Windows.rotate90");
    NamespaceInvocationPlan plan;
    succeeded(explorer::planNamespaceAction(NamespaceAction::RotateRight, facts, {native}, {stored}, &plan), "Plan exact image rotation verb");
    require(plan.enabled && plan.route == NamespaceInvocationRoute::SelectionMenu && plan.commandId == 42 &&
            plan.canonicalVerb == L"rotate90" && plan.target == facts.singlePath, "Canonical rotation target or ordinal changed");
    succeeded(explorer::planNamespaceAction(NamespaceAction::RotateRight, facts, {}, {stored}, &plan), "Plan native CommandStore fallback");
    require(plan.enabled && plan.route == NamespaceInvocationRoute::CommandStoreMenu && plan.commandId == 501,
            "CommandStore fallback did not preserve exact native ordinal");
    native.canonicalVerb.clear();
    native.label = L"Rotate right";
    require(FAILED(explorer::planNamespaceAction(NamespaceAction::RotateRight, facts, {native}, {}, &plan)),
            "Translated menu labels must never be treated as canonical verbs");
    facts.images = false;
    const auto previous = plan.canonicalVerb;
    require(FAILED(explorer::planNamespaceAction(NamespaceAction::RotateRight, facts, {}, {stored}, &plan)) && plan.canonicalVerb == previous,
            "File type guard or failure output preservation is missing");
    require(explorer::planNamespaceAction(NamespaceAction::Count, facts, {}, {}, &plan) == E_INVALIDARG,
            "Unknown namespace action accepted");
    require(explorer::planNamespaceAction(NamespaceAction::Email, facts, {}, {}, nullptr) == E_POINTER,
            "Null namespace plan must return E_POINTER");
    facts = selectedFile(L"C:\\owned\\disc.vhdx");
    facts.discImages = true;
    succeeded(explorer::planNamespaceAction(NamespaceAction::MountDiscImage, facts, {entry(14,L"mount")}, {}, &plan), "Plan supported disk image mount");
    require(FAILED(explorer::planNamespaceAction(NamespaceAction::BurnDiscImage, facts, {entry(15,L"burn")}, {}, &plan)),
            "Virtual hard disk must not be routed to ISO optical burning");
    facts.selectionCount = 2;
    require(FAILED(explorer::planNamespaceAction(NamespaceAction::MountDiscImage, facts, {entry(14,L"mount")}, {}, &plan)),
            "Single-target disk image action accepted a multi-selection");
    facts=selectedFile(L"C:\\owned\\shared.txt");facts.selectionCount=2;facts.singlePath.clear();
    for(const auto action:{NamespaceAction::ShareSpecificPeople,NamespaceAction::RemoveAccess}) {
        const auto command=std::wstring(explorer::namespaceActionCommand(action));
        succeeded(explorer::planNamespaceAction(action,facts,{},{entry(77,command.c_str())},&plan),
                  "Retain native multiple-item access command applicability");
        require(plan.enabled&&plan.commandId==77&&plan.target.empty(),"Multiple sharing targets were collapsed to a guessed single path");
        facts.filesystem=false;
        require(!explorer::namespaceActionApplicable(action,facts),"Virtual/nonfilesystem targets bypassed access semantics");
        facts.filesystem=true;
    }
}

void disabledMenusAndAmbiguousHandlers() {
    NamespaceFacts facts = selectedFile(L"C:\\owned\\image.bmp");
    facts.images = true;
    NamespaceInvocationPlan plan;
    succeeded(explorer::planNamespaceAction(NamespaceAction::RotateLeft, facts, {entry(11,L"rotate270",MFS_DISABLED)}, {}, &plan),
              "Describe disabled native rotation");
    require(!plan.enabled && plan.status == E_ACCESSDENIED, "Disabled handler was accepted");
    auto parent = entry(5,L"container",MFS_DISABLED);
    parent.submenu = true;
    parent.children = {entry(12,L"rotate270")};
    succeeded(explorer::planNamespaceAction(NamespaceAction::RotateLeft, facts, {parent}, {}, &plan), "Describe disabled ancestor");
    require(!plan.enabled, "Enabled child bypassed a disabled parent");
    require(explorer::planNamespaceAction(NamespaceAction::RotateLeft, facts, {entry(11,L"rotate270"), entry(12,L"rotate270")}, {}, &plan) == E_UNEXPECTED,
            "Ambiguous same-level canonical handler must fail");
    require(explorer::planNamespaceAction(NamespaceAction::RotateLeft, facts, {entry(11,L"rotate270"), entry(11,L"different")}, {}, &plan) == E_UNEXPECTED,
            "Duplicate native ordinal must fail even when another entry has a different verb");
    auto malformed = entry(0xffff,L"rotate270");
    require(FAILED(explorer::planNamespaceAction(NamespaceAction::RotateLeft, facts, {malformed}, {}, &plan)), "Foreign menu ID accepted");
    parent.state = MFS_ENABLED;
    succeeded(explorer::planNamespaceAction(NamespaceAction::RotateLeft, facts, {entry(11,L"rotate270"),parent}, {}, &plan),
              "Prefer top-level verb to nested duplicate");
    require(plan.commandId == 11, "Nested duplicate took precedence over exact top-level command");

    facts = {};
    facts.filesystem = facts.physicalFolders = true;
    facts.singlePath = L"C:\\owned\\folder";
    auto library = entry(101,L"Windows.includeinlibrary");
    library.submenu = true;
    library.children = {entry(102,L"",MFS_DISABLED)};
    succeeded(explorer::planNamespaceAction(NamespaceAction::IncludeInLibrary, facts, {}, {library}, &plan), "Describe empty native library cascade");
    require(plan.submenu && !plan.enabled, "Library cascade without enabled choices was enabled");
    library.children[0].state = MFS_ENABLED;
    succeeded(explorer::planNamespaceAction(NamespaceAction::IncludeInLibrary, facts, {}, {library}, &plan), "Describe populated native library cascade");
    require(plan.enabled && plan.submenu && plan.commandId == 101, "Library cascade was flattened into a guessed child invocation");
}

void destructiveAndOptionalActionPlanning() {
    NamespaceFacts facts;
    facts.filesystem = facts.physicalFolders = facts.driveRoot = true;
    facts.driveType = DRIVE_FIXED;
    facts.singlePath = L"C:\\";
    NamespaceInvocationPlan plan;
    succeeded(explorer::planNamespaceAction(NamespaceAction::FormatDrive, facts, {entry(32,L"format")}, {}, &plan), "Plan native format without invocation");
    require(plan.enabled && plan.nativeConfirmation && plan.target == L"C:\\", "Formatting lost native confirmation/target");
    facts.driveType = DRIVE_REMOTE;
    require(FAILED(explorer::planNamespaceAction(NamespaceAction::FormatDrive, facts, {entry(32,L"format")}, {}, &plan)), "Network drive offered local formatting");
    facts.driveType = DRIVE_CDROM;
    require(FAILED(explorer::planNamespaceAction(NamespaceAction::OptimizeDrive, facts, {}, {entry(60,L"Windows.Defragment")}, &plan)),
            "Optical device offered filesystem optimization");
    succeeded(explorer::planNamespaceAction(NamespaceAction::EjectDrive, facts, {entry(33,L"eject")}, {}, &plan), "Plan optical eject without touching a device");
    facts.driveType = DRIVE_FIXED;
    require(FAILED(explorer::planNamespaceAction(NamespaceAction::BitLocker, facts, {}, {}, &plan)), "Missing BitLocker provider was invented");
    succeeded(explorer::planNamespaceAction(NamespaceAction::BitLocker, facts, {entry(50,L"manage-bde")}, {}, &plan), "Plan installed native BitLocker verb");
    require(plan.commandId == 50, "BitLocker command was guessed instead of retained");

    facts = {};
    facts.recycleBin = true;
    facts.recycleStatus = S_OK;
    succeeded(explorer::planNamespaceAction(NamespaceAction::EmptyRecycleBin, facts, {}, {}, &plan), "Describe empty isolated Recycle Bin facts");
    require(!plan.enabled && plan.status == HRESULT_FROM_WIN32(ERROR_NO_MORE_ITEMS), "Empty Bin enabled a destructive command");
    facts.recycleItems = 17;
    succeeded(explorer::planNamespaceAction(NamespaceAction::RestoreAll, facts, {}, {}, &plan), "Plan all-item native restore on simulated Bin facts");
    require(plan.enabled && plan.route == NamespaceInvocationRoute::RestoreAllItems && plan.nativeConfirmation,
            "Restore-all route lost native decision handling");
    facts.recycleStatus = E_ACCESSDENIED;
    require(explorer::planNamespaceAction(NamespaceAction::EmptyRecycleBin, facts, {}, {}, &plan) == E_ACCESSDENIED,
            "Recycle Bin query HRESULT was discarded");
    facts.recycleStatus = S_OK;
    facts.selectionCount = 2;
    succeeded(explorer::planNamespaceAction(NamespaceAction::RestoreSelected, facts, {entry(18,L"undelete")}, {}, &plan),
              "Plan selected native restore on simulated Bin facts");
    require(plan.commandId == 18, "Selected restoration guessed a physical recycle path");
    facts.recycleBin = false;
    require(FAILED(explorer::planNamespaceAction(NamespaceAction::RestoreSelected, facts, {entry(18,L"undelete")}, {}, &plan)),
            "Physical folder offered namespace-only restoration");
}

void targetTypesAndFeatureMatrix() {
    NamespaceFacts files = selectedFile(L"C:\\owned\\file.txt");
    NamespaceInvocationPlan plan;
    for (const auto action : {NamespaceAction::Email,NamespaceAction::Fax,NamespaceAction::BurnToDisc}) {
        const auto command = std::wstring(explorer::namespaceActionCommand(action));
        succeeded(explorer::planNamespaceAction(action, files, {}, {entry(80,command.c_str())}, &plan), "Plan registered native sharing route");
        require(plan.enabled && plan.canonicalVerb == command, "Sharing route lost exact native command");
    }
    files.mailRecipientAvailable = true;
    files.mailRecipientPath = L"C:\\owned\\SendTo\\recipient.MAPIMail";
    succeeded(explorer::planNamespaceAction(NamespaceAction::Email, files, {}, {}, &plan), "Plan exact registered native SendTo mail recipient");
    require(plan.enabled && plan.route == NamespaceInvocationRoute::SendToMailRecipient &&
            plan.target == files.mailRecipientPath, "Mail fallback lost its installed target or native copy route");
    files.media = true;
    for (const auto action : {NamespaceAction::Play,NamespaceAction::PlayAll,NamespaceAction::AddToPlaylist,NamespaceAction::CastToDevice}) {
        const auto command = std::wstring(explorer::namespaceActionCommand(action));
        succeeded(explorer::planNamespaceAction(action, files, {}, {entry(81,command.c_str())}, &plan), "Plan native media route");
    }
    files.media = false;
    require(FAILED(explorer::planNamespaceAction(NamespaceAction::Play, files, {}, {entry(81,L"Windows.play")}, &plan)),
            "Non-media file offered media playback");
    files.images = true;
    files.castHandlerAvailable = files.castHandlerEnabled = files.castHandlerSubmenu = true;
    files.castCommandId = 9;
    succeeded(explorer::planNamespaceAction(NamespaceAction::CastToDevice, files, {}, {}, &plan),
              "Plan image casting through an isolated registered native handler");
    require(plan.route == NamespaceInvocationRoute::RegisteredHandlerMenu && plan.submenu && plan.commandId == 9,
            "Image casting guessed a translated composed-menu item");
    files.images = false;
    files.applications = true;
    succeeded(explorer::planNamespaceAction(NamespaceAction::RunAsAdministrator, files, {entry(88,L"runas")}, {}, &plan), "Plan native elevation without launching anything");
    files.applications = false;
    require(FAILED(explorer::planNamespaceAction(NamespaceAction::RunAsAdministrator, files, {entry(88,L"runas")}, {}, &plan)),
            "Document offered application elevation");
    NamespaceFacts network;
    network.filesystem = network.physicalFolders = network.uncPaths = true;
    network.singlePath = L"\\\\example\\share\\folder";
    succeeded(explorer::planNamespaceAction(NamespaceAction::MapAsDrive, network, {}, {entry(90,L"Windows.connectNetworkDrive")}, &plan),
              "Plan target-specific network mapping without a network request");
    require(FAILED(explorer::planNamespaceAction(NamespaceAction::AlwaysAvailableOffline, network, {}, {}, &plan)),
            "Inactive Offline Files service was offered as an available feature");
    network.offlineActive = true;
    network.offlineStatus = S_OK;
    succeeded(explorer::planNamespaceAction(NamespaceAction::AlwaysAvailableOffline, network, {}, {}, &plan),
              "Plan public asynchronous Offline Files pinning without a network/cache mutation");
    require(plan.route == NamespaceInvocationRoute::OfflineFilesPin && plan.canonicalVerb == L"IOfflineFilesCache.Pin" && !plan.checked,
            "Offline availability lost its exact native API or pin state");
    network.offlinePinnedForUser = true;
    succeeded(explorer::planNamespaceAction(NamespaceAction::AlwaysAvailableOffline, network, {}, {}, &plan), "Plan user pin toggle without changing shared cache");
    require(plan.checked && plan.canonicalVerb == L"IOfflineFilesCache.Unpin", "Existing per-user pin was not offered as a checked toggle");
    network.uncPaths = false;
    require(FAILED(explorer::planNamespaceAction(NamespaceAction::MapAsDrive, network, {}, {entry(90,L"Windows.connectNetworkDrive")}, &plan)),
            "Local folder offered target-specific network mapping");
    for (size_t index = 0; index < static_cast<size_t>(NamespaceAction::Count); ++index)
        require(!explorer::namespaceActionLabel(static_cast<NamespaceAction>(index)).empty(), "Namespace action is missing descriptive metadata");
}

void nativeFixtureCapabilitiesAndHeadlessGuard() {
    Fixture fixture;
    const auto imageBefore = read(fixture.image);
    const auto textBefore = read(fixture.text);
    const auto folder = item(fixture.root);
    const auto image = item(fixture.image);
    explorer::NativeNamespaceActions actions;
    explorer::NamespaceTarget target{folder,array(image.Get()),{}};
    succeeded(actions.initialize(nullptr,target), "Initialize namespace planner on owned image fixture");
    require(actions.facts().selectionCount == 1 && actions.facts().physicalFiles && actions.facts().images &&
            fs::equivalent(fs::path(actions.facts().singlePath), fixture.image), "Actual bitmap facts differ from Shell selection");
    std::vector<explorer::ContextMenuEntry> entries;
    succeeded(actions.commandStoreEntries(entries), "Enumerate installed command handlers without invoking UI");
    require(!entries.empty(), "Native CommandStore menu was unexpectedly empty");
    NamespaceInvocationPlan plan;
    succeeded(actions.planCommandStore(L"Windows.properties",&plan), "Plan exact installed generic Windows command");
    require(plan.route == NamespaceInvocationRoute::CommandStoreMenu && plan.commandId > 0 &&
            plan.canonicalVerb == L"Windows.properties", "Generic command did not preserve installed native target");
    succeeded(actions.planInvocation(NamespaceAction::RotateRight,&plan), "Plan real native bitmap rotation without modifying it");
    require(plan.enabled && (plan.canonicalVerb == L"rotate90" || plan.canonicalVerb == L"Windows.rotate90"),
            "Installed Windows bitmap rotation was not routed through its native verb");
    const HRESULT mail = actions.planInvocation(NamespaceAction::Email,&plan);
    if (SUCCEEDED(mail)) {
        require(plan.enabled && (plan.route == NamespaceInvocationRoute::SendToMailRecipient ||
                                plan.route == NamespaceInvocationRoute::SelectionMenu ||
                                plan.route == NamespaceInvocationRoute::CommandStoreMenu), "Native email capability returned a placeholder route");
        if (plan.route == NamespaceInvocationRoute::SendToMailRecipient)
            require(actions.facts().mailRecipientAvailable && fs::exists(plan.target) &&
                    _wcsicmp(fs::path(plan.target).extension().c_str(),L".MAPIMail") == 0,
                    "Email planner invented a SendTo recipient");
    } else require(!actions.facts().mailRecipientAvailable, "Available native mail recipient was not exposed");
    const HRESULT cast = actions.planInvocation(NamespaceAction::CastToDevice,&plan);
    if (SUCCEEDED(cast) && plan.route == NamespaceInvocationRoute::RegisteredHandlerMenu)
        require(actions.facts().castHandlerAvailable && plan.commandId &&
                plan.submenu == actions.facts().castHandlerSubmenu,
                "Registered PlayTo handler menu was not retained");
    for (size_t index = 0; index < static_cast<size_t>(NamespaceAction::Count); ++index)
        require(actions.invoke(static_cast<NamespaceAction>(index),true) == E_ACCESSDENIED,
                "Headless namespace action reached a native UI/destructive handler");
    require(actions.invokeCommandStore(L"Windows.runas",true) == E_ACCESSDENIED,
            "Headless generic command reached native activation");
    require(actions.invokeCommandStore(L"Windows.properties",false) == E_ACCESSDENIED,
            "Missing visible owner bypassed the UI guard");
    require(actions.planCommandStore(L"Windows.\\malformed",&plan) == E_INVALIDARG &&
            actions.planCommandStore(std::wstring_view(L"Windows.X\0Y",11),&plan) == E_INVALIDARG,
            "Malformed registry command accepted");
    require(FAILED(actions.planCommandStore(L"Windows.ThisCommandIsNotInstalled",&plan)), "Missing registered native command was invented");
    HRESULT otherThread = S_OK;
    std::thread worker([&] { NamespaceInvocationPlan ignored; otherThread = actions.planInvocation(NamespaceAction::RotateRight,&ignored); });
    worker.join();
    require(otherThread == RPC_E_WRONG_THREAD, "STA-owned native menus were accessed on another thread");
    explorer::NamespaceCommandMetadata metadata;
    metadata.label=L"preserved";
    std::thread metadataWorker([&] { otherThread=actions.commandMetadata(L"Windows.copy",&metadata); });
    metadataWorker.join();
    require(otherThread==RPC_E_WRONG_THREAD && metadata.label==L"preserved",
            "View-owned native presentation crossed apartments or changed failure output");
    require(read(fixture.image) == imageBefore && read(fixture.text) == textBefore && fs::exists(fixture.program) && fs::exists(fixture.disc),
            "Native capability planning changed owned fixture content");
    actions.reset();
    require(actions.planInvocation(NamespaceAction::Email,&plan) == E_UNEXPECTED, "Reset namespace planner retained a stale target");
    require(actions.commandMetadata(L"Windows.copy",&metadata)==E_UNEXPECTED && metadata.label==L"preserved",
            "Reset presentation query used a stale selection/site or changed output");
    target.selection.Reset();
    succeeded(actions.initialize(nullptr,target), "Initialize implied current-folder target");
    require(actions.facts().selectionCount == 0 && actions.facts().physicalFolders && !actions.facts().physicalFiles,
            "Current-folder implication became an explicit file selection");
    require(FAILED(actions.planInvocation(NamespaceAction::Email,&plan)), "Current folder was implicitly attached to mail");
    succeeded(actions.refresh(), "Refresh native capability state after operations");
    require(actions.facts().selectionCount == 0 && actions.facts().physicalFolders, "State refresh lost original target scope");
}

void metadataAndReadOnlyDriveEnumeration() {
    explorer::NamespaceCommandMetadata metadata;
    succeeded(explorer::namespaceCommandMetadata(L"Windows.copy",&metadata), "Read installed Windows copy label/icon resources");
    require(metadata.command == L"Windows.copy" && !metadata.label.empty() && !metadata.icon.empty(), "Native command metadata lost label or icon");
    metadata.label = L"preserved";
    require(explorer::namespaceCommandMetadata(L"..\\Windows.copy",&metadata) == E_INVALIDARG && metadata.label == L"preserved",
            "Invalid command metadata lookup changed output");
    require(explorer::namespaceCommandMetadata(L"Windows.copy",nullptr) == E_POINTER, "Null native metadata output accepted");
    const auto root = fs::temp_directory_path().root_path();
    const auto drive = item(root);
    ComPtr<IShellItem> computer;
    succeeded(SHGetKnownFolderItem(FOLDERID_ComputerFolder,KF_FLAG_DEFAULT,nullptr,IID_PPV_ARGS(&computer)), "Get native Computer namespace without opening a view");
    explorer::NativeNamespaceActions actions;
    succeeded(actions.initialize(nullptr,{computer,array(drive.Get()),{}}), "Initialize drive capability planner without touching a device");
    require(actions.facts().driveRoot && fs::equivalent(fs::path(actions.facts().singlePath), root), "Drive target was inferred from the wrong folder");
    NamespaceInvocationPlan plan;
    const HRESULT hr = actions.planInvocation(NamespaceAction::FormatDrive,&plan);
    if (actions.facts().driveType == DRIVE_FIXED || actions.facts().driveType == DRIVE_REMOVABLE || actions.facts().driveType == DRIVE_RAMDISK) {
        succeeded(hr,"Enumerate native format capability without opening the format dialog");
        require(plan.commandId && plan.nativeConfirmation && plan.target == root.native(), "Drive formatting plan lost its exact target");
    }
    require(actions.invoke(NamespaceAction::FormatDrive,true) == E_ACCESSDENIED &&
            actions.invoke(NamespaceAction::EjectDrive,true) == E_ACCESSDENIED &&
            actions.invoke(NamespaceAction::CleanUpDrive,true) == E_ACCESSDENIED,
            "Headless drive actions crossed the execution guard");
    succeeded(actions.commandMetadata(L"Windows.Defragment",&metadata), "Resolve native handler-derived optimization label");
    require(!metadata.label.empty() && !metadata.icon.empty(), "Native optimization title/icon is missing");
    for (const auto key : {L"Windows.Autoplay",L"Windows.FinishBurn",L"Windows.EraseDisc"}) {
        succeeded(actions.commandMetadata(key,&metadata),"Read actual installed Drive Media label/icon without invoking a device");
        require(!metadata.label.empty() && !metadata.icon.empty(),"Drive Media native presentation is missing");
        explorer::NamespaceCommandState state;
        const auto status=actions.queryCommandState(key,&state);
        require(SUCCEEDED(status)||status==E_PENDING,"Registered Drive Media state handler could not be queried read-only");
        if(SUCCEEDED(status))require(state.handler!=CLSID_NULL&&state.initialized&&!state.explorerCommand,
                "Drive Media state did not use its exact initialized native state-only handler");
        require(actions.invokeCommandStore(key,true)==E_ACCESSDENIED,"Drive Media command crossed its headless device guard");
    }
}

void nativeViewGalleryResources() {
    std::vector<explorer::NamespaceSubcommandMetadata> gallery;
    succeeded(explorer::namespaceCommandChildren(L"Windows.IconSize",nullptr,nullptr,&gallery),
              "Enumerate native Windows View gallery resources without changing a view");
    require(gallery.size() == 8, "Windows View gallery does not contain all eight native modes");
    const std::array<const wchar_t*,8> icons{L"shell32.dll,-63001",L"shell32.dll,-63008",L"shell32.dll,-63009",L"shell32.dll,-63010",
                                           L"shell32.dll,-63000",L"shell32.dll,-62998",L"shell32.dll,-62999",L"shell32.dll,-63011"};
    for (size_t index = 0; index < gallery.size(); ++index) {
        require(!gallery[index].label.empty() && !gallery[index].icon.empty(), "Native View mode title/icon was replaced with a placeholder");
        require(gallery[index].icon == icons[index], "Native View mode order or resource differs from installed Windows 10 gallery");
    }
    const auto first = gallery[0].label;
    require(explorer::namespaceCommandChildren(L"Windows.Bad\\Command",nullptr,nullptr,&gallery) == E_INVALIDARG && gallery[0].label == first,
            "Invalid child resource request changed output");
    require(explorer::namespaceCommandChildren(L"Windows.IconSize",nullptr,nullptr,nullptr) == E_POINTER,
            "Null native gallery output accepted");
}

class AggregateSelectionMenu final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IContextMenu,IObjectWithSite> {
public:
    unsigned queries=0,invocations=0,attachments=0,detachments=0;
    IUnknown* expectedSite=nullptr;
    bool exactSite=false;
    HRESULT STDMETHODCALLTYPE QueryContextMenu(HMENU menu,UINT position,UINT first,UINT last,UINT) override {
        ++queries;exactSite=site_.Get()==expectedSite;
        if(last<first+2)return E_INVALIDARG;
        if(!InsertMenuW(menu,position,MF_BYPOSITION|MF_STRING,first+2,L"Owned native provider leaf"))
            return HRESULT_FROM_WIN32(GetLastError());
        return MAKE_HRESULT(SEVERITY_SUCCESS,0,3);
    }
    HRESULT STDMETHODCALLTYPE InvokeCommand(CMINVOKECOMMANDINFO*) override { ++invocations;return E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE GetCommandString(UINT_PTR ordinal,UINT flags,UINT*,LPSTR text,UINT capacity) override {
        if(ordinal!=2||flags!=GCS_VERBW)return E_NOTIMPL;
        return wcscpy_s(reinterpret_cast<wchar_t*>(text),capacity,L"Windows.burn")?E_FAIL:S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) override {
        if(site)++attachments;else++detachments;site_=site;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSite(REFIID iid,void** result) override {
        if(!result)return E_POINTER;*result=nullptr;
        return site_?site_->QueryInterface(iid,result):E_NOINTERFACE;
    }
private:
    ComPtr<IUnknown> site_;
};

class AggregateSelection final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IShellItemArray> {
public:
    DWORD count=100001;
    SFGAOF prefix=SFGAO_FILESYSTEM|SFGAO_FOLDER|SFGAO_LINK;
    SFGAOF tail=SFGAO_FILESYSTEM;
    DWORD inspected=0;
    unsigned itemReads=0,attributeCalls=0,binds=0;
    SIATTRIBFLAGS requested=SIATTRIBFLAGS_OR;
    SFGAOF requestedMask=0;
    HRESULT attributeStatus=S_FALSE;
    ComPtr<AggregateSelectionMenu> menu=Microsoft::WRL::Make<AggregateSelectionMenu>();
    HRESULT STDMETHODCALLTYPE BindToHandler(IBindCtx*,REFGUID handler,REFIID iid,void** result) override {
        if(!result)return E_POINTER;*result=nullptr;++binds;
        return handler==BHID_SFUIObject?menu->QueryInterface(iid,result):E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyStore(GETPROPERTYSTOREFLAGS,REFIID,void** result) override {
        if(!result)return E_POINTER;*result=nullptr;return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyDescriptionList(REFPROPERTYKEY,REFIID,void** result) override {
        if(!result)return E_POINTER;*result=nullptr;return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetAttributes(SIATTRIBFLAGS flags,SFGAOF mask,SFGAOF* result) override {
        if(!result)return E_POINTER;++attributeCalls;requested=flags;requestedMask=mask;
        if(FAILED(attributeStatus))return attributeStatus;
        if(flags!=static_cast<SIATTRIBFLAGS>(SIATTRIBFLAGS_AND|SIATTRIBFLAGS_ALLITEMS))return E_INVALIDARG;
        SFGAOF combined=mask;inspected=0;
        for(DWORD index=0;index<count;++index){combined&=index==count-1?tail:prefix;++inspected;}
        *result=combined;return attributeStatus;
    }
    HRESULT STDMETHODCALLTYPE GetCount(DWORD* result) override { if(!result)return E_POINTER;*result=count;return S_OK; }
    HRESULT STDMETHODCALLTYPE GetItemAt(DWORD,IShellItem** result) override {
        ++itemReads;if(!result)return E_POINTER;*result=nullptr;return E_UNEXPECTED;
    }
    HRESULT STDMETHODCALLTYPE EnumItems(IEnumShellItems** result) override {
        if(!result)return E_POINTER;*result=nullptr;return E_NOTIMPL;
    }
};

void aggregateLargeSelectionAndProviderGuards() {
    Fixture fixture;const auto folder=item(fixture.root);
    const auto textBefore=read(fixture.text),imageBefore=read(fixture.image);
    const auto clipboard=GetClipboardSequenceNumber();
    const auto selected=Microsoft::WRL::Make<AggregateSelection>();
    ComPtr<IShellItemArray> full;full=selected;
    ComPtr<IUnknown> site;succeeded(folder.As(&site),"Retain exact mock view-site identity");
    selected->menu->expectedSite=site.Get();
    explorer::NativeNamespaceActions actions;
    succeeded(actions.initialize(nullptr,{folder,full,site}),"Initialize complete 100001-item selection through native aggregate attributes");
    const auto& facts=actions.facts();
    require(facts.selectionCount==100001 && facts.filesystem && facts.nativeAttributes==SFGAO_FILESYSTEM &&
            facts.nativeAttributesStatus==S_FALSE && !facts.detailedTargetsKnown,
            "Large selection lost its final-item counterexample or unknown-metadata distinction");
    require(selected->inspected==selected->count && selected->requested==static_cast<SIATTRIBFLAGS>(SIATTRIBFLAGS_AND|SIATTRIBFLAGS_ALLITEMS) &&
            selected->requestedMask==(SFGAO_FILESYSTEM|SFGAO_FOLDER|SFGAO_LINK) && selected->itemReads==0,
            "Large facts used a prefix, eager item reads or approximate attributes");
    require(!facts.physicalFiles&&!facts.physicalFolders&&!facts.images&&!facts.media&&!facts.applications&&
            !facts.uncPaths&&facts.singlePath.empty(),"Unknown optional large-selection details were fabricated");
    NamespaceInvocationPlan plan;
    succeeded(actions.planInvocation(NamespaceAction::BurnToDisc,&plan),"Plan actual full-array provider leaf without per-item metadata");
    require(plan.enabled&&plan.route==NamespaceInvocationRoute::SelectionMenu&&plan.canonicalVerb==L"Windows.burn"&&
            selected->binds==1&&selected->menu->exactSite&&selected->menu->attachments==1&&selected->itemReads==0,
            "Full-array native context provider or its exact site was replaced/truncated");
    for(const auto action:{NamespaceAction::Email,NamespaceAction::Fax,NamespaceAction::Play,NamespaceAction::RotateLeft}) {
        require(explorer::namespaceActionApplicable(action,facts),"Unknown optional details blocked an exact native provider query");
        NamespaceFacts unknown=facts;unknown.mailRecipientAvailable=unknown.offlineActive=true;
        unknown.mailRecipientPath=L"owned installed recipient.MAPIMail";
        require(FAILED(explorer::planNamespaceAction(action,unknown,{},{},&plan)),
                "Unknown large-selection details enabled a recipient/type fallback without native evidence");
    }
    require(actions.invoke(NamespaceAction::BurnToDisc,true)==E_ACCESSDENIED&&
            actions.invoke(NamespaceAction::BurnToDisc,false)==E_ACCESSDENIED&&
            actions.invokeCommandStore(L"Windows.recycle",true)==E_ACCESSDENIED&&selected->menu->invocations==0,
            "Large selection weakened a native activation/mutation guard");
    actions.reset();require(selected->menu->detachments==1,"Large provider site remained attached after reset");
    selected->tail=0;
    succeeded(actions.initialize(nullptr,{folder,full,site}),"Retain native all-items filesystem counterexample at index100000");
    require(!actions.facts().filesystem && actions.facts().nativeAttributes==0 && selected->itemReads==0,
            "A last nonfilesystem item was ignored in native eligibility");
    require(!explorer::namespaceActionApplicable(NamespaceAction::ShareSpecificPeople,actions.facts()),
            "A virtual tail inherited filesystem sharing permission from its prefix");
    selected->tail=selected->prefix;
    succeeded(actions.initialize(nullptr,{folder,full,site}),"Read all-folders/all-links native aggregate truth");
    require((actions.facts().nativeAttributes&(SFGAO_FOLDER|SFGAO_LINK))==(SFGAO_FOLDER|SFGAO_LINK) &&
            !explorer::namespaceActionApplicable(NamespaceAction::Play,actions.facts()),
            "Verified all-folders native selection acquired a file-only context");
    selected->attributeStatus=E_ACCESSDENIED;
    require(actions.refresh()==E_ACCESSDENIED && actions.facts().nativeAttributes==(SFGAO_FILESYSTEM|SFGAO_FOLDER|SFGAO_LINK),
            "Failed native aggregate refresh invented attributes or changed existing facts");
    require(actions.initialize(nullptr,{folder,full,site})==E_ACCESSDENIED&&actions.facts().selectionCount==0,
            "Failed initial aggregate query retained partially initialized targets");
    require(read(fixture.text)==textBefore&&read(fixture.image)==imageBefore&&GetClipboardSequenceNumber()==clipboard,
            "Large-array capability proof changed owned contents or clipboard");
}

void nativeLargeArrayTailCounterexamples() {
    Fixture fixture;const auto folder=item(fixture.root),text=item(fixture.text);
    const auto before=read(fixture.text);const auto clipboard=GetClipboardSequenceNumber();
    struct Identities {
        PIDLIST_ABSOLUTE folder=nullptr,text=nullptr,virtualItem=nullptr,shortcut=nullptr;
        ~Identities(){CoTaskMemFree(folder);CoTaskMemFree(text);CoTaskMemFree(virtualItem);CoTaskMemFree(shortcut);}
    } identities;
    succeeded(SHGetIDListFromObject(folder.Get(),&identities.folder),"Retain owned folder PIDL for full native large array");
    succeeded(SHGetIDListFromObject(text.Get(),&identities.text),"Retain owned file PIDL for native final-item counterexample");
    ComPtr<IShellItem> controlPanel;
    succeeded(SHGetKnownFolderItem(FOLDERID_ControlPanelFolder,KF_FLAG_DONT_VERIFY,nullptr,IID_PPV_ARGS(&controlPanel)),
              "Get static native virtual identity without enumerating user items");
    succeeded(SHGetIDListFromObject(controlPanel.Get(),&identities.virtualItem),"Retain static virtual PIDL for native tail counterexample");
    ComPtr<IShellLinkW> link;succeeded(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&link)),
                                     "Create actual owned shortcut without resolving or launching it");
    succeeded(link->SetPath(fixture.text.c_str()),"Set only the stored owned shortcut target");
    ComPtr<IPersistFile> persistence;succeeded(link.As(&persistence),"Read owned shortcut persistence interface");
    const auto shortcutPath=fixture.root/L"owned shortcut.lnk";
    succeeded(persistence->Save(shortcutPath.c_str(),TRUE),"Save only the owned shortcut identity");
    const auto shortcut=item(shortcutPath);
    succeeded(SHGetIDListFromObject(shortcut.Get(),&identities.shortcut),"Retain real native shortcut PIDL");
    std::vector<PCIDLIST_ABSOLUTE> ids(100001,identities.folder);
    explorer::NativeNamespaceActions actions;
    const std::array<std::pair<PCIDLIST_ABSOLUTE,PCIDLIST_ABSOLUTE>,3> examples{{
        {identities.folder,identities.text},{identities.folder,identities.virtualItem},{identities.shortcut,identities.text}}};
    for(const auto& [prefix,tail]:examples) {
        std::fill(ids.begin(),ids.end(),prefix);ids.back()=tail;
        ComPtr<IShellItemArray> selected;
        succeeded(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(ids.size()),ids.data(),&selected),
                  "Construct real native 100001-item array from repeated owned identities");
        DWORD count=0;succeeded(selected->GetCount(&count),"Read exact native large array count");
        require(count==ids.size(),"Native large-array construction changed the original item count");
        SFGAOF expected=0;
        const auto status=selected->GetAttributes(static_cast<SIATTRIBFLAGS>(SIATTRIBFLAGS_AND|SIATTRIBFLAGS_ALLITEMS),
                                                 SFGAO_FILESYSTEM|SFGAO_FOLDER|SFGAO_LINK,&expected);
        succeeded(status,"Compute documented native AND attributes through final counterexample");
        succeeded(actions.initialize(nullptr,{folder,selected,{}}),"Retain real full native large array without item cap");
        require(actions.facts().selectionCount==count&&actions.facts().nativeAttributes==expected&&
                actions.facts().nativeAttributesStatus==status&&!actions.facts().detailedTargetsKnown,
                "Production native aggregate facts differ from complete native array readback");
        std::cout<<"Large native array tailVirtual="<<(tail==identities.virtualItem)<<" prefixLink="<<(prefix==identities.shortcut)
                 <<" attributes=0x"<<std::hex<<expected<<std::dec<<" status="<<status<<'\n';
        require(((expected&SFGAO_FOLDER)!=0)==(tail==identities.virtualItem)&&!(expected&SFGAO_LINK)&&
                (!(expected&SFGAO_FILESYSTEM)==(tail==identities.virtualItem)),
                "Native final file/virtual counterexample was lost behind a repeated-folder prefix");
        ComPtr<IShellItem> retainedTail;succeeded(selected->GetItemAt(count-1,&retainedTail),"Read unchanged exact native last target");
        int order=1;succeeded(retainedTail->Compare(tail==identities.text?text.Get():controlPanel.Get(),SICHINT_CANONICAL,&order),
                              "Compare retained original last-item identity");
        require(order==0,"The native full selection lost its last target identity");
        explorer::NamespaceCommandState facade,direct;
        const auto directStatus=explorer::namespaceCommandState(L"Windows.ShareSpecificUsers",selected.Get(),nullptr,&direct);
        const auto facadeStatus=actions.queryCommandState(L"Windows.ShareSpecificUsers",&facade);
        require(directStatus==facadeStatus&&(!SUCCEEDED(directStatus)||direct.state==facade.state),
                "Large-selection facade changed exact native provider status/state");
        require(actions.invokeCommandStore(L"Windows.ShareSpecificUsers",true)==E_ACCESSDENIED&&
                actions.invokeZip(true)==E_ACCESSDENIED,"Native large-array proof activated recipients or payload handlers");
        actions.reset();
    }
    require(read(fixture.text)==before&&fs::exists(fixture.image)&&GetClipboardSequenceNumber()==clipboard,
            "Native large-array attribute/state proof changed owned files or clipboard");
}
struct SnapshotHandle {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit SnapshotHandle(HANDLE handle):value(handle){}
    ~SnapshotHandle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    SnapshotHandle(const SnapshotHandle&)=delete;
    SnapshotHandle& operator=(const SnapshotHandle&)=delete;
};

void createSnapshotFile(const std::wstring& path) {
    SnapshotHandle file(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    require(file.value!=INVALID_HANDLE_VALUE,"Create only an owned snapshot fixture file");
}

BY_HANDLE_FILE_INFORMATION snapshotFileInformation(const std::wstring& path,bool reparse=false) {
    SnapshotHandle file(CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|(reparse?FILE_FLAG_OPEN_REPARSE_POINT:0),nullptr));
    require(file.value!=INVALID_HANDLE_VALUE,"Open owned identity for native snapshot evidence");
    BY_HANDLE_FILE_INFORMATION result{};
    require(GetFileInformationByHandle(file.value,&result),"Read actual owned file ID and attributes");
    return result;
}

bool sameSnapshotFile(const BY_HANDLE_FILE_INFORMATION& left,const BY_HANDLE_FILE_INFORMATION& right) {
    return left.dwVolumeSerialNumber==right.dwVolumeSerialNumber&&left.nFileIndexHigh==right.nFileIndexHigh&&
        left.nFileIndexLow==right.nFileIndexLow;
}

struct SnapshotFixture {
    Fixture owned;
    fs::path parent=owned.root/L"snapshot facts";
    std::vector<std::wstring> paths;
    SnapshotFixture() {
        require(fs::create_directory(parent),"Create owned snapshot parent");
        paths.reserve(64);
        for(unsigned index=0;index<64;++index) {
            paths.push_back((parent/(index?L"Member-"+std::to_wstring(index)+L".txt":
                std::wstring(L"Unicode-\u65E5\u672C\u8A9E-\u03BB-\U0001F4C1.txt"))).wstring());
            createSnapshotFile(paths.back());
        }
    }
};

void snapshotNativeBoundariesAndAliases() {
    SnapshotFixture fixture;
    explorer::detail::ParentAttributeSnapshot below(63),at(64);
    for(unsigned index=0;index<64;++index) {
        const auto& path=fixture.paths[index];const auto native=snapshotFileInformation(path);
        require(native.dwFileAttributes==GetFileAttributesW(path.c_str()),"Native file-handle and per-item attributes disagree");
        if(index<63) {
            require(below.attributes(path)==native.dwFileAttributes&&!below.lastFromSnapshot(),
                    "63-member selection crossed the snapshot threshold");
        }
        require(at.attributes(path)==native.dwFileAttributes&&at.lastFromSnapshot(),
                "64-member snapshot changed exact Unicode/native file attributes");
    }
    auto alias=fixture.paths.front();const auto leaf=alias.find_last_of(L'\\')+1;
    alias.replace(leaf,7,L"UNICODE");
    const auto aliasAttributes=GetFileAttributesW(alias.c_str());
    require(at.attributes(alias)==aliasAttributes&&!at.lastFromSnapshot(),"Differently cased alias bypassed the per-item call");
    if(aliasAttributes!=INVALID_FILE_ATTRIBUTES)
        require(sameSnapshotFile(snapshotFileInformation(alias),snapshotFileInformation(fixture.paths.front())),
                "Case alias named a different actual file ID");
    else std::cout<<"UNAVAILABLE: existing case alias on this owned directory; native attributes still checked\n";

    std::array<wchar_t,32768> shortPath{};
    const auto shortLength=GetShortPathNameW(fixture.paths.front().c_str(),shortPath.data(),static_cast<DWORD>(shortPath.size()));
    if(!shortLength) {
        const auto error=GetLastError();
        require(error==ERROR_NOT_SUPPORTED||error==ERROR_INVALID_FUNCTION,"Unexpected owned short-path query failure");
        std::cout<<"UNAVAILABLE: owned 8.3 alias query Win32="<<error<<'\n';
    } else {
        require(shortLength<shortPath.size(),"Owned short-path result exceeded bounded buffer");
        const std::wstring shortName(shortPath.data());
        if(shortName!=fixture.paths.front()) {
            require(sameSnapshotFile(snapshotFileInformation(shortName),snapshotFileInformation(fixture.paths.front())),
                    "Actual 8.3 alias changed owned file identity");
            require(at.attributes(shortName)==GetFileAttributesW(shortName.c_str())&&!at.lastFromSnapshot(),
                    "Actual 8.3 alias bypassed native attribute fallback");
            std::cout<<"COVERED: actual owned 8.3 alias and matching native file ID\n";
        } else std::cout<<"UNAVAILABLE: no actual 8.3 alias was assigned to this owned file\n";
    }
    const auto other=fixture.owned.root/L"different parent";
    require(fs::create_directory(other),"Create a different owned parent");
    const auto elsewhere=(other/fs::path(fixture.paths[1]).filename()).wstring();
    require(CreateDirectoryW(elsewhere.c_str(),nullptr),"Create same-leaf owned directory in a different parent");
    const auto otherInfo=snapshotFileInformation(elsewhere);
    require((otherInfo.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&
            !sameSnapshotFile(otherInfo,snapshotFileInformation(fixture.paths[1])),"Different-parent counterexample lacks distinct native identity/type");
    require(at.attributes(elsewhere)==otherInfo.dwFileAttributes&&!at.lastFromSnapshot(),
            "Different parent's same-leaf directory borrowed the first parent's file attributes");
    const auto missing=(fixture.owned.root/L"absent parent"/L"missing.txt").wstring();
    explorer::detail::ParentAttributeSnapshot failed(64);
    require(GetFileAttributesW(missing.c_str())==INVALID_FILE_ATTRIBUTES&&GetLastError()==ERROR_PATH_NOT_FOUND,
            "Owned missing-parent case did not exercise a real native path failure");
    require(failed.attributes(missing)==INVALID_FILE_ATTRIBUTES&&!failed.lastFromSnapshot(),
            "Failed native parent enumeration fabricated attributes");
    require(failed.attributes(fixture.paths.front())==GetFileAttributesW(fixture.paths.front().c_str())&&!failed.lastFromSnapshot(),
            "Failed enumeration stopped returning fresh per-item attributes");

    // A real resident shortcut is always per item, even after its parent is
    // loaded. Change only its owned attributes to prove the fallback is fresh.
    const auto shortcut=(fixture.parent/L"actual owned shortcut.LnK").wstring();
    ComPtr<IShellLinkW> link;succeeded(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&link)),
                                     "Create owned resident shortcut for snapshot fallback");
    succeeded(link->SetPath(fixture.owned.text.c_str()),"Store only an owned shortcut target");
    ComPtr<IPersistFile> saved;succeeded(link.As(&saved),"Get owned shortcut persistence");
    succeeded(saved->Save(shortcut.c_str(),TRUE),"Save actual owned shortcut without resolving it");
    explorer::detail::ParentAttributeSnapshot withLink(64);
    require(withLink.attributes(fixture.paths.front())==GetFileAttributesW(fixture.paths.front().c_str())&&withLink.lastFromSnapshot(),
            "Load shared parent including actual shortcut");
    const auto original=GetFileAttributesW(shortcut.c_str());
    require(original!=INVALID_FILE_ATTRIBUTES&&SetFileAttributesW(shortcut.c_str(),original|FILE_ATTRIBUTE_HIDDEN),
            "Change only the owned resident shortcut after enumeration");
    const auto observed=withLink.attributes(shortcut);const auto native=GetFileAttributesW(shortcut.c_str());
    require(SetFileAttributesW(shortcut.c_str(),original),"Restore owned shortcut attributes");
    require(observed==native&&(native&FILE_ATTRIBUTE_HIDDEN)&&!withLink.lastFromSnapshot(),
            "Shortcut reused stale enumerated attributes instead of actual item safeguards");
}

std::wstring extendedSnapshotPath(const fs::path& path) {
    const auto ordinary=path.wstring();
    require(path.is_absolute()&&ordinary.size()>3&&ordinary[1]==L':',"Extended fixture must stay on its owned absolute local drive");
    return L"\\\\?\\"+ordinary;
}

struct ExtendedSnapshotCleanup {
    std::vector<std::wstring> files,directories;
    ExtendedSnapshotCleanup(){files.reserve(8);directories.reserve(20);}
    ~ExtendedSnapshotCleanup() {
        for(const auto& file:files)DeleteFileW(file.c_str());
        for(auto it=directories.rbegin();it!=directories.rend();++it)RemoveDirectoryW(it->c_str());
    }
    void removeFiles() {
        for(const auto& file:files)require(DeleteFileW(file.c_str()),"Explicitly remove owned extended/reserved file");
        files.clear();
    }
};

void snapshotExtendedReservedAndLongPaths() {
    SnapshotFixture fixture;ExtendedSnapshotCleanup cleanup;
    explorer::detail::ParentAttributeSnapshot snapshot(64);
    const auto extended=extendedSnapshotPath(fs::path(fixture.paths.front()));
    require(sameSnapshotFile(snapshotFileInformation(extended),snapshotFileInformation(fixture.paths.front())),
            "Extended spelling changed the real owned file ID");
    require(snapshot.attributes(extended)==GetFileAttributesW(extended.c_str())&&!snapshot.lastFromSnapshot(),
            "Extended path entered ordinary-name enumeration");
    for(const auto name:{L"COM\u00B9.txt",L"LPT\u00B2.txt"}) {
        cleanup.files.push_back(extendedSnapshotPath(fixture.parent/name));createSnapshotFile(cleanup.files.back());
        const auto info=snapshotFileInformation(cleanup.files.back());
        require(!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY),"Extended literal device-name fixture is not an actual file");
        require(!explorer::validLeafName(name),"Reserved superscript device name passed ordinary leaf validation");
    }
    // Load with both literal reserved entries present; ordinary device
    // spellings must still use their actual native answer, whatever it is.
    require(snapshot.attributes(fixture.paths.front())==GetFileAttributesW(fixture.paths.front().c_str())&&snapshot.lastFromSnapshot(),
            "Load parent containing literal reserved superscript entries");
    for(const auto& path:cleanup.files) {
        const auto ordinary=path.substr(4);
        require(snapshot.attributes(ordinary)==GetFileAttributesW(ordinary.c_str())&&!snapshot.lastFromSnapshot(),
                "Literal superscript device name was incorrectly used for an ordinary device spelling");
        require(snapshot.attributes(path)==snapshotFileInformation(path).dwFileAttributes&&!snapshot.lastFromSnapshot(),
                "Extended literal reserved file bypassed native per-item attributes");
    }
    auto longParent=fixture.parent;
    while(longParent.wstring().size()<MAX_PATH+32) {
        longParent/=L"owned-long-segment-123456";
        cleanup.directories.push_back(extendedSnapshotPath(longParent));
        require(CreateDirectoryW(cleanup.directories.back().c_str(),nullptr),"Create only an owned extended long-path directory");
    }
    const auto longFile=longParent/L"Unicode-\u65E5\u672C\u8A9E.txt";
    cleanup.files.push_back(extendedSnapshotPath(longFile));createSnapshotFile(cleanup.files.back());
    require(snapshot.attributes(cleanup.files.back())==snapshotFileInformation(cleanup.files.back()).dwFileAttributes&&
            !snapshot.lastFromSnapshot(),"Extended long path changed actual native attributes");
    require(longFile.wstring().size()>=MAX_PATH&&snapshot.attributes(longFile.wstring())==GetFileAttributesW(longFile.c_str())&&
            !snapshot.lastFromSnapshot(),"Ordinary long-path spelling bypassed its actual native result");
    cleanup.removeFiles();
    for(auto it=cleanup.directories.rbegin();it!=cleanup.directories.rend();++it)
        require(RemoveDirectoryW(it->c_str()),"Explicitly remove owned extended long-path directory");
    cleanup.directories.clear();
}

void snapshotOwnedJunctionAndMutation() {
    SnapshotFixture fixture;
    const auto target=fixture.owned.root/L"junction target",junction=fixture.parent/L"owned junction";
    require(fs::create_directory(target)&&fs::create_directory(junction),"Create only owned junction and target directories");
    struct JunctionCleanup {fs::path path;~JunctionCleanup(){if(!path.empty())RemoveDirectoryW(path.c_str());}} cleanup{junction};
    const auto substitute=L"\\??\\"+target.wstring(),print=target.wstring();
    // Native mount-point reparse buffer: no symlink privilege, shell command,
    // machine policy or filesystem setting is used by this owned test.
    struct MountPointBuffer {
        DWORD tag=IO_REPARSE_TAG_MOUNT_POINT;WORD bytes=0,reserved=0;
        WORD substituteOffset=0,substituteBytes=0,printOffset=0,printBytes=0;
        wchar_t paths[MAX_PATH*2]{};
    } buffer;
    require(substitute.size()+print.size()+2<std::size(buffer.paths),"Owned junction target exceeded bounded native buffer");
    buffer.substituteBytes=static_cast<WORD>(substitute.size()*sizeof(wchar_t));
    buffer.printOffset=buffer.substituteBytes+sizeof(wchar_t);buffer.printBytes=static_cast<WORD>(print.size()*sizeof(wchar_t));
    buffer.bytes=static_cast<WORD>(8+buffer.printOffset+buffer.printBytes+sizeof(wchar_t));
    std::memcpy(buffer.paths,substitute.c_str(),buffer.substituteBytes);
    std::memcpy(reinterpret_cast<BYTE*>(buffer.paths)+buffer.printOffset,print.c_str(),buffer.printBytes);
    DWORD error=ERROR_SUCCESS;
    {
        SnapshotHandle handle(CreateFileW(junction.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        require(handle.value!=INVALID_HANDLE_VALUE,"Open owned junction for native FSCTL");
        DWORD returned=0;
        if(!DeviceIoControl(handle.value,FSCTL_SET_REPARSE_POINT,&buffer,static_cast<DWORD>(8+buffer.bytes),
                            nullptr,0,&returned,nullptr))error=GetLastError();
    }
    if(error==ERROR_SUCCESS) {
        const auto junctionInfo=snapshotFileInformation(junction.wstring(),true);
        const auto targetInfo=snapshotFileInformation(target.wstring());
        require((junctionInfo.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)&&
                sameSnapshotFile(snapshotFileInformation(junction.wstring()),targetInfo)&&!sameSnapshotFile(junctionInfo,targetInfo),
                "Owned FSCTL mount point did not establish distinct reparse and actual target file IDs");
        explorer::detail::ParentAttributeSnapshot snapshot(64);
        require(snapshot.attributes(fixture.paths.front())==GetFileAttributesW(fixture.paths.front().c_str())&&snapshot.lastFromSnapshot(),
                "Load parent containing real owned junction");
        require(snapshot.attributes(junction.wstring())==GetFileAttributesW(junction.c_str())&&!snapshot.lastFromSnapshot(),
                "Reparse directory reused enumerated attributes");
        std::cout<<"COVERED: owned FSCTL junction with actual reparse/target file IDs\n";
    } else {
        require(error==ERROR_INVALID_FUNCTION||error==ERROR_NOT_SUPPORTED||error==ERROR_PRIVILEGE_NOT_HELD||error==ERROR_ACCESS_DENIED,
                "Owned junction FSCTL failed for an unexpected reason");
        std::cout<<"UNAVAILABLE: owned FSCTL junction Win32="<<error<<'\n';
    }
    require(RemoveDirectoryW(junction.c_str()),"Remove only owned junction without traversing target");
    cleanup.path.clear();

    const auto& victim=fixture.paths.back();const auto original=snapshotFileInformation(victim);
    explorer::detail::ParentAttributeSnapshot loaded(64);
    require(loaded.attributes(fixture.paths.front())==GetFileAttributesW(fixture.paths.front().c_str())&&loaded.lastFromSnapshot(),
            "Load UI snapshot before an owned mutation");
    require(DeleteFileW(victim.c_str())&&CreateDirectoryW(victim.c_str(),nullptr),"Replace only owned file with directory after snapshot load");
    const auto current=snapshotFileInformation(victim);
    require(current.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY,"Owned mutation did not become a real native directory");
    require(loaded.attributes(victim)==original.dwFileAttributes&&loaded.lastFromSnapshot(),
            "Loaded snapshot no longer documents its instantaneous pre-mutation UI fact");
    require(GetFileAttributesW(victim.c_str())==current.dwFileAttributes,"Fresh native per-item read missed the owned mutation");
    explorer::detail::ParentAttributeSnapshot fresh(64);
    require(fresh.attributes(victim)==current.dwFileAttributes&&fresh.lastFromSnapshot(),
            "New per-call snapshot retained an older UI fact");
    const auto& removed=fixture.paths[62];
    require(DeleteFileW(removed.c_str()),"Remove only owned selected file before a new snapshot load");
    require(GetFileAttributesW(removed.c_str())==INVALID_FILE_ATTRIBUTES&&GetLastError()==ERROR_FILE_NOT_FOUND,
            "Removed-before-load fixture did not produce actual native file-not-found");
    explorer::detail::ParentAttributeSnapshot afterRemoval(64);
    require(afterRemoval.attributes(fixture.paths.front())==GetFileAttributesW(fixture.paths.front().c_str())&&afterRemoval.lastFromSnapshot(),
            "Load parent after the owned selected file was removed");
    require(afterRemoval.attributes(removed)==INVALID_FILE_ATTRIBUTES&&!afterRemoval.lastFromSnapshot(),
            "Missing enumerated name was treated as an existing owned selected file");
    // These observations authorize no operation. Public native activation
    // guards and complete retained Shell arrays are tested separately below.
}

// Large same-parent selections read existence/directory facts from one parent
// enumeration. Every case below must match the per-item native answer.
void largeSelectionParentSnapshotFacts() {
    Fixture fixture;
    const auto parent=fixture.root/L"large selection";
    const auto other=fixture.root/L"other parent";
    require(fs::create_directory(parent)&&fs::create_directory(other),"Create owned large-selection parents");
    struct Ids {
        std::vector<PIDLIST_ABSOLUTE> values;
        ~Ids(){for(const auto value:values)CoTaskMemFree(value);}
        PCIDLIST_ABSOLUTE add(const fs::path& path) {
            PIDLIST_ABSOLUTE raw=nullptr;succeeded(SHGetIDListFromObject(item(path).Get(),&raw),"Read owned large-selection PIDL");
            values.push_back(raw);return raw;
        }
    } ids;
    std::vector<PCIDLIST_ABSOLUTE> files,folders;
    for(unsigned index=0;index<200;++index) {
        const auto file=parent/(L"Item-"+std::to_wstring(index)+L".txt");
        std::ofstream(file,std::ios::binary)<<"owned";
        files.push_back(ids.add(file));
    }
    for(unsigned index=0;index<70;++index) {
        const auto folder=parent/(L"Folder-"+std::to_wstring(index));
        require(fs::create_directory(folder),"Create owned large-selection folder");
        folders.push_back(ids.add(folder));
    }
    const auto otherFile=other/L"Elsewhere.txt";std::ofstream(otherFile,std::ios::binary)<<"owned";
    const auto elsewhere=ids.add(otherFile);
    const auto folder=item(parent);
    explorer::NativeNamespaceActions actions;
    const auto describe=[&](std::vector<PCIDLIST_ABSOLUTE> selection) {
        ComPtr<IShellItemArray> selected;
        succeeded(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(selection.size()),selection.data(),&selected),
                  "Create owned large-selection array");
        DWORD count=0;succeeded(selected->GetCount(&count),"Read genuine complete snapshot selection count");
        require(count==selection.size(),"Snapshot test lost members of the actual selected array");
        SFGAOF native=0;
        const auto nativeStatus=selected->GetAttributes(static_cast<SIATTRIBFLAGS>(SIATTRIBFLAGS_AND|SIATTRIBFLAGS_ALLITEMS),
            SFGAO_FILESYSTEM|SFGAO_FOLDER|SFGAO_LINK,&native);
        succeeded(nativeStatus,"Read independent native attributes over every actual selected item");
        succeeded(actions.initialize(nullptr,{folder,selected,{}}),"Describe owned large selection");
        require(actions.facts().selectionCount==count&&actions.facts().nativeAttributesStatus==nativeStatus&&
                actions.facts().nativeAttributes==native,"UI snapshot changed whole-array native authority");
        require(actions.invokeCommandStore(L"Windows.ShareSpecificUsers",true)==E_ACCESSDENIED&&actions.invokeZip(true)==E_ACCESSDENIED,
                "Snapshot UI facts authorized a native operation in a headless test");
        const auto facts=actions.facts();actions.reset();return facts;
    };
    auto facts=describe(files);
    require(facts.detailedTargetsKnown&&facts.physicalFiles&&!facts.physicalFolders&&!facts.images&&!facts.discImages,
            "Snapshot changed all-file selection facts");
    facts=describe(std::vector(files.begin(),files.begin()+63));
    require(facts.physicalFiles&&!facts.physicalFolders,"Per-item boundary selection changed file facts");
    facts=describe(std::vector(files.begin(),files.begin()+64));
    require(facts.physicalFiles&&!facts.physicalFolders,"Snapshot boundary selection changed file facts");
    facts=describe(folders);
    require(facts.physicalFolders&&!facts.physicalFiles,"Snapshot changed all-folder selection facts");
    auto mixed=files;mixed.push_back(folders.back());
    facts=describe(mixed);
    require(!facts.physicalFiles&&!facts.physicalFolders,"Snapshot lost a final folder counterexample");
    auto parents=files;parents.push_back(elsewhere);
    facts=describe(parents);
    require(facts.physicalFiles&&!facts.physicalFolders,"Another parent's existing file was not established per item");
    // The array retains a PIDL for a file removed after construction. Absent
    // snapshot entries must use the actual per-item answer, not stale state.
    ComPtr<IShellItemArray> retained;
    succeeded(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(files.size()),files.data(),&retained),
              "Retain owned identities before removal");
    require(fs::remove(parent/L"Item-199.txt"),"Remove one owned large-selection file");
    const auto removed=actions.initialize(nullptr,{folder,retained,{}});
    require(FAILED(removed)||!actions.facts().physicalFiles,"A removed final file was reported as an existing physical file");
    actions.reset();
}
} // namespace

int runNamespaceActionTests() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"semantic target guards and exact canonical verbs",semanticPlanningAndCanonicalVerbs},
        {"disabled ancestors, ambiguous handlers and native cascades",disabledMenusAndAmbiguousHandlers},
        {"drive and simulated Recycle Bin action planning",destructiveAndOptionalActionPlanning},
        {"sharing, application, network and media routes",targetTypesAndFeatureMatrix},
        {"native fixture capabilities, STA lifetime and headless activation guard",nativeFixtureCapabilitiesAndHeadlessGuard},
        {"Windows command resources and read-only drive enumeration",metadataAndReadOnlyDriveEnumeration},
        {"all eight native localized View gallery titles and icon resources",nativeViewGalleryResources},
        {"actual Recycle Bin Properties native background-menu state and invocation guards",[]{onPrivateNamespaceDesktop(nativeRecyclePropertiesBackgroundState);}},
        {"actual standard-GIT lookup cancellation and initialized final release",[]{onPrivateNamespaceDesktop(realGitLookupCancellationLifetime);}},
        {"exact target registration reuse, standalone ownership and native marshal reentry",[]{onPrivateNamespaceDesktop(exactTargetRegistrationReuseAndReentry);}},
        {"full 100001-item aggregate attributes, provider site and activation guards",[]{onPrivateNamespaceDesktop(aggregateLargeSelectionAndProviderGuards);}},
        {"real native 100001-item arrays with final file, link and virtual counterexamples",[]{onPrivateNamespaceDesktop(nativeLargeArrayTailCounterexamples);}},
        {"large same-parent selection facts, boundaries, other parents and removed items",[]{onPrivateNamespaceDesktop(largeSelectionParentSnapshotFacts);}},
        {"actual snapshot 63/64 threshold, Unicode, case/8.3 aliases, failure and shortcut fallback",[]{onPrivateNamespaceDesktop(snapshotNativeBoundariesAndAliases);}},
        {"actual extended, long and reserved superscript device-name snapshot fallback",[]{onPrivateNamespaceDesktop(snapshotExtendedReservedAndLongPaths);}},
        {"actual owned FSCTL junction and instantaneous UI snapshot mutation",[]{onPrivateNamespaceDesktop(snapshotOwnedJunctionAndMutation);}}
    };
    unsigned failures = 0;
    for (const auto& [name,test] : tests) {
        try { test(); std::cout << "PASS: Namespace actions: " << name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL: Namespace actions: " << name << ": " << error.what() << '\n'; }
        catch (...) { ++failures; std::cerr << "FAIL: Namespace actions: " << name << ": unknown exception\n"; }
    }
    try{succeeded(explorer::drainStaWorkers(10000),"Drain namespace suite workers while creator COM remains initialized");}
    catch(const std::exception& error){++failures;std::cerr<<"FAIL: Namespace actions: final STA drain: "<<error.what()<<'\n';}
    std::cout << tests.size() - failures << '/' << tests.size() << " namespace action groups passed; " << assertions << " assertions\n";
    return static_cast<int>(failures);
}
