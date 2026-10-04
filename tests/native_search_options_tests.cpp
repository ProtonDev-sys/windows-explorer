#include "explorer/search.hpp"
#include "explorer/context_menu.hpp"

#include <shlobj.h>
#include <searchapi.h>
#include <shlwapi.h>
#include <propkey.h>
#include <wrl/implements.h>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_set>

// Actual option invocation is restricted to an explicitly opted-in disposable
// GitHub VM. A separately compiled read-only audit cannot invoke any command.
namespace {
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;
namespace fs = std::filesystem;
#ifdef WINDOWSEXPLORER_SEARCH_OPTIONS_READ_ONLY_AUDIT
constexpr bool ReadOnlyAudit = true;
#else
constexpr bool ReadOnlyAudit = false;
#endif
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void succeeded(HRESULT result, const char* message) {
    if (FAILED(result)) {
        std::cerr << message << " HRESULT=0x" << std::hex << static_cast<unsigned long>(result) << std::dec << '\n';
        throw std::runtime_error(message);
    }
}
bool environmentEquals(const wchar_t* name, const wchar_t* expected) {
    wchar_t value[32]{};
    const DWORD size = GetEnvironmentVariableW(name, value, 32);
    return size && size < 32 && std::wcscmp(value, expected) == 0;
}
void pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
}
void waitFor(const std::function<bool()>& ready, const char* message, DWORD timeout = 15000) {
    const auto end = GetTickCount64() + timeout;
    while (!ready()) {
        require(GetTickCount64() < end, message);
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT); pump();
    }
}
class VisibilityObserver final {
public:
    HRESULT start() {
        EnumWindows([](HWND window, LPARAM argument) -> BOOL {
            if (IsWindowVisible(window)) reinterpret_cast<VisibilityObserver*>(argument)->baseline_.insert(window);
            return TRUE;
        }, reinterpret_cast<LPARAM>(this));
        current_.store(this);
        std::promise<HRESULT> ready;
        auto result = ready.get_future();
        worker_ = std::thread([this, ready = std::move(ready)]() mutable {
            const auto hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, &shown, 0, 0, WINEVENT_OUTOFCONTEXT);
            ready.set_value(hook ? S_OK : HRESULT_FROM_WIN32(GetLastError()));
            while (!stop_.load()) {
                EnumWindows([](HWND window, LPARAM argument) -> BOOL {
                    reinterpret_cast<VisibilityObserver*>(argument)->inspect(window); return TRUE;
                }, reinterpret_cast<LPARAM>(this));
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT); pump();
            }
            if (hook) UnhookWinEvent(hook);
        });
        return result.get();
    }
    ~VisibilityObserver() { stop(); }
    void stop() { stop_.store(true); if (worker_.joinable()) worker_.join(); current_.store(nullptr); }
    bool visible() const { return visible_.load(); }
private:
    static void CALLBACK shown(HWINEVENTHOOK, DWORD, HWND window, LONG object, LONG child, DWORD, DWORD) {
        if (object == OBJID_WINDOW && child == CHILDID_SELF) if (auto current = current_.load()) current->inspect(window);
    }
    void inspect(HWND window) {
        if (!window || !IsWindowVisible(window) || GetAncestor(window, GA_ROOT) != window) return;
        DWORD process = 0; GetWindowThreadProcessId(window, &process);
        if (process == GetCurrentProcessId() || !baseline_.contains(window)) visible_.store(true);
    }
    inline static std::atomic<VisibilityObserver*> current_{nullptr};
    std::unordered_set<HWND> baseline_;
    std::atomic_bool stop_ = false, visible_ = false;
    std::thread worker_;
};
struct KeyCloser { void operator()(HKEY__* key) const { RegCloseKey(key); } };
struct PidlCloser {
    using pointer = PIDLIST_ABSOLUTE;
    void operator()(PIDLIST_ABSOLUTE value) const { CoTaskMemFree(value); }
};
struct RegistryValue {
    DWORD type = 0;
    std::vector<BYTE> bytes;
    bool operator==(const RegistryValue&) const = default;
};
struct SettingsSnapshot {
    std::set<std::wstring> keys;
    std::map<std::pair<std::wstring,std::wstring>, RegistryValue> values;
    bool operator==(const SettingsSnapshot&) const = default;
    void readKey(const std::wstring& path, unsigned depth = 0) {
        require(depth <= 16 && keys.size() <= 256 && values.size() <= 4096, "bound search-setting snapshot");
        HKEY raw = nullptr;
        const LONG opened = RegOpenKeyExW(HKEY_CURRENT_USER,path.c_str(),0,KEY_READ,&raw);
        if (opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND) return;
        succeeded(HRESULT_FROM_WIN32(opened), "read-only search setting key");
        std::unique_ptr<HKEY__,KeyCloser> key(raw);
        keys.insert(path);
        DWORD childCount=0, childLength=0, valueCount=0, nameLength=0, dataLength=0;
        succeeded(HRESULT_FROM_WIN32(RegQueryInfoKeyW(raw,nullptr,nullptr,nullptr,&childCount,&childLength,nullptr,
            &valueCount,&nameLength,&dataLength,nullptr,nullptr)), "search settings metadata");
        require(childCount <= 256 && valueCount <= 4096 && nameLength < 32768 && dataLength <= 1024*1024,
            "search settings snapshot dimensions");
        std::vector<wchar_t> name(static_cast<size_t>(nameLength)+2);
        std::vector<BYTE> data(static_cast<size_t>(dataLength)+1);
        for (DWORD index=0; index<valueCount; ++index) {
            DWORD size=static_cast<DWORD>(name.size()), bytes=static_cast<DWORD>(data.size()), type=0;
            succeeded(HRESULT_FROM_WIN32(RegEnumValueW(raw,index,name.data(),&size,nullptr,&type,data.data(),&bytes)),
                "snapshot actual search setting");
            values.emplace(std::pair{path,std::wstring(name.data(),size)}, RegistryValue{type,{data.begin(),data.begin()+bytes}});
        }
        name.resize(static_cast<size_t>(childLength)+2);
        for (DWORD index=0; index<childCount; ++index) {
            DWORD size=static_cast<DWORD>(name.size());
            succeeded(HRESULT_FROM_WIN32(RegEnumKeyExW(raw,index,name.data(),&size,nullptr,nullptr,nullptr,nullptr)),
                "search setting child key");
            readKey(path+L"\\"+std::wstring(name.data(),size),depth+1);
        }
    }
    static SettingsSnapshot read() {
        SettingsSnapshot snapshot;
        // Settings, not unrelated Explorer MRUs/Bags or the index database.
        for (const wchar_t* suffix : {L"Advanced",L"Search",L"SearchPlatform"})
            snapshot.readKey(std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\")+suffix);
        return snapshot;
    }
};
class SettingsGuard final {
public:
    SettingsGuard() : original_(SettingsSnapshot::read()) {}
    ~SettingsGuard() {
        if constexpr (!ReadOnlyAudit) {
            if (!restored_) try { restore(); } catch (...) { std::cerr << "FAIL: CI-only original search settings restoration failed\n"; }
        }
    }
    void verifyReadOnly() const { require(SettingsSnapshot::read()==original_, "read-only audit unexpectedly changed search settings"); }
    void restore() {
        require(!ReadOnlyAudit && environmentEquals(L"GITHUB_ACTIONS",L"true") &&
            environmentEquals(L"WINDOWSEXPLORER_SEARCH_OPTIONS_TEST",L"1"), "restore registry only in opted-in disposable VM");
        const auto current=SettingsSnapshot::read();
        unsigned differences=0;
        for (const auto& [address,value] : current.values) {
            const auto found=original_.values.find(address);
            if (found!=original_.values.end() && found->second==value) continue;
            ++differences;
            HKEY raw=nullptr;
            succeeded(HRESULT_FROM_WIN32(RegOpenKeyExW(HKEY_CURRENT_USER,address.first.c_str(),0,KEY_SET_VALUE,&raw)),
                "open changed CI search setting for exact restoration");
            std::unique_ptr<HKEY__,KeyCloser> key(raw);
            const LONG result=found==original_.values.end() ? RegDeleteValueW(raw,address.second.c_str()) :
                RegSetValueExW(raw,address.second.c_str(),0,found->second.type,found->second.bytes.data(),static_cast<DWORD>(found->second.bytes.size()));
            succeeded(HRESULT_FROM_WIN32(result), "restore exact original CI search value/absence");
        }
        for (const auto& [address,value] : original_.values) if (!current.values.contains(address)) {
            HKEY raw=nullptr;
            succeeded(HRESULT_FROM_WIN32(RegCreateKeyExW(HKEY_CURRENT_USER,address.first.c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&raw,nullptr)),
                "restore removed original CI search setting key");
            std::unique_ptr<HKEY__,KeyCloser> key(raw);
            succeeded(HRESULT_FROM_WIN32(RegSetValueExW(raw,address.second.c_str(),0,value.type,value.bytes.data(),static_cast<DWORD>(value.bytes.size()))),
                "restore removed original CI search value");
            ++differences;
        }
        // Delete only newly created EMPTY keys, deepest first. Never recursive.
        for (auto key=current.keys.rbegin(); key!=current.keys.rend(); ++key) if (!original_.keys.contains(*key)) {
            const LONG result=RegDeleteKeyW(HKEY_CURRENT_USER,key->c_str());
            require(result==ERROR_SUCCESS || result==ERROR_FILE_NOT_FOUND, "new search key was not empty after restoration");
        }
        require(SettingsSnapshot::read()==original_, "exact search-setting snapshot restoration failed");
        restored_=true;
        std::cout << "Native search setting value differences observed/restored: " << differences << '\n';
    }
private:
    SettingsSnapshot original_; bool restored_=false;
};
ComPtr<IShellItem> shellItem(const fs::path& path) {
    ComPtr<IShellItem> result;
    succeeded(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&result)), "parse owned search fixture");
    return result;
}
std::wstring itemName(IShellItem* item) {
    PWSTR raw = nullptr;
    auto result = item->GetDisplayName(SIGDN_FILESYSPATH, &raw);
    if (FAILED(result)) result = item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &raw);
    const std::wstring name = raw ? raw : L""; CoTaskMemFree(raw);
    succeeded(result, "resolve owned search result identity"); require(!name.empty(), "search result identity empty");
    return name;
}
std::wstring expandedExistingPath(const fs::path& path) {
    const DWORD size=GetLongPathNameW(path.c_str(),nullptr,0);
    require(size && size < 32768, "resolve owned long path dimensions");
    std::vector<wchar_t> value(size);
    const DWORD written=GetLongPathNameW(path.c_str(),value.data(),size);
    require(written && written < size, "resolve owned long path");
    return {value.data(),written};
}
std::vector<std::wstring> ownedScopeAliases(const fs::path& root) {
    // CI TEMP may contain RUNNER~1 while the Shell returns runneradmin.
    // Derive aliases only from the existing, exclusively owned root; never
    // weaken the guard to a filename/suffix check or log an escaped item path.
    auto native=shellItem(root);
    std::vector<std::wstring> aliases{expandedExistingPath(root),root.native(),itemName(native.Get())};
    const DWORD size=GetShortPathNameW(root.c_str(),nullptr,0);
    if (size && size < 32768) {
        std::vector<wchar_t> value(size);
        const DWORD written=GetShortPathNameW(root.c_str(),value.data(),size);
        if (written && written < size) aliases.emplace_back(value.data(),written);
    }
    return aliases;
}
std::set<std::wstring> results(IShellItem* search, const fs::path& root) {
    ComPtr<IShellFolder> folder;
    succeeded(search->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(&folder)), "bind actual query provider");
    ComPtr<IEnumIDList> enumerator;
    succeeded(folder->EnumObjects(nullptr, static_cast<SHCONTF>(SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN | SHCONTF_INCLUDESUPERHIDDEN),
        &enumerator), "enumerate bounded owned query");
    std::set<std::wstring> paths;
    const auto aliases=ownedScopeAliases(root);
    const auto canonicalPrefix=aliases.front()+L"\\";
    while (enumerator) {
        PITEMID_CHILD raw = nullptr;
        const auto next = enumerator->Next(1, &raw, nullptr);
        std::unique_ptr<ITEMIDLIST, PidlCloser> id(raw);
        if (next == S_FALSE) break;
        succeeded(next, "read provider result"); require(raw != nullptr, "provider result PIDL missing");
        ComPtr<IShellItem> item;
        succeeded(SHCreateItemWithParent(nullptr, folder.Get(), raw, IID_PPV_ARGS(&item)), "resolve provider item");
        auto path = itemName(item.Get());
        bool owned=false;
        for (const auto& alias : aliases) {
            const auto prefix=alias+L"\\";
            if (path.size() > prefix.size() && _wcsnicmp(path.c_str(),prefix.c_str(),prefix.size())==0) {
                path=canonicalPrefix+path.substr(prefix.size()); owned=true; break;
            }
        }
        if (!owned) std::cerr << "Owned query scope mismatch: resultCharacters=" << path.size() <<
            " canonicalScopeCharacters=" << canonicalPrefix.size()-1 << " derivedAliases=" << aliases.size() << '\n';
        require(owned,"query result escaped fresh owned scope");
        require(paths.insert(path).second && paths.size() <= 64, "duplicate or unbounded query result");
    }
    return paths;
}
class BrowserEvents final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IExplorerBrowserEvents> {
public:
    bool complete = false; HRESULT status = E_PENDING;
    IFACEMETHODIMP OnNavigationPending(PCIDLIST_ABSOLUTE) override { return S_OK; }
    IFACEMETHODIMP OnViewCreated(IShellView*) override { return S_OK; }
    IFACEMETHODIMP OnNavigationComplete(PCIDLIST_ABSOLUTE) override { status = S_OK; complete = true; return S_OK; }
    IFACEMETHODIMP OnNavigationFailed(PCIDLIST_ABSOLUTE) override { status = E_FAIL; complete = true; return S_OK; }
};
class HiddenBrowser final {
public:
    void initialize() {
        WNDCLASSW type{}; type.lpfnWndProc = DefWindowProcW; type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"WindowsExplorerNativeSearchOptionsHiddenHost";
        require(RegisterClassW(&type) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "register invisible search host");
        window_ = CreateWindowExW(0, type.lpszClassName, L"Owned search-options headless fixture", WS_OVERLAPPEDWINDOW,
            0, 0, 800, 600, nullptr, nullptr, type.hInstance, nullptr);
        require(window_ && !IsWindowVisible(window_), "create invisible search option host");
        succeeded(CoCreateInstance(CLSID_ExplorerBrowser, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&browser_)), "create actual search browser");
        succeeded(browser_->SetOptions(EBO_NOTRAVELLOG | EBO_NOPERSISTVIEWSTATE), "disable search view persistence/travel logging");
        RECT bounds{0,0,800,600}; FOLDERSETTINGS settings{FVM_DETAILS, FWF_AUTOARRANGE};
        succeeded(browser_->Initialize(window_, &bounds, &settings), "initialize hidden native search view");
        events_ = Make<BrowserEvents>(); require(events_ != nullptr, "search navigation observer");
        succeeded(browser_->Advise(events_.Get(), &cookie_), "attach search observer");
    }
    void navigate(IShellItem* search) {
        events_->complete = false; events_->status = E_PENDING;
        succeeded(browser_->BrowseToObject(search, SBSP_ABSOLUTE), "browse to native search provider");
        waitFor([&] { return events_->complete; }, "hidden search navigation timeout");
        succeeded(events_->status, "hidden search navigation");
        require(!IsWindowVisible(window_), "search host was shown");
    }
    ComPtr<IShellView> view() const {
        ComPtr<IShellView> result;
        succeeded(browser_->GetCurrentView(IID_PPV_ARGS(&result)), "actual search view site"); return result;
    }
    ComPtr<IShellItem> current() const {
        ComPtr<IFolderView2> folderView; auto currentView = view();
        succeeded(currentView.As(&folderView), "current folder view");
        ComPtr<IShellItem> result;
        succeeded(folderView->GetFolder(IID_PPV_ARGS(&result)), "native current search item"); return result;
    }
    ~HiddenBrowser() { close(); }
    void close() {
        if (browser_) { if (cookie_) browser_->Unadvise(cookie_); browser_->Destroy(); browser_.Reset(); }
        events_.Reset();
        if (window_) { DestroyWindow(window_); window_ = nullptr; }
    }
    HWND owner() const { return window_; }
private:
    HWND window_ = nullptr; DWORD cookie_ = 0;
    ComPtr<IExplorerBrowser> browser_; ComPtr<BrowserEvents> events_;
};
struct OptionState { bool present = false, enabled = false, checked = false; UINT id = 0, depth = UINT_MAX; };
class OptionMenu final {
public:
    void initialize(HiddenBrowser& browser, const wchar_t* option) {
        option_ = option;
        HKEY raw = nullptr;
        succeeded(HRESULT_FROM_WIN32(RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\CommandStore", 0, KEY_READ, &raw)), "read installed search CommandStore");
        key_.reset(raw);
        auto item = browser.current(); auto site = browser.view();
        ComPtr<IShellFolder> folder;
        succeeded(item->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(&folder)), "bind real search background");
        PIDLIST_ABSOLUTE rawId = nullptr;
        const auto hr = SHGetIDListFromObject(item.Get(), &rawId);
        std::unique_ptr<ITEMIDLIST, PidlCloser> id(rawId); succeeded(hr, "search background PIDL");
        DEFCONTEXTMENU definition{}; definition.hwnd = browser.owner(); definition.pidlFolder = id.get();
        definition.psf = folder.Get(); definition.cKeys = 1; definition.aKeys = &raw;
        succeeded(SHCreateDefaultContextMenu(&definition, IID_PPV_ARGS(&context_)), "registered native search menu");
        succeeded(menu_.create(browser.owner(), context_.Get(), site.Get(), CMF_EXTENDEDVERBS), "site native search command");
        std::vector<explorer::ContextMenuEntry> entries;
        succeeded(menu_.enumerate(entries), "read native advanced option checkbox");
        find(entries, true, 0);
    }
    const OptionState& state() const { return state_; }
    void invoke(HWND owner) {
        require(!ReadOnlyAudit, "read-only audit cannot invoke a native command");
        require(environmentEquals(L"GITHUB_ACTIONS", L"true") && environmentEquals(L"WINDOWSEXPLORER_SEARCH_OPTIONS_TEST", L"1"),
            "both disposable VM gates are required for every native mutation");
        require(state_.present && state_.enabled && state_.id >= menu_.firstCommand(), "only an enabled native search option may be invoked");
        const UINT ordinal = state_.id - menu_.firstCommand();
        require(ordinal < menu_.commandCount(), "native option ordinal outside enumerated menu");
        CMINVOKECOMMANDINFOEX command{}; command.cbSize = sizeof(command);
        command.fMask = CMIC_MASK_UNICODE | CMIC_MASK_FLAG_NO_UI | CMIC_MASK_NOASYNC;
        command.hwnd = owner; command.lpVerb = MAKEINTRESOURCEA(ordinal); command.lpVerbW = MAKEINTRESOURCEW(ordinal); command.nShow = SW_HIDE;
        succeeded(context_->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&command)), "invoke native search option without UI");
    }
private:
    void find(const std::vector<explorer::ContextMenuEntry>& entries, bool enabled, UINT depth) {
        for (const auto& entry : entries) {
            if (!entry.separator() && !entry.submenu && entry.id && _wcsicmp(entry.canonicalVerb.c_str(), option_) == 0) {
                if (depth < state_.depth) state_ = {true, enabled && entry.enabled(), (entry.state & MFS_CHECKED) != 0, entry.id, depth};
                else if (depth == state_.depth) require(false, "ambiguous direct native search option");
            }
            find(entry.children, enabled && entry.enabled(), depth + 1);
        }
    }
    const wchar_t* option_ = nullptr; OptionState state_;
    std::unique_ptr<HKEY__, KeyCloser> key_;
    ComPtr<IContextMenu> context_; explorer::NativeContextMenu menu_;
};
void write(const fs::path& path, const std::string& bytes) {
    std::ofstream stream(path, std::ios::binary); stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(stream.good(), "write owned search fixture");
}
void append16(std::string& bytes, UINT value) { bytes.push_back(static_cast<char>(value)); bytes.push_back(static_cast<char>(value >> 8)); }
void append32(std::string& bytes, UINT value) { append16(bytes,value); append16(bytes,value >> 16); }
void createStoredZip(const fs::path& path) {
    const std::string name = "zipneedle.txt", contents = "owned native ZIP query fixture\r\n";
    UINT crc = 0xffffffff;
    for (const auto byte : contents) {
        crc ^= static_cast<unsigned char>(byte);
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
    }
    crc ^= 0xffffffff;
    std::string bytes;
    append32(bytes,0x04034b50); append16(bytes,20); append16(bytes,0); append16(bytes,0); append16(bytes,0); append16(bytes,0x0021);
    append32(bytes,crc); append32(bytes,static_cast<UINT>(contents.size())); append32(bytes,static_cast<UINT>(contents.size()));
    append16(bytes,static_cast<UINT>(name.size())); append16(bytes,0); bytes += name; bytes += contents;
    const UINT offset = static_cast<UINT>(bytes.size());
    append32(bytes,0x02014b50); append16(bytes,20); append16(bytes,20); append16(bytes,0); append16(bytes,0); append16(bytes,0); append16(bytes,0x0021);
    append32(bytes,crc); append32(bytes,static_cast<UINT>(contents.size())); append32(bytes,static_cast<UINT>(contents.size()));
    append16(bytes,static_cast<UINT>(name.size())); append16(bytes,0); append16(bytes,0); append16(bytes,0); append16(bytes,0); append32(bytes,0); append32(bytes,0); bytes += name;
    const UINT directorySize = static_cast<UINT>(bytes.size()) - offset;
    append32(bytes,0x06054b50); append16(bytes,0); append16(bytes,0); append16(bytes,1); append16(bytes,1);
    append32(bytes,directorySize); append32(bytes,offset); append16(bytes,0); write(path,bytes);
}
struct Fixture {
    fs::path root;
    Fixture() {
        GUID id{}; succeeded(CoCreateGuid(&id), "owned search options ID"); wchar_t name[40]{};
        require(StringFromGUID2(id, name, 40) != 0, "format owned search ID");
        root = fs::temp_directory_path() / (std::wstring(L"WindowsExplorer-NativeSearchOptions-") + name);
        require(fs::create_directory(root) && fs::create_directory(root/L"protected"), "create exclusive owned search scope");
        write(root/L"baseline.txt", "ordinary filename baseline\r\n");
        write(root/L"textonly.txt", "uniquefilecontentneedle\r\n");
        write(root/L"protected"/L"systemneedle.txt", "owned system-folder result\r\n");
        require(SetFileAttributesW((root/L"protected").c_str(), FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_HIDDEN) != FALSE,
            "set attributes only on owned system folder");
        createStoredZip(root/L"owned.zip");
        // Prove the ZIP fixture is accepted by the real compressed-folder
        // provider before any search-option assertion; never extract it.
        auto zip=shellItem(root/L"owned.zip");
        ComPtr<IShellFolder> zipFolder;
        succeeded(zip->BindToHandler(nullptr,BHID_SFObject,IID_PPV_ARGS(&zipFolder)), "validate real ZIP fixture provider");
        ComPtr<IEnumIDList> children;
        succeeded(zipFolder->EnumObjects(nullptr,SHCONTF_NONFOLDERS,&children), "enumerate valid owned ZIP member");
        require(children!=nullptr, "ZIP fixture has no native members");
        PITEMID_CHILD child=nullptr;
        succeeded(children->Next(1,&child,nullptr), "read actual ZIP member");
        std::unique_ptr<ITEMIDLIST,PidlCloser> zipChild(child);
        require(child!=nullptr, "actual ZIP member PIDL missing");
        STRRET display{}, parsing{}; wchar_t displayName[64]{}, parsingName[256]{};
        succeeded(zipFolder->GetDisplayNameOf(child,SHGDN_NORMAL,&display), "read ZIP member display name");
        const UINT displayType=display.uType;
        succeeded(StrRetToBufW(&display,child,displayName,64), "decode ZIP member display name");
        succeeded(zipFolder->GetDisplayNameOf(child,static_cast<SHGDNF>(SHGDN_INFOLDER | SHGDN_FORPARSING),&parsing),
            "read ZIP member canonical parsing name");
        const UINT parsingType=parsing.uType;
        succeeded(StrRetToBufW(&parsing,child,parsingName,256), "decode ZIP member parsing name");
        ComPtr<IShellItem2> zipMember;
        succeeded(SHCreateItemWithParent(nullptr,zipFolder.Get(),child,IID_PPV_ARGS(&zipMember)), "resolve ZIP member canonical metadata");
        PWSTR fileName=nullptr, itemType=nullptr;
        const HRESULT nameStatus=zipMember->GetString(PKEY_FileName,&fileName);
        const HRESULT typeStatus=zipMember->GetString(PKEY_ItemType,&itemType);
        const std::wstring canonicalName=fileName ? fileName : parsingName;
        std::wcout << L"Native ZIP member display='" << displayName << L"' STRRET=" << displayType <<
            L" parsing='" << parsingName << L"' STRRET=" << parsingType << L" filename='" << canonicalName <<
            L"' itemType='" << (itemType ? itemType : L"") << L"' nameHRESULT=0x" << std::hex <<
            static_cast<unsigned long>(nameStatus) << L" typeHRESULT=0x" << static_cast<unsigned long>(typeStatus) << std::dec << L'\n';
        CoTaskMemFree(fileName); CoTaskMemFree(itemType);
        // PKEY_FileName/parsing identity includes the extension even when the
        // runner hides known extensions in normal display labels.
        require(canonicalName==L"zipneedle.txt", "native ZIP fixture canonical filename differs");
        child=nullptr;
        const auto last=children->Next(1,&child,nullptr); CoTaskMemFree(child);
        require(last==S_FALSE, "owned ZIP must contain exactly one native member");
    }
    ~Fixture() {
        if (!root.empty()) {
            SetFileAttributesW((root/L"protected").c_str(), FILE_ATTRIBUTE_DIRECTORY);
            std::error_code ignored; fs::remove_all(root, ignored);
        }
    }
};
ComPtr<IShellItem> query(const wchar_t* text, IShellItem* scope) {
    ComPtr<IShellItem> result;
    succeeded(explorer::createSearchFolder(text, scope, &result), "create app pipeline query without local advanced flags");
    return result;
}
struct TestOption { const wchar_t* command; const wchar_t* query; const wchar_t* expectedSuffix; };
constexpr std::array options{
    TestOption{L"Windows.SearchOptionContents", L"uniquefilecontentneedle", L"\\textonly.txt"},
    TestOption{L"Windows.SearchOptionSystem", L"System.FileName:=\"systemneedle.txt\"", L"\\protected\\systemneedle.txt"},
    TestOption{L"Windows.SearchOptionCompressed", L"System.FileName:=\"zipneedle.txt\"", L"\\owned.zip\\zipneedle.txt"}};
bool matches(const std::set<std::wstring>& items, const wchar_t* suffix) {
    if (items.size() != 1) return false;
    const auto& path = *items.begin(); const auto length = wcslen(suffix);
    return path.size() >= length && _wcsicmp(path.c_str()+path.size()-length, suffix) == 0;
}
void optionState(HiddenBrowser& browser, const wchar_t* option, bool checked) {
    OptionMenu menu; menu.initialize(browser, option);
    require(menu.state().present && menu.state().enabled, "installed native advanced option missing/disabled on real search view");
    if (menu.state().checked != checked) {
        menu.invoke(browser.owner());
        waitFor([&] { OptionMenu updated; updated.initialize(browser,option); return updated.state().enabled && updated.state().checked == checked; },
            "native option checkbox did not reach requested state");
    }
}
void auditAndTest() {
    SettingsGuard settings;
    Fixture fixture; auto scope = shellItem(fixture.root);
    VisibilityObserver visibility; succeeded(visibility.start(), "watch actual Shell providers for visible UI");
    HiddenBrowser browser; browser.initialize();
    auto baseline = query(L"System.FileName:=\"baseline.txt\"", scope.Get()); browser.navigate(baseline.Get());
    require(matches(results(baseline.Get(),fixture.root), L"\\baseline.txt"), "ordinary query provider baseline failed");
    const auto aliases=ownedScopeAliases(fixture.root);
    if (_wcsicmp(aliases.front().c_str(),aliases.back().c_str())!=0) {
        auto shortScope=shellItem(fs::path(aliases.back()));
        auto shortBaseline=query(L"System.FileName:=\"baseline.txt\"",shortScope.Get());
        require(matches(results(shortBaseline.Get(),fs::path(aliases.back())),L"\\baseline.txt"),
            "owned 8.3 query scope did not canonicalize native long-path result");
        std::cout << "Owned 8.3 scope and native long-path results verified without option invocation\n";
    }
    if constexpr (ReadOnlyAudit) {
        auto explicitContents=query(L"System.Search.Contents:uniquefilecontentneedle",scope.Get());
        std::cout << "Explicit documented System.Search.Contents baselineResults=" << results(explicitContents.Get(),fixture.root).size() << '\n';
        auto zipScope=shellItem(fixture.root/L"owned.zip");
        auto explicitZip=query(L"System.FileName:=\"zipneedle.txt\"",zipScope.Get());
        std::cout << "Explicit native ZIP scope baselineResults=" << results(explicitZip.Get(),fixture.root).size() << '\n';
    }
    for (const auto& option : options) {
        auto search = query(option.query,scope.Get()); browser.navigate(search.Get());
        OptionMenu initial; initial.initialize(browser,option.command);
        std::wcout << option.command << L" present=" << initial.state().present << L" enabled=" << initial.state().enabled <<
            L" checked=" << initial.state().checked << L" baselineResults=" << results(search.Get(),fixture.root).size() << L'\n';
        if constexpr (ReadOnlyAudit) continue;
        require(initial.state().present && initial.state().enabled, "native option unavailable: need actual search-view service integration");
        const bool original = initial.state().checked;
        try {
            optionState(browser,option.command,false);
            const auto unchecked = results(browser.current().Get(),fixture.root);
            optionState(browser,option.command,true);
            const auto checked = results(browser.current().Get(),fixture.root);
            auto fresh = query(option.query,scope.Get());
            const auto freshResults = results(fresh.Get(),fixture.root);
            auto checkedQuery = browser.current();
            browser.navigate(fresh.Get());
            OptionMenu freshState; freshState.initialize(browser,option.command);
            std::wcout << L"Existing unchecked=" << unchecked.size() << L" checked=" << checked.size() << L" freshFactory=" << freshResults.size() << L'\n';
            std::wcout << L"Fresh factory native checkbox present=" << freshState.state().present << L" enabled=" <<
                freshState.state().enabled << L" checked=" << freshState.state().checked << L'\n';
            browser.navigate(checkedQuery.Get());
            if (option.command != options[1].command)
                require(unchecked.empty(), "unchecked content/archive option did not exclude owned advanced-query result");
            else if (!unchecked.empty())
                std::cout << "System option does not exclude synthetic SYSTEM-attributed fixture folder when unchecked; not a Windows-system-directory assertion\n";
            require(matches(checked,option.expectedSuffix), "checked native option did not produce exact owned advanced-query result");
            require(matches(freshResults,option.expectedSuffix), "fresh app query factory does not honor native advanced option state");
            optionState(browser,option.command,original);
        } catch (...) {
            try { optionState(browser,option.command,original); } catch (...) { std::cerr << "FAIL: native search option original checkbox restore failed\n"; }
            throw;
        }
        require(!visibility.visible(), "native search advanced option displayed visible UI");
    }
    browser.close(); visibility.stop(); require(!visibility.visible(), "hidden search audit exposed visible UI");
    if constexpr (ReadOnlyAudit) settings.verifyReadOnly();
    else settings.restore();
    std::cout << (ReadOnlyAudit ? "Read-only native advanced-search audit completed; no command invoked\n" :
        "PASS: native Contents/System/Compressed states, exact owned provider results, fresh app factory, restored states, no visible UI\n");
}
} // namespace
int main() {
    if constexpr (!ReadOnlyAudit) {
        if (!environmentEquals(L"GITHUB_ACTIONS",L"true") || !environmentEquals(L"WINDOWSEXPLORER_SEARCH_OPTIONS_TEST",L"1")) {
            std::cout << "SKIP: native search options require disposable GitHub VM and explicit opt-in\n"; return 0;
        }
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    const auto initialized = OleInitialize(nullptr);
    if (FAILED(initialized)) return 2;
    int result = 0;
    try { auditAndTest(); }
    catch (const std::exception& error) { std::cerr << "FAIL: native search option verification: " << error.what() << '\n'; result = 1; }
    OleUninitialize(); return result;
}
