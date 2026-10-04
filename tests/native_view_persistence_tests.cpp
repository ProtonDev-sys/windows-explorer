#include "explorer/headless_visual.hpp"

#include <shlobj.h>
#include <propkey.h>
#include <wrl/implements.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <vector>

// Native view persistence changes the shared per-user Shell view-state store.
// Both gates are checked before COM, desktops, windows, or fixture creation.
// Never enable this executable on a user's PC, even on a private desktop.
namespace {
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;
namespace fs = std::filesystem;
constexpr unsigned memberCount = 8;
constexpr wchar_t bagName[] = L"WindowsExplorer.Native";

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void succeeded(HRESULT result, const char* message) {
    if (FAILED(result)) {
        std::cerr << "HRESULT=0x" << std::hex << static_cast<ULONG>(result) << std::dec << '\n';
        throw std::runtime_error(message);
    }
}
bool environmentEquals(const wchar_t* name, const wchar_t* expected) {
    wchar_t value[32]{};
    const DWORD length = GetEnvironmentVariableW(name, value, 32);
    return length && length < 32 && std::wcscmp(value, expected) == 0;
}
void pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}
void waitFor(const std::function<bool()>& ready, const char* message, DWORD timeout = 10000) {
    const ULONGLONG deadline = GetTickCount64() + timeout;
    while (!ready()) {
        require(GetTickCount64() < deadline, message);
        MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        pump();
    }
}

class VisibilityObserver final {
public:
    HRESULT start() {
        current_.store(this);
        privateHook_ = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW,
            nullptr, &shown, 0, 0, WINEVENT_OUTOFCONTEXT);
        if (!privateHook_) return HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_GEN_FAILURE);
        std::promise<HRESULT> ready;
        auto result = ready.get_future();
        worker_ = std::thread([this, ready = std::move(ready)]() mutable {
            // This fresh worker remains on the process's original desktop.
            // Establish its baseline here, not on the fixture's private desktop.
            EnumWindows([](HWND window, LPARAM argument) -> BOOL {
                if (IsWindowVisible(window)) reinterpret_cast<VisibilityObserver*>(argument)->baseline_.insert(window);
                return TRUE;
            }, reinterpret_cast<LPARAM>(this));
            const HWINEVENTHOOK hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW,
                nullptr, &shown, 0, 0, WINEVENT_OUTOFCONTEXT);
            const DWORD error = hook ? ERROR_SUCCESS : GetLastError();
            ready.set_value(hook ? S_OK : HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE));
            while (!stop_.load()) {
                EnumWindows([](HWND window, LPARAM argument) -> BOOL {
                    reinterpret_cast<VisibilityObserver*>(argument)->inspect(window);
                    return TRUE;
                }, reinterpret_cast<LPARAM>(this));
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
        current_.store(nullptr);
    }
    bool visible() const { return visible_.load(); }
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
        if (process == GetCurrentProcessId() || !baseline_.contains(window)) visible_.store(true);
    }
    inline static std::atomic<VisibilityObserver*> current_{nullptr};
    HWINEVENTHOOK privateHook_ = nullptr;
    std::unordered_set<HWND> baseline_;
    std::atomic_bool visible_ = false;
    std::atomic_bool stop_ = false;
    std::thread worker_;
};

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct FileStamp {
    ULONGLONG volume = 0;
    std::array<BYTE, 16> identity{};
    LONGLONG size = 0, write = 0, change = 0;
    DWORD attributes = 0;
    std::vector<BYTE> bytes;
    auto operator<=>(const FileStamp&) const = default;
};
using FileSnapshot = std::map<std::wstring, FileStamp>;
FileSnapshot snapshot(const fs::path& folder) {
    FileSnapshot result;
    for (const auto& entry : fs::directory_iterator(folder)) {
        require(entry.is_regular_file(), "Owned persistence fixture acquired a non-file member");
        Handle file{CreateFileW(entry.path().c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        require(file.value != INVALID_HANDLE_VALUE, "Read owned persistence fixture member");
        FILE_ID_INFO identity{};
        FILE_STANDARD_INFO standard{};
        FILE_BASIC_INFO basic{};
        require(GetFileInformationByHandleEx(file.value, FileIdInfo, &identity, sizeof(identity)) &&
                GetFileInformationByHandleEx(file.value, FileStandardInfo, &standard, sizeof(standard)) &&
                GetFileInformationByHandleEx(file.value, FileBasicInfo, &basic, sizeof(basic)),
                "Read owned member identity and metadata");
        require(!(basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && standard.EndOfFile.QuadPart > 0 &&
                standard.EndOfFile.QuadPart <= 4096, "Owned persistence fixture member shape changed");
        FileStamp stamp;
        stamp.volume = identity.VolumeSerialNumber;
        std::copy(std::begin(identity.FileId.Identifier), std::end(identity.FileId.Identifier), stamp.identity.begin());
        stamp.size = standard.EndOfFile.QuadPart;
        stamp.write = basic.LastWriteTime.QuadPart;
        stamp.change = basic.ChangeTime.QuadPart;
        stamp.attributes = basic.FileAttributes;
        stamp.bytes.resize(static_cast<size_t>(stamp.size));
        DWORD read = 0;
        require(ReadFile(file.value, stamp.bytes.data(), static_cast<DWORD>(stamp.bytes.size()), &read, nullptr) &&
                read == stamp.bytes.size(), "Read complete owned persistence fixture content");
        require(result.emplace(entry.path().filename().native(), std::move(stamp)).second,
                "Owned persistence fixture acquired duplicate names");
    }
    require(result.size() == memberCount, "Owned persistence fixture count changed");
    return result;
}
struct Fixture {
    fs::path root, first, second;
    std::array<BYTE, 16> rootIdentity{};
    explicit Fixture() {
        GUID identifier{};
        succeeded(CoCreateGuid(&identifier), "Create owned persistence fixture identifier");
        wchar_t text[40]{};
        require(StringFromGUID2(identifier, text, 40) > 0, "Format owned persistence identifier");
        root = fs::temp_directory_path() / (std::wstring(L"WindowsExplorer-ViewPersistence-") + text);
        require(fs::create_directory(root), "Create fresh owned persistence fixture");
        Handle directory{CreateFileW(root.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        FILE_ID_INFO identity{};
        require(directory.value != INVALID_HANDLE_VALUE &&
                GetFileInformationByHandleEx(directory.value, FileIdInfo, &identity, sizeof(identity)),
                "Remember original owned persistence directory identity");
        std::copy(std::begin(identity.FileId.Identifier), std::end(identity.FileId.Identifier), rootIdentity.begin());
        first = root / L"Owned-A";
        second = root / L"Owned-B";
        require(fs::create_directory(first) && fs::create_directory(second), "Create two independent owned folders");
        for (const auto& folder : {first, second}) {
            for (unsigned index = 0; index < memberCount; ++index) {
                wchar_t name[40]{};
                swprintf_s(name, L"Owned-%02u.%s", index, index % 2 ? L"txt" : L"bin");
                Handle file{CreateFileW((folder / name).c_str(), GENERIC_WRITE, 0, nullptr,
                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
                require(file.value != INVALID_HANDLE_VALUE, "Create owned member without overwrite");
                std::vector<BYTE> content(128 + index * 31, static_cast<BYTE>(index + (folder == first ? 1 : 17)));
                DWORD written = 0;
                require(WriteFile(file.value, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) &&
                        written == content.size(), "Write owned persistence fixture content");
                SYSTEMTIME date{};
                date.wYear = 2024; date.wMonth = 1; date.wDay = static_cast<WORD>(index + 1); date.wHour = 12;
                FILETIME modified{};
                require(SystemTimeToFileTime(&date, &modified) && SetFileTime(file.value, nullptr, nullptr, &modified),
                        "Set deterministic owned fixture modified time");
            }
        }
    }
    ~Fixture() {
        // Cleanup is limited to the exact create-new root, with its original
        // identity, expected names and no reparse points. Never delete Bags.
        try {
            if (root.empty() || !root.filename().native().starts_with(L"WindowsExplorer-ViewPersistence-")) return;
            Handle directory{CreateFileW(root.c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
            FILE_ID_INFO identity{};
            FILE_BASIC_INFO basic{};
            if (directory.value == INVALID_HANDLE_VALUE ||
                !GetFileInformationByHandleEx(directory.value, FileIdInfo, &identity, sizeof(identity)) ||
                !GetFileInformationByHandleEx(directory.value, FileBasicInfo, &basic, sizeof(basic)) ||
                (basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                std::memcmp(identity.FileId.Identifier, rootIdentity.data(), rootIdentity.size()) != 0) return;
            for (const auto& entry : fs::recursive_directory_iterator(root)) {
                const auto attributes = GetFileAttributesW(entry.path().c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return;
                if (entry.path() != first && entry.path() != second &&
                    (entry.path().parent_path() != first && entry.path().parent_path() != second)) return;
                if (entry.is_regular_file()) {
                    bool owned = false;
                    for (unsigned index = 0; index < memberCount; ++index) {
                        wchar_t name[40]{};
                        swprintf_s(name, L"Owned-%02u.%s", index, index % 2 ? L"txt" : L"bin");
                        if (entry.path().filename().native() == name) owned = true;
                    }
                    if (!owned) return;
                } else if (entry.path() != first && entry.path() != second) return;
            }
            std::error_code ignored;
            fs::remove_all(root, ignored);
        } catch (...) { /* Preserve an unexpectedly changed fixture. */ }
    }
};
ComPtr<IShellItem> shellItem(const fs::path& path) {
    ComPtr<IShellItem> result;
    succeeded(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&result)),
        "Create actual native owned folder item");
    return result;
}

struct ColumnState {
    PROPERTYKEY key{};
    UINT width = 0;
};
struct ViewState {
    FOLDERVIEWMODE mode = FVM_AUTO;
    int iconSize = 0;
    std::vector<SORTCOLUMN> sort;
    PROPERTYKEY group{};
    BOOL ascending = FALSE;
    std::vector<ColumnState> columns;
};
bool same(const ViewState& left, const ViewState& right) {
    if (left.mode != right.mode || left.iconSize != right.iconSize ||
        !IsEqualPropertyKey(left.group, right.group) || left.ascending != right.ascending ||
        left.sort.size() != right.sort.size() || left.columns.size() != right.columns.size()) return false;
    for (size_t index = 0; index < left.sort.size(); ++index)
        if (!IsEqualPropertyKey(left.sort[index].propkey, right.sort[index].propkey) ||
            left.sort[index].direction != right.sort[index].direction) return false;
    for (size_t index = 0; index < left.columns.size(); ++index)
        if (!IsEqualPropertyKey(left.columns[index].key, right.columns[index].key) ||
            left.columns[index].width != right.columns[index].width) return false;
    return true;
}
void printKey(REFPROPERTYKEY key) {
    PWSTR name = nullptr;
    if (SUCCEEDED(PSGetNameFromPropertyKey(key, &name)) && name) {
        std::wcout << name; CoTaskMemFree(name);
    } else std::wcout << L"property-id:" << key.pid;
}
void describe(const char* label, const ViewState& state) {
    std::cout << label << " mode=" << state.mode << " iconSize=" << state.iconSize << " group=";
    printKey(state.group);
    std::cout << " ascending=" << state.ascending << " sort=";
    for (const auto& column : state.sort) { printKey(column.propkey); std::cout << ':' << column.direction << ' '; }
    std::cout << " columns=";
    for (const auto& column : state.columns) { printKey(column.key); std::cout << ':' << column.width << ' '; }
    std::cout << '\n';
}
class Events final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IExplorerBrowserEvents> {
public:
    bool done = false;
    HRESULT navigation = E_PENDING;
    IFACEMETHODIMP OnNavigationPending(PCIDLIST_ABSOLUTE) override { done = false; navigation = E_PENDING; return S_OK; }
    IFACEMETHODIMP OnViewCreated(IShellView*) override { return S_OK; }
    IFACEMETHODIMP OnNavigationComplete(PCIDLIST_ABSOLUTE) override { navigation = S_OK; done = true; return S_OK; }
    IFACEMETHODIMP OnNavigationFailed(PCIDLIST_ABSOLUTE) override { navigation = E_FAIL; done = true; return S_OK; }
};
class BrowserSite final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IServiceProvider, IExplorerPaneVisibility> {
public:
    IFACEMETHODIMP QueryService(REFGUID service, REFIID iid, void** output) override {
        if (service == SID_ExplorerPaneVisibility) return QueryInterface(iid, output);
        if (!output) return E_POINTER;
        *output = nullptr; return E_NOINTERFACE;
    }
    IFACEMETHODIMP GetPaneState(REFEXPLORERPANE pane, EXPLORERPANESTATE* state) override {
        if (!state) return E_POINTER;
        *state = static_cast<EXPLORERPANESTATE>((pane == EP_NavPane ? EPS_DEFAULT_ON : EPS_DEFAULT_OFF) | EPS_FORCE);
        return S_OK;
    }
};
class Browser final {
public:
    ~Browser() { close(); }
    void initialize(IShellItem* folder, bool persist = true) {
        WNDCLASSW definition{};
        definition.lpfnWndProc = DefWindowProcW;
        definition.hInstance = GetModuleHandleW(nullptr);
        definition.lpszClassName = L"WindowsExplorerOwnedViewPersistenceHost";
        require(RegisterClassW(&definition) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
                "Register invisible private persistence host");
        window_ = CreateWindowExW(0, definition.lpszClassName, L"Owned native view persistence fixture",
            WS_OVERLAPPEDWINDOW, 0, 0, 1000, 700, nullptr, nullptr, definition.hInstance, nullptr);
        require(window_ && !IsWindowVisible(window_), "Create hidden private persistence host");
        succeeded(CoCreateInstance(CLSID_ExplorerBrowser, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&browser_)), "Create fresh native ExplorerBrowser instance");
        site_ = Make<BrowserSite>();
        require(site_ != nullptr, "Allocate public native browser pane site");
        ComPtr<IObjectWithSite> objectSite;
        succeeded(browser_.As(&objectSite), "Obtain native browser public site interface");
        succeeded(objectSite->SetSite(static_cast<IServiceProvider*>(site_.Get())), "Attach public pane site before browser initialization");
        const auto options = static_cast<EXPLORER_BROWSER_OPTIONS>(EBO_SHOWFRAMES | EBO_NOTRAVELLOG | EBO_NOBORDER |
            (persist ? 0 : EBO_NOPERSISTVIEWSTATE));
        succeeded(browser_->SetOptions(options), "Set real production browser persistence options");
        EXPLORER_BROWSER_OPTIONS applied{};
        succeeded(browser_->GetOptions(&applied), "Read actual native browser options");
        require(applied == options, "Native browser did not retain requested persistence policy");
        succeeded(browser_->SetPropertyBag(bagName), "Select production app-specific native view-state bag");
        FOLDERSETTINGS settings{static_cast<UINT>(FVM_AUTO), FWF_AUTOARRANGE};
        RECT rectangle{0, 0, 1000, 700};
        succeeded(browser_->Initialize(window_, &rectangle, &settings), "Initialize native auto/default view");
        events_ = Make<Events>();
        require(events_ != nullptr, "Allocate native view navigation observer");
        succeeded(browser_->Advise(events_.Get(), &cookie_), "Observe real native folder navigation");
        navigate(folder);
    }
    void navigate(IShellItem* folder) {
        columns_.Reset(); folderView_.Reset(); view_.Reset();
        succeeded(browser_->BrowseToObject(folder, SBSP_ABSOLUTE), "Navigate native view to owned folder");
        waitFor([&] { return events_->done; }, "Native persistence navigation timed out");
        succeeded(events_->navigation, "Complete native persistence navigation");
        succeeded(browser_->GetCurrentView(IID_PPV_ARGS(&view_)), "Obtain actual native current Shell view");
        succeeded(view_.As(&folderView_), "Obtain actual native IFolderView2");
        succeeded(view_.As(&columns_), "Obtain actual native IColumnManager");
        waitFor([&] { int count = -1; return SUCCEEDED(folderView_->ItemCount(SVGIO_ALLVIEW, &count)) && count == memberCount; },
                "Native owned folder view did not finish enumeration");
        HWND nativeWindow = nullptr;
        succeeded(view_->GetWindow(&nativeWindow), "Read native view window identity");
        DWORD process = 0;
        const DWORD thread = GetWindowThreadProcessId(nativeWindow, &process);
        require(nativeWindow && IsChild(window_, nativeWindow) && process == GetCurrentProcessId() &&
                thread == GetCurrentThreadId() && !IsWindowVisible(window_) && !IsWindowVisible(nativeWindow),
                "Native view must remain an owned invisible private-desktop child");
        for (unsigned index = 0; index < memberCount; ++index) {
            ComPtr<IShellItem> item, parent;
            succeeded(folderView_->GetItem(static_cast<int>(index), IID_PPV_ARGS(&item)), "Read native owned view member");
            succeeded(item->GetParent(&parent), "Read native member parent before inspecting file identity");
            int comparison = 1;
            succeeded(parent->Compare(folder, SICHINT_CANONICAL, &comparison), "Compare native owned folder identity");
            require(comparison == 0, "Native persistence view item escaped its owned folder");
        }
    }
    ViewState read() const {
        ViewState state;
        succeeded(folderView_->GetViewModeAndIconSize(&state.mode, &state.iconSize), "Read actual native mode and icon size");
        int count = 0;
        succeeded(folderView_->GetSortColumnCount(&count), "Read native sort-column count");
        require(count > 0 && count <= 16, "Native view sort-column count is bounded");
        state.sort.resize(static_cast<size_t>(count));
        succeeded(folderView_->GetSortColumns(state.sort.data(), count), "Read actual native sort order");
        succeeded(folderView_->GetGroupBy(&state.group, &state.ascending), "Read actual native grouping");
        UINT visible = 0;
        succeeded(columns_->GetColumnCount(CM_ENUM_VISIBLE, &visible), "Read actual visible native column count");
        require(visible > 0 && visible <= 32, "Native visible-column count is bounded");
        std::vector<PROPERTYKEY> keys(visible);
        succeeded(columns_->GetColumns(CM_ENUM_VISIBLE, keys.data(), visible), "Read actual native visible-column order");
        for (const auto& key : keys) {
            CM_COLUMNINFO info{sizeof(info)};
            info.dwMask = CM_MASK_WIDTH | CM_MASK_STATE;
            succeeded(columns_->GetColumnInfo(key, &info), "Read actual native column width and visibility");
            require((info.dwState & CM_STATE_VISIBLE) != 0 && info.uWidth > 0 && info.uWidth < 10000,
                    "Native visible column metadata is valid");
            state.columns.push_back({key, info.uWidth});
        }
        return state;
    }
    ViewState configure(bool first) {
        succeeded(folderView_->SetViewModeAndIconSize(FVM_DETAILS, 16), "Activate Details before setting native columns");
        const std::vector<PROPERTYKEY> keys = first ?
            std::vector<PROPERTYKEY>{PKEY_ItemNameDisplay, PKEY_Size, PKEY_DateModified} :
            std::vector<PROPERTYKEY>{PKEY_ItemNameDisplay, PKEY_ItemTypeText, PKEY_DateModified, PKEY_Size};
        const std::array<UINT, 4> widths = first ? std::array<UINT, 4>{237, 113, 181, 0} :
                                                          std::array<UINT, 4>{191, 149, 213, 127};
        succeeded(columns_->SetColumns(keys.data(), static_cast<UINT>(keys.size())), "Set real native visible-column order");
        for (size_t index = 0; index < keys.size(); ++index) {
            CM_COLUMNINFO info{sizeof(info)};
            info.dwMask = CM_MASK_STATE;
            succeeded(columns_->GetColumnInfo(keys[index], &info), "Preserve native fixed-column flags");
            info.dwMask = CM_MASK_WIDTH | CM_MASK_STATE;
            info.dwState |= CM_STATE_VISIBLE;
            info.uWidth = widths[index];
            succeeded(columns_->SetColumnInfo(keys[index], &info), "Set real native visible column width");
        }
        const PROPERTYKEY group = first ? PKEY_DateModified : PKEY_ItemTypeText;
        succeeded(folderView_->SetGroupBy(group, first ? TRUE : FALSE), "Set independent real native grouping");
        const std::array<SORTCOLUMN, 2> sort = first ?
            std::array<SORTCOLUMN, 2>{{{PKEY_Size, SORT_DESCENDING}, {PKEY_ItemNameDisplay, SORT_ASCENDING}}} :
            std::array<SORTCOLUMN, 2>{{{PKEY_ItemNameDisplay, SORT_ASCENDING}, {PKEY_DateModified, SORT_DESCENDING}}};
        succeeded(folderView_->SetSortColumns(sort.data(), static_cast<int>(sort.size())), "Set independent real native sort order");
        succeeded(folderView_->SetViewModeAndIconSize(first ? FVM_DETAILS : FVM_ICON, first ? 16 : 96),
                "Set distinct actual native mode and icon size");
        pump();
        const auto state = read();
        require(state.mode == (first ? FVM_DETAILS : FVM_ICON) && state.iconSize == (first ? 16 : 96),
                "Native view did not apply requested distinct mode and icon size");
        require(IsEqualPropertyKey(state.group, group) && state.ascending == (first ? TRUE : FALSE),
                "Native view did not apply requested independent grouping");
        require(state.sort.size() == sort.size(), "Native view did not apply requested sort-column count");
        for (size_t index = 0; index < sort.size(); ++index)
            require(IsEqualPropertyKey(state.sort[index].propkey, sort[index].propkey) &&
                    state.sort[index].direction == sort[index].direction, "Native view did not apply requested sort sequence");
        require(state.columns.size() == keys.size(), "Native view did not apply requested visible columns");
        for (size_t index = 0; index < keys.size(); ++index)
            require(IsEqualPropertyKey(state.columns[index].key, keys[index]) && state.columns[index].width == widths[index],
                    "Native view did not apply requested column order and width");
        return state;
    }
    void verifyRestored(const ViewState& expected, const char* stage) const {
        const auto initial = read();
        describe(stage, initial);
        if (same(initial, expected)) return;
        try { waitFor([&] { return same(read(), expected); }, "Native view state was not restored after Destroy/recreate", 5000); }
        catch (...) { describe("Expected saved native state", expected); describe("Final restored native state", read()); throw; }
    }
    HRESULT close() noexcept {
        HRESULT result = S_OK;
        // Match ExplorerApp::destroyBrowser's current site-detachment order,
        // rather than proving only a more favorable standalone host lifecycle.
        if (view_) {
            ComPtr<IObjectWithSite> viewSite;
            if (SUCCEEDED(view_.As(&viewSite))) viewSite->SetSite(nullptr);
        }
        columns_.Reset(); folderView_.Reset(); view_.Reset();
        if (browser_) {
            if (cookie_) { result = browser_->Unadvise(cookie_); cookie_ = 0; }
            ComPtr<IObjectWithSite> site;
            if (SUCCEEDED(browser_.As(&site))) site->SetSite(nullptr);
            const auto destroyed = browser_->Destroy();
            if (SUCCEEDED(result)) result = destroyed;
            browser_.Reset();
        }
        events_.Reset(); site_.Reset();
        if (window_) { DestroyWindow(window_); window_ = nullptr; }
        pump();
        return result;
    }
private:
    HWND window_ = nullptr;
    DWORD cookie_ = 0;
    ComPtr<IExplorerBrowser> browser_;
    ComPtr<IShellView> view_;
    ComPtr<IFolderView2> folderView_;
    ComPtr<IColumnManager> columns_;
    ComPtr<BrowserSite> site_;
    ComPtr<Events> events_;
};

void run(const explorer::PrivateDesktop& desktop) {
    // Enumerate a real hidden anchor even before the native view exists. Some
    // desktops report no enumeration on an entirely empty window list.
    struct HiddenAnchor {
        HWND window = CreateWindowExW(0, L"STATIC", L"Owned isolation anchor", WS_POPUP,
            0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ~HiddenAnchor() { if (window) DestroyWindow(window); }
    } anchor;
    require(anchor.window != nullptr && !IsWindowVisible(anchor.window), "Create hidden private-desktop enumeration anchor");
    VisibilityObserver visibility;
    succeeded(visibility.start(), "Observe actual new visible windows on original desktop");
    const DWORD clipboard = GetClipboardSequenceNumber();
    unsigned observations = 0;
    const auto isolated = [&] {
        bool unchanged = false, visible = true;
        succeeded(desktop.verifyIsolation(&unchanged), "Verify private desktop isolation");
        succeeded(desktop.visibleWindowsOnInputDesktop(visible), "Observe own input-desktop window visibility");
        struct WindowObservation { HWND anchor; bool anchorSeen = false; bool visible = false; } privateWindows{anchor.window};
        require(EnumDesktopWindows(GetThreadDesktop(GetCurrentThreadId()), [](HWND window, LPARAM argument) -> BOOL {
            auto& observation = *reinterpret_cast<WindowObservation*>(argument);
            if (window == observation.anchor) observation.anchorSeen = true;
            if (IsWindowVisible(window)) observation.visible = true;
            return TRUE;
        }, reinterpret_cast<LPARAM>(&privateWindows)) != FALSE && privateWindows.anchorSeen,
            "Observe actual private-desktop window visibility and owned anchor");
        ++observations;
        require(unchanged && !visible && !privateWindows.visible && !visibility.visible(), "Native persistence verification exposed UI");
    };
    isolated();
    CABINETSTATE cabinet{};
    const BOOL cabinetRead = ReadCabinetState(&cabinet, sizeof(cabinet));
    std::cout << "Read-only native cabinet policy fromRegistry=" << cabinetRead <<
        " rememberPerFolder=" << cabinet.fSaveLocalView << '\n';
    Fixture fixture;
    const auto filesA = snapshot(fixture.first), filesB = snapshot(fixture.second);
    auto folderA = shellItem(fixture.first), folderB = shellItem(fixture.second);
    ViewState savedA, savedB;
    {
        Browser browser;
        browser.initialize(folderA.Get());
        savedA = browser.configure(true);
        describe("Saved owned A", savedA);
        // Starting a navigation must persist A independently of B.
        browser.navigate(folderB.Get());
        savedB = browser.configure(false);
        describe("Saved owned B", savedB);
        require(!same(savedA, savedB), "Two owned folders need distinct actual native settings");
        isolated();
        succeeded(browser.close(), "Destroy modified native browser and persist both owned views");
    }
    isolated();
    {
        Browser recreated;
        recreated.initialize(folderA.Get());
        recreated.verifyRestored(savedA, "Fresh instance restored owned A");
        recreated.navigate(folderB.Get());
        recreated.verifyRestored(savedB, "Fresh instance independently restored owned B");
        succeeded(recreated.close(), "Destroy first recreated native browser");
    }
    isolated();
    // The real production headless policy must not replace persisted settings.
    // This CI-only negative control uses the documented suppression option.
    {
        Browser nonpersistent;
        nonpersistent.initialize(folderA.Get(), false);
        const auto changed = nonpersistent.configure(false);
        require(!same(changed, savedA), "Suppressed-persistence fixture must apply a genuinely different native state");
        succeeded(nonpersistent.close(), "Destroy native NOPERSISTVIEWSTATE negative control");
    }
    isolated();
    {
        Browser recreatedA;
        recreatedA.initialize(folderA.Get());
        recreatedA.verifyRestored(savedA, "Owned A retained settings after NOPERSISTVIEWSTATE");
        succeeded(recreatedA.close(), "Destroy independently restored A browser");
        Browser recreatedB;
        recreatedB.initialize(folderB.Get());
        recreatedB.verifyRestored(savedB, "Owned B remained independent after A changes");
        succeeded(recreatedB.close(), "Destroy independently restored B browser");
    }
    require(snapshot(fixture.first) == filesA && snapshot(fixture.second) == filesB,
            "Native view persistence changed owned file identities, content or metadata");
    require(GetClipboardSequenceNumber() == clipboard, "Native view persistence changed the clipboard");
    isolated();
    visibility.stop();
    require(!visibility.visible(), "Native persistence verification showed a window on original desktop");
    std::cout << "PASS: actual native navigation/Destroy/recreate restored independent mode/icon size/sort/group/column order/widths; "
                 "NOPERSISTVIEWSTATE preserved prior state; all owned file identities/content unchanged; "
                 "no visible input-desktop UI; observations=" << observations << '\n';
}
} // namespace

int main() {
    if (!environmentEquals(L"GITHUB_ACTIONS", L"true") ||
        !environmentEquals(L"WINDOWSEXPLORER_VIEW_PERSISTENCE_TEST", L"1")) {
        std::cout << "SKIP: native per-folder persistence requires disposable GitHub runner and explicit opt-in\n";
        return 0;
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    explorer::PrivateDesktop desktop;
    HRESULT initialized = desktop.initialize();
    if (FAILED(initialized)) { std::cerr << "FAIL: initialize private persistence desktop\n"; return 1; }
    initialized = OleInitialize(nullptr);
    if (FAILED(initialized)) { std::cerr << "FAIL: initialize native persistence STA\n"; return 1; }
    int result = 0;
    try { run(desktop); }
    catch (const std::exception& error) { std::cerr << "FAIL: CI-only native per-folder persistence: " << error.what() << '\n'; result = 1; }
    catch (...) { std::cerr << "FAIL: CI-only native per-folder persistence: unknown exception\n"; result = 1; }
    OleUninitialize();
    return result;
}
