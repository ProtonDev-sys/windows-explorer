#include "explorer/core.hpp"
#include "explorer/headless_visual.hpp"
#include "explorer/ui_direction.hpp"
#include "explorer/worker_sta.hpp"

#include <commctrl.h>
#include <oleacc.h>
#include <propkey.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/implements.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// Isolated compatibility experiment, deliberately not a production layout API.
// SetRect owns browser layout; public GetWindow does not promise independent
// layout ownership. This probe never changes a WINDOWPOS or private child HWND.
// https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-iexplorerbrowser-setrect
// https://learn.microsoft.com/en-us/windows/win32/api/oleidl/nf-oleidl-iolewindow-getwindow
// https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowpos
namespace {
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
using Identity = std::pair<ULONGLONG, std::array<BYTE, 16>>;

HRESULT nativeError() noexcept {
    const auto error = GetLastError();
    return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
}
void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
void succeeded(HRESULT hr, const char* text) {
    if (FAILED(hr)) throw std::runtime_error(std::string(text) + " HRESULT=" + std::to_string(static_cast<ULONG>(hr)));
}
void exact(HRESULT hr, const char* text) { require(hr == S_OK, text); }
[[noreturn]] void unsafeStop(HRESULT hr) noexcept {
    std::cerr << "preview-layout unsafe-teardown HRESULT=" << static_cast<ULONG>(hr) << std::endl;
    if (!TerminateProcess(GetCurrentProcess(), 11)) std::_Exit(11);
    std::_Exit(11);
}
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct PidlDelete {
    using pointer = PIDLIST_ABSOLUTE;
    void operator()(PIDLIST_ABSOLUTE value) const noexcept { CoTaskMemFree(value); }
};
using Pidl = std::unique_ptr<ITEMIDLIST, PidlDelete>;
Identity identity(const FILE_ID_INFO& native) {
    Identity result{native.VolumeSerialNumber, {}};
    std::copy(std::begin(native.FileId.Identifier), std::end(native.FileId.Identifier), result.second.begin());
    return result;
}
struct NativeSourceSnapshot {
    Identity id{};
    DWORD attributes = 0;
    LONGLONG creation = 0, write = 0, change = 0, size = 0;
    std::vector<BYTE> bytes;
    bool operator==(const NativeSourceSnapshot&) const = default;
};
NativeSourceSnapshot sourceSnapshot(const fs::path& path, bool directory) {
    Handle file{CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | (directory ? 0 : GENERIC_READ),
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr)};
    require(file.value != INVALID_HANDLE_VALUE, "Open exact owned source snapshot");
    FILE_ID_INFO id{}; FILE_BASIC_INFO basic{}; FILE_STANDARD_INFO standard{};
    require(GetFileInformationByHandleEx(file.value, FileIdInfo, &id, sizeof(id)) &&
        GetFileInformationByHandleEx(file.value, FileBasicInfo, &basic, sizeof(basic)) &&
        GetFileInformationByHandleEx(file.value, FileStandardInfo, &standard, sizeof(standard)),
        "Read full owned native identity and metadata");
    require(!(basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        !!(basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == directory, "Owned source changed native object type");
    NativeSourceSnapshot result{identity(id), basic.FileAttributes, basic.CreationTime.QuadPart,
        basic.LastWriteTime.QuadPart, basic.ChangeTime.QuadPart, directory ? 0 : standard.EndOfFile.QuadPart, {}};
    if (!directory) {
        require(result.size >= 0 && result.size <= 128, "Owned source size escaped bound");
        result.bytes.resize(static_cast<size_t>(result.size));
        DWORD read = 0;
        require(ReadFile(file.value, result.bytes.data(), static_cast<DWORD>(result.bytes.size()), &read, nullptr) &&
            read == result.bytes.size(), "Read complete owned source bytes");
    }
    return result;
}
Identity itemIdentity(IShellItem* item) {
    PWSTR raw = nullptr;
    const auto pathRead = item->GetDisplayName(SIGDN_FILESYSPATH, &raw);
    struct Text { PWSTR value; ~Text() { CoTaskMemFree(value); } } text{raw};
    exact(pathRead, "Read native filesystem identity path");
    require(raw != nullptr, "Native filesystem path was null");
    const DWORD attributes = GetFileAttributesW(raw);
    require(attributes != INVALID_FILE_ATTRIBUTES, "Read actual native object attributes");
    Handle file{CreateFileW(raw, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT |
        ((attributes & FILE_ATTRIBUTE_DIRECTORY) ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr)};
    require(file.value != INVALID_HANDLE_VALUE, "Open native item for identity only");
    FILE_ID_INFO id{}; FILE_BASIC_INFO basic{};
    require(GetFileInformationByHandleEx(file.value, FileIdInfo, &id, sizeof(id)) &&
        GetFileInformationByHandleEx(file.value, FileBasicInfo, &basic, sizeof(basic)) &&
        !(basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), "Read exact non-reparse native item identity");
    return identity(id);
}
struct Fixture {
    fs::path root;
    std::array<fs::path, 2> folders, files;
    NativeSourceSnapshot rootBefore;
    std::array<NativeSourceSnapshot, 2> folderBefore, fileBefore;
    bool created = false;
    Fixture() {
      try {
        GUID guid{}; succeeded(CoCreateGuid(&guid), "Create fresh owned fixture identifier");
        wchar_t name[40]{}; require(StringFromGUID2(guid, name, 40) > 0, "Format owned fixture identifier");
        root = fs::temp_directory_path() / (std::wstring(L"WindowsExplorer-PreviewLayout-") + name);
        require(fs::create_directory(root), "Create new owned fixture root");
        created = true;
        for (size_t index = 0; index != folders.size(); ++index) {
            folders[index] = root / (index ? L"Folder-B" : L"Folder-A");
            files[index] = folders[index] / L"Owned.txt";
            require(fs::create_directory(folders[index]), "Create new owned folder");
            Handle file{CreateFileW(files[index].c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
            require(file.value != INVALID_HANDLE_VALUE, "Create owned source without overwrite");
            const std::array<BYTE, 8> bytes{0x4f,0x57,0x4e,0x45,0x44,0x00,static_cast<BYTE>(index),0x0a};
            DWORD written = 0;
            require(WriteFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                written == bytes.size(), "Write exact owned source bytes");
        }
        rootBefore = sourceSnapshot(root, true);
        for (size_t index = 0; index != folders.size(); ++index) {
            folderBefore[index] = sourceSnapshot(folders[index], true);
            fileBefore[index] = sourceSnapshot(files[index], false);
        }
      } catch (...) { cleanup(); throw; }
    }
    void verify() const {
        require(sourceSnapshot(root, true) == rootBefore, "Owned fixture root metadata changed");
        require(std::distance(fs::directory_iterator(root), fs::directory_iterator{}) == 2,
            "Owned root membership changed");
        for (size_t index = 0; index != folders.size(); ++index) {
            require(sourceSnapshot(folders[index], true) == folderBefore[index] &&
                sourceSnapshot(files[index], false) == fileBefore[index], "Owned source identity, metadata or bytes changed");
            require(std::distance(fs::directory_iterator(folders[index]), fs::directory_iterator{}) == 1,
                "Owned source folder membership changed");
        }
    }
    void cleanup() noexcept {
        // Only this exact GUID root was created here; never clean a Shell path.
        if (!created) return;
        std::error_code error; fs::remove_all(root, error);
        if (error) unsafeStop(HRESULT_FROM_WIN32(static_cast<DWORD>(error.value())));
        created = false;
    }
    ~Fixture() { cleanup(); }
};
struct Budget {
    ULONGLONG deadline = GetTickCount64() + 45000;
    void check() const { require(GetTickCount64() < deadline, "Experiment 45-second advisory work deadline expired"); }
    void pump(unsigned milliseconds) const {
        const auto end = std::min(deadline, GetTickCount64() + milliseconds);
        do {
            check(); MSG message{}; unsigned count = 0;
            while (count++ < 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                require(message.message != WM_QUIT, "Unexpected private fixture quit");
                TranslateMessage(&message); DispatchMessageW(&message); check();
            }
            MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        } while (GetTickCount64() < end);
    }
};
bool owned(HWND window, HWND root) noexcept {
    DWORD process = 0;
    return window && IsWindow(window) && GetWindowThreadProcessId(window, &process) == GetCurrentThreadId() &&
        process == GetCurrentProcessId() && (window == root || IsChild(root, window));
}
void isolation(const explorer::PrivateDesktop& desktop) {
    bool unchanged = false, inputVisible = true;
    exact(desktop.verifyIsolation(&unchanged), "Read exact private desktop isolation");
    exact(desktop.visibleWindowsOnInputDesktop(inputVisible), "Read process input desktop visibility");
    require(unchanged && !inputVisible, "Private/input desktop isolation changed");
}
std::string rectangle(const RECT& value) {
    return "[" + std::to_string(value.left) + "," + std::to_string(value.top) + "," +
        std::to_string(value.right) + "," + std::to_string(value.bottom) + "]";
}
std::string identityJson(const Identity& value) {
    std::ostringstream output; output << "{\"volume\":" << value.first << ",\"fileID\":[";
    for (size_t index = 0; index != value.second.size(); ++index) {
        if (index) output << ',';
        output << static_cast<unsigned>(value.second[index]);
    }
    output << "]}"; return output.str();
}
bool sameRect(const RECT& left, const RECT& right) noexcept { return EqualRect(&left, &right) != FALSE; }
RECT screenRect(HWND window) { RECT result{}; require(GetWindowRect(window, &result), "Read owned native screen rectangle"); return result; }

struct PositionObservation {
    struct Entry { UINT message = 0, flags = 0; int x = 0, y = 0, width = 0, height = 0; ULONGLONG tick = 0; };
    HWND window = nullptr;
    std::array<Entry, 128> entries{};
    unsigned count = 0, depth = 0;
    bool overflow = false, destroyed = false;
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                      UINT_PTR identifier, DWORD_PTR reference) {
        auto& state = *reinterpret_cast<PositionObservation*>(reference);
        ++state.depth;
        if ((message == WM_WINDOWPOSCHANGING || message == WM_WINDOWPOSCHANGED) && lparam) {
            if (state.count < state.entries.size()) {
                const auto& position = *reinterpret_cast<const WINDOWPOS*>(lparam);
                state.entries[state.count++] = {message, position.flags, position.x, position.y,
                    position.cx, position.cy, GetTickCount64()};
            } else state.overflow = true;
        }
        if (message == WM_NCDESTROY) {
            state.destroyed = true; RemoveWindowSubclass(window, procedure, identifier); state.window = nullptr;
        }
        // Observation only: the original procedure receives the same WINDOWPOS.
        const auto result = DefSubclassProc(window, message, wparam, lparam);
        --state.depth;
        return result;
    }
    void install(HWND value) {
        require(!window && !depth, "Observer already installed or active");
        require(SetWindowSubclass(value, procedure, reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this)),
            "Install same-STA public view observer");
        window = value;
    }
    void remove() {
        require(!depth, "Public view observer callback still active");
        if (window && IsWindow(window)) require(RemoveWindowSubclass(window, procedure, reinterpret_cast<UINT_PTR>(this)),
            "Remove actual public view observer");
        window = nullptr;
    }
    ~PositionObservation() {
        if (depth) unsafeStop(E_UNEXPECTED);
        if (window && IsWindow(window)) {
            SetLastError(ERROR_SUCCESS);
            if (!RemoveWindowSubclass(window, procedure, reinterpret_cast<UINT_PTR>(this))) unsafeStop(nativeError());
        }
    }
};
class Site final : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                                      IServiceProvider, IExplorerPaneVisibility> {
public:
    HRESULT STDMETHODCALLTYPE QueryService(REFGUID service, REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        return service == SID_ExplorerPaneVisibility ? QueryInterface(iid, result) : E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetPaneState(REFEXPLORERPANE pane, EXPLORERPANESTATE* state) override {
        if (!state) return E_POINTER;
        *state = static_cast<EXPLORERPANESTATE>((pane == EP_NavPane ? EPS_DEFAULT_ON : EPS_DEFAULT_OFF) | EPS_FORCE);
        return S_OK;
    }
};
class Navigation final : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                                            IExplorerBrowserEvents> {
public:
    unsigned version = 0, completions = 0;
    bool complete = false;
    HRESULT status = E_PENDING;
    Pidl location;
    HRESULT STDMETHODCALLTYPE OnNavigationPending(PCIDLIST_ABSOLUTE) override { complete = false; status = E_PENDING; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnViewCreated(IShellView*) override { ++version; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnNavigationComplete(PCIDLIST_ABSOLUTE completedLocation) override {
        location.reset(completedLocation ? ILCloneFull(completedLocation) : nullptr);
        status = location ? S_OK : E_OUTOFMEMORY; complete = true; ++completions; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnNavigationFailed(PCIDLIST_ABSOLUTE) override { status = E_FAIL; complete = true; return S_OK; }
};
struct ViewState {
    Identity folder{};
    std::set<Identity> all, selected;
    DWORD flags = 0;
    FOLDERVIEWMODE mode = FVM_AUTO;
    int iconSize = -1, focusedIndex = -2;
    HRESULT focusedRead = E_PENDING;
    Identity focused{};
    std::vector<SORTCOLUMN> sort;
};
bool equalState(const ViewState& first, const ViewState& second) {
    if (first.folder != second.folder || first.all != second.all || first.selected != second.selected ||
        first.flags != second.flags || first.mode != second.mode || first.iconSize != second.iconSize ||
        first.focusedRead != second.focusedRead || first.focusedIndex != second.focusedIndex ||
        first.focused != second.focused || first.sort.size() != second.sort.size()) return false;
    for (size_t index = 0; index != first.sort.size(); ++index)
        if (!IsEqualPropertyKey(first.sort[index].propkey, second.sort[index].propkey) ||
            first.sort[index].direction != second.sort[index].direction) return false;
    return true;
}
void emitStateComparison(const char* phase,const ViewState& before,const ViewState& after) {
    std::cout << "{\"diagnostic\":\"nativeStateComparison\",\"phase\":\"" << phase
        << "\",\"folderEqual\":" << (before.folder==after.folder)
        << ",\"allEqual\":" << (before.all==after.all) << ",\"selectionEqual\":" << (before.selected==after.selected)
        << ",\"focusedIdentityEqual\":" << (before.focused==after.focused)
        << ",\"beforeFlags\":" << before.flags << ",\"afterFlags\":" << after.flags
        << ",\"beforeMode\":" << static_cast<int>(before.mode) << ",\"afterMode\":" << static_cast<int>(after.mode)
        << ",\"beforeIconSize\":" << before.iconSize << ",\"afterIconSize\":" << after.iconSize
        << ",\"beforeFocusedHR\":" << static_cast<ULONG>(before.focusedRead)
        << ",\"afterFocusedHR\":" << static_cast<ULONG>(after.focusedRead)
        << ",\"beforeFocusedIndex\":" << before.focusedIndex << ",\"afterFocusedIndex\":" << after.focusedIndex
        << ",\"beforeSortCount\":" << before.sort.size() << ",\"afterSortCount\":" << after.sort.size()
        << ",\"exactEqual\":" << equalState(before,after) << '}' << std::endl;
}
struct Browser {
    const explorer::PrivateDesktop& desktop;
    const Budget& budget;
    HWND root = nullptr, placeholder = nullptr, window = nullptr;
    ComPtr<IExplorerBrowser> browser;
    ComPtr<IShellView> view;
    ComPtr<IFolderView2> folder;
    ComPtr<Navigation> events;
    ComPtr<Site> site;
    DWORD cookie = 0;
    unsigned capturedVersion = 0, capturedCompletions = 0;
    ULONGLONG navigationWorkDeadline = 0;
    bool initialized = false, siteAttached = false;
    Pidl capturedLocation;
    explicit Browser(const explorer::PrivateDesktop& guard, const Budget& clock, bool rtl) : desktop(guard), budget(clock) {
      try {
        WNDCLASSW klass{}; klass.lpfnWndProc = DefWindowProcW; klass.hInstance = GetModuleHandleW(nullptr);
        klass.lpszClassName = L"WindowsExplorer.Private.PublicViewLayout";
        require(RegisterClassW(&klass) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "Register owned layout root");
        root = CreateWindowExW(rtl ? WS_EX_LAYOUTRTL : 0, klass.lpszClassName, L"Owned public view layout experiment",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 40, 40, 1100, 740, nullptr, nullptr, klass.hInstance, nullptr);
        require(root != nullptr, "Create owned private layout root");
        placeholder = CreateWindowExW(0, L"STATIC", L"Owned App pane placeholder", WS_CHILD | WS_CLIPSIBLINGS,
            0, 0, 1, 1, root, nullptr, klass.hInstance, nullptr);
        require(placeholder != nullptr, "Create only owned pane placeholder");
        exact(CoCreateInstance(CLSID_ExplorerBrowser, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&browser)), "Create native framed browser");
        site = Microsoft::WRL::Make<Site>(); events = Microsoft::WRL::Make<Navigation>();
        require(site && events, "Create retained native site/events");
        ComPtr<IObjectWithSite> located; exact(browser.As(&located), "Read public browser site");
        exact(located->SetSite(static_cast<IServiceProvider*>(site.Get())), "Attach native pane service");
        siteAttached = true;
        exact(browser->SetOptions(static_cast<EXPLORER_BROWSER_OPTIONS>(EBO_SHOWFRAMES | EBO_NOPERSISTVIEWSTATE | EBO_NOTRAVELLOG)),
            "Keep native frames, prohibit persistent view/history writes");
        RECT bounds{}; require(GetClientRect(root, &bounds), "Read full root client rectangle");
        FOLDERSETTINGS settings{FVM_DETAILS, FWF_AUTOARRANGE};
        exact(browser->Initialize(root, &bounds, &settings), "Initialize full-width native browser");
        initialized = true;
        exact(browser->Advise(events.Get(), &cookie), "Advise retained native navigation");
        ShowWindow(root, SW_SHOWNOACTIVATE); UpdateWindow(root); isolation(desktop);
      } catch (...) { try { finish(); } catch (...) { unsafeStop(E_UNEXPECTED); } throw; }
    }
    bool sourceFence() const noexcept {
        return GetTickCount64() < budget.deadline && owned(root, root) && owned(window, root) && events && events->complete &&
            events->status == S_OK && events->version == capturedVersion && events->location &&
            events->completions == capturedCompletions && capturedLocation &&
            ILGetSize(events->location.get()) == ILGetSize(capturedLocation.get()) &&
            std::memcmp(events->location.get(), capturedLocation.get(), ILGetSize(capturedLocation.get())) == 0 &&
            view && folder && explorer::PrivateDesktop::current() == &desktop;
    }
    void freshFence() const {
        budget.check(); require(sourceFence(), "Native view/window/navigation source changed");
        ComPtr<IUnknown> expected, current; exact(view.As(&expected), "Read retained native view identity");
        exact(browser->GetCurrentView(IID_PPV_ARGS(&current)), "Read actual current native view identity");
        require(sourceFence() && current.Get() == expected.Get(), "Browser replaced the retained view during an observation");
        HWND actual = nullptr; exact(view->GetWindow(&actual), "Read actual public native view window");
        require(sourceFence() && actual == window, "Public native view HWND changed");
        current.Reset(); expected.Reset();
        require(sourceFence(), "Native view identity release changed source"); isolation(desktop);
    }
    void navigate(IShellItem* target, const Identity& expectedFolder, const Identity& expectedFile) {
        budget.check(); view.Reset(); folder.Reset(); window = nullptr;
        events->complete = false; events->status = E_PENDING;
        exact(browser->BrowseToObject(target, SBSP_ABSOLUTE), "Browse only owned source folder");
        const auto end = std::min(budget.deadline, GetTickCount64() + 5000);
        navigationWorkDeadline = end;
        while (!events->complete && GetTickCount64() < end) budget.pump(10);
        exact(events->status, "Owned native navigation did not complete within five seconds");
        exact(browser->GetCurrentView(IID_PPV_ARGS(&view)), "Read current native Shell view");
        exact(view.As(&folder), "Read current native folder interface");
        exact(view->GetWindow(&window), "Read public native Shell view HWND");
        capturedVersion = events->version;
        capturedCompletions = events->completions;
        capturedLocation.reset(ILCloneFull(events->location.get()));
        require(capturedLocation != nullptr, "Retain exact completed native location");
        require(owned(window, root) && window != root, "Public native view is not the exact owned child");
        int count = -1;
        while (GetTickCount64() < end) {
            const auto hr = folder->ItemCount(SVGIO_ALLVIEW, &count);
            if (hr == S_OK && count == 1) break;
            require(sourceFence(), "View changed while native enumeration became ready"); budget.pump(10);
        }
        require(count == 1 && sourceFence(), "Native owned folder did not enumerate its complete one-file source");
        const auto state = snapshot();
        require(state.folder == expectedFolder && state.all == std::set<Identity>{expectedFile}, "Native view escaped full owned folder/file identities");
        ComPtr<IShellItem> completed; exact(SHCreateItemFromIDList(events->location.get(), IID_PPV_ARGS(&completed)), "Read actual completed location");
        require(itemIdentity(completed.Get()) == expectedFolder, "Native completed location differs from owned target"); freshFence();
    }
    std::set<Identity> items(SVGIO scope) const {
        int count = -1; exact(folder->ItemCount(scope, &count), "Read actual native item count");
        require(count >= 0 && count <= 1 && sourceFence(), "Native array escaped owned source bound");
        std::set<Identity> result;
        if (!count) return result;
        ComPtr<IShellItemArray> array; exact(folder->Items(scope, IID_PPV_ARGS(&array)), "Read full native array");
        DWORD total = 0; exact(array->GetCount(&total), "Read full array count");
        require(total == static_cast<DWORD>(count) && sourceFence(), "Native array count changed");
        for (DWORD index = 0; index < total; ++index) {
            ComPtr<IShellItem> item; exact(array->GetItemAt(index, &item), "Read actual native array member");
            require(result.insert(itemIdentity(item.Get())).second && sourceFence(), "Native array identity changed or repeated");
        }
        return result;
    }
    ViewState snapshot() const {
        freshFence(); ViewState state;
        ComPtr<IShellItem> location; exact(folder->GetFolder(IID_PPV_ARGS(&location)), "Read actual native folder object");
        state.folder = itemIdentity(location.Get()); state.all = items(SVGIO_ALLVIEW); state.selected = items(SVGIO_SELECTION);
        exact(folder->GetCurrentFolderFlags(&state.flags), "Read native folder flags");
        exact(folder->GetViewModeAndIconSize(&state.mode, &state.iconSize), "Read native view mode and icon size");
        int columns = -1; exact(folder->GetSortColumnCount(&columns), "Read complete native sort count");
        require(columns >= 0 && columns <= 32, "Native sort count escaped bound"); state.sort.resize(static_cast<size_t>(columns));
        if (columns) exact(folder->GetSortColumns(state.sort.data(), columns), "Read every native sort column");
        state.focusedRead = folder->GetFocusedItem(&state.focusedIndex);
        require((state.focusedRead == S_OK && state.focusedIndex == 0) ||
            (state.focusedRead == S_FALSE && state.focusedIndex == -1), "Native focused item output is unavailable or malformed");
        if (state.focusedRead == S_OK) {
            ComPtr<IShellItem> focused; exact(folder->GetItem(state.focusedIndex, IID_PPV_ARGS(&focused)), "Read exact native focused item");
            state.focused = itemIdentity(focused.Get()); require(state.all.contains(state.focused), "Native focused item escaped source");
        }
        freshFence(); return state;
    }
    void finish() {
        folder.Reset(); view.Reset(); window = nullptr;
        if (browser) {
            if (cookie) { succeeded(browser->Unadvise(cookie), "Unadvise retained actual navigation"); cookie = 0; }
            if (initialized) { const auto destroyed = browser->Destroy(); if (FAILED(destroyed)) unsafeStop(destroyed); initialized = false; }
            ComPtr<IObjectWithSite> located;
            if (siteAttached) {
                exact(browser.As(&located), "Retain actual browser site for teardown");
                exact(located->SetSite(nullptr), "Detach actual retained browser site"); siteAttached = false;
            }
        }
        browser.Reset(); events.Reset(); site.Reset();
        if (root) { require(DestroyWindow(root), "Destroy exact owned root after browser teardown"); root = nullptr; placeholder = nullptr; }
        isolation(desktop);
    }
    ~Browser() { try { finish(); } catch (...) { unsafeStop(E_UNEXPECTED); } }
};

struct FooterObservation {
    HRESULT service = E_PENDING, status = E_PENDING, accessible = E_PENDING;
    HWND statusWindow = nullptr, accessibleWindow = nullptr;
    unsigned nodes = 0, candidates = 0, rejected = 0;
    bool complete = true, found = false;
    RECT bounds{};
};
void walkFooter(Browser& host, IAccessible* object, const VARIANT& child, unsigned depth, FooterObservation& result) {
    host.budget.check(); require(host.sourceFence(), "Footer traversal lost exact view source");
    if (depth > 12 || result.nodes >= 256) { result.complete = false; return; }
    ++result.nodes;
    HWND native = nullptr;
    const auto windowRead = WindowFromAccessibleObject(object, &native);
    require(host.sourceFence(), "Footer provider window lookup changed source");
    if (windowRead != S_OK || !owned(native, host.root)) { ++result.rejected; result.complete = false; return; }
    VARIANT role{}; VariantInit(&role);
    struct Role { VARIANT& value; ~Role() { VariantClear(&value); } } releaseRole{role};
    const auto roleRead = object->get_accRole(child, &role);
    require(host.sourceFence(), "Footer role lookup changed source");
    if (roleRead != S_OK || role.vt != VT_I4) { result.complete = false; return; }
    const auto nativeRole = role.lVal;
    if (nativeRole == ROLE_SYSTEM_STATUSBAR) {
        long x = 0, y = 0, width = 0, height = 0;
        const auto read = object->accLocation(&x, &y, &width, &height, child);
        require(host.sourceFence(), "Footer location lookup changed source");
        ++result.candidates;
        if (read == S_OK && width > 0 && height > 0 &&
            static_cast<LONGLONG>(x) + width <= std::numeric_limits<LONG>::max() &&
            static_cast<LONGLONG>(y) + height <= std::numeric_limits<LONG>::max()) {
            const RECT bounds{x, y, x + width, y + height};
            const auto frame = screenRect(host.root);
            if (bounds.left >= frame.left && bounds.right <= frame.right && bounds.top >= frame.top && bounds.bottom <= frame.bottom) {
                if (!result.found) { result.found = true; result.bounds = bounds; result.accessibleWindow = native; }
                else if (!sameRect(result.bounds, bounds)) result.complete = false;
            }
        }
        return;
    }
    // Do not enumerate file contents, names, cell text or navigation tree items.
    if (nativeRole == ROLE_SYSTEM_LIST || nativeRole == ROLE_SYSTEM_LISTITEM || nativeRole == ROLE_SYSTEM_OUTLINE ||
        nativeRole == ROLE_SYSTEM_OUTLINEITEM || nativeRole == ROLE_SYSTEM_CELL || child.vt != VT_I4 || child.lVal != CHILDID_SELF) return;
    long count = 0; const auto countRead = object->get_accChildCount(&count);
    require(host.sourceFence(), "Footer child count lookup changed source");
    if (countRead != S_OK || count < 0) { result.complete = false; return; }
    if (!count) return;
    if (count > 128) { result.complete = false; return; }
    std::array<VARIANT, 128> children{};
    struct Children { std::array<VARIANT,128>& values; ~Children() { for (auto& value : values) VariantClear(&value); } } release{children};
    long returned = 0;
    const auto childrenRead = AccessibleChildren(object, 0, count, children.data(), &returned);
    require(host.sourceFence(), "Footer child enumeration changed source");
    if (FAILED(childrenRead) || returned < 0 || returned > count) { result.complete = false; return; }
    if (returned != count) result.complete = false;
    for (long index = 0; index != returned; ++index) {
        auto& value = children[static_cast<size_t>(index)];
        if (value.vt == VT_DISPATCH && value.pdispVal) {
            ComPtr<IAccessible> descendant;
            const auto read = value.pdispVal->QueryInterface(IID_PPV_ARGS(&descendant));
            require(host.sourceFence(), "Footer child QI changed source");
            if (read == S_OK) { VARIANT self{}; self.vt = VT_I4; self.lVal = CHILDID_SELF; walkFooter(host, descendant.Get(), self, depth + 1, result); }
        } else if (value.vt == VT_I4) walkFooter(host, object, value, depth + 1, result);
        if (result.nodes >= 256) { result.complete = false; break; }
    }
}
FooterObservation footer(Browser& host) {
    host.freshFence(); FooterObservation result;
    ComPtr<IShellBrowser> frame;
    result.service = IUnknown_QueryService(host.view.Get(), SID_STopLevelBrowser, IID_PPV_ARGS(&frame));
    require(host.sourceFence(), "Footer service query changed source");
    if (result.service == S_OK && frame) {
        result.status = frame->GetControlWindow(FCW_STATUS, &result.statusWindow);
        require(host.sourceFence(), "Footer control query changed source");
        if (result.status == S_OK && owned(result.statusWindow, host.root)) {
            result.bounds = screenRect(result.statusWindow); result.found = true;
        }
    }
    if (!result.found) {
        ComPtr<IAccessible> accessible;
        result.accessible = AccessibleObjectFromWindow(host.root, static_cast<DWORD>(OBJID_CLIENT), IID_PPV_ARGS(&accessible));
        require(host.sourceFence(), "Owned root MSAA lookup changed source");
        if (result.accessible == S_OK && accessible) {
            VARIANT self{}; self.vt = VT_I4; self.lVal = CHILDID_SELF;
            walkFooter(host, accessible.Get(), self, 0, result);
        }
    }
    frame.Reset(); host.freshFence(); return result;
}
RECT clientScreenRect(HWND window) {
    RECT client{}, screen{};
    require(GetClientRect(window, &client), "Read actual owned window client bounds");
    exact(explorer::mapUiRect(window, nullptr, client, &screen), "Map actual owned client bounds to screen");
    return screen;
}
RECT edgeInsets(const RECT& outer, const RECT& inner) noexcept {
    return {inner.left - outer.left, inner.top - outer.top, outer.right - inner.right, outer.bottom - inner.bottom};
}
RECT insetBounds(const RECT& outer, const RECT& insets) noexcept {
    return {outer.left + insets.left, outer.top + insets.top, outer.right - insets.right, outer.bottom - insets.bottom};
}
struct NativeFrameBaseline {
    HWND window = nullptr, footerWindow = nullptr;
    RECT rootOuter{}, rootClient{}, frameOuter{}, frameClient{}, view{};
    RECT frameOuterInsets{}, frameClientInsets{}, viewInsets{}, footerInsets{};
    LONG footerHeight = 0;
    bool footerMatchesFrame = false;
};
NativeFrameBaseline nativeFrameBaseline(Browser& host, const FooterObservation& nativeFooter) {
    host.freshFence(); NativeFrameBaseline result;
    result.window = host.window;
    unsigned ancestors = 0;
    while (GetAncestor(result.window, GA_PARENT) != host.root) {
        require(++ancestors <= 16, "Native frame ancestor chain escaped its fixed bound");
        result.window = GetAncestor(result.window, GA_PARENT);
        require(owned(result.window, host.root) && result.window != host.root,
            "Native frame ancestor escaped the exact owned root");
    }
    require(result.window != host.window && owned(result.window, host.root), "Native view has no independent owned frame ancestor");
    result.rootOuter = screenRect(host.root); result.rootClient = clientScreenRect(host.root);
    result.frameOuter = screenRect(result.window); result.frameClient = clientScreenRect(result.window);
    result.view = screenRect(host.window);
    result.frameOuterInsets = edgeInsets(result.rootClient, result.frameOuter);
    result.frameClientInsets = edgeInsets(result.rootClient, result.frameClient);
    result.viewInsets = edgeInsets(result.rootClient, result.view);
    result.footerInsets = edgeInsets(result.rootClient, nativeFooter.bounds);
    result.footerHeight = nativeFooter.bounds.bottom - nativeFooter.bounds.top;
    // The uncropped native frame is an independent width oracle. Preserve its
    // measured edges exactly; do not accept a pixel tolerance around the root.
    result.footerWindow = nativeFooter.statusWindow ? nativeFooter.statusWindow : nativeFooter.accessibleWindow;
    result.footerMatchesFrame = nativeFooter.found && nativeFooter.complete && owned(result.footerWindow, host.root) &&
        (result.footerWindow == result.window || IsChild(result.window, result.footerWindow)) &&
        nativeFooter.bounds.left == result.frameClient.left && nativeFooter.bounds.right == result.frameClient.right &&
        nativeFooter.bounds.bottom == result.frameClient.bottom && nativeFooter.bounds.top >= result.view.bottom &&
        result.footerHeight > 0;
    host.freshFence(); return result;
}
bool matchesNativeFooter(Browser& host, const NativeFrameBaseline& baseline,
    const RECT& rootClient, const FooterObservation& nativeFooter) {
    require(owned(baseline.window, host.root) && GetAncestor(baseline.window, GA_PARENT) == host.root,
        "Retained native frame identity changed during layout");
    require(IsChild(baseline.window, host.window), "Current public view left its retained native frame");
    const auto frameOuter = screenRect(baseline.window), frameClient = clientScreenRect(baseline.window);
    const auto footerWindow = nativeFooter.statusWindow ? nativeFooter.statusWindow : nativeFooter.accessibleWindow;
    return baseline.footerMatchesFrame && nativeFooter.found && nativeFooter.complete &&
        footerWindow == baseline.footerWindow && owned(footerWindow, host.root) &&
        (footerWindow == baseline.window || IsChild(baseline.window, footerWindow)) &&
        sameRect(edgeInsets(rootClient, frameOuter), baseline.frameOuterInsets) &&
        sameRect(edgeInsets(rootClient, frameClient), baseline.frameClientInsets) &&
        nativeFooter.bounds.left == rootClient.left + baseline.footerInsets.left &&
        nativeFooter.bounds.right == rootClient.right - baseline.footerInsets.right &&
        nativeFooter.bounds.bottom == rootClient.bottom - baseline.footerInsets.bottom &&
        nativeFooter.bounds.bottom - nativeFooter.bounds.top == baseline.footerHeight &&
        nativeFooter.bounds.left == frameClient.left && nativeFooter.bounds.right == frameClient.right &&
        nativeFooter.bounds.bottom == frameClient.bottom;
}
HRESULT cropPublicView(Browser& host, const RECT& target, int reserve, bool rtl) {
    host.freshFence();
    const auto parent = GetParent(host.window);
    require(owned(parent, host.root), "Public crop parent escaped the exact owned root");
    RECT targetParent{};
    exact(explorer::mapUiRect(nullptr, parent, target, &targetParent), "Map one reapplied public view crop");
    SetLastError(ERROR_SUCCESS);
    const auto moved = SetWindowPos(host.window, nullptr, targetParent.left, targetParent.top,
        targetParent.right - targetParent.left, targetParent.bottom - targetParent.top,
        SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    const auto result = moved ? S_OK : nativeError();
    exact(result, "Reapply one crop only to the exact public view HWND"); host.freshFence();
    RECT placeholder{rtl ? target.left - reserve : target.right, target.top,
        rtl ? target.left : target.right + reserve, target.bottom};
    RECT placeholderClient{};
    exact(explorer::mapUiRect(nullptr, host.root, placeholder, &placeholderClient), "Map the owned placeholder after public crop");
    require(SetWindowPos(host.placeholder, HWND_TOP, placeholderClient.left, placeholderClient.top,
        placeholderClient.right - placeholderClient.left, placeholderClient.bottom - placeholderClient.top,
        SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW), "Reposition only the exact owned placeholder");
    require(sameRect(screenRect(host.placeholder), placeholder), "Actual owned placeholder does not exactly fill the reserved public-view region");
    host.freshFence(); return result;
}
void emitPosition(std::ostream& output, const PositionObservation& observer) {
    output << "[";
    for (unsigned index = 0; index != observer.count; ++index) {
        if (index) output << ',';
        const auto& entry = observer.entries[index];
        output << "{\"message\":" << entry.message << ",\"flags\":" << entry.flags << ",\"x\":" << entry.x
            << ",\"y\":" << entry.y << ",\"width\":" << entry.width << ",\"height\":" << entry.height << ",\"tick\":" << entry.tick << '}';
    }
    output << "]";
}
bool runDirection(const explorer::PrivateDesktop& desktop, const Budget& budget, const Fixture& fixture, bool rtl,
    bool controlledSelection = false, bool reapplyLayout = false) {
    Browser host(desktop, budget, rtl);
    std::array<ComPtr<IShellItem>,2> targets;
    for (size_t index = 0; index != targets.size(); ++index)
        exact(SHCreateItemFromParsingName(fixture.folders[index].c_str(), nullptr, IID_PPV_ARGS(&targets[index])), "Parse actual owned folder");
    bool compatible = true;
    HWND previousWindow = nullptr;
    unsigned previousVersion = 0;
    for (unsigned step = 0; step != 3; ++step) {
        const size_t target = step == 1 ? 1 : 0;
        host.navigate(targets[target].Get(), fixture.folderBefore[target].id, fixture.fileBefore[target].id);
        if (controlledSelection) {
            // The observation-only arm proves initial native focus can settle
            // after navigation. Establish this separate controlled baseline
            // once, before any layout action, and never repair it afterward.
            fixture.verify(); host.freshFence();
            const auto controlledBefore = host.snapshot();
            require(GetTickCount64() < host.navigationWorkDeadline, "Controlled selection exhausted the original navigation work bound");
            ShowWindow(host.root, SW_SHOWNOACTIVATE);
            SetActiveWindow(host.root); UpdateWindow(host.root);
            require(IsWindowVisible(host.root) && GetActiveWindow() == host.root, "Present and activate only the actual private owned root");
            host.freshFence(); fixture.verify();
            const auto controlledActivation = host.view->UIActivate(SVUIA_ACTIVATE_FOCUS);
            exact(controlledActivation, "Activate the retained actual native view before the single owned seed");
            host.freshFence();
            ComPtr<IShellItem> controlledItem;
            exact(host.folder->GetItem(0, IID_PPV_ARGS(&controlledItem)), "Read the one actual native owned item");
            require(controlledItem && itemIdentity(controlledItem.Get()) == fixture.fileBefore[target].id,
                "Controlled seed item escaped its complete owned FileID");
            PITEMID_CHILD controlledRawChild = nullptr;
            const auto controlledChildRead = host.folder->Item(0, &controlledRawChild);
            Pidl controlledChild(controlledRawChild);
            exact(controlledChildRead, "Read the actual relative child PIDL for the one owned seed");
            require(controlledChild && host.sourceFence() && GetTickCount64() < host.navigationWorkDeadline,
                "Controlled seed lost the retained source or original navigation bound");
            const auto controlledSelectionRead = host.view->SelectItem(controlledChild.get(),
                SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_FOCUSED | SVSI_NOTAKEFOCUS);
            exact(controlledSelectionRead, "Select and focus the exact owned file once before the baseline");
            ViewState controlledState;
            bool controlledReady = false;
            while (GetTickCount64() < host.navigationWorkDeadline) {
                host.freshFence(); fixture.verify(); controlledState = host.snapshot();
                controlledReady = controlledState.folder == fixture.folderBefore[target].id &&
                    controlledState.all == std::set<Identity>{fixture.fileBefore[target].id} &&
                    controlledState.selected == std::set<Identity>{fixture.fileBefore[target].id} &&
                    controlledState.focusedRead == S_OK && controlledState.focusedIndex == 0 &&
                    controlledState.focused == fixture.fileBefore[target].id;
                if (controlledReady) break;
                const auto controlledNow = GetTickCount64();
                if (controlledNow >= host.navigationWorkDeadline) break;
                budget.pump(static_cast<unsigned>(std::min<ULONGLONG>(10, host.navigationWorkDeadline - controlledNow)));
            }
            require(controlledReady && GetTickCount64() < host.navigationWorkDeadline,
                "Single owned selection/focus did not produce its actual native receipt within the original bound");
            auto controlledExpected = controlledBefore;
            controlledExpected.selected = {fixture.fileBefore[target].id};
            controlledExpected.focusedRead = S_OK; controlledExpected.focusedIndex = 0; controlledExpected.focused = fixture.fileBefore[target].id;
            require(equalState(controlledExpected, controlledState), "Controlled seed changed unrelated native folder, membership, flags, mode or complete sort state");
            host.freshFence(); fixture.verify();
            std::cout << "{\"diagnostic\":\"controlledSelectionPreflight\",\"rtl\":" << rtl << ",\"step\":" << step
                << ",\"activationHR\":" << static_cast<ULONG>(controlledActivation) << ",\"selectionHR\":" << static_cast<ULONG>(controlledSelectionRead)
                << ",\"actualSelectionCalls\":1,\"navigationWorkDeadline\":" << host.navigationWorkDeadline << ",\"currentTick\":" << GetTickCount64()
                << ",\"beforeFocusedHR\":" << static_cast<ULONG>(controlledBefore.focusedRead) << ",\"beforeFocusedIndex\":" << controlledBefore.focusedIndex
                << ",\"afterFocusedHR\":" << static_cast<ULONG>(controlledState.focusedRead) << ",\"afterFocusedIndex\":" << controlledState.focusedIndex
                << ",\"exactSelectedFocusedFileID\":1,\"selectedCount\":" << controlledState.selected.size()
                << ",\"ownedFileIdentity\":" << identityJson(fixture.fileBefore[target].id) << ",\"unrelatedNativeStatePreserved\":1}" << std::endl;
        }
        fixture.verify(); const auto state = host.snapshot();
        auto beforeFooter = footer(host);
        const auto before = screenRect(host.window); const auto parent = GetParent(host.window);
        require(owned(parent, host.root), "Public view immediate parent escaped owned root");
        bool actualRtl = false;
        exact(explorer::windowUiDirection(host.root, &actualRtl), "Read actual root direction");
        require(actualRtl == rtl, "Owned root direction differs from the declared experiment arm");
        const auto nativeDpi = GetDpiForWindow(host.root);
        require(nativeDpi == 96, "This experiment supports only the observed native 96-DPI window contract");
        NativeFrameBaseline baseline;
        if (reapplyLayout) {
            baseline = nativeFrameBaseline(host, beforeFooter);
            fixture.verify(); host.freshFence();
            std::cout << "{\"diagnostic\":\"uncroppedNativeFrameBaseline\",\"rtl\":" << rtl << ",\"step\":" << step
                << ",\"rootOuter\":" << rectangle(baseline.rootOuter) << ",\"rootClientScreen\":" << rectangle(baseline.rootClient)
                << ",\"frameHWND\":" << reinterpret_cast<UINT_PTR>(baseline.window)
                << ",\"frameOuter\":" << rectangle(baseline.frameOuter) << ",\"frameClientScreen\":" << rectangle(baseline.frameClient)
                << ",\"uncroppedView\":" << rectangle(baseline.view) << ",\"nativeFooter\":" << rectangle(beforeFooter.bounds)
                << ",\"frameOuterInsets\":" << rectangle(baseline.frameOuterInsets) << ",\"frameClientInsets\":" << rectangle(baseline.frameClientInsets)
                << ",\"uncroppedViewInsets\":" << rectangle(baseline.viewInsets) << ",\"nativeFooterInsets\":" << rectangle(baseline.footerInsets)
                << ",\"nativeFooterHeight\":" << baseline.footerHeight
                << ",\"footerAccessibleHWND\":" << reinterpret_cast<UINT_PTR>(beforeFooter.accessibleWindow)
                << ",\"footerMatchesIndependentFrameClient\":" << baseline.footerMatchesFrame << ",\"productionLayoutPromise\":0}" << std::endl;
        }
        RECT targetScreen = before;
        const int reserve = MulDiv(180, static_cast<int>(nativeDpi), 96);
        require(reserve > 0 && before.right - before.left > reserve + 120, "Native view too narrow for bounded experiment");
        if (rtl) targetScreen.left += reserve; else targetScreen.right -= reserve;
        RECT targetParent{}; exact(explorer::mapUiRect(nullptr, parent, targetScreen, &targetParent), "Map one normalized public view rectangle to actual parent");
        PositionObservation observer; observer.install(host.window);
        host.freshFence();
        // Old modes make exactly one crop per view. The separate reapply mode
        // additionally makes one public crop after each ordinary SetRect.
        SetLastError(ERROR_SUCCESS);
        const BOOL moved = SetWindowPos(host.window, nullptr, targetParent.left, targetParent.top,
            targetParent.right - targetParent.left, targetParent.bottom - targetParent.top,
            SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
        const HRESULT movedRead = moved ? S_OK : nativeError();
        succeeded(movedRead, "Resize only the public current view HWND"); host.freshFence();
        const auto immediate = screenRect(host.window);
        RECT placeholderScreen{rtl ? targetScreen.left - reserve : targetScreen.right, targetScreen.top,
            rtl ? targetScreen.left : targetScreen.right + reserve, targetScreen.bottom};
        RECT placeholderClient{}; exact(explorer::mapUiRect(nullptr, host.root, placeholderScreen, &placeholderClient), "Map owned placeholder rectangle");
        require(SetWindowPos(host.placeholder, HWND_TOP, placeholderClient.left, placeholderClient.top,
            placeholderClient.right - placeholderClient.left, placeholderClient.bottom - placeholderClient.top,
            SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW), "Position only the owned App pane placeholder");
        budget.pump(80); host.freshFence();
        const auto afterPump = screenRect(host.window);
        RECT full{}; require(GetClientRect(host.root, &full), "Read unchanged full browser rectangle");
        const auto rectRead = host.browser->SetRect(nullptr, full); exact(rectRead, "Perform ordinary full-width browser SetRect");
        RECT setRectBeforeReapply{};
        HRESULT setRectReapply = E_NOTIMPL;
        if (reapplyLayout) {
            host.freshFence(); fixture.verify(); setRectBeforeReapply = screenRect(host.window);
            setRectReapply = cropPublicView(host, targetScreen, reserve, rtl);
        }
        budget.pump(80); host.freshFence();
        const auto afterSetRect = screenRect(host.window);
        const auto afterFooter = footer(host);
        const bool footerSeparate = beforeFooter.found && beforeFooter.bounds.top >= before.bottom &&
            !(beforeFooter.statusWindow && (beforeFooter.statusWindow == host.window || IsChild(host.window, beforeFooter.statusWindow)));
        RECT rootScreen{}; exact(explorer::mapUiRect(host.root, nullptr, full, &rootScreen), "Read normalized root client screen bounds");
        const bool footerFullWidth = reapplyLayout ? matchesNativeFooter(host, baseline, rootScreen, afterFooter) :
            (afterFooter.found && afterFooter.complete && afterFooter.bounds.left == rootScreen.left && afterFooter.bounds.right == rootScreen.right);
        const auto afterState = host.snapshot();
        const bool preserved = equalState(state, afterState);
        emitStateComparison("publicViewResize",state,afterState);
        require(preserved, "View resize changed native selection/focus/flags/mode/full sort state"); fixture.verify();
        const bool stepCompatible = sameRect(immediate, targetScreen) && sameRect(afterPump, targetScreen) &&
            sameRect(afterSetRect, targetScreen) && footerSeparate && footerFullWidth && !observer.overflow;
        compatible = compatible && stepCompatible;
        std::ostringstream output;
        output << "{\"diagnostic\":\"publicViewLayout\",\"rtl\":" << rtl << ",\"step\":" << step
            << ",\"dpi\":" << GetDpiForWindow(host.root) << ",\"viewVersion\":" << host.capturedVersion
            << ",\"dpiAwareness\":" << static_cast<int>(GetAwarenessFromDpiAwarenessContext(GetWindowDpiAwarenessContext(host.root)))
            << ",\"navigationCompletions\":" << host.events->completions << ",\"moveHR\":" << static_cast<ULONG>(movedRead)
            << ",\"rootHWND\":" << reinterpret_cast<UINT_PTR>(host.root) << ",\"viewHWND\":" << reinterpret_cast<UINT_PTR>(host.window)
            << ",\"parentHWND\":" << reinterpret_cast<UINT_PTR>(parent) << ",\"creatorPID\":" << GetCurrentProcessId()
            << ",\"creatorTID\":" << GetCurrentThreadId() << ",\"actualRTL\":" << actualRtl
            << ",\"actualViewExStyle\":" << static_cast<ULONG_PTR>(GetWindowLongPtrW(host.window,GWL_EXSTYLE))
            << ",\"previousViewHWND\":" << reinterpret_cast<UINT_PTR>(previousWindow)
            << ",\"freshViewCreationObserved\":" << (host.capturedVersion > previousVersion)
            << ",\"setRectHR\":" << static_cast<ULONG>(rectRead) << ",\"statusHR\":" << static_cast<ULONG>(afterFooter.status)
            << ",\"frameServiceHR\":" << static_cast<ULONG>(afterFooter.service)
            << ",\"statusWindow\":" << reinterpret_cast<UINT_PTR>(afterFooter.statusWindow)
            << ",\"msaaHR\":" << static_cast<ULONG>(afterFooter.accessible) << ",\"footerNodes\":" << afterFooter.nodes
            << ",\"footerCandidates\":" << afterFooter.candidates << ",\"footerRejected\":" << afterFooter.rejected
            << ",\"footerComplete\":" << afterFooter.complete << ",\"footerMeasured\":" << afterFooter.found
            << ",\"footerSeparate\":" << footerSeparate << ",\"footerFullWidth\":" << footerFullWidth
            << ",\"before\":" << rectangle(before) << ",\"requested\":" << rectangle(targetScreen)
            << ",\"immediate\":" << rectangle(immediate) << ",\"afterPump\":" << rectangle(afterPump)
            << ",\"afterSetRect\":" << rectangle(afterSetRect) << ",\"footer\":" << rectangle(afterFooter.bounds)
            << ",\"nativeOverwrite\":" << !sameRect(afterSetRect, targetScreen) << ",\"statePreserved\":" << preserved
            << ",\"observerOverflow\":" << observer.overflow << ",\"layoutCompatible\":" << stepCompatible << ",\"positionMessages\":";
        emitPosition(output, observer);
        if (reapplyLayout) output << ",\"reapplyLayout\":1,\"footerBasis\":\"uncroppedNativeFrameClient\",\"rootClientScreen\":" << rectangle(rootScreen)
            << ",\"beforeSetRectReapply\":" << rectangle(setRectBeforeReapply) << ",\"setRectReapplyHR\":" << static_cast<ULONG>(setRectReapply)
            << ",\"actualSetRectReapplyCalls\":1,\"nativeFrameOuter\":" << rectangle(screenRect(baseline.window))
            << ",\"nativeFrameClientScreen\":" << rectangle(clientScreenRect(baseline.window));
        output << ",\"folderIdentity\":" << identityJson(state.folder) << ",\"fileIdentity\":" << identityJson(fixture.fileBefore[target].id)
            << ",\"allCount\":" << state.all.size() << ",\"selectedCount\":" << state.selected.size()
            << ",\"focusedHR\":" << static_cast<ULONG>(state.focusedRead) << ",\"focusedIndex\":" << state.focusedIndex
            << ",\"folderFlags\":" << state.flags << ",\"viewMode\":" << static_cast<int>(state.mode)
            << ",\"iconSize\":" << state.iconSize << ",\"sortCount\":" << state.sort.size() << '}';
        std::cout << output.str() << std::endl;
        // Old modes retain the no-correction resize control. Reapply mode makes
        // exactly one further public crop after native full-frame layout.
        require(SetWindowPos(host.root, nullptr, 0, 0, step == 1 ? 1040 : 1140, step == 1 ? 720 : 760,
            SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER), "Resize only the owned root");
        require(GetClientRect(host.root, &full), "Read resized full root bounds");
        exact(host.browser->SetRect(nullptr, full), "Lay out full-width browser after owned root resize");
        RECT resizedBeforeReapply{}, resizedReapplyTarget{}, resizedReapplyRoot{};
        HRESULT resizedReapply = E_NOTIMPL;
        if (reapplyLayout) {
            host.freshFence(); fixture.verify();
            resizedBeforeReapply = screenRect(host.window); resizedReapplyRoot = clientScreenRect(host.root);
            resizedReapplyTarget = insetBounds(resizedReapplyRoot, baseline.viewInsets);
            if (rtl) resizedReapplyTarget.left += reserve; else resizedReapplyTarget.right -= reserve;
            require(resizedReapplyTarget.right - resizedReapplyTarget.left >= 120 &&
                resizedReapplyTarget.bottom > resizedReapplyTarget.top, "Reapplied public crop escaped its positive bounded size");
            resizedReapply = cropPublicView(host, resizedReapplyTarget, reserve, rtl);
        }
        budget.pump(80); host.freshFence();
        const auto resizedState = host.snapshot();
        emitStateComparison("ownedRootResize",state,resizedState);
        require(equalState(state, resizedState), "Ordinary root resize changed native view state"); fixture.verify();
        const auto resizedFooter = footer(host);
        RECT resizedRootScreen{}; exact(explorer::mapUiRect(host.root, nullptr, full, &resizedRootScreen), "Map resized full root screen bounds");
        RECT resizedTarget{targetScreen.left + resizedRootScreen.left - rootScreen.left,
            targetScreen.top + resizedRootScreen.top - rootScreen.top,
            targetScreen.right + resizedRootScreen.right - rootScreen.right,
            targetScreen.bottom + resizedRootScreen.bottom - rootScreen.bottom};
        const auto resizedView = screenRect(host.window);
        const bool resizedFooterFullWidth = reapplyLayout ? matchesNativeFooter(host, baseline, resizedRootScreen, resizedFooter) :
            (resizedFooter.found && resizedFooter.complete && resizedFooter.bounds.left == resizedRootScreen.left && resizedFooter.bounds.right == resizedRootScreen.right);
        const bool resizedCompatible = sameRect(resizedView, resizedTarget) && resizedFooterFullWidth && !observer.overflow;
        compatible = compatible && resizedCompatible;
        std::cout << "{\"diagnostic\":\"ownedRootResize\",\"rtl\":" << rtl << ",\"step\":" << step
            << ",\"view\":" << rectangle(resizedView) << ",\"expectedReservedView\":" << rectangle(resizedTarget)
            << ",\"statusHR\":" << static_cast<ULONG>(resizedFooter.status)
            << ",\"msaaHR\":" << static_cast<ULONG>(resizedFooter.accessible) << ",\"footerMeasured\":" << resizedFooter.found
            << ",\"footer\":" << rectangle(resizedFooter.bounds) << ",\"nativeStatePreserved\":1,\"observerOverflow\":" << observer.overflow
            << ",\"layoutCompatible\":" << resizedCompatible;
        if (reapplyLayout) std::cout << ",\"reapplyLayout\":1,\"footerBasis\":\"uncroppedNativeFrameClient\",\"rootClientScreen\":" << rectangle(resizedRootScreen)
            << ",\"beforeResizeReapply\":" << rectangle(resizedBeforeReapply) << ",\"resizeReapplyTarget\":" << rectangle(resizedReapplyTarget)
            << ",\"resizeReapplyHR\":" << static_cast<ULONG>(resizedReapply) << ",\"actualResizeReapplyCalls\":1"
            << ",\"footerMatchesMeasuredInsets\":" << resizedFooterFullWidth << ",\"nativeFrameOuter\":" << rectangle(screenRect(baseline.window))
            << ",\"nativeFrameClientScreen\":" << rectangle(clientScreenRect(baseline.window));
        std::cout << ",\"positionMessages\":";
        emitPosition(std::cout, observer); std::cout << '}' << std::endl;
        observer.remove(); require(!observer.depth, "Observer active after removal");
        previousWindow = host.window; previousVersion = host.capturedVersion;
    }
    targets = {}; host.finish(); fixture.verify(); return compatible;
}
struct ObservationControlResult {
    unsigned completedViews = 0;
    bool stateChanged = false, focusChanged = false, geometryChanged = false;
};
void emitObservationState(std::ostream& output, const ViewState& state) {
    output << "{\"folder\":" << identityJson(state.folder) << ",\"all\":[";
    bool separator = false;
    for (const auto& item : state.all) { if (separator) output << ','; output << identityJson(item); separator = true; }
    output << "],\"selection\":["; separator = false;
    for (const auto& item : state.selected) { if (separator) output << ','; output << identityJson(item); separator = true; }
    output << "],\"focusedHR\":" << static_cast<ULONG>(state.focusedRead) << ",\"focusedIndex\":" << state.focusedIndex
        << ",\"focusedIdentity\":" << identityJson(state.focused) << ",\"flags\":" << state.flags
        << ",\"mode\":" << static_cast<int>(state.mode) << ",\"iconSize\":" << state.iconSize << ",\"sort\":[";
    for (size_t index = 0; index < state.sort.size(); ++index) {
        if (index) output << ',';
        const auto& column = state.sort[index];
        const auto* bytes = reinterpret_cast<const BYTE*>(&column.propkey.fmtid);
        output << "{\"propertyFmtidBytes\":[";
        for (size_t byte = 0; byte < sizeof(column.propkey.fmtid); ++byte) { if (byte) output << ','; output << static_cast<unsigned>(bytes[byte]); }
        output << "],\"propertyId\":" << column.propkey.pid << ",\"direction\":" << static_cast<int>(column.direction) << '}';
    }
    output << "]}";
}
ObservationControlResult runObservationDirection(const explorer::PrivateDesktop& desktop, const Budget& budget,
    const Fixture& fixture, bool rtl) {
    Browser host(desktop, budget, rtl);
    std::array<ComPtr<IShellItem>, 2> targets;
    for (size_t index = 0; index < targets.size(); ++index)
        exact(SHCreateItemFromParsingName(fixture.folders[index].c_str(), nullptr, IID_PPV_ARGS(&targets[index])), "Parse only the owned observation-control folder");
    ObservationControlResult result;
    HWND previousWindow = nullptr;
    unsigned previousVersion = 0;
    for (unsigned step = 0; step != 3; ++step) {
        const size_t target = step == 1 ? 1 : 0;
        host.navigate(targets[target].Get(), fixture.folderBefore[target].id, fixture.fileBefore[target].id);
        const auto navigationReturned = GetTickCount64();
        PositionObservation observer;
        observer.install(host.window); // Forward every native message unchanged.
        fixture.verify(); host.freshFence();
        bool actualRtl = false;
        exact(explorer::windowUiDirection(host.root, &actualRtl), "Read exact observation-control direction");
        require(actualRtl == rtl && GetDpiForWindow(host.root) == 96, "Observation control requires its actual declared direction and native 96 DPI");
        const auto initialRoot = screenRect(host.root), initialView = screenRect(host.window);
        ViewState initial, previous;
        FooterObservation footerReceipt;
        ULONGLONG footerTick = 0;
        const auto phase = [&](const char* name, bool readFooter) {
            budget.check(); host.freshFence(); fixture.verify();
            if (readFooter) { footerReceipt = footer(host); footerTick = GetTickCount64(); }
            const auto state = host.snapshot();
            require(state.folder == fixture.folderBefore[target].id && state.all == std::set<Identity>{fixture.fileBefore[target].id},
                "Observation-only control lost its complete owned folder/file source");
            const auto rootBounds = screenRect(host.root), viewBounds = screenRect(host.window);
            host.freshFence(); fixture.verify(); budget.check();
            const bool first = std::strcmp(name, "C0") == 0;
            if (first) { initial = state; previous = state; }
            const bool exactInitial = equalState(initial, state), exactPrevious = equalState(previous, state);
            const bool focusEqual = initial.focusedRead == state.focusedRead && initial.focusedIndex == state.focusedIndex && initial.focused == state.focused;
            const bool geometryEqual = sameRect(initialRoot, rootBounds) && sameRect(initialView, viewBounds);
            result.stateChanged = result.stateChanged || !exactInitial;
            result.focusChanged = result.focusChanged || !focusEqual;
            result.geometryChanged = result.geometryChanged || !geometryEqual;
            require(!observer.overflow && !observer.destroyed && !observer.depth, "Observation-only native window messages escaped their fixed bound or live view");
            std::cout << "{\"diagnostic\":\"observationOnlyControl\",\"phase\":\"" << name << "\",\"rtl\":" << rtl
                << ",\"step\":" << step << ",\"elapsedSinceNavigationMs\":" << GetTickCount64() - navigationReturned
                << ",\"rootHWND\":" << reinterpret_cast<UINT_PTR>(host.root) << ",\"viewHWND\":" << reinterpret_cast<UINT_PTR>(host.window)
                << ",\"previousViewHWND\":" << reinterpret_cast<UINT_PTR>(previousWindow)
                << ",\"freshViewCreationObserved\":" << (host.capturedVersion > previousVersion)
                << ",\"viewVersion\":" << host.capturedVersion << ",\"navigationCompletions\":" << host.capturedCompletions
                << ",\"creatorPID\":" << GetCurrentProcessId() << ",\"creatorTID\":" << GetCurrentThreadId()
                << ",\"dpi\":" << GetDpiForWindow(host.root) << ",\"actualRTL\":" << actualRtl
                << ",\"rootBounds\":" << rectangle(rootBounds) << ",\"viewBounds\":" << rectangle(viewBounds)
                << ",\"sameInitialState\":" << exactInitial << ",\"samePreviousState\":" << exactPrevious
                << ",\"focusedIdentityEqual\":" << focusEqual << ",\"geometryEqual\":" << geometryEqual
                << ",\"sourcePreserved\":1,\"footerReadThisPhase\":" << readFooter << ",\"footerSampleTick\":" << footerTick
                << ",\"statusHR\":" << static_cast<ULONG>(footerReceipt.status) << ",\"frameServiceHR\":" << static_cast<ULONG>(footerReceipt.service)
                << ",\"msaaHR\":" << static_cast<ULONG>(footerReceipt.accessible) << ",\"footerMeasured\":" << footerReceipt.found
                << ",\"footerComplete\":" << footerReceipt.complete << ",\"footerBounds\":" << rectangle(footerReceipt.bounds)
                << ",\"nativeState\":";
            emitObservationState(std::cout, state);
            std::cout << ",\"positionMessages\":"; emitPosition(std::cout, observer);
            std::cout << ",\"explicitLayoutCalls\":0,\"explicitFocusSelectionCalls\":0,\"productionLayoutPromise\":0}" << std::endl;
            previous = state;
        };
        // No SetWindowPos, SetRect, activation or selection is performed in
        // this arm. Native messages and read-only provider calls can still
        // complete initialization; report their effects without accepting a
        // layout/preservation claim from an execution-only success result.
        phase("C0", false);
        phase("C1", true);
        budget.pump(80); phase("C2", false);
        budget.pump(80); phase("C3", true);
        observer.remove(); host.freshFence(); fixture.verify();
        ++result.completedViews;
        previousWindow = host.window; previousVersion = host.capturedVersion;
    }
    targets = {}; host.finish(); fixture.verify(); return result;
}
} // namespace

int main(int argc, char** argv) {
    const bool observationOnly = argc == 2 && argv && argv[1] && std::strcmp(argv[1], "--observation-only") == 0;
    const bool controlledSelection = argc == 2 && argv && argv[1] && std::strcmp(argv[1], "--controlled-selection") == 0;
    const bool reapplyLayout = argc == 2 && argv && argv[1] && std::strcmp(argv[1], "--reapply-layout") == 0;
    if (argc != 1 && !observationOnly && !controlledSelection && !reapplyLayout) return 2;
    std::cout << std::unitbuf; std::cerr << std::unitbuf;
    // No COM, common controls or HWND may precede this attachment.
    explorer::PrivateDesktop desktop;
    const auto attached = desktop.initialize();
    if (FAILED(attached)) { std::cerr << "preview-layout desktop HRESULT=" << static_cast<ULONG>(attached) << '\n'; return 1; }
    const Budget budget;
    const auto apartment = OleInitialize(nullptr);
    if (FAILED(apartment)) return 1;
    int exitCode = 1;
    const DWORD clipboardSequence = GetClipboardSequenceNumber();
    try {
        isolation(desktop);
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
        require(InitCommonControlsEx(&controls), "Initialize owned experiment common controls");
        {
            Fixture fixture;
            if (observationOnly) {
                const auto ltr = runObservationDirection(desktop, budget, fixture, false);
                const auto rtl = runObservationDirection(desktop, budget, fixture, true);
                fixture.verify(); isolation(desktop); budget.check();
                require(GetClipboardSequenceNumber() == clipboardSequence, "Clipboard sequence changed during observation-only control");
                require(ltr.completedViews == 3 && rtl.completedViews == 3, "Observation-only control did not complete both A/B/A arms");
                std::cout << "{\"observationControlExecuted\":1,\"observationControlCompleted\":1,\"ownedSourcePreserved\":1,\"nativeStateChanged\":"
                    << (ltr.stateChanged || rtl.stateChanged) << ",\"nativeFocusChanged\":" << (ltr.focusChanged || rtl.focusChanged)
                    << ",\"nativeGeometryChanged\":" << (ltr.geometryChanged || rtl.geometryChanged)
                    << ",\"ltrCompletedViews\":" << ltr.completedViews << ",\"rtlCompletedViews\":" << rtl.completedViews
                    << ",\"layoutCompatible\":null,\"advisoryWorkBudgetMs\":45000,\"externalProcessBoundRequired\":1,\"productionLayoutPromise\":0}" << std::endl;
            } else {
                const bool ltr = runDirection(desktop, budget, fixture, false, controlledSelection || reapplyLayout, reapplyLayout);
                const bool rtl = runDirection(desktop, budget, fixture, true, controlledSelection || reapplyLayout, reapplyLayout);
                fixture.verify(); isolation(desktop); budget.check();
                require(GetClipboardSequenceNumber() == clipboardSequence, "Clipboard sequence changed during read-only layout experiment");
                std::cout << "{\"experimentExecuted\":1,\"ownedSourcePreserved\":1,\"layoutCompatible\":" << (ltr && rtl)
                    << ",\"ltrCompatible\":" << ltr << ",\"rtlCompatible\":" << rtl;
                if (controlledSelection || reapplyLayout) std::cout << ",\"controlledSelection\":1,\"actualPrebaselineSelectionCalls\":6,\"postbaselineSelectionCalls\":0";
                if (reapplyLayout) std::cout << ",\"reapplyLayout\":1,\"actualPostSetRectPublicCropCalls\":12,\"footerBasis\":\"uncroppedNativeFrameClient\"";
                std::cout << ",\"advisoryWorkBudgetMs\":45000,\"externalProcessBoundRequired\":1,\"productionLayoutPromise\":0}" << std::endl;
                if (controlledSelection || reapplyLayout) require(ltr && rtl, "Controlled-selection public view and full-width native footer layout invariants failed");
            }
        }
        exitCode = 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL preview-layout experiment: " << error.what() << std::endl;
    } catch (...) { std::cerr << "FAIL preview-layout experiment: unknown exception" << std::endl; }
    const auto drained = explorer::drainStaWorkers(5000);
    if (FAILED(drained)) unsafeStop(drained);
    try { isolation(desktop); } catch (...) { exitCode = 1; }
    OleUninitialize();
    return exitCode;
}
