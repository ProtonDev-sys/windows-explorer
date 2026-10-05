#include "explorer/shell_operations.hpp"
#include "explorer/headless_visual.hpp"

#include <shlobj.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <sherrors.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <functional>
#include <cstring>
#include <vector>
#include <thread>

namespace {
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
using explorer::ShellOperations;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void succeeded(HRESULT hr, const char* message) {
    if (FAILED(hr)) {
        std::cerr << message << " (HRESULT 0x" << std::hex
                  << static_cast<unsigned long>(hr) << std::dec << ")\n";
        throw std::runtime_error(message);
    }
}

ComPtr<IShellItem> item(const fs::path& path) {
    ComPtr<IShellItem> result;
    succeeded(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&result)),
              "create shell item");
    return result;
}

ComPtr<IShellItemArray> selection(const fs::path& path) {
    const auto source = item(path);
    ComPtr<IShellItemArray> result;
    succeeded(SHCreateShellItemArrayFromShellItem(source.Get(), IID_PPV_ARGS(&result)),
              "create shell selection");
    return result;
}

void write(const fs::path& path, const char* contents) {
    std::ofstream stream(path, std::ios::binary);
    stream << contents;
    require(stream.good(), "write fixture");
}

std::string read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    require(stream.good(), "read fixture");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

struct Fixture {
    fs::path root;
    Fixture() {
        GUID guid{};
        succeeded(CoCreateGuid(&guid), "create fixture id");
        wchar_t identifier[40]{};
        require(StringFromGUID2(guid, identifier, 40) > 0, "format fixture id");
        root = fs::temp_directory_path() / (std::wstring(L"windows-explorer-test-") + identifier);
        require(fs::create_directories(root / L"source"), "create fixture source");
        require(fs::create_directories(root / L"destination"), "create fixture destination");
    }
    ~Fixture() {
        std::error_code error;
        fs::remove_all(root, error);
    }
};

FILE_ID_INFO identity(const fs::path& path) {
    const HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "open fixture identity");
    FILE_ID_INFO id{};
    const BOOL ok = GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id));
    CloseHandle(handle);
    require(ok != FALSE, "read fixture identity");
    return id;
}
bool sameIdentity(const FILE_ID_INFO& a, const FILE_ID_INFO& b) {
    return a.VolumeSerialNumber == b.VolumeSerialNumber &&
        std::memcmp(&a.FileId, &b.FileId, sizeof(a.FileId)) == 0;
}
ComPtr<IShellItemArray> selection(const std::vector<fs::path>& paths) {
    std::vector<PIDLIST_ABSOLUTE> pidls;
    for (const auto& path : paths) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        succeeded(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr), "parse batch fixture");
        pidls.push_back(pidl);
    }
    std::vector<PCIDLIST_ABSOLUTE> values(pidls.begin(), pidls.end());
    ComPtr<IShellItemArray> result;
    const HRESULT hr = SHCreateShellItemArrayFromIDLists(static_cast<UINT>(pidls.size()),
        values.data(), &result);
    for (auto pidl : pidls) CoTaskMemFree(pidl);
    succeeded(hr, "create batch selection");
    return result;
}
enum class CancelPoint { None, BeforeCopy, AfterCopy, BeforeMove, BeforeRename, BeforeNew, BeforeDelete };

class ProgressProbe final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IFileOperationProgressSink> {
public:
    struct Output { fs::path source, created; FILE_ID_INFO id{}; };
    explicit ProgressProbe(CancelPoint point = CancelPoint::None, HRESULT cancellation = COPYENGINE_E_USER_CANCELLED)
        : point_(point), cancellation_(cancellation), thread_(GetCurrentThreadId()) {}
    unsigned starts = 0, finishes = 0, progressCalls = 0, preCalls = 0, postCalls = 0;
    HRESULT finishResult = E_PENDING;
    bool wrongThread = false, captureFailed = false;
    std::vector<Output> outputs;
    IFACEMETHODIMP StartOperations() override { checkThread(); ++starts; return S_OK; }
    IFACEMETHODIMP FinishOperations(HRESULT result) override { checkThread(); ++finishes; finishResult = result; return S_OK; }
    IFACEMETHODIMP PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return before(CancelPoint::BeforeRename); }
    IFACEMETHODIMP PostRenameItem(DWORD, IShellItem* source, LPCWSTR, HRESULT result, IShellItem* created) override {
        return after(source, created, result, CancelPoint::None);
    }
    IFACEMETHODIMP PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return before(CancelPoint::BeforeMove); }
    IFACEMETHODIMP PostMoveItem(DWORD, IShellItem* source, IShellItem*, LPCWSTR, HRESULT result, IShellItem* created) override {
        return after(source, created, result, CancelPoint::None);
    }
    IFACEMETHODIMP PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return before(CancelPoint::BeforeCopy); }
    IFACEMETHODIMP PostCopyItem(DWORD, IShellItem* source, IShellItem*, LPCWSTR, HRESULT result, IShellItem* created) override {
        return after(source, created, result, CancelPoint::AfterCopy);
    }
    IFACEMETHODIMP PreDeleteItem(DWORD, IShellItem*) override { return before(CancelPoint::BeforeDelete); }
    IFACEMETHODIMP PostDeleteItem(DWORD, IShellItem* source, HRESULT result, IShellItem* created) override {
        return after(source, created, result, CancelPoint::None);
    }
    IFACEMETHODIMP PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return before(CancelPoint::BeforeNew); }
    IFACEMETHODIMP PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT result, IShellItem* created) override {
        return after(nullptr, created, result, CancelPoint::None);
    }
    IFACEMETHODIMP UpdateProgress(UINT, UINT) override { checkThread(); ++progressCalls; return S_OK; }
    IFACEMETHODIMP ResetTimer() override { checkThread(); return S_OK; }
    IFACEMETHODIMP PauseTimer() override { checkThread(); return S_OK; }
    IFACEMETHODIMP ResumeTimer() override { checkThread(); return S_OK; }
private:
    void checkThread() noexcept { wrongThread |= GetCurrentThreadId() != thread_; }
    HRESULT before(CancelPoint point) noexcept {
        checkThread(); ++preCalls;
        return point_ == point ? cancellation_ : S_OK;
    }
    static fs::path path(IShellItem* shellItem) {
        if (!shellItem) return {};
        PWSTR raw = nullptr;
        const HRESULT result = shellItem->GetDisplayName(SIGDN_FILESYSPATH, &raw);
        fs::path output;
        try { if (SUCCEEDED(result) && raw) output = raw; }
        catch (...) { CoTaskMemFree(raw); throw; }
        CoTaskMemFree(raw);
        return output;
    }
    HRESULT after(IShellItem* source, IShellItem* created, HRESULT result, CancelPoint point) noexcept {
        checkThread(); ++postCalls;
        if (SUCCEEDED(result) && created) {
            try {
                Output output{path(source), path(created), {}};
                output.id = identity(output.created);
                outputs.push_back(std::move(output));
            } catch (...) { captureFailed = true; return E_FAIL; }
        }
        return point_ != CancelPoint::None && point_ == point && SUCCEEDED(result) ? cancellation_ : S_OK;
    }
    CancelPoint point_;
    HRESULT cancellation_;
    DWORD thread_;
};

void requireProbe(const ProgressProbe* probe) {
    require(probe->starts == 1 && probe->finishes == 1, "native operation brackets progress callbacks exactly once");
    require(!probe->wrongThread && !probe->captureFailed, "progress callbacks retain the initializing STA and capture valid targets");
}

void operationProgressAndTargets() {
    Fixture fixture;
    const auto source = fixture.root / L"source" / L"native-\u03bb.txt";
    const auto destination = fixture.root / L"destination";
    write(source, "copy content");
    write(destination / source.filename(), "existing collision");
    const auto original = identity(source);
    const auto collision = identity(destination / source.filename());
    auto target = item(destination);
    auto selected = selection(source);
    auto copied = Microsoft::WRL::Make<ProgressProbe>();
    succeeded(ShellOperations::copyOrMove(nullptr, selected.Get(), target.Get(), false, true, copied.Get()),
              "native copy with caller progress sink");
    requireProbe(copied.Get());
    require(copied->outputs.size() == 1 && fs::equivalent(copied->outputs[0].created.parent_path(), destination) &&
        copied->outputs[0].created.filename() != source.filename() &&
        _wcsicmp(copied->outputs[0].created.extension().c_str(), L".txt") == 0,
        "native post-copy sink supplies actual extension-preserving collision target");
    require(read(copied->outputs[0].created) == "copy content" && sameIdentity(copied->outputs[0].id, identity(copied->outputs[0].created)),
        "post-copy target retains actual output contents and identity");
    require(read(destination / source.filename()) == "existing collision" &&
        sameIdentity(collision, identity(destination / source.filename())) && sameIdentity(original, identity(source)),
        "native copy preserves existing collision and original identity");

    const auto moving = fixture.root / L"source" / L"moving.txt";
    write(moving, "move content");
    const auto movedId = identity(moving);
    selected = selection(moving);
    auto moved = Microsoft::WRL::Make<ProgressProbe>();
    succeeded(ShellOperations::copyOrMove(nullptr, selected.Get(), target.Get(), true, true, moved.Get()), "native move progress");
    requireProbe(moved.Get());
    require(moved->outputs.size() == 1 && fs::equivalent(moved->outputs[0].created, destination / moving.filename()) &&
        sameIdentity(movedId, moved->outputs[0].id) && !fs::exists(moving), "native move reports original filesystem identity at actual target");

    auto movingItem = item(destination / moving.filename());
    auto renamed = Microsoft::WRL::Make<ProgressProbe>();
    succeeded(ShellOperations::rename(nullptr, movingItem.Get(), L"renamed-\u03bb.txt", true, renamed.Get()), "native rename progress");
    requireProbe(renamed.Get());
    require(renamed->outputs.size() == 1 && fs::equivalent(renamed->outputs[0].created, destination / L"renamed-\u03bb.txt") &&
        sameIdentity(movedId, renamed->outputs[0].id), "native rename reports exact Unicode target and original identity");

    auto folder = Microsoft::WRL::Make<ProgressProbe>();
    succeeded(ShellOperations::newFolder(nullptr, target.Get(), L"created-\u03bb", true, folder.Get()), "native new-folder progress");
    requireProbe(folder.Get());
    require(folder->outputs.size() == 1 && fs::equivalent(folder->outputs[0].created, destination / L"created-\u03bb") &&
        fs::is_directory(folder->outputs[0].created), "native new-folder sink reports actual folder identity");

    selected = selection(renamed->outputs[0].created);
    auto deleted = Microsoft::WRL::Make<ProgressProbe>();
    succeeded(ShellOperations::remove(nullptr, selected.Get(), true, true, deleted.Get()), "owned permanent-delete progress");
    requireProbe(deleted.Get());
    require(deleted->preCalls == 1 && deleted->postCalls == 1 && deleted->outputs.empty() &&
        !fs::exists(renamed->outputs[0].created), "owned permanent deletion reports completion without a recycle target");
    require(read(source) == "copy content" && read(destination / source.filename()) == "existing collision", "completion preserves unrelated files");
    std::cout << "Native file-operation progress callbacks: " << copied->progressCalls + moved->progressCalls +
        renamed->progressCalls + folder->progressCalls + deleted->progressCalls << " updates; five native operation completions checked\n";
}

void operationCancellation() {
    require(explorer::isShellOperationCancelled(COPYENGINE_E_USER_CANCELLED) &&
        explorer::isShellOperationCancelled(E_ABORT) && explorer::isShellOperationCancelled(HRESULT_FROM_WIN32(ERROR_CANCELLED)),
        "native user cancellation is classified without an error prompt");
    require(!explorer::isShellOperationCancelled(COPYENGINE_E_CANCELLED) && !explorer::isShellOperationCancelled(E_ACCESSDENIED) &&
        !explorer::isShellOperationCancelled(S_OK) && !explorer::isShellOperationCancelled(COPYENGINE_S_USER_IGNORED),
        "engine failures, success and ignored items are not misclassified as user cancellation");
    Fixture fixture;
    const auto source = fixture.root / L"source";
    const auto destination = fixture.root / L"destination";
    const auto first = source / L"first.txt", second = source / L"second.txt";
    write(first, "first content"); write(second, "second content");
    const auto firstId = identity(first), secondId = identity(second);
    auto selected = selection(std::vector<fs::path>{first, second});
    auto target = item(destination);
    for (const auto point : {CancelPoint::BeforeCopy, CancelPoint::BeforeMove}) {
        auto cancelled = Microsoft::WRL::Make<ProgressProbe>(point);
        const HRESULT result = ShellOperations::copyOrMove(nullptr, selected.Get(), target.Get(),
            point == CancelPoint::BeforeMove, true, cancelled.Get());
        requireProbe(cancelled.Get());
        require(explorer::isShellOperationCancelled(result), "native pre-transfer cancellation retains a user cancellation result");
        require(fs::is_empty(destination) && read(first) == "first content" && read(second) == "second content" &&
            sameIdentity(firstId, identity(first)) && sameIdentity(secondId, identity(second)), "pre-transfer cancellation preserves every original and creates no output");
    }
    auto failed = Microsoft::WRL::Make<ProgressProbe>(CancelPoint::BeforeCopy, E_ACCESSDENIED);
    const auto failure = ShellOperations::copyOrMove(nullptr, selected.Get(), target.Get(), false, true, failed.Get());
    requireProbe(failed.Get());
    require(FAILED(failure) && !explorer::isShellOperationCancelled(failure) && fs::is_empty(destination),
        "native callback failure remains an operation error with no output");

    auto partial = Microsoft::WRL::Make<ProgressProbe>(CancelPoint::AfterCopy);
    const auto result = ShellOperations::copyOrMove(nullptr, selected.Get(), target.Get(), false, true, partial.Get());
    requireProbe(partial.Get());
    require(explorer::isShellOperationCancelled(result) && partial->outputs.size() == 1 &&
        std::distance(fs::directory_iterator(destination), fs::directory_iterator{}) == 1,
        "post-copy cancellation stops the remaining batch after exactly one native copy");
    require(read(partial->outputs[0].source) == read(partial->outputs[0].created) &&
        sameIdentity(partial->outputs[0].id, identity(partial->outputs[0].created)) &&
        sameIdentity(firstId, identity(first)) && sameIdentity(secondId, identity(second)), "partial cancellation preserves the actual completed copy and both originals");
}

void operationCancelledMutations() {
    Fixture fixture;
    const auto source = fixture.root / L"source" / L"untouched.txt";
    const auto destination = fixture.root / L"destination";
    write(source, "cancel preserves me");
    const auto original = identity(source);
    auto current = item(source);
    auto selected = selection(source);
    auto target = item(destination);
    for (const auto point : {CancelPoint::BeforeRename, CancelPoint::BeforeNew, CancelPoint::BeforeDelete}) {
        auto cancelled = Microsoft::WRL::Make<ProgressProbe>(point);
        const auto result = point == CancelPoint::BeforeRename
            ? ShellOperations::rename(nullptr, current.Get(), L"cancelled.txt", true, cancelled.Get())
            : point == CancelPoint::BeforeNew
            ? ShellOperations::newFolder(nullptr, target.Get(), L"cancelled folder", true, cancelled.Get())
            : ShellOperations::remove(nullptr, selected.Get(), true, true, cancelled.Get());
        requireProbe(cancelled.Get());
        require(explorer::isShellOperationCancelled(result) && fs::is_empty(destination) &&
            !fs::exists(source.parent_path() / L"cancelled.txt") && read(source) == "cancel preserves me" &&
            sameIdentity(original, identity(source)), "native cancellation precedes rename, folder creation and owned permanent deletion");
    }
}

class VerbMenu final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IContextMenu, IObjectWithSite> {
public:
    bool disabled = false, duplicate = false, staleDisabled = false;
    bool queriedWithSite = false, invokedWithSite = false, detachedAfterDestroy = false;
    UINT queryFlags = 0, invokedOrdinal = UINT_MAX;
    unsigned queries = 0, invokes = 0, attaches = 0, detaches = 0;
    HWND invokedOwner = nullptr;
    HRESULT queryResult = S_OK, invokeResult = S_OK;
    IUnknown* expectedSite = nullptr;
    IFACEMETHODIMP QueryContextMenu(HMENU menu, UINT, UINT first, UINT, UINT flags) override {
        ++queries; queryFlags = flags; menu_ = menu; first_ = first;
        queriedWithSite = site_.Get() == expectedSite;
        if (FAILED(queryResult)) return queryResult;
        if (!AppendMenuW(menu, MF_STRING, first, L"Unrelated provider action") ||
            !AppendMenuW(menu, MF_STRING | (disabled ? MF_DISABLED : MF_DEFAULT), first + 2, L"Ouvrir \u6587\u4EF6")) return E_FAIL;
        if (duplicate && !AppendMenuW(menu, MF_STRING, first + 3, L"Duplicate canonical leaf")) return E_FAIL;
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 4);
    }
    IFACEMETHODIMP InvokeCommand(CMINVOKECOMMANDINFO* basic) override {
        if (!basic || basic->cbSize != sizeof(CMINVOKECOMMANDINFOEX)) return E_INVALIDARG;
        const auto* command = reinterpret_cast<const CMINVOKECOMMANDINFOEX*>(basic);
        if (HIWORD(reinterpret_cast<UINT_PTR>(command->lpVerb)) ||
            HIWORD(reinterpret_cast<UINT_PTR>(command->lpVerbW)) ||
            !(command->fMask & CMIC_MASK_UNICODE) || command->nShow != SW_SHOWNORMAL) return E_INVALIDARG;
        ++invokes; invokedOrdinal = LOWORD(reinterpret_cast<UINT_PTR>(command->lpVerb));
        invokedOwner = command->hwnd;
        invokedWithSite = site_.Get() == expectedSite;
        return invokeResult;
    }
    IFACEMETHODIMP GetCommandString(UINT_PTR ordinal, UINT flags, UINT*, LPSTR text, UINT capacity) override {
        if ((ordinal != 2 && (!duplicate || ordinal != 3)) || flags != GCS_VERBW) return E_NOTIMPL;
        if (staleDisabled) EnableMenuItem(menu_, first_ + 2, MF_BYCOMMAND | MF_DISABLED);
        return wcscpy_s(reinterpret_cast<wchar_t*>(text), capacity, L"open") ? E_FAIL : S_OK;
    }
    IFACEMETHODIMP SetSite(IUnknown* site) override {
        if (site) ++attaches;
        else { ++detaches; detachedAfterDestroy = !IsMenu(menu_); }
        site_ = site;
        return S_OK;
    }
    IFACEMETHODIMP GetSite(REFIID interfaceId, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        return site_ ? site_->QueryInterface(interfaceId, output) : E_FAIL;
    }
private:
    ComPtr<IUnknown> site_;
    HMENU menu_ = nullptr;
    UINT first_ = 0;
};

class VerbSelection final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IShellItemArray> {
public:
    explicit VerbSelection(IContextMenu* menu) : menu_(menu) {}
    IFACEMETHODIMP BindToHandler(IBindCtx*, REFGUID handler, REFIID interfaceId, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        return handler == BHID_SFUIObject ? menu_->QueryInterface(interfaceId, output) : E_NOTIMPL;
    }
    IFACEMETHODIMP GetPropertyStore(GETPROPERTYSTOREFLAGS, REFIID, void** output) override { return unavailable(output); }
    IFACEMETHODIMP GetPropertyDescriptionList(REFPROPERTYKEY, REFIID, void** output) override { return unavailable(output); }
    IFACEMETHODIMP GetAttributes(SIATTRIBFLAGS, SFGAOF, SFGAOF* output) override {
        if (!output) return E_POINTER;
        *output = 0; return S_OK;
    }
    IFACEMETHODIMP GetCount(DWORD* output) override { if (!output) return E_POINTER; *output = 1; return S_OK; }
    IFACEMETHODIMP GetItemAt(DWORD, IShellItem** output) override {
        if (!output) return E_POINTER;
        *output = nullptr; return E_NOTIMPL;
    }
    IFACEMETHODIMP EnumItems(IEnumShellItems** output) override {
        if (!output) return E_POINTER;
        *output = nullptr; return E_NOTIMPL;
    }
private:
    static HRESULT unavailable(void** output) noexcept { if (!output) return E_POINTER; *output = nullptr; return E_NOTIMPL; }
    ComPtr<IContextMenu> menu_;
};

void operationVerbSiteAndOrdinal() {
    // Every handler is an owned mock. No Shell menu, association or application
    // is invoked: only its public COM container/site/ordinal contract is tested.
    auto site = Microsoft::WRL::Make<VerbMenu>();
    const auto owner = CreateWindowExW(0, L"STATIC", L"Owned verb owner", WS_POPUP, 0, 0, 32, 32,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(owner != nullptr, "create private hidden verb owner");
    struct OwnerGuard { HWND window; ~OwnerGuard() { DestroyWindow(window); } } guard{owner};
    for (unsigned scenario = 0; scenario != 7; ++scenario) {
        auto menu = Microsoft::WRL::Make<VerbMenu>();
        menu->expectedSite = static_cast<IContextMenu*>(site.Get());
        menu->disabled = scenario == 1;
        menu->duplicate = scenario == 2;
        menu->staleDisabled = scenario == 3;
        if (scenario == 4) menu->queryResult = E_ACCESSDENIED;
        if (scenario == 5) menu->invokeResult = COPYENGINE_E_USER_CANCELLED;
        auto selected = Microsoft::WRL::Make<VerbSelection>(menu.Get());
        const auto result = ShellOperations::invoke(owner, selected.Get(), scenario == 6 ? L"missing" : L"OpEn",
            static_cast<IContextMenu*>(site.Get()));
        require(menu->queries == 1 && menu->queriedWithSite && menu->attaches == 1 && menu->detaches == 1 &&
            menu->detachedAfterDestroy, "exact COM site remains attached through native query/invoke and is detached after menu destruction");
        require((menu->queryFlags & (CMF_ITEMMENU | CMF_EXTENDEDVERBS | CMF_SYNCCASCADEMENU)) ==
            (CMF_ITEMMENU | CMF_EXTENDEDVERBS | CMF_SYNCCASCADEMENU), "sited helper preserves native selection/extended/delayed-menu querying");
        if (scenario == 0 || scenario == 5) {
            require(result == menu->invokeResult && menu->invokes == 1 && menu->invokedOrdinal == 2 &&
                menu->invokedOwner == owner && menu->invokedWithSite,
                "localized default action uses exact enabled native ordinal, owner/site and unchanged provider HRESULT");
        } else {
            require(menu->invokes == 0 && FAILED(result), "missing, disabled, stale and ambiguous canonical verbs never invoke a provider");
            if (scenario == 1 || scenario == 4) require(result == E_ACCESSDENIED, "disabled/query failure retains exact HRESULT");
            if (scenario == 2) require(result == HRESULT_FROM_WIN32(ERROR_DUP_NAME), "duplicate canonical verb is explicitly ambiguous");
            if (scenario == 3) require(result == E_INVALIDARG, "native menu ordinal is revalidated after enumeration");
            if (scenario == 6) require(result == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), "missing canonical verb reports unsupported association");
        }
    }
}

int operationProgressTests() {
    int failures = 0;
    std::thread worker([&] {
        explorer::PrivateDesktop desktop;
        const auto isolated = desktop.initialize();
        if (FAILED(isolated)) { ++failures; std::cerr << "FAIL: File-operation private desktop initialization\n"; return; }
        const auto initialized = OleInitialize(nullptr);
        if (FAILED(initialized)) { ++failures; std::cerr << "FAIL: File-operation private STA initialization\n"; return; }
        const auto clipboard = GetClipboardSequenceNumber();
        const std::vector<std::pair<const char*, std::function<void()>>> tests{
            {"actual native progress and collision/completion targets", operationProgressAndTargets},
            {"native whole/partial transfer cancellation and engine failure", operationCancellation},
            {"native pre-rename/new-folder/delete cancellation", operationCancelledMutations},
            {"sited canonical native verb and retained enabled ordinal", operationVerbSiteAndOrdinal}
        };
        for (const auto& [name, test] : tests) {
            try { test(); std::cout << "PASS: " << name << '\n'; }
            catch (const std::exception& error) { ++failures; std::cerr << "FAIL: " << name << ": " << error.what() << '\n'; }
            catch (...) { ++failures; std::cerr << "FAIL: " << name << ": unknown exception\n"; }
        }
        bool visible = false;
        if (FAILED(desktop.verifyIsolation()) || FAILED(desktop.visibleWindowsOnInputDesktop(visible)) || visible ||
            clipboard != GetClipboardSequenceNumber()) {
            ++failures; std::cerr << "FAIL: Native file-operation fixtures changed desktop or clipboard\n";
        }
        OleUninitialize();
    });
    worker.join();
    return failures;
}
} // namespace

int runShellOperationTests() {
    // Every fixture mutation is silent: no Shell undo records. Clipboard tests
    // stop at argument validation, so the user's clipboard is never replaced.
    const HRESULT initialized = OleInitialize(nullptr);
    if (FAILED(initialized)) {
        std::cerr << "Shell tests require an STA/OLE apartment\n";
        return 1;
    }
    int failures = 0;
    try {
        require(ShellOperations::copyOrMove(nullptr, nullptr, nullptr, false, true) == E_INVALIDARG,
                "copy rejects null selection");
        require(ShellOperations::remove(nullptr, nullptr, true, true) == E_INVALIDARG,
                "delete rejects null selection");
        require(ShellOperations::copyToClipboard(nullptr, nullptr, false) == E_INVALIDARG,
                "clipboard rejects null selection");
        require(ShellOperations::copyPaths(nullptr, nullptr) == E_INVALIDARG,
                "paths reject null selection");

        Fixture fixture;
        const fs::path source = fixture.root / L"source";
        const fs::path destination = fixture.root / L"destination";
        auto target = item(destination);
        write(source / L"copy.txt", "original contents");
        auto selected = selection(source / L"copy.txt");
        succeeded(ShellOperations::copyOrMove(nullptr, selected.Get(), target.Get(), false, true),
                  "silent copy");
        require(read(source / L"copy.txt") == "original contents", "copy preserves source");
        require(read(destination / L"copy.txt") == "original contents", "copy preserves contents");

        write(destination / L"copy.txt", "existing destination");
        succeeded(ShellOperations::copyOrMove(nullptr, selected.Get(), target.Get(), false, true),
                  "silent copy collision");
        require(read(destination / L"copy.txt") == "existing destination",
                "collision preserves existing destination");
        bool renamedCopy = false;
        for (const auto& entry : fs::directory_iterator(destination)) {
            if (entry.path().filename() != L"copy.txt" && entry.is_regular_file() &&
                read(entry.path()) == "original contents") renamedCopy = true;
        }
        require(renamedCopy, "collision creates a separate copy");

        write(source / L"move.txt", "moved contents");
        selected = selection(source / L"move.txt");
        succeeded(ShellOperations::copyOrMove(nullptr, selected.Get(), target.Get(), true, true),
                  "silent move");
        require(!fs::exists(source / L"move.txt"), "move removes source");
        require(read(destination / L"move.txt") == "moved contents", "move preserves contents");

        auto moved = item(destination / L"move.txt");
        require(ShellOperations::rename(nullptr, moved.Get(), L"..", true) == E_INVALIDARG,
                "rename rejects parent traversal");
        require(ShellOperations::rename(nullptr, moved.Get(), L"CON.txt", true) == E_INVALIDARG,
                "rename rejects reserved Windows devices");
        require(FAILED(ShellOperations::rename(nullptr, moved.Get(), L"copy.txt", true)),
                "rename rejects collision");
        require(read(destination / L"copy.txt") == "existing destination", "rename preserves collision");
        succeeded(ShellOperations::rename(nullptr, moved.Get(), L"renamed-\u03bb.txt", true), "silent Unicode rename");
        require(!fs::exists(destination / L"move.txt"), "rename removes old name");
        require(read(destination / L"renamed-\u03bb.txt") == "moved contents", "Unicode rename preserves contents");

        succeeded(ShellOperations::newFolder(nullptr, target.Get(), L"New folder \u03bb", true),
                  "silent Unicode folder creation");
        require(fs::is_directory(destination / L"New folder \u03bb"), "folder created");
        require(FAILED(ShellOperations::newFolder(nullptr, target.Get(), L"New folder \u03bb", true)),
                "new folder rejects collision");
        require(ShellOperations::newFolder(nullptr, target.Get(), L"LPT1", true) == E_INVALIDARG,
                "new folder rejects reserved Windows devices");

        selected = selection(destination / L"renamed-\u03bb.txt");
        succeeded(ShellOperations::remove(nullptr, selected.Get(), true, true), "silent permanent delete");
        require(!fs::exists(destination / L"renamed-\u03bb.txt"), "permanent delete removes fixture");
        require(read(source / L"copy.txt") == "original contents", "delete preserves unrelated source");
        std::cout << "Shell operations: headless copy, collision, move, Unicode rename, new-folder and permanent delete passed\n";
    } catch (const std::exception& error) {
        std::cerr << "Shell operations failed: " << error.what() << '\n';
        ++failures;
    }
    ShellOperations::flushClipboardIfOwned();
    OleUninitialize();
    failures += operationProgressTests();
    return failures;
}
