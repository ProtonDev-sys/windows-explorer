#include "explorer/native_apartment.hpp"
#include "explorer/context_menu.hpp"
#include "explorer/namespace_actions.hpp"
#include "explorer/headless_visual.hpp"

#include <shlobj.h>
#include <wrl/implements.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <iomanip>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <thread>
#include <unordered_set>

// This executable must never publish or invoke shared Shell history on a user's
// computer. CTest enables it only on the disposable GitHub-hosted Windows VM.
// Both independent environment gates are checked before COM, windows or files.
namespace {
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;
namespace fs = std::filesystem;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void succeeded(HRESULT hr, const char* message) {
    if (FAILED(hr)) {
        std::cerr << "HRESULT 0x" << std::hex << static_cast<unsigned long>(hr) << std::dec << '\n';
        throw std::runtime_error(message);
    }
}
bool environmentEquals(const wchar_t* name, const wchar_t* expected) {
    wchar_t value[32]{};
    const DWORD length = GetEnvironmentVariableW(name, value, 32);
    return length && length < 32 && std::wcscmp(value, expected) == 0;
}
void pump() {
    if (const auto desktop = explorer::PrivateDesktop::current()) {
        bool visible = true;
        succeeded(desktop->verifyIsolation(), "native-history pump lost its private desktop");
        succeeded(desktop->visibleWindowsOnInputDesktop(visible), "observe native-history input desktop");
        require(!visible, "native-history fixture exposed an input-desktop window");
    }
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}
void waitFor(const std::function<bool()>& ready, const char* message, DWORD timeout = 15000) {
    const ULONGLONG deadline = GetTickCount64() + timeout;
    while (!ready()) {
        require(GetTickCount64() < deadline, message);
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
        pump();
    }
}

class VisibilityObserver final {
public:
    HRESULT start() {
        input_ = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS | DESKTOP_ENUMERATE | DESKTOP_HOOKCONTROL);
        if (!input_) return HRESULT_FROM_WIN32(GetLastError());
        if (!EnumDesktopWindows(input_, [](HWND window, LPARAM argument) -> BOOL {
            if (IsWindowVisible(window)) reinterpret_cast<VisibilityObserver*>(argument)->baseline_.insert(window);
            return TRUE;
        }, reinterpret_cast<LPARAM>(this))) return HRESULT_FROM_WIN32(GetLastError());
        current_.store(this);
        privateHook_ = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr,
            &shown, 0, 0, WINEVENT_OUTOFCONTEXT);
        if (!privateHook_) return HRESULT_FROM_WIN32(GetLastError());
        std::promise<HRESULT> ready;
        auto result = ready.get_future();
        worker_ = std::thread([this, ready = std::move(ready)]() mutable {
            if (!SetThreadDesktop(input_)) { ready.set_value(HRESULT_FROM_WIN32(GetLastError())); return; }
            const HWINEVENTHOOK hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr,
                &shown, 0, 0, WINEVENT_OUTOFCONTEXT);
            const DWORD error = hook ? ERROR_SUCCESS : GetLastError();
            ready.set_value(hook ? S_OK : HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE));
            while (!stop_.load()) {
                if (!EnumDesktopWindows(input_, [](HWND window, LPARAM argument) -> BOOL {
                    reinterpret_cast<VisibilityObserver*>(argument)->inspect(window);
                    return TRUE;
                }, reinterpret_cast<LPARAM>(this))) visible_.store(true);
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
                pump();
            }
            if (hook) UnhookWinEvent(hook);
        });
        return result.get();
    }
    ~VisibilityObserver() { stop(); }
    void stop() {
        if (privateHook_) { UnhookWinEvent(privateHook_); privateHook_ = nullptr; }
        stop_.store(true);
        if (worker_.joinable()) worker_.join();
        if (input_) { CloseDesktop(input_); input_ = nullptr; }
        current_.store(nullptr);
    }
    bool sawVisibleWindow() const { return visible_.load(); }
private:
    static void CALLBACK shown(HWINEVENTHOOK, DWORD, HWND window, LONG object,
                               LONG child, DWORD, DWORD) {
        if (object == OBJID_WINDOW && child == CHILDID_SELF) {
            if (auto observer = current_.load()) observer->inspect(window);
        }
    }
    void inspect(HWND window) {
        if (!window || !IsWindowVisible(window) || GetAncestor(window, GA_ROOT) != window) return;
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        // Catch own UI and new top-level UI from a Shell surrogate or explorer.
        // Existing desktop/runner windows are baseline observations only.
        if (process == GetCurrentProcessId() || !baseline_.contains(window)) visible_.store(true);
    }
    inline static std::atomic<VisibilityObserver*> current_{nullptr};
    HDESK input_ = nullptr;
    HWINEVENTHOOK privateHook_ = nullptr;
    std::unordered_set<HWND> baseline_;
    std::atomic_bool visible_ = false;
    std::atomic_bool stop_ = false;
    std::thread worker_;
};

class BrowserEvents final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IExplorerBrowserEvents> {
public:
    bool complete = false;
    HRESULT navigation = E_PENDING;
    IFACEMETHODIMP OnNavigationPending(PCIDLIST_ABSOLUTE) override { return S_OK; }
    IFACEMETHODIMP OnViewCreated(IShellView*) override { return S_OK; }
    IFACEMETHODIMP OnNavigationComplete(PCIDLIST_ABSOLUTE) override {
        navigation = S_OK; complete = true; return S_OK;
    }
    IFACEMETHODIMP OnNavigationFailed(PCIDLIST_ABSOLUTE) override {
        navigation = E_FAIL; complete = true; return S_OK;
    }
};
class HiddenBrowser final {
public:
    void initialize(IShellItem* folder) {
        WNDCLASSW definition{};
        definition.lpfnWndProc = DefWindowProcW;
        definition.hInstance = GetModuleHandleW(nullptr);
        definition.lpszClassName = L"WindowsExplorerNativeHistoryHiddenHost";
        const ATOM atom = RegisterClassW(&definition);
        require(atom || GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "register hidden native-history host");
        window_ = CreateWindowExW(0, definition.lpszClassName, L"Native history headless fixture",
            WS_OVERLAPPEDWINDOW, 0, 0, 900, 600, nullptr, nullptr, definition.hInstance, nullptr);
        require(window_ != nullptr && !IsWindowVisible(window_), "create invisible native-history owner");
        succeeded(CoCreateInstance(CLSID_ExplorerBrowser, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&browser_)), "create actual native ExplorerBrowser");
        succeeded(browser_->SetOptions(EBO_NAVIGATEONCE | EBO_NOTRAVELLOG | EBO_NOPERSISTVIEWSTATE),
            "suppress history-fixture view persistence");
        RECT rectangle{0, 0, 900, 600};
        FOLDERSETTINGS settings{FVM_DETAILS, FWF_AUTOARRANGE};
        succeeded(browser_->Initialize(window_, &rectangle, &settings), "initialize hidden actual native Shell view");
        events_ = Make<BrowserEvents>();
        require(events_ != nullptr, "allocate native-history navigation observer");
        succeeded(browser_->Advise(events_.Get(), &cookie_), "observe native-history folder navigation");
        succeeded(browser_->BrowseToObject(folder, SBSP_ABSOLUTE), "navigate hidden browser to owned destination");
        waitFor([&] { return events_->complete; }, "native-history view navigation timed out");
        succeeded(events_->navigation, "complete native-history folder navigation");
        succeeded(browser_->GetCurrentView(IID_PPV_ARGS(&view_)), "obtain actual native Shell view site");
        ComPtr<IFolderView2> folderView;
        succeeded(view_.As(&folderView), "obtain actual native folder view");
        waitFor([&] { int count = -1; return SUCCEEDED(folderView->ItemCount(SVGIO_ALLVIEW, &count)) && !count; },
            "native-history destination view was not initially empty");
        require(!IsWindowVisible(window_), "native-history host remained hidden after navigation");
    }
    ~HiddenBrowser() { close(); }
    void close() {
        view_.Reset();
        if (browser_) {
            if (cookie_) browser_->Unadvise(cookie_);
            browser_->Destroy();
            browser_.Reset();
        }
        events_.Reset();
        if (window_) { DestroyWindow(window_); window_ = nullptr; }
    }
    HWND window() const { return window_; }
    IUnknown* site() const { return view_.Get(); }
private:
    HWND window_ = nullptr;
    DWORD cookie_ = 0;
    ComPtr<IExplorerBrowser> browser_;
    ComPtr<IShellView> view_;
    ComPtr<BrowserEvents> events_;
};

struct PidlDeleter {
    using pointer = PIDLIST_ABSOLUTE;
    void operator()(PIDLIST_ABSOLUTE value) const { CoTaskMemFree(value); }
};
struct KeyCloser { void operator()(HKEY__* value) const { RegCloseKey(value); } };
struct HistoryState { bool undo = false; bool redo = false; UINT undoId = 0; UINT redoId = 0; };
class HistoryMenu final {
public:
    void initialize(HWND owner, IShellItem* folder, IUnknown* site) {
        HKEY raw = nullptr;
        const LONG error = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\CommandStore", 0, KEY_READ, &raw);
        succeeded(HRESULT_FROM_WIN32(error), "open installed native CommandStore read-only");
        key_.reset(raw);
        ComPtr<IShellFolder> nativeFolder;
        succeeded(folder->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(&nativeFolder)),
            "bind native history destination folder");
        PIDLIST_ABSOLUTE rawId = nullptr;
        const HRESULT idResult = SHGetIDListFromObject(folder, &rawId);
        std::unique_ptr<ITEMIDLIST, PidlDeleter> id(rawId);
        succeeded(idResult, "obtain native history folder PIDL");
        DEFCONTEXTMENU definition{};
        definition.hwnd = owner;
        definition.pidlFolder = id.get();
        definition.psf = nativeFolder.Get();
        definition.cKeys = 1;
        definition.aKeys = &raw;
        succeeded(SHCreateDefaultContextMenu(&definition, IID_PPV_ARGS(&context_)),
            "create registered native background CommandStore menu");
        succeeded(menu_.create(owner, context_.Get(), site, CMF_EXTENDEDVERBS),
            "attach actual native Shell view site to CommandStore handlers");
        std::vector<explorer::ContextMenuEntry> entries;
        succeeded(menu_.enumerate(entries), "read native Undo/Redo state without displaying menus");
        find(entries, true, 0);
        require(state_.undoId && state_.redoId, "both registered Windows.undo/redo commands must exist");
    }
    const HistoryState& state() const { return state_; }
    void invoke(HWND owner, bool redo) {
        const UINT id = redo ? state_.redoId : state_.undoId;
        require(redo ? state_.redo : state_.undo, "invoke only an enabled native history command");
        require(id >= menu_.firstCommand() && id - menu_.firstCommand() < menu_.commandCount(),
            "native history ordinal belongs to the actual enumerated menu");
        CMINVOKECOMMANDINFOEX command{};
        command.cbSize = sizeof(command);
        command.fMask = CMIC_MASK_UNICODE | CMIC_MASK_FLAG_NO_UI | CMIC_MASK_NOASYNC;
        command.hwnd = owner;
        const UINT ordinal = id - menu_.firstCommand();
        command.lpVerb = MAKEINTRESOURCEA(ordinal);
        command.lpVerbW = MAKEINTRESOURCEW(ordinal);
        command.nShow = SW_HIDE;
        // Strictly CI-only. Do not weaken NativeNamespaceActions' production
        // hidden-owner/headless rejection just to execute this owned fixture.
        succeeded(context_->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&command)),
            redo ? "invoke native registered Redo without UI" : "invoke native registered Undo without UI");
    }
private:
    void find(const std::vector<explorer::ContextMenuEntry>& entries, bool ancestorsEnabled, unsigned depth) {
        for (const auto& entry : entries) {
            if (!entry.separator() && !entry.submenu && entry.id) {
                const bool undo = _wcsicmp(entry.canonicalVerb.c_str(), L"Windows.undo") == 0;
                const bool redo = _wcsicmp(entry.canonicalVerb.c_str(), L"Windows.redo") == 0;
                if (undo || redo) {
                    UINT& id = undo ? state_.undoId : state_.redoId;
                    unsigned& bestDepth = undo ? undoDepth_ : redoDepth_;
                    // The broad installed CommandStore also exposes these
                    // commands inside File/QAT cascades. Prefer the direct
                    // entry, matching production Namespace command planning.
                    if (depth < bestDepth) {
                        bestDepth = depth;
                        id = entry.id;
                        (undo ? state_.undo : state_.redo) = ancestorsEnabled && entry.enabled();
                    } else if (depth == bestDepth) {
                        require(!id, "native history direct canonical command must be unambiguous");
                    }
                }
            }
            find(entry.children, ancestorsEnabled && entry.enabled(), depth + 1);
        }
    }
    std::unique_ptr<HKEY__, KeyCloser> key_;
    ComPtr<IContextMenu> context_;
    explorer::NativeContextMenu menu_;
    HistoryState state_;
    unsigned undoDepth_ = UINT_MAX, redoDepth_ = UINT_MAX;
};

void compareFastHistoryState(HiddenBrowser& browser, IShellItem* destination, const char* stage) {
    HistoryMenu menu;
    menu.initialize(browser.window(), destination, browser.site());
    const std::array commands{L"Windows.undo", L"Windows.redo"};
    const std::array enabled{menu.state().undo, menu.state().redo};
    for (size_t index = 0; index < commands.size(); ++index) {
        explorer::NamespaceCommandState state;
        succeeded(explorer::namespaceCommandState(commands[index], nullptr, browser.site(), &state),
            "read-only fast native GetState(FALSE) history query");
        require(state.enabled() == enabled[index],
            "fast native history capability differs from actual CommandStore menu state");
    }
    std::cout << "Fast native history states equal actual IContextMenu at " << stage << '\n';
}

ComPtr<IShellItem> item(const fs::path& path) {
    ComPtr<IShellItem> result;
    succeeded(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&result)),
        "create owned native-history Shell item");
    return result;
}
FILE_ID_INFO identity(const fs::path& path) {
    const HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "open owned native-history identity");
    FILE_ID_INFO result{};
    const BOOL ok = GetFileInformationByHandleEx(handle, FileIdInfo, &result, sizeof(result));
    CloseHandle(handle);
    require(ok != FALSE, "read owned native-history volume and 128-bit file identity");
    return result;
}
bool sameIdentity(const FILE_ID_INFO& a, const FILE_ID_INFO& b) {
    return a.VolumeSerialNumber == b.VolumeSerialNumber &&
        std::memcmp(&a.FileId, &b.FileId, sizeof(a.FileId)) == 0;
}
void reportIdentity(const char* name, const FILE_ID_INFO& id) {
    std::cout << name << ": volume " << std::hex << id.VolumeSerialNumber << " file ";
    for (const BYTE value : id.FileId.Identifier) std::cout << std::setw(2) << std::setfill('0') << static_cast<unsigned>(value);
    std::cout << std::dec << std::setfill(' ') << '\n';
}
std::string read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(file.good(), "read owned native-history file contents");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
size_t childCount(const fs::path& path) {
    return static_cast<size_t>(std::distance(fs::directory_iterator(path), fs::directory_iterator{}));
}
struct Fixture {
    fs::path root;
    Fixture() {
        GUID guid{};
        succeeded(CoCreateGuid(&guid), "generate owned native-history fixture identity");
        wchar_t value[40]{};
        require(StringFromGUID2(guid, value, 40) != 0, "format native-history fixture identity");
        root = fs::temp_directory_path() / (std::wstring(L"WindowsExplorer-NativeHistory-") + value);
        require(fs::create_directories(root / L"source") && fs::create_directory(root / L"destination"),
            "create fresh owned native-history directories");
    }
    ~Fixture() { std::error_code ignored; fs::remove_all(root, ignored); }
};
class CopySink final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IFileOperationProgressSink> {
public:
    HRESULT result = S_OK;
    unsigned copies = 0;
    ComPtr<IShellItem> created;
    void record(HRESULT hr) { if (FAILED(hr) && SUCCEEDED(result)) result = hr; }
    IFACEMETHODIMP StartOperations() override { return S_OK; }
    IFACEMETHODIMP FinishOperations(HRESULT hr) override { record(hr); return S_OK; }
    IFACEMETHODIMP PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return E_UNEXPECTED; }
    IFACEMETHODIMP PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return E_UNEXPECTED; }
    IFACEMETHODIMP PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return E_UNEXPECTED; }
    IFACEMETHODIMP PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return E_UNEXPECTED; }
    IFACEMETHODIMP PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT hr, IShellItem* output) override {
        record(hr);
        if (SUCCEEDED(hr) && output) { ++copies; created = output; }
        else if (SUCCEEDED(result)) result = E_UNEXPECTED;
        return S_OK;
    }
    IFACEMETHODIMP PreDeleteItem(DWORD, IShellItem*) override { return E_UNEXPECTED; }
    IFACEMETHODIMP PostDeleteItem(DWORD, IShellItem*, HRESULT, IShellItem*) override { return E_UNEXPECTED; }
    IFACEMETHODIMP PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return E_UNEXPECTED; }
    IFACEMETHODIMP PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override { return E_UNEXPECTED; }
    IFACEMETHODIMP UpdateProgress(UINT, UINT) override { return S_OK; }
    IFACEMETHODIMP ResetTimer() override { return S_OK; }
    IFACEMETHODIMP PauseTimer() override { return S_OK; }
    IFACEMETHODIMP ResumeTimer() override { return S_OK; }
};
fs::path nativeCopy(HWND owner, IShellItem* source, IShellItem* destination) {
    ComPtr<IFileOperation> operation;
    succeeded(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&operation)), "create owned native-history copy operation");
    succeeded(operation->SetOperationFlags(FOF_NO_UI | FOFX_ADDUNDORECORD | FOFX_EARLYFAILURE |
        FOFX_NOCOPYHOOKS | FOF_NOCONFIRMMKDIR), "publish exactly one native Undo record without operation UI");
    succeeded(operation->SetOwnerWindow(owner), "attach hidden native-history operation owner");
    auto sink = Make<CopySink>();
    require(sink != nullptr, "allocate native-history copy result observer");
    DWORD cookie = 0;
    succeeded(operation->Advise(sink.Get(), &cookie), "observe exact owned native copy output");
    HRESULT result = operation->CopyItem(source, destination, nullptr, nullptr);
    if (SUCCEEDED(result)) result = operation->PerformOperations();
    BOOL aborted = FALSE;
    const HRESULT abortedResult = operation->GetAnyOperationsAborted(&aborted);
    operation->Unadvise(cookie);
    succeeded(result, "perform owned native-history copy");
    succeeded(sink->result, "owned native-history per-item copy result");
    succeeded(abortedResult, "read native-history operation abort state");
    require(!aborted && sink->copies == 1 && sink->created, "exactly one owned native copy completed");
    PWSTR raw = nullptr;
    const HRESULT pathResult = sink->created->GetDisplayName(SIGDN_FILESYSPATH, &raw);
    const fs::path path = raw ? fs::path(raw) : fs::path{};
    CoTaskMemFree(raw);
    succeeded(pathResult, "obtain actual native-history copy destination");
    require(!path.empty(), "native-history copy has a physical output path");
    return path;
}

void runNativeHistory() {
    const auto desktop = explorer::PrivateDesktop::current();
    require(desktop && desktop->ready(), "native history requires its initialized private desktop");
    succeeded(desktop->verifyIsolation(), "native history lost initial desktop isolation");
    VisibilityObserver observer;
    succeeded(observer.start(), "start headless native-history visibility observer");
    Fixture fixture;
    const auto source = fixture.root / L"source" / L"owned \u03bb copy.txt";
    const auto destination = fixture.root / L"destination";
    std::string contents(8192, '\0');
    for (size_t index = 0; index < contents.size(); ++index)
        contents[index] = static_cast<char>((index * 31) ^ (index >> 3));
    {
        std::ofstream output(source, std::ios::binary);
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        require(output.good(), "write exactly 8 KB owned native-history source");
    }
    const auto sourceId = identity(source);
    auto sourceItem = item(source);
    auto destinationItem = item(destination);
    HiddenBrowser browser;
    browser.initialize(destinationItem.Get());
    {
        HistoryMenu initial;
        initial.initialize(browser.window(), destinationItem.Get(), browser.site());
        require(!initial.state().undo && !initial.state().redo,
            "isolated VM must initially have no unrelated native Undo or Redo history");
    }
    compareFastHistoryState(browser, destinationItem.Get(), "initial empty history");
    require(!observer.sawVisibleWindow(), "initial native-history setup displayed UI");
    const fs::path copied = nativeCopy(browser.window(), sourceItem.Get(), destinationItem.Get());
    require(sameIdentity(identity(copied.parent_path()), identity(destination)) && copied.filename() == source.filename(),
        "native record targets exactly the fresh owned destination");
    require(childCount(destination) == 1 && read(copied) == contents,
        "native copy creates exactly one correct owned output");
    const auto copiedId = identity(copied);
    require(!sameIdentity(sourceId, copiedId), "native copy is a distinct filesystem object");
    require(read(source) == contents && sameIdentity(sourceId, identity(source)), "native copy preserves original source");
    waitFor([&] {
        HistoryMenu menu;
        menu.initialize(browser.window(), destinationItem.Get(), browser.site());
        return menu.state().undo && !menu.state().redo;
    }, "new owned native record did not enable Undo and clear Redo");
    compareFastHistoryState(browser, destinationItem.Get(), "after sole owned copy");
    require(!observer.sawVisibleWindow(), "native history copy displayed UI");
    {
        HistoryMenu undo;
        undo.initialize(browser.window(), destinationItem.Get(), browser.site());
        undo.invoke(browser.window(), false);
    }
    waitFor([&] { return !fs::exists(copied); }, "native Undo did not remove its sole owned copy");
    require(!childCount(destination) && read(source) == contents && sameIdentity(sourceId, identity(source)),
        "native Undo removes only owned output and preserves source identity/content");
    waitFor([&] {
        HistoryMenu menu;
        menu.initialize(browser.window(), destinationItem.Get(), browser.site());
        return !menu.state().undo && menu.state().redo;
    }, "native Undo did not transition its sole owned record to Redo");
    compareFastHistoryState(browser, destinationItem.Get(), "after owned Undo");
    require(!observer.sawVisibleWindow(), "native Undo displayed UI");
    {
        HistoryMenu redo;
        redo.initialize(browser.window(), destinationItem.Get(), browser.site());
        redo.invoke(browser.window(), true);
    }
    waitFor([&] { return fs::exists(copied) && fs::file_size(copied) == contents.size(); },
        "native Redo did not restore owned output");
    const auto restoredId = identity(copied);
    require(childCount(destination) == 1 && read(copied) == contents &&
        read(source) == contents && sameIdentity(sourceId, identity(source)),
        "native Redo restores exactly one correct copy and preserves original source");
    require(!sameIdentity(sourceId, restoredId), "native Redo output remains distinct from its original source");
    waitFor([&] {
        HistoryMenu menu;
        menu.initialize(browser.window(), destinationItem.Get(), browser.site());
        return menu.state().undo && !menu.state().redo;
    }, "native Redo did not restore Undo state and clear Redo");
    compareFastHistoryState(browser, destinationItem.Get(), "after owned Redo");
    require(!IsWindowVisible(browser.window()), "native-history owner remained hidden through Redo");
    browser.close();
    observer.stop();
    succeeded(desktop->verifyIsolation(), "native history changed desktop isolation");
    require(!observer.sawVisibleWindow(),
        "native Undo/Redo fixture must never display a window");
    // A Copy Redo may legitimately recreate the copy. Identity preservation is
    // asserted for the source and reported for copies, rather than invented as
    // a public Shell Undo contract.
    std::cout << "Native history: real registered Undo/Redo, 8 KB copy, exact contents/count, source identity, state transitions and no visible UI passed\n";
    std::cout << "Native Redo copy retained first copy identity: " << (sameIdentity(copiedId, restoredId) ? "yes" : "no (copy recreated)") << '\n';
    reportIdentity("Owned source identity", sourceId);
    reportIdentity("First native copy identity", copiedId);
    reportIdentity("Native Redo copy identity", restoredId);
}
} // namespace

int main() {
    if (!environmentEquals(L"GITHUB_ACTIONS", L"true") ||
        !environmentEquals(L"WINDOWSEXPLORER_NATIVE_HISTORY_TEST", L"1")) {
        std::cout << "SKIP: native shared history fixture requires the disposable GitHub runner and explicit opt-in\n";
        return 0;
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    explorer::PrivateDesktop desktop;
    if (FAILED(desktop.initialize()) || FAILED(desktop.verifyIsolation())) {
        std::cerr << "Cannot initialize isolated native-history desktop\n"; return 1;
    }
    explorer::NativeApartmentOwner nativeApartment;
    const HRESULT initialized = nativeApartment.initializeOle();
    if (FAILED(initialized)) { std::cerr << "Cannot initialize native-history STA\n"; return 1; }
    int result = 0;
    try { runNativeHistory(); }
    catch (const std::exception& error) { std::cerr << "FAIL: CI-only native Undo/Redo: " << error.what() << '\n'; result = 1; }
    catch (...) { std::cerr << "FAIL: CI-only native Undo/Redo: unknown exception\n"; result = 1; }
    nativeApartment.finishOrTerminate();
    if (FAILED(desktop.verifyIsolation())) result = 1;
    return result;
}
