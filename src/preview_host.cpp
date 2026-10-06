#include "explorer/preview_host.hpp"
#include "explorer/worker_sta.hpp"
#include <shlobj.h>
#include <shobjidl.h>
#include <shlguid.h>
#include <shlwapi.h>
#include <propsys.h>
#include <wrl/client.h>
#include <process.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <mutex>
#include <vector>
#include <new>
#include <limits>

namespace explorer {
namespace {
using Microsoft::WRL::ComPtr;
constexpr wchar_t ownerProperty[] = L"WindowsExplorer.Native.PreviewHost";
HRESULT nativeError() noexcept { const auto value=GetLastError();return HRESULT_FROM_WIN32(value?value:ERROR_GEN_FAILURE); }
HRESULT exactSuccess(HRESULT value) noexcept { return value==S_OK?S_OK:FAILED(value)?value:E_FAIL; }
struct PidlDeleter { using pointer=PIDLIST_ABSOLUTE;void operator()(pointer value) const noexcept {CoTaskMemFree(value);} };
using OwnedPidl=std::unique_ptr<ITEMIDLIST,PidlDeleter>;
struct TaskText {PWSTR value=nullptr;~TaskText(){CoTaskMemFree(value);} };
struct FileHandle {
    HANDLE value=INVALID_HANDLE_VALUE;
    void reset() noexcept {if(value!=INVALID_HANDLE_VALUE){CloseHandle(value);value=INVALID_HANDLE_VALUE;}}
    ~FileHandle(){reset();}
};
bool validBounds(const RECT& rect) noexcept {
    return rect.left>=0&&rect.top>=0&&rect.right>rect.left&&rect.bottom>rect.top&&rect.right<=32768&&rect.bottom<=32768;
}
bool sameFont(const LOGFONTW& a,const LOGFONTW& b) noexcept {
    return a.lfHeight==b.lfHeight&&a.lfWidth==b.lfWidth&&a.lfEscapement==b.lfEscapement&&
        a.lfOrientation==b.lfOrientation&&a.lfWeight==b.lfWeight&&a.lfItalic==b.lfItalic&&
        a.lfUnderline==b.lfUnderline&&a.lfStrikeOut==b.lfStrikeOut&&a.lfCharSet==b.lfCharSet&&
        a.lfOutPrecision==b.lfOutPrecision&&a.lfClipPrecision==b.lfClipPrecision&&
        a.lfQuality==b.lfQuality&&a.lfPitchAndFamily==b.lfPitchAndFamily&&
        std::equal(a.lfFaceName,a.lfFaceName+LF_FACESIZE,b.lfFaceName);
}
bool sameVisuals(const PreviewVisualSuggestions& a,const PreviewVisualSuggestions& b) noexcept {
    return a.backgroundPresent==b.backgroundPresent&&a.textPresent==b.textPresent&&a.fontPresent==b.fontPresent&&
        (!a.backgroundPresent||a.background==b.background)&&(!a.textPresent||a.text==b.text)&&
        (!a.fontPresent||sameFont(a.font,b.font));
}
HRESULT copyVisuals(const PreviewVisualSuggestions& input,PreviewVisualSuggestions& result) noexcept {
    result={};result.backgroundPresent=input.backgroundPresent;result.textPresent=input.textPresent;result.fontPresent=input.fontPresent;
    if(input.backgroundPresent)result.background=input.background;
    if(input.textPresent)result.text=input.text;
    if(input.fontPresent) {
        const auto end=std::find(input.font.lfFaceName,input.font.lfFaceName+LF_FACESIZE,L'\0');
        if(end==input.font.lfFaceName+LF_FACESIZE)return E_INVALIDARG;
        result.font=input.font;
        std::fill(result.font.lfFaceName+(end-input.font.lfFaceName),result.font.lfFaceName+LF_FACESIZE,L'\0');
    }
    return S_OK;
}
HRESULT policyDword(HKEY hive,const wchar_t* key,const wchar_t* name,DWORD& value,bool& present) noexcept {
    DWORD bytes=sizeof(value);value=0;present=false;
    const auto error=RegGetValueW(hive,key,name,RRF_RT_REG_DWORD,nullptr,&value,&bytes);
    if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)return S_FALSE;
    if(error!=ERROR_SUCCESS)return HRESULT_FROM_WIN32(error);
    if(bytes!=sizeof(value))return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    present=true;return S_OK;
}
HRESULT previewAllowed(bool& allowed) noexcept {
    allowed=true;DWORD value=0;bool present=false;
    auto hr=policyDword(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",L"ShowPreviewHandlers",value,present);
    if(FAILED(hr))return hr;if(present&&!value)allowed=false;
    for(const auto hive:{HKEY_CURRENT_USER,HKEY_LOCAL_MACHINE}) {
        hr=policyDword(hive,L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer",L"NoReadingPane",value,present);
        if(FAILED(hr))return hr;if(present&&value)allowed=false;
    }
    return S_OK;
}
HRESULT handlerAllowed(REFCLSID handler,bool& allowed) noexcept {
    allowed=true;bool enforced=false;DWORD value=0;bool present=false;
    constexpr auto policies=L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer";
    for(const auto hive:{HKEY_CURRENT_USER,HKEY_LOCAL_MACHINE}) {
        const auto hr=policyDword(hive,policies,L"EnforceShellExtensionSecurity",value,present);
        if(FAILED(hr))return hr;if(present&&value)enforced=true;
    }
    if(!enforced)return S_OK;
    wchar_t classId[40]{};
    if(!StringFromGUID2(handler,classId,static_cast<int>(std::size(classId))))return E_UNEXPECTED;
    // Enabled policy admits the exact extension only if either per-user or
    // administrator Approved entry exists. Read existence, never its label.
    // https://learn.microsoft.com/windows/client-management/mdm/policy-csp-admx-windowsexplorer#enforceshellextensionsecurity
    constexpr auto approved=L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved";
    for(const auto hive:{HKEY_CURRENT_USER,HKEY_LOCAL_MACHINE}) {
        DWORD bytes=0;
        const auto error=RegGetValueW(hive,approved,classId,RRF_RT_ANY,nullptr,nullptr,&bytes);
        if(error==ERROR_SUCCESS)return S_OK;
        if(error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND)return HRESULT_FROM_WIN32(error);
    }
    allowed=false;return S_OK;
}
}

struct NativePreviewHost::Impl : std::enable_shared_from_this<NativePreviewHost::Impl> {
    struct Request {
        std::uint64_t epoch=0;
        OwnedPidl item;
        PreviewEmptyReason reason=PreviewEmptyReason::None;
        // Created/released on the creator STA. Only the documented thread-safe
        // inter-thread marshal stream is read by the worker. No raw Frame COM
        // pointer or source IShellItem crosses apartments.
        ComPtr<IStream> marshal;
        bool marshalHeld=false;
        std::atomic_bool claimed{false},complete{false},unmarshaled{false};
        std::atomic<HWND> window{nullptr};
        std::weak_ptr<Impl> owner;
        HRESULT releaseMarshal() noexcept {
            // Detach before callbacks: reentrant creator cleanup must never
            // release the same marshal data twice or mutate its owner vector.
            ComPtr<IStream> packet;packet.Attach(marshal.Detach());
            const bool releaseData=marshalHeld&&!unmarshaled.load();marshalHeld=false;
            HRESULT hr=S_OK;
            if(packet&&releaseData) {
                LARGE_INTEGER zero{};hr=packet->Seek(zero,STREAM_SEEK_SET,nullptr);
                if(SUCCEEDED(hr))hr=CoReleaseMarshalData(packet.Get());
            }
            packet.Reset();return hr;
        }
        ~Request(){const auto hr=releaseMarshal();if(const auto state=owner.lock())state->cleanup(hr);}
    };
    struct Frame final : IPreviewHandlerFrame,IOleWindow {
        std::atomic<ULONG> references{1};
        std::weak_ptr<Impl> owner;
        std::weak_ptr<Request> request;
        DWORD creator=0;
        Frame(const std::shared_ptr<Impl>& state,const std::shared_ptr<Request>& ticket):owner(state),request(ticket),creator(state->creator){}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** result) override {
            if(!result)return E_POINTER;*result=nullptr;
            if(iid==IID_IUnknown||iid==IID_IPreviewHandlerFrame)*result=static_cast<IPreviewHandlerFrame*>(this);
            else if(iid==IID_IOleWindow)*result=static_cast<IOleWindow*>(this);
            else return E_NOINTERFACE;
            AddRef();return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override{return ++references;}
        ULONG STDMETHODCALLTYPE Release() override{const auto count=--references;if(!count)delete this;return count;}
        HRESULT STDMETHODCALLTYPE GetWindowContext(PREVIEWHANDLERFRAMEINFO* info) override {
            if(!info)return E_POINTER;*info={};
            const auto state=owner.lock();const auto ticket=request.lock();
            return state&&ticket&&state->current(*ticket)?S_OK:E_ABORT;
        }
        HRESULT STDMETHODCALLTYPE GetWindow(HWND* value) override {
            if(!value)return E_POINTER;*value=nullptr;
            const auto state=owner.lock();const auto ticket=request.lock();
            if(!state||!ticket||!state->current(*ticket))return E_ABORT;
            const auto hwnd=ticket->window.load();
            if(!hwnd||!state->sessionOwned(hwnd))return E_FAIL;
            *value=hwnd;return state->current(*ticket)?S_OK:E_ABORT;
        }
        HRESULT STDMETHODCALLTYPE ContextSensitiveHelp(BOOL) override{return E_NOTIMPL;}
        HRESULT STDMETHODCALLTYPE TranslateAccelerator(MSG* message) override {
            if(!message)return E_POINTER;
            if(GetCurrentThreadId()!=creator)return RPC_E_WRONG_THREAD;
            const auto state=owner.lock();const auto ticket=request.lock();
            if(!state||!ticket||!state->current(*ticket))return S_FALSE;
            const auto hwnd=ticket->window.load();
            if(!hwnd||!state->sessionOwned(hwnd)||!message->hwnd||
               (message->hwnd!=hwnd&&!IsChild(hwnd,message->hwnd)))return S_FALSE;
            if(!state->callbacks.translateAccelerator)return S_FALSE;
            try {
                const auto hr=state->callbacks.translateAccelerator(ticket->epoch,*message);
                // A legitimately processed host shortcut can itself navigate
                // and retire this ticket. Do not offer that key a second time.
                return hr==S_OK?S_OK:state->current(*ticket)?hr:S_FALSE;
            }catch(const std::bad_alloc&){return E_OUTOFMEMORY;}catch(...){return E_FAIL;}
        }
    };
    struct Session {
        Impl& owner;
        std::shared_ptr<Request> ticket;
        ComPtr<IShellItem> item;
        ComPtr<IUnknown> instance;
        ComPtr<IPreviewHandler> preview;
        ComPtr<IObjectWithSite> site;
        ComPtr<IInitializeWithStream> streamInitializer;
        ComPtr<IInitializeWithItem> itemInitializer;
        ComPtr<IInitializeWithFile> fileInitializer;
        ComPtr<IPreviewHandlerVisuals> visuals;
        ComPtr<IPreviewHandlerFrame> frame;
        ComPtr<IStream> stream;
        FileHandle file;
        HWND window=nullptr;
        bool siteAttempted=false,initialized=false,closed=false;
        bool visualQueryAttempted=false;
        HRESULT visualQueryResult=E_PENDING;
        std::uint64_t visualRevision=0;
        explicit Session(Impl& state,std::shared_ptr<Request> request):owner(state),ticket(std::move(request)){}
        HRESULT close() noexcept {
            if(closed)return S_OK;closed=true;
            HRESULT first=S_OK;
            PreviewCleanupStatus receipt;receipt.epoch=ticket->epoch;receipt.stage=PreviewHostStage::Retired;
            receipt.result=E_PENDING;receipt.pending=true;owner.recordCleanup(receipt);
            if(window) {
                if(owner.sessionOwned(window))ShowWindow(window,SW_HIDE);
                else first=E_ACCESSDENIED;
            }
            if(initialized&&preview) {
                receipt.stage=PreviewHostStage::Unloading;receipt.unloadAttempted=true;owner.recordCleanup(receipt);
                const auto hr=preview->Unload();receipt.unloadResult=hr;
                if(first==S_OK&&hr!=S_OK)first=exactSuccess(hr);
                owner.recordCleanup(receipt);
            }
            if(siteAttempted&&site) {
                receipt.stage=PreviewHostStage::Site;receipt.siteClearAttempted=true;owner.recordCleanup(receipt);
                const auto hr=site->SetSite(nullptr);receipt.siteClearResult=hr;
                if(first==S_OK&&hr!=S_OK)first=exactSuccess(hr);
                owner.recordCleanup(receipt);
            }
            // All handler aliases and the instance precede the source. Even a
            // failed Initialize may have retained a reference to that stream.
            visuals.Reset();fileInitializer.Reset();itemInitializer.Reset();streamInitializer.Reset();
            preview.Reset();site.Reset();instance.Reset();frame.Reset();
            stream.Reset();file.reset();item.Reset();
            if(window) {
                const auto hwnd=window;
                if(!owner.sessionOwned(hwnd)) {if(first==S_OK)first=E_ACCESSDENIED;}
                else {
                    receipt.stage=PreviewHostStage::Window;receipt.windowDestroyAttempted=true;owner.recordCleanup(receipt);
                    if(!DestroyWindow(hwnd)) {receipt.windowDestroyResult=nativeError();if(first==S_OK)first=receipt.windowDestroyResult;}
                    else {receipt.windowDestroyResult=S_OK;window=nullptr;ticket->window.store(nullptr);}
                }
            }
            receipt.stage=PreviewHostStage::Retired;receipt.result=first;receipt.pending=false;owner.recordCleanup(receipt);
            owner.cleanup(first);return first;
        }
        ~Session(){close();}
    };
    struct Start {std::shared_ptr<Impl> state;std::unique_ptr<StaWorkerLease> lease;};

    DWORD creator=GetCurrentThreadId(),process=GetCurrentProcessId();
    HWND container=nullptr;
    HDESK creatorDesktop=nullptr;
    PreviewHostCallbacks callbacks;
    HANDLE wake=nullptr,thread=nullptr;
    std::atomic<DWORD> worker{0};
    std::atomic_bool stopping{false};
    std::atomic<std::uint64_t> desired{0};
    mutable std::mutex mutex;
    std::shared_ptr<Request> latest;
    // Creator owns all marshal packets until worker aliases have gone. Worker
    // sets complete only AFTER releasing its own shared_ptr. Thus disposal of
    // never-unmarshaled packets always occurs in their original apartment.
    std::vector<std::shared_ptr<Request>> records;
    RECT bounds{};
    std::uint64_t sizeSequence=0,focusSequence=0,focusEpoch=0;
    std::uint64_t visualSequence=0;
    PreviewVisualSuggestions visualSuggestions;
    bool focusReverse=false;
    PreviewHostStatus readback;

    ~Impl() {
        // The lease retains this Impl for creator-STA release after actual
        // kernel exit; handles, pending packets and callbacks remain owned.
        latest.reset();clearRecords();
        if(container&&GetPropW(container,ownerProperty)==this)RemovePropW(container,ownerProperty);
        if(thread)CloseHandle(thread);if(wake)CloseHandle(wake);
    }
    bool creatorOwned() const noexcept {
        DWORD pid=0;const auto tid=GetWindowThreadProcessId(container,&pid);
        return container&&tid==creator&&pid==process&&GetPropW(container,ownerProperty)==this;
    }
    bool sessionOwned(HWND hwnd) const noexcept {
        DWORD pid=0;const auto tid=GetWindowThreadProcessId(hwnd,&pid);
        return hwnd&&creatorOwned()&&tid==worker.load()&&pid==process&&GetParent(hwnd)==container&&IsChild(container,hwnd)&&
            GetPropW(hwnd,ownerProperty)==this;
    }
    bool current(const Request& ticket) const noexcept {
        return !stopping.load()&&desired.load()==ticket.epoch&&creatorOwned();
    }
    HRESULT creatorCheck() const noexcept {
        if(GetCurrentThreadId()!=creator)return RPC_E_WRONG_THREAD;
        return creatorOwned()?S_OK:E_ACCESSDENIED;
    }
    void notify(std::uint64_t epoch) const noexcept {
        if(callbacks.notifyWindow&&callbacks.notifyMessage&&GetWindowThreadProcessId(callbacks.notifyWindow,nullptr)==creator)
            PostMessageW(callbacks.notifyWindow,callbacks.notifyMessage,static_cast<WPARAM>(epoch&0xffffffffu),
                         static_cast<LPARAM>(epoch>>32));
    }
    void record(const Request& ticket,PreviewHostStage stage,HRESULT native) noexcept {
        std::lock_guard lock(mutex);
        if(desired.load()==ticket.epoch&&!stopping.load()) {
            readback.activeEpoch=ticket.epoch;readback.nativeStage=stage;readback.nativeResult=native;
            readback.stage=stage;readback.result=native;
        }
    }
    void complete(const Request& ticket,HRESULT result,PreviewHostStage stage,PreviewEmptyReason reason=PreviewEmptyReason::None) noexcept {
        {
            std::lock_guard lock(mutex);
            if(desired.load()==ticket.epoch&&!stopping.load()) {
                readback.activeEpoch=ticket.epoch;readback.stage=stage;readback.result=result;readback.emptyReason=reason;
                readback.ready=stage==PreviewHostStage::Ready&&result==S_OK;
                readback.pending=false;readback.sessionWindow=ticket.window.load();
                if(stage!=PreviewHostStage::Ready)readback.visuals.pending=false;
            }
        }
        notify(ticket.epoch);
    }
    void cleanup(HRESULT hr) noexcept {
        if(FAILED(hr)){std::lock_guard lock(mutex);if(SUCCEEDED(readback.cleanupResult))readback.cleanupResult=hr;}
    }
    void recordCleanup(const PreviewCleanupStatus& status) noexcept {
        std::lock_guard lock(mutex);readback.cleanup=status;
    }
    HRESULT visualFence(const Session& session,std::uint64_t revision) const noexcept {
        if(!current(*session.ticket)||session.ticket->window.load()!=session.window)return E_ABORT;
        if(!sessionOwned(session.window))return E_ACCESSDENIED;
        std::lock_guard lock(mutex);
        if(stopping.load()||desired.load()!=session.ticket->epoch)return E_ABORT;
        return revision==visualSequence?S_OK:S_FALSE;
    }
    bool publishVisuals(const Session& session,std::uint64_t revision,const PreviewVisualStatus& status) noexcept {
        std::lock_guard lock(mutex);
        if(stopping.load()||desired.load()!=session.ticket->epoch||revision!=visualSequence||
           readback.activeEpoch!=session.ticket->epoch||readback.sessionWindow!=session.window||
           session.ticket->window.load()!=session.window)return false;
        readback.visuals=status;return true;
    }
    HRESULT failedVisualPublication(const Session& session,std::uint64_t revision) const noexcept {
        const auto checked=visualFence(session,revision);return checked==S_OK?E_ABORT:checked;
    }
    HRESULT applyVisuals(Session& session) {
        PreviewVisualSuggestions values;std::uint64_t revision=0;
        {std::lock_guard lock(mutex);values=visualSuggestions;revision=visualSequence;}
        if(!revision||session.visualRevision==revision)return S_OK;
        auto fence=visualFence(session,revision);if(fence!=S_OK)return fence;
        session.visualRevision=revision;
        PreviewVisualStatus receipt;
        receipt.requestedRevision=receipt.attemptedRevision=revision;receipt.attemptedEpoch=session.ticket->epoch;
        receipt.requested=receipt.attempted=values;receipt.pending=true;receipt.result=E_PENDING;
        receipt.queryAttempted=session.visualQueryAttempted;receipt.queryResult=session.visualQueryResult;
        if(!publishVisuals(session,revision,receipt))return failedVisualPublication(session,revision);
        const bool any=values.backgroundPresent||values.textPresent||values.fontPresent;
        if(any&&!session.visualQueryAttempted) {
            fence=visualFence(session,revision);if(fence!=S_OK)return fence;
            session.visualQueryAttempted=true;receipt.queryAttempted=true;
            if(!publishVisuals(session,revision,receipt))return failedVisualPublication(session,revision);
            session.visualQueryResult=session.instance.As(&session.visuals);
            if(session.visualQueryResult!=S_OK||!session.visuals)session.visuals.Reset();
            receipt.queryResult=session.visualQueryResult;
            fence=visualFence(session,revision);if(fence!=S_OK)return fence;
            if(!publishVisuals(session,revision,receipt))return failedVisualPublication(session,revision);
        }
        receipt.result=any?(session.visuals?S_OK:session.visualQueryResult==S_OK?E_UNEXPECTED:session.visualQueryResult):S_FALSE;
        auto apply=[&](bool present,bool& attempted,HRESULT& raw,auto&& call)->HRESULT {
            if(!present||!session.visuals)return S_OK;
            auto checked=visualFence(session,revision);if(checked!=S_OK)return checked;
            attempted=true;
            if(!publishVisuals(session,revision,receipt))return failedVisualPublication(session,revision);
            raw=call(); // Optional native failure is a receipt, not render failure.
            if(receipt.result==S_OK&&raw!=S_OK)receipt.result=raw;
            checked=visualFence(session,revision);if(checked!=S_OK)return checked;
            if(!publishVisuals(session,revision,receipt))return failedVisualPublication(session,revision);
            return S_OK;
        };
        auto hr=apply(values.backgroundPresent,receipt.backgroundAttempted,receipt.backgroundResult,
                      [&]{return session.visuals->SetBackgroundColor(values.background);});
        if(hr!=S_OK)return hr;
        hr=apply(values.textPresent,receipt.textAttempted,receipt.textResult,
                 [&]{return session.visuals->SetTextColor(values.text);});
        if(hr!=S_OK)return hr;
        hr=apply(values.fontPresent,receipt.fontAttempted,receipt.fontResult,
                 [&]{return session.visuals->SetFont(&values.font);});
        if(hr!=S_OK)return hr;
        fence=visualFence(session,revision);if(fence!=S_OK)return fence;
        receipt.pending=false;
        if(!publishVisuals(session,revision,receipt))return failedVisualPublication(session,revision);
        notify(session.ticket->epoch);return S_OK;
    }
    void reap() {
        std::vector<std::shared_ptr<Request>> retired;retired.reserve(records.size());
        for(auto& request:records)if(request->complete.load())retired.push_back(std::move(request));
        records.erase(std::remove(records.begin(),records.end(),nullptr),records.end());
        // Native origin-apartment cleanup may dispatch COM. Finish the vector
        // mutation first; a callback may safely publish newer records now.
        for(auto& request:retired){cleanup(request->releaseMarshal());request.reset();}
    }
    void clearRecords() noexcept {
        std::vector<std::shared_ptr<Request>> retired;retired.swap(records);
        for(auto& request:retired){cleanup(request->releaseMarshal());request.reset();}
    }
    template<class Call> HRESULT invoke(const Request& ticket,PreviewHostStage stage,Call&& call) {
        if(!current(ticket))return E_ABORT;
        record(ticket,stage,E_PENDING);
        const auto hr=call();record(ticket,stage,hr);
        return current(ticket)?hr:E_ABORT;
    }
    HRESULT initialize(Session& session) {
        auto& ticket=*session.ticket;
        auto hr=invoke(ticket,PreviewHostStage::Source,[&]{return session.instance.As(&session.streamInitializer);});
        if(hr==S_OK&&!session.streamInitializer)return E_NOINTERFACE;
        if(hr==S_OK) {
            ComPtr<IBindCtx> bind;
            hr=invoke(ticket,PreviewHostStage::Source,[&]{return CreateBindCtx(0,&bind);});
            if(hr!=S_OK||!bind)return hr==S_OK?E_NOINTERFACE:exactSuccess(hr);
            BIND_OPTS options{};options.cbStruct=sizeof(options);options.grfMode=STGM_READ;
            hr=invoke(ticket,PreviewHostStage::Source,[&]{return bind->SetBindOptions(&options);});
            if(hr!=S_OK)return exactSuccess(hr);
            hr=invoke(ticket,PreviewHostStage::Source,[&]{return session.item->BindToHandler(bind.Get(),BHID_Stream,IID_PPV_ARGS(&session.stream));});
            if(hr==S_OK&&!session.stream)return E_NOINTERFACE;
            if(hr==S_OK) {
                {std::lock_guard lock(mutex);readback.initialization=PreviewInitialization::Stream;}
                hr=invoke(ticket,PreviewHostStage::Initialization,[&]{
                    const auto value=session.streamInitializer->Initialize(session.stream.Get(),STGM_READ);
                    session.initialized=SUCCEEDED(value);return value;
                });
                return exactSuccess(hr); // Never initialize this object again.
            }
            // Unsupported stream binding can select an advertised Item source,
            // but permission/IO/initialization failures never trigger a retry.
            if(hr!=E_NOINTERFACE&&hr!=HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED))return exactSuccess(hr);
        }else if(hr!=E_NOINTERFACE)return exactSuccess(hr);
        hr=invoke(ticket,PreviewHostStage::Source,[&]{return session.instance.As(&session.itemInitializer);});
        if(hr==S_OK&&!session.itemInitializer)return E_NOINTERFACE;
        if(hr==S_OK) {
            {std::lock_guard lock(mutex);readback.initialization=PreviewInitialization::Item;}
            hr=invoke(ticket,PreviewHostStage::Initialization,[&]{
                const auto value=session.itemInitializer->Initialize(session.item.Get(),STGM_READ);
                session.initialized=SUCCEEDED(value);return value;
            });return exactSuccess(hr);
        }
        if(hr!=E_NOINTERFACE)return exactSuccess(hr);
        hr=invoke(ticket,PreviewHostStage::Source,[&]{return session.instance.As(&session.fileInitializer);});
        if(hr!=S_OK||!session.fileInitializer)return hr==S_OK?E_NOINTERFACE:exactSuccess(hr);
        TaskText path;
        hr=invoke(ticket,PreviewHostStage::Source,[&]{return session.item->GetDisplayName(SIGDN_FILESYSPATH,&path.value);});
        if(hr!=S_OK||!path.value||!path.value[0])return hr==S_OK?E_INVALIDARG:exactSuccess(hr);
        hr=invoke(ticket,PreviewHostStage::Source,[&]{
            session.file.value=CreateFileW(path.value,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            return session.file.value==INVALID_HANDLE_VALUE?nativeError():S_OK;
        });if(hr!=S_OK)return exactSuccess(hr);
        {std::lock_guard lock(mutex);readback.initialization=PreviewInitialization::File;}
        hr=invoke(ticket,PreviewHostStage::Initialization,[&]{
            const auto value=session.fileInitializer->Initialize(path.value,STGM_READ);
            session.initialized=SUCCEEDED(value);return value;
        });return exactSuccess(hr);
    }
    HRESULT open(Session& session,std::uint64_t& sized) {
        auto& ticket=*session.ticket;
        // Keep the original marshal packet until creator cleanup can handle a
        // failed unmarshal in its originating apartment.
        ComPtr<IStream> packet=ticket.marshal;
        auto hr=invoke(ticket,PreviewHostStage::Starting,[&]{
            const auto value=CoGetInterfaceAndReleaseStream(packet.Detach(),IID_PPV_ARGS(&session.frame));
            // Record native consumption before invoke applies its stale fence.
            // A successful but stale unmarshal has already released the packet.
            if(SUCCEEDED(value))ticket.unmarshaled.store(true);
            return value;
        });
        if(hr!=S_OK||!session.frame)return hr==S_OK?E_NOINTERFACE:exactSuccess(hr);
        hr=invoke(ticket,PreviewHostStage::Source,[&]{return SHCreateItemFromIDList(ticket.item.get(),IID_PPV_ARGS(&session.item));});
        if(hr!=S_OK||!session.item)return hr==S_OK?E_NOINTERFACE:exactSuccess(hr);
        SFGAOF attributes=SFGAO_FOLDER;
        hr=invoke(ticket,PreviewHostStage::Source,[&]{return session.item->GetAttributes(SFGAO_FOLDER,&attributes);});
        // S_FALSE is the documented successful result for a non-folder when
        // the requested SFGAO_FOLDER bit is absent; consume its actual output.
        if(hr!=S_OK&&hr!=S_FALSE)return exactSuccess(hr);
        if(attributes&SFGAO_FOLDER){complete(ticket,S_FALSE,PreviewHostStage::Idle,PreviewEmptyReason::Folder);return S_FALSE;}
        bool allowed=false;hr=invoke(ticket,PreviewHostStage::Association,[&]{return previewAllowed(allowed);});
        if(hr!=S_OK)return exactSuccess(hr);
        if(!allowed){complete(ticket,S_FALSE,PreviewHostStage::Idle,PreviewEmptyReason::Disabled);return S_FALSE;}
        ComPtr<IQueryAssociations> association;
        hr=invoke(ticket,PreviewHostStage::Association,[&]{return session.item->BindToHandler(nullptr,BHID_AssociationArray,IID_PPV_ARGS(&association));});
        if(hr==E_NOINTERFACE||hr==HRESULT_FROM_WIN32(ERROR_NO_ASSOCIATION)) {
            complete(ticket,S_FALSE,PreviewHostStage::Idle,PreviewEmptyReason::NoAssociation);return S_FALSE;
        }
        if(hr!=S_OK||!association)return hr==S_OK?E_NOINTERFACE:exactSuccess(hr);
        std::array<wchar_t,128> guid{};std::array<wchar_t,40> previewId{};
        if(!StringFromGUID2(IID_IPreviewHandler,previewId.data(),static_cast<int>(previewId.size())))return E_UNEXPECTED;
        DWORD count=static_cast<DWORD>(guid.size());
        hr=invoke(ticket,PreviewHostStage::Association,[&]{return association->GetString(ASSOCF_NOTRUNCATE,ASSOCSTR_SHELLEXTENSION,
            previewId.data(),guid.data(),&count);});
        if(hr==HRESULT_FROM_WIN32(ERROR_NO_ASSOCIATION)||hr==HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)||hr==HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) {
            complete(ticket,S_FALSE,PreviewHostStage::Idle,PreviewEmptyReason::NoAssociation);return S_FALSE;
        }
        if(hr!=S_OK)return exactSuccess(hr);
        if(!count||count>guid.size()||guid.back()!=0)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        CLSID clsid{};hr=CLSIDFromString(guid.data(),&clsid);if(FAILED(hr))return hr;
        {std::lock_guard lock(mutex);readback.handler=clsid;}
        allowed=false;hr=invoke(ticket,PreviewHostStage::Association,[&]{return handlerAllowed(clsid,allowed);});
        if(hr!=S_OK)return exactSuccess(hr);
        if(!allowed) {
            complete(ticket,HRESULT_FROM_WIN32(ERROR_ACCESS_DISABLED_BY_POLICY),PreviewHostStage::Idle,PreviewEmptyReason::Disabled);
            return S_FALSE;
        }
        hr=invoke(ticket,PreviewHostStage::Activation,[&]{return CoCreateInstance(clsid,nullptr,CLSCTX_LOCAL_SERVER,IID_PPV_ARGS(&session.instance));});
        if(hr!=S_OK||!session.instance)return hr==S_OK?E_NOINTERFACE:exactSuccess(hr);
        hr=invoke(ticket,PreviewHostStage::Activation,[&]{return session.instance.As(&session.preview);});
        if(hr!=S_OK||!session.preview)return hr==S_OK?E_NOINTERFACE:exactSuccess(hr);
        hr=invoke(ticket,PreviewHostStage::Site,[&]{return session.instance.As(&session.site);});
        if(hr!=S_OK||!session.site)return hr==S_OK?E_NOINTERFACE:exactSuccess(hr);
        RECT rect{};{std::lock_guard lock(mutex);rect=bounds;sized=sizeSequence;}
        if(!validBounds(rect)||!current(ticket))return E_ABORT;
        session.window=CreateWindowExW(WS_EX_CONTROLPARENT,L"STATIC",nullptr,
            WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,rect.left,rect.top,
            rect.right-rect.left,rect.bottom-rect.top,container,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(!session.window)return nativeError();
        if(!SetPropW(session.window,ownerProperty,this)) {
            const auto error=nativeError();
            record(ticket,PreviewHostStage::Window,error);
            if(DestroyWindow(session.window))session.window=nullptr;
            else {cleanup(nativeError());stopping.store(true);}
            return error;
        }
        ticket.window.store(session.window);
        if(!sessionOwned(session.window)||GetThreadDesktop(GetCurrentThreadId())!=creatorDesktop)return E_ACCESSDENIED;
        {
            DWORD pid=0;const auto tid=GetWindowThreadProcessId(session.window,&pid);
            std::lock_guard lock(mutex);readback.sessionWindow=session.window;readback.sessionProcess=pid;readback.sessionThread=tid;
        }
        hr=invoke(ticket,PreviewHostStage::Site,[&]{session.siteAttempted=true;return session.site->SetSite(session.frame.Get());});
        if(hr!=S_OK)return exactSuccess(hr);
        hr=initialize(session);if(hr!=S_OK)return hr;
        RECT client{0,0,rect.right-rect.left,rect.bottom-rect.top};
        hr=invoke(ticket,PreviewHostStage::Window,[&]{return session.preview->SetWindow(session.window,&client);});
        if(hr!=S_OK)return exactSuccess(hr);
        hr=applyVisuals(session);
        if(FAILED(hr))return hr;
        hr=invoke(ticket,PreviewHostStage::Rendering,[&]{return session.preview->DoPreview();});
        if(hr!=S_OK)return exactSuccess(hr);
        if(!sessionOwned(session.window)||!current(ticket))return E_ABORT;
        complete(ticket,S_OK,PreviewHostStage::Ready);return S_OK;
    }
    HRESULT retire(std::unique_ptr<Session>& session) noexcept {
        if(!session)return S_OK;
        auto request=session->ticket;
        const auto hr=session->close();session.reset();
        auto* finished=request.get();request.reset();finished->complete.store(true);
        if(FAILED(hr))stopping.store(true);
        return hr;
    }
    bool focusCurrent(const Session& session,std::uint64_t sequence) const noexcept {
        if(!current(*session.ticket)||!sessionOwned(session.window))return false;
        std::lock_guard lock(mutex);
        return !stopping.load()&&desired.load()==session.ticket->epoch&&focusEpoch==session.ticket->epoch&&
            focusSequence==sequence&&readback.focus.requestedSequence==sequence&&
            readback.ready&&readback.activeEpoch==session.ticket->epoch&&readback.sessionWindow==session.window&&
            session.ticket->window.load()==session.window;
    }
    void applyFocus(Session& session,std::uint64_t sequence,bool reverse) {
        if(!focusCurrent(session,sequence))return;
        PreviewFocusStatus receipt;
        receipt.requestedSequence=receipt.completedSequence=sequence;
        receipt.requestedEpoch=receipt.completedEpoch=session.ticket->epoch;
        receipt.requestedReverse=receipt.completedReverse=reverse;
        // Sample the same real worker input-queue Shift state used by the
        // native SetFocus contract; never inject or manufacture modifiers.
        receipt.workerShiftDown=(GetKeyState(VK_SHIFT)&0x8000)!=0;receipt.workerShiftRead=true;
        HRESULT setOutcome=HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
        if(receipt.workerShiftDown!=reverse)receipt.result=setOutcome;
        else {
            if(!focusCurrent(session,sequence))return;
            receipt.setAttempted=true;receipt.setResult=session.preview->SetFocus();setOutcome=receipt.setResult;
            if(!focusCurrent(session,sequence))return;
            receipt.result=receipt.setResult;
            if(receipt.setResult==S_OK) {
                if(!focusCurrent(session,sequence))return;
                receipt.queryAttempted=true;receipt.queryResult=session.preview->QueryFocus(&receipt.queriedWindow);
                if(!focusCurrent(session,sequence))return;
                receipt.result=receipt.queryResult;
            }
        }
        if(!focusCurrent(session,sequence))return;
        {
            std::lock_guard lock(mutex);
            if(stopping.load()||desired.load()!=session.ticket->epoch||focusEpoch!=session.ticket->epoch||
               focusSequence!=sequence||readback.focus.requestedSequence!=sequence||!readback.ready||
               readback.activeEpoch!=session.ticket->epoch||readback.sessionWindow!=session.window||
               session.ticket->window.load()!=session.window)return;
            readback.focus=receipt;readback.focusResult=setOutcome;
        }
        notify(session.ticket->epoch);
    }
    void run() {
        std::unique_ptr<Session> session;
        std::uint64_t sized=0,focused=0;
        try {
            while(!stopping.load()) {
                std::shared_ptr<Request> next;
                {std::lock_guard lock(mutex);if(latest){next=std::move(latest);next->claimed.store(true);}}
                if(next) {
                    const auto retired=retire(session);focused=0;
                    if(FAILED(retired)) {
                        complete(*next,retired,PreviewHostStage::Failed);
                        auto* finished=next.get();next.reset();finished->complete.store(true);break;
                    }
                    if(!next->item) {
                        complete(*next,S_OK,PreviewHostStage::Idle,next->reason);
                        auto* finished=next.get();next.reset();finished->complete.store(true);
                    }else {
                        session=std::make_unique<Session>(*this,std::move(next));
                        const auto hr=open(*session,sized);
                        if(hr!=S_OK) {
                            if(hr!=S_FALSE)complete(*session->ticket,hr,PreviewHostStage::Failed);
                            retire(session);
                        }
                    }
                    continue;
                }
                if(session&&current(*session->ticket)) {
                    const auto visuals=applyVisuals(*session);
                    if(FAILED(visuals)) {
                        complete(*session->ticket,visuals,PreviewHostStage::Failed);retire(session);continue;
                    }
                    RECT rect{};std::uint64_t resizeVersion=0,focusVersion=0,requestedFocusEpoch=0;bool reverse=false;
                    {std::lock_guard lock(mutex);rect=bounds;resizeVersion=sizeSequence;focusVersion=focusSequence;
                        reverse=focusReverse;requestedFocusEpoch=focusEpoch;}
                    if(resizeVersion!=sized) {
                        const auto hr=invoke(*session->ticket,PreviewHostStage::Resizing,[&]{
                            if(!sessionOwned(session->window))return E_ACCESSDENIED;
                            if(!MoveWindow(session->window,rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,TRUE))return nativeError();
                            if(!current(*session->ticket)||!sessionOwned(session->window))return E_ABORT;
                            RECT client{0,0,rect.right-rect.left,rect.bottom-rect.top};return session->preview->SetRect(&client);
                        });sized=resizeVersion;
                        if(hr!=S_OK){complete(*session->ticket,exactSuccess(hr),PreviewHostStage::Failed);retire(session);continue;}
                        complete(*session->ticket,S_OK,PreviewHostStage::Ready);
                    }
                    if(focusVersion!=focused&&requestedFocusEpoch==session->ticket->epoch) {
                        applyFocus(*session,focusVersion,reverse);focused=focusVersion;
                    }
                }
                HANDLE event=wake;
                const auto waited=MsgWaitForMultipleObjectsEx(1,&event,INFINITE,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
                if(waited==WAIT_FAILED){cleanup(nativeError());break;}
                MSG message{};unsigned count=0;
                while(count++<64&&PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
                    if(message.message==WM_QUIT){stopping.store(true);break;}
                    TranslateMessage(&message);DispatchMessageW(&message);
                }
            }
        }catch(const std::bad_alloc&){cleanup(E_OUTOFMEMORY);}catch(...){cleanup(E_FAIL);}
        retire(session);
    }
    static unsigned __stdcall threadMain(void* value) noexcept {
        std::unique_ptr<Start> start(static_cast<Start*>(value));const auto state=start->state;
        state->worker.store(GetCurrentThreadId());
        {std::lock_guard lock(state->mutex);state->readback.workerThread=GetCurrentThreadId();state->readback.workerStarted=true;}
        auto hr=start->lease->attach();
        bool apartment=false;
        if(SUCCEEDED(hr)) {
            const auto desk=GetThreadDesktop(GetCurrentThreadId());DWORD bytes=0;
            std::lock_guard lock(state->mutex);
            state->readback.desktopMatchesCreator=desk==state->creatorDesktop;
            if(!desk||!state->readback.desktopMatchesCreator)hr=E_ACCESSDENIED;
            else if(!GetUserObjectInformationW(desk,UOI_NAME,state->readback.desktopName,sizeof(state->readback.desktopName),&bytes))hr=nativeError();
        }
        if(SUCCEEDED(hr)){hr=OleInitialize(nullptr);apartment=SUCCEEDED(hr);}
        if(SUCCEEDED(hr))state->run();else state->cleanup(hr);
        // No publication can enter after dispatch has ended, including during
        // teardown callbacks or before the terminal status is written.
        state->stopping.store(true);
        // The complete creator-owned state (including failed marshal packets)
        // must be reaped on that STA only after this real kernel thread exits.
        std::shared_ptr<void> keepalive=state;
        const auto deferred=start->lease->deferCreatorRelease(keepalive);state->cleanup(deferred);
        if(apartment)OleUninitialize();
        const auto finished=start->lease->finish();state->cleanup(finished);
        {
            std::lock_guard lock(state->mutex);state->readback.ready=false;state->readback.pending=false;
            state->readback.visuals.pending=false;
            state->readback.focus.pending=false;
            if(state->readback.stage!=PreviewHostStage::Failed) {
                state->readback.stage=PreviewHostStage::Stopped;
                if(FAILED(state->readback.cleanupResult))state->readback.result=state->readback.cleanupResult;
            }
        }
        state->notify(state->desired.load());return 0;
    }
    HRESULT enqueue(PCIDLIST_ABSOLUTE item,std::uint64_t epoch,PreviewEmptyReason reason) {
        const auto checked=creatorCheck();if(FAILED(checked))return checked;
        if(stopping.load())return E_ABORT;
        const auto originalEpoch=desired.load();
        if(!epoch||epoch<=originalEpoch)return E_INVALIDARG;
        auto request=std::make_shared<Request>();request->owner=shared_from_this();request->epoch=epoch;request->reason=reason;
        if(item) {
            request->item.reset(ILCloneFull(item));if(!request->item)return E_OUTOFMEMORY;
        }
        // Own the caller's PIDL before origin-apartment cleanup can dispatch
        // callbacks which invalidate that caller's selection storage.
        reap();
        if(item) {
            ComPtr<IPreviewHandlerFrame> frame;frame.Attach(new Frame(shared_from_this(),request));
            const auto marshaled=CoMarshalInterThreadInterfaceInStream(IID_IPreviewHandlerFrame,frame.Get(),&request->marshal);
            if(FAILED(marshaled)||!request->marshal)return FAILED(marshaled)?marshaled:E_NOINTERFACE;
            request->marshalHeld=true;
        }
        if(stopping.load()||!creatorOwned()||desired.load()!=originalEpoch)return HRESULT_FROM_WIN32(ERROR_RETRY);
        records.push_back(request);
        ShowWindow(container,SW_HIDE); // Same creator HWND; never waits for worker.
        if(stopping.load()||!creatorOwned()||desired.load()!=originalEpoch) {
            request->complete.store(true);reap();return HRESULT_FROM_WIN32(ERROR_RETRY);
        }
        {
            std::lock_guard lock(mutex);
            if(stopping.load()||epoch<=desired.load()) {
                request->complete.store(true);return HRESULT_FROM_WIN32(ERROR_RETRY);
            }
            if(latest&&!latest->claimed.load())latest->complete.store(true);
            latest=request;desired.store(epoch);
            readback.requestedEpoch=epoch;readback.ready=false;readback.pending=true;
            readback.stage=PreviewHostStage::Queued;readback.result=E_PENDING;readback.emptyReason=reason;
            readback.nativeStage=PreviewHostStage::Queued;readback.nativeResult=E_PENDING;
            readback.initialization=PreviewInitialization::None;readback.focusResult=E_PENDING;
            readback.focus={};
            readback.visuals={};readback.visuals.requestedRevision=visualSequence;
            readback.visuals.requested=visualSuggestions;readback.visuals.pending=item&&visualSequence!=0;
        }
        reap();return SetEvent(wake)?S_OK:nativeError();
    }
};

NativePreviewHost::NativePreviewHost(std::shared_ptr<Impl> implementation) noexcept:impl_(std::move(implementation)){}
HRESULT NativePreviewHost::create(HWND container,PreviewHostCallbacks callbacks,std::unique_ptr<NativePreviewHost>* result) noexcept {
    if(!result)return E_POINTER;result->reset();
    try {
        DWORD pid=0;const auto tid=GetWindowThreadProcessId(container,&pid);
        if(!container||tid!=GetCurrentThreadId()||pid!=GetCurrentProcessId()||!(GetWindowLongPtrW(container,GWL_STYLE)&WS_CHILD))return E_ACCESSDENIED;
        if(GetPropW(container,ownerProperty))return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        if(callbacks.notifyWindow&&(GetWindowThreadProcessId(callbacks.notifyWindow,nullptr)!=tid||callbacks.notifyMessage<WM_APP))return E_INVALIDARG;
        RECT bounds{};if(!GetClientRect(container,&bounds))return nativeError();if(!validBounds(bounds))return E_INVALIDARG;
        std::unique_ptr<StaWorkerLease> lease;auto hr=StaWorkerLease::prepare(&lease);if(FAILED(hr))return hr;
        auto state=std::make_shared<Impl>();state->container=container;state->callbacks=std::move(callbacks);
        state->creatorDesktop=GetThreadDesktop(GetCurrentThreadId());state->bounds=bounds;state->sizeSequence=1;
        state->wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!state->wake)return nativeError();
        if(!SetPropW(container,ownerProperty,state.get()))return nativeError();
        auto host=std::unique_ptr<NativePreviewHost>(new NativePreviewHost(state));
        auto start=std::make_unique<Impl::Start>();start->state=state;start->lease=std::move(lease);
        unsigned worker=0;const auto handle=_beginthreadex(nullptr,0,Impl::threadMain,start.get(),0,&worker);
        if(!handle)return E_FAIL;
        start.release();state->thread=reinterpret_cast<HANDLE>(handle);state->worker.store(worker);
        ShowWindow(container,SW_HIDE);*result=std::move(host);return S_OK;
    }catch(const std::bad_alloc&){return E_OUTOFMEMORY;}catch(...){return E_FAIL;}
}
NativePreviewHost::~NativePreviewHost() {
    if(!impl_)return;
    impl_->stopping.store(true);
    if(GetCurrentThreadId()==impl_->creator&&impl_->creatorOwned())ShowWindow(impl_->container,SW_HIDE);
    if(impl_->wake)SetEvent(impl_->wake);
    // A live worker still owns its state and exact kernel handle through the
    // global lease. This does not substitute for the caller's mandatory drain.
}
HRESULT NativePreviewHost::update(PCIDLIST_ABSOLUTE item,std::uint64_t epoch) noexcept {
    if(!item)return E_INVALIDARG;
    const auto state=impl_;
    try{return state->enqueue(item,epoch,PreviewEmptyReason::None);}catch(const std::bad_alloc&){return E_OUTOFMEMORY;}catch(...){return E_FAIL;}
}
HRESULT NativePreviewHost::clear(std::uint64_t epoch,PreviewEmptyReason reason) noexcept {
    const auto state=impl_;
    try{return state->enqueue(nullptr,epoch,reason);}catch(const std::bad_alloc&){return E_OUTOFMEMORY;}catch(...){return E_FAIL;}
}
HRESULT NativePreviewHost::resize(HWND container,const RECT& bounds) noexcept {
    const auto state=impl_;
    const auto hr=state->creatorCheck();if(FAILED(hr))return hr;
    if(state->stopping.load())return E_ABORT;
    if(container!=state->container||!validBounds(bounds))return E_INVALIDARG;
    {std::lock_guard lock(state->mutex);state->bounds=bounds;++state->sizeSequence;}
    return SetEvent(state->wake)?S_OK:nativeError();
}
HRESULT NativePreviewHost::suggestVisuals(const PreviewVisualSuggestions& suggestions) noexcept {
    const auto state=impl_;
    const auto checked=state->creatorCheck();if(FAILED(checked))return checked;
    PreviewVisualSuggestions values;const auto copied=copyVisuals(suggestions,values);if(FAILED(copied))return copied;
    {
        std::lock_guard lock(state->mutex);
        if(state->stopping.load())return E_ABORT;
        if(sameVisuals(values,state->visualSuggestions))return S_FALSE;
        if(state->visualSequence==std::numeric_limits<std::uint64_t>::max())return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
        state->visualSuggestions=values;++state->visualSequence;
        state->readback.visuals.requestedRevision=state->visualSequence;state->readback.visuals.requested=values;
        state->readback.visuals.pending=state->readback.ready||state->readback.pending;
    }
    return SetEvent(state->wake)?S_OK:nativeError();
}
HRESULT NativePreviewHost::focus(bool reverse) noexcept {
    const auto state=impl_;
    const auto hr=state->creatorCheck();if(FAILED(hr))return hr;
    if(state->stopping.load())return E_ABORT;
    {
        std::lock_guard lock(state->mutex);
        if(state->stopping.load())return E_ABORT;
        if(!state->readback.ready||state->readback.activeEpoch!=state->desired.load())return E_PENDING;
        if(state->focusSequence==std::numeric_limits<std::uint64_t>::max())return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
        state->focusReverse=reverse;state->focusEpoch=state->desired.load();++state->focusSequence;
        state->readback.focusResult=E_PENDING;
        state->readback.focus.requestedSequence=state->focusSequence;
        state->readback.focus.requestedEpoch=state->focusEpoch;state->readback.focus.requestedReverse=reverse;
        state->readback.focus.result=E_PENDING;state->readback.focus.pending=true;
    }
    return SetEvent(state->wake)?S_OK:nativeError();
}
HRESULT NativePreviewHost::showCurrent(std::uint64_t epoch) noexcept {
    const auto state=impl_;
    const auto hr=state->creatorCheck();if(FAILED(hr))return hr;
    if(state->stopping.load()||epoch!=state->desired.load())return E_ABORT;
    HWND window=nullptr;
    {std::lock_guard lock(state->mutex);if(!state->readback.ready||state->readback.activeEpoch!=epoch)return E_PENDING;window=state->readback.sessionWindow;}
    if(!state->sessionOwned(window))return E_ACCESSDENIED;
    ShowWindow(state->container,SW_SHOW);
    if(epoch!=state->desired.load()||state->stopping.load()||!state->creatorOwned()) {
        if(state->creatorOwned())ShowWindow(state->container,SW_HIDE);
        return E_ABORT;
    }
    return S_OK;
}
PreviewHostStatus NativePreviewHost::status() const noexcept {
    const auto state=impl_;
    std::lock_guard lock(state->mutex);auto result=state->readback;
    result.workerExited=state->thread&&WaitForSingleObject(state->thread,0)==WAIT_OBJECT_0;
    if(GetCurrentThreadId()!=state->creator){result.result=RPC_E_WRONG_THREAD;result.ready=false;}
    if(state->stopping.load()||result.activeEpoch!=state->desired.load())result.ready=false;
    return result;
}
HRESULT NativePreviewHost::drain(DWORD milliseconds) noexcept {
    const auto state=impl_;
    if(GetCurrentThreadId()!=state->creator)return RPC_E_WRONG_THREAD;
    state->stopping.store(true);if(state->creatorOwned())ShowWindow(state->container,SW_HIDE);
    if(state->wake&&!SetEvent(state->wake))return nativeError();
    if(!state->thread)return S_OK;
    const auto deadline=GetTickCount64()+milliseconds;
    for(;;) {
        const auto waited=WaitForSingleObject(state->thread,0);
        if(waited==WAIT_OBJECT_0)break;if(waited==WAIT_FAILED)return nativeError();
        const auto now=GetTickCount64();if(now>=deadline)return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        DWORD index=0;const auto remaining=static_cast<DWORD>(std::min<ULONGLONG>(deadline-now,20));
        const auto hr=CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS|COWAIT_DISPATCH_WINDOW_MESSAGES,remaining,1,&state->thread,&index);
        if(FAILED(hr)&&hr!=RPC_S_CALLPENDING)return hr;
    }
    // Exact kernel completion makes every packet eligible for creator cleanup,
    // including a request coalesced away or never consumed during shutdown.
    state->latest.reset();state->clearRecords();
    std::lock_guard lock(state->mutex);
    return FAILED(state->readback.cleanupResult)?state->readback.cleanupResult:S_OK;
}
} // namespace explorer
