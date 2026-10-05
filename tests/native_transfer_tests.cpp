#include "explorer/breadcrumb.hpp"
#include "explorer/headless_visual.hpp"
#include "explorer/namespace_actions.hpp"
#include "explorer/shell_operations.hpp"
#include "explorer/worker_sta.hpp"

#include <shlobj.h>
#include <wrl/implements.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

// These are real shared-clipboard/native-history operations. The independent
// gates precede COM, HWNDs and fixture creation. Only the dedicated disposable
// CI job enables this executable; ordinary local and CI suites skip it.
namespace {
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;
namespace fs = std::filesystem;
unsigned activeNativeOperations = 0;
[[noreturn]] void unfinishedFailure(const char* message) {
    std::cerr << "FAIL: native transfer resources retained: " << message << std::endl;
    // This executable has already verified both genuine disposable-CI gates.
    // Never unwind a live native asynchronous operation's data/site/fixture.
    if (!TerminateProcess(GetCurrentProcess(), 1)) std::_Exit(1);
    std::_Exit(1);
}
class OperationLease final {
public:
    OperationLease() noexcept { ++activeNativeOperations; }
    ~OperationLease() {
        if (active_) unfinishedFailure("operation did not reach verified completion");
    }
    void complete() noexcept { if (active_) { --activeNativeOperations; active_ = false; } }
private:
    bool active_ = true;
};

void require(bool value, const char* message) {
    if (!value) {
        if (activeNativeOperations) unfinishedFailure(message);
        throw std::runtime_error(message);
    }
}
void succeeded(HRESULT result, const char* message) {
    if (FAILED(result)) {
        std::cerr << "Native transfer HRESULT=" << static_cast<ULONG>(result) << '\n';
        if (activeNativeOperations) unfinishedFailure(message);
        throw std::runtime_error(message);
    }
}
bool environmentEquals(const wchar_t* name, const wchar_t* expected) {
    wchar_t value[32]{};
    const auto count = GetEnvironmentVariableW(name, value, 32);
    return count && count < 32 && std::wcscmp(value, expected) == 0;
}
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE handle = nullptr) noexcept : value(handle) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
struct PidlDeleter {
    using pointer = PIDLIST_ABSOLUTE;
    void operator()(PIDLIST_ABSOLUTE value) const noexcept { CoTaskMemFree(value); }
};
using Pidl = std::unique_ptr<ITEMIDLIST, PidlDeleter>;
struct Medium {
    STGMEDIUM value{};
    ~Medium() { if (value.tymed != TYMED_NULL) ReleaseStgMedium(&value); }
};
struct Identity {
    ULONGLONG volume = 0;
    std::array<BYTE, 16> id{};
    auto operator<=>(const Identity&) const = default;
};
Identity identity(const fs::path& path) {
    Handle file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    require(file.value != INVALID_HANDLE_VALUE, "Open owned transfer identity");
    FILE_ID_INFO native{};
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    require(GetFileInformationByHandleEx(file.value, FileIdInfo, &native, sizeof(native)) &&
        GetFileInformationByHandleEx(file.value, FileAttributeTagInfo, &attributes, sizeof(attributes)),
        "Read owned transfer identity");
    require(!(attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), "Unexpected owned transfer reparse point");
    Identity result; result.volume = native.VolumeSerialNumber;
    std::copy(std::begin(native.FileId.Identifier), std::end(native.FileId.Identifier), result.id.begin());
    return result;
}
fs::path itemPath(IShellItem* item) {
    PWSTR text = nullptr;
    succeeded(item->GetDisplayName(SIGDN_FILESYSPATH, &text), "Read owned native filesystem path");
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned(text, CoTaskMemFree);
    require(text != nullptr, "Native filesystem path was null");
    return fs::path(text);
}
ComPtr<IShellItem> shellItem(const fs::path& path) {
    ComPtr<IShellItem> result;
    succeeded(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&result)), "Create owned native Shell item");
    return result;
}
std::vector<BYTE> readFile(const fs::path& path) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    require(file.value != INVALID_HANDLE_VALUE, "Open owned transfer bytes");
    LARGE_INTEGER length{};
    require(GetFileSizeEx(file.value, &length) && length.QuadPart >= 0 && length.QuadPart <= 65536,
        "Owned transfer byte bound");
    std::vector<BYTE> bytes(static_cast<size_t>(length.QuadPart));
    DWORD read = 0;
    require(ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) &&
        read == bytes.size(), "Read complete owned transfer bytes");
    return bytes;
}
struct Source {
    fs::path path;
    Identity id;
    std::vector<BYTE> bytes;
    LONGLONG modified = 0;
};
LONGLONG modified(const fs::path& path) {
    Handle file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    FILE_BASIC_INFO basic{};
    require(file.value != INVALID_HANDLE_VALUE && GetFileInformationByHandleEx(file.value, FileBasicInfo, &basic, sizeof(basic)),
        "Read owned source modification time");
    return basic.LastWriteTime.QuadPart;
}
bool preserved(const Source& source) {
    return fs::exists(source.path) && identity(source.path) == source.id &&
        readFile(source.path) == source.bytes && modified(source.path) == source.modified;
}
std::vector<fs::path> children(const fs::path& folder) {
    std::vector<fs::path> result;
    for (const auto& entry : fs::directory_iterator(folder)) {
        require(entry.is_regular_file() && !(GetFileAttributesW(entry.path().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT),
            "Transfer output must be an owned regular file");
        result.push_back(entry.path());
    }
    std::sort(result.begin(), result.end());
    return result;
}
struct Fixture {
    fs::path root;
    Identity rootId;
    std::vector<Source> sources;
    explicit Fixture() {
        GUID identifier{}; succeeded(CoCreateGuid(&identifier), "Create transfer fixture identifier");
        wchar_t name[40]{}; require(StringFromGUID2(identifier, name, 40) != 0, "Format transfer identifier");
        root = fs::canonical(fs::temp_directory_path()) / (std::wstring(L"WindowsExplorer-NativeTransfer-") + name);
        require(fs::create_directory(root), "Create fresh owned transfer root");
        rootId = identity(root);
        require(fs::create_directory(root / L"Source"), "Create owned transfer source folder");
        for (unsigned index = 0; index < 7; ++index) {
            const auto path = root / L"Source" / (L"Owned-" + std::to_wstring(index) + L".bin");
            std::vector<BYTE> bytes(8192);
            for (size_t offset = 0; offset < bytes.size(); ++offset)
                bytes[offset] = static_cast<BYTE>((offset * 13 + index * 37) & 255);
            Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
            DWORD written = 0;
            require(file.value != INVALID_HANDLE_VALUE && WriteFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                written == bytes.size() && FlushFileBuffers(file.value), "Write owned transfer source");
        }
        for (unsigned index = 0; index < 7; ++index) {
            const auto path = root / L"Source" / (L"Owned-" + std::to_wstring(index) + L".bin");
            sources.push_back({path, identity(path), readFile(path), modified(path)});
        }
        for (const wchar_t* directory : {L"Copy", L"Move", L"Shortcut", L"DropCopy", L"DropMove", L"DropLink"})
            require(fs::create_directory(root / directory), "Create collision-free owned transfer destination");
    }
    ~Fixture() {
        // No path returned by a provider becomes a cleanup root. Verify the
        // original create-new directory and every descendant before removal.
        try {
            if (root.empty() || root.filename().native().find(L"WindowsExplorer-NativeTransfer-") != 0 ||
                !fs::exists(root) || identity(root) != rootId) return;
            for (const auto& entry : fs::recursive_directory_iterator(root))
                if (GetFileAttributesW(entry.path().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) return;
            std::error_code error; fs::remove_all(root, error);
        } catch (...) { }
    }
    void cleanup() {
        require(!root.empty() && root.filename().native().find(L"WindowsExplorer-NativeTransfer-") == 0 &&
            fs::exists(root) && identity(root) == rootId, "Verify exact create-new transfer cleanup root");
        for (const auto& entry : fs::recursive_directory_iterator(root))
            require(!(GetFileAttributesW(entry.path().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT), "Refuse transfer cleanup through a reparse point");
        std::error_code error; const auto removed = fs::remove_all(root, error);
        require(!error && removed && !fs::exists(root), "Remove only the verified completed owned transfer fixture");
        root.clear();
    }
    void verifySources(const std::set<unsigned>& moved) const {
        for (unsigned index = 0; index < sources.size(); ++index) {
            if (moved.contains(index)) require(!fs::exists(sources[index].path), "Native move must remove only its owned source");
            else require(preserved(sources[index]), "Native transfer changed an unrelated owned source");
        }
    }
};

// Observe both desktops on separate non-COM threads. The only permitted
// private top-level HWND is our frame. No desktop is ever switched. New input
// desktop windows (including another Shell process) fail the fixture.
class VisibilityObserver final {
public:
    void start() {
        const auto desktop = explorer::PrivateDesktop::current();
        require(desktop && desktop->ready(), "Transfer observer requires its initialized private desktop");
        bool unchanged = false;
        succeeded(desktop->verifyIsolation(&unchanged), "Verify transfer observer desktop before attachment");
        require(unchanged, "Transfer observer input desktop changed before attachment");
        private_ = GetThreadDesktop(GetCurrentThreadId()); // borrowed; guard outlives both threads
        station_ = GetProcessWindowStation(); // borrowed; never closed or reassigned
        privateName_ = desktop->name(); inputName_ = desktop->originalInputName();
        input_ = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS | DESKTOP_ENUMERATE | DESKTOP_HOOKCONTROL);
        require(input_ != nullptr, "Open input desktop for read-only transfer observation");
        current_.store(this);
        // Read the baseline on its actual attached thread: GetTopWindow(NULL)
        // must observe the same input desktop as EnumDesktopWindows.
        startWorker(input_, false, inputWorker_);
        startWorker(private_, true, privateWorker_);
    }
    ~VisibilityObserver() { stop(); }
    void allowFrame(HWND frame) noexcept { frame_.store(frame); }
    bool unexpected() const noexcept { return unexpected_.load(); }
    unsigned long long inputObservations() const noexcept { return inputObservations_.load(); }
    unsigned long long privateObservations() const noexcept { return privateObservations_.load(); }
    void reportEnumeration() const {
        // Called only after stop joined both observer threads.
        const auto report = [](const char* desktop, const Sample& snapshot, unsigned long long empty) {
            std::cout << "Native transfer desktop=" << desktop << " enumerated=" << snapshot.enumerated
                << " error=" << snapshot.error << " callbacks=" << snapshot.totalVisited
                << " topBefore=" << reinterpret_cast<UINT_PTR>(snapshot.topBefore)
                << " topAfter=" << reinterpret_cast<UINT_PTR>(snapshot.topAfter)
                << " topErrors=" << snapshot.topBeforeError << '/' << snapshot.topAfterError
                << " identityHRESULT=" << static_cast<ULONG>(snapshot.identityStatus)
                << " enumerationHRESULT=" << static_cast<ULONG>(snapshot.status)
                << " confirmedEmpty=" << snapshot.confirmedEmpty << " emptySamples=" << empty << '\n';
        };
        report("input", inputSample_, inputEmptyObservations_.load());
        report("private", privateSample_, privateEmptyObservations_.load());
    }
    void stop() noexcept {
        stop_.store(true);
        if (inputWorker_.joinable()) inputWorker_.join();
        if (privateWorker_.joinable()) privateWorker_.join();
        current_.store(nullptr);
        if (input_) { CloseDesktop(input_); input_ = nullptr; }
    }
private:
    struct Sample {
        BOOL enumerated = FALSE;
        DWORD error = ERROR_SUCCESS, topBeforeError = ERROR_SUCCESS, topAfterError = ERROR_SUCCESS;
        size_t totalVisited = 0;
        HWND topBefore = nullptr, topAfter = nullptr;
        HRESULT identityStatus = E_PENDING, status = E_PENDING;
        bool confirmedEmpty = false;
    };
    static HRESULT nameOf(HANDLE object, std::wstring& result) {
        wchar_t text[256]{}; DWORD needed = 0;
        if (!object || !GetUserObjectInformationW(object, UOI_NAME, text, sizeof(text), &needed)) {
            const DWORD error = GetLastError();
            return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
        }
        if (needed > sizeof(text) || !needed || !text[0] || text[255]) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        result.assign(text); return S_OK;
    }
    HRESULT verifyDesktopIdentity(HDESK desktop, bool privateDesktop) const {
        if (!desktop || GetProcessWindowStation() != station_) return E_ACCESSDENIED;
        std::wstring current, enumerated, privateName, inputName;
        auto status = nameOf(GetThreadDesktop(GetCurrentThreadId()), current);
        if (SUCCEEDED(status)) status = nameOf(desktop, enumerated);
        if (SUCCEEDED(status)) status = nameOf(private_, privateName);
        const auto input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
        if (!input) {
            const DWORD error = GetLastError();
            return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
        }
        if (SUCCEEDED(status)) status = nameOf(input, inputName);
        const BOOL closed = CloseDesktop(input);
        const DWORD closeError = closed ? ERROR_SUCCESS : GetLastError();
        if (FAILED(status)) return status;
        if (!closed) return HRESULT_FROM_WIN32(closeError ? closeError : ERROR_GEN_FAILURE);
        const auto& expected = privateDesktop ? privateName_ : inputName_;
        return _wcsicmp(current.c_str(), expected.c_str()) == 0 &&
            _wcsicmp(enumerated.c_str(), expected.c_str()) == 0 &&
            _wcsicmp(privateName.c_str(), privateName_.c_str()) == 0 &&
            _wcsicmp(inputName.c_str(), inputName_.c_str()) == 0 &&
            _wcsicmp(privateName.c_str(), inputName.c_str()) != 0 ? S_OK : E_ACCESSDENIED;
    }
    Sample sample(HDESK desktop, bool privateDesktop, bool baseline = false) {
        Sample result;
        SetLastError(ERROR_SUCCESS); result.topBefore = GetTopWindow(nullptr);
        result.topBeforeError = result.topBefore ? ERROR_SUCCESS : GetLastError();
        struct Context {
            VisibilityObserver* observer; Sample* result; bool privateDesktop, baseline, allocationFailed = false;
        } context{this, &result, privateDesktop, baseline};
        SetLastError(ERROR_SUCCESS);
        result.enumerated = EnumDesktopWindows(desktop, [](HWND window, LPARAM argument) -> BOOL {
            auto& observed = *reinterpret_cast<Context*>(argument);
            ++observed.result->totalVisited; // Includes foreign/hidden HWNDs.
            if (observed.baseline) {
                try { if (IsWindowVisible(window)) observed.observer->baseline_.insert(window); }
                catch (...) { observed.allocationFailed = true; return FALSE; }
            } else observed.observer->inspect(window, observed.privateDesktop);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&context));
        result.error = result.enumerated ? ERROR_SUCCESS : GetLastError();
        SetLastError(ERROR_SUCCESS); result.topAfter = GetTopWindow(nullptr);
        result.topAfterError = result.topAfter ? ERROR_SUCCESS : GetLastError();
        result.identityStatus = verifyDesktopIdentity(desktop, privateDesktop);
        result.confirmedEmpty = !result.enumerated && !result.error && !context.allocationFailed &&
            !result.totalVisited && !result.topBefore && !result.topAfter &&
            !result.topBeforeError && !result.topAfterError && SUCCEEDED(result.identityStatus);
        if (FAILED(result.identityStatus)) result.status = result.identityStatus;
        else if (context.allocationFailed) result.status = E_OUTOFMEMORY;
        else if (result.enumerated) result.status = S_OK;
        else if (result.error) result.status = HRESULT_FROM_WIN32(result.error);
        else result.status = result.confirmedEmpty ? S_FALSE : HRESULT_FROM_WIN32(ERROR_GEN_FAILURE);
        return result;
    }
    static void CALLBACK shown(HWINEVENTHOOK, DWORD, HWND window, LONG object, LONG child, DWORD, DWORD) {
        if (object == OBJID_WINDOW && child == CHILDID_SELF)
            if (const auto observer = current_.load()) observer->inspect(window, privateThread_);
    }
    void inspect(HWND window, bool privateDesktop) noexcept {
        if (!window || !IsWindowVisible(window) || GetAncestor(window, GA_ROOT) != window) return;
        if (privateDesktop) {
            if (window != frame_.load()) unexpected_.store(true);
        } else {
            DWORD process = 0; GetWindowThreadProcessId(window, &process);
            if (process == GetCurrentProcessId() || !baseline_.contains(window)) unexpected_.store(true);
        }
    }
    void startWorker(HDESK desktop, bool privateDesktop, std::thread& worker) {
        std::promise<HRESULT> ready; auto started = ready.get_future();
        worker = std::thread([this, desktop, privateDesktop, ready = std::move(ready)]() mutable {
            privateThread_ = privateDesktop;
            if (!SetThreadDesktop(desktop)) { ready.set_value(HRESULT_FROM_WIN32(GetLastError())); return; }
            if (!privateDesktop) {
                try {
                    inputSample_ = sample(desktop, false, true);
                    if (FAILED(inputSample_.status)) { ready.set_value(inputSample_.status); return; }
                } catch (...) { ready.set_value(E_OUTOFMEMORY); return; }
            }
            const auto hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, shown, 0, 0, WINEVENT_OUTOFCONTEXT);
            const auto error = hook ? ERROR_SUCCESS : GetLastError();
            ready.set_value(hook ? S_OK : HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE));
            while (!stop_.load()) {
                try {
                    const auto result = sample(desktop, privateDesktop);
                    if (privateDesktop) privateSample_ = result; else inputSample_ = result;
                    if (FAILED(result.status)) unexpected_.store(true);
                    else if (privateDesktop) {
                        ++privateObservations_;
                        if (result.confirmedEmpty) ++privateEmptyObservations_;
                    } else {
                        ++inputObservations_;
                        if (result.confirmedEmpty) ++inputEmptyObservations_;
                    }
                } catch (...) { unexpected_.store(true); }
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
                MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            }
            if (hook) UnhookWinEvent(hook);
        });
        succeeded(started.get(), "Start exact-desktop transfer visibility observer");
    }
    inline static std::atomic<VisibilityObserver*> current_{nullptr};
    inline static thread_local bool privateThread_ = false;
    HDESK input_ = nullptr, private_ = nullptr;
    HWINSTA station_ = nullptr;
    std::wstring privateName_, inputName_;
    Sample inputSample_, privateSample_;
    std::unordered_set<HWND> baseline_;
    std::atomic<HWND> frame_{nullptr};
    std::atomic_bool stop_{false}, unexpected_{false};
    std::atomic<unsigned long long> inputObservations_{0}, privateObservations_{0};
    std::atomic<unsigned long long> inputEmptyObservations_{0}, privateEmptyObservations_{0};
    std::thread inputWorker_, privateWorker_;
};

VisibilityObserver* observation = nullptr;
void pump() {
    const auto desktop = explorer::PrivateDesktop::current();
    require(desktop && desktop->ready(), "Transfer pump requires its initialized private desktop");
    succeeded(desktop->verifyIsolation(), "Transfer changed input desktop isolation");
    bool visible = true;
    succeeded(desktop->visibleWindowsOnInputDesktop(visible), "Observe transfer input desktop");
    require(!visible && (!observation || !observation->unexpected()), "Native transfer displayed unexpected UI");
    MSG message{};
    unsigned count = 0;
    while (count++ < 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        require(message.message != WM_QUIT, "Transfer fixture received quit");
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    Handle event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    require(event.value != nullptr, "Create owned bounded transfer pump event");
    DWORD index = 0;
    const HRESULT waited = CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS | COWAIT_DISPATCH_WINDOW_MESSAGES,
        5, 1, &event.value, &index);
    require(waited == RPC_S_CALLPENDING || SUCCEEDED(waited), "Native transfer STA dispatch failed");
}
void waitFor(const std::function<bool()>& predicate, const char* message, DWORD duration = 12000) {
    const auto deadline = GetTickCount64() + duration;
    while (!predicate()) { require(GetTickCount64() < deadline, message); pump(); }
    pump();
}
class Events final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IExplorerBrowserEvents> {
public:
    bool complete = false; HRESULT result = E_PENDING;
    IFACEMETHODIMP OnNavigationPending(PCIDLIST_ABSOLUTE) override { return S_OK; }
    IFACEMETHODIMP OnViewCreated(IShellView*) override { return S_OK; }
    IFACEMETHODIMP OnNavigationComplete(PCIDLIST_ABSOLUTE) override { complete = true; result = S_OK; return S_OK; }
    IFACEMETHODIMP OnNavigationFailed(PCIDLIST_ABSOLUTE) override { complete = true; result = E_FAIL; return S_OK; }
};
struct Browser {
    HWND owner = nullptr;
    DWORD cookie = 0;
    ComPtr<IExplorerBrowser> browser;
    ComPtr<IShellView> view;
    ComPtr<IFolderView2> folderView;
    ComPtr<IShellItem> folder;
    ComPtr<Events> events;
    explorer::NativeNamespaceActions actions;
    ~Browser() { close(); }
    void close() {
        if (FAILED(explorer::drainStaWorkers(5000)))
            unfinishedFailure("STA worker still owns native view/site during browser teardown");
        actions.reset(true); folderView.Reset(); view.Reset(); folder.Reset();
        if (browser) { if (cookie) browser->Unadvise(cookie); browser->Destroy(); browser.Reset(); }
        events.Reset();
        if (owner) { DestroyWindow(owner); owner = nullptr; }
        if (observation) observation->allowFrame(nullptr);
    }
    void initialize(const fs::path& destination) {
        WNDCLASSW definition{}; definition.lpfnWndProc = DefWindowProcW;
        definition.hInstance = GetModuleHandleW(nullptr); definition.lpszClassName = L"WindowsExplorerCITransferOwner";
        require(RegisterClassW(&definition) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "Register private transfer host");
        owner = CreateWindowExW(0, definition.lpszClassName, L"Owned native transfer fixture", WS_OVERLAPPEDWINDOW,
            0, 0, 900, 600, nullptr, nullptr, definition.hInstance, nullptr);
        require(owner != nullptr, "Create private transfer host");
        observation->allowFrame(owner);
        succeeded(CoCreateInstance(CLSID_ExplorerBrowser, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&browser)), "Create transfer native browser");
        succeeded(browser->SetOptions(EBO_NOTRAVELLOG | EBO_NOPERSISTVIEWSTATE), "Suppress transfer navigation/view persistence");
        RECT bounds{0, 0, 900, 600}; FOLDERSETTINGS settings{FVM_DETAILS, FWF_AUTOARRANGE};
        succeeded(browser->Initialize(owner, &bounds, &settings), "Initialize transfer native browser");
        events = Make<Events>(); require(events != nullptr, "Allocate transfer browser events");
        succeeded(browser->Advise(events.Get(), &cookie), "Observe transfer navigation");
        navigate(destination);
        ShowWindow(owner, SW_SHOWNOACTIVATE); SetActiveWindow(owner); UpdateWindow(owner);
        require(IsWindowVisible(owner) && GetActiveWindow() == owner, "Activate exact private transfer frame");
        succeeded(view->UIActivate(SVUIA_ACTIVATE_NOFOCUS), "Activate exact private native transfer view");
        pump();
    }
    void navigate(const fs::path& destination) {
        actions.reset(); folderView.Reset(); view.Reset(); folder = shellItem(destination);
        events->complete = false; events->result = E_PENDING;
        succeeded(browser->BrowseToObject(folder.Get(), SBSP_ABSOLUTE), "Browse fresh owned transfer destination");
        waitFor([&] { return events->complete; }, "Native transfer navigation timeout");
        succeeded(events->result, "Complete native transfer destination navigation");
        succeeded(browser->GetCurrentView(IID_PPV_ARGS(&view)), "Read exact native transfer view");
        succeeded(view.As(&folderView), "Read exact native transfer folder view");
        HWND child = nullptr; succeeded(view->GetWindow(&child), "Read native transfer view HWND");
        DWORD process = 0;
        require(child && IsChild(owner, child) && GetWindowThreadProcessId(child, &process) == GetCurrentThreadId() &&
            process == GetCurrentProcessId(), "Native transfer view must belong to its exact owner STA");
        waitFor([&] { int count = -1; return SUCCEEDED(folderView->ItemCount(SVGIO_ALLVIEW, &count)) && !count; },
            "Fresh native transfer destination is not empty");
        explorer::NamespaceTarget target; target.folder = folder; target.site = view;
        succeeded(actions.initialize(owner, target), "Attach original native transfer view site");
        if (IsWindowVisible(owner))
            succeeded(view->UIActivate(SVUIA_ACTIVATE_NOFOCUS), "Activate newly navigated private transfer view");
    }
    explorer::NamespaceInvocationPlan plan(const wchar_t* command) {
        succeeded(actions.refresh(), "Refresh actual native transfer command state");
        explorer::NamespaceInvocationPlan result;
        succeeded(actions.planCommandStore(command, &result, explorer::NamespaceMenuScope::Background), "Plan exact native transfer command");
        require(!result.submenu && result.commandId && result.route == explorer::NamespaceInvocationRoute::CommandStoreMenu,
            "Native transfer command must have one exact canonical leaf");
        return result;
    }
    void invoke(const wchar_t* command) {
        require(plan(command).enabled, "Native transfer command is not enabled");
        succeeded(actions.invokeCommandStore(command, false, {}, explorer::NamespaceMenuScope::Background),
            "Invoke normal exact native transfer command");
    }
    std::set<Identity> members() {
        int count = -1; succeeded(folderView->ItemCount(SVGIO_ALLVIEW, &count), "Read native transfer member count");
        require(count >= 0 && count <= 8, "Native transfer membership escaped fixture bound");
        if (!count) return {};
        ComPtr<IShellItemArray> items;
        succeeded(folderView->Items(SVGIO_ALLVIEW, IID_PPV_ARGS(&items)), "Read native transfer complete item array");
        DWORD size = 0; succeeded(items->GetCount(&size), "Read native transfer array count");
        require(size == static_cast<DWORD>(count), "Native transfer count/array mismatch");
        std::set<Identity> result;
        for (DWORD index = 0; index < size; ++index) {
            ComPtr<IShellItem> item; succeeded(items->GetItemAt(index, &item), "Read native transfer member");
            const auto path = itemPath(item.Get());
            require(fs::equivalent(path.parent_path(), itemPath(folder.Get())), "Native transfer member escaped owned destination");
            require(result.insert(identity(path)).second, "Duplicate native transfer identity");
        }
        return result;
    }
};

ComPtr<IShellItemArray> sourceArray(const std::vector<const Source*>& sources) {
    std::vector<Pidl> owned; std::vector<PCIDLIST_ABSOLUTE> native;
    for (const auto source : sources) {
        const auto item = shellItem(source->path); PIDLIST_ABSOLUTE raw = nullptr;
        succeeded(SHGetIDListFromObject(item.Get(), &raw), "Read actual owned source PIDL");
        owned.emplace_back(raw); native.push_back(raw);
    }
    ComPtr<IShellItemArray> result;
    succeeded(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(native.size()), native.data(), &result), "Create actual complete owned source array");
    return result;
}
std::vector<BYTE> globalData(IDataObject* object, const wchar_t* name, bool optional = false) {
    const auto format = RegisterClipboardFormatW(name); require(format != 0, "Register native transfer format");
    FORMATETC request{static_cast<CLIPFORMAT>(format), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    Medium data; const auto result = object->GetData(&request, &data.value);
    if (optional && (result == DV_E_FORMATETC || result == DV_E_TYMED)) return {};
    succeeded(result, "Read native transfer data format");
    require(data.value.tymed == TYMED_HGLOBAL && data.value.hGlobal, "Native transfer format is not a valid global block");
    const auto size = GlobalSize(data.value.hGlobal); require(size && size <= 1024 * 1024, "Native transfer format size bound");
    const auto bytes = static_cast<const BYTE*>(GlobalLock(data.value.hGlobal)); require(bytes != nullptr, "Lock native transfer format");
    std::vector<BYTE> copy(bytes, bytes + size); GlobalUnlock(data.value.hGlobal); return copy;
}
DWORD effectData(IDataObject* object, const wchar_t* format, bool optional = false) {
    const auto bytes = globalData(object, format, optional);
    if (bytes.empty() && optional) return MAXDWORD;
    require(bytes.size() >= sizeof(DWORD), "Native transfer effect block is truncated");
    DWORD result = 0; std::memcpy(&result, bytes.data(), sizeof(result)); return result;
}
Pidl cidaPidl(const std::vector<BYTE>& bytes, UINT offset, size_t header) {
    require(offset >= header && offset < bytes.size() && offset % alignof(USHORT) == 0, "Native CIDA offset bound");
    size_t end = offset;
    for (;;) {
        require(bytes.size() - end >= sizeof(USHORT), "Native CIDA PIDL lacks termination");
        USHORT size = 0; std::memcpy(&size, bytes.data() + end, sizeof(size));
        if (!size) { end += sizeof(size); break; }
        require(size >= sizeof(size) && size <= bytes.size() - end, "Native CIDA item bound"); end += size;
    }
    const auto copy = static_cast<PIDLIST_ABSOLUTE>(CoTaskMemAlloc(end - offset)); require(copy != nullptr, "Copy bounded native CIDA PIDL");
    std::memcpy(copy, bytes.data() + offset, end - offset); return Pidl(copy);
}
void verifyCida(IDataObject* object, const std::vector<const Source*>& expected) {
    const auto bytes = globalData(object, CFSTR_SHELLIDLIST);
    require(bytes.size() >= sizeof(UINT), "Native CIDA header bound");
    UINT count = 0; std::memcpy(&count, bytes.data(), sizeof(count));
    require(count == expected.size() && count && count <= 8, "Native CIDA preserves full source count");
    const size_t header = sizeof(UINT) * (static_cast<size_t>(count) + 2);
    require(bytes.size() >= header, "Native CIDA offset table bound");
    std::vector<UINT> offsets(static_cast<size_t>(count) + 1);
    std::memcpy(offsets.data(), bytes.data() + sizeof(UINT), offsets.size() * sizeof(UINT));
    const auto parent = cidaPidl(bytes, offsets[0], header);
    std::set<Identity> actual, wanted;
    for (const auto source : expected) wanted.insert(source->id);
    require(wanted.size() == expected.size(), "Owned source selection identities are unique");
    for (UINT index = 0; index < count; ++index) {
        const auto child = cidaPidl(bytes, offsets[index + 1], header);
        Pidl combined(ILCombine(parent.get(), child.get())); require(combined != nullptr, "Combine authentic bounded native CIDA item");
        ComPtr<IShellItem> item; succeeded(SHCreateItemFromIDList(combined.get(), IID_PPV_ARGS(&item)), "Read authentic owned native CIDA member");
        require(actual.insert(identity(itemPath(item.Get()))).second, "Native CIDA contains a duplicate identity");
    }
    require(actual == wanted, "Native CIDA must identify only the complete owned selection");
}
bool clipboardEmpty(HWND owner) {
    require(OpenClipboard(owner) != FALSE, "Open clipboard for read-only baseline");
    SetLastError(ERROR_SUCCESS); const UINT format = EnumClipboardFormats(0); const DWORD error = GetLastError();
    const BOOL closed = CloseClipboard();
    require(closed && (!format ? error == ERROR_SUCCESS : true), "Read clipboard format baseline");
    return format == 0;
}
struct Publication {
    HWND owner;
    ComPtr<IDataObject> producer;
    explicit Publication(HWND window) : owner(window) {}
    ~Publication() {
        if (producer) {
            try { clear(); }
            catch (...) { unfinishedFailure("exact owned clipboard cleanup failed while its producer/owner remain alive"); }
        }
    }
    void publish(const std::vector<const Source*>& sources, bool cut) {
        require(clipboardEmpty(owner), "Refuse to replace unrelated clipboard contents");
        const auto selection = sourceArray(sources);
        succeeded(explorer::ShellOperations::copyToClipboard(owner, selection.Get(), cut, &producer), "Publish actual normal Shell copy/cut producer");
        require(producer && OleIsCurrentClipboard(producer.Get()) == S_OK, "Actual producer owns the clipboard");
        verifyCida(producer.Get(), sources);
        const DWORD expectedEffect = cut ? static_cast<DWORD>(DROPEFFECT_MOVE) : static_cast<DWORD>(DROPEFFECT_COPY);
        require(effectData(producer.Get(), CFSTR_PREFERREDDROPEFFECT) == expectedEffect,
            "Normal copy/cut preferred effect differs");
        ComPtr<IDataObject> consumer; succeeded(OleGetClipboard(&consumer), "Read actual clipboard consumer wrapper");
        verifyCida(consumer.Get(), sources);
        require(effectData(consumer.Get(), CFSTR_PREFERREDDROPEFFECT) == expectedEffect,
            "Clipboard consumer effect differs");
    }
    void clear() {
        const auto deadline = GetTickCount64() + 2000;
        HRESULT result = E_PENDING;
        bool empty = false;
        do {
            result = explorer::ShellOperations::clearClipboardIfOwned(producer.Get(), owner);
            if (result == S_OK || result == S_FALSE) {
                try { empty = clipboardEmpty(owner); } catch (...) { empty = false; }
                if (empty) break;
                // The exact producer is no longer current: a different
                // publication must never be cleared to satisfy this fixture.
                if (result == S_FALSE) break;
            }
            if (GetTickCount64() >= deadline) break;
            pump();
        } while (!empty);
        require((result == S_OK || result == S_FALSE) && empty,
            "Exact owned clipboard clear did not reach an empty clipboard; unrelated data was preserved");
        producer.Reset();
    }
    void waitComplete() {
        ComPtr<IDataObjectAsyncCapability> asynchronous;
        if (SUCCEEDED(producer.As(&asynchronous))) {
            waitFor([&] { BOOL active = TRUE; return SUCCEEDED(asynchronous->InOperation(&active)) && !active; },
                "Actual native clipboard transfer did not finish");
        }
    }
};
void waitMembership(Browser& browser, const fs::path& destination, size_t count) {
    waitFor([&] {
        const auto paths = children(destination); if (paths.size() != count) return false;
        std::set<Identity> expected; for (const auto& path : paths) expected.insert(identity(path));
        return expected.size() == count && browser.members() == expected;
    }, "Actual native destination membership did not complete");
}
void verifyCopy(const Source& source, const fs::path& copied) {
    require(readFile(copied) == source.bytes && identity(copied) != source.id && preserved(source),
        "Native copy must preserve source and create one distinct correct file");
}
void verifyMove(const Source& source, const fs::path& moved) {
    require(!fs::exists(source.path) && readFile(moved) == source.bytes && identity(moved) == source.id,
        "Native same-volume move must preserve exact file identity/content");
}
void verifyShortcut(const Source& source, const fs::path& shortcut) {
    require(_wcsicmp(shortcut.extension().c_str(), L".lnk") == 0, "Native Paste shortcut output is not a link");
    ComPtr<IShellLinkW> link; succeeded(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)), "Load native owned shortcut");
    ComPtr<IPersistFile> persistent; succeeded(link.As(&persistent), "Read native shortcut persistence");
    succeeded(persistent->Load(shortcut.c_str(), STGM_READ), "Read actual native shortcut without resolving/launching it");
    PIDLIST_ABSOLUTE raw = nullptr; succeeded(link->GetIDList(&raw), "Read native shortcut target PIDL"); Pidl target(raw);
    require(target != nullptr, "Native shortcut has no actual target");
    ComPtr<IShellItem> item; succeeded(SHCreateItemFromIDList(target.get(), IID_PPV_ARGS(&item)), "Read actual owned shortcut target");
    wchar_t arguments[512]{}; succeeded(link->GetArguments(arguments, 512), "Read actual owned shortcut arguments");
    require(identity(itemPath(item.Get())) == source.id && !arguments[0] && preserved(source),
        "Native shortcut target/source differs from owned original");
}
void drop(Browser& browser, const Source& source, const fs::path& destination, DWORD requested, DWORD keys) {
    const auto selection = sourceArray({&source}); ComPtr<IDataObject> data;
    succeeded(selection->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data)), "Bind original native drop data object");
    verifyCida(data.Get(), {&source});
    explorer::BreadcrumbDropOptions options; options.site = browser.view;
    options.hitTest = [destination = browser.folder](POINTL, IShellItem** result) -> HRESULT {
        if (!result) return E_POINTER; *result = destination.Get(); (*result)->AddRef(); return S_OK;
    };
    // No dependency override: the production adapter binds the actual owned
    // folder's public native IDropTarget and attaches the actual view site.
    ComPtr<explorer::BreadcrumbDropTarget> controller;
    succeeded(explorer::BreadcrumbDropTarget::create(browser.owner, options, &controller), "Create production native breadcrumb drop adapter");
    succeeded(controller->registerWindow(), "Register actual owned private drop target");
    struct Registration {
        explorer::BreadcrumbDropTarget* value;
        ~Registration() { if (value->registered()) value->revokeWindow(); }
    } registered{controller.Get()};
    RECT bounds{}; require(GetClientRect(browser.owner, &bounds) != FALSE, "Read owned drop client bounds");
    POINT point{(bounds.right - bounds.left) / 2, (bounds.bottom - bounds.top) / 2};
    require(ClientToScreen(browser.owner, &point) != FALSE, "Read owned drop screen point");
    const POINTL position{point.x, point.y}; DWORD effect = requested;
    OperationLease operation;
    succeeded(controller->DragEnter(data.Get(), keys, position, &effect), "Forward actual native DragEnter");
    require((effect & (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK)) == requested, "Native DragEnter did not accept requested action");
    effect = requested; succeeded(controller->DragOver(keys, position, &effect), "Forward actual native DragOver");
    require((effect & (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK)) == requested, "Native DragOver changed requested action");
    effect = requested; succeeded(controller->Drop(data.Get(), keys, position, &effect), "Forward actual native Drop");
    const DWORD completedEffect = effect & (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK);
    // An optimized move has already moved the original and can return NONE,
    // avoiding a second source-side deletion. Exact source/output identity is
    // verified after completion instead of inventing a universal MOVE return.
    require(completedEffect == requested || (requested == DROPEFFECT_MOVE && completedEffect == DROPEFFECT_NONE),
        "Actual native Drop returned an unrelated logical action");
    std::cout << "Native Drop requested=" << requested << " returned=" << completedEffect << '\n';
    ComPtr<IDataObjectAsyncCapability> asynchronous;
    if (SUCCEEDED(data.As(&asynchronous))) {
        waitFor([&] { BOOL active = TRUE; return SUCCEEDED(asynchronous->InOperation(&active)) && !active; },
            "Actual native asynchronous Drop did not finish");
    }
    waitMembership(browser, destination, 1);
    const auto output = children(destination).front();
    if (requested == DROPEFFECT_COPY) verifyCopy(source, output);
    else if (requested == DROPEFFECT_MOVE) verifyMove(source, output);
    else verifyShortcut(source, output);
    operation.complete();
    succeeded(controller->revokeWindow(), "Revoke only the owned registered drop target");
}

void run() {
    VisibilityObserver observer; observer.start(); observation = &observer;
    struct ObservationScope { ~ObservationScope() { observation = nullptr; } } scoped;
    Fixture fixture; Browser browser; browser.initialize(fixture.root / L"Copy");
    require(clipboardEmpty(browser.owner), "Fresh disposable transfer VM clipboard must initially be empty");
    require(!browser.plan(L"Windows.undo").enabled && !browser.plan(L"Windows.redo").enabled,
        "Fresh disposable transfer VM must have no pre-existing native history");
    require(!browser.plan(L"Windows.paste").enabled && !browser.plan(L"Windows.pastelink").enabled,
        "Native paste commands must be disabled with empty clipboard");
    {
        Publication clipboard(browser.owner); clipboard.publish({&fixture.sources[0], &fixture.sources[1]}, false);
        OperationLease operation;
        browser.invoke(L"Windows.paste"); clipboard.waitComplete(); waitMembership(browser, fixture.root / L"Copy", 2);
        for (unsigned index = 0; index < 2; ++index)
            verifyCopy(fixture.sources[index], fixture.root / L"Copy" / fixture.sources[index].path.filename());
        fixture.verifySources({}); operation.complete(); clipboard.clear();
        require(!browser.plan(L"Windows.paste").enabled, "Native Paste remained enabled after exact producer cleanup");
        std::cout << "PASS native Copy/Paste: complete two-item CIDA, COPY effect, content/new IDs, source and view membership\n";
    }
    browser.navigate(fixture.root / L"Move");
    {
        Publication clipboard(browser.owner); clipboard.publish({&fixture.sources[2]}, true);
        OperationLease operation;
        browser.invoke(L"Windows.paste"); clipboard.waitComplete(); waitMembership(browser, fixture.root / L"Move", 1);
        waitFor([&] { return !fs::exists(fixture.sources[2].path); }, "Native Cut/Paste source removal did not complete");
        verifyMove(fixture.sources[2], children(fixture.root / L"Move").front()); fixture.verifySources({2});
        // Optimized same-volume moves can report PERFORMEDDROPEFFECT_NONE.
        // A supported logical acknowledgment must still identify MOVE.
        const auto logical = effectData(clipboard.producer.Get(), CFSTR_LOGICALPERFORMEDDROPEFFECT, true);
        require(logical == MAXDWORD || logical == DROPEFFECT_MOVE, "Supported logical native cut acknowledgment differs");
        std::cout << "Native cut logical acknowledgment supported=" << (logical != MAXDWORD) << '\n';
        operation.complete(); clipboard.clear();
        std::cout << "PASS native Cut/Paste: MOVE preference, same-volume exact ID/content, source removal and native view\n";
    }
    browser.navigate(fixture.root / L"Shortcut");
    {
        Publication clipboard(browser.owner); clipboard.publish({&fixture.sources[3]}, false);
        OperationLease operation;
        browser.invoke(L"Windows.pastelink"); clipboard.waitComplete(); waitMembership(browser, fixture.root / L"Shortcut", 1);
        verifyShortcut(fixture.sources[3], children(fixture.root / L"Shortcut").front()); fixture.verifySources({2}); operation.complete(); clipboard.clear();
        std::cout << "PASS native Copy/Paste shortcut: one actual IShellLink target, source identity/content and native view\n";
    }
    const std::array<const wchar_t*, 3> directories{L"DropCopy", L"DropMove", L"DropLink"};
    const std::array<DWORD, 3> effects{DROPEFFECT_COPY, DROPEFFECT_MOVE, DROPEFFECT_LINK};
    const std::array<DWORD, 3> keys{MK_CONTROL, MK_SHIFT, MK_CONTROL | MK_SHIFT};
    for (unsigned index = 0; index < 3; ++index) {
        const auto destination = fixture.root / directories[index]; browser.navigate(destination);
        drop(browser, fixture.sources[index + 4], destination, effects[index], keys[index]);
        const auto output = children(destination).front();
        if (!index) verifyCopy(fixture.sources[4], output);
        else if (index == 1) verifyMove(fixture.sources[5], output);
        else verifyShortcut(fixture.sources[6], output);
        fixture.verifySources(index ? std::set<unsigned>{2, 5} : std::set<unsigned>{2});
        require(clipboardEmpty(browser.owner), "Direct native drop unexpectedly changed the clipboard");
        std::cout << "PASS production breadcrumb native IDropTarget protocol action=" << effects[index] << '\n';
    }
    pump(); require(!observer.unexpected(), "Transfer observer saw unexpected visible UI");
    succeeded(explorer::drainStaWorkers(5000), "Drain native transfer STA workers before view teardown");
    require(clipboardEmpty(browser.owner), "Native transfer must leave the owned clipboard empty");
    browser.close(); fixture.cleanup();
    pump(); observer.stop(); observer.reportEnumeration();
    require(observer.inputObservations() > 0 && observer.privateObservations() > 0,
        "Native transfer visibility observer must independently sample both desktops");
    require(!observer.unexpected(), "Native transfer teardown displayed unexpected visible UI");
    succeeded(explorer::PrivateDesktop::current()->verifyIsolation(), "Native transfer teardown changed desktop isolation");
    std::cout << "Native transfer observations input=" << observer.inputObservations()
        << " private=" << observer.privateObservations() << '\n';
    std::cout << "PASS private desktop/input desktop isolation; no device, Bin or associated application invocation\n";
    // Native owned undo records remain confined to this disposable VM. No
    // undocumented global undo reset or foreign clipboard restoration occurs.
}
} // namespace

int main() {
    if (!environmentEquals(L"GITHUB_ACTIONS", L"true") ||
        !environmentEquals(L"WINDOWSEXPLORER_NATIVE_TRANSFER_TEST", L"1")) {
        std::cout << "SKIP: real native clipboard transfers require a fresh disposable GitHub VM and explicit opt-in\n";
        return 77;
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    explorer::PrivateDesktop desktop;
    if (FAILED(desktop.initialize()) || FAILED(desktop.verifyIsolation())) {
        std::cerr << "FAIL: initialize isolated transfer desktop\n"; return 1;
    }
    const auto initialized = OleInitialize(nullptr);
    if (FAILED(initialized)) { std::cerr << "FAIL: initialize isolated transfer STA\n"; return 1; }
    int result = 0;
    try { run(); }
    catch (const std::exception& error) { std::cerr << "FAIL: native transfer: " << error.what() << '\n'; result = 1; }
    catch (...) { std::cerr << "FAIL: native transfer: unknown exception\n"; result = 1; }
    if (FAILED(explorer::drainStaWorkers(5000))) {
        // A native provider can be noncancelable. Do not tear its borrowed STA
        // or desktop down while that provider still holds marshaled interfaces.
        std::cerr << "FAIL: native transfer worker shutdown did not complete\n";
        if (!TerminateProcess(GetCurrentProcess(), 1)) std::_Exit(1);
        std::_Exit(1);
    }
    OleUninitialize();
    if (FAILED(desktop.verifyIsolation())) result = 1;
    return result;
}
