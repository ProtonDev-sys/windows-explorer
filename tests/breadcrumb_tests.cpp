#include "explorer/breadcrumb.hpp"
#include "explorer/worker_sta.hpp"

#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/implements.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;
using explorer::BreadcrumbDropTarget;
using explorer::BreadcrumbEnumerationOptions;
using explorer::BreadcrumbEnumerationTask;
using explorer::BreadcrumbSnapshot;
namespace fs = std::filesystem;
unsigned assertions = 0;

void require(bool value,const char* text) {
    ++assertions;
    if (!value) throw std::runtime_error(text);
}
void succeeded(HRESULT value,const char* text) {
    ++assertions;
    if (FAILED(value)) {
        std::cerr << "HRESULT 0x" << std::hex << static_cast<unsigned long>(value) << std::dec << '\n';
        throw std::runtime_error(text);
    }
}

struct Fixture {
    fs::path root,first,second,hidden,empty,nested,text;
    explicit Fixture(const fs::path& base = fs::temp_directory_path()) {
        GUID id{};
        succeeded(CoCreateGuid(&id),"Generate breadcrumb fixture identity");
        wchar_t formatted[40]{};
        require(StringFromGUID2(id,formatted,40) != 0,"Format breadcrumb fixture identity");
        root = base / (std::wstring(L"WindowsExplorer-Breadcrumb-") + formatted);
        require(fs::create_directory(root),"Create owned breadcrumb fixture");
        first = root / L"Folder 2";
        second = root / L"Folder 10";
        hidden = root / L"Hidden folder";
        empty = root / L"\u65E5\u672C\u8A9E \u03BB \U0001F4C1";
        nested = first / L"Nested \u65E5\u672C";
        text = root / L"regular-file.txt";
        for (const auto& path : {first,second,hidden,empty,nested})
            require(fs::create_directory(path),"Create owned breadcrumb subfolder");
        require(SetFileAttributesW(hidden.c_str(),FILE_ATTRIBUTE_HIDDEN),"Set hidden attribute only on owned folder");
        std::ofstream output(text,std::ios::binary);
        output << "breadcrumb fixture must remain unchanged";
        require(output.good(),"Write owned breadcrumb file");
    }
    ~Fixture() { std::error_code ignored; fs::remove_all(root,ignored); }
};

ComPtr<IShellItem> item(const fs::path& path) {
    ComPtr<IShellItem> result;
    succeeded(SHCreateItemFromParsingName(path.c_str(),nullptr,IID_PPV_ARGS(&result)),"Create owned breadcrumb Shell item");
    return result;
}

bool same(IShellItem* left,IShellItem* right) {
    int comparison = 1;
    succeeded(left->Compare(right,static_cast<SICHINTF>(SICHINT_CANONICAL | SICHINT_TEST_FILESYSPATH_IF_NOT_EQUAL),&comparison),
              "Compare breadcrumb Shell identities");
    return comparison == 0;
}

std::wstring filePath(IShellItem* value) {
    PWSTR text = nullptr;
    succeeded(value->GetDisplayName(SIGDN_FILESYSPATH,&text),"Read owned breadcrumb destination path");
    require(text != nullptr,"Shell returned null breadcrumb path");
    std::wstring path(text);
    CoTaskMemFree(text);
    return path;
}

void nativeOrder(IShellItem* parent,const BreadcrumbSnapshot& snapshot) {
    ComPtr<IShellFolder> folder;
    succeeded(parent->BindToHandler(nullptr,BHID_SFObject,IID_PPV_ARGS(&folder)),"Bind native breadcrumb folder for order readback");
    PIDLIST_ABSOLUTE parentId = nullptr;
    succeeded(SHGetIDListFromObject(parent,&parentId),"Read parent breadcrumb PIDL for relative order");
    for (size_t index = 1; index < snapshot.children.size(); ++index) {
        PIDLIST_ABSOLUTE left = nullptr,right = nullptr;
        succeeded(SHGetIDListFromObject(snapshot.children[index - 1].item.Get(),&left),"Read previous native breadcrumb PIDL");
        const HRESULT second = SHGetIDListFromObject(snapshot.children[index].item.Get(),&right);
        if (FAILED(second)) { CoTaskMemFree(left); succeeded(second,"Read next native breadcrumb PIDL"); }
        const PCUIDLIST_RELATIVE leftChild = ILFindChild(parentId,left),rightChild = ILFindChild(parentId,right);
        const HRESULT compared = leftChild && rightChild ? folder->CompareIDs(0,leftChild,rightChild) : E_UNEXPECTED;
        CoTaskMemFree(left);
        CoTaskMemFree(right);
        succeeded(compared,"Read native breadcrumb ordering");
        require(static_cast<short>(HRESULT_CODE(compared)) <= 0,"Breadcrumb order differs from provider CompareIDs order");
    }
    CoTaskMemFree(parentId);
}

void foldersHiddenUnicodeAndSelection() {
    Fixture fixture;
    auto parent = item(fixture.root),selected = item(fixture.second),hidden = item(fixture.hidden),unicode = item(fixture.empty);
    BreadcrumbSnapshot snapshot;
    BreadcrumbEnumerationOptions options;
    require(explorer::enumerateBreadcrumbChildren(parent.Get(),selected.Get(),options,&snapshot) == S_OK,
            "Native owned breadcrumb enumeration failed");
    require(snapshot.complete && snapshot.stopReason == S_OK && snapshot.children.size() == 3,
            "Default breadcrumb snapshot did not exclude hidden and nonfolder items");
    unsigned selectedCount = 0;
    bool unicodeFound = false;
    for (const auto& child : snapshot.children) {
        require(child.item != nullptr && !child.label.empty(),"Breadcrumb child is missing its native item or localized label");
        SFGAOF attributes = 0;
        succeeded(child.item->GetAttributes(SFGAO_FOLDER | SFGAO_HIDDEN,&attributes),"Read native breadcrumb attributes");
        require((attributes & SFGAO_FOLDER) && !(attributes & SFGAO_HIDDEN),"Breadcrumb included a hidden or nonfolder destination");
        require(!same(child.item.Get(),hidden.Get()),"Hidden folder leaked into default breadcrumb list");
        require(child.selected == same(child.item.Get(),selected.Get()),"Selected breadcrumb identity did not match native comparison");
        selectedCount += child.selected ? 1u : 0u;
        if (same(child.item.Get(),unicode.Get())) {
            unicodeFound = true;
            require(child.label == fixture.empty.filename().wstring(),"Unicode breadcrumb display label was changed");
            require(fs::equivalent(filePath(child.item.Get()),fixture.empty),"Unicode breadcrumb destination did not retain its actual filesystem identity");
        }
    }
    require(selectedCount == 1 && unicodeFound,"Selected or Unicode breadcrumb destination is missing");
    nativeOrder(parent.Get(),snapshot);
    options.showHidden = true;
    require(explorer::enumerateBreadcrumbChildren(parent.Get(),hidden.Get(),options,&snapshot) == S_OK && snapshot.children.size() == 4,
            "Show-hidden breadcrumb policy did not include the owned hidden folder");
    require(std::count_if(snapshot.children.begin(),snapshot.children.end(),[](const auto& child) { return child.selected; }) == 1,
            "Hidden selected breadcrumb identity was lost");
    nativeOrder(parent.Get(),snapshot);
    auto first = item(fixture.first),empty = item(fixture.empty);
    require(explorer::enumerateBreadcrumbChildren(first.Get(),nullptr,{},&snapshot) == S_OK && snapshot.children.size() == 1,
            "Nested breadcrumb enumeration was not lazy/direct-child only");
    require(fs::equivalent(filePath(snapshot.children.front().item.Get()),fixture.nested),"Nested breadcrumb destination has the wrong filesystem identity");
    require(explorer::enumerateBreadcrumbChildren(empty.Get(),nullptr,{},&snapshot) == S_OK && snapshot.children.empty() && snapshot.complete,
            "Empty native folder did not produce a complete empty breadcrumb snapshot");
    std::ifstream input(fixture.text,std::ios::binary);
    const std::string unchanged{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
    require(unchanged == "breadcrumb fixture must remain unchanged","Read-only breadcrumb enumeration changed owned file contents");
}

void boundedFailureAndVirtualNamespaces() {
    Fixture fixture;
    auto parent = item(fixture.root),file = item(fixture.text);
    BreadcrumbSnapshot result;
    result.stopReason = E_ABORT;
    require(explorer::enumerateBreadcrumbChildren(nullptr,nullptr,{},&result) == E_INVALIDARG && result.stopReason == E_ABORT,
            "Null parent did not preserve output on failure");
    require(explorer::enumerateBreadcrumbChildren(parent.Get(),nullptr,{},nullptr) == E_POINTER,"Null snapshot accepted");
    BreadcrumbEnumerationOptions options;
    options.maximumEntries = 0;
    require(explorer::enumerateBreadcrumbChildren(parent.Get(),nullptr,options,&result) == E_INVALIDARG,"Zero entry budget accepted");
    options.maximumEntries = 4097;
    require(explorer::enumerateBreadcrumbChildren(parent.Get(),nullptr,options,&result) == E_INVALIDARG,"Unbounded entry budget accepted");
    options = {};
    options.timeBudgetMilliseconds = 0;
    require(explorer::enumerateBreadcrumbChildren(parent.Get(),nullptr,options,&result) == E_INVALIDARG,"Zero time budget accepted");
    options.timeBudgetMilliseconds = 60001;
    require(explorer::enumerateBreadcrumbChildren(parent.Get(),nullptr,options,&result) == E_INVALIDARG,"Unbounded time budget accepted");
    options = {};
    options.cancelled = std::make_shared<std::atomic<bool>>(true);
    require(explorer::enumerateBreadcrumbChildren(parent.Get(),nullptr,options,&result) == HRESULT_FROM_WIN32(ERROR_CANCELLED) &&
            result.stopReason == E_ABORT,"Cancelled enumeration changed output or called native provider");
    require(FAILED(explorer::enumerateBreadcrumbChildren(file.Get(),nullptr,{},&result)) && result.stopReason == E_ABORT,
            "Nonfolder parent unexpectedly became a breadcrumb folder");
    options = {};
    options.maximumEntries = 2;
    require(explorer::enumerateBreadcrumbChildren(parent.Get(),nullptr,options,&result) == S_FALSE && result.children.size() == 2 &&
            !result.complete && result.stopReason == HRESULT_FROM_WIN32(ERROR_MORE_DATA),"Entry bound did not return a usable partial snapshot");
    nativeOrder(parent.Get(),result);
    options.maximumEntries = 3;
    require(explorer::enumerateBreadcrumbChildren(parent.Get(),nullptr,options,&result) == S_OK && result.complete,
            "Exact entry count incorrectly reported truncation");
    ComPtr<IShellItem> desktop;
    succeeded(SHGetKnownFolderItem(FOLDERID_Desktop,KF_FLAG_DEFAULT,nullptr,IID_PPV_ARGS(&desktop)),"Create virtual desktop breadcrumb item");
    options.maximumEntries = 64;
    options.timeBudgetMilliseconds = 1000;
    const HRESULT hr = explorer::enumerateBreadcrumbChildren(desktop.Get(),nullptr,options,&result);
    succeeded(hr,"Read-only null-owner virtual desktop breadcrumb enumeration");
    require(!result.children.empty(),"Virtual desktop has no native folder destinations");
    for (const auto& child : result.children) {
        require(child.item != nullptr && !child.label.empty(),"Virtual namespace child was reduced to a filesystem-only path");
        SFGAOF attributes = 0;
        succeeded(child.item->GetAttributes(SFGAO_FOLDER,&attributes),"Read virtual breadcrumb folder attributes");
        require((attributes & SFGAO_FOLDER) != 0,"Virtual breadcrumb contains a nonfolder item");
    }
    nativeOrder(desktop.Get(),result);
}

HRESULT waitTask(BreadcrumbEnumerationTask* task,BreadcrumbSnapshot& result) {
    const ULONGLONG deadline = GetTickCount64() + 5000;
    for (;;) {
        const HRESULT hr = task->poll(&result);
        if (hr != E_PENDING) return hr;
        require(GetTickCount64() < deadline,"Asynchronous breadcrumb enumeration timed out");
        MSG message{};
        while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        Sleep(1);
    }
}

void asynchronousSnapshotAndCancellation() {
    Fixture fixture;
    auto parent = item(fixture.root),selected = item(fixture.second);
    std::unique_ptr<BreadcrumbEnumerationTask> task;
    require(BreadcrumbEnumerationTask::start(nullptr,nullptr,{},&task) == E_INVALIDARG && !task,"Asynchronous null parent accepted");
    require(BreadcrumbEnumerationTask::start(parent.Get(),nullptr,{},nullptr) == E_POINTER,"Asynchronous null output accepted");
    succeeded(BreadcrumbEnumerationTask::start(parent.Get(),selected.Get(),{},&task),"Start asynchronous native breadcrumb snapshot");
    BreadcrumbSnapshot snapshot;
    require(task->poll(nullptr) == E_POINTER,"Asynchronous null poll output accepted");
    require(waitTask(task.get(),snapshot) == S_OK && snapshot.children.size() == 3 && snapshot.complete,
            "Worker snapshot did not materialize complete native items on caller STA");
    nativeOrder(parent.Get(),snapshot);
    require(std::count_if(snapshot.children.begin(),snapshot.children.end(),[](const auto& child) { return child.selected; }) == 1,
            "Worker snapshot lost selected PIDL identity");
    for (const auto& child : snapshot.children) require(fs::is_directory(filePath(child.item.Get())),"Worker item failed caller-apartment path readback");
    HRESULT wrongThread = S_OK;
    std::thread other([&]() { BreadcrumbSnapshot ignored; wrongThread = task->poll(&ignored); });
    other.join();
    require(wrongThread == RPC_E_WRONG_THREAD,"Asynchronous Shell items were materialized on an unauthorized thread");
    task->cancel();
    snapshot.stopReason = E_ABORT;
    require(task->poll(&snapshot) == HRESULT_FROM_WIN32(ERROR_CANCELLED) && snapshot.stopReason == E_ABORT,
            "Task cancellation did not preserve caller snapshot");
    const ULONGLONG started = GetTickCount64();
    task.reset();
    require(GetTickCount64() - started < 1000,"Task destruction blocks waiting for a provider");
    BreadcrumbEnumerationOptions bounded;
    bounded.maximumEntries = 2;
    succeeded(BreadcrumbEnumerationTask::start(parent.Get(),nullptr,bounded,&task),"Start bounded worker snapshot");
    require(waitTask(task.get(),snapshot) == S_FALSE && snapshot.children.size() == 2 && !snapshot.complete &&
            snapshot.stopReason == HRESULT_FROM_WIN32(ERROR_MORE_DATA),"Worker truncated snapshot lost native stop reason");
    task.reset();
    bounded.cancelled = std::make_shared<std::atomic<bool>>(true);
    require(BreadcrumbEnumerationTask::start(parent.Get(),nullptr,bounded,&task) == HRESULT_FROM_WIN32(ERROR_CANCELLED) && !task,
            "Already cancelled worker was started");
    HRESULT nonSta = S_OK;
    std::thread mta([&]() {
        const HRESULT initialized = CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        BreadcrumbSnapshot ignored;
        nonSta = explorer::enumerateBreadcrumbChildren(parent.Get(),nullptr,{},&ignored);
        if (SUCCEEDED(initialized)) CoUninitialize();
    });
    mta.join();
    require(nonSta == RPC_E_WRONG_THREAD,"Native breadcrumb enumeration accepted a non-STA caller");
}

struct DataStats { unsigned destroyed = 0,rendered = 0; };
class FakeData final : public RuntimeClass<RuntimeClassFlags<ClassicCom>,IDataObject> {
public:
    explicit FakeData(std::shared_ptr<DataStats> stats) : stats_(std::move(stats)) {}
    ~FakeData() { ++stats_->destroyed; }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC*,STGMEDIUM*) override { ++stats_->rendered; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*,STGMEDIUM*) override { ++stats_->rendered; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*,FORMATETC*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*,STGMEDIUM*,BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD,IEnumFORMATETC**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*,DWORD,IAdviseSink*,DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
private:
    std::shared_ptr<DataStats> stats_;
};

struct DropStats {
    unsigned entered = 0,over = 0,left = 0,dropped = 0,siteAttached = 0,siteDetached = 0,destroyed = 0;
    DWORD keys = 0,receivedEffect = 0,returnedEffect = DROPEFFECT_COPY;
    POINTL point{};
    IDataObject* data = nullptr;
    HRESULT enterResult = S_OK,overResult = S_OK,leaveResult = S_OK,dropResult = S_OK,siteResult = S_OK;
};
class FakeTarget final : public RuntimeClass<RuntimeClassFlags<ClassicCom>,IDropTarget,IObjectWithSite> {
public:
    explicit FakeTarget(std::shared_ptr<DropStats> stats) : stats_(std::move(stats)) {}
    ~FakeTarget() { ++stats_->destroyed; }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data,DWORD keys,POINTL point,DWORD* effect) override {
        ++stats_->entered; remember(data,keys,point,effect); return stats_->enterResult;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys,POINTL point,DWORD* effect) override {
        ++stats_->over; remember(stats_->data,keys,point,effect); return stats_->overResult;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override { ++stats_->left; return stats_->leaveResult; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data,DWORD keys,POINTL point,DWORD* effect) override {
        ++stats_->dropped; remember(data,keys,point,effect); return stats_->dropResult;
    }
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) override {
        if (site) ++stats_->siteAttached; else ++stats_->siteDetached;
        if (FAILED(stats_->siteResult)) return stats_->siteResult;
        site_ = site;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSite(REFIID iid,void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        return site_ ? site_->QueryInterface(iid,result) : E_FAIL;
    }
private:
    void remember(IDataObject* data,DWORD keys,POINTL point,DWORD* effect) {
        stats_->data = data; stats_->keys = keys; stats_->point = point;
        stats_->receivedEffect = *effect; *effect = stats_->returnedEffect;
    }
    std::shared_ptr<DropStats> stats_;
    ComPtr<IUnknown> site_;
};

struct DragFixture {
    Fixture files;
    ComPtr<IShellItem> first = item(files.first),firstAlias = item(files.first),second = item(files.second),file = item(files.text);
    std::shared_ptr<DropStats> firstStats = std::make_shared<DropStats>(),secondStats = std::make_shared<DropStats>();
    ComPtr<FakeTarget> firstTarget = Make<FakeTarget>(firstStats),secondTarget = Make<FakeTarget>(secondStats);
    std::shared_ptr<DataStats> dataStats = std::make_shared<DataStats>();
    ComPtr<FakeData> data = Make<FakeData>(dataStats);
    unsigned binds = 0,hits = 0;
    HRESULT hitResult = S_OK,bindResult = S_OK;
    explorer::BreadcrumbDropOptions options(bool headless = false) {
        explorer::BreadcrumbDropOptions options;
        options.headless = headless;
        options.site = data;
        options.hitTest = [this](POINTL point,IShellItem** result) {
            ++hits;
            *result = nullptr;
            if (FAILED(hitResult)) return hitResult;
            if (point.x < 0) return S_FALSE;
            auto target = point.x < 20 ? first : point.x < 100 ? firstAlias : point.x < 200 ? second : file;
            *result = target.Detach();
            return S_OK;
        };
        options.bindTarget = [this](IShellItem* target,IDropTarget** result) {
            ++binds;
            *result = nullptr;
            if (FAILED(bindResult)) return bindResult;
            int order = 1;
            const HRESULT compared = first->Compare(target,SICHINT_CANONICAL,&order);
            if (FAILED(compared)) return compared;
            return (order == 0 ? firstTarget : secondTarget).CopyTo(result);
        };
        return options;
    }
    ComPtr<BreadcrumbDropTarget> controller(bool headless = false) {
        ComPtr<BreadcrumbDropTarget> target;
        succeeded(BreadcrumbDropTarget::create(nullptr,options(headless),&target),"Create isolated breadcrumb drag controller");
        return target;
    }
};

void dragLifecycleModifiersAndCanonicalIdentity() {
    DragFixture fixture;
    auto controller = fixture.controller();
    const DWORD allowed = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    DWORD effect = allowed;
    succeeded(controller->DragEnter(fixture.data.Get(),MK_CONTROL,{10,30},&effect),"Delegate fake native DragEnter");
    require(fixture.binds == 1 && fixture.firstStats->entered == 1 && fixture.firstStats->siteAttached == 1 && effect == DROPEFFECT_COPY,
            "Initial native target/site/effect was not delegated");
    require(fixture.firstStats->data == fixture.data.Get() && fixture.firstStats->keys == MK_CONTROL &&
            fixture.firstStats->point.x == 10 && fixture.firstStats->point.y == 30 && fixture.firstStats->receivedEffect == allowed,
            "Native DragEnter lost original data object, modifiers, screen coordinates, or allowed effects");
    effect = DROPEFFECT_COPY;
    fixture.firstStats->returnedEffect = DROPEFFECT_MOVE | DROPEFFECT_SCROLL;
    succeeded(controller->DragOver(MK_SHIFT,{40,35},&effect),"Delegate fake native DragOver to canonical alias");
    require(fixture.binds == 1 && fixture.firstStats->entered == 1 && fixture.firstStats->over == 1 && fixture.firstStats->left == 0,
            "Same canonical folder unnecessarily restarted native drag feedback");
    require(effect == (DROPEFFECT_MOVE | DROPEFFECT_SCROLL) && fixture.firstStats->receivedEffect == allowed &&
            fixture.firstStats->keys == MK_SHIFT,"DragOver failed to preserve source effects or native modifier behavior");
    effect = allowed;
    succeeded(controller->DragOver(MK_CONTROL | MK_SHIFT,{110,40},&effect),"Route drag across breadcrumb destinations");
    require(fixture.binds == 2 && fixture.firstStats->left == 1 && fixture.firstStats->siteDetached == 1 &&
            fixture.secondStats->entered == 1 && fixture.secondStats->siteAttached == 1,"Destination transition did not leave/detach old target and enter new target");
    require(fixture.secondStats->data == fixture.data.Get() && fixture.secondStats->keys == (MK_CONTROL | MK_SHIFT),
            "Destination transition changed native data or modifiers");
    effect = allowed;
    succeeded(controller->Drop(fixture.data.Get(),MK_ALT,{115,42},&effect),"Invoke isolated fake-provider Drop");
    require(fixture.secondStats->dropped == 1 && fixture.secondStats->left == 0 && fixture.secondStats->siteDetached == 1,
            "Drop did not preserve provider lifecycle or incorrectly called DragLeave afterwards");
    require(fixture.secondStats->keys == MK_ALT && fixture.secondStats->data == fixture.data.Get() && effect == DROPEFFECT_COPY,
            "Drop lost native modifiers, original data identity, or effect");
    effect = allowed;
    succeeded(controller->DragOver(0,{10,0},&effect),"DragOver after completed drop");
    require(effect == DROPEFFECT_NONE && fixture.firstStats->over == 1,"Completed drop retained an active drag data object");
    require(fixture.dataStats->rendered == 0,"Drag tests rendered or published external transfer data");
}

void dragBoundsAndProviderFailures() {
    DragFixture fixture;
    auto controller = fixture.controller();
    DWORD effect = DROPEFFECT_COPY;
    fixture.firstStats->returnedEffect = DROPEFFECT_MOVE | DROPEFFECT_LINK | DROPEFFECT_SCROLL;
    succeeded(controller->DragEnter(fixture.data.Get(),0,{10,0},&effect),"Enter disallowed fake target effect");
    require(effect == DROPEFFECT_NONE,"Provider was allowed to broaden source effects or return scroll without an action");
    effect = DROPEFFECT_COPY;
    fixture.firstStats->returnedEffect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
    succeeded(controller->DragOver(0,{10,0},&effect),"Clamp provider effect to allowed copy");
    require(effect == DROPEFFECT_COPY,"Native effect was not restricted to original source mask");
    effect = DROPEFFECT_COPY;
    succeeded(controller->DragOver(0,{-1,0},&effect),"Leave all breadcrumb targets");
    require(effect == DROPEFFECT_NONE && fixture.firstStats->left == 1 && fixture.firstStats->siteDetached == 1,
            "Empty hit did not clear native target feedback");
    effect = DROPEFFECT_COPY;
    succeeded(controller->DragOver(0,{210,0},&effect),"Exclude nonfolder drop destination");
    require(effect == DROPEFFECT_NONE && fixture.binds == 1,"Nonfolder breadcrumb was passed to a native drop target");
    fixture.bindResult = E_NOINTERFACE;
    effect = DROPEFFECT_COPY;
    require(controller->DragOver(0,{110,0},&effect) == E_NOINTERFACE && effect == DROPEFFECT_NONE,
            "Unavailable native target was not disabled with its exact HRESULT");
    fixture.bindResult = S_OK;
    fixture.hitResult = HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    effect = DROPEFFECT_COPY;
    require(controller->DragOver(0,{10,0},&effect) == fixture.hitResult && effect == DROPEFFECT_NONE,"Hit test failure HRESULT was lost");
    fixture.hitResult = S_OK;
    fixture.firstStats->enterResult = E_ABORT;
    effect = DROPEFFECT_COPY;
    require(controller->DragOver(0,{10,0},&effect) == E_ABORT && effect == DROPEFFECT_NONE && fixture.firstStats->left == 2,
            "Provider DragEnter failure did not terminate target feedback");
    fixture.firstStats->enterResult = S_OK;
    effect = DROPEFFECT_COPY;
    succeeded(controller->DragOver(0,{10,0},&effect),"Reenter target after provider failure");
    fixture.firstStats->overResult = E_FAIL;
    effect = DROPEFFECT_COPY;
    require(controller->DragOver(0,{10,0},&effect) == E_FAIL && effect == DROPEFFECT_NONE && fixture.firstStats->left == 3,
            "Provider DragOver failure did not leave target or preserve HRESULT");
    fixture.firstStats->overResult = S_OK;
    fixture.firstStats->siteResult = E_ACCESSDENIED;
    effect = DROPEFFECT_COPY;
    require(controller->DragOver(0,{10,0},&effect) == E_ACCESSDENIED && effect == DROPEFFECT_NONE,"Site attachment failure was ignored");
    fixture.firstStats->siteResult = S_OK;
    effect = DROPEFFECT_COPY;
    succeeded(controller->DragOver(0,{10,0},&effect),"Enter native target after site failure");
    fixture.firstStats->dropResult = HRESULT_FROM_WIN32(ERROR_CANCELLED);
    const auto beforeLeave = fixture.firstStats->left;
    effect = DROPEFFECT_COPY;
    require(controller->Drop(fixture.data.Get(),0,{10,0},&effect) == fixture.firstStats->dropResult && effect == DROPEFFECT_NONE &&
            fixture.firstStats->left == beforeLeave,"Failed Drop did not preserve HRESULT or used invalid extra DragLeave");
    require(fixture.dataStats->rendered == 0,"Failure tests rendered external transfer data");
}

void headlessDropIdentityAndOwnershipGuards() {
    DragFixture fixture;
    auto controller = fixture.controller(true);
    DWORD effect = DROPEFFECT_COPY;
    succeeded(controller->DragEnter(fixture.data.Get(),0,{10,0},&effect),"Headless simulated hover with isolated fake binder");
    effect = DROPEFFECT_COPY;
    require(controller->Drop(fixture.data.Get(),0,{10,0},&effect) == E_ACCESSDENIED && effect == DROPEFFECT_NONE &&
            fixture.firstStats->dropped == 0 && fixture.firstStats->left == 1,"Headless Drop reached a provider");
    require(controller->registerWindow() == E_ACCESSDENIED && !controller->registered(),"Headless controller registered an OLE window");
    auto options = fixture.options(true);
    options.bindTarget = {};
    ComPtr<BreadcrumbDropTarget> native;
    succeeded(BreadcrumbDropTarget::create(nullptr,options,&native),"Create headless default native-binding controller");
    const unsigned beforeHit = fixture.hits;
    effect = DROPEFFECT_COPY;
    require(native->DragEnter(fixture.data.Get(),0,{10,0},&effect) == E_ACCESSDENIED && effect == DROPEFFECT_NONE &&
            fixture.hits == beforeHit,"Headless native DragEnter attempted actual provider binding");
    options.headless = false;
    native.Reset();
    succeeded(BreadcrumbDropTarget::create(nullptr,options,&native),"Create unregistered normal native controller");
    effect = DROPEFFECT_COPY;
    require(native->Drop(fixture.data.Get(),0,{10,0},&effect) == E_ACCESSDENIED && effect == DROPEFFECT_NONE,
            "Unregistered or hidden native Drop was not rejected");
    require(native->registerWindow() == DRAGDROP_E_INVALIDHWND,"Null owner registered a native target");
    controller = fixture.controller();
    effect = DROPEFFECT_COPY;
    succeeded(controller->DragEnter(fixture.data.Get(),0,{10,0},&effect),"Enter identity guard fixture");
    auto otherStats = std::make_shared<DataStats>();
    auto otherData = Make<FakeData>(otherStats);
    effect = DROPEFFECT_COPY;
    require(controller->Drop(otherData.Get(),0,{10,0},&effect) == E_INVALIDARG && effect == DROPEFFECT_NONE &&
            fixture.firstStats->dropped == 0,"Different Drop data identity was accepted");
    require(controller->DragEnter(nullptr,0,{10,0},&effect) == E_INVALIDARG && effect == DROPEFFECT_NONE,"Null transfer data accepted");
    require(controller->DragEnter(fixture.data.Get(),0,{10,0},nullptr) == E_POINTER &&
            controller->DragOver(0,{10,0},nullptr) == E_POINTER && controller->Drop(fixture.data.Get(),0,{10,0},nullptr) == E_POINTER,
            "Null effect pointers accepted");
    ComPtr<BreadcrumbDropTarget> rejected;
    require(BreadcrumbDropTarget::create(nullptr,{},&rejected) == E_INVALIDARG && !rejected,"Controller without hit test accepted");
    require(BreadcrumbDropTarget::create(nullptr,fixture.options(),nullptr) == E_POINTER,"Null controller output accepted");
    HRESULT wrongThread = S_OK;
    std::thread other([&]() { DWORD denied = DROPEFFECT_COPY; wrongThread = controller->DragOver(0,{10,0},&denied); });
    other.join();
    require(wrongThread == RPC_E_WRONG_THREAD,"Drag target accepted calls from a foreign thread");
    succeeded(controller->DragLeave(),"Clear owned fake drag state");
    require(fixture.dataStats->rendered == 0 && otherStats->rendered == 0,"Headless tests rendered transfer data");
}

void fakeLifetimeAndHiddenOleRegistration() {
    Fixture files;
    auto shell = item(files.first);
    auto dataStats = std::make_shared<DataStats>();
    auto dropStats = std::make_shared<DropStats>();
    auto data = Make<FakeData>(dataStats);
    auto provider = Make<FakeTarget>(dropStats);
    explorer::BreadcrumbDropOptions options;
    options.hitTest = [shell](POINTL,IShellItem** result) { return shell.CopyTo(result); };
    options.bindTarget = [&provider](IShellItem*,IDropTarget** result) { return provider.CopyTo(result); };
    ComPtr<BreadcrumbDropTarget> controller;
    succeeded(BreadcrumbDropTarget::create(nullptr,options,&controller),"Create lifetime guard controller");
    DWORD effect = DROPEFFECT_COPY;
    succeeded(controller->DragEnter(data.Get(),0,{0,0},&effect),"Retain original data and provider during drag");
    data.Reset();
    provider.Reset();
    require(dataStats->destroyed == 0 && dropStats->destroyed == 0,"Active drag lost data or native target ownership");
    succeeded(controller->DragLeave(),"Release drag references at native DragLeave");
    require(dataStats->destroyed == 1 && dropStats->destroyed == 1 && dropStats->left == 1,
            "DragLeave leaked provider/data or failed native leave callback");
    require(controller->revokeWindow() == S_FALSE,"Unregistered controller revoke should be a harmless no-op");
    const HRESULT ole = OleInitialize(nullptr);
    succeeded(ole,"Initialize OLE for isolated hidden-window registration");
    HWND hidden = CreateWindowExW(0,L"STATIC",L"owned headless breadcrumb fixture",WS_POPUP,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    require(hidden && !IsWindowVisible(hidden),"Owned OLE fixture window was not hidden");
    options.bindTarget = [](IShellItem*,IDropTarget** result) { *result = nullptr; return E_NOINTERFACE; };
    controller.Reset();
    const HRESULT created = BreadcrumbDropTarget::create(hidden,options,&controller);
    HRESULT registered = created;
    HRESULT repeated = created;
    HRESULT revoked = created;
    bool retained = false;
    if (SUCCEEDED(created)) {
        registered = controller->registerWindow();
        retained = controller->registered();
        repeated = controller->registerWindow();
        revoked = controller->revokeWindow();
    }
    const bool cleared = controller && !controller->registered();
    const bool stillHidden = !IsWindowVisible(hidden);
    controller.Reset();
    DestroyWindow(hidden);
    OleUninitialize();
    succeeded(created,"Create hidden-owned OLE registration controller");
    succeeded(registered,"Register actual OLE target on only the app-owned hidden fixture window");
    require(retained && repeated == DRAGDROP_E_ALREADYREGISTERED,"OLE registration state or duplicate guard is incorrect");
    succeeded(revoked,"Revoke owned hidden-window OLE target");
    require(cleared && stillHidden,"OLE registration leaked or displayed a window");
    require(dataStats->rendered == 0 && dropStats->dropped == 0,"Lifetime/registration fixture performed transfer data rendering or Drop");
}

class TreeBrowserSite final : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
    IServiceProvider,IExplorerPaneVisibility,IExplorerBrowserEvents> {
public:
    bool completed = false;
    HRESULT navigation = E_PENDING;
    HRESULT STDMETHODCALLTYPE QueryService(REFGUID service,REFIID iid,void** result) override {
        if (service == SID_ExplorerPaneVisibility || service == SID_SExplorerBrowserFrame) return QueryInterface(iid,result);
        if (!result) return E_POINTER;
        *result = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetPaneState(REFEXPLORERPANE pane,EXPLORERPANESTATE* state) override {
        if (!state) return E_POINTER;
        *state = static_cast<EXPLORERPANESTATE>((pane == EP_NavPane ? EPS_DEFAULT_ON : EPS_DEFAULT_OFF) | EPS_FORCE);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnNavigationPending(PCIDLIST_ABSOLUTE) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnViewCreated(IShellView*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnNavigationComplete(PCIDLIST_ABSOLUTE) override { completed = true; navigation = S_OK; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnNavigationFailed(PCIDLIST_ABSOLUTE) override { completed = true; navigation = E_FAIL; return S_OK; }
};

bool pumpTree(const std::function<bool()>& ready,DWORD duration) {
    const auto deadline = GetTickCount64() + duration;
    do {
        MSG message{};
        while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        if (ready()) return true;
        MsgWaitForMultipleObjectsEx(0,nullptr,10,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
    } while (GetTickCount64() < deadline);
    return ready();
}

struct PrivateTreeApartment {
    HDESK previous = nullptr, desktop = nullptr;
    bool initialized = false;
    PrivateTreeApartment() {
        GUID id{}; succeeded(CoCreateGuid(&id),"Generate private native-tree desktop identity");
        wchar_t name[40]{}; require(StringFromGUID2(id,name,40) != 0,"Format private tree desktop identity");
        previous = GetThreadDesktop(GetCurrentThreadId());
        desktop = CreateDesktopW((std::wstring(L"ExplorerNativeTreeTest") + name).c_str(),nullptr,nullptr,0,GENERIC_ALL,nullptr);
        require(desktop != nullptr,"Create private desktop before native-tree COM/windows");
        if (!SetThreadDesktop(desktop)) { CloseDesktop(desktop); desktop = nullptr; require(false,"Attach private native-tree desktop"); }
        const auto hr = OleInitialize(nullptr);
        if (FAILED(hr)) { SetThreadDesktop(previous); CloseDesktop(desktop); desktop = nullptr; succeeded(hr,"Initialize private native-tree STA"); }
        initialized = true;
    }
    ~PrivateTreeApartment() {
        if (initialized) OleUninitialize();
        if (desktop && SetThreadDesktop(previous)) CloseDesktop(desktop);
    }
};

struct NativeTreeHost {
    HWND owner = nullptr;
    ComPtr<IExplorerBrowser> browser;
    ComPtr<IShellView> view;
    ComPtr<INameSpaceTreeControl2> tree;
    ComPtr<TreeBrowserSite> site;
    DWORD cookie = 0;
    ~NativeTreeHost() {
        tree.Reset();
        if (view) { ComPtr<IObjectWithSite> located; if (SUCCEEDED(view.As(&located))) located->SetSite(nullptr); view.Reset(); }
        if (browser) {
            if (cookie) browser->Unadvise(cookie);
            ComPtr<IObjectWithSite> located; if (SUCCEEDED(browser.As(&located))) located->SetSite(nullptr);
            browser->Destroy(); browser.Reset();
        }
        site.Reset(); if (owner) DestroyWindow(owner);
    }
    void initialize(IShellItem* folder) {
        owner = CreateWindowExW(0,L"STATIC",L"owned private native-tree headless fixture",WS_POPUP,
            120,180,900,600,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        require(owner && !IsWindowVisible(owner),"Create hidden native-tree fixture only on private desktop");
        succeeded(CoCreateInstance(CLSID_ExplorerBrowser,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&browser)),"Create real embedded native tree browser");
        site = Make<TreeBrowserSite>(); require(site != nullptr,"Create native tree site");
        ComPtr<IObjectWithSite> located; succeeded(browser.As(&located),"Locate native browser site");
        succeeded(located->SetSite(static_cast<IServiceProvider*>(site.Get())),"Attach native tree pane/events site");
        succeeded(browser->SetOptions(EBO_SHOWFRAMES | EBO_NOPERSISTVIEWSTATE | EBO_NOTRAVELLOG),"Forbid native tree fixture view persistence");
        RECT bounds{0,140,900,600}; FOLDERSETTINGS settings{FVM_DETAILS,0};
        succeeded(browser->Initialize(owner,&bounds,&settings),"Initialize hidden native browser tree");
        succeeded(browser->Advise(site.Get(),&cookie),"Observe actual native navigation");
        succeeded(browser->BrowseToObject(folder,SBSP_ABSOLUTE),"Browse exclusively owned native tree fixture");
        require(pumpTree([this] { return site->completed; },5000),"Native tree fixture navigation deadline");
        succeeded(site->navigation,"Complete owned native tree navigation");
        succeeded(browser->GetCurrentView(IID_PPV_ARGS(&view)),"Retain actual native Shell view");
        auto hr = IUnknown_QueryService(browser.Get(),SID_SNavigationPane,IID_PPV_ARGS(&tree));
        if (FAILED(hr)) hr = IUnknown_QueryService(view.Get(),SID_SNavigationPane,IID_PPV_ARGS(&tree));
        if (FAILED(hr)) {
            ComPtr<IObjectWithSite> locatedView; ComPtr<IServiceProvider> provider;
            succeeded(view.As(&locatedView),"Locate native view's actual frame");
            succeeded(locatedView->GetSite(IID_PPV_ARGS(&provider)),"Read native frame service provider");
            hr = provider->QueryService(SID_SNavigationPane,IID_PPV_ARGS(&tree));
        }
        succeeded(hr,"Read original embedded native navigation tree");
        succeeded(tree->SetControlStyle2(NSTCS2_DISPLAYPINNEDONLY,NSTCS2_DEFAULT),"Set only app-owned native show-all style");
    }
};

void nativeLargeTreeExpansionAndOwnership() {
    // Keep the exercised ancestor chain ordinary and visible; a system Temp
    // path can add a hidden AppData ancestor filtered by the native provider.
    Fixture files(fs::current_path());
    for (unsigned index = 0; index < 101; ++index)
        require(fs::create_directory(files.first / (L"owned sibling " + std::to_wstring(index))),"Create owned large native tree fixture");
    std::exception_ptr failure;
    std::atomic<bool> completed = false;
    std::thread worker([&] {
        try {
            PrivateTreeApartment apartment;
            ComPtr<IShellItem> computer; ComPtr<IShellFolder> computerFolder;
            succeeded(SHGetKnownFolderItem(FOLDERID_ComputerFolder,KF_FLAG_DEFAULT,nullptr,IID_PPV_ARGS(&computer)),"Read native Computer namespace root");
            succeeded(computer->BindToHandler(nullptr,BHID_SFObject,IID_PPV_ARGS(&computerFolder)),"Bind real Computer filesystem ancestry");
            auto parsing = files.nested.wstring(); PITEMID_CHILD relative = nullptr; PIDLIST_ABSOLUTE computerId = nullptr;
            auto hr = computerFolder->ParseDisplayName(nullptr,nullptr,parsing.data(),nullptr,&relative,nullptr);
            if (SUCCEEDED(hr)) hr = SHGetIDListFromObject(computer.Get(),&computerId);
            PIDLIST_ABSOLUTE absolute = relative && computerId ? ILCombine(computerId,relative) : nullptr;
            ComPtr<IShellItem> target;
            if (SUCCEEDED(hr)) hr = absolute ? SHCreateItemFromIDList(absolute,IID_PPV_ARGS(&target)) : E_UNEXPECTED;
            CoTaskMemFree(relative); CoTaskMemFree(computerId);
            std::vector<ComPtr<IShellItem>> chain;
            if (SUCCEEDED(hr)) for (unsigned depth = 0; absolute && depth < 64; ++depth) {
                ComPtr<IShellItem> ancestor; hr = SHCreateItemFromIDList(absolute,IID_PPV_ARGS(&ancestor));
                if (FAILED(hr)) break;
                chain.push_back(ancestor);
                if (ILIsEmpty(absolute) || !ILRemoveLastID(absolute)) break;
            }
            CoTaskMemFree(absolute); succeeded(hr,"Retain exact native Computer/drive ancestor PIDLs");
            std::reverse(chain.begin(),chain.end()); require(chain.size() > 2,"Deep native tree fixture has real ancestors");
            NativeTreeHost host; host.initialize(target.Get());
            const auto focus = GetFocus();
            for (size_t index = 0; index + 1 < chain.size(); ++index) {
                NSTCITEMSTATE initial = NSTCIS_NONE;
                if (FAILED(host.tree->GetItemState(chain[index].Get(),NSTCIS_EXPANDED,&initial)))
                    succeeded(host.tree->SetItemState(chain[index].Get(),NSTCIS_EXPANDED,NSTCIS_EXPANDED),"Materialize original native ancestor state");
                const auto expanded = explorer::expandNativeTreeItem(host.tree.Get(),chain[index].Get(),host.owner);
                require(expanded == S_OK || expanded == E_PENDING,"Verified native expansion dispatch failed");
                require(pumpTree([&] {
                    NSTCITEMSTATE state = NSTCIS_NONE,child = NSTCIS_NONE;
                    if (FAILED(host.tree->GetItemState(chain[index].Get(),NSTCIS_EXPANDED,&state)) || !(state & NSTCIS_EXPANDED)) return false;
                    if (SUCCEEDED(host.tree->GetItemState(chain[index + 1].Get(),NSTCIS_EXPANDED,&child))) return true;
                    ComPtr<IShellItem> nativeChild;
                    if (FAILED(host.tree->GetNextItem(chain[index].Get(),NSTCGNI_CHILD,&nativeChild))) return false;
                    for (unsigned visited = 0; nativeChild && visited < 4096; ++visited) {
                        int order = 1;
                        if (SUCCEEDED(nativeChild->Compare(chain[index + 1].Get(),SICHINT_CANONICAL,&order)) && !order) {
                            chain[index + 1] = nativeChild;
                            return SUCCEEDED(host.tree->GetItemState(nativeChild.Get(),NSTCIS_EXPANDED,&child));
                        }
                        ComPtr<IShellItem> next;
                        if (FAILED(host.tree->GetNextItem(nativeChild.Get(),NSTCGNI_NEXT,&next))) break;
                        nativeChild = next;
                    }
                    return false;
                },5000),("Actual native expansion did not materialize its exact next child at ancestor " + std::to_string(index)).c_str());
            }
            auto large = chain[chain.size() - 2];
            ComPtr<IShellItem> child; succeeded(host.tree->GetNextItem(large.Get(),NSTCGNI_CHILD,&child),"Read native large folder's first child");
            unsigned count = 0; bool selectedChild = false;
            while (child && count < 4096) {
                ++count; if (same(child.Get(),target.Get())) selectedChild = true;
                ComPtr<IShellItem> next;
                if (FAILED(host.tree->GetNextItem(child.Get(),NSTCGNI_NEXT,&next))) break;
                child = next;
            }
            require(count == 102 && selectedChild,"Original native tree omitted owned children or exact Unicode destination");
            succeeded(host.tree->EnsureItemVisible(target.Get()),"Scroll actual selected native fixture child into view");
            RECT bounds{}; succeeded(host.tree->GetItemRect(target.Get(),&bounds),"Read actual materialized native child bounds");
            require(bounds.right > bounds.left && bounds.bottom > bounds.top,"Native tree returned empty selected item bounds");
            require(GetFocus() == focus,"Native tree expansion changed keyboard focus");
            require(explorer::expandNativeTreeItem(nullptr,target.Get(),host.owner) == E_POINTER,"Reject missing native tree");
            require(explorer::expandNativeTreeItem(host.tree.Get(),nullptr,host.owner) == E_POINTER,"Reject missing native item");
            require(explorer::expandNativeTreeItem(host.tree.Get(),large.Get(),nullptr) == HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE),"Reject missing tree owner");
            HWND unrelated = CreateWindowExW(0,L"STATIC",L"unrelated hidden owner",WS_POPUP,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
            require(unrelated != nullptr,"Create unrelated owned guard fixture");
            const auto wrongOwner = explorer::expandNativeTreeItem(host.tree.Get(),large.Get(),unrelated);
            DestroyWindow(unrelated); require(wrongOwner == E_ACCESSDENIED,"Reject native tree outside claimed owner's ancestry");
            HRESULT wrongThread = S_OK;
            std::thread foreign([&] { wrongThread = explorer::expandNativeTreeItem(host.tree.Get(),large.Get(),host.owner); });
            foreign.join(); require(wrongThread == RPC_E_WRONG_THREAD,"Reject foreign thread before calling any native interface");
            auto text = item(files.text);
            require(FAILED(explorer::expandNativeTreeItem(host.tree.Get(),text.Get(),host.owner)),"Do not expand a native nonfolder");
            require(!IsWindowVisible(host.owner),"Native expansion displayed its private hidden owner");
        } catch (...) { failure = std::current_exception(); }
        completed.store(true);
    });
    // The process's existing main STA must dispatch COM calls while a separate
    // private STA uses shared Shell services. A blocking join starves those
    // services during the native large-folder enumeration path.
    const bool finished = pumpTree([&] { return completed.load(); },60000);
    worker.join(); if (failure) std::rethrow_exception(failure);
    require(finished,"Private native-tree fixture completion exceeded its bounded wait");
    require(fs::exists(files.text) && fs::exists(files.nested),"Native tree UI changed owned filesystem contents");
}

} // namespace

int runBreadcrumbTests() {
    const std::array<std::pair<const char*,std::function<void()>>,8> cases{{
        {"breadcrumb_native_hidden_unicode_selection",foldersHiddenUnicodeAndSelection},
        {"breadcrumb_bounded_virtual_namespaces",boundedFailureAndVirtualNamespaces},
        {"breadcrumb_async_snapshot_cancel",asynchronousSnapshotAndCancellation},
        {"breadcrumb_drag_lifecycle_modifiers_identity",dragLifecycleModifiersAndCanonicalIdentity},
        {"breadcrumb_drag_effect_bounds_failures",dragBoundsAndProviderFailures},
        {"breadcrumb_headless_drop_guards",headlessDropIdentityAndOwnershipGuards},
        {"breadcrumb_lifetimes_hidden_ole_registration",fakeLifetimeAndHiddenOleRegistration},
        {"breadcrumb_native_large_tree_expand_ownership",nativeLargeTreeExpansionAndOwnership}
    }};
    int failures = 0;
    for (const auto& [name,test] : cases) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    try{succeeded(explorer::drainStaWorkers(10000),"Drain breadcrumb workers while creator COM remains initialized");}
    catch(const std::exception& error){++failures;std::cerr<<"FAIL breadcrumb final STA drain: "<<error.what()<<'\n';}
    std::cout << "Breadcrumb headless groups: " << cases.size() << ", assertions: " << assertions << ", failures: " << failures << '\n';
    return failures;
}
