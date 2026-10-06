#include "explorer/namespace_actions.hpp"
#include "explorer/headless_visual.hpp"
#include "explorer/worker_sta.hpp"
#include "../src/parent_attribute_snapshot.hpp"

#include <shlobj.h>
#include <objbase.h>
#include <propkey.h>
#include <propvarutil.h>
#include <winioctl.h>
#include <vfw.h>
#include <wrl/implements.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
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

bool pumpPrivateNamespaceUntil(const std::function<bool()>& complete,DWORD milliseconds,ULONGLONG absoluteDeadline=0) {
    struct Event {HANDLE value=CreateEventW(nullptr,TRUE,FALSE,nullptr);~Event(){if(value)CloseHandle(value);}}event;
    require(event.value!=nullptr,"Create owned namespace COM-dispatch event");
    const auto desktop=explorer::PrivateDesktop::current();
    require(desktop!=nullptr,"Native background fixture has no private desktop");
    const auto deadline=absoluteDeadline?(std::min)(GetTickCount64()+milliseconds,absoluteDeadline):GetTickCount64()+milliseconds;
    do {
        if(absoluteDeadline&&GetTickCount64()>=deadline)return false;
        succeeded(desktop->verifyIsolation(),"Keep native background query on its private desktop");
        if(absoluteDeadline&&GetTickCount64()>=deadline)return false;
        if(complete())return !absoluteDeadline||GetTickCount64()<deadline;
        MSG message{};unsigned dispatched=0;
        while(dispatched++<32&&PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            if(message.message==WM_QUIT){PostQuitMessage(static_cast<int>(message.wParam));return false;}
            TranslateMessage(&message);DispatchMessageW(&message);
        }
        DWORD index=0;
        const auto waited=CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS|COWAIT_DISPATCH_WINDOW_MESSAGES,5,1,&event.value,&index);
        if(waited!=RPC_S_CALLPENDING)succeeded(waited,"Dispatch native background provider's marshaled view calls");
    }while(GetTickCount64()<deadline);
    return !absoluteDeadline&&complete();
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
    struct PidlDelete {
        using pointer=LPITEMIDLIST;
        void operator()(pointer value) const noexcept {CoTaskMemFree(value);}
    };
    using Pidl=std::unique_ptr<ITEMIDLIST,PidlDelete>;
    struct OwnedMember {fs::path path;FILE_ID_INFO identity{};Pidl pidl;};
    HWND owner=nullptr;
    DWORD cookie=0;
    ComPtr<IExplorerBrowser> browser;
    ComPtr<IShellView> view;
    ComPtr<BackgroundNavigationEvents> events;
    ComPtr<IShellItem> expectedFolder;
    std::vector<OwnedMember> expectedMembers;
    Pidl expectedFolderId,targetId;
    static HRESULT readIdentity(const fs::path& path,FILE_ID_INFO& identity) {
        struct File {HANDLE value=INVALID_HANDLE_VALUE;~File(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}}file;
        file.value=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                               nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(file.value==INVALID_HANDLE_VALUE)return HRESULT_FROM_WIN32(GetLastError());
        if(!GetFileInformationByHandleEx(file.value,FileIdInfo,&identity,sizeof(identity)))return HRESULT_FROM_WIN32(GetLastError());
        return S_OK;
    }
    static bool sameIdentity(const FILE_ID_INFO& left,const FILE_ID_INFO& right) {
        return left.VolumeSerialNumber==right.VolumeSerialNumber&&
            std::memcmp(left.FileId.Identifier,right.FileId.Identifier,sizeof(left.FileId.Identifier))==0;
    }
    static void printIdentity(const char* phase,const char* origin,size_t index,HRESULT status,const FILE_ID_INFO& identity) {
        std::array<char,33> encoded{};constexpr char digits[]="0123456789abcdef";
        for(size_t byte=0;byte<16;++byte){encoded[2*byte]=digits[identity.FileId.Identifier[byte]>>4];encoded[2*byte+1]=digits[identity.FileId.Identifier[byte]&15];}
        std::cout<<"NativeBackground identity phase="<<phase<<" origin="<<origin<<" member="<<index<<" HRESULT="<<static_cast<unsigned long>(status)
            <<" volume="<<identity.VolumeSerialNumber<<" FileID="<<encoded.data()<<'\n';
    }
    struct PathIdentityReadback {
        HRESULT originalRead=E_PENDING,reportedRead=E_PENDING,finalOriginalRead=E_PENDING;
        FILE_ID_INFO original{},reported{},finalOriginal{};
        bool accepted=false;
    };
    // The caller first proves the actual native child/full-PIDL and retained
    // private view source. Spelling, case and 8.3 aliases are not file identity.
    static PathIdentityReadback readOwnedReportedPathIdentity(const fs::path& originalPath,
            const fs::path& reportedPath,const FILE_ID_INFO& immutableIdentity,
            const std::function<bool()>& stillCurrent={}) {
        PathIdentityReadback result;
        if(stillCurrent&&!stillCurrent()){result.originalRead=E_ABORT;return result;}
        result.originalRead=readIdentity(originalPath,result.original);
        if(result.originalRead!=S_OK||!sameIdentity(result.original,immutableIdentity))return result;
        if(stillCurrent&&!stillCurrent()){result.reportedRead=E_ABORT;return result;}
        result.reportedRead=readIdentity(reportedPath,result.reported);
        if(result.reportedRead!=S_OK||!sameIdentity(result.reported,immutableIdentity))return result;
        if(stillCurrent&&!stillCurrent()){result.finalOriginalRead=E_ABORT;return result;}
        result.finalOriginalRead=readIdentity(originalPath,result.finalOriginal);
        result.accepted=result.finalOriginalRead==S_OK&&sameIdentity(result.finalOriginal,immutableIdentity);
        return result;
    }
    bool sourceCurrent(IFolderView2* folderView,const char* phase=nullptr) {
        const auto desktop=explorer::PrivateDesktop::current();
        if(!desktop||FAILED(desktop->verifyIsolation())||!owner||!IsWindow(owner)||IsWindowVisible(owner))return false;
        ComPtr<IShellItem> folder;const auto folderRead=folderView->GetFolder(IID_PPV_ARGS(&folder));
        int order=1;const auto compare=folder?folder->Compare(expectedFolder.Get(),SICHINT_CANONICAL,&order):E_NOINTERFACE;
        PIDLIST_ABSOLUTE raw=nullptr;const auto pidlRead=folder?SHGetIDListFromObject(folder.Get(),&raw):E_NOINTERFACE;Pidl folderId(raw);
        HWND child=nullptr;const auto windowRead=view->GetWindow(&child);DWORD process=0;
        const auto thread=child?GetWindowThreadProcessId(child,&process):0;
        DWORD ownerProcess=0;const auto ownerThread=GetWindowThreadProcessId(owner,&ownerProcess);
        const bool exactFolder=SUCCEEDED(pidlRead)&&folderId&&ILIsEqual(expectedFolderId.get(),folderId.get());
        ComPtr<IShellView> current;const auto currentRead=browser->GetCurrentView(IID_PPV_ARGS(&current));
        const bool exact=currentRead==S_OK&&current.Get()==view.Get()&&folderRead==S_OK&&compare==S_OK&&order==0&&exactFolder&&
            windowRead==S_OK&&child&&IsChild(owner,child)&&thread==GetCurrentThreadId()&&process==GetCurrentProcessId()&&
            ownerThread==GetCurrentThreadId()&&ownerProcess==GetCurrentProcessId()&&
            !IsWindowVisible(owner)&&SUCCEEDED(desktop->verifyIsolation());
        if(phase)std::cout<<"NativeBackground fence phase="<<phase<<" currentViewHRESULT="<<static_cast<unsigned long>(currentRead)
            <<" sameView="<<(current.Get()==view.Get())<<" folderHRESULT="<<static_cast<unsigned long>(folderRead)
            <<" folderCompareHRESULT="<<static_cast<unsigned long>(compare)<<" folderOrder="<<order
            <<" folderPidlHRESULT="<<static_cast<unsigned long>(pidlRead)<<" exactFolderPIDL="<<exactFolder
            <<" windowHRESULT="<<static_cast<unsigned long>(windowRead)<<" viewHWND="<<reinterpret_cast<uintptr_t>(child)
            <<" viewPID="<<process<<" viewTID="<<thread<<" ownerHWND="<<reinterpret_cast<uintptr_t>(owner)
            <<" ownerPID="<<ownerProcess<<" ownerTID="<<ownerThread<<" exactPrivateOwnerView="<<exact<<'\n'<<std::flush;
        return exact;
    }
    void selectionDiagnostic(const char* phase) {
        ComPtr<IFolderView2> folderView;succeeded(view.As(&folderView),"Read exact initialized native selection view");
        require(sourceCurrent(folderView.Get(),phase),"Native selection diagnostic lost its original private folder/view");
        int all=-1,count=-1;const auto allRead=folderView->ItemCount(SVGIO_ALLVIEW,&all);
        const auto countRead=folderView->ItemCount(SVGIO_SELECTION,&count);
        ComPtr<IShellItemArray> selected;const auto selectionRead=folderView->GetSelection(FALSE,&selected);
        DWORD selectedCount=MAXDWORD;const auto arrayRead=selected?selected->GetCount(&selectedCount):E_NOINTERFACE;
        std::cout<<"NativeBackground selection phase="<<phase<<" allHRESULT="<<static_cast<unsigned long>(allRead)<<" all="<<all
            <<" selectedHRESULT="<<static_cast<unsigned long>(countRead)<<" selected="<<count
            <<" arrayHRESULT="<<static_cast<unsigned long>(selectionRead)<<" arrayCountHRESULT="<<static_cast<unsigned long>(arrayRead)
            <<" arrayCount="<<selectedCount<<" currentPrivateFolderView=1\n";
        if(arrayRead==S_OK&&selectedCount<=expectedMembers.size())for(DWORD index=0;index<selectedCount;++index) {
            ComPtr<IShellItem> selectedItem;auto status=selected->GetItemAt(index,&selectedItem);PWSTR raw=nullptr;
            if(status==S_OK&&selectedItem)status=selectedItem->GetDisplayName(SIGDN_FILESYSPATH,&raw);
            else if(SUCCEEDED(status))status=E_UNEXPECTED;
            std::unique_ptr<wchar_t,decltype(&CoTaskMemFree)> path(raw,CoTaskMemFree);FILE_ID_INFO identity{};
            if(status==S_OK&&path) {
                const auto found=std::find_if(expectedMembers.begin(),expectedMembers.end(),[&](const auto& member){return member.path==fs::path(path.get());});
                status=found!=expectedMembers.end()?readIdentity(found->path,identity):E_ACCESSDENIED;
            }else if(SUCCEEDED(status))status=E_UNEXPECTED;
            printIdentity(phase,"native-selected",index,status,identity);
        }
        for(size_t index=0;index<expectedMembers.size();++index) {
            FILE_ID_INFO identity{};const auto status=readIdentity(expectedMembers[index].path,identity);printIdentity(phase,"owned-source",index,status,identity);
            require(status==S_OK&&sameIdentity(identity,expectedMembers[index].identity),"Native selection changed an original owned source FileID");
        }
        require(sourceCurrent(folderView.Get(),phase),"Native selection readbacks changed the original private folder/view");
        std::cout<<std::flush;
    }
    int ownedNativeIndex(PCIDLIST_ABSOLUTE target,ULONGLONG deadline,const char* phase) {
        ComPtr<IFolderView2> folderView;succeeded(view.As(&folderView),"Retain original native indexed selection view");
        const auto current=[&]{return GetTickCount64()<deadline&&sourceCurrent(folderView.Get())&&GetTickCount64()<deadline;};
        require(current()&&target&&!expectedMembers.empty(),"Native indexed selection lost its exact owned source/deadline");
        ComPtr<IShellFolder> desktopFolder;const auto desktopRead=SHGetDesktopFolder(&desktopFolder);
        require(desktopRead==S_OK&&desktopFolder&&current(),"Retain native Desktop full PIDL authority for indexed selection");
        auto targetMember=expectedMembers.end();
        for(auto member=expectedMembers.begin();member!=expectedMembers.end();++member) {
            require(current(),"Indexed target canonical lookup exceeded original deadline");
            const auto canonical=desktopFolder->CompareIDs(SHCIDS_CANONICALONLY,target,member->pidl.get());
            require(SUCCEEDED(canonical)&&current(),"Indexed target full canonical lookup failed or changed source");
            if(canonical==S_OK&&static_cast<short>(HRESULT_CODE(canonical))==0) {
                require(targetMember==expectedMembers.end(),"Indexed target matches multiple original canonical members");targetMember=member;
            }
        }
        require(targetMember!=expectedMembers.end(),"Indexed selection target is not an original owned canonical member");
        ComPtr<IShellFolder> nativeFolder;succeeded(folderView->GetFolder(IID_PPV_ARGS(&nativeFolder)),"Retain exact native indexed selection folder");
        require(nativeFolder&&current(),"Indexed selection folder read changed the original source");
        int count=-1;exactNativeCount(folderView.Get(),count,current);
        std::vector<bool> matched(expectedMembers.size());int targetIndex=-1;
        Pidl observedTarget;
        for(int index=0;index<count;++index) {
            require(current(),"Indexed native membership exceeded original selection deadline");
            PITEMID_CHILD rawChild=nullptr;const auto childRead=folderView->Item(index,&rawChild);Pidl child(rawChild);
            require(childRead==S_OK&&child&&current(),"Indexed native row child is absent or stale");
            const auto found=std::find_if(expectedMembers.begin(),expectedMembers.end(),[&](const auto& member){return ILIsEqual(child.get(),ILFindLastID(member.pidl.get()));});
            require(found!=expectedMembers.end(),"Indexed native row is not an original owned member");
            const auto memberIndex=static_cast<size_t>(found-expectedMembers.begin());
            require(!matched[memberIndex],"Indexed native membership contains a duplicate original member");
            const auto canonical=nativeFolder->CompareIDs(SHCIDS_CANONICALONLY,child.get(),ILFindLastID(found->pidl.get()));
            require(canonical==S_OK&&static_cast<short>(HRESULT_CODE(canonical))==0&&current(),"Indexed native child canonical identity differs");
            ComPtr<IShellItem> row;const auto rowRead=folderView->GetItem(index,IID_PPV_ARGS(&row));
            require(rowRead==S_OK&&row&&current(),"Indexed native item read is absent or stale");
            PIDLIST_ABSOLUTE rawFull=nullptr;const auto fullRead=SHGetIDListFromObject(row.Get(),&rawFull);Pidl full(rawFull);
            require(fullRead==S_OK&&full&&current(),"Indexed native full item identity read is absent or stale");
            const auto fullCanonical=desktopFolder->CompareIDs(SHCIDS_CANONICALONLY,full.get(),found->pidl.get());
            require(fullCanonical==S_OK&&static_cast<short>(HRESULT_CODE(fullCanonical))==0&&current(),"Indexed native full canonical identity differs");
            PWSTR rawPath=nullptr;const auto pathRead=row->GetDisplayName(SIGDN_FILESYSPATH,&rawPath);
            std::unique_ptr<wchar_t,decltype(&CoTaskMemFree)> path(rawPath,CoTaskMemFree);
            require(pathRead==S_OK&&path&&current(),"Indexed native owned row path read is absent or stale");
            const auto identity=readOwnedReportedPathIdentity(found->path,fs::path(path.get()),found->identity,current);
            require(identity.accepted&&current(),"Indexed native row lost its immutable original file identity");
            matched[memberIndex]=true;
            if(found==targetMember){require(targetIndex==-1,"Indexed native target is not unique");targetIndex=index;observedTarget=std::move(child);}
        }
        require(targetIndex>=0&&std::all_of(matched.begin(),matched.end(),[](bool value){return value;}),"Indexed native selection lacks the complete original membership");
        int finalCount=-1;exactNativeCount(folderView.Get(),finalCount,current);
        require(finalCount==count,"Indexed native membership changed during exact target lookup");
        PITEMID_CHILD rawAgain=nullptr;const auto againRead=folderView->Item(targetIndex,&rawAgain);Pidl again(rawAgain);
        require(againRead==S_OK&&again&&current(),"Indexed native target read is absent or stale before selection");
        const auto finalCanonical=nativeFolder->CompareIDs(SHCIDS_CANONICALONLY,again.get(),observedTarget.get());
        require(finalCanonical==S_OK&&static_cast<short>(HRESULT_CODE(finalCanonical))==0&&current(),"Indexed native target moved before its single selection request");
        std::cout<<"NativeBackground indexedTarget phase="<<phase<<" index="<<targetIndex<<" count="<<count
            <<" canonicalHRESULT=0 ownedFullFileID=1 selectionRequests=0 deadlineReached=0\n"<<std::flush;
        require(current(),"Indexed native target source/deadline expired after its diagnostic readback");
        return targetIndex;
    }
    void exactNativeCount(IFolderView2* folderView,int& count,const std::function<bool()>& current) {
        require(current(),"Indexed native count source is stale");
        const auto status=folderView->ItemCount(SVGIO_ALLVIEW,&count);
        require(status==S_OK&&count==static_cast<int>(expectedMembers.size())&&current(),"Indexed native view lacks complete owned membership");
    }
    void initialize(IShellItem* folder,std::vector<fs::path> ownedPaths={},IShellItem* target=nullptr) {
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
        if(!ownedPaths.empty()) {
            require(target!=nullptr&&ownedPaths.size()<=64,"Owned native membership readiness requires a bounded exact target");expectedFolder=folder;
            PIDLIST_ABSOLUTE rawFolder=nullptr,rawTarget=nullptr;
            const auto folderRead=SHGetIDListFromObject(folder,&rawFolder);expectedFolderId.reset(rawFolder);
            succeeded(folderRead,"Retain exact expected native folder identity");
            const auto targetRead=SHGetIDListFromObject(target,&rawTarget);targetId.reset(rawTarget);
            succeeded(targetRead,"Retain exact expected native selection identity");
            require(expectedFolderId&&targetId,"Expected native folder/target PIDLs must be nonnull");
            for(auto& path:ownedPaths) {
                OwnedMember member;member.path=std::move(path);
                succeeded(readIdentity(member.path,member.identity),"Capture original owned native readiness FileID");
                auto expected=item(member.path);PIDLIST_ABSOLUTE raw=nullptr;
                const auto rowRead=SHGetIDListFromObject(expected.Get(),&raw);member.pidl.reset(raw);
                succeeded(rowRead,"Capture original owned native row PIDL");
                require(member.pidl!=nullptr,"Expected owned native row PIDL must be nonnull");
                expectedMembers.push_back(std::move(member));
            }
        }
        succeeded(browser->BrowseToObject(folder,SBSP_ABSOLUTE),"Browse native background namespace without invoking a command");
        const auto deadline=GetTickCount64()+5000;
        require(pumpPrivateNamespaceUntil([&]{return events->complete;},5000),"Native background navigation exceeded bounded wait");
        succeeded(events->status,"Read actual native background navigation result");
        succeeded(browser->GetCurrentView(IID_PPV_ARGS(&view)),"Read actual native background view site");
        require(!IsWindowVisible(owner),"Native background browser displayed its owner");
        if(!expectedMembers.empty()) {
            ComPtr<IFolderView2> folderView;succeeded(view.As(&folderView),"Read original native membership readiness view");
            unsigned observations=0;int lastCount=-2;HRESULT lastStatus=E_PENDING;
            std::array<bool,10> rejectionLogged{};
            bool aliasAdmissionLogged=false;
            const auto reject=[&](unsigned reason,const char* name,int index,HRESULT status,bool present,PCUIDLIST_RELATIVE child=nullptr) noexcept {
                if(reason>=rejectionLogged.size()||rejectionLogged[reason])return;
                rejectionLogged[reason]=true;
                try {
                    std::cout<<"NativeBackground readinessReject reason="<<name<<" observation="<<observations<<" row="<<index
                        <<" HRESULT="<<static_cast<unsigned long>(status)<<" outputPresent="<<present
                        <<" childBytes="<<(child?ILGetSize(child):0)<<" elapsedMs="<<(GetTickCount64()-(deadline-5000))
                        <<" deadlineReached="<<(GetTickCount64()>=deadline)<<'\n'<<std::flush;
                    if(reason!=3||!child||GetTickCount64()>=deadline||!sourceCurrent(folderView.Get()))return;
                    // Extra reads are diagnostics only after the original byte
                    // equality predicate has already rejected this actual row.
                    ComPtr<IShellItem> row;const auto rowRead=folderView->GetItem(index,IID_PPV_ARGS(&row));
                    PWSTR rawPath=nullptr;const auto pathRead=rowRead==S_OK&&row?row->GetDisplayName(SIGDN_FILESYSPATH,&rawPath):E_NOINTERFACE;
                    std::unique_ptr<wchar_t,decltype(&CoTaskMemFree)> path(rawPath,CoTaskMemFree);
                    const auto owned=pathRead==S_OK&&path?std::find_if(expectedMembers.begin(),expectedMembers.end(),
                        [&](const auto& member){return member.path==fs::path(path.get());}):expectedMembers.end();
                    FILE_ID_INFO identity{};
                    const auto identityRead=owned!=expectedMembers.end()?readIdentity(owned->path,identity):E_ACCESSDENIED;
                    const bool ownedIdentity=identityRead==S_OK&&owned!=expectedMembers.end()&&sameIdentity(identity,owned->identity);
                    const bool currentBefore=sourceCurrent(folderView.Get());
                    std::cout<<"NativeBackground readinessReject rowGetItemHRESULT="<<static_cast<unsigned long>(rowRead)
                        <<" rowPresent="<<(row!=nullptr)<<" displayPathHRESULT="<<static_cast<unsigned long>(pathRead)
                        <<" displayPathPresent="<<(path!=nullptr)<<" ownedPathIndex="<<(owned==expectedMembers.end()?-1:static_cast<int>(owned-expectedMembers.begin()))
                        <<" identityHRESULT="<<static_cast<unsigned long>(identityRead)<<" ownedFileIDMatches="<<ownedIdentity
                        <<" sourceCurrent="<<currentBefore<<'\n';
                    printIdentity("readiness-byte-rejection","actual-row-owned-path",static_cast<size_t>(index),identityRead,identity);
                    if(!ownedIdentity||!currentBefore||GetTickCount64()>=deadline){std::cout<<std::flush;return;}
                    ComPtr<IShellFolder> nativeFolder;const auto folderRead=folderView->GetFolder(IID_PPV_ARGS(&nativeFolder));
                    std::cout<<"NativeBackground readinessReject nativeFolderHRESULT="<<static_cast<unsigned long>(folderRead)
                        <<" nativeFolderPresent="<<(nativeFolder!=nullptr)<<'\n';
                    for(size_t member=0;folderRead==S_OK&&nativeFolder&&member<expectedMembers.size()&&GetTickCount64()<deadline;++member) {
                        if(!sourceCurrent(folderView.Get()))break;
                        const auto expectedChild=ILFindLastID(expectedMembers[member].pidl.get());
                        const bool byteEqual=ILIsEqual(child,expectedChild);
                        const auto canonical=nativeFolder->CompareIDs(SHCIDS_CANONICALONLY,child,expectedChild);
                        const int order=SUCCEEDED(canonical)?static_cast<short>(HRESULT_CODE(canonical)):0;
                        const bool currentAfter=sourceCurrent(folderView.Get());
                        std::cout<<"NativeBackground readinessReject canonicalDiagnosticOnly=1 row="<<index<<" expectedMember="<<member
                            <<" rawPIDLByteEqual="<<byteEqual<<" nativeCompareHRESULT="<<static_cast<unsigned long>(canonical)
                            <<" nativeCompareSucceeded="<<SUCCEEDED(canonical)<<" nativeCanonicalOrder="<<order
                            <<" expectedChildBytes="<<ILGetSize(expectedChild)<<" expectedTargetByteEqual="<<ILIsEqual(expectedChild,ILFindLastID(targetId.get()))
                            <<" sourceCurrentAfter="<<currentAfter<<'\n';
                        if(!currentAfter)break;
                    }
                    std::cout<<std::flush;
                } catch(...) {std::cout<<"NativeBackground readinessReject diagnosticException=1\n"<<std::flush;}
            };
            const auto ready=[&] {
                if(GetTickCount64()>=deadline){reject(0,"deadline",-1,E_PENDING,false);return false;}
                require(sourceCurrent(folderView.Get()),"Owned native membership lost its private source/view");
                int count=-1;const auto status=folderView->ItemCount(SVGIO_ALLVIEW,&count);++observations;
                if(status!=lastStatus||count!=lastCount) {
                    std::cout<<"NativeBackground readiness allHRESULT="<<static_cast<unsigned long>(status)<<" actual="<<count
                        <<" expected="<<expectedMembers.size()<<" elapsedMs="<<(GetTickCount64()-(deadline-5000))<<'\n'<<std::flush;
                    lastStatus=status;lastCount=count;
                }
                if(status!=S_OK||count!=static_cast<int>(expectedMembers.size())){reject(1,"all-view-count",-1,status,false);return false;}
                std::vector<bool> matched(expectedMembers.size());bool targetAvailable=false;
                for(int index=0;index<count;++index) {
                    PITEMID_CHILD rawChild=nullptr;const auto childRead=folderView->Item(index,&rawChild);Pidl child(rawChild);
                    if(childRead!=S_OK||!child){reject(2,"native-child-read",index,childRead,child!=nullptr,child.get());return false;}
                    const auto found=std::find_if(expectedMembers.begin(),expectedMembers.end(),[&](const auto& member){return ILIsEqual(child.get(),ILFindLastID(member.pidl.get()));});
                    if(found==expectedMembers.end()){reject(3,"native-child-byte-identity",index,childRead,true,child.get());return false;}
                    const auto memberIndex=static_cast<size_t>(found-expectedMembers.begin());if(matched[memberIndex]){reject(4,"duplicate-owned-member",index,S_OK,true);return false;}
                    ComPtr<IShellItem> row;const auto rowRead=folderView->GetItem(index,IID_PPV_ARGS(&row));
                    if(rowRead!=S_OK||!row){reject(5,"native-row-read",index,rowRead,row!=nullptr);return false;}
                    PIDLIST_ABSOLUTE rawRowIdentity=nullptr;
                    const auto rowIdentityRead=SHGetIDListFromObject(row.Get(),&rawRowIdentity);Pidl rowIdentity(rawRowIdentity);
                    if(rowIdentityRead!=S_OK||!rowIdentity||!ILIsEqual(rowIdentity.get(),found->pidl.get())) {
                        reject(5,"native-row-full-pidl",index,rowIdentityRead,rowIdentity!=nullptr);return false;
                    }
                    PWSTR rawPath=nullptr;const auto pathRead=row->GetDisplayName(SIGDN_FILESYSPATH,&rawPath);
                    std::unique_ptr<wchar_t,decltype(&CoTaskMemFree)> path(rawPath,CoTaskMemFree);
                    if(pathRead!=S_OK||!path){reject(6,"native-display-path",index,pathRead,path!=nullptr);return false;}
                    // Exact complete child membership/full row PIDL plus a
                    // fresh immutable original FileID authorizes metadata-only
                    // reading of this real native row's reported path.
                    const auto stillCurrent=[&]{return GetTickCount64()<deadline&&sourceCurrent(folderView.Get())&&GetTickCount64()<deadline;};
                    const bool identitySourceBefore=stillCurrent();
                    if(!identitySourceBefore){reject(9,"source-before-path-identity",index,E_ABORT,true);return false;}
                    const auto identity=readOwnedReportedPathIdentity(found->path,fs::path(path.get()),found->identity,stillCurrent);
                    const bool identitySourceAfter=stillCurrent();
                    if(!identity.accepted||!identitySourceAfter) {
                        const auto identityStatus=identity.originalRead!=S_OK?identity.originalRead:
                            identity.reportedRead!=S_OK?identity.reportedRead:
                            identity.finalOriginalRead!=S_OK?identity.finalOriginalRead:HRESULT_FROM_WIN32(ERROR_RETRY);
                        reject(7,"owned-reported-path-identity",index,identityStatus,true);
                        if(fs::path(path.get())!=found->path) {
                            const bool first=!rejectionLogged[6];
                            reject(6,"native-display-path",index,pathRead,path!=nullptr);
                            // Preserve the original one-shot failure receipt.
                            // It never admits a failed immutable-ID/source check.
                            if(first&&pathRead==S_OK&&path)try {
                                const auto expectedText=found->path.native();
                                const std::wstring nativeText(path.get());
                                const auto sensitive=CompareStringOrdinal(expectedText.c_str(),-1,nativeText.c_str(),-1,FALSE);
                                const auto insensitive=CompareStringOrdinal(expectedText.c_str(),-1,nativeText.c_str(),-1,TRUE);
                                const auto prefix=[](const std::wstring& text,const wchar_t* start) {
                                    const size_t length=wcslen(start);
                                    return text.size()>=length&&CompareStringOrdinal(text.data(),static_cast<int>(length),start,
                                        static_cast<int>(length),TRUE)==CSTR_EQUAL;
                                };
                                const bool sourceBefore=sourceCurrent(folderView.Get(),"display-path-rejection-before");
                                FILE_ID_INFO originalIdentity{},nativeIdentity{};
                                const auto originalRead=sourceBefore&&GetTickCount64()<deadline?readIdentity(found->path,originalIdentity):E_ABORT;
                                const bool originalMatches=originalRead==S_OK&&sameIdentity(originalIdentity,found->identity);
                                const bool byteMatches=ILIsEqual(child.get(),ILFindLastID(found->pidl.get()));
                                // Exact byte membership, the original source ID and
                                // the retained private folder/view authorize only
                                // this real native row's reported path read.
                                const bool nativeAuthorized=originalMatches&&byteMatches&&sourceCurrent(folderView.Get())&&GetTickCount64()<deadline;
                                const auto nativeRead=nativeAuthorized?readIdentity(fs::path(nativeText),nativeIdentity):E_ACCESSDENIED;
                                std::wcout<<L"NativeBackground displayPathDiagnostic expectedPath=["<<expectedText
                                    <<L"] nativePath=["<<nativeText<<L"]\n";
                                const auto units=[](const char* label,const std::wstring& text) {
                                    constexpr char hex[]="0123456789abcdef";
                                    std::cout<<"NativeBackground displayPathDiagnostic "<<label<<"Length="<<text.size()<<" utf16=";
                                    for(const auto character:text) {
                                        const auto value=static_cast<unsigned short>(character);
                                        const std::array<char,4> encoded{hex[(value>>12)&15],hex[(value>>8)&15],hex[(value>>4)&15],hex[value&15]};
                                        std::cout.write(encoded.data(),static_cast<std::streamsize>(encoded.size()));
                                    }
                                    std::cout<<'\n';
                                };
                                units("expectedPath",expectedText);units("nativePath",nativeText);
                                std::cout<<"NativeBackground displayPathDiagnostic diagnosticOnly=1 row="<<index
                                    <<" expectedMember="<<memberIndex<<" ordinalSensitive="<<sensitive<<" ordinalInsensitive="<<insensitive
                                    <<" expectedExtendedPrefix="<<prefix(expectedText,L"\\\\?\\")<<" nativeExtendedPrefix="<<prefix(nativeText,L"\\\\?\\")
                                    <<" expectedUNCPrefix="<<prefix(expectedText,L"\\\\")<<" nativeUNCPrefix="<<prefix(nativeText,L"\\\\")
                                    <<" exactChildByteMatch="<<byteMatches<<" sourceBefore="<<sourceBefore
                                    <<" originalIdentityHRESULT="<<static_cast<unsigned long>(originalRead)<<" originalImmutableFileIDMatches="<<originalMatches
                                    <<" nativePathReadAuthorized="<<nativeAuthorized<<" nativeIdentityHRESULT="<<static_cast<unsigned long>(nativeRead)
                                    <<" nativeImmutableFileIDMatches="<<(nativeRead==S_OK&&sameIdentity(nativeIdentity,found->identity))<<'\n';
                                printIdentity("display-path-rejection","original-owned-path",memberIndex,originalRead,originalIdentity);
                                printIdentity("display-path-rejection","native-reported-owned-row-path",memberIndex,nativeRead,nativeIdentity);
                                PIDLIST_ABSOLUTE rawRow=nullptr;
                                const auto rowPidlRead=nativeAuthorized&&sourceCurrent(folderView.Get())&&GetTickCount64()<deadline?
                                    SHGetIDListFromObject(row.Get(),&rawRow):E_ABORT;
                                Pidl rowPidl(rawRow);
                                ComPtr<IShellFolder> nativeFolder;
                                const auto nativeFolderRead=sourceCurrent(folderView.Get())&&GetTickCount64()<deadline?
                                    folderView->GetFolder(IID_PPV_ARGS(&nativeFolder)):E_ABORT;
                                const auto canonical=sourceCurrent(folderView.Get())&&nativeFolderRead==S_OK&&nativeFolder&&GetTickCount64()<deadline?
                                    nativeFolder->CompareIDs(SHCIDS_CANONICALONLY,child.get(),ILFindLastID(found->pidl.get())):E_ABORT;
                                const bool sourceAfter=sourceCurrent(folderView.Get(),"display-path-rejection-after");
                                std::cout<<"NativeBackground displayPathDiagnostic rowFullPIDLHRESULT="<<static_cast<unsigned long>(rowPidlRead)
                                    <<" rowFullPIDLPresent="<<(rowPidl!=nullptr)<<" rowFullPIDLBytes="<<(rowPidl?ILGetSize(rowPidl.get()):0)
                                    <<" expectedFullPIDLBytes="<<ILGetSize(found->pidl.get())
                                    <<" exactFullPIDLByteMatch="<<(rowPidl&&ILIsEqual(rowPidl.get(),found->pidl.get()))
                                    <<" nativeFolderHRESULT="<<static_cast<unsigned long>(nativeFolderRead)
                                    <<" nativeCanonicalHRESULT="<<static_cast<unsigned long>(canonical)
                                    <<" nativeCanonicalSucceeded="<<SUCCEEDED(canonical)
                                    <<" nativeCanonicalOrder="<<(SUCCEEDED(canonical)?static_cast<short>(HRESULT_CODE(canonical)):0)
                                    <<" sourceAfter="<<sourceAfter<<" deadlineReached="<<(GetTickCount64()>=deadline)<<'\n'<<std::flush;
                            }catch(...) {std::cout<<"NativeBackground displayPathDiagnostic diagnosticException=1\n"<<std::flush;}
                        }
                        return false;
                    }
                    if(!aliasAdmissionLogged&&fs::path(path.get())!=found->path) {
                        aliasAdmissionLogged=true;
                        std::cout<<"NativeBackground pathIdentityAdmission spellingDiffers=1 row="<<index<<" expectedMember="<<memberIndex
                            <<" originalPathUnits="<<found->path.native().size()<<" reportedPathUnits="<<wcslen(path.get())
                            <<" exactChildAndFullPIDL=1 original/reported/finalOriginalHRESULT="<<static_cast<unsigned long>(identity.originalRead)
                            <<"/"<<static_cast<unsigned long>(identity.reportedRead)<<"/"<<static_cast<unsigned long>(identity.finalOriginalRead)
                            <<" completeImmutableFileIDs=1 sourceBefore/After="<<identitySourceBefore<<"/"<<identitySourceAfter
                            <<" elapsedMs="<<(GetTickCount64()-(deadline-5000))<<'\n';
                        printIdentity("path-alias-admission","original-owned-path",memberIndex,identity.originalRead,identity.original);
                        printIdentity("path-alias-admission","native-reported-owned-row-path",memberIndex,identity.reportedRead,identity.reported);
                        printIdentity("path-alias-admission","final-original-owned-path",memberIndex,identity.finalOriginalRead,identity.finalOriginal);
                        std::cout<<std::flush;
                    }
                    matched[memberIndex]=true;targetAvailable=targetAvailable||ILIsEqual(child.get(),ILFindLastID(targetId.get()));
                }
                const bool result=targetAvailable&&sourceCurrent(folderView.Get())&&GetTickCount64()<deadline;
                if(!result)reject(targetAvailable?9:8,targetAvailable?"final-source-or-deadline":"target-child-byte-identity",-1,E_PENDING,targetAvailable);
                return result;
            };
            const auto now=GetTickCount64();const auto remaining=now<deadline?static_cast<DWORD>(deadline-now):0;
            const bool membershipReady=remaining&&pumpPrivateNamespaceUntil(ready,remaining);
            if(!membershipReady)selectionDiagnostic("initialization-not-ready");
            require(membershipReady,"Original native owned membership/target did not become ready within initialization budget");
            std::cout<<"NativeBackground readiness completeOwnedFileIDs="<<expectedMembers.size()<<" targetChildAvailable=1 observations="<<observations<<'\n'<<std::flush;
            selectionDiagnostic("before-original-selection");
            require(GetTickCount64()<deadline,"Native owned readiness readbacks exceeded original initialization budget");
        }
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

// This control owns exactly two files and one new directory. Every cleanup
// checks the retained handle and current owned path, then deletes that exact
// handle. It never recursively traverses or deletes an unexpected replacement.
struct OwnedPathIdentityControl {
    struct File {fs::path path;HANDLE handle=INVALID_HANDLE_VALUE;FILE_ID_INFO identity{};bool captured=false;};
    fs::path root;
    HANDLE directory=INVALID_HANDLE_VALUE;
    FILE_ID_INFO directoryIdentity{};
    bool created=false,captured=false;
    std::array<File,2> files;
    static HRESULT exactOwnedHandle(HANDLE handle,const fs::path& path,const FILE_ID_INFO& immutable,bool isDirectory) {
        FILE_ID_INFO held{};FILE_BASIC_INFO basic{};FILE_STANDARD_INFO standard{};
        if(!GetFileInformationByHandleEx(handle,FileIdInfo,&held,sizeof(held))||
           !GetFileInformationByHandleEx(handle,FileBasicInfo,&basic,sizeof(basic))||
           !GetFileInformationByHandleEx(handle,FileStandardInfo,&standard,sizeof(standard)))return HRESULT_FROM_WIN32(GetLastError());
        if(!NativeBackgroundView::sameIdentity(held,immutable)||(basic.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)||
           static_cast<bool>(standard.Directory)!=isDirectory)return E_ACCESSDENIED;
        struct Current {HANDLE value=INVALID_HANDLE_VALUE;~Current(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}}current;
        current.value=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|(isDirectory?FILE_FLAG_BACKUP_SEMANTICS:0),nullptr);
        if(current.value==INVALID_HANDLE_VALUE)return HRESULT_FROM_WIN32(GetLastError());
        FILE_ID_INFO actual{};
        if(!GetFileInformationByHandleEx(current.value,FileIdInfo,&actual,sizeof(actual)))return HRESULT_FROM_WIN32(GetLastError());
        return NativeBackgroundView::sameIdentity(actual,immutable)?S_OK:E_ACCESSDENIED;
    }
    void initialize() {
        GUID guid{};succeeded(CoCreateGuid(&guid),"Generate owned path-control directory identity");
        wchar_t formatted[40]{};require(StringFromGUID2(guid,formatted,40)!=0,"Format owned path-control identity");
        root=fs::temp_directory_path()/(std::wstring(L"WindowsExplorer-PathIdentity-")+formatted);
        require(root.is_absolute()&&CreateDirectoryW(root.c_str(),nullptr)!=FALSE,"Create only the new owned path-control directory");created=true;
        // Retain the immutable parent identity without DELETE access that
        // conflicts with the native rename API's target-directory open.
        directory=CreateFileW(root.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        require(directory!=INVALID_HANDLE_VALUE,"Retain exclusively owned path-control directory");
        require(GetFileInformationByHandleEx(directory,FileIdInfo,&directoryIdentity,sizeof(directoryIdentity))!=FALSE,
            "Capture immutable owned path-control directory FileID");captured=true;
        files[0].path=root/L"native reported document with long name.txt";
        files[1].path=root/L"different owned file.txt";
        for(auto& file:files) {
            file.handle=CreateFileW(file.path.c_str(),FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
            require(file.handle!=INVALID_HANDLE_VALUE,"Create only the new owned path-control file");
            require(GetFileInformationByHandleEx(file.handle,FileIdInfo,&file.identity,sizeof(file.identity))!=FALSE,
                "Capture immutable owned path-control file FileID");file.captured=true;
            require(exactOwnedHandle(file.handle,file.path,file.identity,false)==S_OK,"Verify newly owned ordinary path-control file");
        }
        require(exactOwnedHandle(directory,root,directoryIdentity,true)==S_OK,"Verify newly owned ordinary path-control directory");
    }
    void renameOwned(File& file,const fs::path& target) {
        require(file.captured&&file.path.parent_path()==root&&target.parent_path()==root&&
            exactOwnedHandle(directory,root,directoryIdentity,true)==S_OK&&
            exactOwnedHandle(file.handle,file.path,file.identity,false)==S_OK,"Owned rename source/parent identity");
        const auto mutation=CreateFileW(file.path.c_str(),FILE_READ_ATTRIBUTES|DELETE,
            FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        require(mutation!=INVALID_HANDLE_VALUE,"Open only exact owned rename handle");
        struct Close {HANDLE value;~Close(){CloseHandle(value);}} close{mutation};
        require(exactOwnedHandle(mutation,file.path,file.identity,false)==S_OK,"Owned rename handle FileID");
        const auto name=target.native();
        require(name.size()<32768,"Owned rename target length");
        std::vector<BYTE> storage(sizeof(FILE_RENAME_INFO)+name.size()*sizeof(wchar_t),0);
        auto* rename=reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
        rename->ReplaceIfExists=FALSE;rename->RootDirectory=nullptr;
        rename->FileNameLength=static_cast<DWORD>(name.size()*sizeof(wchar_t));
        std::memcpy(rename->FileName,name.c_str(),name.size()*sizeof(wchar_t));
        const auto renamed=SetFileInformationByHandle(mutation,FileRenameInfo,rename,static_cast<DWORD>(storage.size()));
        const auto error=renamed?ERROR_SUCCESS:GetLastError();
        if(!renamed)std::cout<<"NativeBackground pathIdentityControl renameWin32="<<error<<'\n'<<std::flush;
        require(renamed!=FALSE,"Rename only exact owned native file handle without replacement");
        file.path=target;
        require(exactOwnedHandle(directory,root,directoryIdentity,true)==S_OK,"Owned rename final parent FileID");
        require(exactOwnedHandle(file.handle,file.path,file.identity,false)==S_OK,"Owned rename final retained FileID");
    }
    ~OwnedPathIdentityControl() noexcept {
        if(!created)return;
        if(!captured||directory==INVALID_HANDLE_VALUE)namespaceFatal("owned path-control cleanup has no captured directory identity",E_ACCESSDENIED);
        const auto parent=exactOwnedHandle(directory,root,directoryIdentity,true);
        if(parent!=S_OK)namespaceFatal("owned path-control cleanup directory identity",parent);
        for(auto& file:files)if(file.handle!=INVALID_HANDLE_VALUE) {
            if(!file.captured||file.path.parent_path()!=root)namespaceFatal("owned path-control cleanup child ownership",E_ACCESSDENIED);
            const auto status=exactOwnedHandle(file.handle,file.path,file.identity,false);
            if(status!=S_OK)namespaceFatal("owned path-control cleanup child FileID",status);
            // Keep native parsing free of a persistent DELETE-access handle.
            // Acquire it only for cleanup, then revalidate its exact FileID.
            const auto deletion=CreateFileW(file.path.c_str(),FILE_READ_ATTRIBUTES|DELETE,
                FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
            if(deletion==INVALID_HANDLE_VALUE)namespaceFatal("open exact owned path-control deletion handle",HRESULT_FROM_WIN32(GetLastError()));
            const auto deletionStatus=exactOwnedHandle(deletion,file.path,file.identity,false);
            if(deletionStatus!=S_OK)namespaceFatal("owned path-control deletion-handle FileID",deletionStatus);
            FILE_DISPOSITION_INFO disposition{TRUE};
            if(!SetFileInformationByHandle(deletion,FileDispositionInfo,&disposition,sizeof(disposition)))
                namespaceFatal("delete exact owned path-control file handle",HRESULT_FROM_WIN32(GetLastError()));
            if(!CloseHandle(file.handle))namespaceFatal("close deleted owned path-control file",HRESULT_FROM_WIN32(GetLastError()));
            file.handle=INVALID_HANDLE_VALUE;
            if(!CloseHandle(deletion))namespaceFatal("close exact owned path-control deletion handle",HRESULT_FROM_WIN32(GetLastError()));
        }
        const auto finalParent=exactOwnedHandle(directory,root,directoryIdentity,true);
        if(finalParent!=S_OK)namespaceFatal("owned path-control final directory FileID",finalParent);
        const auto deletion=CreateFileW(root.c_str(),FILE_READ_ATTRIBUTES|DELETE,
            FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(deletion==INVALID_HANDLE_VALUE)namespaceFatal("open exact owned path-control directory deletion handle",HRESULT_FROM_WIN32(GetLastError()));
        const auto deletionStatus=exactOwnedHandle(deletion,root,directoryIdentity,true);
        if(deletionStatus!=S_OK)namespaceFatal("owned path-control directory deletion-handle FileID",deletionStatus);
        FILE_DISPOSITION_INFO disposition{TRUE};
        if(!SetFileInformationByHandle(deletion,FileDispositionInfo,&disposition,sizeof(disposition)))
            namespaceFatal("delete exact empty owned path-control directory handle",HRESULT_FROM_WIN32(GetLastError()));
        if(!CloseHandle(directory))namespaceFatal("close deleted owned path-control directory",HRESULT_FROM_WIN32(GetLastError()));
        if(!CloseHandle(deletion))namespaceFatal("close exact owned path-control directory deletion handle",HRESULT_FROM_WIN32(GetLastError()));
    }
};

void nativeOwnedDisplayPathIdentityAdmission() {
    const auto clipboard=GetClipboardSequenceNumber();
    OwnedPathIdentityControl owned;owned.initialize();
    const auto originalPath=owned.files[0].path,otherPath=owned.files[1].path;
    const auto immutable=owned.files[0].identity,wrongIdentity=owned.files[1].identity;
    require(!NativeBackgroundView::sameIdentity(immutable,wrongIdentity),"Two genuinely owned files must have distinct full native FileIDs");
    const auto same=NativeBackgroundView::readOwnedReportedPathIdentity(originalPath,originalPath,immutable);
    require(same.accepted&&same.originalRead==S_OK&&same.reportedRead==S_OK&&same.finalOriginalRead==S_OK,
        "Exact owned path identity was rejected");
    const auto wrong=NativeBackgroundView::readOwnedReportedPathIdentity(originalPath,otherPath,immutable);
    require(!wrong.accepted&&wrong.originalRead==S_OK&&wrong.reportedRead==S_OK&&wrong.finalOriginalRead==E_PENDING&&
        !NativeBackgroundView::sameIdentity(wrong.reported,immutable),"Different owned FileID was admitted as the native reported row");
    auto original=item(originalPath);PIDLIST_ABSOLUTE rawExpected=nullptr;
    const auto expectedRead=SHGetIDListFromObject(original.Get(),&rawExpected);NativeBackgroundView::Pidl expected(rawExpected);
    require(expectedRead==S_OK&&expected,"Capture actual original owned full PIDL");
    const auto verifyAlias=[&](const fs::path& aliasPath,const char* label) {
        const auto alias=item(aliasPath);PIDLIST_ABSOLUTE rawActual=nullptr;
        const auto actualRead=SHGetIDListFromObject(alias.Get(),&rawActual);NativeBackgroundView::Pidl actual(rawActual);
        require(actualRead==S_OK&&actual&&ILIsEqual(actual.get(),expected.get()),"Native owned alias changed the exact original full PIDL");
        int order=1;const auto canonical=original->Compare(alias.Get(),SICHINT_CANONICAL,&order);
        require(canonical==S_OK&&order==0,"Actual owned alias lost its independent native canonical Shell identity");
        const auto admitted=NativeBackgroundView::readOwnedReportedPathIdentity(originalPath,aliasPath,immutable);
        require(admitted.accepted&&admitted.originalRead==S_OK&&admitted.reportedRead==S_OK&&admitted.finalOriginalRead==S_OK,
            "Distinct native spelling of the same immutable owned file was rejected");
        std::cout<<"NativeBackground pathIdentityControl alias="<<label<<" actualAliasCovered=1 spellingDiffers="<<(aliasPath!=originalPath)
            <<" originalPathUnits="<<originalPath.native().size()<<" aliasPathUnits="<<aliasPath.native().size()<<" exactFullPIDL=1 canonicalS_OKOrder0=1 full128FileID=1\n";
    };
    std::array<wchar_t,32768> aliasPath{};
    for(const bool shortName:{true,false}) {
        SetLastError(ERROR_SUCCESS);
        const auto length=shortName?GetShortPathNameW(originalPath.c_str(),aliasPath.data(),static_cast<DWORD>(aliasPath.size())):
            GetLongPathNameW(originalPath.c_str(),aliasPath.data(),static_cast<DWORD>(aliasPath.size()));
        const auto error=GetLastError();
        require(length<aliasPath.size(),"Owned native alias query exceeded bounded buffer");
        require(length!=0||error==ERROR_NOT_SUPPORTED||error==ERROR_INVALID_FUNCTION,"Unexpected native owned alias query failure");
        const bool distinct=length!=0&&fs::path(aliasPath.data())!=originalPath;
        const char* label=shortName?"actual-native-8dot3":"actual-native-long-path";
        if(distinct)verifyAlias(fs::path(aliasPath.data()),label);
        else std::cout<<"UNAVAILABLE: NativeBackground pathIdentityControl alias="<<label<<" noDistinctNativeAlias=1 queryLength="<<length<<" Win32="<<error<<'\n';
    }
    PWSTR rawReported=nullptr;const auto pathRead=original->GetDisplayName(SIGDN_FILESYSPATH,&rawReported);
    std::unique_ptr<wchar_t,decltype(&CoTaskMemFree)> reported(rawReported,CoTaskMemFree);
    require(pathRead==S_OK&&reported,"Read actual owned native filesystem display path");
    verifyAlias(fs::path(reported.get()),"actual-native-display-path");
    original.Reset(); // End native parsed-item aliases before the owned replacement control.
    // Rename the still-open original inside the owned parent, then move the
    // already-existing distinct file to its former path. Keeping the old
    // handle alive proves the old FileID without delete-pending name reuse.
    const auto retired=owned.root/L"retained original document.txt";
    require(OwnedPathIdentityControl::exactOwnedHandle(owned.files[0].handle,originalPath,immutable,false)==S_OK&&
        OwnedPathIdentityControl::exactOwnedHandle(owned.files[1].handle,otherPath,wrongIdentity,false)==S_OK,
        "Replacement control lost original owned path/FileID authority");
    owned.renameOwned(owned.files[0],retired);
    require(OwnedPathIdentityControl::exactOwnedHandle(owned.files[0].handle,retired,immutable,false)==S_OK,
        "Retained original identity changed after owned rename");
    owned.renameOwned(owned.files[1],originalPath);
    const auto replaced=NativeBackgroundView::readOwnedReportedPathIdentity(originalPath,originalPath,immutable);
    require(!replaced.accepted&&replaced.originalRead==S_OK&&replaced.reportedRead==E_PENDING&&replaced.finalOriginalRead==E_PENDING&&
        NativeBackgroundView::sameIdentity(replaced.original,wrongIdentity),"Replacement source was admitted under its retired immutable FileID");
    require(OwnedPathIdentityControl::exactOwnedHandle(owned.files[0].handle,retired,immutable,false)==S_OK&&
        OwnedPathIdentityControl::exactOwnedHandle(owned.files[1].handle,originalPath,wrongIdentity,false)==S_OK,
        "Owned replacement changed either retained original full FileID");
    require(GetClipboardSequenceNumber()==clipboard,"Path identity control changed the shared clipboard");
    std::cout<<"NativeBackground pathIdentityControl wrongFileRejected=1 replacedOriginalRejected=1 reportedPathReadSkipped=1 full128FileID=1\n"<<std::flush;
}

struct CastFileSnapshot {
    FILE_ID_INFO identity{};
    FILE_BASIC_INFO basic{};
    FILE_STANDARD_INFO standard{};
};

CastFileSnapshot castFileSnapshot(const fs::path& path) {
    struct File {HANDLE value=INVALID_HANDLE_VALUE;~File(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}}file;
    file.value=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                           nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    require(file.value!=INVALID_HANDLE_VALUE,"Open only owned Cast fixture identity");
    CastFileSnapshot result;
    require(GetFileInformationByHandleEx(file.value,FileIdInfo,&result.identity,sizeof(result.identity))&&
            GetFileInformationByHandleEx(file.value,FileBasicInfo,&result.basic,sizeof(result.basic))&&
            GetFileInformationByHandleEx(file.value,FileStandardInfo,&result.standard,sizeof(result.standard)),
            "Read complete owned Cast fixture identity/metadata");
    require(!(result.basic.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)&&!result.standard.Directory,
            "Cast fixture must remain an owned ordinary file");
    return result;
}

bool sameCastFile(const CastFileSnapshot& left,const CastFileSnapshot& right) {
    return left.identity.VolumeSerialNumber==right.identity.VolumeSerialNumber&&
        std::memcmp(left.identity.FileId.Identifier,right.identity.FileId.Identifier,sizeof(left.identity.FileId.Identifier))==0;
}

std::vector<BYTE> castVideoBytes() {
    const auto u16=[](std::vector<BYTE>& bytes,std::uint16_t value){bytes.push_back(static_cast<BYTE>(value));bytes.push_back(static_cast<BYTE>(value>>8));};
    const auto u32=[](std::vector<BYTE>& bytes,std::uint32_t value){for(unsigned shift=0;shift<32;shift+=8)bytes.push_back(static_cast<BYTE>(value>>shift));};
    const auto four=[](std::vector<BYTE>& bytes,const char* value){for(unsigned index=0;index<4;++index)bytes.push_back(static_cast<BYTE>(value[index]));};
    const auto chunk=[&](std::vector<BYTE>& bytes,const char* tag,const std::vector<BYTE>& payload){
        four(bytes,tag);u32(bytes,static_cast<std::uint32_t>(payload.size()));bytes.insert(bytes.end(),payload.begin(),payload.end());
        if(payload.size()&1)bytes.push_back(0);
    };
    // Same complete, uncompressed 2x2 DIB AVI recipe as visual_fixture_builder;
    // read-only native AVI parsing below validates the real frame and index.
    std::vector<BYTE> main,stream,bitmap;
    for(const auto value:std::array<std::uint32_t,14>{1000000,16,0,0x10,1,0,1,16,2,2,0,0,0,0})u32(main,value);
    four(stream,"vids");four(stream,"DIB ");u32(stream,0);u16(stream,0);u16(stream,0);
    for(const auto value:std::array<std::uint32_t,8>{0,1,1,0,1,16,0xffffffffu,0})u32(stream,value);
    u16(stream,0);u16(stream,0);u16(stream,2);u16(stream,2);
    u32(bitmap,40);u32(bitmap,2);u32(bitmap,2);u16(bitmap,1);u16(bitmap,24);
    for(const auto value:std::array<std::uint32_t,6>{0,16,0,0,0,0})u32(bitmap,value);
    std::vector<BYTE> streams;four(streams,"strl");chunk(streams,"strh",stream);chunk(streams,"strf",bitmap);
    std::vector<BYTE> headers;four(headers,"hdrl");chunk(headers,"avih",main);chunk(headers,"LIST",streams);
    const std::vector<BYTE> pixels{0x20,0x60,0xc0,0x20,0x60,0xc0,0,0,0xc0,0x60,0x20,0xc0,0x60,0x20,0,0};
    std::vector<BYTE> movie;four(movie,"movi");chunk(movie,"00db",pixels);
    std::vector<BYTE> index;four(index,"00db");u32(index,0x10);u32(index,4);u32(index,16);
    std::vector<BYTE> body;four(body,"AVI ");chunk(body,"LIST",headers);chunk(body,"LIST",movie);chunk(body,"idx1",index);
    std::vector<BYTE> result;chunk(result,"RIFF",body);return result;
}

void validateCastVideo(const fs::path& path) {
    struct Avi {
        PAVIFILE file=nullptr;PAVISTREAM stream=nullptr;
        Avi(){AVIFileInit();}
        ~Avi(){if(stream)AVIStreamRelease(stream);if(file)AVIFileRelease(file);AVIFileExit();}
    }avi;
    succeeded(AVIFileOpenW(&avi.file,path.c_str(),OF_READ|OF_SHARE_DENY_WRITE,nullptr),"Parse owned Cast AVI without playback");
    succeeded(AVIFileGetStream(avi.file,&avi.stream,streamtypeVIDEO,0),"Read owned Cast video stream");
    AVISTREAMINFOW info{};succeeded(AVIStreamInfoW(avi.stream,&info,sizeof(info)),"Read actual native AVI dimensions");
    std::array<BYTE,16> frame{};LONG bytes=0,samples=0;
    succeeded(AVIStreamRead(avi.stream,0,1,frame.data(),static_cast<LONG>(frame.size()),&bytes,&samples),"Read owned Cast AVI frame without decoding/player UI");
    require(info.dwLength==1&&info.rcFrame.right==2&&info.rcFrame.bottom==2&&samples==1&&bytes==16,
            "Owned Cast AVI lacks its complete native frame");
}

void verifyCastCida(IDataObject* data,const CastFileSnapshot& expected) {
    const auto format=RegisterClipboardFormatW(CFSTR_SHELLIDLIST);require(format!=0,"Register identification format without touching clipboard");
    FORMATETC request{static_cast<CLIPFORMAT>(format),nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};
    struct Medium {STGMEDIUM value{};~Medium(){ReleaseStgMedium(&value);}}medium;
    succeeded(data->GetData(&request,&medium.value),"Read original native owned-selection CIDA");
    require(medium.value.tymed==TYMED_HGLOBAL&&medium.value.hGlobal,"Native Cast CIDA medium is invalid");
    const auto size=GlobalSize(medium.value.hGlobal);require(size>=3*sizeof(UINT)&&size<=1024*1024,"Native Cast CIDA size bound");
    struct Lock {HGLOBAL value;const BYTE* bytes;~Lock(){if(bytes)GlobalUnlock(value);}}lock{
        medium.value.hGlobal,static_cast<const BYTE*>(GlobalLock(medium.value.hGlobal))};
    require(lock.bytes!=nullptr,"Lock bounded native Cast CIDA");
    UINT count=0;std::memcpy(&count,lock.bytes,sizeof(count));require(count==1,"Cast CIDA lost its complete one-item selection");
    std::array<UINT,2> offsets{};std::memcpy(offsets.data(),lock.bytes+sizeof(UINT),sizeof(offsets));
    using Pidl=std::unique_ptr<ITEMIDLIST,decltype(&CoTaskMemFree)>;
    const auto copy=[&](UINT offset){
        require(offset>=3*sizeof(UINT)&&offset<size&&offset%alignof(USHORT)==0,"Native Cast CIDA offset bound");
        size_t end=offset;
        for(;;){require(size-end>=sizeof(USHORT),"Native Cast CIDA PIDL terminator bound");USHORT length=0;
            std::memcpy(&length,lock.bytes+end,sizeof(length));if(!length){end+=sizeof(length);break;}
            require(length>=sizeof(length)&&length<=size-end,"Native Cast CIDA item bound");end+=length;}
        auto owned=static_cast<ITEMIDLIST*>(CoTaskMemAlloc(end-offset));require(owned!=nullptr,"Copy bounded original native Cast PIDL");
        std::memcpy(owned,lock.bytes+offset,end-offset);return Pidl(owned,CoTaskMemFree);
    };
    auto parent=copy(offsets[0]),child=copy(offsets[1]);
    struct AbsolutePidl {PIDLIST_ABSOLUTE value=nullptr;~AbsolutePidl(){CoTaskMemFree(value);}}absolute{ILCombine(parent.get(),child.get())};
    require(absolute.value!=nullptr,"Combine original native Cast parent/child identity");
    ComPtr<IShellItem> source;succeeded(SHCreateItemFromIDList(absolute.value,IID_PPV_ARGS(&source)),"Read actual native Cast CIDA member");
    PWSTR raw=nullptr;succeeded(source->GetDisplayName(SIGDN_FILESYSPATH,&raw),"Read only owned Cast CIDA filesystem identity");
    std::unique_ptr<wchar_t,decltype(&CoTaskMemFree)> path(raw,CoTaskMemFree);require(path!=nullptr,"Owned Cast CIDA path is null");
    require(sameCastFile(castFileSnapshot(path.get()),expected),"Cast CIDA substituted a different native FileID");
}

struct CastMenuSnapshot {
    HRESULT status=E_PENDING,siteStatus=E_NOINTERFACE;
    CLSID identifier{};
    UINT id=0,state=0;bool submenu=false,parentEnabled=false,derivedEnabled=false;
    unsigned children=0,enabledLeaves=0,maximumDepth=0;
};

struct CastVisibleWindow {
    HWND window=nullptr,owner=nullptr;
    DWORD process=0,thread=0;
    HDESK threadDesktop=nullptr;
    std::array<char,256> name{};
};

struct CastVisibilityBaseline {
    std::array<CastVisibleWindow,128> windows{};
    size_t count=0;
};

CastVisibilityBaseline readCastIsolation(HWND owner,const char* checkpoint) {
    const auto desktop=explorer::PrivateDesktop::current();require(desktop!=nullptr,"Cast metadata requires its owned private desktop");
    succeeded(desktop->verifyIsolation(),"Keep Cast metadata on its unchanged private/input desktop");
    bool inputVisible=true;succeeded(desktop->visibleWindowsOnInputDesktop(inputVisible),"Observe actual Cast input-desktop visibility");
    struct Observation {
        const char* checkpoint;const std::wstring* desktopName;
        CastVisibilityBaseline result;
        unsigned total=0,visibleForeign=0;
        bool identitiesValid=true,overflow=false;
    }observation{checkpoint,&desktop->name()};
    SetLastError(ERROR_SUCCESS);
    const auto enumerated=EnumDesktopWindows(GetThreadDesktop(GetCurrentThreadId()),[](HWND window,LPARAM context)->BOOL {
        auto& observed=*reinterpret_cast<Observation*>(context);++observed.total;
        DWORD process=0;const auto thread=GetWindowThreadProcessId(window,&process);
        if(!IsWindowVisible(window)){SetLastError(ERROR_SUCCESS);return TRUE;}
        if(process!=GetCurrentProcessId()){++observed.visibleForeign;SetLastError(ERROR_SUCCESS);return TRUE;}
        std::array<char,256> name{};SetLastError(ERROR_SUCCESS);
        const auto classLength=GetClassNameA(window,name.data(),static_cast<int>(name.size()));const auto classError=GetLastError();
        RECT rect{};SetLastError(ERROR_SUCCESS);const auto rectRead=GetWindowRect(window,&rect);const auto rectError=GetLastError();
        const auto style=GetWindowLongPtrW(window,GWL_STYLE),extended=GetWindowLongPtrW(window,GWL_EXSTYLE);
        const auto nativeOwner=GetWindow(window,GW_OWNER);
        SetLastError(ERROR_SUCCESS);const auto threadDesktop=GetThreadDesktop(thread);const auto desktopError=GetLastError();
        std::array<wchar_t,256> desktopName{};DWORD needed=0;SetLastError(ERROR_SUCCESS);
        const auto desktopRead=threadDesktop?GetUserObjectInformationW(threadDesktop,UOI_NAME,desktopName.data(),
            static_cast<DWORD>(sizeof(desktopName)),&needed):FALSE;
        const auto desktopNameError=GetLastError();
        const bool desktopMatches=desktopRead&&needed>=sizeof(wchar_t)&&needed<=sizeof(desktopName)&&
            needed%sizeof(wchar_t)==0&&desktopName[needed/sizeof(wchar_t)-1]==L'\0'&&
            std::wstring_view(desktopName.data(),needed/sizeof(wchar_t)-1)==*observed.desktopName;
        observed.identitiesValid=observed.identitiesValid&&thread!=0&&classLength>0&&rectRead&&desktopMatches;
        if(observed.result.count<observed.result.windows.size())
            observed.result.windows[observed.result.count++]={window,nativeOwner,process,thread,threadDesktop,name};
        else observed.overflow=true;
        std::cout<<"CastState visiblePrivate checkpoint="<<observed.checkpoint
            <<" hwnd="<<reinterpret_cast<uintptr_t>(window)<<" process="<<process<<" thread="<<thread
            <<" class="<<name.data()<<" classLength="<<classLength<<" classError="<<classError
            <<" owner="<<reinterpret_cast<uintptr_t>(nativeOwner)<<" rectRead="<<rectRead<<" rectError="<<rectError
            <<" left="<<rect.left<<" top="<<rect.top<<" right="<<rect.right<<" bottom="<<rect.bottom
            <<" style="<<static_cast<DWORD>(style)<<" exStyle="<<static_cast<DWORD>(extended)
            <<" nativeThreadDesktop="<<reinterpret_cast<uintptr_t>(threadDesktop)<<" desktopError="<<desktopError
            <<" desktopRead="<<desktopRead<<" desktopNameError="<<desktopNameError<<" samePrivateDesktop="<<desktopMatches<<'\n'<<std::flush;
        SetLastError(ERROR_SUCCESS);
        return TRUE;
    },reinterpret_cast<LPARAM>(&observation));
    const auto enumerationError=GetLastError();const auto ownerVisible=IsWindowVisible(owner);
    std::cout<<"CastState isolation checkpoint="<<checkpoint<<" enumerated="<<enumerated<<" enumerationError="<<enumerationError
        <<" visited="<<observation.total<<" visibleOwned="<<observation.result.count<<" visibleForeign="<<observation.visibleForeign
        <<" identitiesValid="<<observation.identitiesValid<<" overflow="<<observation.overflow
        <<" inputVisible="<<inputVisible<<" fixtureOwnerVisible="<<ownerVisible<<'\n'<<std::flush;
    require(enumerated!=FALSE,"Read native Cast desktop windows while its hidden owner remains alive");
    require(!inputVisible&&!ownerVisible&&observation.visibleForeign==0&&observation.identitiesValid&&!observation.overflow,
            "Cast fixture visibility lacks exact own-process/native-thread/private-desktop provenance");
    succeeded(desktop->verifyIsolation(),"Reverify private/input desktop after Cast visibility enumeration");
    succeeded(desktop->visibleWindowsOnInputDesktop(inputVisible),"Recheck input visibility after Cast native-window identities");
    require(!inputVisible&&!IsWindowVisible(owner),"Cast visibility read displayed input UI or its fixture owner");
    return observation.result;
}

void verifyCastIsolation(HWND owner,const char* checkpoint,const CastVisibilityBaseline& baseline) {
    const auto current=readCastIsolation(owner,checkpoint);
    bool unchanged=current.count==baseline.count;
    for(size_t index=0;index<current.count;++index){
        const auto& actual=current.windows[index];
        const auto matching=std::find_if(baseline.windows.begin(),baseline.windows.begin()+baseline.count,[&](const auto& original){
            return actual.window==original.window&&actual.owner==original.owner&&actual.process==original.process&&
                actual.thread==original.thread&&actual.threadDesktop==original.threadDesktop&&actual.name==original.name;
        });
        unchanged=unchanged&&matching!=baseline.windows.begin()+baseline.count;
    }
    std::cout<<"CastState baseline checkpoint="<<checkpoint<<" expectedVisible="<<baseline.count
        <<" actualVisible="<<current.count<<" exactNativeWindowSet="<<unchanged<<'\n'<<std::flush;
    require(unchanged,"Read-only Cast provider changed the settled exact visible native-window baseline");
}

CastMenuSnapshot readCastMenu(const wchar_t* association,const char* diagnostic,NativeBackgroundView& host,IDataObject* data,
                              const CastVisibilityBaseline& baseline) {
    CastMenuSnapshot result;
    std::cout<<"CastState association="<<diagnostic<<" stage=before-provider-isolation\n"<<std::flush;
    verifyCastIsolation(host.owner,"before-provider",baseline);
    std::cout<<"CastState association="<<diagnostic<<" stage=registry\n";
    struct Key {HKEY value=nullptr;~Key(){if(value)RegCloseKey(value);}}key;
    const auto opened=RegOpenKeyExW(HKEY_CLASSES_ROOT,association,0,KEY_READ,&key.value);
    if(opened!=ERROR_SUCCESS){result.status=HRESULT_FROM_WIN32(opened);return result;}
    std::array<wchar_t,128> handler{};DWORD bytes=static_cast<DWORD>(sizeof(handler));
    const auto readHandler=RegGetValueW(key.value,L"shellex\\ContextMenuHandlers\\PlayTo",nullptr,RRF_RT_REG_SZ,
                                       nullptr,handler.data(),&bytes);
    if(readHandler!=ERROR_SUCCESS){result.status=HRESULT_FROM_WIN32(readHandler);return result;}
    require(bytes>=2*sizeof(wchar_t)&&bytes<=sizeof(handler)&&handler[(bytes/sizeof(wchar_t))-1]==0,
            "Installed Cast handler registration lacks a bounded CLSID");
    CLSID identifier{};succeeded(CLSIDFromString(handler.data(),&identifier),"Read actual installed Cast handler CLSID");result.identifier=identifier;
    ComPtr<IContextMenu> context;std::cout<<"CastState association="<<diagnostic<<" stage=create-provider\n";
    result.status=CoCreateInstance(identifier,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&context));
    if(FAILED(result.status))return result;
    ComPtr<IShellExtInit> initialize;succeeded(context.As(&initialize),"Read native Cast initialization contract");
    std::cout<<"CastState association="<<diagnostic<<" stage=initialize\n";
    succeeded(initialize->Initialize(nullptr,data,key.value),"Initialize real Cast handler with original selection and exact association key");
    explorer::NativeContextMenu menu;std::cout<<"CastState association="<<diagnostic<<" stage=query-menu\n";
    // Preserve this isolated handler's original QueryContextMenu flags. The
    // later snapshot never sends WM_INITMENUPOPUP to the artificial root.
    succeeded(menu.create(host.owner,context.Get(),host.view.Get(),CMF_NORMAL),"Query isolated real Cast handler with original synchronous-cascade flags and native view site");
    ComPtr<IObjectWithSite> sited;
    result.siteStatus=context.As(&sited);
    if(SUCCEEDED(result.siteStatus)){
        ComPtr<IUnknown> actual,expected;result.siteStatus=sited->GetSite(IID_PPV_ARGS(&actual));
        succeeded(host.view.As(&expected),"Read original Cast native view COM identity");
        if(SUCCEEDED(result.siteStatus))require(actual.Get()==expected.Get(),"Cast handler received a different native view site");
    }
    std::vector<explorer::ContextMenuEntry> entries;
    std::cout<<"CastState association="<<diagnostic<<" stage=snapshot-native-parent\n";
    succeeded(menu.enumerate(entries,false),"Read actual native Cast parent before any popup initialization");
    const auto readParent=[&](const char* phase) {
        const explorer::ContextMenuEntry* parent=nullptr;
        for(const auto& entry:entries)if(!entry.separator()){
            std::cout<<"CastState association="<<diagnostic<<" phase="<<phase<<" parentCandidateId="<<entry.id<<" parentCandidateState="<<entry.state
                <<" parentCandidateEnabled="<<entry.enabled()<<" parentCandidateSubmenu="<<entry.submenu<<'\n';
            require(!parent,"Isolated Cast handler returned ambiguous parent commands");parent=&entry;
        }
        require(parent&&parent->id&&parent->id<=0x7fff,"Isolated Cast handler has no retained native parent ordinal");
        MENUITEMINFOW metadata{sizeof(metadata)};metadata.fMask=MIIM_STATE|MIIM_SUBMENU|MIIM_ID;
        require(GetMenuItemInfoW(menu.menu(),parent->id,FALSE,&metadata)!=FALSE,"Read raw native Cast parent menu state");
        require(metadata.wID==parent->id&&metadata.fState==parent->state&&(metadata.hSubMenu!=nullptr)==parent->submenu,
                "Cast metadata disagrees with the original native parent menu");
        std::cout<<"CastState association="<<diagnostic<<" phase="<<phase<<" rawParentId="<<metadata.wID<<" rawParentState="<<metadata.fState
            <<" rawParentSubmenu="<<reinterpret_cast<uintptr_t>(metadata.hSubMenu)<<" rootWM_INIT=0\n"<<std::flush;
        result.id=metadata.wID;result.state=metadata.fState;result.submenu=metadata.hSubMenu!=nullptr;
        result.parentEnabled=parent->enabled();
        return parent;
    };
    const auto* parent=readParent("before-population");
    if(parent->enabled()&&parent->submenu) {
        const auto command=parent->id;
        std::cout<<"CastState association="<<diagnostic<<" stage=populate-exact-enabled-parent parentId="<<command<<'\n'<<std::flush;
        succeeded(menu.enumerateForCommand(command,entries),"Populate only the actual enabled native Cast submenu without displaying or invoking it");
        parent=readParent("after-population");
        require(parent->id==command,"Native Cast submenu population replaced the original parent ordinal");
    } else {
        std::cout<<"CastState association="<<diagnostic<<" stage=parent-population-not-applicable enabled="<<parent->enabled()
            <<" submenu="<<parent->submenu<<" popupAttempts=0\n"<<std::flush;
    }
    unsigned budget=4096;
    std::function<void(const std::vector<explorer::ContextMenuEntry>&,unsigned,bool)> count;
    count=[&](const auto& rows,unsigned depth,bool ancestorsEnabled){
        require(depth<=16,"Native Cast children exceed bounded depth");
        for(const auto& row:rows){require(budget!=0,"Native Cast children exceed bounded count");--budget;
            if(row.separator())continue;++result.children;result.maximumDepth=(std::max)(result.maximumDepth,depth);
            const bool enabled=ancestorsEnabled&&row.enabled();
            if(row.submenu)count(row.children,depth+1,enabled);else if(enabled)++result.enabledLeaves;}
    };
    count(parent->children,1,true);
    result.derivedEnabled=result.parentEnabled&&(!result.submenu||result.enabledLeaves!=0);result.status=S_OK;
    std::cout<<"CastState association="<<diagnostic<<" parentId="<<result.id<<" parentState="<<result.state
        <<" parentEnabled="<<result.parentEnabled<<" submenu="<<result.submenu<<" childCount="<<result.children
        <<" enabledLeaves="<<result.enabledLeaves<<" maximumDepth="<<result.maximumDepth
        <<" hostLeafDerivedEnabled="<<result.derivedEnabled<<" siteHRESULT="<<static_cast<unsigned long>(result.siteStatus)<<'\n';
    std::cout<<"CastState association="<<diagnostic<<" stage=after-menu-isolation\n"<<std::flush;
    verifyCastIsolation(host.owner,"after-menu",baseline);
    return result;
}

void nativeVideoCastParentState() {
    std::cout<<"CastState stage=owned-valid-avi\n";
    Fixture fixture;const auto source=fixture.root/L"owned Cast video.avi";const auto bytes=castVideoBytes();
    {std::ofstream output(source,std::ios::binary);output.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
        require(output.good(),"Create complete owned Cast video without overwriting other files");}
    const auto before=castFileSnapshot(source);const auto originalBytes=read(source),originalText=read(fixture.text);
    validateCastVideo(source);const auto clipboard=GetClipboardSequenceNumber();
    SHELLSTATE settings{};constexpr DWORD settingsMask=SSF_SHOWALLOBJECTS|SSF_SHOWSUPERHIDDEN|SSF_SHOWEXTENSIONS|SSF_NOCONFIRMRECYCLE;
    SHGetSetSettings(&settings,settingsMask,FALSE);
    auto folder=item(fixture.root),video=item(source);NativeBackgroundView host;
    host.initialize(folder.Get(),{fixture.image,fixture.program,fixture.disc,fixture.text,source},video.Get());
    ComPtr<IFolderView2> folderView;succeeded(host.view.As(&folderView),"Read actual native Cast fixture folder view");
    struct Pidl {PIDLIST_ABSOLUTE value=nullptr;~Pidl(){CoTaskMemFree(value);}}videoPidl;
    succeeded(SHGetIDListFromObject(video.Get(),&videoPidl.value),"Retain actual owned video identity for native selection");
    host.selectionDiagnostic("video-before-original-selection");
    const auto selectionDeadline=GetTickCount64()+5000;
    const auto nativeIndex=host.ownedNativeIndex(videoPidl.value,selectionDeadline,"video");
    require(GetTickCount64()<selectionDeadline&&host.sourceCurrent(folderView.Get())&&GetTickCount64()<selectionDeadline,"Video indexed selection source/deadline expired before its single request");
    const auto selectStatus=folderView->SelectItem(nativeIndex,SVSI_SELECT|SVSI_DESELECTOTHERS|SVSI_FOCUSED);
    std::cout<<"NativeBackground selection action=video HRESULT="<<static_cast<unsigned long>(selectStatus)<<" requests=1\n"<<std::flush;
    succeeded(selectStatus,"Select only owned video in actual native view");
    ComPtr<IShellItemArray> selected;
    const auto selectionWaitStart=GetTickCount64();
    const bool selectionReady=selectionWaitStart<selectionDeadline&&pumpPrivateNamespaceUntil([&]{
        if(GetTickCount64()>=selectionDeadline||!host.sourceCurrent(folderView.Get()))return false;
        selected.Reset();if(FAILED(folderView->GetSelection(FALSE,&selected))||!selected)return false;
        DWORD count=0;return SUCCEEDED(selected->GetCount(&count))&&count==1&&GetTickCount64()<selectionDeadline&&host.sourceCurrent(folderView.Get())&&GetTickCount64()<selectionDeadline;
    },static_cast<DWORD>(selectionDeadline-selectionWaitStart),selectionDeadline);
    host.selectionDiagnostic("video-after-original-selection");
    require(selectionReady,"Actual native Cast video selection exceeded bounded wait");
    ComPtr<IShellItem> actual;succeeded(selected->GetItemAt(0,&actual),"Read exact original Cast selection member");
    int order=1;succeeded(actual->Compare(video.Get(),SICHINT_CANONICAL,&order),"Compare original native selected video identity");require(order==0,"Native Cast view selected another fixture item");
    explorer::NamespaceSelectionKinds kinds;succeeded(explorer::namespaceSelectionKinds(selected.Get(),&kinds),"Read actual native Cast video kind");
    require(kinds.count==1&&kinds.video&&!kinds.music,"Valid AVI did not supply the native Video context");
    ComPtr<IDataObject> data;succeeded(selected->BindToHandler(nullptr,BHID_DataObject,IID_PPV_ARGS(&data)),"Retain original native Cast selection data object");
    verifyCastCida(data.Get(),before);
    // Native Shell/input helpers may finish creating their owned private HWNDs
    // after navigation. Settle them before the first provider exists; no class
    // name is exempted from exact baseline identity/owner preservation later.
    const auto settleStarted=GetTickCount64();
    require(pumpPrivateNamespaceUntil([&]{
        const auto desktop=explorer::PrivateDesktop::current();bool inputVisible=true;
        succeeded(desktop->visibleWindowsOnInputDesktop(inputVisible),"Keep input desktop invisible while settling native fixture helpers");
        require(!inputVisible&&!IsWindowVisible(host.owner),"Settling native Cast fixture displayed input UI or its owner");
        return GetTickCount64()-settleStarted>=450;
    },500),"Settle native fixture messages within the bounded pre-provider window");
    const auto settleElapsed=GetTickCount64()-settleStarted;
    std::cout<<"CastState stage=settled-before-provider elapsedMilliseconds="<<settleElapsed<<'\n'<<std::flush;
    require(settleElapsed<=500,"Native fixture settlement exceeded its 500 ms admission budget");
    const auto visibilityBaseline=readCastIsolation(host.owner,"baseline-before-any-cast-provider");
    const auto audio=readCastMenu(L"SystemFileAssociations\\audio","audio",host,data.Get(),visibilityBaseline);
    verifyCastCida(data.Get(),before);
    const auto videoState=readCastMenu(L"SystemFileAssociations\\video","video",host,data.Get(),visibilityBaseline);
    verifyCastCida(data.Get(),before);
    verifyCastIsolation(host.owner,"before-production-state",visibilityBaseline);
    DWORD nativeSelectionCount=0;SFGAOF nativeSelectionAttributes=0;
    succeeded(selected->GetCount(&nativeSelectionCount),"Read original whole-array Cast selection count");
    const auto nativeAttributesStatus=selected->GetAttributes(static_cast<SIATTRIBFLAGS>(SIATTRIBFLAGS_AND|SIATTRIBFLAGS_ALLITEMS),
        SFGAO_FILESYSTEM|SFGAO_FOLDER|SFGAO_LINK,&nativeSelectionAttributes);
    succeeded(nativeAttributesStatus,"Read original whole-array Cast attributes without sampling");
    require(nativeSelectionCount==1,"Original whole-array Cast selection no longer contains only the owned video");
    explorer::NativeNamespaceActions actions;succeeded(actions.initialize(host.owner,{folder,selected,host.view}),"Retain same original video array/native site for production Cast state");
    explorer::NamespaceCommandState state;std::cout<<"CastState stage=production-state\n";
    const auto status=actions.queryActionState(NamespaceAction::CastToDevice,&state);const auto facts=actions.facts();
    std::cout<<"CastState audioHRESULT="<<static_cast<unsigned long>(audio.status)<<" videoHRESULT="<<static_cast<unsigned long>(videoState.status)
        <<" sameProviderClass="<<(SUCCEEDED(audio.status)&&SUCCEEDED(videoState.status)&&IsEqualCLSID(audio.identifier,videoState.identifier))
        <<" productionHRESULT="<<static_cast<unsigned long>(status)<<" nativeState="<<state.state
        <<" productionEnabled="<<(SUCCEEDED(status)&&state.enabled())<<" selectionCount="<<facts.selectionCount
        <<" physicalFiles="<<facts.physicalFiles<<" media="<<facts.media<<" castItems="<<facts.castItems
        <<" fallbackAvailable="<<facts.castHandlerAvailable<<" fallbackEnabled="<<facts.castHandlerEnabled
        <<" fallbackSubmenu="<<facts.castHandlerSubmenu<<" fallbackId="<<facts.castCommandId<<'\n';
    const bool sameNativeProvider=audio.status==S_OK&&videoState.status==S_OK&&IsEqualCLSID(audio.identifier,videoState.identifier);
    if(sameNativeProvider)require(status==S_OK&&facts.castHandlerAvailable,
        "Production rejected the independently available same-class native Cast handler for the original whole array");
    if(facts.castHandlerAvailable&&audio.status==S_OK)require(status==S_OK&&
        state.state==static_cast<EXPCMDSTATE>(audio.derivedEnabled?ECS_ENABLED:ECS_DISABLED)&&state.enabled()==facts.castHandlerEnabled&&
        facts.castHandlerEnabled==audio.derivedEnabled&&facts.castCommandId==audio.id&&facts.castHandlerSubmenu==audio.submenu,
        "Production Cast admission/ordinal/submenu/state differs from the actual audio-initialized native menu reduction");
    require(!facts.castHandlerAvailable||(audio.status==S_OK&&videoState.status==S_OK),"Available native Cast fallback could not be independently compared");
    require(facts.selectionCount==nativeSelectionCount&&facts.nativeAttributesStatus==nativeAttributesStatus&&
        facts.nativeAttributes==nativeSelectionAttributes&&facts.detailedTargetsKnown&&facts.filesystem&&facts.physicalFiles&&
        !facts.physicalFolders&&facts.media&&facts.castItems,
        "Production Cast eligibility lost the original complete native video array facts");
    const auto after=castFileSnapshot(source);SHELLSTATE current{};SHGetSetSettings(&current,settingsMask,FALSE);
    const auto desktop=explorer::PrivateDesktop::current();bool visible=true;
    succeeded(desktop->verifyIsolation(),"Preserve exact private/input desktop during Cast metadata reads");
    succeeded(desktop->visibleWindowsOnInputDesktop(visible),"Inspect input-desktop visibility after native Cast readback");
    require(!visible&&!IsWindowVisible(host.owner)&&GetClipboardSequenceNumber()==clipboard&&read(source)==originalBytes&&read(fixture.text)==originalText&&
        sameCastFile(before,after)&&before.basic.CreationTime.QuadPart==after.basic.CreationTime.QuadPart&&
        before.basic.LastWriteTime.QuadPart==after.basic.LastWriteTime.QuadPart&&before.basic.FileAttributes==after.basic.FileAttributes&&
        before.standard.EndOfFile.QuadPart==after.standard.EndOfFile.QuadPart&&
        current.fShowAllObjects==settings.fShowAllObjects&&current.fShowSuperHidden==settings.fShowSuperHidden&&
        current.fShowExtensions==settings.fShowExtensions&&current.fNoConfirmRecycle==settings.fNoConfirmRecycle,
        "Native Cast state read changed source identities/content/settings/clipboard or input visibility");
    verifyCastCida(data.Get(),before);
    verifyCastIsolation(host.owner,"after-production-state",visibilityBaseline);
    std::cout<<"CastState sourceNativeVideo=1 frameSamples=1 frameBytes=16 exactCidaFileID=1 sourceUnchanged=1 settingsUnchanged=1 noInputUI=1\n";
}

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

class PlanningMenu final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
    Microsoft::WRL::ChainInterfaces<IContextMenu3,IContextMenu2,IContextMenu>> {
public:
    unsigned queries=0,rootMessages=0,unrelatedMessages=0,libraryMessages=0,nestedMessages=0,containerMessages=0,invocations=0,message2Calls=0,message3Calls=0;
    UINT flags=0,first=0;
    HMENU root=nullptr,unrelated=nullptr,library=nullptr,nested=nullptr,container=nullptr;
    HRESULT libraryStatus=S_OK;
    bool failUnrelated=true,disabledLibrary=false,disabledContainer=false,nestedDuplicate=false,ambiguousLibrary=false;
    bool anonymousLibrary=false,duplicateLibraryId=false;
    std::function<void()> duringLibrary;

    static HRESULT insert(HMENU menu,UINT position,UINT id,const wchar_t* label,UINT state=MFS_ENABLED,HMENU child=nullptr) {
        MENUITEMINFOW info{sizeof(info)};
        info.fMask=MIIM_ID|MIIM_STRING|MIIM_STATE|MIIM_SUBMENU;
        info.wID=id;info.dwTypeData=const_cast<wchar_t*>(label);info.fState=state;info.hSubMenu=child;
        if(InsertMenuItemW(menu,position,TRUE,&info))return S_OK;
        const auto error=GetLastError();return HRESULT_FROM_WIN32(error?error:ERROR_GEN_FAILURE);
    }
    static HRESULT cascade(HMENU menu,UINT position,UINT id,const wchar_t* label,UINT state,HMENU* child) {
        *child=CreatePopupMenu();if(!*child)return E_OUTOFMEMORY;
        const auto hr=insert(menu,position,id,label,state,*child);
        if(FAILED(hr)){DestroyMenu(*child);*child=nullptr;}return hr;
    }
    HRESULT STDMETHODCALLTYPE QueryContextMenu(HMENU menu,UINT position,UINT begin,UINT last,UINT queryFlags) override {
        ++queries;root=menu;first=begin;flags=queryFlags;
        if(last<first+23)return E_INVALIDARG;
        rootMessages=unrelatedMessages=libraryMessages=nestedMessages=containerMessages=message2Calls=message3Calls=0;
        unrelated=library=nested=container=nullptr;
        HRESULT hr=insert(root,position,first+2,L"Owned translated leaf",MFS_DEFAULT|MFS_CHECKED);
        if(SUCCEEDED(hr))hr=cascade(root,position+1,first+8,L"Unrelated translated cascade",MFS_ENABLED,&unrelated);
        if(SUCCEEDED(hr))hr=cascade(root,position+2,first+10,L"Requested translated cascade",disabledLibrary?MFS_DISABLED:MFS_ENABLED,&library);
        if(SUCCEEDED(hr))hr=cascade(root,position+3,first+14,L"Owned ancestor",disabledContainer?MFS_DISABLED:MFS_ENABLED,&container);
        if(SUCCEEDED(hr))hr=insert(container,0,first+15,L"Owned nested leaf");
        if(SUCCEEDED(hr)&&ambiguousLibrary)hr=insert(root,position+4,first+20,L"Ambiguous canonical peer");
        if(SUCCEEDED(hr)&&duplicateLibraryId)hr=insert(root,position+4,first+10,L"Duplicate native ordinal");
        return FAILED(hr)?hr:MAKE_HRESULT(SEVERITY_SUCCESS,0,24);
    }
    HRESULT STDMETHODCALLTYPE InvokeCommand(CMINVOKECOMMANDINFO*) override {++invocations;return E_ACCESSDENIED;}
    HRESULT STDMETHODCALLTYPE GetCommandString(UINT_PTR ordinal,UINT requested,UINT*,LPSTR text,UINT capacity) override {
        if(requested!=GCS_VERBW)return E_NOTIMPL;
        const wchar_t* verb=nullptr;
        switch(ordinal) {
        case 2:verb=L"rotate90";break;
        case 8:verb=L"PlayTo";break;
        case 10:case 20:if(anonymousLibrary)return E_NOTIMPL;verb=L"Windows.includeinlibrary";break;
        case 14:verb=L"ownedcontainer";break;
        case 15:verb=nestedDuplicate?L"rotate90":L"rotate270";break;
        case 13:verb=L"ownednested";break;
        default:return E_NOTIMPL;
        }
        return wcscpy_s(reinterpret_cast<wchar_t*>(text),capacity,verb)?E_FAIL:S_OK;
    }
    HRESULT message(UINT message,WPARAM raw,LPARAM packed) {
        if(message!=WM_INITMENUPOPUP||HIWORD(packed))return E_NOTIMPL;
        const auto menu=reinterpret_cast<HMENU>(raw);
        if(menu==root){++rootMessages;return S_OK;}
        if(menu==unrelated) {
            ++unrelatedMessages;if(LOWORD(packed)!=1)return E_INVALIDARG;
            if(failUnrelated)return E_ACCESSDENIED;
            return GetMenuItemCount(unrelated)?S_OK:insert(unrelated,0,first+9,L"Real unrelated provider choice");
        }
        if(menu==container){++containerMessages;return LOWORD(packed)==3?S_OK:E_INVALIDARG;}
        if(menu==library) {
            ++libraryMessages;if(LOWORD(packed)!=2)return E_INVALIDARG;
            if(duringLibrary)duringLibrary();
            if(libraryStatus!=S_OK)return libraryStatus;
            if(GetMenuItemCount(library))return S_OK;
            HRESULT hr=insert(library,0,first+11,L"Owned native default",MFS_DEFAULT|MFS_CHECKED);
            if(SUCCEEDED(hr))hr=insert(library,1,first+12,L"Owned native disabled",MFS_DISABLED);
            if(SUCCEEDED(hr))hr=cascade(library,2,first+13,L"Owned nested cascade",MFS_ENABLED,&nested);
            return hr;
        }
        if(menu==nested) {
            ++nestedMessages;if(LOWORD(packed)!=2)return E_INVALIDARG;
            return GetMenuItemCount(nested)?S_OK:insert(nested,0,first+17,L"Owned native nested choice");
        }
        return E_INVALIDARG;
    }
    HRESULT STDMETHODCALLTYPE HandleMenuMsg(UINT message,WPARAM raw,LPARAM packed) override {++message2Calls;return this->message(message,raw,packed);}
    HRESULT STDMETHODCALLTYPE HandleMenuMsg2(UINT message,WPARAM raw,LPARAM packed,LRESULT* result) override {
        ++message3Calls;if(!result)return E_POINTER;*result=0;return this->message(message,raw,packed);
    }
};

class PlanningSelection final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IShellItemArray> {
public:
    ComPtr<PlanningMenu> provider=Microsoft::WRL::Make<PlanningMenu>();
    ComPtr<IShellItem> selected;
    HRESULT STDMETHODCALLTYPE BindToHandler(IBindCtx*,REFGUID handler,REFIID iid,void** output) override {
        if(!output)return E_POINTER;*output=nullptr;
        return handler==BHID_SFUIObject&&provider?provider->QueryInterface(iid,output):E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyStore(GETPROPERTYSTOREFLAGS,REFIID,void** output) override {
        if(!output)return E_POINTER;*output=nullptr;return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyDescriptionList(REFPROPERTYKEY,REFIID,void** output) override {
        if(!output)return E_POINTER;*output=nullptr;return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetAttributes(SIATTRIBFLAGS,SFGAOF mask,SFGAOF* output) override {
        if(!output)return E_POINTER;*output=0;return selected?selected->GetAttributes(mask,output):E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetCount(DWORD* output) override {if(!output)return E_POINTER;*output=1;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetItemAt(DWORD index,IShellItem** output) override {
        if(!output)return E_POINTER;*output=nullptr;
        return !index&&selected?selected.CopyTo(output):E_INVALIDARG;
    }
    HRESULT STDMETHODCALLTYPE EnumItems(IEnumShellItems** output) override {if(!output)return E_POINTER;*output=nullptr;return E_NOTIMPL;}
};

void targetedNativeMenuPlanningAndFullInspection() {
    const std::array<std::wstring_view,1> rotate{L"ROTATE90"},library{L"Windows.includeinlibrary"},left{L"rotate270"};
    auto selected=Microsoft::WRL::Make<PlanningSelection>();require(selected&&selected->provider,"Allocate owned planning provider");
    auto provider=selected->provider;provider->nestedDuplicate=true;
    explorer::NativeContextMenu menu;
    succeeded(menu.createSelectionForPlanning(nullptr,selected.Get(),nullptr,CMF_EXTENDEDVERBS),"Create exact native planning menu");
    require(!(provider->flags&CMF_SYNCCASCADEMENU)&&(provider->flags&CMF_ITEMMENU)&&(provider->flags&CMF_EXTENDEDVERBS),
            "Planning changed provider flags beyond synchronous cascade population");
    std::vector<explorer::ContextMenuEntry> entries;
    succeeded(menu.enumerateForVerbs(rotate,entries),"Inspect actual known leaf without populating unrelated failing branch");
    require(entries.size()==4&&provider->rootMessages==0&&provider->unrelatedMessages==0&&provider->libraryMessages==0&&provider->containerMessages==0,
            "Known leaf planning initialized a root or unrelated native cascade");
    auto facts=selectedFile(L"C:\\owned\\image.bmp");facts.images=true;NamespaceInvocationPlan plan;
    succeeded(explorer::planNamespaceAction(NamespaceAction::RotateRight,facts,entries,{},&plan),"Preserve shallow native canonical authority");
    require(plan.commandId==provider->first+2&&plan.checked&&plan.enabled&&!plan.submenu,"Leaf default/state/ordinal or shallow precedence changed");
    succeeded(menu.enumerateForVerbs(library,entries),"Populate only requested exact native cascade and its descendants");
    require(provider->libraryMessages==1&&provider->nestedMessages==1&&provider->unrelatedMessages==0&&provider->rootMessages==0&&provider->containerMessages==0,
            "Requested native branch populated an unrelated popup or repeated a descendant");
    require(entries[2].children.size()==3&&entries[2].children[0].id==provider->first+11&&
            (entries[2].children[0].state&(MFS_DEFAULT|MFS_CHECKED))==(MFS_DEFAULT|MFS_CHECKED)&&
            !entries[2].children[1].enabled()&&entries[2].children[2].children.size()==1&&
            entries[2].children[2].children[0].id==provider->first+17,"Native child default/disabled/hierarchy/ordinal changed");
    succeeded(menu.enumerateForVerbs(library,entries),"Reuse already initialized requested native cascade");
    require(provider->libraryMessages==1&&provider->nestedMessages==1,"Exact branch query retried native delayed population");
    provider->failUnrelated=false;
    succeeded(menu.enumerate(entries),"Explicit complete native menu inspection remains available");
    require(provider->rootMessages==1&&provider->unrelatedMessages==1&&provider->containerMessages==1&&entries[1].children.size()==1,
            "Explicit full inspection returned a partial planning snapshot");
    require(provider->invocations==0,"Read-only native planning activated a provider command");

    provider->nestedDuplicate=false;provider->disabledContainer=true;provider->disabledLibrary=true;
    succeeded(menu.createSelectionForPlanning(nullptr,selected.Get()),"Create native disabled ancestor/cascade fixture");
    succeeded(menu.enumerateForVerbs(left,entries),"Read nested native leaf below disabled ancestor");
    succeeded(explorer::planNamespaceAction(NamespaceAction::RotateLeft,facts,entries,{},&plan),"Preserve disabled ancestor authority");
    require(!plan.enabled&&provider->containerMessages==0&&provider->unrelatedMessages==0,"Nested leaf bypassed native disabled ancestor or initialized it");
    succeeded(menu.enumerateForVerbs(library,entries),"Read disabled exact cascade without activating it");
    require(provider->libraryMessages==0&&entries[2].children.empty(),"Disabled native cascade was initialized");

    provider->disabledContainer=provider->disabledLibrary=false;provider->ambiguousLibrary=true;
    succeeded(menu.createSelectionForPlanning(nullptr,selected.Get()),"Create native ambiguous cascade fixture");
    require(menu.enumerateForVerbs(library,entries)==E_UNEXPECTED&&entries.empty()&&provider->libraryMessages==0,
            "Ambiguous canonical branch was initialized or accepted");
    provider->ambiguousLibrary=false;provider->libraryStatus=HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    succeeded(menu.createSelectionForPlanning(nullptr,selected.Get()),"Create native failing requested cascade fixture");
    require(menu.enumerateForVerbs(library,entries)==provider->libraryStatus&&entries.empty()&&provider->libraryMessages==1&&provider->unrelatedMessages==0,
            "Requested native failure was hidden or unrelated native branch initialized");
    require(menu.enumerateForVerbs(library,entries)==provider->libraryStatus&&provider->libraryMessages==1,
            "Failed requested native cascade was retried");
    const std::array<std::wstring_view,1> malformed{std::wstring_view(L"owned\0bad",9)};
    require(menu.enumerateForVerbs(malformed,entries)==E_INVALIDARG&&entries.empty(),"Embedded-NUL canonical alias was accepted");
    const std::array<std::wstring_view,1> unknown{L"label-only-not-a-native-verb"};
    succeeded(menu.enumerateForVerbs(unknown,entries),"Unknown canonical alias stays unpopulated");
    require(provider->unrelatedMessages==0&&provider->rootMessages==0,"Unknown canonical alias guessed an unrelated branch");

    for(const auto nativeStatus:{E_NOTIMPL,S_FALSE}) {
        provider->libraryStatus=nativeStatus;
        succeeded(menu.createSelectionForPlanning(nullptr,selected.Get()),"Create strict native message-status fixture");
        const auto expected=nativeStatus==S_FALSE?E_UNEXPECTED:nativeStatus;
        require(menu.enumerateForVerbs(library,entries)==expected&&entries.empty()&&provider->libraryMessages==1&&
                provider->message3Calls==1&&provider->message2Calls==0,
                "Native CM3 failure/status was hidden by an alternate CM2 dispatch");
        require(menu.enumerateForVerbs(library,entries)==expected&&entries.empty()&&provider->libraryMessages==1&&
                provider->message3Calls==1&&provider->message2Calls==0,
                "Failed native message admission was retried or published");
    }

    provider->libraryStatus=S_OK;
    succeeded(menu.createSelectionForPlanning(nullptr,selected.Get()),"Create reentrant native requested branch fixture");
    provider->duringLibrary=[&]{menu.reset();};
    require(menu.enumerateForVerbs(library,entries)==E_ABORT&&entries.empty(),"Replaced native menu published a stale planned branch");
    provider->duringLibrary={};

    succeeded(menu.createSelectionForPlanning(nullptr,selected.Get()),"Create reentrant same-branch attempt fixture");
    HRESULT recursive=S_OK;
    provider->duringLibrary=[&]{std::vector<explorer::ContextMenuEntry> ignored;recursive=menu.enumerateForVerbs(library,ignored);};
    succeeded(menu.enumerateForVerbs(library,entries),"Complete one original native cascade attempt after reentrant query");
    require(recursive==E_PENDING&&provider->libraryMessages==1&&provider->nestedMessages==1,
            "Reentrant planning repeated a native popup initialization");
    provider->duringLibrary={};

    Fixture fixture;auto folder=item(fixture.root);selected->selected=item(fixture.image);
    explorer::NativeNamespaceActions actions;
    succeeded(actions.initialize(nullptr,{folder,selected,{}}),"Initialize exact planner with owned native item and mock menu provider");
    provider->failUnrelated=true;
    const auto beforeQueries=provider->queries;
    succeeded(actions.planInvocation(NamespaceAction::RotateRight,&plan),"Namespace leaf plan avoids unrelated failing native cascade");
    require(provider->rootMessages==0&&provider->unrelatedMessages==0&&provider->queries==beforeQueries+1&&plan.commandId==provider->first+2,
            "Namespace exact planning used a fully populated or unrelated native menu");
    provider->failUnrelated=false;
    succeeded(actions.selectionEntries(entries),"Explicit namespace full inspection upgrades its partial retained snapshot");
    require(provider->rootMessages==1&&provider->unrelatedMessages==1&&entries[1].children.size()==1,
            "Partial namespace cache was incorrectly marked fully populated");
    const auto allMessages=provider->rootMessages+provider->unrelatedMessages+provider->libraryMessages+provider->nestedMessages+provider->containerMessages;
    succeeded(actions.selectionEntries(entries),"Reuse actual completed full namespace snapshot");
    succeeded(actions.planInvocation(NamespaceAction::RotateRight,&plan),"Full namespace snapshot preserves later leaf planning");
    require(allMessages==provider->rootMessages+provider->unrelatedMessages+provider->libraryMessages+provider->nestedMessages+provider->containerMessages&&provider->invocations==0,
            "Full-cache lookup repeated menu initialization or invoked a provider");
    actions.reset();selected->selected=folder;provider->libraryStatus=HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    succeeded(actions.initialize(nullptr,{folder,selected,{}}),"Initialize owned folder with failing native requested cascade");
    require(actions.planInvocation(NamespaceAction::IncludeInLibrary,&plan)==provider->libraryStatus&&provider->libraryMessages==1&&provider->unrelatedMessages==0,
            "Registered fallback hid an authoritative requested native cascade failure");
    require(actions.planInvocation(NamespaceAction::IncludeInLibrary,&plan)==provider->libraryStatus&&provider->libraryMessages==1,
            "Namespace planner retried an already failed native branch");
}

void anonymousNativeBranchPlanning() {
    auto provider=Microsoft::WRL::Make<PlanningMenu>();require(provider!=nullptr,"Allocate anonymous native branch provider");
    provider->anonymousLibrary=true;
    explorer::NativeContextMenu menu;
    std::vector<explorer::ContextMenuEntry> entries;
    succeeded(menu.createForPlanning(nullptr,provider.Get(),nullptr,CMF_EXTENDEDVERBS),"Create isolated anonymous native planning menu");
    require(!(provider->flags&CMF_SYNCCASCADEMENU)&&(provider->flags&CMF_EXTENDEDVERBS),"Anonymous planning changed unrelated QueryContextMenu flags");
    succeeded(menu.enumerate(entries,false),"Read original anonymous parent state without any popup notification");
    require(entries.size()==4&&entries[2].id==provider->first+10&&entries[2].canonicalVerb.empty()&&entries[2].submenu&&
            entries[2].children.empty()&&provider->message3Calls==0&&provider->message2Calls==0,"Anonymous native snapshot invented authority or initialized a popup");
    const auto actualId=entries[2].id;
    succeeded(menu.enumerateForCommand(actualId,entries),"Populate only the actual anonymous provider parent ordinal");
    require(provider->rootMessages==0&&provider->unrelatedMessages==0&&provider->containerMessages==0&&provider->libraryMessages==1&&
            provider->nestedMessages==1&&entries[2].children.size()==3&&entries[2].children[0].id==provider->first+11&&
            (entries[2].children[0].state&(MFS_DEFAULT|MFS_CHECKED))==(MFS_DEFAULT|MFS_CHECKED)&&!entries[2].children[1].enabled()&&
            entries[2].children[2].children.size()==1&&entries[2].children[2].children[0].id==provider->first+17,
            "Anonymous branch lost exact native positions, children/default/state or initialized the artificial root");
    succeeded(menu.enumerateForCommand(actualId,entries),"Reuse completed exact anonymous branch");
    require(provider->libraryMessages==1&&provider->nestedMessages==1,"Anonymous branch population was retried");
    provider->disabledLibrary=true;
    succeeded(menu.createForPlanning(nullptr,provider.Get()),"Create actual disabled anonymous parent");
    succeeded(menu.enumerateForCommand(provider->first+10,entries),"Preserve disabled native parent without notification");
    require(!entries[2].enabled()&&entries[2].children.empty()&&provider->message3Calls==0&&provider->message2Calls==0,
            "Disabled anonymous parent was populated or replaced by invented enabled state");
    provider->disabledLibrary=false;provider->duplicateLibraryId=true;
    succeeded(menu.createForPlanning(nullptr,provider.Get()),"Create ambiguous actual native ordinal");
    require(menu.enumerateForCommand(provider->first+10,entries)==E_UNEXPECTED&&entries.empty()&&provider->message3Calls==0,
            "Duplicate anonymous ordinal was accepted or initialized");
    provider->duplicateLibraryId=false;
    succeeded(menu.createForPlanning(nullptr,provider.Get()),"Create exact native ordinal validation fixture");
    require(menu.enumerateForCommand(0,entries)==E_INVALIDARG&&entries.empty()&&
            menu.enumerateForCommand(0x8000,entries)==E_INVALIDARG&&entries.empty()&&
            menu.enumerateForCommand(provider->first+23,entries)==HRESULT_FROM_WIN32(ERROR_NOT_FOUND)&&entries.empty()&&provider->message3Calls==0,
            "Missing or invalid anonymous ordinal guessed an unrelated submenu");
    for(const auto nativeStatus:{E_NOTIMPL,S_FALSE}) {
        provider->libraryStatus=nativeStatus;
        succeeded(menu.createForPlanning(nullptr,provider.Get()),"Create anonymous native notification failure fixture");
        const auto expected=nativeStatus==S_FALSE?E_UNEXPECTED:nativeStatus;
        require(menu.enumerateForCommand(provider->first+10,entries)==expected&&entries.empty()&&provider->message3Calls==1&&provider->message2Calls==0&&
                menu.enumerateForCommand(provider->first+10,entries)==expected&&entries.empty()&&provider->message3Calls==1,
                "Anonymous native notification failure was hidden, retried or published");
    }
    provider->libraryStatus=S_OK;
    succeeded(menu.createForPlanning(nullptr,provider.Get()),"Create anonymous native same-branch reentry fixture");
    HRESULT recursive=S_OK;
    provider->duringLibrary=[&]{std::vector<explorer::ContextMenuEntry> ignored;recursive=menu.enumerateForCommand(provider->first+10,ignored);};
    succeeded(menu.enumerateForCommand(provider->first+10,entries),"Complete original anonymous branch after recursive inspection");
    require(recursive==E_PENDING&&provider->libraryMessages==1&&provider->nestedMessages==1&&provider->invocations==0,
            "Anonymous native reentry retried initialization or activated a provider");
    provider->duringLibrary={};
    succeeded(menu.createForPlanning(nullptr,provider.Get()),"Create anonymous native replacement fixture");
    provider->duringLibrary=[&]{menu.reset();};
    require(menu.enumerateForCommand(provider->first+10,entries)==E_ABORT&&entries.empty(),"Retired anonymous native menu published a stale branch");
    provider->duringLibrary={};
}

void nativeCastParentPlanningAndOwnerControls() {
    Fixture fixture;
    const auto originalBytes=read(fixture.image),originalText=read(fixture.text);
    const auto before=castFileSnapshot(fixture.image);
    auto folder=item(fixture.root),image=item(fixture.image);auto selection=array(image.Get());
    ComPtr<IDataObject> data;succeeded(selection->BindToHandler(nullptr,BHID_DataObject,IID_PPV_ARGS(&data)),"Bind original owned bitmap Cast data object");
    require(data!=nullptr,"Native Cast data object returned no interface");verifyCastCida(data.Get(),before);
    struct Key {HKEY value=nullptr;~Key(){if(value)RegCloseKey(value);}}key;
    require(RegOpenKeyExW(HKEY_CLASSES_ROOT,L"SystemFileAssociations\\image",0,KEY_READ,&key.value)==ERROR_SUCCESS,"Open exact image association for native Cast parent controls");
    std::array<wchar_t,128> registration{};DWORD bytes=static_cast<DWORD>(sizeof(registration));
    require(RegGetValueW(key.value,L"shellex\\ContextMenuHandlers\\PlayTo",nullptr,RRF_RT_REG_SZ,nullptr,registration.data(),&bytes)==ERROR_SUCCESS&&
            bytes>=2*sizeof(wchar_t)&&bytes<=sizeof(registration)&&bytes%sizeof(wchar_t)==0&&registration[bytes/sizeof(wchar_t)-1]==0,
            "Read exact bounded registered image Cast CLSID");
    CLSID identifier{};succeeded(CLSIDFromString(registration.data(),&identifier),"Parse actual registered image Cast CLSID");
    NativeBackgroundView host;host.initialize(folder.Get());
    struct Pidl {PIDLIST_ABSOLUTE value=nullptr;~Pidl(){CoTaskMemFree(value);}}location;
    const auto view=host.view;succeeded(SHGetIDListFromObject(folder.Get(),&location.value),"Retain actual native Cast control location");
    require(location.value!=nullptr,"Native Cast control location has no PIDL");
    struct ArmReadback {
        HRESULT factory=E_PENDING,initializationInterface=E_PENDING,initialization=E_PENDING,query=E_PENDING,snapshot=E_PENDING;
        HRESULT parentStatus=E_PENDING,branch=E_PENDING;
        UINT commandCount=0,id=0,state=0;
        size_t rows=0,parents=0,children=0;
        bool contextPresent=false,initializerPresent=false,submenu=false,enabled=false,rawMatches=false,branchAttempted=false;
    };
    std::array<ArmReadback,3> arms{};
    for(unsigned arm=0;arm<3;++arm) {
        auto& observed=arms[arm];
        const auto owner=arm?host.owner:nullptr;IUnknown* site=arm==2?view.Get():nullptr;
        ComPtr<IContextMenu> context;
        observed.factory=CoCreateInstance(identifier,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&context));observed.contextPresent=context!=nullptr;
        ComPtr<IShellExtInit> initialize;
        if(SUCCEEDED(observed.factory)&&context)observed.initializationInterface=context.As(&initialize);
        observed.initializerPresent=initialize!=nullptr;
        if(SUCCEEDED(observed.initializationInterface)&&initialize)
            observed.initialization=initialize->Initialize(nullptr,data.Get(),key.value);
        explorer::NativeContextMenu menu;
        if(SUCCEEDED(observed.initialization))observed.query=menu.create(owner,context.Get(),site,CMF_NORMAL);
        observed.commandCount=menu.commandCount();
        std::vector<explorer::ContextMenuEntry> entries;
        if(SUCCEEDED(observed.query))observed.snapshot=menu.enumerate(entries,false);
        observed.rows=entries.size();
        const explorer::ContextMenuEntry* parent=nullptr;
        for(const auto& row:entries)if(!row.separator()){++observed.parents;if(!parent)parent=&row;}
        if(SUCCEEDED(observed.snapshot)) {
            if(!observed.parents)observed.parentStatus=HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            else if(observed.parents!=1)observed.parentStatus=E_UNEXPECTED;
            else if(!parent->id||parent->id>0x7fff)observed.parentStatus=HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            else {
                observed.parentStatus=S_OK;observed.id=parent->id;observed.state=parent->state;
                observed.submenu=parent->submenu;observed.enabled=parent->enabled();observed.children=parent->children.size();
                MENUITEMINFOW raw{sizeof(raw)};raw.fMask=MIIM_ID|MIIM_STATE|MIIM_SUBMENU;
                observed.rawMatches=GetMenuItemInfoW(menu.menu(),observed.id,FALSE,&raw)&&raw.wID==observed.id&&
                    raw.fState==observed.state&&(raw.hSubMenu!=nullptr)==observed.submenu;
            }
        }
        std::cout<<"CastPlanning arm="<<arm<<" owner="<<reinterpret_cast<uintptr_t>(owner)<<" site="<<(site!=nullptr)
                 <<" queryFlags="<<(CMF_NORMAL|CMF_SYNCCASCADEMENU)
                 <<" factory="<<static_cast<unsigned long>(observed.factory)<<" initializer="<<static_cast<unsigned long>(observed.initializationInterface)
                 <<" initialize="<<static_cast<unsigned long>(observed.initialization)<<" query="<<static_cast<unsigned long>(observed.query)
                 <<" snapshot="<<static_cast<unsigned long>(observed.snapshot)<<" parentHRESULT="<<static_cast<unsigned long>(observed.parentStatus)
                 <<" nativeCommands="<<observed.commandCount<<" rows="<<observed.rows<<" parents="<<observed.parents
                 <<" nativeId="<<observed.id<<" state="<<observed.state<<" submenu="<<observed.submenu<<" children="<<observed.children<<" rootWM_INIT=0\n"<<std::flush;
        // Unsited/missing-owner arms are read-only metadata controls. Only the
        // actual owned view/site arm requests the enabled provider subgroup.
        if(arm==2&&observed.parentStatus==S_OK&&observed.enabled&&observed.submenu) {
            observed.branchAttempted=true;observed.branch=menu.enumerateForCommand(observed.id,entries);
            if(observed.branch==S_OK) {
                size_t parents=0;parent=nullptr;for(const auto& row:entries)if(!row.separator()){++parents;if(!parent)parent=&row;}
                if(parents!=1||!parent||parent->id!=observed.id)observed.branch=E_ABORT;
            }
            std::cout<<"CastPlanning arm="<<arm<<" branchHRESULT="<<static_cast<unsigned long>(observed.branch)<<'\n'<<std::flush;
        }
        // Teardown can also call the native site. Observe original view/source
        // only after this arm's original menu has detached its site.
        menu.reset();
        ComPtr<IShellView> actualView;succeeded(host.browser->GetCurrentView(IID_PPV_ARGS(&actualView)),"Read original native view after Cast control snapshot");
        ComPtr<IFolderView2> folderView;succeeded(actualView.As(&folderView),"Read original Cast control folder view");
        ComPtr<IShellItem> actualFolder;succeeded(folderView->GetFolder(IID_PPV_ARGS(&actualFolder)),"Read actual Cast control location after native callback");
        Pidl actualLocation;succeeded(SHGetIDListFromObject(actualFolder.Get(),&actualLocation.value),"Read current native Cast control PIDL");
        require(actualView.Get()==view.Get()&&actualLocation.value&&ILIsEqual(location.value,actualLocation.value)&&!IsWindowVisible(host.owner),
                "Read-only Cast parent controls changed original view/location or displayed their owner");
        verifyCastCida(data.Get(),before);
    }
    // Every original arm has now been captured under the same QueryContextMenu
    // flags. Native absence is capability evidence, never an enabled parent or
    // proof that a requested enabled Cast subgroup was covered.
    for(size_t arm=0;arm<arms.size();++arm) {
        const auto& observed=arms[arm];
        succeeded(observed.factory,"Read actual original Cast control factory result");require(observed.contextPresent,"Native Cast control factory returned no interface");
        succeeded(observed.initializationInterface,"Read actual original Cast control initialization interface");require(observed.initializerPresent,"Native Cast initializer interface is absent");
        succeeded(observed.initialization,"Read original Cast control initialization result");
        succeeded(observed.query,"Read original Cast control QueryContextMenu result");
        succeeded(observed.snapshot,"Read original unpopulated Cast control snapshot result");
        if(observed.parentStatus==HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)) {
            require(observed.parents==0&&observed.commandCount==0&&observed.id==0&&!observed.branchAttempted,
                    "Absent actual Cast parent was fabricated or initialized");
            std::cout<<"COVERED: Cast native capability absence arm="<<arm<<" parentHRESULT="<<static_cast<unsigned long>(observed.parentStatus)
                     <<"; enabled native subgroup NOT COVERED\n"<<std::flush;
        } else {
            succeeded(observed.parentStatus,"Read exact original Cast control parent authority");
            require(observed.parents==1&&observed.rawMatches,"Original Cast parent lost unique actual native ordinal/state");
            if(observed.branchAttempted)succeeded(observed.branch,"Read actual enabled Cast subgroup result without rescue");
            else require(arm!=2||!observed.enabled||!observed.submenu,"Enabled actual native site subgroup was skipped");
        }
    }
    const auto after=castFileSnapshot(fixture.image);
    require(sameCastFile(before,after)&&before.basic.CreationTime.QuadPart==after.basic.CreationTime.QuadPart&&
            before.basic.LastWriteTime.QuadPart==after.basic.LastWriteTime.QuadPart&&before.basic.ChangeTime.QuadPart==after.basic.ChangeTime.QuadPart&&
            before.basic.FileAttributes==after.basic.FileAttributes&&before.standard.EndOfFile.QuadPart==after.standard.EndOfFile.QuadPart&&
            read(fixture.image)==originalBytes&&read(fixture.text)==originalText,"Native Cast parent controls changed exact owned source identity, metadata or bytes");
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

// This custom namespace publishes only its documented aggregate property
// authority. Standard GIT/FTM marshaling must retain it instead of replacing
// it with a filesystem/CIDA selection or asking for individual items.
class KindAggregateSelection final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IShellItemArray,IPropertyStore,Microsoft::WRL::FtmBase> {
public:
    std::atomic<DWORD> count{100001},propertyThread{0},requested{GPS_DEFAULT};
    std::atomic<unsigned> stores{0},values{0},itemReads{0},binds{0};
    std::atomic<bool> entered{false},hold{false};
    HRESULT countStatus=S_OK,storeStatus=S_OK,valueStatus=S_OK;
    bool malformed=false,changeCount=false;
    std::vector<std::wstring> kinds{L"Music",L"audio"};
    HANDLE release=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    ~KindAggregateSelection(){if(release)CloseHandle(release);}
    HRESULT STDMETHODCALLTYPE BindToHandler(IBindCtx*,REFGUID,REFIID,void** output) override {
        ++binds;if(!output)return E_POINTER;*output=nullptr;return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyStore(GETPROPERTYSTOREFLAGS flags,REFIID iid,void** output) override {
        ++stores;requested=static_cast<DWORD>(flags);
        if(!output)return E_POINTER;*output=nullptr;
        return FAILED(storeStatus)?storeStatus:QueryInterface(iid,output);
    }
    HRESULT STDMETHODCALLTYPE GetPropertyDescriptionList(REFPROPERTYKEY,REFIID,void** output) override {
        if(!output)return E_POINTER;*output=nullptr;return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetAttributes(SIATTRIBFLAGS flags,SFGAOF mask,SFGAOF* output) override {
        if(!output)return E_POINTER;
        if(flags!=static_cast<SIATTRIBFLAGS>(SIATTRIBFLAGS_AND|SIATTRIBFLAGS_ALLITEMS))return E_INVALIDARG;
        *output=mask&SFGAO_FILESYSTEM;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCount(DWORD* output) override {
        if(!output)return E_POINTER;if(FAILED(countStatus))return countStatus;
        *output=count.load();return countStatus;
    }
    HRESULT STDMETHODCALLTYPE GetItemAt(DWORD,IShellItem** output) override {
        ++itemReads;if(!output)return E_POINTER;*output=nullptr;return E_UNEXPECTED;
    }
    HRESULT STDMETHODCALLTYPE EnumItems(IEnumShellItems** output) override {
        if(!output)return E_POINTER;*output=nullptr;return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetAt(DWORD index,PROPERTYKEY* output) override {
        if(!output)return E_POINTER;if(index)return E_INVALIDARG;*output=PKEY_Kind;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetValue(REFPROPERTYKEY key,PROPVARIANT* output) override {
        ++values;propertyThread=GetCurrentThreadId();entered=true;
        if(hold.load()) {
            const auto waited=WaitForSingleObject(release,3000);
            if(waited!=WAIT_OBJECT_0)return waited==WAIT_TIMEOUT?HRESULT_FROM_WIN32(ERROR_TIMEOUT):HRESULT_FROM_WIN32(GetLastError());
        }
        if(!output)return E_POINTER;if(FAILED(valueStatus))return valueStatus;
        if(!IsEqualPropertyKey(key,PKEY_Kind))return E_INVALIDARG;
        if(changeCount)count=100000;
        if(malformed){output->vt=VT_I4;output->lVal=1;return S_OK;}
        if(kinds.empty()){output->vt=VT_EMPTY;return S_OK;}
        std::vector<const wchar_t*> names;for(const auto& kind:kinds)names.push_back(kind.c_str());
        return InitPropVariantFromStringVector(names.data(),static_cast<ULONG>(names.size()),output);
    }
    HRESULT STDMETHODCALLTYPE SetValue(REFPROPERTYKEY,REFPROPVARIANT) override {return E_ACCESSDENIED;}
    HRESULT STDMETHODCALLTYPE Commit() override {return E_ACCESSDENIED;}
};

bool sameKinds(const explorer::NamespaceSelectionKinds& left,const explorer::NamespaceSelectionKinds& right) {
    return left.count==right.count&&left.music==right.music&&left.video==right.video;
}

void asyncCustomKindAuthorityAndFailures() {
    Fixture fixture;const auto folder=item(fixture.root);
    const auto textBefore=read(fixture.text),imageBefore=read(fixture.image);
    const auto clipboard=GetClipboardSequenceNumber();
    const auto selected=Microsoft::WRL::Make<KindAggregateSelection>();
    require(selected&&selected->release,"Create bounded custom aggregate and its cancellation gate");
    explorer::NativeNamespaceActions actions;
    succeeded(actions.initialize(nullptr,{folder,selected,folder}),"Retain full original custom aggregate and exact site");
    struct Drain {
        KindAggregateSelection* selected;
        ~Drain(){SetEvent(selected->release);drainNamespaceWorkers("custom aggregate before retained namespace teardown");}
    }drain{selected.Get()};
    std::unique_ptr<explorer::NamespaceCommandStateTask> task;
    const auto start=[&] {
        task.reset();selected->entered=false;
        succeeded(actions.startSelectionKindsTask(&task),"Start original aggregate Kind independently of native command menus");
        require(task!=nullptr,"Kind start succeeded without a native task");
    };
    const auto finish=[&](explorer::NamespaceSelectionKinds* output) {
        require(pumpPrivateNamespaceUntil([&]{return task->completed();},4000),"Aggregate Kind worker exceeded its bounded completion wait");
        return task->pollSelectionKinds(output);
    };
    start();explorer::NamespaceSelectionKinds actual;
    require(finish(&actual)==S_OK&&actual.count==100001&&actual.music&&!actual.video,
            "Original 100001-item custom aggregate Kind was replaced, capped or lost");
    require(selected->stores.load()==1&&selected->values.load()==1&&selected->itemReads.load()==0&&selected->binds.load()==0&&
            selected->requested.load()==static_cast<DWORD>(GPS_FASTPROPERTIESONLY|GPS_BESTEFFORT)&&
            selected->propertyThread.load()!=GetCurrentThreadId(),
            "Kind worker exported CIDA, enumerated items, changed fast flags or ran its agile aggregate on the creator");
    explorer::NamespaceCommandState wrong;wrong.state=ECS_CHECKED;wrong.selectionCount=17;
    std::vector<explorer::NamespaceSelectionVerbState> wrongBatch(1);wrongBatch.front().verb=L"unchanged";
    require(task->poll(&wrong)==E_INVALIDARG&&wrong.state==ECS_CHECKED&&wrong.selectionCount==17&&
            task->pollSelectionVerbBatch(&wrongBatch)==E_INVALIDARG&&wrongBatch.front().verb==L"unchanged"&&
            task->pollSelectionKinds(nullptr)==E_POINTER,"Kind task accepted another result mode or changed failure output");
    const auto retainedTask=task.get();
    require(explorer::NamespaceCommandStateTask::startSelectionKinds(nullptr,folder.Get(),&task)==E_POINTER&&
            task.get()==retainedTask,"Invalid Kind start replaced the existing actual task");
    explorer::NamespaceCommandStateTimings timings;
    succeeded(task->pollTimings(&timings),"Read actual Kind worker phase timing");
    require(timings.workerMicroseconds>=timings.kindReadMicroseconds&&timings.menuQueryMicroseconds==0&&
            timings.dataObjectExportMicroseconds==0,"Kind task ran a native menu or identity export phase");
    const explorer::NamespaceSelectionKinds sentinel{47,true,true};
    HRESULT foreignRead=S_OK;auto foreignOutput=sentinel;
    std::thread foreign([&]{foreignRead=task->pollSelectionKinds(&foreignOutput);});foreign.join();
    require(foreignRead==RPC_E_WRONG_THREAD&&sameKinds(foreignOutput,sentinel),
            "Kind result crossed its creator apartment or changed a rejected foreign-thread output");
    for(const auto failure:{E_PENDING,E_ACCESSDENIED}) {
        selected->valueStatus=failure;start();actual=sentinel;
        require(finish(&actual)==failure&&task->completed()&&sameKinds(actual,sentinel),
                "Completed native Kind failure stayed falsely pending or changed prior output");
    }
    selected->valueStatus=S_OK;selected->malformed=true;start();actual=sentinel;
    require(finish(&actual)==HRESULT_FROM_WIN32(ERROR_INVALID_DATA)&&sameKinds(actual,sentinel),
            "Malformed original aggregate Kind invented contextual state");
    selected->malformed=false;selected->changeCount=true;start();actual=sentinel;
    require(finish(&actual)==HRESULT_FROM_WIN32(ERROR_RETRY)&&sameKinds(actual,sentinel),
            "Kind worker published a selection whose native count changed during its property read");
    selected->changeCount=false;selected->count=100001;selected->countStatus=E_ACCESSDENIED;
    start();actual=sentinel;
    require(finish(&actual)==E_ACCESSDENIED&&sameKinds(actual,sentinel),"Failed native count was mistaken for an empty selection");
    selected->countStatus=S_OK;selected->count=0;
    const auto storesBefore=selected->stores.load();start();actual=sentinel;
    require(finish(&actual)==S_OK&&sameKinds(actual,{})&&selected->stores.load()==storesBefore,
            "Actual known-zero selection retained media context or accessed properties");
    selected->count=100001;selected->hold=true;
    require(ResetEvent(selected->release)!=FALSE,"Arm only the owned aggregate worker gate");start();
    require(pumpPrivateNamespaceUntil([&]{return selected->entered.load();},3000),"Actual original aggregate did not enter its bounded worker read");
    actual=sentinel;
    require(!task->completed()&&task->pollSelectionKinds(&actual)==E_PENDING&&sameKinds(actual,sentinel),
            "Unfinished original aggregate read reported completed native state");
    task->cancel();
    require(task->pollSelectionKinds(&actual)==HRESULT_FROM_WIN32(ERROR_CANCELLED)&&sameKinds(actual,sentinel),
            "Cancelled Kind worker published its original pending property read");
    require(SetEvent(selected->release)!=FALSE,"Release the original cancelled aggregate call");
    require(pumpPrivateNamespaceUntil([&]{return task->completed();},3000),"Cancelled Kind worker did not release its actual native lease");
    selected->hold=false;task.reset();drainNamespaceWorkers("join cancelled aggregate Kind before facade reuse");
    succeeded(actions.initialize(nullptr,{folder,{},folder}),"Retain real folder fallback with no selected items");
    require(actions.startSelectionKindsTask(&task)==E_INVALIDARG&&!task,
            "Kind facade classified the current-folder fallback as a selected item");
    actions.reset();
    require(selected->itemReads.load()==0&&selected->binds.load()==0&&read(fixture.text)==textBefore&&
            read(fixture.image)==imageBefore&&GetClipboardSequenceNumber()==clipboard,
            "Custom Kind classification replaced its original authority or changed owned files/clipboard");
}

void asyncNativeKindAndOriginalView() {
    Fixture fixture;const auto source=fixture.root/L"owned music.mp3";
    std::ofstream(source,std::ios::binary)<<"owned fast Kind fixture; never decoded, played or invoked";
    const auto sourceBefore=castFileSnapshot(source),textBefore=castFileSnapshot(fixture.text);
    const auto sourceBytes=read(source),textBytes=read(fixture.text);
    const auto clipboard=GetClipboardSequenceNumber();
    const auto folder=item(fixture.root),music=item(source),text=item(fixture.text);
    NativeBackgroundView host;
    host.initialize(folder.Get(),{fixture.image,fixture.program,fixture.disc,fixture.text,source},music.Get());
    NamespaceDrainGuard drain;
    ComPtr<IFolderView2> view;succeeded(host.view.As(&view),"Retain original native Kind selection view");
    struct PidlDeleter {
        using pointer=LPITEMIDLIST;
        void operator()(pointer value) const noexcept {CoTaskMemFree(value);}
    };
    using Pidl=std::unique_ptr<ITEMIDLIST,PidlDeleter>;
    PIDLIST_ABSOLUTE rawMusic=nullptr,rawText=nullptr,rawFolder=nullptr;
    succeeded(SHGetIDListFromObject(music.Get(),&rawMusic),"Read exact owned Music PIDL");Pidl musicId(rawMusic);
    succeeded(SHGetIDListFromObject(text.Get(),&rawText),"Read exact owned nonmedia PIDL");Pidl textId(rawText);
    succeeded(SHGetIDListFromObject(folder.Get(),&rawFolder),"Retain exact native folder PIDL");Pidl folderId(rawFolder);
    host.selectionDiagnostic("music-before-original-selection");
    const auto selectionDeadline=GetTickCount64()+2000;
    const auto nativeIndex=host.ownedNativeIndex(musicId.get(),selectionDeadline,"music");
    require(GetTickCount64()<selectionDeadline&&host.sourceCurrent(view.Get())&&GetTickCount64()<selectionDeadline,"Music indexed selection source/deadline expired before its single request");
    const auto selectStatus=view->SelectItem(nativeIndex,SVSI_SELECT|SVSI_DESELECTOTHERS|SVSI_NOTAKEFOCUS);
    std::cout<<"NativeBackground selection action=music HRESULT="<<static_cast<unsigned long>(selectStatus)<<" requests=1\n"<<std::flush;
    succeeded(selectStatus,
              "Select exactly the owned native Music identity");
    const auto selectionWaitStart=GetTickCount64();
    const bool selectionReady=selectionWaitStart<selectionDeadline&&pumpPrivateNamespaceUntil([&]{int count=-1;return GetTickCount64()<selectionDeadline&&host.sourceCurrent(view.Get())&&view->ItemCount(SVGIO_SELECTION,&count)==S_OK&&count==1&&host.sourceCurrent(view.Get())&&GetTickCount64()<selectionDeadline;},static_cast<DWORD>(selectionDeadline-selectionWaitStart),selectionDeadline);
    host.selectionDiagnostic("music-after-original-selection");
    require(selectionReady,
            "Original native Music selection was not ready");
    ComPtr<IShellItemArray> selected;succeeded(view->GetSelection(FALSE,&selected),"Retain original native selected array");
    ComPtr<IDataObject> data;succeeded(selected->BindToHandler(nullptr,BHID_DataObject,IID_PPV_ARGS(&data)),"Read complete selected native CIDA without clipboard publication");
    verifyCastCida(data.Get(),sourceBefore);
    explorer::NativeNamespaceActions actions;
    succeeded(actions.initialize(host.owner,{folder,selected,host.view}),"Initialize exact original native media array and site");
    std::unique_ptr<explorer::NamespaceCommandStateTask> task;
    succeeded(actions.startSelectionKindsTask(&task),"Start native selected Kind using cached original-array registration");
    require(pumpPrivateNamespaceUntil([&]{return task->completed();},4000),"Native Kind exceeded independent readiness wait");
    explorer::NamespaceSelectionKinds actual,expected;
    succeeded(explorer::namespaceSelectionKinds(selected.Get(),&expected),"Read independent native selected Kind intersection");
    require(task->pollSelectionKinds(&actual)==S_OK&&sameKinds(actual,expected)&&actual.count==1&&actual.music&&!actual.video,
            "Worker Kind differs from the actual original native selection property store");
    task.reset();
    std::vector<PCIDLIST_ABSOLUTE> identities(258,musicId.get());identities.back()=textId.get();
    ComPtr<IShellItemArray> full;
    succeeded(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(identities.size()),identities.data(),&full),
              "Create actual full native media array with final nonmedia counterexample");
    succeeded(explorer::NamespaceCommandStateTask::startSelectionKinds(full.Get(),host.view.Get(),&task),"Start complete native large-array Kind without menu reconstruction");
    require(pumpPrivateNamespaceUntil([&]{return task->completed();},4000),"Complete native large-array Kind exceeded bounded readiness");
    succeeded(explorer::namespaceSelectionKinds(full.Get(),&expected),"Read original complete native large-array property intersection");
    require(task->pollSelectionKinds(&actual)==S_OK&&sameKinds(actual,expected)&&actual.count==258&&!actual.music&&!actual.video,
            "Async native Kind omitted its final nonmedia identity or reused stale Music context");
    task.reset();drainNamespaceWorkers("native Kind completion before original view preservation proof");
    ComPtr<IShellItemArray> after;succeeded(view->GetSelection(FALSE,&after),"Read preserved original native selected array");
    ComPtr<IDataObject> afterData;succeeded(after->BindToHandler(nullptr,BHID_DataObject,IID_PPV_ARGS(&afterData)),"Read preserved actual selected CIDA");
    verifyCastCida(afterData.Get(),sourceBefore);
    ComPtr<IShellItem> actualFolder;succeeded(view->GetFolder(IID_PPV_ARGS(&actualFolder)),"Read original native Kind folder after worker teardown");
    int order=1;succeeded(actualFolder->Compare(folder.Get(),SICHINT_CANONICAL,&order),"Compare exact original native folder identity");
    PIDLIST_ABSOLUTE rawAfterFolder=nullptr;
    succeeded(SHGetIDListFromObject(actualFolder.Get(),&rawAfterFolder),"Read actual retained native folder PIDL after Kind work");
    Pidl afterFolder(rawAfterFolder);
    ComPtr<IShellView> current;succeeded(host.browser->GetCurrentView(IID_PPV_ARGS(&current)),"Retain actual original browser view after Kind completion");
    const auto sourceAfter=castFileSnapshot(source),textAfter=castFileSnapshot(fixture.text);
    require(order==0&&afterFolder&&ILIsEqual(folderId.get(),afterFolder.get())&&current.Get()==host.view.Get()&&!IsWindowVisible(host.owner)&&sameCastFile(sourceBefore,sourceAfter)&&
            sameCastFile(textBefore,textAfter)&&sourceBefore.basic.CreationTime.QuadPart==sourceAfter.basic.CreationTime.QuadPart&&
            sourceBefore.basic.LastWriteTime.QuadPart==sourceAfter.basic.LastWriteTime.QuadPart&&
            sourceBefore.basic.FileAttributes==sourceAfter.basic.FileAttributes&&sourceBefore.standard.EndOfFile.QuadPart==sourceAfter.standard.EndOfFile.QuadPart&&
            textBefore.basic.CreationTime.QuadPart==textAfter.basic.CreationTime.QuadPart&&
            textBefore.basic.LastWriteTime.QuadPart==textAfter.basic.LastWriteTime.QuadPart&&textBefore.basic.FileAttributes==textAfter.basic.FileAttributes&&
            textBefore.standard.EndOfFile.QuadPart==textAfter.standard.EndOfFile.QuadPart&&read(source)==sourceBytes&&read(fixture.text)==textBytes&&
            GetClipboardSequenceNumber()==clipboard,"Native Kind worker changed original selection/view/source or private clipboard");
    actions.reset();
}

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

int runNamespaceCastStateTests() {
    try {
        onPrivateNamespaceDesktop(nativeVideoCastParentState);
        std::cout<<"PASS: Namespace actions: actual Video Cast parent state, association/site comparison and unchanged owned source\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<"FAIL: Namespace actions: actual Video Cast parent state: "<<error.what()<<'\n';
    } catch(...) {
        std::cerr<<"FAIL: Namespace actions: actual Video Cast parent state: unknown exception\n";
    }
    return 1;
}

int runNamespaceActionTests() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"semantic target guards and exact canonical verbs",semanticPlanningAndCanonicalVerbs},
        {"disabled ancestors, ambiguous handlers and native cascades",disabledMenusAndAmbiguousHandlers},
        {"drive and simulated Recycle Bin action planning",destructiveAndOptionalActionPlanning},
        {"sharing, application, network and media routes",targetTypesAndFeatureMatrix},
        {"exact native leaf/cascade planning, failure isolation and partial/full menu caches",targetedNativeMenuPlanningAndFullInspection},
        {"anonymous exact native-ID branch planning, disabled state, failures and reentry",anonymousNativeBranchPlanning},
        {"actual registered Cast capability absence/parent snapshots and hidden native view/site controls",[]{onPrivateNamespaceDesktop(nativeCastParentPlanningAndOwnerControls);}},
        {"native fixture capabilities, STA lifetime and headless activation guard",nativeFixtureCapabilitiesAndHeadlessGuard},
        {"Windows command resources and read-only drive enumeration",metadataAndReadOnlyDriveEnumeration},
        {"all eight native localized View gallery titles and icon resources",nativeViewGalleryResources},
        {"actual Recycle Bin Properties native background-menu state and invocation guards",[]{onPrivateNamespaceDesktop(nativeRecyclePropertiesBackgroundState);}},
        {"actual standard-GIT lookup cancellation and initialized final release",[]{onPrivateNamespaceDesktop(realGitLookupCancellationLifetime);}},
        {"exact target registration reuse, standalone ownership and native marshal reentry",[]{onPrivateNamespaceDesktop(exactTargetRegistrationReuseAndReentry);}},
        {"async original 100001-item Kind authority, native failures, cancellation and known-zero guard",[]{onPrivateNamespaceDesktop(asyncCustomKindAuthorityAndFailures);}},
        {"actual native reported path identity, available short/long aliases and wrong/replaced FileID rejection",[]{onPrivateNamespaceDesktop(nativeOwnedDisplayPathIdentityAdmission);}},
        {"async actual native Kind, full media counterexample and unchanged original view/CIDA",[]{onPrivateNamespaceDesktop(asyncNativeKindAndOriginalView);}},
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
