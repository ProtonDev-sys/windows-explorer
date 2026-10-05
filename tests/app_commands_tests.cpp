#include "explorer/app_commands.hpp"
#include "explorer/search.hpp"
#include "explorer/library.hpp"
#include "explorer/headless_visual.hpp"
#include "explorer/worker_sta.hpp"

#include <shlobj.h>
#include <propkey.h>
#include <propvarutil.h>
#include <wrl/implements.h>
#include <array>
#include <atomic>
#include <cwctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>

namespace {
using namespace explorer;
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
unsigned assertions = 0;

void require(bool condition,const char* message) {
    ++assertions;
    if (!condition) throw std::runtime_error(message);
}
void succeeded(HRESULT hr,const char* message) {
    ++assertions;
    if (FAILED(hr)) {
        std::cerr << "HRESULT 0x" << std::hex << static_cast<unsigned long>(hr) << std::dec << '\n';
        throw std::runtime_error(message);
    }
}
ComPtr<IShellItem> item(const fs::path& path) {
    ComPtr<IShellItem> result;
    succeeded(SHCreateItemFromParsingName(path.c_str(),nullptr,IID_PPV_ARGS(&result)),"Create owned command fixture Shell item");
    return result;
}
ComPtr<IShellItemArray> array(IShellItem* shellItem) {
    ComPtr<IShellItemArray> result;
    succeeded(SHCreateShellItemArrayFromShellItem(shellItem,IID_PPV_ARGS(&result)),"Create actual native command selection");
    return result;
}
std::string read(const fs::path& path) {
    std::ifstream stream(path,std::ios::binary);
    require(stream.good(),"Read owned command fixture");
    return {std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>()};
}

struct Fixture {
    fs::path root,text,archive,directory;
    Fixture() {
        GUID id{};
        succeeded(CoCreateGuid(&id),"Create owned command fixture identity");
        wchar_t formatted[40]{};
        require(StringFromGUID2(id,formatted,40) != 0,"Format command fixture identity");
        root = fs::temp_directory_path()/(std::wstring(L"WindowsExplorer-CommandCatalog-")+formatted);
        require(fs::create_directory(root),"Create isolated command fixture");
        text = root/L"\u65E5\u672C\u8A9E \u03BB document.txt";
        directory = root/L"owned subfolder";
        require(fs::create_directory(directory),"Create owned subfolder");
        std::ofstream(text,std::ios::binary) << "This fixture is never sent, opened, printed, deleted or archived by a UI handler.";
        archive = root/L"owned empty.zip";
        const std::array<unsigned char,22> bytes{'P','K',5,6};
        std::ofstream output(archive,std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
        require(output.good(),"Write valid isolated empty ZIP");
    }
    ~Fixture() { std::error_code ignored; fs::remove_all(root,ignored); }
};

class NavigationEvents final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IExplorerBrowserEvents> {
public:
    bool finished = false;
    HRESULT status = E_PENDING;
    HRESULT STDMETHODCALLTYPE OnNavigationPending(PCIDLIST_ABSOLUTE) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnViewCreated(IShellView*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnNavigationComplete(PCIDLIST_ABSOLUTE) override { finished = true; status = S_OK; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnNavigationFailed(PCIDLIST_ABSOLUTE) override { finished = true; status = E_FAIL; return S_OK; }
};
struct HiddenView {
    HWND owner = nullptr;
    ComPtr<IExplorerBrowser> browser;
    ComPtr<IShellView> view;
    ComPtr<NavigationEvents> events;
    DWORD cookie = 0;
    explicit HiddenView(IShellItem* folder) {
        owner = CreateWindowExW(0,L"STATIC",L"headless native command fixture",WS_POPUP,
            0,0,800,600,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        require(owner && !IsWindowVisible(owner),"Create exclusively hidden owned provider host");
        succeeded(CoCreateInstance(CLSID_ExplorerBrowser,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&browser)),"Create hidden native ExplorerBrowser");
        succeeded(browser->SetOptions(EBO_NOPERSISTVIEWSTATE | EBO_NOTRAVELLOG),"Prohibit native view persistence in fixture");
        RECT bounds{0,0,800,600};
        FOLDERSETTINGS settings{FVM_DETAILS,0};
        succeeded(browser->Initialize(owner,&bounds,&settings),"Initialize actual hidden provider view");
        events = Microsoft::WRL::Make<NavigationEvents>();
        require(events != nullptr,"Create native navigation observer");
        succeeded(browser->Advise(events.Get(),&cookie),"Observe hidden native view navigation");
        succeeded(browser->BrowseToObject(folder,SBSP_ABSOLUTE),"Browse owned fixture without displaying a window");
        const auto deadline = GetTickCount64()+5000;
        while (!events->finished && GetTickCount64()<deadline) {
            MSG message{};
            while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
            Sleep(1);
        }
        require(events->finished,"Hidden native view navigation exceeded bounded wait");
        succeeded(events->status,"Native hidden provider completed navigation");
        succeeded(browser->GetCurrentView(IID_PPV_ARGS(&view)),"Read actual native IShellView site");
        require(!IsWindowVisible(owner),"Native view initialization displayed its owner");
    }
    ~HiddenView() {
        view.Reset();
        if (browser) { if (cookie) browser->Unadvise(cookie); browser->Destroy(); browser.Reset(); }
        events.Reset();
        if (owner) DestroyWindow(owner);
    }
};

void onPrivateDesktop(const std::function<void()>& body, bool registerGuard = true) {
    const auto borrowedDesktop = GetThreadDesktop(GetCurrentThreadId());
    if (!registerGuard) {
        const auto creator = explorer::PrivateDesktop::current();
        require(creator && creator->ready() && SUCCEEDED(creator->verifyIsolation()),
                "Unguarded negative control requires an already isolated creator desktop");
    }
    std::exception_ptr failure;
    std::atomic<bool> complete{false};
    std::thread worker([&] {
        explorer::PrivateDesktop desktop;
        bool initialized=false;
        try {
            if (registerGuard) {
                succeeded(desktop.initialize(),"Attach guarded private provider desktop before native initialization");
                succeeded(desktop.verifyIsolation(),"Verify private provider desktop isolation");
            } else {
                // Use the creator's non-input desktop without registering a
                // thread-local guard. Native headless invocation must refuse.
                require(SetThreadDesktop(borrowedDesktop) != FALSE,
                        "Attach unguarded control to the creator's private desktop before COM");
                require(explorer::PrivateDesktop::current() == nullptr,
                        "Negative-control thread unexpectedly registered a guard");
            }
            succeeded(OleInitialize(nullptr),"Initialize private provider STA");initialized=true;
            body();
        } catch (...) { failure=std::current_exception(); }
        if(initialized){
            try{succeeded(drainStaWorkers(10000),"Drain actual STA provider/desktop leases before apartment shutdown");}
            catch(...){if(!failure)failure=std::current_exception();}
            OleUninitialize();
        }
        complete.store(true);
    });
    while(!complete.load()) {
        MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
        MsgWaitForMultipleObjectsEx(0,nullptr,1,QS_ALLINPUT,MWMO_ALERTABLE|MWMO_INPUTAVAILABLE);
    }
    worker.join();if(failure)std::rethrow_exception(failure);
}

AppCommandContext folderContext() {
    AppCommandContext context;
    context.folderView = context.physicalDirectory = context.detailsView = true;
    return context;
}

void catalogCoverageAndExactRoutes() {
    std::set<UINT> ids;
    for (const auto& entry : appCommandCatalog()) {
        require(ids.insert(entry.command).second,"Command catalog contains ambiguous duplicate IDs");
        const auto binding = appCommandBinding(entry.command);
        require(binding && binding->route == entry.route && binding->scope == entry.scope,"Lookup lost exact command route/scope");
        if (binding->route != AppCommandRoute::Host && binding->route != AppCommandRoute::NamespaceAction)
            require(binding->commandStore.starts_with(L"Windows."),"Native command has no registered Windows key");
    }
    for (UINT command = Back; command <= AddressList; ++command)
        require(appCommandBinding(command).has_value(),"An existing host leaf/menu command is omitted from the catalog");
    for (const auto command : {RibbonBitLocker,RibbonMountDiscImage,RibbonBurnDiscImage,RibbonCastToDevice,RibbonWorkOffline,RibbonSyncOffline})
        require(appCommandBinding(command)->route == AppCommandRoute::NamespaceAction,"Contextual native action bypasses namespace planner");
    const std::array<std::pair<UINT,std::wstring_view>,16> exact{{
        {Undo,L"Windows.undo"},{Redo,L"Windows.redo"},{SortMenu,L"Windows.SortByColumn"},
        {GroupMenu,L"Windows.GroupByColumn"},{ColumnsMenu,L"Windows.AddColumns"},
        {RibbonSearchContents,L"Windows.SearchOptionContents"},{RibbonSearchSystemFiles,L"Windows.SearchOptionSystem"},
        {RibbonSearchZipFiles,L"Windows.SearchOptionCompressed"},{Extract,L"Windows.CompressedFolder.extract"},
        {RibbonAutoPlay,L"Windows.Autoplay"},{RibbonFinishBurning,L"Windows.FinishBurn"},{RibbonEraseDisc,L"Windows.EraseDisc"},
        {RibbonRemoveMediaServer,L"Windows.RemoveMediaServer"},{RibbonOpenSearchViewSite,L"Windows.OpenSearchViewSite"},
        {RibbonGroupSortAscending,L"Windows.SortGroupsAscending"},{RibbonGroupSortDescending,L"Windows.SortGroupsDescending"}
    }};
    for (const auto& [command,key] : exact) require(appCommandStoreName(command) == key,"Public native command mapping was guessed or changed");
    for (const auto command : {Undo,Redo,SortMenu,GroupMenu,ColumnsMenu})
        require(appCommandBinding(command)->scope == NamespaceMenuScope::Background,"View command did not receive actual background/view site");
    require(appCommandBinding(Extract)->scope == NamespaceMenuScope::Selection,"ZIP extract lost its explicit archive item target");
    require(appCommandStoreName(PreviewPane)==L"Windows.readingpane" && appCommandStoreName(DetailsPane)==L"Windows.previewpane",
            "Installed native pane presentation keys were inferred from their misleading registry names");
    require(appCommandBinding(Zip)->route == AppCommandRoute::NativeZip,"ZIP creation still uses staged/tar archive route");
    for(const auto command:{Delete,PermanentDelete})
        require(appCommandBinding(command)->route==AppCommandRoute::CommandStore &&
                appCommandBinding(command)->scope==NamespaceMenuScope::Selection &&
                appCommandStoreName(command)==(command==Delete?L"Windows.recycle":L"Windows.PermanentDelete"),
                "Deletion bypasses exact native confirmation/policy/selection providers");
    require(appCommandBinding(OpenFileLocation)->route == AppCommandRoute::CommandStore &&
            appCommandBinding(OpenFileLocation)->scope == NamespaceMenuScope::Selection &&
            appCommandStoreName(OpenFileLocation) == L"Windows.SearchOpenLocation",
            "Search location still bypasses the full native selected-result provider");
    require(appCommandBinding(Sharing)->route == AppCommandRoute::CommandStore &&
            appCommandBinding(Sharing)->scope == NamespaceMenuScope::Selection &&
            appCommandStoreName(Sharing) == L"Windows.ModernShare",
            "Normal Sharing retained a host-only count/StorageFile restriction");
    for (const auto command : std::array<UINT,4>{NewWindow,RibbonNewProcess,FolderOptions,RibbonFolderOptions})
        require(appCommandBinding(command)->route == AppCommandRoute::Host,"App window/options command would launch stock Explorer instead of host");
    require(!appCommandBinding(0) && !appCommandBinding(0xffffffff) && appCommandStoreName(0).empty(),"Unknown command was silently accepted");
    require(appCommandBinding(ExpandAncestors)->route == AppCommandRoute::Host &&
            appCommandBinding(ExpandAncestors)->requirements == RequireFolderView,
            "One-time ancestor expansion changed a persisted policy or bypassed its actual folder view");
}

void applicabilityAndOutputContracts() {
    auto context = folderContext();
    require(!appCommandApplicable(*appCommandBinding(Copy),context),"Copy enabled with no actual selected targets");
    context.selectionCount = 1;
    context.selectionAttributes = SFGAO_FILESYSTEM | SFGAO_CANCOPY | SFGAO_CANMOVE | SFGAO_CANDELETE | SFGAO_CANRENAME;
    for (const auto command : {Copy,Cut,Delete,PermanentDelete,Rename,Zip})
        require(appCommandApplicable(*appCommandBinding(command),context),"Actual allowed filesystem selection disabled");
    context.selectionAttributes &= ~SFGAO_CANRENAME;
    require(!appCommandApplicable(*appCommandBinding(Rename),context),"Provider rename restriction ignored");
    context.selectionCount = 2;
    require(!appCommandApplicable(*appCommandBinding(Pin),context),"Single-target pin command accepted multiselect");
    context.physicalDirectory = false;
    for (const auto command : {NewText,NewShortcut,Terminal})
        require(!appCommandApplicable(*appCommandBinding(command),context),"Virtual/archive folder accepted a physical destination action");
    for (const auto command : std::array<UINT,3>{NewFolder,NewItems,RibbonNewMenu})
        require(appCommandApplicable(*appCommandBinding(command),context) &&
                appCommandBinding(command)->scope==NamespaceMenuScope::Background,
                "New item command rejected native Library/virtual eligibility before querying its actual provider");
    for (const auto command : {Paste,PasteShortcut})
        require(appCommandApplicable(*appCommandBinding(command),context) && appCommandBinding(command)->scope == NamespaceMenuScope::Background,
                "Paste incorrectly excluded native virtual/ZIP destinations before actual provider query");
    require(!appCommandApplicable(*appCommandBinding(RibbonSearchContents),context),"Native Search option applied outside Search namespace");
    context.searchBackground = true;
    require(appCommandApplicable(*appCommandBinding(RibbonSearchContents),context),"Actual Search background option disabled");
    context.selectionAttributes = 0;
    context.selectionCount = 3;
    require(appCommandApplicable(*appCommandBinding(OpenFileLocation),context),
            "Search location excluded virtual or multiple results before consulting native state");
    context.selectionCount = 0;
    require(!appCommandApplicable(*appCommandBinding(OpenFileLocation),context),"Search location enabled with no selected results");
    context.selectionCount = 2;
    require(!appCommandApplicable(*appCommandBinding(LibraryDefault),context),"Library provider command applied outside a Library");
    context.library = true;
    require(!appCommandApplicable(*appCommandBinding(LibraryDefault),context),"Read-only Library accepted default-save mutation");
    context.writableLibrary = true;
    require(appCommandApplicable(*appCommandBinding(LibraryDefault),context),"Writable Library default-save provider omitted");
    require(!appCommandApplicable(*appCommandBinding(RibbonSystemProperties),context),"Computer-specific command applied to arbitrary folder");
    context.computer = true;
    require(appCommandApplicable(*appCommandBinding(RibbonSystemProperties),context),"Computer provider command unavailable in This PC");
    for (const auto command : {RibbonAddNetworkDevice,RibbonSearchActiveDirectory,RibbonNetworkSharingCenter}) {
        require(!appCommandApplicable(*appCommandBinding(command),context),"Network-root tool applied to an unrelated namespace");
        context.network=true;
        require(appCommandApplicable(*appCommandBinding(command),context) && appCommandBinding(command)->scope==NamespaceMenuScope::Background,
                "Network tool lost actual native folder/background state");
        context.network=false;
    }
    context.selectionCount = 1;
    for (const auto command : {RibbonAutoPlay,RibbonFinishBurning,RibbonEraseDisc}) {
        require(!appCommandApplicable(*appCommandBinding(command),context),"Disc/device command applied to an ordinary selected folder");
        context.driveRoot = true;
        require(appCommandApplicable(*appCommandBinding(command),context) &&
                appCommandBinding(command)->scope == NamespaceMenuScope::Selection,
                "Drive Media command lost actual drive target/native state scope");
        context.selectionCount = 0;
        require(appCommandApplicable(*appCommandBinding(command),context),
                "Drive Media excluded the actually browsed drive when no inner item was selected");
        context.selectionCount = 2;
        require(!appCommandApplicable(*appCommandBinding(command),context),"Drive Media command accepted ambiguous device targets");
        context.selectionCount = 1;
        context.driveRoot = false;
    }
    context.navigating = true;
    require(!appCommandApplicable(*appCommandBinding(SortMenu),context),"Command used stale view during navigation");
    NativeNamespaceActions actions;
    AppCommandCapability preserved;
    preserved.binding.command = 12345;
    require(queryAppCommand(actions,0,context,&preserved) == E_INVALIDARG && preserved.binding.command == 12345,"Invalid catalog query changed output");
    require(queryAppCommand(actions,Open,context,nullptr) == E_POINTER,"Null catalog output accepted");
    NamespaceCommandState state;
    state.state = ECS_CHECKED;
    require(namespaceCommandState(L"Windows.Bad\\Command",nullptr,nullptr,&state) == E_INVALIDARG && state.state == ECS_CHECKED,"Invalid native key mutated state");
    require(namespaceCommandState(L"Windows.copy",nullptr,nullptr,nullptr) == E_POINTER,"Null native state output accepted");
    require(FAILED(namespaceCommandState(L"Windows.ThisProviderDoesNotExist",nullptr,nullptr,&state)) && state.state == ECS_CHECKED,"Missing provider invented state");
    HRESULT otherThread = S_OK;
    std::thread worker([&] {
        const auto hr = CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        NamespaceCommandState ignored;
        otherThread = namespaceCommandState(L"Windows.undo",nullptr,nullptr,&ignored);
        if (SUCCEEDED(hr)) CoUninitialize();
    });
    worker.join();
    require(otherThread == RPC_E_WRONG_THREAD,"STA-only native command activated in MTA");
}

class PresentationSelection final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IShellItemArray,IPropertyStore> {
public:
    explicit PresentationSelection(IShellItemArray* selected):native(selected) {}
    ComPtr<IShellItemArray> native;
    std::wstring url;
    GETPROPERTYSTOREFLAGS requested=GPS_DEFAULT;
    unsigned stores=0;
    HRESULT propertyStatus=S_OK;
    bool malformed=false;
    HRESULT STDMETHODCALLTYPE BindToHandler(IBindCtx* context,REFGUID handler,REFIID iid,void** result) override {
        return native->BindToHandler(context,handler,iid,result);
    }
    HRESULT STDMETHODCALLTYPE GetPropertyStore(GETPROPERTYSTOREFLAGS flags,REFIID iid,void** result) override {
        ++stores;requested=flags;return QueryInterface(iid,result);
    }
    HRESULT STDMETHODCALLTYPE GetPropertyDescriptionList(REFPROPERTYKEY key,REFIID iid,void** result) override {
        return native->GetPropertyDescriptionList(key,iid,result);
    }
    HRESULT STDMETHODCALLTYPE GetAttributes(SIATTRIBFLAGS flags,SFGAOF mask,SFGAOF* result) override {
        return native->GetAttributes(flags,mask,result);
    }
    HRESULT STDMETHODCALLTYPE GetCount(DWORD* result) override { return native->GetCount(result); }
    HRESULT STDMETHODCALLTYPE GetItemAt(DWORD index,IShellItem** result) override { return native->GetItemAt(index,result); }
    HRESULT STDMETHODCALLTYPE EnumItems(IEnumShellItems** result) override { return native->EnumItems(result); }
    HRESULT STDMETHODCALLTYPE GetAt(DWORD index,PROPERTYKEY* result) override {
        if(!result)return E_POINTER;if(index)return E_INVALIDARG;*result=PKEY_Devices_PresentationUrl;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetValue(REFPROPERTYKEY key,PROPVARIANT* result) override {
        if(!result)return E_POINTER;if(FAILED(propertyStatus))return propertyStatus;
        if(!IsEqualPropertyKey(key,PKEY_Devices_PresentationUrl))return E_INVALIDARG;
        if(malformed){result->vt=VT_I4;result->lVal=1;return S_OK;}
        if(url.empty()){result->vt=VT_EMPTY;return S_OK;}
        return InitPropVariantFromString(url.c_str(),result);
    }
    HRESULT STDMETHODCALLTYPE SetValue(REFPROPERTYKEY,REFPROPVARIANT) override { return E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE Commit() override { return E_ACCESSDENIED; }
};

class KindSelection final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IShellItemArray,IPropertyStore> {
public:
    DWORD count = 100001;
    unsigned stores = 0,values = 0,itemReads = 0;
    GETPROPERTYSTOREFLAGS requested = GPS_DEFAULT;
    HRESULT storeStatus = S_OK,valueStatus = S_OK;
    bool malformed = false;
    std::vector<std::wstring> kinds{L"Music",L"audio"};
    HRESULT STDMETHODCALLTYPE BindToHandler(IBindCtx*,REFGUID,REFIID,void**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPropertyStore(GETPROPERTYSTOREFLAGS flags,REFIID iid,void** result) override {
        ++stores;requested = flags;
        return FAILED(storeStatus) ? storeStatus : QueryInterface(iid,result);
    }
    HRESULT STDMETHODCALLTYPE GetPropertyDescriptionList(REFPROPERTYKEY,REFIID,void**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetAttributes(SIATTRIBFLAGS,SFGAOF,SFGAOF*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCount(DWORD* result) override { if (!result) return E_POINTER; *result=count;return S_OK; }
    HRESULT STDMETHODCALLTYPE GetItemAt(DWORD,IShellItem**) override { ++itemReads;return E_UNEXPECTED; }
    HRESULT STDMETHODCALLTYPE EnumItems(IEnumShellItems**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetAt(DWORD index,PROPERTYKEY* result) override {
        if (!result) return E_POINTER;
        if (index) return E_INVALIDARG;
        *result = PKEY_Kind;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetValue(REFPROPERTYKEY key,PROPVARIANT* result) override {
        ++values;
        if (!result) return E_POINTER;
        if (FAILED(valueStatus)) return valueStatus;
        if (!IsEqualPropertyKey(key,PKEY_Kind)) return E_INVALIDARG;
        if (malformed) { result->vt=VT_I4;result->lVal=1;return S_OK; }
        if (kinds.empty()) { result->vt=VT_EMPTY;return S_OK; }
        std::vector<const wchar_t*> strings;
        for (const auto& kind:kinds) strings.push_back(kind.c_str());
        return InitPropVariantFromStringVector(strings.data(),static_cast<ULONG>(strings.size()),result);
    }
    HRESULT STDMETHODCALLTYPE SetValue(REFPROPERTYKEY,REFPROPVARIANT) override { return E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE Commit() override { return E_ACCESSDENIED; }
};

class CancelSite final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IServiceProvider> {
public:
    explicit CancelSite(std::atomic<unsigned>* destructions):destructions_(destructions) {}
    ~CancelSite() { ++*destructions_; }
    HRESULT STDMETHODCALLTYPE QueryService(REFGUID,REFIID,void** result) override {
        if (!result) return E_POINTER;
        *result=nullptr;return E_NOINTERFACE;
    }
private:
    std::atomic<unsigned>* destructions_;
};

void cancelledStateSiteApartmentLifetime() {
    Fixture fixture;
    for (unsigned attempt=0;attempt<4;++attempt) {
        std::atomic<unsigned> destroyed{0};
        std::atomic<bool> retained{false},cancelled{false},drained{false},closed{false};
        HRESULT status=E_PENDING;
        std::thread creator([&] {
            status=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
            if(FAILED(status))return;
            {
                ComPtr<IShellItem> folder;
                status=SHCreateItemFromParsingName(fixture.root.c_str(),nullptr,IID_PPV_ARGS(&folder));
                ComPtr<IShellItemArray> selection;
                if(SUCCEEDED(status))status=SHCreateShellItemArrayFromShellItem(folder.Get(),IID_PPV_ARGS(&selection));
                auto site=Microsoft::WRL::Make<CancelSite>(&destroyed);
                std::unique_ptr<NamespaceCommandStateTask> task;
                if(SUCCEEDED(status))status=NamespaceCommandStateTask::start(L"Windows.paste",selection.Get(),site.Get(),true,&task);
                if(SUCCEEDED(status)) {
                    site.Reset();retained=destroyed.load()==0;
                    task->cancel();
                    NamespaceCommandState unchanged;unchanged.state=ECS_CHECKED;
                    cancelled=task->poll(&unchanged)==HRESULT_FROM_WIN32(ERROR_CANCELLED)&&unchanged.state==ECS_CHECKED;
                    task.reset();
                    const auto deadline=GetTickCount64()+3000;
                    while(!destroyed.load()&&GetTickCount64()<deadline) {
                        MSG message{};
                        while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
                        Sleep(1);
                    }
                    drained=destroyed.load()==1;
                }
            }
            const auto drain=drainStaWorkers(10000);
            if(SUCCEEDED(status)&&FAILED(drain))status=drain;
            CoUninitialize();closed=true;
        });
        creator.join();
        succeeded(status,"Start real native pending state on an independent owning STA");
        require(retained,"GIT failed to keep the independent COM site alive after owner release");
        require(cancelled,"Cancelled native state changed output or accepted a stale completion");
        require(drained&&closed&&destroyed.load()==1,"Cancelled GIT/site lifetime did not drain before creator apartment shutdown");
    }
}

void entireSelectionFastKindIntersection() {
    auto native = Microsoft::WRL::Make<KindSelection>();
    NamespaceSelectionKinds kinds;
    succeeded(namespaceSelectionKinds(native.Get(),&kinds),"Read every selected kind through actual aggregate property API");
    require(kinds.count == 100001 && kinds.music && !kinds.video,"Large native selection was capped or lost its aggregate media kind");
    require(native->stores == 1 && native->values == 1 && native->itemReads == 0 &&
            native->requested == (GPS_FASTPROPERTIESONLY | GPS_BESTEFFORT),
            "Media classification enumerated items or permitted disk/content/online reads");
    native->kinds={L"video"};
    succeeded(namespaceSelectionKinds(native.Get(),&kinds),"Read aggregate video kind");
    require(kinds.video && !kinds.music,"Video classification retained stale Music context");
    const auto original=kinds;
    native->malformed=true;
    require(namespaceSelectionKinds(native.Get(),&kinds) == HRESULT_FROM_WIN32(ERROR_INVALID_DATA) &&
            kinds.count == original.count && kinds.video == original.video && kinds.music == original.music,
            "Malformed aggregate property changed output or invented media state");
    native->malformed=false;native->storeStatus=E_ACCESSDENIED;
    require(namespaceSelectionKinds(native.Get(),&kinds) == E_ACCESSDENIED && kinds.video,
            "Native property failure was replaced with guessed classification");
    succeeded(namespaceSelectionKinds(nullptr,&kinds),"Null/empty selection has no contextual media page");
    require(!kinds.count && !kinds.music && !kinds.video,"Empty selection retained old media context");
    require(namespaceSelectionKinds(native.Get(),nullptr) == E_POINTER,"Null classification output accepted");

    Fixture fixture;
    std::vector<PIDLIST_ABSOLUTE> owned;
    std::vector<PCIDLIST_ABSOLUTE> pidls;
    struct Cleanup { std::vector<PIDLIST_ABSOLUTE>& list;~Cleanup(){for(auto value:list)CoTaskMemFree(value);} } cleanup{owned};
    for (unsigned index=0;index<257;++index) {
        const auto path=fixture.root/(std::to_wstring(index)+L" owned music.mp3");
        std::ofstream(path,std::ios::binary) << "owned fast-property fixture; never decoded or played";
        const auto shellItem=item(path);
        PIDLIST_ABSOLUTE raw=nullptr;
        succeeded(SHGetIDListFromObject(shellItem.Get(),&raw),"Read owned native media identity");
        owned.push_back(raw);pidls.push_back(raw);
    }
    ComPtr<IShellItemArray> selection;
    succeeded(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(pidls.size()),pidls.data(),&selection),
              "Create actual 257-item media selection");
    succeeded(namespaceSelectionKinds(selection.Get(),&kinds),"Read actual native fast Kind intersection for 257 files");
    require(kinds.count == 257 && kinds.music && !kinds.video,"Actual Music context was disabled above 256 items");
    {
        const auto folder=item(fixture.root);
        HiddenView host(folder.Get());
        NativeNamespaceActions actions;
        succeeded(actions.initialize(host.owner,{folder,selection,host.view}),"Attach entire owned media selection to real Share provider site");
        auto context=folderContext();context.selectionCount=257;
        context.selectionAttributes=SFGAO_FILESYSTEM | SFGAO_CANCOPY;
        AppCommandCapability share;
        succeeded(queryAppCommand(actions,Sharing,context,&share),"Read actual Windows.ModernShare state for 257 files");
        succeeded(share.status,"Actual native Share provider state failed");
        require(share.enabled && share.native.explorerCommand && share.native.initialized && share.native.siteAttached,
                "Native Windows Share was disabled by the removed host count limit");
        NamespaceInvocationPlan nativeShare;
        succeeded(actions.planCommandStore(L"Windows.ModernShare",&nativeShare),"Read native Share menu without displaying recipient UI");
        require(nativeShare.enabled == share.enabled,"Full native Share menu disagrees with direct registered state");
        require(invokeAppNativeCommand(actions,Sharing,context,true) == E_ACCESSDENIED &&
                invokeAppNativeCommand(actions,Sharing,context,false) == E_ACCESSDENIED,
                "Share recipient handler accepted a headless/hidden owner");
        actions.reset();
    }
    const auto unrelated=item(fixture.text);
    PIDLIST_ABSOLUTE raw=nullptr;
    succeeded(SHGetIDListFromObject(unrelated.Get(),&raw),"Read owned non-media identity");
    owned.push_back(raw);pidls.push_back(raw);selection.Reset();
    succeeded(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(pidls.size()),pidls.data(),&selection),"Create mixed native selection");
    succeeded(namespaceSelectionKinds(selection.Get(),&kinds),"Read all-item native kind intersection with non-media item");
    require(kinds.count == 258 && !kinds.music && !kinds.video,"One non-media item was omitted from contextual classification");
    selection.Reset();
}

void actualViewFastStateAndNativePopups() {
    Fixture fixture;
    const auto folder = item(fixture.root);
    HiddenView host(folder.Get());
    NativeNamespaceActions actions;
    succeeded(actions.initialize(host.owner,{folder,{},host.view}),"Attach actual hidden IShellView command site");
    onPrivateDesktop([path = fixture.root] {
        const auto unguardedFolder = item(path);
        HiddenView unguardedHost(unguardedFolder.Get());
        NativeNamespaceActions unguarded;
        succeeded(unguarded.initialize(unguardedHost.owner, {unguardedFolder, {}, unguardedHost.view}),
                  "Attach actual unguarded negative-control view on a non-input desktop");
        require(explorer::PrivateDesktop::current() == nullptr &&
                unguarded.invokeViewSelection(L"Windows.selectall", unguardedHost.view.Get(), true) == E_ACCESSDENIED,
                "Selection-only surface accepted an unguarded headless desktop");
    }, false);
    HRESULT foreignThread=S_OK;
    std::thread selectionWorker([&]{
        const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        foreignThread=actions.invokeViewSelection(L"Windows.selectall",host.view.Get(),true);
        if(SUCCEEDED(com))CoUninitialize();
    });
    selectionWorker.join();
    require(foreignThread==RPC_E_WRONG_THREAD,"Selection-only surface called its attached native view from another apartment");
    const auto context = folderContext();
    for (const auto command : {Undo,Redo,SortMenu,GroupMenu,ColumnsMenu,Paste,PasteShortcut}) {
        AppCommandCapability capability;
        succeeded(queryAppCommand(actions,command,context,&capability),"Read direct GetState(FALSE) provider capability");
        if (capability.status == E_PENDING) {
            require(!capability.enabled,"Pending native provider was guessed enabled");
            std::unique_ptr<NamespaceCommandStateTask> pending;
            succeeded(actions.startCommandStateTask(appCommandStoreName(command),&pending,NamespaceMenuScope::Background),
                      "Start correctly marshaled native slow-state worker");
            const auto deadline = GetTickCount64()+5000;
            do {
                MSG message{};
                while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
                capability.status = pending->poll(&capability.native);
                if (capability.status != E_PENDING) break;
                Sleep(1);
            } while (GetTickCount64()<deadline);
            capability.enabled = SUCCEEDED(capability.status) && capability.native.enabled();
            capability.checked = SUCCEEDED(capability.status) && capability.native.checked();
        }
        succeeded(capability.status,"Actual view provider failed fast state query");
        require(capability.native.handler != CLSID_NULL,"Fast view state lost actual registered handler");
        if (capability.native.explorerCommand) require(capability.native.siteAttached,"Fast Explorer view provider lost actual site");
        NamespaceInvocationPlan full;
        succeeded(actions.planCommandStore(appCommandStoreName(command),&full,NamespaceMenuScope::Background),"Read native composed menu for state equivalence");
        require(capability.enabled == full.enabled && capability.checked == full.checked,"Fast provider state differs from native IContextMenu state");
    }
    for (const auto command : {SortMenu,GroupMenu,ColumnsMenu}) {
        NamespaceCommandPopup popup;
        succeeded(queryAppCommandPopup(actions,command,context,&popup),"Read native property cascade without displaying it");
        require(popup.menu && IsMenu(popup.menu) && popup.plan.submenu && popup.plan.enabled,"Native property popup is not the actual provider submenu");
        require(GetMenuItemCount(popup.menu) == static_cast<int>(popup.entries.size()),"Copied property metadata differs from actual borrowed menu");
        require(popup.entries.size()>4,"Native property popup is still limited to four hardcoded keys");
        for (const auto& entry : popup.entries) if (!entry.separator())
            require(!entry.label.empty(),"Native property label missing");
        require(actions.invokeCommandStorePopup(popup,true) == E_ACCESSDENIED &&
                actions.invokeCommandStorePopup(popup,false) == E_ACCESSDENIED,"Hidden provider submenu displayed/invoked");
    }
    AppCommandCapability search;
    succeeded(queryAppCommand(actions,RibbonSearchContents,context,&search),"Query Search option semantic guard");
    require(!search.enabled && FAILED(search.status),"Native Search option became enabled merely because provider exists");
    NamespaceCommandPopup unchanged;
    unchanged.generation = 456;
    require(queryAppCommandPopup(actions,Open,context,&unchanged) == E_INVALIDARG && unchanged.generation == 456,"Host leaf accepted as native cascade");
    require(!IsWindowVisible(host.owner),"Native state/menu enumeration displayed fixture host");
    std::unique_ptr<NamespaceCommandStateTask> cancelled;
    succeeded(actions.startCommandStateTask(L"Windows.SortByColumn",&cancelled,NamespaceMenuScope::Background),"Start cancellable native query");
    cancelled->cancel();
    NamespaceCommandState preserved;
    preserved.state = ECS_CHECKED;
    require(cancelled->poll(&preserved) == HRESULT_FROM_WIN32(ERROR_CANCELLED) && preserved.state == ECS_CHECKED,
            "Cancelled/stale native query published a result");
    HRESULT wrongThread = S_OK;
    std::thread foreign([&]{ wrongThread = cancelled->poll(&preserved); });
    foreign.join();
    require(wrongThread == RPC_E_WRONG_THREAD,"Native query result crossed its creating STA");
    require(NamespaceCommandStateTask::start(L"Windows.Bad\\Key",nullptr,nullptr,false,&cancelled) == E_INVALIDARG &&
            cancelled->poll(&preserved) == HRESULT_FROM_WIN32(ERROR_CANCELLED),"Invalid async query replaced an existing task");
    cancelled.reset();
    actions.reset();
}

void nativeSearchLocationDelegatesAndTargets() {
    // A fresh worker attaches a private desktop before creating COM/any HWND.
    // Providers are only asked for state/menu metadata; no association opens.
    std::exception_ptr failure;
    std::atomic<bool> complete{false};
    std::thread worker([&] {
        HDESK previous = GetThreadDesktop(GetCurrentThreadId()),desktop = nullptr;
        bool initialized = false;
        try {
            GUID id{}; succeeded(CoCreateGuid(&id),"Private Search-location desktop identity");
            wchar_t name[40]{};
            require(StringFromGUID2(id,name,40) != 0,"Format private Search-location desktop identity");
            desktop = CreateDesktopW((std::wstring(L"ExplorerSearchLocation-")+name).c_str(),nullptr,nullptr,0,GENERIC_ALL,nullptr);
            require(desktop && SetThreadDesktop(desktop),"Attach private Search-location desktop before native initialization");
            succeeded(OleInitialize(nullptr),"Initialize private Search-location STA");
            initialized = true;
            {
                Fixture fixture;
                const std::array<fs::path,3> paths{fixture.root/L"needle-first.txt",fixture.root/L"needle-second.txt",
                                                fixture.directory/L"needle-third.txt"};
                for (const auto& path : paths) std::ofstream(path,std::ios::binary) << "owned native location fixture";
                const auto scope = item(fixture.root);
                ComPtr<IShellItem> search;
                succeeded(createSearchFolder(L"System.FileName:needle",scope.Get(),&search),"Create exclusively owned native result scope");
                HiddenView host(search.Get());
                ComPtr<IFolderView2> view;
                succeeded(host.view.As(&view),"Read actual native Search view interface");
                int count = 0;
                const auto deadline = GetTickCount64()+5000;
                while (count < 3 && GetTickCount64() < deadline) {
                    MSG message{};
                    while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
                    succeeded(view->ItemCount(SVGIO_ALLVIEW,&count),"Read actual owned Search result count");
                    if (count < 3) Sleep(1);
                }
                require(count == 3,"Native Search did not return the exact three owned results");
                std::array<int,3> indexes{-1,-1,-1};
                for (int index=0;index<count;++index) {
                    ComPtr<IShellItem> result;
                    succeeded(view->GetItem(index,IID_PPV_ARGS(&result)),"Read native result wrapper without replacing its parent");
                    PWSTR raw = nullptr;
                    succeeded(result->GetDisplayName(SIGDN_FILESYSPATH,&raw),"Read exclusively owned result identity");
                    const fs::path path(raw ? raw : L""); CoTaskMemFree(raw);
                    for (size_t expected=0;expected<paths.size();++expected)
                        if (fs::equivalent(path, paths[expected])) indexes[expected]=index;
                }
                for (const auto index : indexes) require(index >= 0,"Native Search wrapper lost an owned result identity");
                ComPtr<IShellItem> current;
                succeeded(view->GetFolder(IID_PPV_ARGS(&current)),"Read actual Search provider parent");
                for (DWORD selected=0;selected<=3;++selected) {
                    succeeded(host.view->SelectItem(nullptr,SVSI_DESELECTOTHERS),"Clear only the hidden fixture selection");
                    for (DWORD index=0;index<selected;++index)
                        succeeded(view->SelectItem(indexes[index],SVSI_SELECT),"Select exact native same/different-folder result wrapper");
                    ComPtr<IShellItemArray> targets;
                    const HRESULT collected = view->Items(SVGIO_SELECTION,IID_PPV_ARGS(&targets));
                    if (selected) succeeded(collected,"Collect complete actual native Search selection");
                    NativeNamespaceActions actions;
                    succeeded(actions.initialize(host.owner,{current,targets,host.view}),"Keep native Search selection and view site intact");
                    AppCommandContext context;
                    context.folderView = context.search = context.searchBackground = true;
                    context.selectionCount = selected; // No synthetic filesystem attribute is supplied.
                    AppCommandCapability openSite;
                    succeeded(queryAppCommand(actions,RibbonOpenSearchViewSite,context,&openSite),"Read actual conditional Search-again view-site leaf");
                    NamespaceInvocationPlan siteMenu;
                    const auto sitePlan=actions.planCommandStore(L"Windows.OpenSearchViewSite",&siteMenu,NamespaceMenuScope::Background);
                    require(openSite.enabled==(SUCCEEDED(sitePlan)&&siteMenu.enabled),
                            "Search view-site capability lost its actual current-query folder and native menu");
                    require(invokeAppNativeCommand(actions,RibbonOpenSearchViewSite,context,true)==E_ACCESSDENIED,
                            "Search view-site capability probe launched an associated search window");
                    AppCommandCapability capability;
                    succeeded(queryAppCommand(actions,OpenFileLocation,context,&capability),"Query actual location leaf authority");
                    NamespaceCommandState direct;
                    const HRESULT native = namespaceCommandState(L"Windows.SearchOpenLocation",targets.Get(),host.view.Get(),&direct);
                    NamespaceInvocationPlan menu;
                    const HRESULT planned = actions.planCommandStore(L"Windows.SearchOpenLocation",&menu);
                    const bool enabled = SUCCEEDED(planned) && menu.enabled;
                    require(capability.enabled == enabled,"Native delegated capability differs from actual composed Search menu");
                    if (selected) require(capability.status == native,"Catalog replaced the native leaf HRESULT");
                    if (SUCCEEDED(native)) {
                        require(!direct.delegatedCommand.empty() && direct.initialized,
                                "Composite state was guessed without an actual initialized installed leaf");
                        NamespaceCommandState leaf;
                        succeeded(namespaceCommandState(direct.delegatedCommand,targets.Get(),host.view.Get(),&leaf),
                                  "Independently corroborate retained delegated state");
                        require(direct.state == leaf.state && direct.handler == leaf.handler,"Composite state differs from native child authority");
                    } else {
                        NamespaceCommandState unchanged;
                        unchanged.state = ECS_CHECKED; unchanged.delegatedCommand = L"preserved";
                        require(namespaceCommandState(L"Windows.SearchOpenLocation",targets.Get(),host.view.Get(),&unchanged) == native &&
                                unchanged.state == ECS_CHECKED && unchanged.delegatedCommand == L"preserved",
                                "A failed native location query altered caller output");
                    }
                    require(invokeAppNativeCommand(actions,OpenFileLocation,context,true) == E_ACCESSDENIED &&
                            (!selected || invokeAppNativeCommand(actions,OpenFileLocation,context,false) == E_ACCESSDENIED),
                            "Location action launched a window from a hidden/headless fixture");
                    actions.reset();
                }
                ComPtr<IShellItem> controlPanel;
                succeeded(SHGetKnownFolderItem(FOLDERID_ControlPanelFolder,KF_FLAG_DEFAULT,nullptr,IID_PPV_ARGS(&controlPanel)),
                          "Obtain static virtual Control Panel identity without enumerating user items");
                for (auto target : {controlPanel.Get(),search.Get()}) {
                    NativeNamespaceActions actions;
                    const auto targets = array(target);
                    succeeded(actions.initialize(host.owner,{current,targets,host.view}),"Retain actual virtual/query metadata targets");
                    AppCommandContext context;
                    context.folderView = context.searchBackground = true; context.selectionCount = 1;
                    AppCommandCapability capability;
                    succeeded(queryAppCommand(actions,OpenFileLocation,context,&capability),"Ask providers about static virtual targets");
                    NamespaceInvocationPlan menu;
                    const auto planned = actions.planCommandStore(L"Windows.SearchOpenLocation",&menu);
                    require(capability.enabled == (SUCCEEDED(planned) && menu.enabled),
                            "Virtual eligibility was inferred from filesystem paths instead of native providers");
                    require(invokeAppNativeCommand(actions,OpenFileLocation,context,true) == E_ACCESSDENIED,
                            "Virtual location probe invoked associated UI");
                    actions.reset();
                }
                require(!IsWindowVisible(host.owner),"Read-only Search providers displayed the owned host");
            }
        } catch (...) { failure = std::current_exception(); }
        if (initialized) OleUninitialize();
        if (desktop && SetThreadDesktop(previous)) CloseDesktop(desktop);
        complete.store(true);
    });
    while (!complete.load()) {
        MSG message{};
        while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        Sleep(1);
    }
    worker.join();
    if (failure) std::rethrow_exception(failure);
}

void nativeAdditionalToolsAndRetainedHierarchy() {
    onPrivateDesktop([] {
        Fixture fixture;
        const auto folder=item(fixture.root),text=item(fixture.text),directory=item(fixture.directory);
        HiddenView host(folder.Get());
        NativeNamespaceActions actions;
        ComPtr<IShellItem> network;
        succeeded(SHGetKnownFolderItem(FOLDERID_NetworkFolder,KF_FLAG_DEFAULT,nullptr,IID_PPV_ARGS(&network)),
                  "Read static Network identity without enumerating devices");
        succeeded(actions.initialize(host.owner,{network,{},host.view}),"Attach native Network background identity");
        auto context=folderContext();context.network=true;context.physicalDirectory=false;
        for(const auto command:{RibbonAddNetworkDevice,RibbonSearchActiveDirectory,RibbonNetworkSharingCenter}) {
            AppCommandCapability capability;
            succeeded(queryAppCommand(actions,command,context,&capability),"Read actual registered Network capability");
            NamespaceInvocationPlan menu;
            const auto planned=actions.planCommandStore(appCommandStoreName(command),&menu,NamespaceMenuScope::Background);
            require(capability.enabled==(SUCCEEDED(planned)&&menu.enabled),"Network capability differs from actual native background menu");
            require(invokeAppNativeCommand(actions,command,context,true)==E_ACCESSDENIED &&
                    invokeAppNativeCommand(actions,command,context,false)==E_ACCESSDENIED,
                    "Network capability probe opened discovery/domain/Control Panel UI");
        }
        actions.reset();
        succeeded(actions.initialize(host.owner,{folder,{},host.view}),"Attach owned view for native delete-confirmation state");
        context=folderContext();
        for (const auto command : {RibbonGroupSortAscending,RibbonGroupSortDescending}) {
            AppCommandCapability groupOrder;
            succeeded(queryAppCommand(actions,command,context,&groupOrder),"Read native group ordering without changing hidden view");
            NamespaceInvocationPlan groupMenu;
            const auto planned=actions.planCommandStore(appCommandStoreName(command),&groupMenu,NamespaceMenuScope::Background);
            require(groupOrder.enabled==(SUCCEEDED(planned)&&groupMenu.enabled),
                    "Group ascending/descending state differs from actual native view menu");
            require(invokeAppNativeCommand(actions,command,context,true)==E_ACCESSDENIED,
                    "Group-state capability probe invoked a provider");
        }
        AppCommandCapability confirmation;
        succeeded(queryAppCommand(actions,RibbonDeleteConfirmation,context,&confirmation),"Read actual delete-confirmation checkbox");
        succeeded(confirmation.status,"Delete-confirmation provider unavailable");
        NamespaceCommandState original;
        succeeded(actions.queryCommandState(L"Windows.ToggleRecycleConfirmations",&original,NamespaceMenuScope::Background),
                  "Independently read native confirmation authority");
        require(confirmation.enabled==original.enabled()&&confirmation.checked==original.checked(),
                "Native checked state was replaced by the generic menu's missing checkbox");
        actions.reset();

        ComPtr<IShellItem> computer;
        succeeded(SHGetKnownFolderItem(FOLDERID_ComputerFolder,KF_FLAG_DEFAULT,nullptr,IID_PPV_ARGS(&computer)),
                  "Read actual Computer identity without opening discovery UI");
        {
            HiddenView computerHost(computer.Get());
            succeeded(actions.initialize(computerHost.owner,{computer,{},computerHost.view}),
                      "Attach actual native Computer view site for media-server commands");
            for(const auto key:{L"Windows.AddMediaServer",L"Windows.RemoveMediaServer"}) {
                NamespaceCommandState media;
                succeeded(actions.queryCommandState(key,&media,NamespaceMenuScope::Background),
                          "Read exact installed media-server provider without discovering or removing devices");
                NamespaceInvocationPlan menu;
                const auto planned=actions.planCommandStore(key,&menu,NamespaceMenuScope::Background);
                require(media.enabled()==(SUCCEEDED(planned)&&menu.enabled),
                        "Media-server capability differs from actual native Computer menu");
                require(actions.invokeCommandStore(key,true,{},NamespaceMenuScope::Background)==E_ACCESSDENIED,
                        "Media-server state probe invoked device discovery or removal");
            }
            actions.reset();
        }

        const auto exePath=fixture.root/L"owned never executed.exe",linkPath=fixture.root/L"owned shortcut.lnk";
        std::ofstream(exePath,std::ios::binary)<<"owned association fixture; never executed";
        ComPtr<IShellLinkW> nativeLink;
        succeeded(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&nativeLink)),"Create only owned real shortcut");
        succeeded(nativeLink->SetPath(fixture.text.c_str()),"Set owned shortcut target");
        ComPtr<IPersistFile> persist;succeeded(nativeLink.As(&persist),"Persist owned shortcut through public Shell API");
        succeeded(persist->Save(linkPath.c_str(),FALSE),"Save owned shortcut without starting its target");
        persist.Reset();nativeLink.Reset();
        const auto exe=item(exePath),link=item(linkPath);
        const auto executableNamedFolder=fixture.root/L"owned directory.exe";
        require(fs::create_directory(executableNamedFolder),"Create owned directory with executable-looking extension");
        for(const auto& target:std::array<fs::path,4>{fixture.text,exePath,fixture.directory,executableNamedFolder}) {
            const auto shortcut=fixture.root/(target.filename().wstring()+L" semantic target.lnk");
            ComPtr<IShellLinkW> native;
            succeeded(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&native)),"Create owned target-classification shortcut");
            succeeded(native->SetPath(target.c_str()),"Set only owned shortcut metadata target");
            ComPtr<IPersistFile> writer;succeeded(native.As(&writer),"Read owned shortcut writer");
            succeeded(writer->Save(shortcut.c_str(),FALSE),"Persist owned target-classification shortcut");
            writer.Reset();native.Reset();
            succeeded(actions.initialize(host.owner,{folder,array(item(shortcut).Get()),host.view}),
                      "Classify actual cached shortcut target without resolving or opening it");
            require(actions.facts().applications==(target==exePath),
                    "A document/folder shortcut incorrectly acquired Application Tools or an executable shortcut lost it");
            actions.reset();
        }
        for(auto selected:{text.Get(),exe.Get(),link.Get(),network.Get()}) {
            succeeded(actions.initialize(host.owner,{folder,array(selected),host.view}),"Keep actual selected namespace/association target");
            context=folderContext();context.selectionCount=1;
            for(const auto command:{RibbonShortcutOpenLocation,RibbonConnectRemotePrinter,RibbonDeviceWebpage,
                                    RibbonRemoveProperties,RibbonRunAsAnotherUser}) {
                AppCommandCapability capability;
                succeeded(queryAppCommand(actions,command,context,&capability),"Read real selected-target capability");
                NamespaceInvocationPlan menu;
                const auto planned=actions.planCommandStore(appCommandStoreName(command),&menu);
                if(command==RibbonRemoveProperties||command==RibbonRunAsAnotherUser) {
                    NamespaceCommandState association;
                    const auto native=actions.queryStaticVerbState(command==RibbonRemoveProperties?L"removeproperties":L"runasuser",&association);
                    require(capability.status==native&&capability.enabled==(SUCCEEDED(native)&&association.enabled()),
                            "Static wrapper ignored the actual selected native association");
                    if(capability.enabled)require(SUCCEEDED(planned)&&menu.enabled,"Available native association has no registered invocation wrapper");
                } else require(capability.enabled==(SUCCEEDED(planned)&&menu.enabled),"Selected capability differs from actual registered native menu");
                require(invokeAppNativeCommand(actions,command,context,true)==E_ACCESSDENIED,
                        "Selected tool probe invoked a target, credentials, browser or metadata dialog");
            }
            actions.reset();
        }

        auto properties=Microsoft::WRL::Make<PresentationSelection>(array(text.Get()).Get());
        succeeded(actions.initialize(host.owner,{folder,properties,host.view}),"Attach owned property-contract selection");
        NamespaceCommandState state;
        succeeded(actions.queryRegisteredComponentState(L"Windows.NetworkViewDeviceWebpage",&state),"Empty device URL property remains disabled");
        require(!state.enabled(),"Device webpage enabled without actual nonempty property");
        properties->url=L"https://example.invalid/never-contacted-headless-fixture";
        succeeded(actions.queryRegisteredComponentState(L"Windows.NetworkViewDeviceWebpage",&state),"Verify actual static property rule and registered protocol");
        require(state.enabled()&&properties->requested==(GPS_FASTPROPERTIESONLY|GPS_BESTEFFORT),
                "Static property verb ignored its native rule or permitted content/device hydration");
        properties->malformed=true;
        state.state=ECS_CHECKED;
        require(actions.queryRegisteredComponentState(L"Windows.NetworkViewDeviceWebpage",&state)==HRESULT_FROM_WIN32(ERROR_INVALID_DATA)&&state.state==ECS_CHECKED,
                "Malformed property enabled a native tool or altered failure output");
        properties->malformed=false;properties->propertyStatus=E_ACCESSDENIED;
        require(actions.queryRegisteredComponentState(L"Windows.NetworkViewDeviceWebpage",&state)==E_ACCESSDENIED&&state.state==ECS_CHECKED,
                "Provider property failure was guessed or changed caller output");
        actions.reset();

        // All destinations stay inside this fixture; no user Libraries/defaults
        // are written. Public and private defaults are persisted independently.
        explorer::ShellLibrary library;
        succeeded(explorer::ShellLibrary::create(library),"Create owned library for actual public save-location provider");
        succeeded(library.addFolder(fixture.root),"Include owned private library location");
        succeeded(library.addFolder(fixture.directory),"Include owned public library location");
        succeeded(library.setDefaultSaveFolder(fixture.root,DSFT_PRIVATE),"Set only owned private default");
        succeeded(library.setDefaultSaveFolder(fixture.directory,DSFT_PUBLIC),"Set only owned public default");
        ComPtr<IShellItem> saved;
        succeeded(library.save(fixture.root,L"owned public save location",saved),"Save owned library without changing user's library descriptions");
        library=explorer::ShellLibrary{};
        {
            HiddenView libraryHost(saved.Get());
            succeeded(actions.initialize(libraryHost.owner,{saved,{},libraryHost.view}),"Attach actual owned native library view site");
            context=folderContext();context.library=context.writableLibrary=true;
            context.physicalDirectory=false;
            for(const auto command:std::array<UINT,3>{NewFolder,NewItems,RibbonNewMenu}) {
                AppCommandCapability creation;
                succeeded(queryAppCommand(actions,command,context,&creation),"Read native new-item eligibility in real owned Library");
                NamespaceInvocationPlan native;
                const auto planned=actions.planCommandStore(appCommandStoreName(command),&native,NamespaceMenuScope::Background);
                require(creation.enabled && creation.enabled==(SUCCEEDED(planned)&&native.enabled),
                        "Library default-folder creation was disabled by host filesystem guesses");
                require(invokeAppNativeCommand(actions,command,context,true)==E_ACCESSDENIED,
                        "Library eligibility probe created an item through interactive native UI");
            }
            AppCommandCapability publicSave;
            succeeded(queryAppCommand(actions,RibbonLibraryPublicSaveLocation,context,&publicSave),"Read actual public save-location capability");
            NamespaceInvocationPlan menu;
            const auto planned=actions.planCommandStore(L"Windows.LibraryPublicSaveLocation",&menu,NamespaceMenuScope::Background);
            require(publicSave.enabled==(SUCCEEDED(planned)&&menu.enabled),"Public default capability differs from native Library provider");
            std::unique_ptr<NativeNamespaceCommandChildren> children;
            succeeded(actions.queryCommandChildren(L"Windows.LibraryPublicSaveLocation",&children,NamespaceMenuScope::Background),
                      "Retain actual native public save-location commands");
            require(children&&children->entries().size()>=2,"Native public/default destinations were replaced with fixed host choices");
            const std::array<size_t,1> path{0};
            require(children->invokePath(path,true)==E_ACCESSDENIED&&children->invokePath(path,false)==E_ACCESSDENIED,
                    "Public/default location probe modified library settings");
            children.reset();actions.reset();
        }

        {
            const auto zip=item(fixture.archive);HiddenView zipHost(zip.Get());
            succeeded(actions.initialize(zipHost.owner,{zip,{},zipHost.view}),"Attach actual owned ZIP New-menu state");
            context=folderContext();context.physicalDirectory=false;context.archive=true;
            AppCommandCapability creation;
            succeeded(queryAppCommand(actions,RibbonNewMenu,context,&creation),"Read real provider-backed Host New-menu capability");
            NamespaceCommandState native;
            const auto status=actions.queryCommandState(L"Windows.newitem",&native,NamespaceMenuScope::Background);
            require(creation.status==status && creation.enabled==(SUCCEEDED(status)&&native.enabled()),
                    "Host New menu invented availability instead of preserving the actual ZIP provider");
            require(!creation.enabled,"Read-only compressed namespace unexpectedly admitted new-item creation");
            actions.reset();
        }

        succeeded(actions.initialize(host.owner,{folder,array(directory.Get()),host.view}),"Attach owned native cascade selection/view");
        unsigned captured=0,nested=0;
        for(const auto& [command,scope]:std::array<std::pair<std::wstring_view,NamespaceMenuScope>,9>{{
                {L"Windows.CopyToMenu",NamespaceMenuScope::Selection},{L"Windows.MoveToMenu",NamespaceMenuScope::Selection},
                {L"Windows.LibraryIncludeInLibrary",NamespaceMenuScope::Selection},{L"Windows.SortByColumn",NamespaceMenuScope::Background},
                {L"Windows.GroupByColumn",NamespaceMenuScope::Background},{L"Windows.AddColumns",NamespaceMenuScope::Background},
                {L"Windows.OpenWith",NamespaceMenuScope::Selection},{L"Windows.PlayTo",NamespaceMenuScope::Selection},
                {L"Windows.SearchFilterDate",NamespaceMenuScope::Background}}}) {
            std::unique_ptr<NativeNamespaceCommandChildren> children;
            const auto status=actions.queryCommandChildren(command,&children,scope);
            if(FAILED(status)){require(!children,"Failed cascade capture returned partial mutable commands");continue;}
            ++captured;
            std::function<void(const std::vector<NamespaceSubcommandMetadata>&,std::vector<size_t>)> check;
            check=[&](const auto& entries,auto path) {
                for(size_t index=0;index<entries.size();++index) {
                    auto identity=path;identity.push_back(index);
                    require(children->invokePath(identity,true)==E_ACCESSDENIED&&children->invokePath(identity,false)==E_ACCESSDENIED,
                            "Retained nested cascade crossed hidden/headless activation guard");
                    if(!entries[index].children.empty()){++nested;require((entries[index].flags&ECF_HASSUBCOMMANDS)!=0,"Nested metadata lost actual native cascade flag");}
                    check(entries[index].children,std::move(identity));
                }
            };
            check(children->entries(),{});
        }
        require(captured>=4,"Native installed cascade providers could not be retained through their public API");
        std::cout<<"Read-only native cascades captured="<<captured<<" nested="<<nested<<"; destination labels/paths omitted\n";
        actions.reset();

        ComPtr<IShellItem> documents;
        succeeded(SHGetKnownFolderItem(FOLDERID_Documents,KF_FLAG_DONT_VERIFY,nullptr,IID_PPV_ARGS(&documents)),
                  "Read selected known-folder identity without enumerating its contents");
        for(auto selected:{text.Get(),directory.Get(),documents.Get()}) {
            succeeded(actions.initialize(host.owner,{folder,array(selected),host.view}),"Attach actual owned/known-folder backup target metadata");
            context=folderContext();context.selectionCount=1;
            AppCommandCapability backup;
            succeeded(queryAppCommand(actions,RibbonCloudBackup,context,&backup),"Read actual Cloud Backup provider eligibility");
            NamespaceInvocationPlan menu;
            const auto planned=actions.planCommandStore(L"Windows.CloudBackup",&menu);
            require(backup.enabled==(SUCCEEDED(planned)&&menu.enabled),"Backup context was inferred from latent markup or a folder extension");
            require(invokeAppNativeCommand(actions,RibbonCloudBackup,context,true)==E_ACCESSDENIED,
                    "Cloud Backup capability probe launched setup or altered user backup settings");
            actions.reset();
        }
        NamespaceCommandState unchanged;
        succeeded(namespaceCommandState(L"Windows.ToggleRecycleConfirmations",nullptr,host.view.Get(),&unchanged),
                  "Confirm settings were untouched after all read-only probes");
        require(unchanged.state==original.state,"Read-only tool probes changed global delete-confirmation policy");
        require(!IsWindowVisible(host.owner),"Read-only provider fixture displayed its owner");
    });
}

void nativeDeletionStateAndUntouchedPayloads() {
    onPrivateDesktop([] {
        Fixture fixture;const auto textBefore=read(fixture.text),zipBefore=read(fixture.archive);
        const auto clipboard=GetClipboardSequenceNumber();
        const auto folder=item(fixture.root);HiddenView host(folder.Get());
        const auto text=item(fixture.text),directory=item(fixture.directory);
        struct Identities {
            PIDLIST_ABSOLUTE text=nullptr,directory=nullptr;
            ~Identities(){CoTaskMemFree(text);CoTaskMemFree(directory);}
        } identities;
        succeeded(SHGetIDListFromObject(text.Get(),&identities.text),"Retain owned deletion-state file identity");
        succeeded(SHGetIDListFromObject(directory.Get(),&identities.directory),"Retain owned deletion-state folder identity");
        std::array<PCIDLIST_ABSOLUTE,2> ids{identities.text,identities.directory};
        NativeNamespaceActions actions;
        for(const UINT count:{1u,2u}) {
            ComPtr<IShellItemArray> selected;
            succeeded(SHCreateShellItemArrayFromIDLists(count,ids.data(),&selected),"Keep complete original deletion-state selection array");
            succeeded(actions.initialize(host.owner,{folder,selected,host.view}),"Attach actual native deletion provider selection/site");
            auto context=folderContext();context.selectionCount=count;
            succeeded(selected->GetAttributes(SIATTRIBFLAGS_AND,SFGAO_CANDELETE,&context.selectionAttributes),"Read actual native delete capability attributes");
            for(const auto command:{Delete,PermanentDelete}) {
                AppCommandCapability capability;
                succeeded(queryAppCommand(actions,command,context,&capability),"Read actual native deletion state without invoking it");
                if(capability.status==E_PENDING) {
                    require(!capability.enabled,"Pending deletion provider was invented enabled");
                    std::unique_ptr<NamespaceCommandStateTask> pending;
                    succeeded(actions.startCommandStateTask(appCommandStoreName(command),&pending,NamespaceMenuScope::Selection),"Complete only registered deletion capability on private worker STA");
                    const auto deadline=GetTickCount64()+5000;
                    do {
                        MSG message{};
                        while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
                        capability.status=pending->poll(&capability.native);
                        if(capability.status!=E_PENDING)break;
                        Sleep(1);
                    }while(GetTickCount64()<deadline);
                    capability.enabled=SUCCEEDED(capability.status)&&capability.native.enabled();
                }
                succeeded(capability.status,"Actual native deletion capability did not finish read-only state");
                require(capability.native.handler!=CLSID_NULL,"Deletion capability lost its exact registered native handler");
                NamespaceInvocationPlan plan;
                succeeded(actions.planCommandStore(appCommandStoreName(command),&plan,NamespaceMenuScope::Selection),"Read exact native deletion menu ordinal without activation");
                require(capability.enabled==plan.enabled && !plan.submenu && plan.commandId && plan.canonicalVerb==appCommandStoreName(command),
                        "Native deletion fast state differs from its actual registered menu/selection");
                require(invokeAppNativeCommand(actions,command,context,true)==E_ACCESSDENIED &&
                        invokeAppNativeCommand(actions,command,context,false)==E_ACCESSDENIED,
                        "Read-only deletion probe reached files or the user's Bin through a hidden owner");
            }
            actions.reset();
        }
        require(read(fixture.text)==textBefore && read(fixture.archive)==zipBefore && fs::is_directory(fixture.directory),
                "Native deletion state/menu probes changed owned payloads");
        require(GetClipboardSequenceNumber()==clipboard,"Deletion capability inspection changed clipboard state");
        succeeded(explorer::PrivateDesktop::current()->verifyIsolation(),"Confirm deletion state probes retained private desktop isolation");
    });
}

void nativeMultipleAccessStateAndUntouchedPermissions() {
    onPrivateDesktop([] {
        Fixture fixture;const auto textBefore=read(fixture.text),zipBefore=read(fixture.archive);
        const auto security=[](const fs::path& path) {
            constexpr SECURITY_INFORMATION wanted=OWNER_SECURITY_INFORMATION|GROUP_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION;
            DWORD length=0;GetFileSecurityW(path.c_str(),wanted,nullptr,0,&length);
            require(length&&length<=64*1024,"Owned access security snapshot exceeds its read-only budget");
            std::vector<BYTE> result(length);
            require(GetFileSecurityW(path.c_str(),wanted,result.data(),length,&length)!=FALSE,"Read only owned access owner/group/DACL");
            result.resize(length);return result;
        };
        const auto fileSecurity=security(fixture.text),folderSecurity=security(fixture.directory);
        const auto folder=item(fixture.root),text=item(fixture.text),directory=item(fixture.directory);
        HiddenView host(folder.Get());
        struct Identities {PIDLIST_ABSOLUTE text=nullptr,directory=nullptr;~Identities(){CoTaskMemFree(text);CoTaskMemFree(directory);}} identities;
        succeeded(SHGetIDListFromObject(text.Get(),&identities.text),"Retain owned access-state file identity");
        succeeded(SHGetIDListFromObject(directory.Get(),&identities.directory),"Retain owned access-state folder identity");
        std::array<PCIDLIST_ABSOLUTE,2> ids{identities.text,identities.directory};
        ComPtr<IShellItemArray> selected;
        succeeded(SHCreateShellItemArrayFromIDLists(2,ids.data(),&selected),"Retain full mixed file/folder native access selection");
        NativeNamespaceActions actions;
        succeeded(actions.initialize(host.owner,{folder,selected,host.view}),"Attach native multiple-item access target/site");
        require(actions.facts().selectionCount==2&&actions.facts().filesystem&&actions.facts().singlePath.empty(),
                "Multiple-item access lost original targets or became a single-path guess");
        for(const auto action:{NamespaceAction::ShareSpecificPeople,NamespaceAction::RemoveAccess}) {
            require(namespaceActionApplicable(action,actions.facts()),"A host single-item gate disabled native multiple-item sharing");
            NamespaceCommandState capability,native;
            succeeded(actions.queryActionState(action,&capability),"Read native multiple-item access state without ACL changes");
            succeeded(actions.queryCommandState(namespaceActionCommand(action),&native,NamespaceMenuScope::Selection),"Read exact registered access provider over original full array");
            NamespaceInvocationPlan plan;
            succeeded(actions.planInvocation(action,&plan),"Read native multiple-item access menu ordinal without invocation");
            require(capability.state==native.state&&capability.enabled()==plan.enabled&&plan.commandId,
                    "Multiple access state differs from actual native selection/menu authority");
            require(capability.enabled(),"Installed native access provider rejected the supported owned mixed selection");
            require(actions.invoke(action,true)==E_ACCESSDENIED&&actions.invoke(action,false)==E_ACCESSDENIED,
                    "Read-only access capability probe reached ACLs, sharing or recipient UI");
        }
        actions.reset();
        require(read(fixture.text)==textBefore&&read(fixture.archive)==zipBefore&&fs::is_directory(fixture.directory),
                "Native multiple-item access inspection changed owned payloads");
        require(security(fixture.text)==fileSecurity&&security(fixture.directory)==folderSecurity,
                "Read-only native access queries changed owned owner/group/DACL state");
        succeeded(explorer::PrivateDesktop::current()->verifyIsolation(),"Verify input desktop isolation after access-state queries");
    });
}

void realAssociationsAndZipCapabilities() {
    Fixture fixture;
    const auto textBefore = read(fixture.text),zipBefore = read(fixture.archive);
    const auto folder = item(fixture.root),directory = item(fixture.directory),zip = item(fixture.archive);
    NativeNamespaceActions actions;
    succeeded(actions.initialize(nullptr,{folder,array(directory.Get()),{}}),"Initialize real directory association target");
    auto context = folderContext();
    context.selectionCount = 1;
    context.selectionAttributes = SFGAO_FILESYSTEM | SFGAO_FOLDER;
    for (const auto command : {Open,Edit,Print}) {
        AppCommandCapability capability;
        succeeded(queryAppCommand(actions,command,context,&capability),"Read actual static association capability");
        require(capability.enabled == (command == Open),"Folder Open/Edit/Print state does not reflect actual native association");
    }
    NamespaceCommandState state;
    for (const auto command : {CopyTo,MoveTo,HideSelected}) {
        context.selectionAttributes = SFGAO_FILESYSTEM | SFGAO_FOLDER | SFGAO_CANCOPY | SFGAO_CANMOVE;
        AppCommandCapability capability;
        succeeded(queryAppCommand(actions,command,context,&capability),"Read actual selected-folder provider state");
        succeeded(capability.status,"Installed copy/move/attributes provider failed fast state");
        NamespaceInvocationPlan native;
        succeeded(actions.planCommandStore(appCommandStoreName(command),&native),"Read actual selected-folder native command state");
        require(capability.enabled == native.enabled,"Fast selected-folder provider state differs from native menu");
    }
    const auto zipCreation = actions.queryZipState(&state);
    if (SUCCEEDED(zipCreation)) require(state.enabled(),"Actual native ZIP SendTo target returned disabled copy capability");
    else require(FAILED(zipCreation),"Unavailable compressed SendTo target was treated as installed");
    succeeded(actions.initialize(nullptr,{folder,array(zip.Get()),{}}),"Initialize actual owned compressed-folder item");
    context.archive = true;
    AppCommandCapability extract;
    succeeded(queryAppCommand(actions,Extract,context,&extract),"Query isolated actual compressed-folder Extract provider");
    succeeded(extract.status,"Installed compressed-folder handler did not expose its actual extract verb");
    NamespaceInvocationPlan nativeExtract;
    succeeded(actions.planCommandStore(L"Windows.CompressedFolder.extract",&nativeExtract),"Read actual full native Extract state without invoking wizard");
    require(extract.enabled == nativeExtract.enabled && nativeExtract.commandId != 0,"Fast Extract state differs from actual compressed native command");
    std::unique_ptr<NativeNamespaceCommandChildren> destinations;
    succeeded(actions.queryCommandChildren(L"Windows.CompressedFile.ExtractTo",&destinations),"Read retained native extraction destinations without extracting anything");
    require(destinations && !destinations->entries().empty(),"Installed extraction destination provider returned no native choices");
    for (const auto& destination : destinations->entries()) {
        if (!(destination.flags & ECF_ISSEPARATOR)) require(!destination.label.empty(),"Native extraction destination lost its provider label");
    }
    require(destinations->invoke(0,true) == E_ACCESSDENIED && destinations->invoke(0,false) == E_ACCESSDENIED,
            "Retained native extraction destination bypassed normal visible interaction guard");
    const auto* retainedIdentity = destinations.get();
    require(actions.queryCommandChildren(L"Windows.Bad\\Key",&destinations) == E_INVALIDARG && destinations.get() == retainedIdentity,
            "Invalid native gallery request replaced actual retained destinations");
    ComPtr<IDataObject> data;
    succeeded(array(zip.Get())->BindToHandler(nullptr,BHID_DataObject,IID_PPV_ARGS(&data)),"Obtain original native ZIP IDataObject without clipboard publication");
    require(data != nullptr,"Native ZIP capability relies on a synthetic/staged payload");
    for (const auto key : {L"Windows.AddNetworkLocation",L"Windows.ChangeIndexedLocations"}) {
        const auto component = actions.queryRegisteredComponentState(key,&state);
        if (SUCCEEDED(component)) require(state.enabled(),"Installed public native wizard/control-panel component was disabled");
        else require(FAILED(component),"Missing optional component was guessed enabled");
    }
    for (const auto action : {NamespaceAction::Email,NamespaceAction::Fax}) {
        const auto recipient = actions.queryActionState(action,&state);
        if (SUCCEEDED(recipient)) require((state.state & ~(ECS_DISABLED | ECS_HIDDEN | ECS_CHECKBOX | ECS_CHECKED | ECS_RADIOCHECK)) == 0,
                                         "Actual recipient capability has invalid provider state bits");
        else require(FAILED(recipient),"Absent recipient was not reported unavailable");
    }
    require(actions.invokeZip(true) == E_ACCESSDENIED && actions.invokeZip(false) == E_ACCESSDENIED &&
            actions.invokeExtract(true) == E_ACCESSDENIED && actions.invokeExtract(false) == E_ACCESSDENIED,"Native ZIP operation bypassed headless/visible-owner guard");
    require(read(fixture.text) == textBefore && read(fixture.archive) == zipBefore && fs::is_empty(fixture.directory),"Capability query modified or staged selected files");
}

void nativeViewSelectionWhitelist() {
    // The invocation surface is restricted to the exact three native selection
    // commands, the identical attached view and a verified private desktop.
    onPrivateDesktop([] {
        Fixture fixture;
        const auto textBefore=read(fixture.text),zipBefore=read(fixture.archive);
        for(unsigned index=0;index<24;++index)std::ofstream(fixture.root/(std::to_wstring(index)+L".txt"))<<"owned selection";
        const auto folder=item(fixture.root);
        HiddenView host(folder.Get()),foreign(folder.Get());
        ComPtr<IFolderView2> view;
        succeeded(host.view.As(&view),"Read exact owned view selection interface");
        int total=0;
        const auto deadline=GetTickCount64()+5000;
        while(total<24&&GetTickCount64()<deadline){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}succeeded(view->ItemCount(SVGIO_ALLVIEW,&total),"Count exclusively owned native view items");if(total<24)Sleep(1);}
        require(total>=24,"Owned native selection view did not finish enumerating");
        NativeNamespaceActions actions;
        succeeded(actions.initialize(host.owner,{folder,{},host.view}),"Attach exact native selection view site");
        require(actions.invokeViewSelection(L"Windows.delete",host.view.Get(),true)==E_INVALIDARG,
                "Selection-only surface admitted a destructive or generic native verb");
        require(actions.invokeViewSelection(L"Windows.selectall",nullptr,true)==E_POINTER,
                "Selection-only surface accepted an absent view");
        require(actions.invokeViewSelection(L"Windows.selectall",foreign.view.Get(),true)==E_ACCESSDENIED,
                "Selection-only surface accepted a different native view");
        require(actions.invokeViewSelection(L"Windows.selectall",host.view.Get(),false)==E_ACCESSDENIED,
                "Hidden normal-mode selection owner was accepted");
        NativeNamespaceActions wrongOwner;
        succeeded(wrongOwner.initialize(foreign.owner,{folder,{},host.view}),"Attach deliberately mismatched owned selection window");
        require(wrongOwner.invokeViewSelection(L"Windows.selectall",host.view.Get(),true)==E_ACCESSDENIED,
                "Selection-only surface accepted a view outside its owner window");
        wrongOwner.reset();
        require(actions.invokeCommandStore(L"Windows.selectall",true,{},NamespaceMenuScope::Background)==E_ACCESSDENIED,
                "Selection exception weakened the generic headless activation guard");
        for(const auto name:{L"Windows.selectall",L"Windows.selectnone",L"Windows.invertselection",L"Windows.selectnone"}){
            const auto status=actions.invokeViewSelection(name,host.view.Get(),true);
            std::wcout<<L"Owned native selection "<<name<<L" HRESULT="<<std::hex<<status<<std::dec<<L"\n";
            succeeded(status,"Invoke only the exact native owned-view selection command");
            MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
            int selected=0;succeeded(view->ItemCount(SVGIO_SELECTION,&selected),"Read actual selected count after native command");
            const bool none=_wcsicmp(name,L"Windows.selectnone")==0;
            require(selected==(none?0:total),"Native selection provider did not produce the exact owned view count");
        }
        bool visible=true;
        succeeded(explorer::PrivateDesktop::current()->visibleWindowsOnInputDesktop(visible),"Inspect selection fixture input-desktop visibility");
        require(!visible,"Native selection fixture displayed an input-desktop window");
        require(read(fixture.text)==textBefore && read(fixture.archive)==zipBefore,"Native selection changed owned file contents");
        actions.reset();
    });
}

HRESULT finishStateTask(NamespaceCommandStateTask& task,NamespaceCommandState* result) {
    const auto deadline=GetTickCount64()+10000;
    while(!task.completed()&&GetTickCount64()<deadline) {
        MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
        MsgWaitForMultipleObjectsEx(0,nullptr,1,QS_ALLINPUT,MWMO_ALERTABLE|MWMO_INPUTAVAILABLE);
    }
    require(task.completed(),"Read-only native menu state worker exceeded isolated completion budget");
    return task.poll(result);
}

HRESULT finishStateBatch(NamespaceCommandStateTask& task,std::vector<NamespaceSelectionVerbState>* result) {
    const auto deadline=GetTickCount64()+10000;
    while(!task.completed()&&GetTickCount64()<deadline) {
        MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
        MsgWaitForMultipleObjectsEx(0,nullptr,1,QS_ALLINPUT,MWMO_ALERTABLE|MWMO_INPUTAVAILABLE);
    }
    if(!task.completed())return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    return task.pollSelectionVerbBatch(result);
}

void nativeLeafStateMenuEquivalence() {
    onPrivateDesktop([] {
        Fixture fixture;const auto before=read(fixture.text),zipBefore=read(fixture.archive);
        const auto clipboard=GetClipboardSequenceNumber();
        const auto clipboardOwnerBefore=GetClipboardOwner();
        const auto folder=item(fixture.root),text=item(fixture.text),directory=item(fixture.directory);
        HiddenView host(folder.Get());
        struct Identities {
            PIDLIST_ABSOLUTE text=nullptr,directory=nullptr,parent=nullptr;
            ~Identities(){CoTaskMemFree(text);CoTaskMemFree(directory);CoTaskMemFree(parent);}
        } identities;
        succeeded(SHGetIDListFromObject(text.Get(),&identities.text),"Read exact owned leaf-state file identity");
        succeeded(SHGetIDListFromObject(directory.Get(),&identities.directory),"Read exact owned mixed leaf-state tail identity");
        identities.parent=ILCloneFull(identities.text);
        require(identities.parent&&ILRemoveLastID(identities.parent),"Retain actual registered-menu parent identity");
        ComPtr<IShellFolder> parent;PCUITEMID_CHILD unused=nullptr;
        succeeded(SHBindToParent(identities.text,IID_PPV_ARGS(&parent),&unused),"Bind exact native registered-menu parent");
        struct Registry {HKEY key=nullptr;~Registry(){if(key)RegCloseKey(key);}} registry;
        succeeded(HRESULT_FROM_WIN32(RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\CommandStore",0,KEY_READ,&registry.key)),
            "Read native CommandStore without writing registration");
        const auto leaves=[](const std::vector<ContextMenuEntry>& entries) {
            std::multimap<std::wstring,UINT> states;
            for(const auto& entry:entries)if(!entry.separator()&&!entry.submenu&&!entry.canonicalVerb.empty()) {
                auto verb=entry.canonicalVerb;
                for(auto& character:verb)character=static_cast<wchar_t>(towlower(character));
                states.emplace(std::move(verb),entry.state&(MFS_DISABLED|MFS_GRAYED|MFS_CHECKED));
            }
            return states;
        };
        for(const DWORD count:{1u,2u,16u,5001u,10000u})for(const bool mixed:{false,true}) {
            if(count==1&&mixed)continue;
            std::vector<PCIDLIST_ABSOLUTE> ids(count,identities.text);
            if(mixed)ids.back()=identities.directory;
            ComPtr<IShellItemArray> selected;
            succeeded(SHCreateShellItemArrayFromIDLists(count,ids.data(),&selected),"Retain every native root-leaf comparison target");
            std::vector<PCUITEMID_CHILD> children;children.reserve(ids.size());
            for(const auto id:ids)children.push_back(ILFindLastID(id));
            DWORD retained=0;succeeded(selected->GetCount(&retained),"Read original full native root-leaf count");
            require(retained==count,"Native root-leaf comparison truncated its original selection");
            for(const bool registered:{false,true}) {
                const auto createContext=[&](IContextMenu** result) {
                    if(!registered)return selected->BindToHandler(nullptr,BHID_SFUIObject,IID_IContextMenu,reinterpret_cast<void**>(result));
                    DEFCONTEXTMENU definition{};definition.pidlFolder=identities.parent;definition.psf=parent.Get();
                    definition.cidl=count;definition.apidl=children.data();definition.cKeys=1;definition.aKeys=&registry.key;
                    return SHCreateDefaultContextMenu(&definition,IID_IContextMenu,reinterpret_cast<void**>(result));
                };
                ComPtr<IContextMenu> full;succeeded(createContext(&full),"Create original full-array native comparison menu");
                const auto normalStarted=std::chrono::steady_clock::now();
                const UINT nativeFlags=CMF_EXTENDEDVERBS|(registered?0:CMF_ITEMMENU);
                NativeContextMenu normal;succeeded(normal.create(nullptr,full.Get(),host.view.Get(),nativeFlags),
                    "Read native synchronous-cascade comparison truth without invocation");
                const auto normalMicros=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-normalStarted).count();
                std::vector<ContextMenuEntry> original;succeeded(normal.enumerate(original,false),"Read original actual native leaf states");
                normal.reset();full.Reset();
                ComPtr<IContextMenu> stateContext;succeeded(createContext(&stateContext),"Create fresh original-target native leaf-state menu");
                const auto stateStarted=std::chrono::steady_clock::now();
                NativeContextMenu state;succeeded(state.createLeafState(stateContext.Get(),host.view.Get(),nativeFlags),
                    "Read same native target/site without forcing cascade population");
                const auto stateMicros=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-stateStarted).count();
                std::vector<ContextMenuEntry> snapshot;succeeded(state.enumerate(snapshot,false),"Read unpopulated actual native leaf states");
                if(registered) {
                    const auto all=leaves(original),lazy=leaves(snapshot);
                    const auto range=all.equal_range(L"windows.removeproperties"),other=lazy.equal_range(L"windows.removeproperties");
                    if(count==1)require(range.first!=range.second&&other.first!=other.second,
                        "Registered single-file comparison did not contain the actual RemoveProperties leaf");
                    require(std::vector(range.first,range.second)==std::vector(other.first,other.second),
                        "Registered RemoveProperties state differs without synchronous cascades");
                } else require(leaves(original)==leaves(snapshot),
                    "Native root canonical leaf missing/ambiguity/disabled/checked truth differs without synchronous cascades");
                std::cout<<"Native leaf-state equivalence: count="<<count<<" mixed="<<mixed<<" registered="<<registered
                    <<" synchronousQuery_us="<<normalMicros<<" stateQuery_us="<<stateMicros<<'\n';
                require(state.invoke(state.firstCommand())==E_ACCESSDENIED,
                    "Read-only root state comparison could activate a native command");
                state.reset();stateContext.Reset();
                ComPtr<IContextMenu> restrictedContext;succeeded(createContext(&restrictedContext),
                    "Retain the same full native array/site for the no-resource state comparison");
                NativeContextMenu restricted;succeeded(restricted.createLeafState(restrictedContext.Get(),host.view.Get(),nativeFlags,true),
                    "Read native association/dynamic leaf truth without unrelated built-in operations");
                std::vector<ContextMenuEntry> restrictedEntries;succeeded(restricted.enumerate(restrictedEntries,false),
                    "Read actual restricted native leaves without invoking or populating cascades");
                const auto nonResource=[&](const std::vector<ContextMenuEntry>& values) {
                    auto result=leaves(values);
                    std::erase_if(result,[](const auto& entry) {
                        auto verb=std::wstring_view(entry.first);
                        if(verb.starts_with(L"windows."))verb.remove_prefix(8);
                        return verb==L"cut"||verb==L"copy"||verb==L"paste"||verb==L"link"||verb==L"delete"||
                            verb==L"rename"||verb==L"properties"||verb==L"pastelink"||verb==L"pasteshortcut"||
                            verb==L"recycle"||verb==L"permanentdelete"||verb==L"ribbondelete"||verb==L"copyaspath"||
                            verb.ends_with(L".properties");
                    });
                    return result;
                };
                require(nonResource(original)==nonResource(restrictedEntries),
                    "Native no-resource restriction changed a non-resource canonical leaf's presence, multiplicity, disabled or checked state");
                require(restricted.invoke(restricted.firstCommand())==E_ACCESSDENIED,
                    "Restricted leaf-state query permitted native invocation");
                restricted.reset();
                if(count==1||count==10000) {
                    NativeContextMenu followup;succeeded(followup.create(nullptr,restrictedContext.Get(),host.view.Get(),nativeFlags),
                        "Reuse the exact restricted native provider for an ordinary complete menu");
                    std::vector<ContextMenuEntry> restored;succeeded(followup.enumerate(restored,false),
                        "Read canonical IDs and resource leaves after native restriction restoration");
                    require(leaves(original)==leaves(restored),
                        "Retained provider normal-menu followup lost native resource or non-resource canonical states");
                    followup.reset();
                }
                restrictedContext.Reset();
                if(!registered) {
                    // Exercise the real worker, full CIDA and actual site for
                    // every catalog static fallback (including absent aliases),
                    // rather than accepting only a creator-STA menu comparison.
                    const std::array<std::wstring_view,10> requested{L"open",L"edit",L"print",L"pintohome",L"runasuser",
                        L"runas",L"enqueue",L"manage",L"remotedesktop",L"pintostartscreen"};
                    std::unique_ptr<NamespaceCommandStateTask> task;
                    succeeded(NamespaceCommandStateTask::startSelectionVerbBatch(requested,selected.Get(),host.view.Get(),&task),
                        "Read every catalog static leaf from one genuine optimized full-selection worker menu");
                    std::vector<NamespaceSelectionVerbState> actual;
                    succeeded(finishStateBatch(*task,&actual),"Complete native restricted state worker on the original full selection/site");
                    require(actual.size()==requested.size(),"Restricted native worker dropped a catalog canonical request");
                    const auto expected=leaves(original);
                    for(size_t index=0;index<requested.size();++index) {
                        const auto range=expected.equal_range(std::wstring(requested[index]));
                        const auto matches=std::distance(range.first,range.second);
                        const auto& result=actual[index];
                        require(result.verb==requested[index],"Restricted worker reordered catalog aliases");
                        if(!matches)require(result.status==HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
                            "Restricted worker invented an absent native catalog leaf");
                        else if(matches>1)require(result.status==E_UNEXPECTED,"Restricted worker erased native canonical ambiguity");
                        else {
                            succeeded(result.status,"Restricted worker lost an actual native catalog leaf");
                            const UINT flags=range.first->second;
                            require(result.native.contextMenu&&result.native.identitySnapshot&&result.native.siteAttached&&
                                result.native.selectionCount==count&&result.native.enabled()==!(flags&(MFS_DISABLED|MFS_GRAYED))&&
                                result.native.checked()==!!(flags&MFS_CHECKED),
                                "Restricted worker changed full-selection native catalog presence, disabled or checked state");
                        }
                    }
                }
            }
            ComPtr<IShellItem> tail;succeeded(selected->GetItemAt(count-1,&tail),"Read untouched original full-array tail");
            int comparison=1;succeeded(tail->Compare(mixed?directory.Get():text.Get(),SICHINT_CANONICAL,&comparison),
                "Verify native root-state original tail identity");
            require(comparison==0,"Read-only native state comparison replaced or lost the original tail");
        }
        const bool textUnchanged=read(fixture.text)==before,zipUnchanged=read(fixture.archive)==zipBefore;
        const bool directoryEmpty=fs::is_empty(fixture.directory),ownerHidden=!IsWindowVisible(host.owner);
        const auto clipboardAfter=GetClipboardSequenceNumber();
        const auto clipboardOwnerAfter=GetClipboardOwner();
        const bool clipboardUnchanged=clipboardAfter==clipboard;
        if(!textUnchanged||!zipUnchanged||!directoryEmpty||!clipboardUnchanged||!ownerHidden) {
            DWORD ownerProcess=0;if(clipboardOwnerAfter)GetWindowThreadProcessId(clipboardOwnerAfter,&ownerProcess);
            std::cerr<<"Native root-leaf isolation: textUnchanged="<<textUnchanged<<" zipUnchanged="<<zipUnchanged
                <<" directoryEmpty="<<directoryEmpty<<" ownerHidden="<<ownerHidden
                <<" clipboardSequenceUnchanged="<<clipboardUnchanged<<" clipboardSequenceDelta="<<clipboardAfter-clipboard
                <<" clipboardOwnerChanged="<<(clipboardOwnerAfter!=clipboardOwnerBefore)
                <<" clipboardOwnerIsThisProcess="<<(ownerProcess==GetCurrentProcessId())<<'\n';
        }
        require(textUnchanged&&zipUnchanged&&directoryEmpty,"Native root-leaf equivalence changed owned payloads");
        require(clipboardUnchanged,"Native root-leaf equivalence observed a changed window-station clipboard sequence");
        require(ownerHidden,"Native root-leaf equivalence displayed its hidden owner");
    });
}

void nativeRegisteredStaticAndShortcutState() {
    onPrivateDesktop([] {
        Fixture fixture;const auto before=read(fixture.text),zipBefore=read(fixture.archive);
        const auto clipboard=GetClipboardSequenceNumber();
        wchar_t module[32768]{};require(GetModuleFileNameW(nullptr,module,static_cast<DWORD>(std::size(module)))!=0,
                                       "Read only this fixture executable's location");
        const auto executable=fixture.root/L"owned capability application.exe";
        require(CopyFileW(module,executable.c_str(),TRUE)!=FALSE,"Copy owned executable without running it");
        const auto appLink=fixture.root/L"owned application shortcut.lnk",documentLink=fixture.root/L"owned document shortcut.lnk";
        for(const auto& [destination,target]:std::array<std::pair<fs::path,fs::path>,2>{{{appLink,executable},{documentLink,fixture.text}}}) {
            ComPtr<IShellLinkW> link;succeeded(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&link)),"Create owned static-state shortcut");
            succeeded(link->SetPath(target.c_str()),"Store only owned static-state target");
            ComPtr<IPersistFile> persist;succeeded(link.As(&persist),"Read public shortcut persistence interface");
            succeeded(persist->Save(destination.c_str(),FALSE),"Save owned shortcut without resolving or executing it");
        }
        const auto folder=item(fixture.root);HiddenView host(folder.Get());NativeNamespaceActions actions;
        for(const auto& path:std::array<fs::path,5>{fixture.text,executable,appLink,documentLink,fixture.directory}) {
            auto selected=array(item(path).Get());
            succeeded(actions.initialize(host.owner,{folder,selected,host.view}),"Attach exact original static selection and view site");
            auto context=folderContext();context.selectionCount=1;context.selectionAttributes=SFGAO_FILESYSTEM;
            for(const auto id:{RibbonRemoveProperties,RibbonRunAsAnotherUser,RibbonTroubleshootCompatibility,RibbonPinToTaskbar}) {
                AppCommandCapability capability;succeeded(queryAppCommand(actions,id,context,&capability),"Read actual static/native shortcut capability");
                NamespaceInvocationPlan registered;const auto direct=actions.planCommandStore(appCommandStoreName(id),&registered);
                if(capability.status==E_PENDING) {
                    std::unique_ptr<NamespaceCommandStateTask> task;
                    succeeded(startAppCommandStateTask(actions,id,&task),"Complete exact registered or shortcut state on native worker");
                    NamespaceCommandState state;const auto result=finishStateTask(*task,&state);
                    if(id==RibbonRemoveProperties||id==RibbonTroubleshootCompatibility) {
                        succeeded(result,"Ribbon-only or redirected shortcut command lost native registered state");
                        require(state.contextMenu&&state.identitySnapshot&&state.selectionCount==1&&
                                state.delegatedCommand==appCommandStoreName(id)&&state.enabled()==(SUCCEEDED(direct)&&registered.enabled),
                                "Registered worker differs from exact native Ribbon-only/shortcut menu");
                    } else {
                        NativeContextMenu menu;succeeded(menu.createSelection(nullptr,selected.Get(),host.view.Get(),CMF_EXTENDEDVERBS),"Read actual redirected shortcut selection menu");
                        std::vector<ContextMenuEntry> entries;succeeded(menu.enumerate(entries,false),"Read actual canonical shortcut leaf state");
                        const auto leaf=std::find_if(entries.begin(),entries.end(),[](const auto& entry){return entry.canonicalVerb==L"runasuser"&&!entry.separator();});
                        if(leaf==entries.end()||leaf->submenu)require(result==HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),"Missing shortcut verb was enabled");
                        else{succeeded(result,"Real shortcut association was rejected by .lnk prefilter");require(state.enabled()==leaf->enabled(),"Shortcut worker lost native target redirection state");}
                    }
                } else if(id==RibbonPinToTaskbar)require(capability.enabled==(SUCCEEDED(direct)&&registered.enabled),
                                                       "Taskbar state was inferred from handler presence rather than its actual provider");
                if(path==appLink&&id==RibbonRunAsAnotherUser)require(capability.status==E_PENDING,
                    "A shortcut's missing association array incorrectly rejected its actual target verb");
                if(path==appLink&&id==RibbonTroubleshootCompatibility)require(capability.status==E_PENDING,
                    "Direct compatibility state skipped the real registered shortcut-redirection menu");
                require(invokeAppNativeCommand(actions,id,context,true)==E_ACCESSDENIED,
                        "Static state verification pinned an application, launched credentials/compatibility or changed metadata");
            }
            actions.reset();
        }
        require(read(fixture.text)==before&&read(fixture.archive)==zipBefore&&GetClipboardSequenceNumber()==clipboard&&!IsWindowVisible(host.owner),
                "Native static/shortcut capability verification changed owned payloads, clipboard or input-desktop UI");
    });
}

class StaticStateSite final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IServiceProvider,Microsoft::WRL::FtmBase> {
public:
    HRESULT STDMETHODCALLTYPE QueryService(REFGUID,REFIID,void** result) override {
        if(!result)return E_POINTER;*result=nullptr;return E_NOINTERFACE;
    }
};

class StaticStateMenu final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IContextMenu,IObjectWithSite,Microsoft::WRL::FtmBase> {
public:
    DWORD fullCount=100001;
    bool tailSupports=true,omit=false,duplicate=false,cascade=false;
    bool checked=false;
    std::wstring canonicalVerb=L"edit";
    HRESULT queryStatus=S_OK;
    IUnknown* expectedSite=nullptr;
    std::atomic<unsigned> queries{0},invocations{0},attachments{0},detachments{0};
    std::atomic<DWORD> queryThread{0};
    std::atomic<UINT> queryFlags{0};
    std::atomic<bool> exactSite{false};
    HANDLE entered=CreateEventW(nullptr,TRUE,FALSE,nullptr),release=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    bool block=false;
    ~StaticStateMenu(){if(entered)CloseHandle(entered);if(release)CloseHandle(release);}
    HRESULT STDMETHODCALLTYPE QueryContextMenu(HMENU menu,UINT position,UINT first,UINT last,UINT flags) override {
        ++queries;queryThread=GetCurrentThreadId();queryFlags=flags;exactSite=site_.Get()==expectedSite;
        SetEvent(entered);
        if(block&&WaitForSingleObject(release,5000)!=WAIT_OBJECT_0)return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        if(FAILED(queryStatus))return queryStatus;
        if(last<first+3)return E_INVALIDARG;
        if(omit)return S_OK;
        const UINT state=(tailSupports?MFS_ENABLED:MFS_DISABLED)|(checked?MFS_CHECKED:0);
        if(cascade) {
            MENUITEMINFOW entry{sizeof(entry)};entry.fMask=MIIM_ID|MIIM_STRING|MIIM_STATE|MIIM_SUBMENU;
            entry.wID=first+2;entry.dwTypeData=const_cast<wchar_t*>(L"Owned native cascade");entry.fState=state;entry.hSubMenu=CreatePopupMenu();
            if(!entry.hSubMenu)return E_OUTOFMEMORY;
            if(!InsertMenuItemW(menu,position,TRUE,&entry)){DestroyMenu(entry.hSubMenu);return E_FAIL;}
        } else if(!InsertMenuW(menu,position,MF_BYPOSITION|MF_STRING|state,first+2,L"Owned native leaf"))return E_FAIL;
        if(duplicate&&!InsertMenuW(menu,position+1,MF_BYPOSITION|MF_STRING,first+3,L"Duplicate owned native leaf"))return E_FAIL;
        return MAKE_HRESULT(SEVERITY_SUCCESS,0,duplicate?4:3);
    }
    HRESULT STDMETHODCALLTYPE GetCommandString(UINT_PTR ordinal,UINT flags,UINT*,LPSTR output,UINT capacity) override {
        if((ordinal!=2&&(!duplicate||ordinal!=3))||flags!=GCS_VERBW)return E_NOTIMPL;
        return wcscpy_s(reinterpret_cast<wchar_t*>(output),capacity,canonicalVerb.c_str())?E_FAIL:S_OK;
    }
    HRESULT STDMETHODCALLTYPE InvokeCommand(CMINVOKECOMMANDINFO*) override {++invocations;return E_ACCESSDENIED;}
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) override {
        if(site)++attachments;else++detachments;site_=site;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSite(REFIID iid,void** output) override {
        if(!output)return E_POINTER;*output=nullptr;return site_?site_->QueryInterface(iid,output):E_NOINTERFACE;
    }
private:
    ComPtr<IUnknown> site_;
};

class StaticStateSelection final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IShellItemArray> {
public:
    ComPtr<StaticStateMenu> menu=Microsoft::WRL::Make<StaticStateMenu>();
    ComPtr<IContextMenu> configuredMenu;
    ComPtr<IDataObject> exportedData;
    std::atomic<unsigned> itemReads{0},binds{0},menuBinds{0},dataBinds{0};
    HRESULT STDMETHODCALLTYPE GetCount(DWORD* output) override {if(!output)return E_POINTER;*output=menu->fullCount;return S_OK;}
    HRESULT STDMETHODCALLTYPE BindToHandler(IBindCtx*,REFGUID handler,REFIID iid,void** output) override {
        if(!output)return E_POINTER;*output=nullptr;++binds;
        if(handler==BHID_DataObject){++dataBinds;return exportedData?exportedData->QueryInterface(iid,output):E_NOTIMPL;}
        if(handler==BHID_SFUIObject){++menuBinds;return (configuredMenu?configuredMenu.Get():menu.Get())->QueryInterface(iid,output);}
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetItemAt(DWORD,IShellItem** output) override {
        ++itemReads;if(!output)return E_POINTER;*output=nullptr;return E_UNEXPECTED;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyStore(GETPROPERTYSTOREFLAGS,REFIID,void** output) override {
        if(!output)return E_POINTER;*output=nullptr;return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyDescriptionList(REFPROPERTYKEY,REFIID,void** output) override {
        if(!output)return E_POINTER;*output=nullptr;return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetAttributes(SIATTRIBFLAGS,SFGAOF,SFGAOF*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE EnumItems(IEnumShellItems** output) override {if(!output)return E_POINTER;*output=nullptr;return E_NOTIMPL;}
};

class MalformedCidaData final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IDataObject,Microsoft::WRL::FtmBase> {
public:
    explicit MalformedCidaData(std::vector<BYTE> bytes):bytes_(std::move(bytes)),format_(RegisterClipboardFormatW(CFSTR_SHELLIDLIST)) {}
    std::atomic<unsigned> exports{0};
    std::atomic<SIZE_T> exportedBytes{0};
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format,STGMEDIUM* output) override {
        if(!output)return E_POINTER;
        const auto status=QueryGetData(format);if(FAILED(status))return status;
        HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,bytes_.size());if(!memory)return E_OUTOFMEMORY;
        const SIZE_T size=GlobalSize(memory);
        auto* data=static_cast<BYTE*>(GlobalLock(memory));
        if(!data){const auto error=HRESULT_FROM_WIN32(GetLastError());GlobalFree(memory);return error;}
        // Poison allocation padding too: no implicit zero may terminate a
        // deliberately unterminated PIDL after the supplied payload.
        std::memset(data,0xff,size);std::memcpy(data,bytes_.data(),bytes_.size());GlobalUnlock(memory);
        *output={};output->tymed=TYMED_HGLOBAL;output->hGlobal=memory;
        exportedBytes=size;++exports;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
        if(!format)return E_POINTER;
        return format_&&format->cfFormat==format_&&format->dwAspect==DVASPECT_CONTENT&&format->lindex==-1&&
            !format->ptd&&(format->tymed&TYMED_HGLOBAL)?S_OK:DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*,STGMEDIUM*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*,FORMATETC* output) override {
        if(!output)return E_POINTER;output->ptd=nullptr;return DATA_S_SAMEFORMATETC;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*,STGMEDIUM*,BOOL) override {return E_ACCESSDENIED;}
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD,IEnumFORMATETC** output) override {
        if(!output)return E_POINTER;*output=nullptr;return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*,DWORD,IAdviseSink*,DWORD*) override {return OLE_E_ADVISENOTSUPPORTED;}
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override {return OLE_E_ADVISENOTSUPPORTED;}
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA** output) override {
        if(!output)return E_POINTER;*output=nullptr;return OLE_E_ADVISENOTSUPPORTED;
    }
private:
    std::vector<BYTE> bytes_;
    UINT format_=0;
};

void malformedExportedSelectionIdentity() {
    onPrivateDesktop([] {
        const auto site=Microsoft::WRL::Make<StaticStateSite>();
        ComPtr<IUnknown> identity;succeeded(site.As(&identity),"Retain exact malformed-selection worker site");
        const auto clipboard=GetClipboardSequenceNumber();
        struct Export {DWORD count;std::vector<BYTE> bytes;};
        const auto words=[](std::initializer_list<UINT> values) {
            std::vector<BYTE> bytes(values.size()*sizeof(UINT));
            std::memcpy(bytes.data(),values.begin(),bytes.size());return bytes;
        };
        // These are malformed exported wire payloads, not a second CIDA parser.
        // The two last payloads respectively omit parent and child termination.
        auto parent=words({1,12,12,4});
        auto child=words({1,12,14,0x00020000});
        const std::array<Export,6> payloads{{
            {std::numeric_limits<UINT>::max(),words({std::numeric_limits<UINT>::max()})},
            {8,words({8,0})},
            {1,words({1,std::numeric_limits<UINT>::max(),12,0})},
            {1,words({1,0,12,0})},
            {1,std::move(parent)},
            {1,std::move(child)}
        }};
        NamespaceCommandState sentinel;sentinel.handler=CLSID_ShellDesktop;sentinel.state=ECS_CHECKED;
        sentinel.explorerCommand=sentinel.initialized=sentinel.siteAttached=sentinel.contextMenu=sentinel.identitySnapshot=true;
        sentinel.selectionCount=23;sentinel.delegatedCommand=L"untouched native state";
        const auto unchanged=[&](const NamespaceCommandState& value) {
            return IsEqualCLSID(value.handler,sentinel.handler)&&value.state==sentinel.state&&
                value.explorerCommand==sentinel.explorerCommand&&value.initialized==sentinel.initialized&&
                value.siteAttached==sentinel.siteAttached&&value.contextMenu==sentinel.contextMenu&&
                value.identitySnapshot==sentinel.identitySnapshot&&value.selectionCount==sentinel.selectionCount&&
                value.delegatedCommand==sentinel.delegatedCommand;
        };
        const std::array<std::wstring_view,2> verbs{L"edit",L"print"};
        for(size_t index=0;index<payloads.size();++index) {
            const auto selected=Microsoft::WRL::Make<StaticStateSelection>();selected->menu->fullCount=payloads[index].count;
            const auto data=Microsoft::WRL::Make<MalformedCidaData>(payloads[index].bytes);
            succeeded(data.As(&selected->exportedData),"Expose real exported HGLOBAL CIDA data object");
            std::unique_ptr<NamespaceCommandStateTask> task;
            succeeded(NamespaceCommandStateTask::startSelectionVerb(L"edit",selected.Get(),identity.Get(),&task),
                "Start public full-selection native worker with malformed exported data");
            auto state=sentinel;
            require(finishStateTask(*task,&state)==HRESULT_FROM_WIN32(ERROR_INVALID_DATA)&&unchanged(state),
                "Malformed CIDA changed single-state output or escaped exact invalid-data rejection");
            task.reset();
            succeeded(NamespaceCommandStateTask::startSelectionVerbBatch(verbs,selected.Get(),identity.Get(),&task),
                "Start public full-selection native batch with malformed exported data");
            std::vector<NamespaceSelectionVerbState> states{{L"untouched batch",E_ABORT,sentinel}};
            require(finishStateBatch(*task,&states)==HRESULT_FROM_WIN32(ERROR_INVALID_DATA)&&states.size()==1&&
                states.front().verb==L"untouched batch"&&states.front().status==E_ABORT&&unchanged(states.front().native),
                "Malformed CIDA changed batch output or invented per-leaf state");
            require(data->exports==2&&selected->dataBinds==2&&selected->binds==2&&selected->menuBinds==0&&
                selected->itemReads==0&&selected->menu->queries==0&&selected->menu->attachments==0&&selected->menu->invocations==0,
                "Malformed full-selection export fell back to native menu binding, per-item reads, site attachment or invocation");
            if(index==0)require(data->exportedBytes<40,"Overflow-count payload unexpectedly became a large allocation");
            if(index==1)require(data->exportedBytes<40,"Missing-offset payload unexpectedly acquired its entire CIDA header");
        }
        require(GetClipboardSequenceNumber()==clipboard,"Malformed exported-selection worker published clipboard data");
    });
}

class RestrictedStateMenu final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
    IContextMenu,IObjectWithSite,IDefaultFolderMenuInitialize,Microsoft::WRL::FtmBase> {
public:
    explicit RestrictedStateMenu(StaticStateMenu* native):native_(native) {}
    DEFAULT_FOLDER_MENU_RESTRICTIONS restrictions=DFMR_NO_ASYNC_VERBS;
    HRESULT getStatus=S_OK,setStatus=S_OK,restoreStatus=S_OK;
    DEFAULT_FOLDER_MENU_RESTRICTIONS queriedRestrictions=DFMR_DEFAULT;
    std::atomic<unsigned> gets{0},sets{0};
    std::atomic<bool> setterHasSite{false};
    HRESULT STDMETHODCALLTYPE QueryContextMenu(HMENU menu,UINT position,UINT first,UINT last,UINT flags) override {
        queriedRestrictions=restrictions;
        return native_->QueryContextMenu(menu,position,first,last,flags);
    }
    HRESULT STDMETHODCALLTYPE GetCommandString(UINT_PTR ordinal,UINT flags,UINT* reserved,LPSTR output,UINT capacity) override {
        return native_->GetCommandString(ordinal,flags,reserved,output,capacity);
    }
    HRESULT STDMETHODCALLTYPE InvokeCommand(CMINVOKECOMMANDINFO* command) override {return native_->InvokeCommand(command);}
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) override {return native_->SetSite(site);}
    HRESULT STDMETHODCALLTYPE GetSite(REFIID iid,void** output) override {return native_->GetSite(iid,output);}
    HRESULT STDMETHODCALLTYPE Initialize(HWND,IContextMenuCB*,PCIDLIST_ABSOLUTE,IShellFolder*,UINT,
                                         PCUITEMID_CHILD_ARRAY,IUnknown*,UINT,const HKEY*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetMenuRestrictions(DEFAULT_FOLDER_MENU_RESTRICTIONS values) override {
        const auto attempt=++sets;ComPtr<IUnknown> site;setterHasSite=SUCCEEDED(native_->GetSite(IID_PPV_ARGS(&site)))&&site.Get()==native_->expectedSite;
        if(FAILED(setStatus))return setStatus;if(attempt==2&&FAILED(restoreStatus))return restoreStatus;
        restrictions=values;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMenuRestrictions(DEFAULT_FOLDER_MENU_RESTRICTIONS mask,
                                                   DEFAULT_FOLDER_MENU_RESTRICTIONS* output) override {
        ++gets;if(!output)return E_POINTER;if(FAILED(getStatus))return getStatus;
        *output=static_cast<DEFAULT_FOLDER_MENU_RESTRICTIONS>(restrictions&mask);return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetHandlerClsid(REFCLSID) override {return E_NOTIMPL;}
private:
    ComPtr<StaticStateMenu> native_;
};

void resourceVerbStateRestrictions() {
    onPrivateDesktop([] {
        const auto site=Microsoft::WRL::Make<StaticStateSite>();
        ComPtr<IUnknown> identity;succeeded(site.As(&identity),"Retain original restriction-test view site");
        const auto clipboard=GetClipboardSequenceNumber();
        for(unsigned variant=0;variant<11;++variant) {
            const auto selected=Microsoft::WRL::Make<StaticStateSelection>();
            selected->menu->expectedSite=identity.Get();selected->menu->checked=true;
            selected->menu->tailSupports=variant!=1;selected->menu->omit=variant==2;
            selected->menu->duplicate=variant==3;selected->menu->cascade=variant==4;
            selected->menu->queryStatus=variant==5||variant==10?E_ACCESSDENIED:S_OK;
            const auto configured=Microsoft::WRL::Make<RestrictedStateMenu>(selected->menu.Get());
            configured->setStatus=variant==6?E_ACCESSDENIED:S_OK;configured->getStatus=variant==7?E_FAIL:S_OK;
            configured->restoreStatus=variant==9||variant==10?E_ABORT:S_OK;
            if(variant==8)configured->restrictions=static_cast<DEFAULT_FOLDER_MENU_RESTRICTIONS>(DFMR_NO_ASYNC_VERBS|DFMR_NO_RESOURCE_VERBS);
            succeeded(configured.As(&selected->configuredMenu),"Expose actual supported restriction interface");
            std::unique_ptr<NamespaceCommandStateTask> task;
            succeeded(NamespaceCommandStateTask::startSelectionVerb(L"edit",selected.Get(),identity.Get(),&task),
                "Read complete custom native selection state through documented no-resource restriction");
            NamespaceCommandState state;state.state=ECS_CHECKED;state.selectionCount=23;
            const auto status=finishStateTask(*task,&state);
            if(variant<2||variant==8) {
                succeeded(status,"Restricted native leaf lost its actual enabled/disabled/checked state");
                require(state.contextMenu&&state.siteAttached&&state.selectionCount==100001&&state.checked()&&state.enabled()==(variant!=1),
                    "Restricted native state changed the original full-array tail or view site");
            } else {
                const auto expected=variant==3?E_UNEXPECTED:(variant==5||variant==6||variant==10)?E_ACCESSDENIED:
                    variant==7?E_FAIL:variant==9?E_ABORT:HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
                require(status==expected&&state.state==ECS_CHECKED&&state.selectionCount==23,
                    "Restriction/leaf failure was erased or changed capability failure output");
            }
            const bool prepared=variant!=6&&variant!=7;
            require(configured->gets==1&&configured->sets==(variant==7||variant==8?0u:variant==6?1u:2u)&&
                selected->menu->queries==(prepared?1u:0u),"Restriction failure queried a menu or skipped native preparation");
            if(prepared) {
                require(configured->queriedRestrictions==static_cast<DEFAULT_FOLDER_MENU_RESTRICTIONS>(DFMR_NO_ASYNC_VERBS|DFMR_NO_RESOURCE_VERBS),
                    "State-only preparation replaced an existing native restriction or skipped the optimized query");
                require(configured->restrictions==(variant==8||variant==9||variant==10?
                    static_cast<DEFAULT_FOLDER_MENU_RESTRICTIONS>(DFMR_NO_ASYNC_VERBS|DFMR_NO_RESOURCE_VERBS):DFMR_NO_ASYNC_VERBS),
                    "State-only query failed to restore the retained provider's original native restrictions");
            }
            if(variant!=7&&variant!=8)require(configured->setterHasSite,"Restriction was set before attaching the original native view site");
            require(selected->menu->attachments==selected->menu->detachments&&selected->menu->invocations==0&&selected->itemReads==0,
                "Restricted state leaked its site, invoked a native command or truncated the original array");
            if(variant==0) {
                selected->menu->canonicalVerb=L"copy";
                NativeContextMenu followup;succeeded(followup.create(nullptr,configured.Get(),identity.Get(),CMF_EXTENDEDVERBS),
                    "Retained provider exposes its genuine resource verb in a normal-menu followup");
                std::vector<ContextMenuEntry> entries;succeeded(followup.enumerate(entries,false),"Read retained normal resource leaf");
                require(configured->gets==1&&configured->sets==2&&configured->queriedRestrictions==DFMR_NO_ASYNC_VERBS&&
                    entries.size()==1&&entries.front().canonicalVerb==L"copy"&&entries.front().enabled(),
                    "Worker-only optimization suppressed a later normal menu on the same provider");followup.reset();
            }
        }
        for(const auto resource:{L"cut",L"copy",L"paste",L"link",L"delete",L"rename",L"properties",L"pastelink",
                                  L"pasteshortcut",L"recycle",L"PermanentDelete",L"RibbonDelete",L"copyaspath",L"RecycleBin.properties"}) {
            for(const bool registeredAlias:{false,true}) {
                const auto selected=Microsoft::WRL::Make<StaticStateSelection>();
                selected->menu->expectedSite=identity.Get();
                selected->menu->canonicalVerb=(registeredAlias?L"Windows.":L"")+std::wstring(resource);
                const auto configured=Microsoft::WRL::Make<RestrictedStateMenu>(selected->menu.Get());
                succeeded(configured.As(&selected->configuredMenu),"Retain resource-request restriction spy");
                const std::array<std::wstring_view,2> verbs{L"edit",selected->menu->canonicalVerb};
                std::unique_ptr<NamespaceCommandStateTask> task;
                succeeded(NamespaceCommandStateTask::startSelectionVerbBatch(verbs,selected.Get(),identity.Get(),&task),
                    "Resource aliases keep the actual unrestricted complete-selection menu");
                std::vector<NamespaceSelectionVerbState> states;succeeded(finishStateBatch(*task,&states),"Read real resource leaf in a mixed batch");
                require(configured->gets==0&&configured->sets==0&&states.size()==2&&states[1].status==S_OK&&
                    states[1].native.selectionCount==100001&&states[1].native.enabled()&&selected->menu->queries==1&&selected->menu->exactSite,
                    "Resource request was restricted or lost its real native full-array state");
            }
        }
        {
            const auto native=Microsoft::WRL::Make<StaticStateMenu>();native->expectedSite=identity.Get();
            const auto configured=Microsoft::WRL::Make<RestrictedStateMenu>(native.Get());
            NativeContextMenu normal;succeeded(normal.create(nullptr,configured.Get(),identity.Get(),CMF_EXTENDEDVERBS),
                "Normal menus preserve their native resource and invocation preparation");
            require(configured->gets==0&&configured->sets==0&&native->queries==1&&native->queryFlags==(CMF_EXTENDEDVERBS|CMF_SYNCCASCADEMENU),
                "Worker-only restriction changed normal popup/invocation preparation");normal.reset();
        }
        {
            const auto selected=Microsoft::WRL::Make<StaticStateSelection>();selected->menu->expectedSite=identity.Get();
            ComPtr<IDefaultFolderMenuInitialize> unsupported;
            require(selected->menu.As(&unsupported)==E_NOINTERFACE,"Unsupported-interface control unexpectedly supports native restrictions");
            std::unique_ptr<NamespaceCommandStateTask> task;
            succeeded(NamespaceCommandStateTask::startSelectionVerb(L"edit",selected.Get(),identity.Get(),&task),
                "Unsupported native restriction interface retains the ordinary menu query");
            NamespaceCommandState state;succeeded(finishStateTask(*task,&state),"Read unchanged fallback native leaf");
            require(state.selectionCount==100001&&state.enabled()&&selected->menu->queries==1&&selected->menu->exactSite,
                "Unsupported restriction interface erased genuine native leaf state");
        }
        require(GetClipboardSequenceNumber()==clipboard,"Restriction tests published clipboard data");
    });
}

void defaultMenuWorkerSlotSurvivesCancellation() {
    onPrivateDesktop([] {
        const auto selected=Microsoft::WRL::Make<StaticStateSelection>();
        const auto site=Microsoft::WRL::Make<StaticStateSite>();
        ComPtr<IUnknown> identity;succeeded(site.As(&identity),"Retain exact heavy-worker mock site");
        selected->menu->expectedSite=identity.Get();selected->menu->block=true;
        const auto clipboard=GetClipboardSequenceNumber();
        std::unique_ptr<NamespaceCommandStateTask> original;
        succeeded(NamespaceCommandStateTask::startSelectionVerb(L"edit",selected.Get(),identity.Get(),&original),
            "Start blocked full-selection native default-menu worker");
        const auto deadline=GetTickCount64()+5000;
        while(WaitForSingleObject(selected->menu->entered,0)!=WAIT_OBJECT_0&&GetTickCount64()<deadline) {
            MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
            MsgWaitForMultipleObjectsEx(0,nullptr,1,QS_ALLINPUT,MWMO_ALERTABLE|MWMO_INPUTAVAILABLE);
        }
        require(WaitForSingleObject(selected->menu->entered,0)==WAIT_OBJECT_0,"Owned blocked native query never entered");
        const std::array<std::wstring_view,2> verbs{L"edit",L"print"};
        std::unique_ptr<NamespaceCommandStateTask> untouched;
        const auto busy=HRESULT_FROM_WIN32(ERROR_BUSY);
        require(NamespaceCommandStateTask::startSelectionVerbBatch(verbs,selected.Get(),identity.Get(),&untouched)==busy&&!untouched,
            "Concurrent native batch ignored the live default-menu work reservation");
        const auto cancelled=GetTickCount64();original->cancel();original.reset();
        require(GetTickCount64()-cancelled<250,"Task destruction blocked on a live native query");
        require(NamespaceCommandStateTask::startSelectionVerb(L"edit",selected.Get(),identity.Get(),&untouched)==busy&&!untouched&&
            NamespaceCommandStateTask::startSelectionVerbs(verbs,selected.Get(),identity.Get(),&untouched)==busy&&!untouched&&
            NamespaceCommandStateTask::startSelectionVerbBatch(verbs,selected.Get(),identity.Get(),&untouched)==busy&&!untouched,
            "Cancellation/destruction prematurely released native default-menu ownership");
        require(selected->menu->queries==1&&selected->menu->invocations==0,
            "Canceled generations constructed concurrent default menus or invoked a command");
        std::unique_ptr<NamespaceCommandStateTask> registered;
        require(NamespaceCommandStateTask::startRegisteredMenu(L"Windows.removeproperties",selected.Get(),identity.Get(),&registered)==busy&&!registered,
            "Registered selection menu overlapped an in-flight native default menu");
        SetEvent(selected->menu->release);
        succeeded(drainStaWorkers(10000),"Wait for canceled native provider references/COM and exact worker termination");
        require(selected->menu->attachments==selected->menu->detachments,
            "Canceled heavy-menu ownership leaked its native view site");
        succeeded(NamespaceCommandStateTask::startRegisteredMenu(L"Windows.removeproperties",selected.Get(),identity.Get(),&registered),
            "Registered selection menu did not acquire the released native menu slot");
        NamespaceCommandState preserved;preserved.selectionCount=91;
        require(finishStateTask(*registered,&preserved)==E_UNEXPECTED&&preserved.selectionCount==91,
            "Registered menu changed unsupported mock output after serialized preparation");
        require(selected->menu->queries==1,"Registered-only preparation used the wrong default menu");
        succeeded(drainStaWorkers(10000),"Release exact serialized registered-menu worker lifetime");
        selected->menu->block=false;
        succeeded(NamespaceCommandStateTask::startSelectionVerbBatch(verbs,selected.Get(),identity.Get(),&untouched),
            "Latest generation did not acquire native work slot after actual cleanup");
        std::vector<NamespaceSelectionVerbState> states;succeeded(finishStateBatch(*untouched,&states),"Complete latest real full-selection menu truth");
        require(states.size()==2&&states[0].status==S_OK&&states[0].native.selectionCount==100001&&states[0].native.siteAttached&&
            states[1].status==HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)&&selected->menu->queries==2&&
            selected->menu->invocations==0&&GetClipboardSequenceNumber()==clipboard,
            "Retried generation truncated targets, guessed sibling state, or changed clipboard/associated-app state");
    });
}

void staticMenuWorkerContractsAndCancellation() {
    onPrivateDesktop([] {
        const auto selected=Microsoft::WRL::Make<StaticStateSelection>();
        const auto site=Microsoft::WRL::Make<StaticStateSite>();
        ComPtr<IUnknown> identity;succeeded(site.As(&identity),"Retain agile read-only mock site identity");
        selected->menu->expectedSite=identity.Get();
        const DWORD creator=GetCurrentThreadId();
        const auto clipboard=GetClipboardSequenceNumber();
        for(unsigned variant=0;variant<6;++variant) {
            selected->menu->tailSupports=variant!=1;selected->menu->omit=variant==2;
            selected->menu->duplicate=variant==3;selected->menu->cascade=variant==4;
            selected->menu->queryStatus=variant==5?E_ACCESSDENIED:S_OK;
            std::unique_ptr<NamespaceCommandStateTask> task;
            succeeded(NamespaceCommandStateTask::startSelectionVerb(L"edit",selected.Get(),identity.Get(),&task),
                      "Start complete native selection-menu worker without per-item association scan");
            NamespaceCommandState state;state.state=ECS_CHECKED;state.selectionCount=23;
            const auto status=finishStateTask(*task,&state);
            if(variant<2) {
                succeeded(status,"Complete exact native enabled/disabled static leaf state");
                require(state.contextMenu&&state.siteAttached&&state.selectionCount==100001&&state.enabled()==(variant==0),
                        "Full-selection native leaf state ignored its tail or original site");
            } else {
                const auto expected=variant==3?E_UNEXPECTED:variant==5?E_ACCESSDENIED:HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
                require(status==expected&&state.state==ECS_CHECKED&&state.selectionCount==23,
                        "Missing/duplicate/cascade/failed native static leaf invented state or changed failure output");
            }
            require(selected->menu->queryThread!=creator&&selected->menu->exactSite&&
                    selected->itemReads==0&&selected->menu->attachments==selected->menu->detachments&&selected->menu->invocations==0,
                    "Static menu construction ran in the UI callback, truncated targets, lost its site or invoked a verb");
            require(selected->menu->queryFlags==(CMF_ITEMMENU|CMF_EXTENDEDVERBS),
                "State-only worker requested synchronous cascade population or changed native ordinary/extended flags");
        }
        selected->menu->queryStatus=S_OK;selected->menu->duplicate=selected->menu->cascade=selected->menu->omit=false;
        const std::array<std::wstring_view,2> batchVerbs{L"edit",L"print"};
        for(unsigned variant=0;variant<6;++variant) {
            selected->menu->checked=true;
            selected->menu->tailSupports=variant!=1;selected->menu->omit=variant==2;
            selected->menu->duplicate=variant==3;selected->menu->cascade=variant==4;
            selected->menu->queryStatus=variant==5?E_ACCESSDENIED:S_OK;
            const auto beforeQueries=selected->menu->queries.load();
            std::unique_ptr<NamespaceCommandStateTask> task;
            succeeded(NamespaceCommandStateTask::startSelectionVerbBatch(batchVerbs,selected.Get(),identity.Get(),&task),
                      "Read independent static states from one original full native menu");
            std::vector<NamespaceSelectionVerbState> states{{L"untouched",E_ABORT,{}}};
            NamespaceCommandState one;one.selectionCount=77;
            require(task->poll(&one)==E_INVALIDARG&&one.selectionCount==77,"Batch task accepted single-result polling");
            const auto result=finishStateBatch(*task,&states);
            NamespaceCommandStateTimings timing;succeeded(task->pollTimings(&timing),"Completed native timing remains independent of per-leaf/preparation failure");
            require(timing.workerMicroseconds>=timing.dataObjectExportMicroseconds+timing.identityConstructionMicroseconds+
                    timing.contextBindMicroseconds+timing.menuQueryMicroseconds+timing.menuEnumerationMicroseconds+timing.stateReductionMicroseconds,
                    "Native phase timings overlap or exceed actual worker duration");
            require(task->pollTimings(nullptr)==E_POINTER,"Native timing readback accepted null output");
            if(variant==5)require(result==E_ACCESSDENIED&&states.size()==1&&states.front().verb==L"untouched",
                                 "Failed batch preparation changed output or invented leaf states");
            else {
                succeeded(result,"Complete full native independent static state batch");
                require(states.size()==2&&states[0].verb==L"edit"&&states[1].verb==L"print",
                        "Batch reordered or omitted requested canonical states");
                require(states[1].status==HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),"Missing sibling state inherited another leaf's enablement");
                const auto expected=variant<2?S_OK:variant==3?E_UNEXPECTED:HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
                require(states[0].status==expected,"Batch lost the exact disabled/missing/ambiguous/submenu result");
                if(variant<2)require(states[0].native.contextMenu&&states[0].native.selectionCount==100001&&
                                    states[0].native.siteAttached&&states[0].native.checked()&&states[0].native.enabled()==(variant==0),
                                    "Batch truncated a full array or ignored the original site/tail state");
                AppSelectionStateBinding alias;alias.command=RibbonRunAsAnotherUser;alias.verb=L"print";alias.aliases={L"print",L"edit"};
                NamespaceCommandState reduced;reduced.selectionCount=71;
                const auto reducedStatus=applyAppSelectionStateBatch(alias,states,&reduced);
                if(variant<2)require(reducedStatus==S_OK&&reduced.delegatedCommand==L"edit"&&reduced.enabled()==(variant==0)&&reduced.selectionCount==100001,
                                    "Alias reduction inferred first-item availability or lost the exact full-menu leaf");
                else require(reducedStatus==expected&&reduced.selectionCount==71,"Alias reduction lost native missing/ambiguity or changed failed output");
            }
            require(selected->menu->queries==beforeQueries+1&&selected->itemReads==0&&selected->menu->invocations==0,
                    "Independent static states constructed multiple native menus or invoked a command");
            const auto retained=task.get();const std::array<std::wstring_view,2> duplicate{L"edit",L"EDIT"};
            require(NamespaceCommandStateTask::startSelectionVerbBatch(duplicate,selected.Get(),identity.Get(),&task)==E_INVALIDARG&&task.get()==retained,
                    "Duplicate batch input replaced a retained task");
        }
        selected->menu->queryStatus=S_OK;selected->menu->duplicate=selected->menu->cascade=selected->menu->omit=false;
        selected->menu->tailSupports=true;
        selected->menu->checked=false;
        {
            std::vector<std::wstring> names{L"edit"};
            for(unsigned index=0;index<31;++index)names.push_back(L"owned-absent-"+std::to_wstring(index));
            std::vector<std::wstring_view> verbs;for(const auto& name:names)verbs.emplace_back(name);
            const auto beforeQueries=selected->menu->queries.load();
            std::unique_ptr<NamespaceCommandStateTask> task;
            succeeded(NamespaceCommandStateTask::startSelectionVerbBatch(verbs,selected.Get(),identity.Get(),&task),
                      "More than sixteen independent state requests share one exact complete native menu");
            std::vector<NamespaceSelectionVerbState> states;succeeded(finishStateBatch(*task,&states),"Complete wide native state batch");
            require(states.size()==32&&states.front().status==S_OK&&states.front().native.selectionCount==100001&&
                    selected->menu->queries==beforeQueries+1,"Wide state batch split native menus or truncated original targets");
            for(size_t index=1;index<states.size();++index)require(states[index].status==HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
                    "Wide batch enabled an absent canonical leaf");
        }
        {
            selected->menu->block=true;ResetEvent(selected->menu->entered);ResetEvent(selected->menu->release);
            std::unique_ptr<NamespaceCommandStateTask> task;
            succeeded(NamespaceCommandStateTask::startSelectionVerbBatch(batchVerbs,selected.Get(),identity.Get(),&task),"Start cancellable complete menu batch");
            const auto until=GetTickCount64()+5000;
            while(WaitForSingleObject(selected->menu->entered,0)!=WAIT_OBJECT_0&&GetTickCount64()<until) {
                MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
                MsgWaitForMultipleObjectsEx(0,nullptr,1,QS_ALLINPUT,MWMO_ALERTABLE|MWMO_INPUTAVAILABLE);
            }
            require(WaitForSingleObject(selected->menu->entered,0)==WAIT_OBJECT_0,"Batch fixture did not begin native query");
            NamespaceCommandStateTimings pendingTime;pendingTime.workerMicroseconds=73;
            require(task->pollTimings(&pendingTime)==E_PENDING&&pendingTime.workerMicroseconds==73,"Pending timing readback changed caller output");
            const auto started=GetTickCount64();task->cancel();
            std::vector<NamespaceSelectionVerbState> preserved{{L"original",E_ABORT,{}}};
            require(task->pollSelectionVerbBatch(&preserved)==HRESULT_FROM_WIN32(ERROR_CANCELLED)&&preserved.size()==1&&
                    preserved.front().verb==L"original"&&GetTickCount64()-started<250,
                    "Batch cancellation blocked or published stale native state");
            require(task->pollTimings(&pendingTime)==HRESULT_FROM_WIN32(ERROR_CANCELLED)&&pendingTime.workerMicroseconds==73,
                    "Cancelled timing readback changed caller output or became capability authority");
            SetEvent(selected->menu->release);
            require(finishStateBatch(*task,&preserved)==HRESULT_FROM_WIN32(ERROR_CANCELLED)&&preserved.front().verb==L"original",
                    "Cancelled native batch changed output after cleanup");
            selected->menu->block=false;
        }
        selected->menu->block=true;ResetEvent(selected->menu->entered);ResetEvent(selected->menu->release);
        std::unique_ptr<NamespaceCommandStateTask> cancelled;
        succeeded(NamespaceCommandStateTask::startSelectionVerb(L"edit",selected.Get(),identity.Get(),&cancelled),
                  "Start intentionally delayed owned read-only native menu fixture");
        const auto deadline=GetTickCount64()+5000;
        while(WaitForSingleObject(selected->menu->entered,0)!=WAIT_OBJECT_0&&GetTickCount64()<deadline) {
            MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}Sleep(1);
        }
        require(WaitForSingleObject(selected->menu->entered,0)==WAIT_OBJECT_0,"Owned native static provider did not begin read-only enumeration");
        const auto cancelStart=GetTickCount64();cancelled->cancel();
        require(GetTickCount64()-cancelStart<250,"Static state cancellation waited for a blocked native provider");
        NamespaceCommandState untouched;untouched.state=ECS_CHECKED;untouched.selectionCount=17;
        require(cancelled->poll(&untouched)==HRESULT_FROM_WIN32(ERROR_CANCELLED)&&untouched.state==ECS_CHECKED&&untouched.selectionCount==17,
                "Cancelled native static capability published stale state");
        SetEvent(selected->menu->release);
        require(finishStateTask(*cancelled,&untouched)==HRESULT_FROM_WIN32(ERROR_CANCELLED)&&
                selected->menu->attachments==selected->menu->detachments,
                "Cancelled native menu worker leaked its site or changed output after draining");
        const auto original=cancelled.get();
        require(NamespaceCommandStateTask::startSelectionVerb(L"bad\\verb",selected.Get(),identity.Get(),&cancelled)==E_INVALIDARG&&cancelled.get()==original,
                "Malformed static task replaced the current generation");
        require(NamespaceCommandStateTask::startSelectionVerb(L"edit",nullptr,identity.Get(),&cancelled)==E_INVALIDARG,
                "Static state task accepted a missing original selection");
        require(NamespaceCommandStateTask::startSelectionVerb(L"edit",selected.Get(),identity.Get(),nullptr)==E_POINTER,
                "Static state task accepted a missing output");
        {
            const std::array<std::wstring_view,2> aliases{L"not-installed",L"edit"};
            selected->menu->block=false;
            std::unique_ptr<NamespaceCommandStateTask> task;
            succeeded(NamespaceCommandStateTask::startSelectionVerbs(aliases,selected.Get(),identity.Get(),&task),
                      "Start native alias state on the same complete retained menu");
            NamespaceCommandState state;succeeded(finishStateTask(*task,&state),"Read actual second canonical alias");
            require(state.delegatedCommand==L"edit"&&state.selectionCount==100001&&state.enabled(),
                    "Native alias state guessed the first verb or lost the full original array");
            const auto retained=task.get();
            const std::array<std::wstring_view,2> duplicate{L"edit",L"EDIT"};
            require(NamespaceCommandStateTask::startSelectionVerbs(duplicate,selected.Get(),identity.Get(),&task)==E_INVALIDARG&&
                    task.get()==retained,"Ambiguous alias input replaced a retained native task");
            require(NamespaceCommandStateTask::startSelectionVerbs({},selected.Get(),identity.Get(),&task)==E_INVALIDARG,
                    "An empty native alias list was accepted");
        }
        require(GetClipboardSequenceNumber()==clipboard&&selected->menu->invocations==0,
                "Read-only static menu state changed clipboard or activated an association");
    });
}

void actualLargeStaticMenuStateEquivalence() {
    onPrivateDesktop([] {
        Fixture fixture;const auto before=read(fixture.text),zipBefore=read(fixture.archive);
        const auto clipboard=GetClipboardSequenceNumber();
        WIN32_FILE_ATTRIBUTE_DATA textStamp{},archiveStamp{};
        require(GetFileAttributesExW(fixture.text.c_str(),GetFileExInfoStandard,&textStamp)&&
                GetFileAttributesExW(fixture.archive.c_str(),GetFileExInfoStandard,&archiveStamp),
                "Read original owned static-state source stamps");
        const auto folder=item(fixture.root),text=item(fixture.text),directory=item(fixture.directory);
        HiddenView host(folder.Get());
        struct Identities {PIDLIST_ABSOLUTE text=nullptr,directory=nullptr;~Identities(){CoTaskMemFree(text);CoTaskMemFree(directory);}} identities;
        succeeded(SHGetIDListFromObject(text.Get(),&identities.text),"Retain owned static-verb file identity");
        succeeded(SHGetIDListFromObject(directory.Get(),&identities.directory),"Retain last-folder native counterexample identity");
        std::vector<PCIDLIST_ABSOLUTE> ids(5001,identities.text);
        for(const bool mixed:{false,true}) {
            ids.back()=mixed?identities.directory:identities.text;
            ComPtr<IShellItemArray> selected;
            succeeded(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(ids.size()),ids.data(),&selected),
                      "Create actual full native 5001-item static capability selection");
            NativeNamespaceActions actions;
            succeeded(actions.initialize(host.owner,{folder,selected,host.view}),"Retain complete static capability array/site");
            NativeContextMenu menu;
            succeeded(menu.createSelection(nullptr,selected.Get(),host.view.Get(),CMF_EXTENDEDVERBS),
                      "Read real native full-selection comparison menu without launching an application");
            std::vector<ContextMenuEntry> entries;succeeded(menu.enumerate(entries,false),"Read native canonical leaf states only");
            menu.reset(); // Compare immutable metadata, never two live menus.
            const std::array<std::wstring_view,4> batchVerbs{L"copy",L"open",L"edit",L"print"};
            std::unique_ptr<NamespaceCommandStateTask> batch;
            succeeded(actions.startStaticVerbStateBatch(batchVerbs,&batch),"Read all large static leaves from one exact native menu");
            std::vector<NamespaceSelectionVerbState> states;succeeded(finishStateBatch(*batch,&states),"Complete actual 5001-item native batch");
            require(states.size()==batchVerbs.size(),"Native batch omitted requested states");
            for(size_t index=0;index<states.size();++index) {
                const auto& state=states[index];const auto leaf=std::find_if(entries.begin(),entries.end(),[&](const auto& entry){return entry.canonicalVerb==state.verb&&!entry.separator();});
                if(leaf==entries.end()||leaf->submenu)require(state.status==HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),"Native batch fabricated an absent large-selection leaf");
                else {succeeded(state.status,"Native batch lost a real complete-selection leaf");require(state.native.contextMenu&&state.native.identitySnapshot&&
                    state.native.selectionCount==5001&&state.native.enabled()==leaf->enabled(),"Native batch differs from fresh original full-selection menu");}
            }
            auto context=folderContext();context.selectionCount=5001;
            std::vector<AppCommandCapability> pendingCapabilities;
            for(const auto& binding:appCommandCatalog()) {
                AppCommandCapability capability;succeeded(queryAppCommand(actions,binding.command,context,&capability),"Read every applicable catalog batch route and alias");
                if(capability.status==E_PENDING&&!capability.selectionVerbs.empty())pendingCapabilities.push_back(capability);
            }
            std::vector<AppSelectionStateBinding> mapping;
            succeeded(startAppSelectionStateBatch(actions,pendingCapabilities,&batch,&mapping),"Batch cached compatible command capabilities and aliases");
            succeeded(finishStateBatch(*batch,&states),"Complete catalog native batch");
            std::set<std::wstring_view> uniqueVerbs;
            for(const auto& capability:pendingCapabilities)for(const auto verb:capability.selectionVerbs)uniqueVerbs.insert(verb);
            require(mapping.size()==pendingCapabilities.size()&&states.size()==uniqueVerbs.size(),
                    "Catalog batch omitted states or repeated the same canonical leaf");
            for(const auto& binding:mapping)require(std::any_of(states.begin(),states.end(),[&](const auto& state){return state.verb==binding.verb;}),
                    "Catalog control lost its exact native canonical mapping");
            for(const auto& binding:mapping) {
                NamespaceCommandState reduced;const auto status=applyAppSelectionStateBatch(binding,states,&reduced);
                const auto actual=std::find_if(states.begin(),states.end(),[&](const auto& state){return state.verb==binding.verb;});
                require(status==actual->status&&(FAILED(status)||reduced.enabled()==actual->native.enabled()),
                        "Catalog batch reduction changed actual independent native state");
            }
            const auto retained=batch.get();const auto originalMapping=mapping.size();
            AppCommandCapability registered;succeeded(queryAppCommand(actions,RibbonRemoveProperties,context,&registered),"Read separate registered Ribbon-only state");
            require(registered.selectionVerb.empty()&&startAppSelectionStateBatch(actions,std::span(&registered,1),&batch,&mapping)==E_INVALIDARG&&
                    batch.get()==retained&&mapping.size()==originalMapping,"Registered-only state joined the default selection-menu batch or changed output");
            for(const auto command:{Edit,Print}) {
                AppCommandCapability capability;succeeded(queryAppCommand(actions,command,context,&capability),
                    "Return pending instead of constructing large static menu in capability callback");
                require(capability.status==E_PENDING&&!capability.enabled,"Large static association callback invented an enabled state");
                const auto& verb=capability.selectionVerb;
                const auto actual=std::find_if(states.begin(),states.end(),[&](const auto& state){return state.verb==verb;});
                require(actual!=states.end()&&actual->status!=E_PENDING&&(FAILED(actual->status)||actual->native.contextMenu),
                    "Catalog batch kept a permanently pending static wrapper or used guessed state");
            }
            ComPtr<IShellItem> tail;succeeded(selected->GetItemAt(5000,&tail),"Read original retained static selection tail");
            int order=1;succeeded(tail->Compare(mixed?directory.Get():text.Get(),SICHINT_CANONICAL,&order),"Compare exact original static tail identity");
            require(!order&&actions.facts().selectionCount==5001,"Static state task truncated or replaced the original selected targets");
            require(actions.invokeCommandStore(L"Windows.print",true,{},NamespaceMenuScope::Selection,L"print")==E_ACCESSDENIED,
                    "Static capability worker weakened headless native invocation guard");
            actions.reset();
        }
        for(const DWORD count:{1u,2u,16u}) {
            for(const bool mixed:{false,true}) {
                if(mixed&&count==1)continue;
                std::vector<PCIDLIST_ABSOLUTE> smallIds(count,identities.text);
                if(mixed)smallIds.back()=identities.directory;
                ComPtr<IShellItemArray> selected;
                succeeded(SHCreateShellItemArrayFromIDLists(count,smallIds.data(),&selected),"Create exact native small/mixed selection");
                NativeNamespaceActions actions;succeeded(actions.initialize(host.owner,{folder,selected,host.view}),"Retain small native association scope");
                NativeContextMenu menu;succeeded(menu.createSelection(nullptr,selected.Get(),host.view.Get(),CMF_EXTENDEDVERBS),
                                                "Read direct native single/mixed/MultiSelectModel leaf authority");
                std::vector<ContextMenuEntry> entries;succeeded(menu.enumerate(entries,false),"Retain actual small native states");menu.reset();
                for(const auto verb:{L"edit",L"print"}) {
                    NamespaceCommandState state;state.state=ECS_CHECKED;
                    const auto fast=actions.queryStaticVerbState(verb,&state);
                    require(FAILED(fast)&&state.state==ECS_CHECKED,"Association presence fabricated an enabled static state or changed output");
                    if(count>1)require(fast==E_PENDING,"Mixed/MultiSelectModel states were inferred from per-item association presence");
                    std::unique_ptr<NamespaceCommandStateTask> task;
                    succeeded(actions.startStaticVerbStateTask(verb,&task),"Read exact small native static state on worker");
                    const auto status=finishStateTask(*task,&state);
                    const auto leaf=std::find_if(entries.begin(),entries.end(),[verb](const auto& entry){return entry.canonicalVerb==verb&&!entry.separator();});
                    if(leaf==entries.end()||leaf->submenu)require(status==HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)&&state.state==ECS_CHECKED,
                        "Missing small/mixed/MultiSelectModel leaf was enabled by association inference");
                    else{ require(fast==E_PENDING,"Single-item association prefilter rejected a real native static leaf");
                        succeeded(status,"Native small selection worker failed");require(state.contextMenu&&state.identitySnapshot&&
                        state.selectionCount==count&&state.enabled()==leaf->enabled(),"Static state differs from actual native small/mixed/MultiSelectModel menu");}
                }
                actions.reset();
            }
        }
        const bool textUnchanged=read(fixture.text)==before,archiveUnchanged=read(fixture.archive)==zipBefore;
        const bool directoryEmpty=fs::is_empty(fixture.directory),clipboardUnchanged=GetClipboardSequenceNumber()==clipboard;
        const bool ownerHidden=!IsWindowVisible(host.owner);
        bool inputUnchanged=false,visibleInput=true;
        const auto desktop=PrivateDesktop::current();
        const auto isolated=desktop?desktop->verifyIsolation(&inputUnchanged):E_ACCESSDENIED;
        const auto observed=desktop?desktop->visibleWindowsOnInputDesktop(visibleInput):E_ACCESSDENIED;
        WIN32_FILE_ATTRIBUTE_DATA textAfter{},archiveAfter{};
        const auto sameStamp=[](const WIN32_FILE_ATTRIBUTE_DATA& before,const WIN32_FILE_ATTRIBUTE_DATA& after) {
            return before.dwFileAttributes==after.dwFileAttributes&&before.nFileSizeHigh==after.nFileSizeHigh&&
                before.nFileSizeLow==after.nFileSizeLow&&CompareFileTime(&before.ftCreationTime,&after.ftCreationTime)==0&&
                CompareFileTime(&before.ftLastWriteTime,&after.ftLastWriteTime)==0;
        };
        const bool stampsUnchanged=GetFileAttributesExW(fixture.text.c_str(),GetFileExInfoStandard,&textAfter)&&
            GetFileAttributesExW(fixture.archive.c_str(),GetFileExInfoStandard,&archiveAfter)&&
            sameStamp(textStamp,textAfter)&&sameStamp(archiveStamp,archiveAfter);
        const bool unchanged=textUnchanged&&archiveUnchanged&&directoryEmpty&&clipboardUnchanged&&ownerHidden&&
            SUCCEEDED(isolated)&&inputUnchanged&&SUCCEEDED(observed)&&!visibleInput&&stampsUnchanged;
        if(!unchanged)std::cout<<"Static-state isolation: textUnchanged="<<textUnchanged<<" archiveUnchanged="<<archiveUnchanged
            <<" directoryEmpty="<<directoryEmpty<<" clipboardSequenceUnchanged="<<clipboardUnchanged<<" ownerHidden="<<ownerHidden
            <<" inputDesktopUnchanged="<<inputUnchanged<<" noVisibleInputWindow="<<!visibleInput
            <<" sourceFileStampUnchanged="<<stampsUnchanged<<" isolationStatus="<<static_cast<unsigned long>(isolated)
            <<" visibilityStatus="<<static_cast<unsigned long>(observed)<<'\n';
        require(unchanged,
                "Static native menu capability checks changed owned files/clipboard or displayed application UI");
    });
}

void handlerPresentationAndActivationGuards() {
    for (const auto key : {L"Windows.ShareSpecificUsers",L"Windows.SharePrivate",L"Windows.RibbonPermissionsDialog"}) {
        NamespaceCommandMetadata metadata;
        succeeded(namespaceCommandMetadata(key,&metadata),"Read real provider presentation");
        require(!metadata.label.empty() && !metadata.icon.empty(),"Native Share provider's title/icon missing");
        const auto registryPath = std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\CommandStore\\shell\\")+key;
        wchar_t handler[64]{};
        DWORD size = sizeof(handler);
        succeeded(HRESULT_FROM_WIN32(RegGetValueW(HKEY_LOCAL_MACHINE,registryPath.c_str(),L"ExplorerCommandHandler",RRF_RT_REG_SZ,nullptr,handler,&size)),"Read exact installed provider CLSID");
        CLSID clsid{};
        succeeded(CLSIDFromString(handler,&clsid),"Parse actual native Share presentation class");
        ComPtr<IExplorerCommand> provider;
        succeeded(CoCreateInstance(clsid,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&provider)),"Read independently activated native presentation provider");
        PWSTR raw = nullptr;
        const auto hr = provider->GetIcon(nullptr,&raw);
        const std::wstring native = raw ? raw : L"";
        CoTaskMemFree(raw);
        succeeded(hr,"Read real IExplorerCommand icon without invocation");
        require(metadata.icon == native,"Registry legacy/missing Icon overrode actual native handler artwork");
    }
    NativeNamespaceActions actions;
    auto context = folderContext();
    for (const auto& entry : appCommandCatalog()) {
        require(invokeAppNativeCommand(actions,entry.command,context,true) == E_ACCESSDENIED,"Catalog command crossed universal headless activation guard");
        if (entry.route != AppCommandRoute::Host && appCommandApplicable(entry,context))
            require(invokeAppNativeCommand(actions,entry.command,context,false) == E_ACCESSDENIED,"Native action accepted absent/hidden owner");
    }
    AppCommandCapability unavailable;
    succeeded(queryAppCommand(actions,Undo,context,&unavailable),"Mapped unavailable capability returns structured status");
    require(!unavailable.enabled && unavailable.status == E_UNEXPECTED,"Uninitialized provider capability invented enabled state");
}
} // namespace

int runAppCommandTests() {
    const std::vector<std::pair<const char*,std::function<void()>>> groups{
        {"complete unique command mapping and native scope",catalogCoverageAndExactRoutes},
        {"semantic applicability, failure output and STA contracts",applicabilityAndOutputContracts},
        {"entire-selection native fast Kind intersection without hydration",entireSelectionFastKindIntersection},
        {"cancelled state site lifetime and creator apartment shutdown",cancelledStateSiteApartmentLifetime},
        {"actual view fast state equivalence and full native property cascades",actualViewFastStateAndNativePopups},
        {"native Search location delegation and full virtual/multiple provider targets",nativeSearchLocationDelegatesAndTargets},
        {"native Network, Shortcut, Library, static property and retained cascade contracts",nativeAdditionalToolsAndRetainedHierarchy},
        {"native deletion pending-state/menu equivalence and unchanged files/clipboard",nativeDeletionStateAndUntouchedPayloads},
        {"native multiple-item sharing/access state and strict mutation guards",nativeMultipleAccessStateAndUntouchedPermissions},
        {"actual associations, compressed handlers and unchanged owned payloads",realAssociationsAndZipCapabilities},
        {"strict isolated native view selection whitelist and exact counts",nativeViewSelectionWhitelist},
        {"static native menu worker full arrays, exact leaf failures and cancellation",staticMenuWorkerContractsAndCancellation},
        {"malformed exported full-selection CIDA rejection and unchanged public outputs",malformedExportedSelectionIdentity},
        {"worker-only native resource restrictions, exact failures and unrestricted resource aliases",resourceVerbStateRestrictions},
        {"default-menu native work reservation survives cancellation and task destruction",defaultMenuWorkerSlotSurvivesCancellation},
        {"actual native 5001-item static menu worker/direct state equivalence",actualLargeStaticMenuStateEquivalence},
        {"native registered Ribbon-only states and application-shortcut redirection",nativeRegisteredStaticAndShortcutState},
        {"real native Share artwork and universal headless activation guard",handlerPresentationAndActivationGuards}
    };
    unsigned failures = 0;
    for (const auto& [name,test] : groups) {
        try { test(); std::cout << "PASS: App commands: " << name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL: App commands: " << name << ": " << error.what() << '\n'; }
        catch (...) { ++failures; std::cerr << "FAIL: App commands: " << name << ": unknown exception\n"; }
    }
    std::cout << groups.size()-failures << '/' << groups.size() << " app command groups passed; " << assertions << " assertions\n";
    try{succeeded(drainStaWorkers(10000),"Drain catalog suite native workers while creator COM remains initialized");}
    catch(const std::exception& error){++failures;std::cerr<<"FAIL: App commands: final STA drain: "<<error.what()<<'\n';}
    return static_cast<int>(failures);
}

int runNativeMenuStateTests() {
    try {
        nativeLeafStateMenuEquivalence();
        std::cout << "PASS: native root-leaf state versus synchronous cascades on full 1/2/16/5001/10000 targets\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << "FAIL: native root-leaf state equivalence: " << error.what() << '\n';
        return 1;
    } catch(...) {
        std::cerr << "FAIL: native root-leaf state equivalence: unknown exception\n";
        return 1;
    }
}
