#include "explorer/core.hpp"
#include "explorer/headless_visual.hpp"
#include "explorer/namespace_actions.hpp"
#include "explorer/selection.hpp"

#include <shlobj.h>
#include <wrl/implements.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;
namespace fs = std::filesystem;
constexpr unsigned itemCount = 10000;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void succeeded(HRESULT result, const char* message) {
    if (FAILED(result)) throw std::runtime_error(std::string(message) + " HRESULT=" + std::to_string(static_cast<ULONG>(result)));
}
double milliseconds() {
    LARGE_INTEGER value{}, frequency{};
    QueryPerformanceCounter(&value);
    QueryPerformanceFrequency(&frequency);
    return static_cast<double>(value.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
}
void waitFor(const std::function<bool()>& ready, const char* message) {
    const auto end = GetTickCount64() + 15000;
    while (!ready()) {
        require(GetTickCount64() < end, message);
        MSG event{};
        while (PeekMessageW(&event, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&event);
            DispatchMessageW(&event);
        }
        MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct PidlDeleter {
    using pointer = PIDLIST_ABSOLUTE;
    void operator()(PIDLIST_ABSOLUTE value) const noexcept { CoTaskMemFree(value); }
};
struct FileStamp {
    ULONGLONG volume = 0;
    std::array<BYTE, 16> identity{};
    LONGLONG size = 0, write = 0, change = 0;
    DWORD attributes = 0;
    std::array<BYTE, 16> bytes{};
    auto operator<=>(const FileStamp&) const = default;
};
struct Fixture {
    fs::path root;
    unsigned count = itemCount;
    explicit Fixture(unsigned number = itemCount) : count(number) {
        GUID identifier{};
        succeeded(CoCreateGuid(&identifier), "Create owned selection fixture identifier");
        wchar_t text[40]{};
        require(StringFromGUID2(identifier, text, 40) > 0, "Format owned selection fixture identifier");
        root = fs::temp_directory_path() / (std::wstring(L"WindowsExplorer-Selection-") + text);
        require(fs::create_directory(root), "Create fresh owned selection fixture");
        for (unsigned index = 0; index < count; ++index) {
            wchar_t name[40]{};
            swprintf_s(name, L"Owned-%05u.bin", index);
            Handle file{CreateFileW((root / name).c_str(), GENERIC_WRITE, 0, nullptr,
                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
            require(file.value != INVALID_HANDLE_VALUE, "Create owned selection member without overwrite");
            const std::array<ULONGLONG, 2> content{0x4f574e454453454cULL, index};
            DWORD written = 0;
            require(WriteFile(file.value, content.data(), sizeof(content), &written, nullptr) && written == sizeof(content),
                    "Write owned selection fixture content");
        }
    }
    ~Fixture() {
        // The exact GUID directory was created by this instance. Never derive a
        // cleanup target from a Shell return value or another enumerated path.
        if (root.empty() || root.filename().native().find(L"WindowsExplorer-Selection-") != 0) return;
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    std::map<std::wstring, FileStamp> snapshot() const {
        std::map<std::wstring, FileStamp> result;
        for (const auto& entry : fs::directory_iterator(root)) {
            require(entry.is_regular_file(), "Owned fixture contains an unexpected directory or object");
            Handle file{CreateFileW(entry.path().c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
            require(file.value != INVALID_HANDLE_VALUE, "Read owned fixture member");
            FILE_ID_INFO identity{};
            FILE_STANDARD_INFO standard{};
            FILE_BASIC_INFO basic{};
            require(GetFileInformationByHandleEx(file.value, FileIdInfo, &identity, sizeof(identity)) &&
                    GetFileInformationByHandleEx(file.value, FileStandardInfo, &standard, sizeof(standard)) &&
                    GetFileInformationByHandleEx(file.value, FileBasicInfo, &basic, sizeof(basic)),
                    "Read owned fixture identity and metadata");
            FileStamp stamp;
            stamp.volume = identity.VolumeSerialNumber;
            std::copy(std::begin(identity.FileId.Identifier), std::end(identity.FileId.Identifier), stamp.identity.begin());
            stamp.size = standard.EndOfFile.QuadPart;
            stamp.write = basic.LastWriteTime.QuadPart;
            stamp.change = basic.ChangeTime.QuadPart;
            stamp.attributes = basic.FileAttributes;
            DWORD read = 0;
            require(stamp.size == static_cast<LONGLONG>(stamp.bytes.size()) &&
                    ReadFile(file.value, stamp.bytes.data(), static_cast<DWORD>(stamp.bytes.size()), &read, nullptr) &&
                    read == stamp.bytes.size(), "Read complete owned fixture content");
            require(result.emplace(entry.path().filename().native(), stamp).second, "Duplicate owned fixture name");
        }
        require(result.size() == count, "Owned fixture member count changed");
        return result;
    }
};
class Events final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IExplorerBrowserEvents> {
public:
    bool done = false;
    HRESULT result = E_PENDING;
    IFACEMETHODIMP OnNavigationPending(PCIDLIST_ABSOLUTE) override { return S_OK; }
    IFACEMETHODIMP OnViewCreated(IShellView*) override { return S_OK; }
    IFACEMETHODIMP OnNavigationComplete(PCIDLIST_ABSOLUTE) override { done = true; result = S_OK; return S_OK; }
    IFACEMETHODIMP OnNavigationFailed(PCIDLIST_ABSOLUTE) override { done = true; result = E_FAIL; return S_OK; }
};
struct Browser {
    HWND owner = nullptr;
    HWND previousActive = nullptr;
    ComPtr<IExplorerBrowser> browser;
    ComPtr<IShellView> view;
    ComPtr<IFolderView2> folderView;
    ComPtr<Events> events;
    DWORD cookie = 0;
    void initialize(IShellItem* folder, unsigned expected = itemCount) {
        WNDCLASSW cls{};
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpfnWndProc = DefWindowProcW;
        cls.lpszClassName = L"WindowsExplorerOwnedNativeSelectionRegression";
        require(RegisterClassW(&cls) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "Register private selection host");
        owner = CreateWindowExW(0, cls.lpszClassName, L"Owned native selection fixture", WS_OVERLAPPEDWINDOW,
            0, 0, 1000, 700, nullptr, nullptr, cls.hInstance, nullptr);
        require(owner != nullptr, "Create private selection host");
        succeeded(CoCreateInstance(CLSID_ExplorerBrowser, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&browser)), "Create actual native selection browser");
        succeeded(browser->SetOptions(EBO_NAVIGATEONCE | EBO_NOTRAVELLOG | EBO_NOPERSISTVIEWSTATE),
                  "Disable selection fixture history and view persistence");
        RECT bounds{0, 0, 1000, 700};
        FOLDERSETTINGS settings{FVM_DETAILS, FWF_AUTOARRANGE};
        succeeded(browser->Initialize(owner, &bounds, &settings), "Initialize actual private Shell view");
        events = Make<Events>();
        succeeded(browser->Advise(events.Get(), &cookie), "Observe owned selection navigation");
        succeeded(browser->BrowseToObject(folder, SBSP_ABSOLUTE), "Browse only fresh owned selection fixture");
        waitFor([&] { return events->done; }, "Owned selection navigation timed out");
        succeeded(events->result, "Complete owned selection navigation");
        succeeded(browser->GetCurrentView(IID_PPV_ARGS(&view)), "Read exact native selection view");
        succeeded(view.As(&folderView), "Read actual native folder view");
        ShowWindow(owner, SW_SHOWNOACTIVATE);
        previousActive=GetActiveWindow();
        SetActiveWindow(owner);
        require(GetActiveWindow()==owner,"Activate the exact owned private selection frame");
        UpdateWindow(owner);
        succeeded(view->UIActivate(SVUIA_ACTIVATE_NOFOCUS), "Activate only the owned private Shell view without focus");
        waitFor([&] { int count = -1; return SUCCEEDED(folderView->ItemCount(SVGIO_ALLVIEW, &count)) && count == static_cast<int>(expected); },
                "Native view did not enumerate all ten thousand owned members");
        require(RedrawWindow(owner,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_UPDATENOW)!=FALSE,
            "Render the exact owned private selection viewport before native focus setup");
        MSG message{};
        unsigned dispatched=0;
        while(dispatched++<64&&PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            require(message.message!=WM_QUIT,"Private selection fixture received quit");
            TranslateMessage(&message);DispatchMessageW(&message);
        }
    }
    ~Browser() {
        if(owner&&GetActiveWindow()==owner)SetActiveWindow(previousActive);
        folderView.Reset(); view.Reset();
        if (browser) { if (cookie) browser->Unadvise(cookie); browser->Destroy(); browser.Reset(); }
        events.Reset();
        if (owner) DestroyWindow(owner);
    }
};
using IdentitySet = std::set<std::vector<BYTE>>;
IdentitySet identities(IFolderView2* view, SVGIO scope, IShellItem* ownedFolder) {
    int count = -1;
    succeeded(view->ItemCount(scope, &count), "Read actual native selection count");
    require(count >= 0 && count <= itemCount, "Native selection count escaped fixture bound");
    if (!count) return {};
    ComPtr<IShellItemArray> items;
    succeeded(view->Items(scope, IID_PPV_ARGS(&items)), "Read actual native selection objects");
    DWORD actual = 0;
    succeeded(items->GetCount(&actual), "Read native selection object count");
    require(actual == static_cast<DWORD>(count), "Native item array differs from current view count");
    IdentitySet result;
    for (DWORD index = 0; index < actual; ++index) {
        ComPtr<IShellItem> item, parent;
        succeeded(items->GetItemAt(index, &item), "Read exact native selected item");
        succeeded(item->GetParent(&parent), "Read native selected parent identity");
        int order = 1;
        succeeded(parent->Compare(ownedFolder, SICHINT_CANONICAL, &order), "Compare selected parent canonical identity");
        require(order == 0, "Native selection escaped the fresh owned view");
        PIDLIST_ABSOLUTE raw = nullptr;
        succeeded(SHGetIDListFromObject(item.Get(), &raw), "Read exact native selected PIDL");
        std::unique_ptr<ITEMIDLIST, PidlDeleter> identity(raw);
        const auto bytes = ILGetSize(raw);
        require(bytes >= sizeof(USHORT) && bytes <= 65536, "Native selected PIDL size is invalid");
        const auto begin = reinterpret_cast<const BYTE*>(raw);
        require(result.emplace(begin, begin + bytes).second, "Native selected array repeated an item identity");
    }
    return result;
}
IdentitySet fileIdentities(IFolderView2* view, IShellItem* ownedFolder) {
    int count = -1;
    succeeded(view->ItemCount(SVGIO_SELECTION, &count), "Read native selected-file identity count");
    require(count >= 0 && count <= itemCount, "Selected-file identities escaped owned fixture bound");
    if (!count) return {};
    ComPtr<IShellItemArray> selected;
    succeeded(view->Items(SVGIO_SELECTION, IID_PPV_ARGS(&selected)), "Read exact selected-file objects");
    DWORD actual = 0;
    succeeded(selected->GetCount(&actual), "Read selected-file object count");
    require(actual == static_cast<DWORD>(count), "Selected-file array differs from current view");
    IdentitySet result;
    for (DWORD index = 0; index < actual; ++index) {
        ComPtr<IShellItem> item, parent;
        succeeded(selected->GetItemAt(index, &item), "Read native selected file");
        succeeded(item->GetParent(&parent), "Read selected-file parent");
        int order = 1;
        succeeded(parent->Compare(ownedFolder, SICHINT_CANONICAL, &order), "Compare selected-file owned parent");
        require(order == 0, "Selected-file identity escaped the fresh owned folder");
        PWSTR raw = nullptr;
        succeeded(item->GetDisplayName(SIGDN_FILESYSPATH, &raw), "Read owned selected-file path");
        Handle file{CreateFileW(raw, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        CoTaskMemFree(raw);
        require(file.value != INVALID_HANDLE_VALUE, "Open selected-file native identity");
        FILE_ID_INFO id{};
        require(GetFileInformationByHandleEx(file.value, FileIdInfo, &id, sizeof(id)), "Read selected-file native volume/FileID");
        std::vector<BYTE> key(sizeof(id.VolumeSerialNumber) + sizeof(id.FileId.Identifier));
        std::memcpy(key.data(), &id.VolumeSerialNumber, sizeof(id.VolumeSerialNumber));
        std::memcpy(key.data() + sizeof(id.VolumeSerialNumber), id.FileId.Identifier, sizeof(id.FileId.Identifier));
        require(result.insert(std::move(key)).second, "Selected-file array repeated a volume/FileID identity");
    }
    return result;
}
struct Sample { std::string action; double facadeMs = 0, readbackMs = 0; unsigned selected = 0; };
void verifyIsolation(explorer::PrivateDesktop& desktop) {
    bool same = false, visible = true;
    succeeded(desktop.verifyIsolation(&same), "Read private selection desktop isolation");
    succeeded(desktop.visibleWindowsOnInputDesktop(visible), "Read input-desktop visibility");
    require(same && !visible, "Native selection exposed input-desktop UI");
}
void verifyDocumentedFallback(explorer::PrivateDesktop& desktop) {
    Fixture fixture(24);
    const auto before = fixture.snapshot();
    const auto clipboard = GetClipboardSequenceNumber();
    ComPtr<IShellItem> folder;
    succeeded(SHCreateItemFromParsingName(fixture.root.c_str(), nullptr, IID_PPV_ARGS(&folder)), "Parse owned fallback fixture");
    Browser browser;
    browser.initialize(folder.Get(), 24);
    const auto all = identities(browser.folderView.Get(), SVGIO_ALLVIEW, folder.Get());
    require(all.size() == 24, "Documented fallback view membership differs");
    const auto change = [&](explorer::SelectionAction action, const IdentitySet& expected) {
        std::cerr << "Fallback begin action=" << static_cast<unsigned>(action) << " expected=" << expected.size() << '\n';
        succeeded(explorer::changeShellSelection(browser.folderView.Get(), browser.view.Get(), action, nullptr, true),
                  "Execute documented actual-view selection fallback");
        waitFor([&] {
            int count = -1;
            return SUCCEEDED(browser.folderView->ItemCount(SVGIO_SELECTION, &count)) &&
                   count == static_cast<int>(expected.size());
        }, "Documented native selection fallback did not complete its requested selection");
        const auto actual = identities(browser.folderView.Get(), SVGIO_SELECTION, folder.Get());
        if (actual != expected)
            std::cerr << "Fallback action=" << static_cast<unsigned>(action) << " expected=" << expected.size()
                      << " actual=" << actual.size() << '\n';
        require(actual == expected,
                "Documented fallback changed actual selected identities incorrectly");
    };
    change(explorer::SelectionAction::All, all);
    change(explorer::SelectionAction::None, {});
    DWORD original = 0, flags = 0;
    succeeded(browser.folderView->GetCurrentFolderFlags(&original), "Read fallback view flags");
    succeeded(browser.folderView->SetCurrentFolderFlags(FWF_CHECKSELECT, FWF_CHECKSELECT), "Enable fallback checkbox view flag");
    succeeded(browser.folderView->GetCurrentFolderFlags(&flags), "Read fallback checkbox flag");
    PITEMID_CHILD focusChild=nullptr;
    succeeded(browser.folderView->Item(2,&focusChild),"Read actual fallback focus child PIDL");
    const auto focusedSelection=browser.view->SelectItem(focusChild,SVSI_SELECT|SVSI_FOCUSED|SVSI_ENSUREVISIBLE);
    CoTaskMemFree(focusChild);
    succeeded(focusedSelection,"Seed actual private fallback item focus through the original Shell view");
    for (int index : {7, 19})
        succeeded(browser.folderView->SelectItem(index, SVSI_SELECT | SVSI_NOTAKEFOCUS), "Seed actual sparse fallback selection");
    int seedCount=-1,seedFocus=-1;
    browser.folderView->ItemCount(SVGIO_SELECTION,&seedCount);
    browser.folderView->GetFocusedItem(&seedFocus);
    std::cerr<<"Fallback seed selected="<<seedCount<<" focused="<<seedFocus<<" flags="<<flags<<'\n';
    waitFor([&] {
        int selected=-1,focused=-1;
        return SUCCEEDED(browser.folderView->ItemCount(SVGIO_SELECTION,&selected))&&selected==3&&
            SUCCEEDED(browser.folderView->GetFocusedItem(&focused))&&focused==2;
    },"Native fallback seed did not realize its exact selection and item focus");
    const auto seed = identities(browser.folderView.Get(), SVGIO_SELECTION, folder.Get());
    require(seed.size() == 3, "Fallback sparse seed differs");
    IdentitySet complement;
    std::set_difference(all.begin(), all.end(), seed.begin(), seed.end(), std::inserter(complement, complement.end()));
    const auto focusWindow = GetFocus();
    int focused = -1;
    succeeded(browser.folderView->GetFocusedItem(&focused), "Read fallback item focus");
    require(focused == 2 && (flags & FWF_CHECKSELECT), "Fallback focus/checkbox setup differs");
    change(explorer::SelectionAction::Invert, complement);
    change(explorer::SelectionAction::Invert, seed);
    int restoredFocus = -1;
    DWORD restoredFlags = 0;
    succeeded(browser.folderView->GetFocusedItem(&restoredFocus), "Read fallback focus after inverse");
    succeeded(browser.folderView->GetCurrentFolderFlags(&restoredFlags), "Read fallback flags after inverse");
    require(restoredFocus == focused && GetFocus() == focusWindow && restoredFlags == flags,
            "Documented fallback changed focus or checkbox/view flags");
    succeeded(browser.folderView->SetCurrentFolderFlags(FWF_CHECKSELECT, original & FWF_CHECKSELECT), "Restore fallback checkbox flag");
    change(explorer::SelectionAction::None, {});
    explorer::NativeNamespaceActions actions;
    succeeded(actions.initialize(browser.owner, {folder, nullptr, browser.view}), "Attach exact 24-file no-selection facade");
    explorer::NamespaceCommandState nativeNone;
    succeeded(actions.queryCommandState(L"Windows.selectnone", &nativeNone, explorer::NamespaceMenuScope::Background),
              "Read actual SelectNone provider without invoking");
    const auto noOpFocus = GetFocus();
    int noOpItem = -1, afterNoOpItem = -1;
    DWORD noOpFlags = 0, afterNoOpFlags = 0;
    succeeded(browser.folderView->GetFocusedItem(&noOpItem), "Read no-selection focused item");
    succeeded(browser.folderView->GetCurrentFolderFlags(&noOpFlags), "Read no-selection view flags");
    succeeded(explorer::changeShellSelection(browser.folderView.Get(), browser.view.Get(),
        explorer::SelectionAction::None, &actions, true), "Disabled native SelectNone is an ordinary no-op");
    require(identities(browser.folderView.Get(), SVGIO_SELECTION, folder.Get()).empty(), "No-op SelectNone selected a file");
    succeeded(browser.folderView->GetFocusedItem(&afterNoOpItem), "Read focus after no-selection no-op");
    succeeded(browser.folderView->GetCurrentFolderFlags(&afterNoOpFlags), "Read view flags after no-selection no-op");
    require(noOpFocus == GetFocus() && noOpItem == afterNoOpItem && noOpFlags == afterNoOpFlags,
            "No-selection no-op changed focus or flags");
    require(fixture.snapshot() == before && GetClipboardSequenceNumber() == clipboard,
            "Documented selection fallback changed source files or clipboard");
    verifyIsolation(desktop);
    std::cout << "PASS: documented 24-file fallback exact all/none/sparse complement/inverse/focus/flags without native facade\n";
    std::cout << "PASS: native facade SelectNone on actual 24-file empty selection is a preserving no-op"
              << " providerState=" << static_cast<unsigned>(nativeNone.state) << '\n';
}
void verifyEmptyViewNoOps(explorer::PrivateDesktop& desktop) {
    Fixture fixture(0);
    const auto before = fixture.snapshot();
    const auto clipboard = GetClipboardSequenceNumber();
    ComPtr<IShellItem> folder;
    succeeded(SHCreateItemFromParsingName(fixture.root.c_str(), nullptr, IID_PPV_ARGS(&folder)), "Parse fresh empty selection fixture");
    Browser browser;
    browser.initialize(folder.Get(), 0);
    explorer::NativeNamespaceActions actions;
    succeeded(actions.initialize(browser.owner, {folder, nullptr, browser.view}), "Attach empty-view native facade");
    require(identities(browser.folderView.Get(), SVGIO_ALLVIEW, folder.Get()).empty(), "Fresh empty native view contains a file");
    const auto focus = GetFocus();
    DWORD flags = 0;
    succeeded(browser.folderView->GetCurrentFolderFlags(&flags), "Read empty-view flags");
    for (const auto action : {explorer::SelectionAction::All, explorer::SelectionAction::None,
                              explorer::SelectionAction::Invert}) {
        for (const auto native : {static_cast<explorer::NativeNamespaceActions*>(nullptr), &actions}) {
            succeeded(explorer::changeShellSelection(browser.folderView.Get(), browser.view.Get(), action, native, true),
                      "Empty native view shortcut is an ordinary no-op");
            require(identities(browser.folderView.Get(), SVGIO_ALLVIEW, folder.Get()).empty() &&
                    identities(browser.folderView.Get(), SVGIO_SELECTION, folder.Get()).empty(),
                    "Empty-view shortcut changed view identities");
            DWORD afterFlags = 0;
            succeeded(browser.folderView->GetCurrentFolderFlags(&afterFlags), "Read flags after empty-view shortcut");
            require(afterFlags == flags && GetFocus() == focus, "Empty-view shortcut changed focus or flags");
        }
    }
    require(fixture.snapshot() == before && GetClipboardSequenceNumber() == clipboard,
            "Empty-view shortcuts changed source files or clipboard");
    verifyIsolation(desktop);
    std::cout << "PASS: empty actual view All/None/Invert with native facade and fallback are preserving no-ops\n";
}
}

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    explorer::PrivateDesktop desktop;
    if (FAILED(desktop.initialize())) return 2;
    if (FAILED(OleInitialize(nullptr))) return 3;
    int result = 0;
    try {
        verifyIsolation(desktop);
        Fixture fixture;
        const auto before = fixture.snapshot();
        const auto clipboardBefore = GetClipboardSequenceNumber();
        ComPtr<IShellItem> folder;
        succeeded(SHCreateItemFromParsingName(fixture.root.c_str(), nullptr, IID_PPV_ARGS(&folder)), "Parse fresh owned fixture");
        Browser browser;
        browser.initialize(folder.Get());
        explorer::NativeNamespaceActions actions;
        succeeded(actions.initialize(browser.owner, {folder, nullptr, browser.view}), "Attach pure selection facade to exact owned view");
        const auto all = identities(browser.folderView.Get(), SVGIO_ALLVIEW, folder.Get());
        require(all.size() == itemCount, "Native all-view identities differ from owned fixture");
        std::vector<Sample> samples;
        const auto invoke = [&](const wchar_t* command, const IdentitySet& expected, const char* description) {
            waitFor([&] {
                explorer::NamespaceCommandState state;
                return SUCCEEDED(actions.queryCommandState(command, &state, explorer::NamespaceMenuScope::Background)) &&
                       state.enabled();
            }, "Actual native selection provider did not become ready");
            const auto started = milliseconds();
            const auto hr = actions.invokeViewSelection(command, browser.view.Get(), true);
            const auto called = milliseconds();
            succeeded(hr, description);
            waitFor([&] { int count = -1; return SUCCEEDED(browser.folderView->ItemCount(SVGIO_SELECTION, &count)) &&
                count == static_cast<int>(expected.size()); }, "Native selection notification timed out");
            const auto selected = identities(browser.folderView.Get(), SVGIO_SELECTION, folder.Get());
            require(selected == expected, "Native command changed selection identities incorrectly");
            samples.push_back({description, called - started, milliseconds() - called, static_cast<unsigned>(selected.size())});
            verifyIsolation(desktop);
        };
        invoke(L"Windows.selectall", all, "SelectAll");
        invoke(L"Windows.selectnone", {}, "SelectNone");
        invoke(L"Windows.invertselection", all, "InvertNoneToAll");
        invoke(L"Windows.invertselection", {}, "InvertAllToNone");

        DWORD originalFlags = 0, testFlags = 0;
        succeeded(browser.folderView->GetCurrentFolderFlags(&originalFlags), "Read original native view flags");
        succeeded(browser.folderView->SetCurrentFolderFlags(FWF_CHECKSELECT, FWF_CHECKSELECT), "Enable owned native checkbox selection");
        succeeded(browser.folderView->GetCurrentFolderFlags(&testFlags), "Read actual checkbox view flags");
        require((testFlags & FWF_CHECKSELECT) != 0, "Actual native checkbox flag was not enabled");
        succeeded(browser.folderView->SelectItem(3, SVSI_SELECT | SVSI_FOCUSED | SVSI_NOTAKEFOCUS), "Seed focused owned selected item");
        for (int index : {107, 9631})
            succeeded(browser.folderView->SelectItem(index, SVSI_SELECT | SVSI_NOTAKEFOCUS), "Seed sparse mixed selection");
        const auto seed = identities(browser.folderView.Get(), SVGIO_SELECTION, folder.Get());
        require(seed.size() == 3, "Actual native sparse selection has wrong identities");
        int focused = -1;
        succeeded(browser.folderView->GetFocusedItem(&focused), "Read native focused item before invert");
        require(focused == 3, "Native seed focus differs");
        const auto focusWindow = GetFocus();
        IdentitySet complement;
        std::set_difference(all.begin(), all.end(), seed.begin(), seed.end(), std::inserter(complement, complement.end()));
        invoke(L"Windows.invertselection", complement, "InvertMixedComplement");
        int afterFocus = -1;
        DWORD afterFlags = 0;
        succeeded(browser.folderView->GetFocusedItem(&afterFocus), "Read actual native focus after mixed invert");
        succeeded(browser.folderView->GetCurrentFolderFlags(&afterFlags), "Read actual native flags after mixed invert");
        require(afterFocus == focused && GetFocus() == focusWindow && afterFlags == testFlags,
                "Native mixed invert changed focus or checkbox/view flags");
        invoke(L"Windows.invertselection", seed, "InvertMixedRestoresSeed");
        IdentitySet expectedFileIds;
        for (const auto& [name, stamp] : before) {
            (void)name;
            std::vector<BYTE> key(sizeof(stamp.volume) + stamp.identity.size());
            std::memcpy(key.data(), &stamp.volume, sizeof(stamp.volume));
            std::memcpy(key.data() + sizeof(stamp.volume), stamp.identity.data(), stamp.identity.size());
            require(expectedFileIds.insert(std::move(key)).second, "Owned fixture repeated a volume/FileID identity");
        }
        for (const auto action : {explorer::SelectionAction::None, explorer::SelectionAction::Invert}) {
            for (const auto native : {static_cast<explorer::NativeNamespaceActions*>(nullptr), &actions}) {
                invoke(L"Windows.selectall", all, "SelectAllBeforeDocumentedClear");
                require(fileIdentities(browser.folderView.Get(), folder.Get()) == expectedFileIds,
                        "Documented-clear seed does not select all exact owned volume/FileIDs");
                int beforeItem = -1;
                DWORD beforeFlags = 0;
                succeeded(browser.folderView->GetFocusedItem(&beforeItem), "Read full-selection focused item");
                succeeded(browser.folderView->GetCurrentFolderFlags(&beforeFlags), "Read full-selection checkbox/view flags");
                const auto beforeWindow = GetFocus();
                const auto started = milliseconds();
                succeeded(explorer::changeShellSelection(browser.folderView.Get(), browser.view.Get(), action, native, true),
                          "Execute actual ten-thousand-item documented selection clear");
                const auto called = milliseconds();
                waitFor([&] {
                    int count = -1;
                    return SUCCEEDED(browser.folderView->ItemCount(SVGIO_SELECTION, &count)) && count == 0;
                }, "Documented clear did not deselect all ten thousand native items");
                require(identities(browser.folderView.Get(), SVGIO_SELECTION, folder.Get()).empty() &&
                        fileIdentities(browser.folderView.Get(), folder.Get()).empty(),
                        "Documented full clear retained a native PIDL or file identity");
                int afterItem = -1;
                DWORD clearFlags = 0;
                succeeded(browser.folderView->GetFocusedItem(&afterItem), "Read focused item after full clear");
                succeeded(browser.folderView->GetCurrentFolderFlags(&clearFlags), "Read checkbox/view flags after full clear");
                require(beforeItem == afterItem && beforeWindow == GetFocus() && beforeFlags == clearFlags &&
                        (clearFlags & FWF_CHECKSELECT), "Documented full clear changed item/window focus or checkbox/view flags");
                require(GetClipboardSequenceNumber() == clipboardBefore, "Documented full clear changed the clipboard");
                const auto description = action == explorer::SelectionAction::None
                    ? native ? "DocumentedNoneWithFacade" : "DocumentedNoneWithoutFacade"
                    : native ? "DocumentedFullInvertWithFacade" : "DocumentedFullInvertWithoutFacade";
                samples.push_back({description, called - started, milliseconds() - called, 0});
                verifyIsolation(desktop);
            }
        }
        succeeded(browser.folderView->SetCurrentFolderFlags(FWF_CHECKSELECT, originalFlags & FWF_CHECKSELECT), "Restore owned native checkbox flag");
        invoke(L"Windows.selectall", all, "SelectAllAfterMixed");
        invoke(L"Windows.selectnone", {}, "SelectNoneAfterMixed");
        const auto fallbackFocus=GetFocus();
        int fallbackItem=-1,afterFallbackItem=-1;
        DWORD fallbackFlags=0,afterFallbackFlags=0;
        succeeded(browser.folderView->GetFocusedItem(&fallbackItem),"Read large fallback focused item");
        succeeded(browser.folderView->GetCurrentFolderFlags(&fallbackFlags),"Read large fallback view flags");
        const auto fallbackStarted=milliseconds();
        succeeded(explorer::changeShellSelection(browser.folderView.Get(),browser.view.Get(),explorer::SelectionAction::All,nullptr,true),
            "Select every actual large-view member without native facade");
        const auto fallbackCalled=milliseconds();
        waitFor([&]{int count=-1;return SUCCEEDED(browser.folderView->ItemCount(SVGIO_SELECTION,&count))&&count==itemCount;},
            "Large documented selection fallback did not select every member");
        require(identities(browser.folderView.Get(),SVGIO_SELECTION,folder.Get())==all&&
            fileIdentities(browser.folderView.Get(),folder.Get())==expectedFileIds,"Large fallback selected incorrect native or file identities");
        succeeded(browser.folderView->GetFocusedItem(&afterFallbackItem),"Read large fallback item focus");
        succeeded(browser.folderView->GetCurrentFolderFlags(&afterFallbackFlags),"Read large fallback flags");
        require(fallbackFocus==GetFocus()&&fallbackItem==afterFallbackItem&&fallbackFlags==afterFallbackFlags,
            "Large documented selection fallback changed focus or flags");
        samples.push_back({"DocumentedAllWithoutFacade",fallbackCalled-fallbackStarted,milliseconds()-fallbackCalled,itemCount});
        succeeded(explorer::changeShellSelection(browser.folderView.Get(),browser.view.Get(),explorer::SelectionAction::None,nullptr,true),
            "Restore empty large selection after documented All");
        std::array<double, 3> construction{};
        for (auto& elapsed : construction) {
            explorer::NativeNamespaceActions fresh;
            succeeded(fresh.initialize(browser.owner, {folder, nullptr, browser.view}), "Attach read-only menu timing facade");
            const auto started = milliseconds();
            explorer::NamespaceInvocationPlan plan;
            succeeded(fresh.planCommandStore(L"Windows.selectall", &plan, explorer::NamespaceMenuScope::Background),
                      "Construct actual read-only native selection menu");
            elapsed = milliseconds() - started;
            require(plan.enabled && !plan.submenu && plan.canonicalVerb == L"Windows.selectall",
                    "Read-only construction did not retain the exact native selection command");
        }
        require(GetClipboardSequenceNumber() == clipboardBefore, "Pure selection changed clipboard sequence");
        require(fixture.snapshot() == before, "Pure selection changed owned file identity/content/metadata/count");
        std::cout << "PASS: 10000 native identities, all/none/mixed complement, inverse, focus, flags, files, clipboard and private desktop\n";
        std::cout << "PASS: documented 10000-file All-to-None and full-Invert-to-None, with/without native facade, exact volume/FileIDs/focus/flags\n";
        for (const auto& sample : samples)
            std::cout << "TIMING " << sample.action << " facadeMs=" << sample.facadeMs << " exactReadbackMs=" << sample.readbackMs
                      << " selected=" << sample.selected << '\n';
        for (const auto elapsed : construction)
            std::cout << "TIMING readOnlyCommandStoreConstructionMs=" << elapsed << '\n';
        verifyDocumentedFallback(desktop);
        verifyEmptyViewNoOps(desktop);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        result = 1;
    }
    OleUninitialize();
    return result;
}
