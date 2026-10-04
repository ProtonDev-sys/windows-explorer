#include "explorer/namespace_actions.hpp"
#include "explorer/headless_visual.hpp"
#include "explorer/worker_sta.hpp"

#include <shlobj.h>
#include <shlwapi.h>
#include <propkey.h>
#include <propsys.h>
#include <cscobj.h>
#include <wrl.h>
#include <array>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <cstring>
#include <limits>
#include <new>
#include <mutex>
#include <thread>
#include <utility>

namespace explorer {
namespace {
using Microsoft::WRL::ComPtr;
constexpr wchar_t commandStorePath[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\CommandStore";
constexpr DWORD detailedTargetBudget = 100000;
constexpr unsigned maximumMenuDepth = 16;
constexpr unsigned maximumMenuEntries = 4096;

struct ActionDescription { std::wstring_view command; std::wstring_view label; };
constexpr std::array<ActionDescription, static_cast<size_t>(NamespaceAction::Count)> actions{{
    {L"Windows.DiskFormat", L"Format"}, {L"Windows.Defragment", L"Optimize"},
    {L"Windows.CleanUp", L"Clean up"}, {L"Windows.Eject", L"Eject"}, {L"", L"BitLocker"},
    {L"Windows.mount", L"Mount"}, {L"Windows.DiscImage.burn", L"Burn"},
    {L"Windows.rotate270", L"Rotate left"}, {L"Windows.rotate90", L"Rotate right"},
    {L"Windows.slideshow", L"Slide show"}, {L"Windows.setdesktopwallpaper", L"Set as background"},
    {L"Windows.RecycleBin.RestoreItems", L"Restore the selected items"},
    {L"Windows.RecycleBin.RestoreAll", L"Restore all items"},
    {L"Windows.RecycleBin.Empty", L"Empty Recycle Bin"},
    {L"Windows.runas", L"Run as administrator"}, {L"Windows.Troubleshoot", L"Troubleshoot compatibility"},
    {L"Windows.includeinlibrary", L"Include in library"}, {L"Windows.CscPin", L"Always available offline"},
    {L"Windows.CscWorkOfflineOnline", L"Work offline"}, {L"Windows.CscSync", L"Sync"},
    {L"Windows.connectNetworkDrive", L"Map as drive"},
    {L"Windows.ShareSpecificUsers", L"Specific people"}, {L"Windows.SharePrivate", L"Remove access"},
    {L"Windows.RibbonPermissionsDialog", L"Advanced security"},
    {L"Windows.HistoryVaultRestore", L"History"}, {L"Windows.email", L"Email"},
    {L"Windows.fax", L"Fax"}, {L"Windows.burn", L"Burn to disc"},
    {L"Windows.play", L"Play"}, {L"Windows.playall", L"Play all"},
    {L"Windows.Enqueue", L"Add to playlist"}, {L"Windows.PlayTo", L"Cast to device"}
}};

bool equal(std::wstring_view left, std::wstring_view right) noexcept {
    return left.size() == right.size() && CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
        right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool validCommand(std::wstring_view command) noexcept {
    if (command.size() < 9 || command.size() > 128 || !equal(command.substr(0, 8), L"Windows.")) return false;
    for (wchar_t ch : command.substr(8)) {
        if (!((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') ||
              (ch >= L'0' && ch <= L'9') || ch == L'.' || ch == L'_')) return false;
    }
    return command.back() != L'.';
}

struct TaskMemoryDelete { void operator()(void* value) const noexcept { CoTaskMemFree(value); } };
struct PidlDelete {
    using pointer = PIDLIST_ABSOLUTE;
    void operator()(PIDLIST_ABSOLUTE value) const noexcept { CoTaskMemFree(value); }
};
using OwnedPidl = std::unique_ptr<ITEMIDLIST_ABSOLUTE, PidlDelete>;
using OwnedText = std::unique_ptr<wchar_t, TaskMemoryDelete>;
struct RegistryKey {
    HKEY value = nullptr;
    ~RegistryKey() { if (value) RegCloseKey(value); }
    RegistryKey() = default;
    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;
    void reset() noexcept { if (value) RegCloseKey(value); value = nullptr; }
};

HRESULT offlineStatus(BOOL& active, BOOL& enabled) {
    const HMODULE module = LoadLibraryExW(L"cscapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return HRESULT_FROM_WIN32(GetLastError());
    using QueryStatus = DWORD (WINAPI*)(BOOL*, BOOL*);
    const auto function = reinterpret_cast<QueryStatus>(GetProcAddress(module, "OfflineFilesQueryStatus"));
    const DWORD result = function ? function(&active, &enabled) : ERROR_PROC_NOT_FOUND;
    FreeLibrary(module);
    return HRESULT_FROM_WIN32(result);
}

struct OfflineCompletion {
    std::atomic<bool> cancelled{false};
    std::atomic<bool> finished{false};
    std::atomic<HRESULT> firstFailure{S_OK};
    HWND owner = nullptr;
    UINT message = 0;
};

class OfflineProgress final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
    IOfflineFilesSyncProgress, Microsoft::WRL::FtmBase> {
public:
    explicit OfflineProgress(std::shared_ptr<OfflineCompletion> completion) : completion_(std::move(completion)) {}
    HRESULT STDMETHODCALLTYPE Begin(BOOL* abort) override { return QueryAbort(abort); }
    HRESULT STDMETHODCALLTYPE QueryAbort(BOOL* abort) override {
        if (!abort) return E_POINTER;
        *abort = completion_->cancelled.load() ? TRUE : FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE End(HRESULT result) override {
        if (!completion_->finished.exchange(true) && !completion_->cancelled.load()) {
            if (SUCCEEDED(result)) result = completion_->firstFailure.load();
            if (completion_->message && IsWindow(completion_->owner))
                PostMessageW(completion_->owner, completion_->message,
                             static_cast<WPARAM>(NamespaceAction::AlwaysAvailableOffline), static_cast<LPARAM>(result));
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SyncItemBegin(LPCWSTR, OFFLINEFILES_OP_RESPONSE* response) override {
        if (!response) return E_POINTER;
        *response = completion_->cancelled.load() ? OFFLINEFILES_OP_ABORT : OFFLINEFILES_OP_CONTINUE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SyncItemResult(LPCWSTR, HRESULT result, IOfflineFilesSyncErrorInfo*,
                                              OFFLINEFILES_OP_RESPONSE* response) override {
        if (!response) return E_POINTER;
        if (FAILED(result)) { HRESULT expected = S_OK; completion_->firstFailure.compare_exchange_strong(expected, result); }
        *response = completion_->cancelled.load() ? OFFLINEFILES_OP_ABORT : OFFLINEFILES_OP_CONTINUE;
        return S_OK;
    }
private:
    std::shared_ptr<OfflineCompletion> completion_;
};

HRESULT readRegistryText(HKEY key, const wchar_t* name, std::wstring& result) {
    DWORD bytes = 0;
    LONG error = RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                              nullptr, nullptr, &bytes);
    if (error != ERROR_SUCCESS) return HRESULT_FROM_WIN32(error);
    if (bytes < sizeof(wchar_t) || bytes > 65536 || bytes % sizeof(wchar_t))
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    std::wstring text(bytes / sizeof(wchar_t), L'\0');
    error = RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                         nullptr, text.data(), &bytes);
    if (error != ERROR_SUCCESS) return HRESULT_FROM_WIN32(error);
    if (!bytes || bytes % sizeof(wchar_t) || bytes / sizeof(wchar_t) > text.size())
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    text.resize(bytes / sizeof(wchar_t));
    if (text.back() != L'\0') return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    text.pop_back();
    if (text.find(L'\0') != std::wstring::npos) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    result = std::move(text);
    return S_OK;
}

std::wstring loadIndirect(const std::wstring& text) {
    if (text.empty() || text.front() != L'@') return text;
    std::array<wchar_t, 32768> buffer{};
    if (FAILED(SHLoadIndirectString(text.c_str(), buffer.data(), static_cast<UINT>(buffer.size()), nullptr))) return {};
    return buffer.data();
}

bool guidText(std::wstring_view value) noexcept {
    if (value.size() != 38 || value.front() != L'{' || value.back() != L'}') return false;
    for (size_t index = 1; index < 37; ++index) {
        const wchar_t ch = value[index];
        if (index == 9 || index == 14 || index == 19 || index == 24) {
            if (ch != L'-') return false;
        } else if (!((ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f') || (ch >= L'A' && ch <= L'F'))) return false;
    }
    return true;
}

class RegistryPropertyBag final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IPropertyBag> {
public:
    explicit RegistryPropertyBag(const std::wstring& path) {
        status_ = HRESULT_FROM_WIN32(RegOpenKeyExW(HKEY_LOCAL_MACHINE,path.c_str(),0,KEY_READ,&key_.value));
    }
    HRESULT status() const noexcept { return status_; }
    HRESULT STDMETHODCALLTYPE Read(LPCOLESTR name,VARIANT* output,IErrorLog*) override {
        if (!name || !output) return E_POINTER;
        if (FAILED(status_)) return status_;
        try {
            DWORD type = 0,bytes = 0;
            LONG error = RegQueryValueExW(key_.value,name,nullptr,&type,nullptr,&bytes);
            if (error != ERROR_SUCCESS) return HRESULT_FROM_WIN32(error);
            if (bytes > 65536) return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
            std::vector<unsigned char> data(bytes + sizeof(wchar_t),0);
            error = RegQueryValueExW(key_.value,name,nullptr,&type,data.data(),&bytes);
            if (error != ERROR_SUCCESS) return HRESULT_FROM_WIN32(error);
            VARIANT decoded{};
            HRESULT hr = S_OK;
            if (type == REG_SZ || type == REG_EXPAND_SZ) {
                if (bytes % sizeof(wchar_t)) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                const auto text = reinterpret_cast<const wchar_t*>(data.data());
                size_t length = bytes / sizeof(wchar_t);
                if (length && text[length - 1] == L'\0') --length;
                if (wcsnlen_s(text,length + 1) != length) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                decoded.vt = VT_BSTR;
                decoded.bstrVal = SysAllocStringLen(text,static_cast<UINT>(length));
                if (!decoded.bstrVal) return E_OUTOFMEMORY;
            } else if (type == REG_DWORD && bytes == sizeof(DWORD)) {
                decoded.vt = VT_UI4;
                memcpy(&decoded.ulVal,data.data(),sizeof(DWORD));
            } else if (type == REG_QWORD && bytes == sizeof(ULONGLONG)) {
                decoded.vt = VT_UI8;
                memcpy(&decoded.ullVal,data.data(),sizeof(ULONGLONG));
            } else if (type == REG_NONE && !bytes) {
                decoded.vt = VT_EMPTY;
            } else if (type == REG_BINARY) {
                decoded.vt = VT_ARRAY | VT_UI1;
                decoded.parray = SafeArrayCreateVector(VT_UI1,0,bytes);
                if (!decoded.parray) return E_OUTOFMEMORY;
                void* buffer = nullptr;
                hr = SafeArrayAccessData(decoded.parray,&buffer);
                if (SUCCEEDED(hr)) { if (bytes) memcpy(buffer,data.data(),bytes); SafeArrayUnaccessData(decoded.parray); }
            } else return DISP_E_TYPEMISMATCH;
            VARIANT converted{};
            if (SUCCEEDED(hr)) hr = output->vt == VT_EMPTY || output->vt == decoded.vt
                ? VariantCopy(&converted,&decoded) : VariantChangeType(&converted,&decoded,0,output->vt);
            VariantClear(&decoded);
            if (FAILED(hr)) { VariantClear(&converted); return hr; }
            hr = VariantClear(output);
            if (FAILED(hr)) { VariantClear(&converted); return hr; }
            *output = converted;
            return S_OK;
        } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
          catch (...) { return E_FAIL; }
    }
    HRESULT STDMETHODCALLTYPE Write(LPCOLESTR,VARIANT*) override { return E_ACCESSDENIED; }
private:
    RegistryKey key_;
    HRESULT status_ = E_PENDING;
};

struct RegisteredProvider {
    CLSID handler = CLSID_NULL;
    ComPtr<IExplorerCommand> command;
    ComPtr<IExplorerCommandState> state;
    ComPtr<IUnknown> object;
    ComPtr<IObjectWithSite> withSite;
    bool initialized = false;
    bool siteAttached = false;
    ~RegisteredProvider() { if (siteAttached && withSite) withSite->SetSite(nullptr); }
};

HRESULT loadRegisteredProvider(std::wstring_view command,IUnknown* site,bool allowStateHandler,
                               RegisteredProvider& result) {
    APTTYPE apartment{};
    APTTYPEQUALIFIER qualifier{};
    HRESULT hr = CoGetApartmentType(&apartment,&qualifier);
    if (FAILED(hr)) return hr;
    if (apartment != APTTYPE_STA && apartment != APTTYPE_MAINSTA) return RPC_E_WRONG_THREAD;
    const std::wstring name(command);
    const std::wstring path = std::wstring(commandStorePath) + L"\\shell\\" + name;
    RegistryKey key;
    const LONG error = RegOpenKeyExW(HKEY_LOCAL_MACHINE,path.c_str(),0,KEY_READ,&key.value);
    if (error != ERROR_SUCCESS) return HRESULT_FROM_WIN32(error);
    std::wstring handler;
    hr = readRegistryText(key.value,L"ExplorerCommandHandler",handler);
    const bool explorerCommand = SUCCEEDED(hr);
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) && allowStateHandler)
        hr = readRegistryText(key.value,L"CommandStateHandler",handler);
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    if (FAILED(hr)) return hr;
    if (!guidText(handler)) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    hr = CLSIDFromString(handler.c_str(),&result.handler);
    if (FAILED(hr)) return hr;
    if (explorerCommand) {
        hr = CoCreateInstance(result.handler,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&result.command));
        if (SUCCEEDED(hr)) hr = result.command.As(&result.object);
    } else {
        hr = CoCreateInstance(result.handler,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&result.state));
        if (SUCCEEDED(hr)) hr = result.state.As(&result.object);
    }
    if (FAILED(hr)) return hr;
    ComPtr<IInitializeCommand> initialize;
    hr = result.object.As(&initialize);
    if (SUCCEEDED(hr)) {
        auto bag = Microsoft::WRL::Make<RegistryPropertyBag>(path);
        if (!bag) return E_OUTOFMEMORY;
        hr = bag->status();
        if (SUCCEEDED(hr)) hr = initialize->Initialize(name.c_str(),bag.Get());
        if (FAILED(hr)) return hr;
        result.initialized = true;
    } else if (hr != E_NOINTERFACE) return hr;
    hr = result.object.As(&result.withSite);
    if (FAILED(hr) && hr != E_NOINTERFACE) return hr;
    if (site && result.withSite) {
        result.siteAttached = true;
        hr = result.withSite->SetSite(site);
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}

HRESULT registeredCommandState(std::wstring_view command,IShellItemArray* selection,IUnknown* site,
                               BOOL slow,bool background,NamespaceCommandState* result) {
    RegisteredProvider provider;
    HRESULT hr = loadRegisteredProvider(command,site,true,provider);
    if (FAILED(hr)) return hr;
    NamespaceCommandState state;
    state.handler = provider.handler;
    state.explorerCommand = provider.command != nullptr;
    state.initialized = provider.initialized;
    state.siteAttached = provider.siteAttached;
    hr = provider.command ? provider.command->GetState(background ? nullptr : selection,slow,&state.state)
                          : provider.state->GetState(selection,slow,&state.state);
    if (SUCCEEDED(hr)) { *result = std::move(state); return hr; }

    // This installed composite has no public initialization interface. The
    // native menu initializes/arbitrates it; read-only capabilities instead
    // ask the exact registered leaves named by its VerbList. Keep this fallback
    // confined to the composite corroborated against actual Search menus.
    if (hr != E_UNEXPECTED || !equal(command,L"Windows.SearchOpenLocation")) return hr;
    const CLSID composite{0xafa470fe,0x371d,0x4f98,{0x95,0x92,0x39,0xe3,0xc7,0x22,0x7e,0x5c}};
    if (provider.handler != composite) return hr;
    const std::wstring path = std::wstring(commandStorePath) + L"\\shell\\" + std::wstring(command);
    RegistryKey key;
    LONG error = RegOpenKeyExW(HKEY_LOCAL_MACHINE,path.c_str(),0,KEY_READ,&key.value);
    if (error != ERROR_SUCCESS) return HRESULT_FROM_WIN32(error);
    DWORD type = 0,bytes = 0;
    error = RegQueryValueExW(key.value,L"VerbList",nullptr,&type,nullptr,&bytes);
    if (error != ERROR_SUCCESS) return HRESULT_FROM_WIN32(error);
    if (type != REG_MULTI_SZ || bytes < 2*sizeof(wchar_t) || bytes > 65536 || bytes % sizeof(wchar_t))
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    std::vector<wchar_t> names(bytes/sizeof(wchar_t));
    error = RegQueryValueExW(key.value,L"VerbList",nullptr,&type,reinterpret_cast<BYTE*>(names.data()),&bytes);
    if (error != ERROR_SUCCESS) return HRESULT_FROM_WIN32(error);
    if (type != REG_MULTI_SZ || bytes != names.size()*sizeof(wchar_t) ||
        names.back() != L'\0' || names[names.size()-2] != L'\0') return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    std::vector<std::wstring_view> leaves;
    for (size_t offset=0;offset+1<names.size() && names[offset];) {
        const size_t length = wcsnlen_s(names.data()+offset,names.size()-offset);
        const std::wstring_view leaf(names.data()+offset,length);
        if (length == names.size()-offset || !validCommand(leaf) || equal(leaf,command) || leaves.size() >= 16)
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        for (const auto previous : leaves) if (equal(previous,leaf)) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        leaves.push_back(leaf);
        offset += length+1;
    }
    if (leaves.size() != 2 || !equal(leaves[0],L"Windows.OpenContainingFolder.opencontaining") ||
        !equal(leaves[1],L"Windows.OpenSearch.openfilelocation")) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    HRESULT failure = S_OK,chosenStatus = E_UNEXPECTED;
    bool pending = false,chosen = false;
    NamespaceCommandState delegated;
    for (const auto leaf : leaves) {
        NamespaceCommandState candidate;
        const HRESULT status = registeredCommandState(leaf,selection,site,slow,background,&candidate);
        if (status == E_PENDING) pending = true;
        else if (FAILED(status) && SUCCEEDED(failure)) failure = status;
        if (SUCCEEDED(status) && (!chosen || (!delegated.enabled() && candidate.enabled()))) {
            candidate.delegatedCommand = leaf;
            delegated = std::move(candidate);
            chosenStatus = status;
            chosen = true;
        }
    }
    if (pending) return E_PENDING;
    // A missing/failing leaf must not disappear behind a successful sibling.
    // This also preserves the provider's real multiselect failure.
    if (FAILED(failure)) return failure;
    if (!chosen) return E_UNEXPECTED;
    *result = std::move(delegated);
    return chosenStatus;
}

HRESULT commandChildren(IExplorerCommand* command, IShellItemArray* selection,
                        unsigned depth, unsigned& budget,
                        std::vector<NamespaceSubcommandMetadata>& result) {
    if (depth > 8) return HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES);
    ComPtr<IEnumExplorerCommand> enumerator;
    HRESULT hr = command->EnumSubCommands(&enumerator);
    if (hr == E_NOTIMPL || hr == E_NOINTERFACE || hr == S_FALSE) return S_FALSE;
    if (FAILED(hr)) return hr;
    if (!enumerator) return S_FALSE;
    std::vector<NamespaceSubcommandMetadata> children;
    for (;;) {
        ComPtr<IExplorerCommand> child;
        ULONG fetched = 0;
        hr = enumerator->Next(1, &child, &fetched);
        if (hr == S_FALSE && !fetched) break;
        if (FAILED(hr)) return hr;
        if (!child || fetched != 1) return E_UNEXPECTED;
        if (!budget) return HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES);
        --budget;
        NamespaceSubcommandMetadata metadata;
        child->GetCanonicalName(&metadata.canonicalName);
        hr = child->GetFlags(&metadata.flags);
        if (FAILED(hr)) return hr;
        PWSTR raw = nullptr;
        hr = child->GetTitle(selection, &raw);
        OwnedText label(raw);
        if (FAILED(hr) && !(metadata.flags & ECF_ISSEPARATOR)) return hr;
        if (label) metadata.label = label.get();
        raw = nullptr;
        hr = child->GetIcon(selection, &raw);
        OwnedText icon(raw);
        if (SUCCEEDED(hr) && icon) metadata.icon = icon.get();
        raw = nullptr;
        hr = child->GetToolTip(selection, &raw);
        OwnedText description(raw);
        if (SUCCEEDED(hr) && description) metadata.description = description.get();
        metadata.stateStatus = child->GetState(selection, FALSE, &metadata.state);
        if (FAILED(metadata.stateStatus)) metadata.state = ECS_DISABLED;
        if ((metadata.flags & ECF_HASSUBCOMMANDS) && SUCCEEDED(metadata.stateStatus) &&
            !(metadata.state & (ECS_DISABLED | ECS_HIDDEN))) {
            hr = commandChildren(child.Get(), selection, depth + 1, budget, metadata.children);
            if (FAILED(hr)) return hr;
        }
        children.push_back(std::move(metadata));
    }
    result = std::move(children);
    return S_OK;
}

struct MenuMatch {
    const ContextMenuEntry* item = nullptr;
    bool ancestorsEnabled = true;
    unsigned depth = std::numeric_limits<unsigned>::max();
    bool ambiguous = false;
};

void findVerb(const std::vector<ContextMenuEntry>& entries, std::wstring_view verb,
              bool ancestorsEnabled, unsigned depth, unsigned& budget, MenuMatch& match) {
    if (depth > maximumMenuDepth) return;
    for (const auto& entry : entries) {
        if (!budget) return;
        --budget;
        if (!entry.separator() && !entry.canonicalVerb.empty() && equal(entry.canonicalVerb, verb)) {
            if (depth < match.depth) match = {&entry, ancestorsEnabled, depth, false};
            else if (depth == match.depth && match.item) match.ambiguous = true;
        }
        findVerb(entry.children, verb, ancestorsEnabled && entry.enabled(), depth + 1, budget, match);
    }
}

bool enabledLeaf(const std::vector<ContextMenuEntry>& entries, unsigned depth, unsigned& budget) {
    if (depth > maximumMenuDepth) return false;
    for (const auto& entry : entries) {
        if (!budget) return false;
        --budget;
        if (entry.separator() || !entry.enabled()) continue;
        if (!entry.submenu && entry.id && entry.id <= 0x7fff) return true;
        if (entry.submenu && enabledLeaf(entry.children, depth + 1, budget)) return true;
    }
    return false;
}

unsigned commandOccurrences(const std::vector<ContextMenuEntry>& entries, UINT id,
                            unsigned depth, unsigned& budget) {
    if (depth > maximumMenuDepth) return 0;
    unsigned matches = 0;
    for (const auto& entry : entries) {
        if (!budget) return matches;
        --budget;
        if (!entry.separator() && entry.id == id) ++matches;
        matches += commandOccurrences(entry.children, id, depth + 1, budget);
    }
    return matches;
}

HRESULT planVerb(const std::vector<ContextMenuEntry>& entries, std::wstring_view verb,
                 NamespaceInvocationRoute route, NamespaceInvocationPlan& plan) {
    unsigned budget = maximumMenuEntries;
    MenuMatch match;
    findVerb(entries, verb, true, 0, budget, match);
    if (!match.item) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    if (match.ambiguous) return E_UNEXPECTED;
    const auto& entry = *match.item;
    if (!entry.id || entry.id > 0x7fff) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    budget = maximumMenuEntries;
    if (commandOccurrences(entries, entry.id, 0, budget) != 1 || !budget) return E_UNEXPECTED;
    plan.route = route;
    plan.commandId = entry.id;
    plan.canonicalVerb = entry.canonicalVerb;
    plan.submenu = entry.submenu;
    plan.checked=(entry.state&MFS_CHECKED)!=0;
    budget = maximumMenuEntries;
    plan.enabled = match.ancestorsEnabled && entry.enabled() &&
        (!entry.submenu || enabledLeaf(entry.children, 0, budget));
    plan.status = plan.enabled ? S_OK : E_ACCESSDENIED;
    return S_OK;
}

std::vector<std::wstring_view> nativeVerbs(NamespaceAction action) {
    using A = NamespaceAction;
    switch (action) {
    case A::FormatDrive: return {L"format"};
    case A::OptimizeDrive: return {L"defragment"};
    case A::CleanUpDrive: return {L"cleanup"};
    case A::EjectDrive: return {L"eject"};
    case A::BitLocker: return {L"manage-bde", L"unlock-bde", L"encrypt-bde", L"resume-bde", L"en-bde", L"dis-bde"};
    case A::MountDiscImage: return {L"mount"};
    case A::BurnDiscImage: return {L"burn"};
    case A::RotateLeft: return {L"rotate270"};
    case A::RotateRight: return {L"rotate90"};
    case A::SlideShow: return {L"slideshow"};
    case A::SetWallpaper: return {L"setdesktopwallpaper"};
    case A::RestoreSelected: return {L"undelete", L"restore"};
    case A::RunAsAdministrator: return {L"runas"};
    case A::TroubleshootCompatibility: return {L"Troubleshoot", L"compatibility"};
    case A::IncludeInLibrary: return {L"Windows.includeinlibrary", L"library"};
    case A::AlwaysAvailableOffline: return {L"cscpin", L"Windows.CscPin"};
    case A::WorkOffline: return {L"Windows.CscWorkOfflineOnline"};
    case A::SyncOffline: return {L"Windows.CscSync"};
    case A::MapAsDrive: return {L"connectNetworkDrive"};
    case A::ShareSpecificPeople: return {L"Windows.ShareSpecificUsers"};
    case A::RemoveAccess: return {L"Windows.SharePrivate"};
    case A::AdvancedSecurity: return {L"Windows.RibbonPermissionsDialog"};
    case A::FileHistory: return {L"Windows.HistoryVaultRestore"};
    case A::Email: return {L"Windows.email"};
    case A::Fax: return {L"Windows.fax"};
    case A::BurnToDisc: return {L"Windows.burn"};
    case A::Play: return {L"play"};
    case A::PlayAll: return {L"playall", L"playmusic"};
    case A::AddToPlaylist: return {L"enqueue"};
    case A::CastToDevice: return {L"PlayTo", L"casttodevice"};
    default: return {};
    }
}

bool applicable(NamespaceAction action, const NamespaceFacts& facts) {
    using A = NamespaceAction;
    const bool one = facts.selectionCount <= 1;
    const bool selected = facts.selectionCount != 0;
    // Unknown optional metadata permits an exact provider query/menu plan,
    // never a guessed enabled command or an unchecked public API fallback.
    const bool providerFileCandidate = !facts.detailedTargetsKnown && facts.filesystem &&
        !(facts.nativeAttributes & SFGAO_FOLDER);
    const bool providerFilesystemCandidate = !facts.detailedTargetsKnown && facts.filesystem;
    const bool localDrive = facts.driveRoot && one &&
        (facts.driveType == DRIVE_FIXED || facts.driveType == DRIVE_REMOVABLE || facts.driveType == DRIVE_RAMDISK);
    switch (action) {
    case A::FormatDrive: case A::OptimizeDrive: case A::CleanUpDrive: return localDrive;
    case A::EjectDrive: return facts.driveRoot && one &&
        (facts.driveType == DRIVE_REMOVABLE || facts.driveType == DRIVE_CDROM || facts.driveType == DRIVE_FIXED);
    case A::BitLocker: return localDrive;
    case A::MountDiscImage: return selected && one && facts.physicalFiles && facts.discImages;
    case A::BurnDiscImage: return selected && one && facts.physicalFiles && facts.discImages &&
        !equal(PathFindExtensionW(facts.singlePath.c_str()), L".vhd") &&
        !equal(PathFindExtensionW(facts.singlePath.c_str()), L".vhdx");
    case A::RotateLeft: case A::RotateRight: case A::SlideShow:
        return selected && ((facts.physicalFiles && facts.images) || providerFileCandidate);
    case A::SetWallpaper: return selected && one && facts.physicalFiles && facts.images;
    case A::RestoreSelected: return facts.recycleBin && selected;
    case A::RestoreAll: case A::EmptyRecycleBin: return facts.recycleBin;
    case A::RunAsAdministrator: case A::TroubleshootCompatibility:
        return selected && one && facts.physicalFiles && facts.applications;
    case A::IncludeInLibrary: return one && facts.physicalFolders && !facts.driveRoot;
    case A::AlwaysAvailableOffline: case A::WorkOffline: case A::SyncOffline:
        return (facts.filesystem && facts.uncPaths) || providerFilesystemCandidate;
    case A::MapAsDrive: return one && facts.physicalFolders && facts.uncPaths;
    case A::ShareSpecificPeople: case A::RemoveAccess: return facts.filesystem;
    case A::AdvancedSecurity: return facts.filesystem && one;
    case A::FileHistory: return facts.filesystem && one && !facts.driveRoot;
    case A::Email: case A::Fax: return selected && (facts.physicalFiles || providerFileCandidate);
    case A::BurnToDisc: return selected && facts.filesystem && !facts.driveRoot;
    case A::Play: case A::PlayAll: case A::AddToPlaylist:
        return selected && ((facts.physicalFiles && facts.media) || providerFileCandidate);
    case A::CastToDevice:
        return selected && ((facts.physicalFiles && (facts.media || facts.images || facts.castItems)) || providerFileCandidate);
    default: return false;
    }
}

HRESULT itemPath(IShellItem* item, std::wstring& path) {
    PWSTR raw = nullptr;
    const HRESULT hr = item->GetDisplayName(SIGDN_FILESYSPATH, &raw);
    OwnedText owned(raw);
    if (FAILED(hr)) return hr;
    if (!raw || !*raw) return E_UNEXPECTED;
    path = raw;
    return S_OK;
}

bool extensionIs(std::wstring_view extension, std::initializer_list<std::wstring_view> options) {
    return std::any_of(options.begin(), options.end(), [extension](auto candidate) { return equal(extension, candidate); });
}

HRESULT residentShortcutExtension(const std::wstring& path,DWORD attributes,std::wstring& result) {
    constexpr DWORD unavailable = FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE |
        FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS;
    if ((attributes & unavailable) || PathIsUNCW(path.c_str())) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    struct File { HANDLE value=INVALID_HANDLE_VALUE;~File(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);} } file;
    file.value=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                          FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL | FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if(file.value==INVALID_HANDLE_VALUE)return HRESULT_FROM_WIN32(GetLastError());
    FILE_ATTRIBUTE_TAG_INFO tag{};FILE_STANDARD_INFO information{};
    if(!GetFileInformationByHandleEx(file.value,FileAttributeTagInfo,&tag,sizeof(tag)) ||
       !GetFileInformationByHandleEx(file.value,FileStandardInfo,&information,sizeof(information)))return HRESULT_FROM_WIN32(GetLastError());
    if((tag.FileAttributes & unavailable) || information.Directory || information.DeletePending ||
       information.EndOfFile.QuadPart<76 || information.EndOfFile.QuadPart>1024*1024)return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    std::vector<BYTE> bytes(static_cast<size_t>(information.EndOfFile.QuadPart));DWORD fetched=0;
    if(!ReadFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&fetched,nullptr))return HRESULT_FROM_WIN32(GetLastError());
    if(fetched!=bytes.size())return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
    DWORD header=0;CLSID identity{};
    std::memcpy(&header,bytes.data(),sizeof(header));std::memcpy(&identity,bytes.data()+sizeof(header),sizeof(identity));
    if(header!=76 || !IsEqualGUID(identity,CLSID_ShellLink))return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    ComPtr<IStream> stream;stream.Attach(SHCreateMemStream(bytes.data(),static_cast<UINT>(bytes.size())));
    if(!stream)return E_OUTOFMEMORY;
    ComPtr<IShellLinkW> link;HRESULT hr=CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&link));
    ComPtr<IPersistStream> persistence;
    if(SUCCEEDED(hr))hr=link.As(&persistence);
    if(SUCCEEDED(hr))hr=persistence->Load(stream.Get());
    if(FAILED(hr))return hr;
    std::array<wchar_t,32768> target{};
    // Read only the path stored in the selected resident shortcut. Do not call
    // Resolve, expand its environment, bind its PIDL or request target data.
    hr=link->GetPath(target.data(),static_cast<int>(target.size()),nullptr,SLGP_RAWPATH);
    if(hr!=S_OK || !target.front())return FAILED(hr)?hr:HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    result=PathFindExtensionW(target.data());return S_OK;
}

HRESULT describeTargets(IShellItem* folder, IShellItemArray* targets, DWORD selected,
                        NamespaceFacts& facts) {
    facts.selectionCount = selected;
    DWORD count = 0;
    HRESULT hr = targets->GetCount(&count);
    if (FAILED(hr)) return hr;
    if (!count) return E_INVALIDARG;
    // ALLITEMS is required here: the default native array can sample a large
    // prefix, which may miss a counterexample at the end. These attributes
    // determine eligibility, so completeness outweighs prefix-only speed.
    facts.nativeAttributesStatus = targets->GetAttributes(
        static_cast<SIATTRIBFLAGS>(SIATTRIBFLAGS_AND | SIATTRIBFLAGS_ALLITEMS),
        SFGAO_FILESYSTEM | SFGAO_FOLDER | SFGAO_LINK,&facts.nativeAttributes);
    if (FAILED(facts.nativeAttributesStatus)) return facts.nativeAttributesStatus;
    facts.filesystem = (facts.nativeAttributes & SFGAO_FILESYSTEM) != 0;
    facts.detailedTargetsKnown = count <= detailedTargetBudget;
    facts.physicalFiles = facts.physicalFolders = facts.detailedTargetsKnown;
    facts.images = facts.applications = facts.discImages = facts.media = facts.castItems = facts.uncPaths = facts.detailedTargetsKnown;
    bool singleNativeFolder = false;
    for (DWORD index = 0; facts.detailedTargetsKnown && index < count; ++index) {
        ComPtr<IShellItem> item;
        hr = targets->GetItemAt(index, &item);
        if (FAILED(hr)) return hr;
        SFGAOF flags = 0;
        hr = item->GetAttributes(SFGAO_FILESYSTEM | SFGAO_FOLDER, &flags);
        if (FAILED(hr)) return hr;
        std::wstring path;
        if (!(flags & SFGAO_FILESYSTEM) || FAILED(itemPath(item.Get(), path))) {
            facts.physicalFiles = facts.physicalFolders = false;
            facts.images = facts.applications = facts.discImages = facts.media = facts.castItems = facts.uncPaths = false;
            continue;
        }
        if (count == 1) singleNativeFolder = (flags & SFGAO_FOLDER) != 0;
        const DWORD attributes = GetFileAttributesW(path.c_str());
        const bool exists = attributes != INVALID_FILE_ATTRIBUTES;
        const bool directory = exists && (attributes & FILE_ATTRIBUTE_DIRECTORY);
        facts.physicalFiles &= exists && !directory;
        facts.physicalFolders &= directory;
        facts.uncPaths &= path.size() > 4 && path[0] == L'\\' && path[1] == L'\\' && path[2] != L'?' && path[2] != L'.';
        const auto extension = std::wstring_view(PathFindExtensionW(path.c_str()));
        PERCEIVED type = PERCEIVED_TYPE_UNSPECIFIED;
        ComPtr<IShellItem2> item2;
        ComPtr<IPropertyStore> fastProperties;
        const bool perceivedCandidate = facts.images || facts.media || facts.castItems;
        const bool applicationLink = facts.applications && equal(extension,L".lnk");
        if (perceivedCandidate) {
            PERCEIVEDFLAG perceivedFlags = 0;
            AssocGetPerceivedType(std::wstring(extension).c_str(), &type, &perceivedFlags, nullptr);
        }
        if ((perceivedCandidate || applicationLink) && SUCCEEDED(item.As(&item2)))
            item2->GetPropertyStore(GPS_FASTPROPERTIESONLY,IID_PPV_ARGS(&fastProperties));
        if (perceivedCandidate && fastProperties) {
            PROPVARIANT perceived{};
            if (SUCCEEDED(fastProperties->GetValue(PKEY_PerceivedType, &perceived)) && perceived.vt == VT_I4)
                type = static_cast<PERCEIVED>(perceived.lVal);
            PropVariantClear(&perceived);
        }
        facts.images &= exists && !directory && type == PERCEIVED_TYPE_IMAGE;
        facts.media &= exists && !directory && (type == PERCEIVED_TYPE_AUDIO || type == PERCEIVED_TYPE_VIDEO);
        facts.castItems &= exists && !directory && (type == PERCEIVED_TYPE_IMAGE || type == PERCEIVED_TYPE_AUDIO || type == PERCEIVED_TYPE_VIDEO);
        bool application = extensionIs(extension,{L".exe",L".com",L".msi",L".bat",L".cmd"});
        if (applicationLink && fastProperties) {
            // Use only the selected shortcut's cached native properties. Never
            // resolve/bind/open its target, query target attributes or execute
            // it merely to choose an Application Tools context.
            PROPVARIANT target{},targetFlags{};
            const auto targetStatus = fastProperties->GetValue(PKEY_Link_TargetParsingPath,&target);
            const auto flagsStatus = fastProperties->GetValue(PKEY_Link_TargetSFGAOFlags,&targetFlags);
            if (SUCCEEDED(flagsStatus) && targetFlags.vt == VT_UI4 && !(targetFlags.ulVal & SFGAO_FOLDER)) {
                std::wstring targetExtension;
                if(SUCCEEDED(targetStatus) && target.vt == VT_LPWSTR && target.pwszVal && *target.pwszVal)
                    targetExtension=PathFindExtensionW(target.pwszVal);
                else residentShortcutExtension(path,attributes,targetExtension);
                application = extensionIs(targetExtension,{L".exe",L".com",L".msi",L".bat",L".cmd"});
            }
            PropVariantClear(&targetFlags); PropVariantClear(&target);
        }
        facts.applications &= exists && !directory && application;
        facts.discImages &= exists && !directory && extensionIs(extension, {L".iso", L".img", L".vhd", L".vhdx"});
        if (count == 1) facts.singlePath = std::move(path);
    }
    // Empty optical/removable drives can have no readable filesystem yet. The
    // native folder identity still names a real drive; media availability is
    // determined by its actual command state, not GetFileAttributes success.
    if (count == 1 && singleNativeFolder && facts.singlePath.size() == 3 &&
        ((facts.singlePath[0] >= L'A' && facts.singlePath[0] <= L'Z') ||
         (facts.singlePath[0] >= L'a' && facts.singlePath[0] <= L'z')) &&
        facts.singlePath[1] == L':' && facts.singlePath[2] == L'\\') {
        facts.driveType = GetDriveTypeW(facts.singlePath.c_str());
        facts.driveRoot = facts.driveType != DRIVE_UNKNOWN && facts.driveType != DRIVE_NO_ROOT_DIR;
    }
    ComPtr<IShellItem> recycle;
    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_RecycleBinFolder, KF_FLAG_DEFAULT, nullptr,
                                       IID_PPV_ARGS(&recycle)))) {
        int order = 1;
        facts.recycleBin = SUCCEEDED(folder->Compare(recycle.Get(), SICHINT_CANONICAL, &order)) && order == 0;
    }
    if (facts.recycleBin) {
        SHQUERYRBINFO info{sizeof(info)};
        facts.recycleStatus = SHQueryRecycleBinW(nullptr, &info);
        if (SUCCEEDED(facts.recycleStatus) && info.i64NumItems >= 0)
            facts.recycleItems = static_cast<ULONGLONG>(info.i64NumItems);
    }
    return S_OK;
}

HMENU findSubmenu(HMENU menu, UINT command, unsigned depth, unsigned& budget) {
    if (!menu || depth > maximumMenuDepth) return nullptr;
    const int count = GetMenuItemCount(menu);
    for (int index = 0; index < count && budget; ++index) {
        --budget;
        MENUITEMINFOW entry{sizeof(entry)};
        entry.fMask = MIIM_ID | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(index), TRUE, &entry)) continue;
        if (entry.wID == command && entry.hSubMenu) return entry.hSubMenu;
        if (const HMENU nested = findSubmenu(entry.hSubMenu, command, depth + 1, budget)) return nested;
    }
    return nullptr;
}
} // namespace

HRESULT namespaceSelectionKinds(IShellItemArray* selection,NamespaceSelectionKinds* result) {
    if (!result) return E_POINTER;
    NamespaceSelectionKinds kinds;
    if (!selection) { *result = kinds; return S_OK; }
    auto hr = selection->GetCount(&kinds.count);
    if (FAILED(hr)) return hr;
    if (!kinds.count) { *result = kinds; return S_OK; }
    ComPtr<IPropertyStore> properties;
    hr = selection->GetPropertyStore(GPS_FASTPROPERTIESONLY | GPS_BESTEFFORT,IID_PPV_ARGS(&properties));
    if (FAILED(hr)) return hr;
    if (!properties) return E_UNEXPECTED;
    PROPVARIANT value{};
    hr = properties->GetValue(PKEY_Kind,&value);
    if (SUCCEEDED(hr)) {
        if (value.vt == (VT_VECTOR | VT_LPWSTR)) {
            if (value.calpwstr.cElems && !value.calpwstr.pElems) hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            else for (ULONG index = 0; index < value.calpwstr.cElems; ++index) {
                const auto* kind = value.calpwstr.pElems[index];
                if (!kind) { hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA); break; }
                kinds.music |= equal(kind,L"music");
                kinds.video |= equal(kind,L"video");
            }
        } else if (value.vt != VT_EMPTY && value.vt != VT_NULL) hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    PropVariantClear(&value);
    if (SUCCEEDED(hr)) *result = kinds;
    return hr;
}

std::wstring_view namespaceActionCommand(NamespaceAction action) noexcept {
    const auto index = static_cast<size_t>(action);
    return index < actions.size() ? actions[index].command : std::wstring_view{};
}

std::wstring_view namespaceActionLabel(NamespaceAction action) noexcept {
    const auto index = static_cast<size_t>(action);
    return index < actions.size() ? actions[index].label : std::wstring_view{};
}

bool namespaceActionApplicable(NamespaceAction action,const NamespaceFacts& facts) noexcept {
    return static_cast<size_t>(action) < actions.size() && applicable(action,facts);
}

HRESULT namespaceCommandMetadata(std::wstring_view command, NamespaceCommandMetadata* result,
                                 IShellItemArray* selection,IUnknown* site) {
    if (!result) return E_POINTER;
    if (!validCommand(command)) return E_INVALIDARG;
    const std::wstring path = std::wstring(commandStorePath) + L"\\shell\\" + std::wstring(command);
    RegistryKey key;
    const LONG error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &key.value);
    if (error != ERROR_SUCCESS) return HRESULT_FROM_WIN32(error);
    NamespaceCommandMetadata metadata;
    metadata.command = command;
    std::wstring raw;
    HRESULT hr = readRegistryText(key.value, L"MUIVerb", raw);
    if (SUCCEEDED(hr)) metadata.label = loadIndirect(raw);
    else if (hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) return hr;
    if (metadata.label.empty() && SUCCEEDED(readRegistryText(key.value, nullptr, raw))) metadata.label = loadIndirect(raw);
    hr = readRegistryText(key.value, L"Description", raw);
    if (SUCCEEDED(hr)) metadata.description = loadIndirect(raw);
    else if (hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) return hr;
    hr = readRegistryText(key.value, L"Icon", metadata.icon);
    if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) return hr;
    // Registry icons can be missing or describe a legacy command. The installed
    // IExplorerCommand supplies the current native artwork and localized text.
    RegisteredProvider provider;
    if (SUCCEEDED(loadRegisteredProvider(command,site,false,provider))) {
        for (unsigned field = 0; field < 3; ++field) {
            PWSTR rawText = nullptr;
            hr = field == 0 ? provider.command->GetTitle(selection,&rawText)
                : field == 1 ? provider.command->GetIcon(selection,&rawText)
                             : provider.command->GetToolTip(selection,&rawText);
            OwnedText text(rawText);
            if (SUCCEEDED(hr) && text && *text) {
                auto& destination = field == 0 ? metadata.label : field == 1 ? metadata.icon : metadata.description;
                destination = text.get();
            }
        }
    }
    *result = std::move(metadata);
    return S_OK;
}

HRESULT namespaceCommandState(std::wstring_view command, IShellItemArray* selection,
                              IUnknown* site, NamespaceCommandState* result) {
    if (!result) return E_POINTER;
    if (!validCommand(command)) return E_INVALIDARG;
    try {
        return registeredCommandState(command,selection,site,FALSE,false,result);
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (...) { return E_FAIL; }
}

struct NamespaceCommandStateTask::Impl {
    struct Shared {
        std::atomic<bool> cancelled{false};
        std::mutex mutex;
        bool ready = false;
        HRESULT status = E_PENDING;
        NamespaceCommandState result;
        std::vector<NamespaceSelectionVerbState> verbResults;
        NamespaceCommandStateTimings timings;
    };
    DWORD thread = GetCurrentThreadId();
    bool independentVerbs = false;
    ComPtr<IGlobalInterfaceTable> git;
    DWORD selection = 0,site = 0;
    std::shared_ptr<Shared> shared = std::make_shared<Shared>();
    void revoke() noexcept {
        if (git) {
            if (selection) git->RevokeInterfaceFromGlobal(selection);
            if (site) git->RevokeInterfaceFromGlobal(site);
        }
        selection = site = 0;
    }
    ~Impl() { shared->cancelled.store(true); revoke(); }
};

namespace {
struct StatePhaseTimer {
    ULONGLONG* value;
    std::chrono::steady_clock::time_point started=std::chrono::steady_clock::now();
    ~StatePhaseTimer(){if(value)*value=static_cast<ULONGLONG>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-started).count());}
};
bool validAssociationVerb(std::wstring_view verb) noexcept {
    return !verb.empty() && verb.size()<=128 && verb.find_first_of(L"\\/\0",0,3)==std::wstring_view::npos;
}

HRESULT selectionIdentitySnapshot(IShellItemArray* original,DWORD count,
                                  const std::atomic<bool>* cancelled,IShellItemArray** result,NamespaceCommandStateTimings* timings=nullptr) {
    if(!result)return E_POINTER;*result=nullptr;
    if(!original||!count)return E_INVALIDARG;
    const auto stopped=[cancelled]{return cancelled&&cancelled->load();};
    if(stopped())return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    struct Medium {STGMEDIUM value{};~Medium(){if(value.tymed)ReleaseStgMedium(&value);}} medium;
    HRESULT hr=S_OK;
    {
        StatePhaseTimer phase{timings?&timings->dataObjectExportMicroseconds:nullptr};
        ComPtr<IDataObject> data;
        hr=original->BindToHandler(nullptr,BHID_DataObject,IID_PPV_ARGS(&data));
        if(FAILED(hr))return hr;if(!data)return E_UNEXPECTED;
        const UINT format=RegisterClipboardFormatW(CFSTR_SHELLIDLIST);
        if(!format)return HRESULT_FROM_WIN32(GetLastError());
        FORMATETC request{static_cast<CLIPFORMAT>(format),nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};
        hr=data->GetData(&request,&medium.value);
        if(FAILED(hr))return hr;
    }
    StatePhaseTimer construction{timings?&timings->identityConstructionMicroseconds:nullptr};
    if(medium.value.tymed!=TYMED_HGLOBAL||!medium.value.hGlobal)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    const SIZE_T bytes=GlobalSize(medium.value.hGlobal);
    if(bytes<sizeof(UINT)||bytes>std::numeric_limits<UINT>::max())return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    struct Lock {HGLOBAL memory;const BYTE* value;~Lock(){if(value)GlobalUnlock(memory);}} locked{
        medium.value.hGlobal,static_cast<const BYTE*>(GlobalLock(medium.value.hGlobal))};
    if(!locked.value)return HRESULT_FROM_WIN32(GetLastError());
    UINT nativeCount=0;std::memcpy(&nativeCount,locked.value,sizeof(nativeCount));
    const size_t header=sizeof(UINT)+(static_cast<size_t>(nativeCount)+1)*sizeof(UINT);
    if(nativeCount!=count||header>bytes)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    const auto idAt=[&](size_t index,PCUIDLIST_RELATIVE* item)->HRESULT {
        UINT offset=0;std::memcpy(&offset,locked.value+sizeof(UINT)+index*sizeof(UINT),sizeof(offset));
        if(offset<header||offset>bytes-sizeof(USHORT))return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        size_t cursor=offset;
        for(;;) {
            if(stopped())return HRESULT_FROM_WIN32(ERROR_CANCELLED);
            if(cursor>bytes-sizeof(USHORT))return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            USHORT length=0;std::memcpy(&length,locked.value+cursor,sizeof(length));
            if(!length){*item=reinterpret_cast<PCUIDLIST_RELATIVE>(locked.value+offset);return S_OK;}
            if(length<sizeof(USHORT)||length>bytes-cursor)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            cursor+=length;
        }
    };
    PCUIDLIST_RELATIVE parent=nullptr;hr=idAt(0,&parent);if(FAILED(hr))return hr;
    std::vector<OwnedPidl> owned;std::vector<PCIDLIST_ABSOLUTE> ids;
    owned.reserve(count);ids.reserve(count);
    for(DWORD index=0;index<count;++index) {
        PCUIDLIST_RELATIVE child=nullptr;hr=idAt(static_cast<size_t>(index)+1,&child);if(FAILED(hr))return hr;
        OwnedPidl absolute(ILCombine(parent,child));if(!absolute)return E_OUTOFMEMORY;
        ids.push_back(absolute.get());owned.push_back(std::move(absolute));
    }
    if(stopped())return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    DWORD retainedCount=0;hr=original->GetCount(&retainedCount);if(FAILED(hr))return hr;
    if(retainedCount!=count)return HRESULT_FROM_WIN32(ERROR_RETRY);
    // CFSTR_SHELLIDLIST contains every exact namespace identity, including
    // virtual items. No filesystem path, first-item association or item cap.
    return SHCreateShellItemArrayFromIDLists(count,ids.data(),result);
}

struct RegisteredSelectionMenu {
    RegistryKey key;
    OwnedPidl folderId;
    ComPtr<IShellFolder> parent;
    std::vector<OwnedPidl> absolute;
    std::vector<PCUITEMID_CHILD> children;
    HRESULT create(IShellItemArray* selection,const std::atomic<bool>* cancelled,IContextMenu** result) {
        if(!result)return E_POINTER;*result=nullptr;
        DWORD count=0;auto hr=selection->GetCount(&count);if(FAILED(hr))return hr;
        if(!count)return E_INVALIDARG;
        absolute.reserve(count);children.reserve(count);
        for(DWORD index=0;index<count;++index) {
            if(cancelled&&cancelled->load())return HRESULT_FROM_WIN32(ERROR_CANCELLED);
            ComPtr<IShellItem> item;hr=selection->GetItemAt(index,&item);if(FAILED(hr))return hr;
            PIDLIST_ABSOLUTE raw=nullptr;hr=SHGetIDListFromObject(item.Get(),&raw);
            OwnedPidl id(raw);if(FAILED(hr))return hr;
            if(!id||ILIsEmpty(id.get()))return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            OwnedPidl candidate(ILCloneFull(id.get()));if(!candidate)return E_OUTOFMEMORY;
            if(!ILRemoveLastID(candidate.get()))return E_UNEXPECTED;
            if(!index) {
                PCUITEMID_CHILD unused=nullptr;
                hr=SHBindToParent(id.get(),IID_PPV_ARGS(&parent),&unused);if(FAILED(hr))return hr;
                folderId=std::move(candidate);
            } else if(!ILIsEqual(folderId.get(),candidate.get()))return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            children.push_back(ILFindLastID(id.get()));absolute.push_back(std::move(id));
        }
        hr=HRESULT_FROM_WIN32(RegOpenKeyExW(HKEY_LOCAL_MACHINE,commandStorePath,0,KEY_READ,&key.value));
        if(FAILED(hr))return hr;
        DEFCONTEXTMENU definition{};definition.pidlFolder=folderId.get();definition.psf=parent.Get();
        definition.cidl=count;definition.apidl=children.data();definition.cKeys=1;definition.aKeys=&key.value;
        return SHCreateDefaultContextMenu(&definition,IID_PPV_ARGS(result));
    }
};

HRESULT readSelectionVerbState(IShellItemArray* selection,IUnknown* site,std::span<const std::wstring_view> verbs,
                               const std::atomic<bool>* cancelled,NamespaceCommandState* result,bool workerLocal=false,
                               std::wstring_view registeredCommand={},std::vector<NamespaceSelectionVerbState>* independent=nullptr,
                               NamespaceCommandStateTimings* timings=nullptr) {
    if(!result)return E_POINTER;
    if(!selection||verbs.empty()||verbs.size()>(independent?maximumMenuEntries:16))return E_INVALIDARG;
    for(const auto verb:verbs)if(!validAssociationVerb(verb))return E_INVALIDARG;
    const auto stopped=[cancelled]{return cancelled&&cancelled->load();};
    if(stopped())return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    DWORD count=0;HRESULT hr=selection->GetCount(&count);
    if(FAILED(hr))return hr;if(!count)return E_INVALIDARG;
    if(stopped())return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    bool identitySnapshot=false;
    ComPtr<IShellItemArray> local;
    if(workerLocal) {
        hr=selectionIdentitySnapshot(selection,count,cancelled,&local,timings);
        if(SUCCEEDED(hr)) {
            DWORD localCount=0;hr=local->GetCount(&localCount);if(FAILED(hr))return hr;
            if(localCount!=count)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            identitySnapshot=true;
        } else {
            if(hr!=E_NOTIMPL&&hr!=E_NOINTERFACE&&hr!=DV_E_FORMATETC&&hr!=HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED))return hr;
            // A custom namespace may expose its native menu without CIDA.
            // The original full array is already a correctly marshaled GIT
            // interface; use its documented binding, never a foreign pointer.
        }
    }
    RegisteredSelectionMenu registered;
    ComPtr<IContextMenu> context;
    {
        StatePhaseTimer phase{timings?&timings->contextBindMicroseconds:nullptr};
        hr=registeredCommand.empty()?(local?local.Get():selection)->BindToHandler(nullptr,BHID_SFUIObject,IID_PPV_ARGS(&context))
                                    :registered.create(local?local.Get():selection,cancelled,&context);
    }
    if(FAILED(hr))return hr;if(!context)return E_UNEXPECTED;
    ComPtr<IObjectWithSite> withSite;context.As(&withSite);
    if(stopped())return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    NativeContextMenu menu;
    // A worker uses no creator HWND: all native menu resources belong to its
    // STA, while the real view/site is supplied through a marshaled interface.
    {
        StatePhaseTimer phase{timings?&timings->menuQueryMicroseconds:nullptr};
        const UINT flags=CMF_ITEMMENU|CMF_EXTENDEDVERBS;
        hr=workerLocal?menu.createLeafState(context.Get(),site,flags):menu.create(nullptr,context.Get(),site,flags);
    }
    if(FAILED(hr))return hr;
    if(stopped())return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    std::vector<ContextMenuEntry> entries;
    {
        StatePhaseTimer phase{timings?&timings->menuEnumerationMicroseconds:nullptr};
        hr=menu.enumerate(entries,false); // No delayed submenu/MRU discovery.
    }
    if(FAILED(hr))return hr;
    if(stopped())return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    StatePhaseTimer reduction{timings?&timings->stateReductionMicroseconds:nullptr};
    const auto stateFor=[&](const NamespaceInvocationPlan& plan) {
        NamespaceCommandState state;
        state.contextMenu=true;state.selectionCount=count;
        state.identitySnapshot=identitySnapshot;
        state.siteAttached=site!=nullptr&&withSite!=nullptr;
        state.state=plan.enabled?ECS_ENABLED:ECS_DISABLED;
        if(plan.checked)state.state=static_cast<EXPCMDSTATE>(state.state|ECS_CHECKED|ECS_CHECKBOX);
        state.delegatedCommand=plan.canonicalVerb;
        return state;
    };
    if(independent) {
        std::vector<NamespaceSelectionVerbState> states;states.reserve(verbs.size());
        for(const auto verb:verbs) {
            if(stopped())return HRESULT_FROM_WIN32(ERROR_CANCELLED);
            NamespaceSelectionVerbState entry;entry.verb=verb;
            NamespaceInvocationPlan plan;
            entry.status=planVerb(entries,verb,NamespaceInvocationRoute::SelectionMenu,plan);
            if(entry.status==HRESULT_FROM_WIN32(ERROR_NOT_FOUND)||(SUCCEEDED(entry.status)&&plan.submenu))
                entry.status=HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            if(SUCCEEDED(entry.status))entry.native=stateFor(plan);
            states.push_back(std::move(entry));
        }
        DWORD retainedCount=0;hr=selection->GetCount(&retainedCount);if(FAILED(hr))return hr;
        if(retainedCount!=count)return HRESULT_FROM_WIN32(ERROR_RETRY);
        if(stopped())return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        menu.reset();*independent=std::move(states);return S_OK;
    }
    NamespaceInvocationPlan plan;
    bool found=false;
    for(const auto verb:verbs) {
        NamespaceInvocationPlan candidate;
        hr=planVerb(entries,verb,NamespaceInvocationRoute::SelectionMenu,candidate);
        if(hr==HRESULT_FROM_WIN32(ERROR_NOT_FOUND))continue;
        if(FAILED(hr))return hr;
        if(candidate.submenu)continue;
        if(!found||candidate.enabled){plan=std::move(candidate);found=true;}
        if(plan.enabled)break;
    }
    if(!found)return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    DWORD retainedCount=0;hr=selection->GetCount(&retainedCount);if(FAILED(hr))return hr;
    if(retainedCount!=count)return HRESULT_FROM_WIN32(ERROR_RETRY);
    auto state=stateFor(plan);
    menu.reset();
    *result=std::move(state);return S_OK;
}
} // namespace

NamespaceCommandStateTask::NamespaceCommandStateTask(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
NamespaceCommandStateTask::~NamespaceCommandStateTask() = default;

HRESULT NamespaceCommandStateTask::start(std::wstring_view command,IShellItemArray* selection,IUnknown* site,
                                         bool background,std::unique_ptr<NamespaceCommandStateTask>* result) {
    return startImpl(command,selection,site,background,{},result);
}

HRESULT NamespaceCommandStateTask::startSelectionVerb(std::wstring_view verb,IShellItemArray* selection,IUnknown* site,
                                                      std::unique_ptr<NamespaceCommandStateTask>* result) {
    const std::array<std::wstring_view,1> verbs{verb};
    return startSelectionVerbs(verbs,selection,site,result);
}

HRESULT NamespaceCommandStateTask::startSelectionVerbs(std::span<const std::wstring_view> verbs,IShellItemArray* selection,IUnknown* site,
                                                       std::unique_ptr<NamespaceCommandStateTask>* result) {
    if(!result)return E_POINTER;
    if(!selection||verbs.empty()||verbs.size()>16)return E_INVALIDARG;
    try {
        std::vector<std::wstring> names;names.reserve(verbs.size());
        for(const auto verb:verbs) {
            if(!validAssociationVerb(verb))return E_INVALIDARG;
            for(const auto& previous:names)if(equal(previous,verb))return E_INVALIDARG;
            names.emplace_back(verb);
        }
        return startImpl({},selection,site,false,std::move(names),result);
    } catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
      catch(...){return E_FAIL;}
}

HRESULT NamespaceCommandStateTask::startSelectionVerbBatch(std::span<const std::wstring_view> verbs,IShellItemArray* selection,IUnknown* site,
                                                           std::unique_ptr<NamespaceCommandStateTask>* result) {
    if(!result)return E_POINTER;
    if(!selection||verbs.empty()||verbs.size()>maximumMenuEntries)return E_INVALIDARG;
    try {
        std::vector<std::wstring> names;names.reserve(verbs.size());
        for(const auto verb:verbs) {
            if(!validAssociationVerb(verb))return E_INVALIDARG;
            for(const auto& previous:names)if(equal(previous,verb))return E_INVALIDARG;
            names.emplace_back(verb);
        }
        return startImpl({},selection,site,false,std::move(names),result,true);
    }catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
     catch(...){return E_FAIL;}
}

HRESULT NamespaceCommandStateTask::startRegisteredMenu(std::wstring_view command,IShellItemArray* selection,IUnknown* site,
                                                       std::unique_ptr<NamespaceCommandStateTask>* result) {
    if(!result)return E_POINTER;
    if(!selection||!validCommand(command))return E_INVALIDARG;
    try{return startImpl(command,selection,site,false,{std::wstring(command)},result);}
    catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
    catch(...){return E_FAIL;}
}

HRESULT NamespaceCommandStateTask::startImpl(std::wstring_view command,IShellItemArray* selection,IUnknown* site,
                                            bool background,std::vector<std::wstring> selectionVerbs,
                                            std::unique_ptr<NamespaceCommandStateTask>* result,bool independentVerbs) {
    if (!result) return E_POINTER;
    const bool selectionVerb=!selectionVerbs.empty();
    if(selectionVerb?!selection:!validCommand(command))return E_INVALIDARG;
    APTTYPE apartment{};
    APTTYPEQUALIFIER qualifier{};
    HRESULT hr = CoGetApartmentType(&apartment,&qualifier);
    if (FAILED(hr)) return hr;
    if (apartment != APTTYPE_STA && apartment != APTTYPE_MAINSTA) return RPC_E_WRONG_THREAD;
    // Cancellation cannot interrupt a native QueryContextMenu already in
    // flight. Retain one default-selection-menu work slot until that worker
    // releases native references, uninitializes COM and finishes its lease.
    // Registered-only menus and registered state providers remain separate.
    static std::atomic<unsigned> defaultMenuWorkers{0};
    const bool defaultMenu=selectionVerb&&command.empty();
    if(defaultMenu) {
        unsigned expected=0;
        if(!defaultMenuWorkers.compare_exchange_strong(expected,1))return HRESULT_FROM_WIN32(ERROR_BUSY);
    }
    struct DefaultMenuReservation {
        std::atomic<unsigned>* value;
        bool transferred=false;
        ~DefaultMenuReservation(){if(value&&!transferred)--*value;}
    } defaultReservation{defaultMenu?&defaultMenuWorkers:nullptr};
    static std::atomic<unsigned> workers{0};
    unsigned count = workers.load();
    do { if (count >= 16) return HRESULT_FROM_WIN32(ERROR_BUSY); }
    while (!workers.compare_exchange_weak(count,count+1));
    struct WorkerReservation {
        std::atomic<unsigned>* value;
        bool transferred = false;
        ~WorkerReservation() { if (!transferred) --*value; }
    } reservation{&workers};
    try {
        std::unique_ptr<StaWorkerLease> lease;
        hr=StaWorkerLease::prepare(&lease);
        if(FAILED(hr))return hr;
        auto impl = std::make_unique<Impl>();
        impl->independentVerbs=independentVerbs;
        hr = CoCreateInstance(CLSID_StdGlobalInterfaceTable,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&impl->git));
        if (SUCCEEDED(hr) && selection) hr = impl->git->RegisterInterfaceInGlobal(selection,__uuidof(IShellItemArray),&impl->selection);
        if (SUCCEEDED(hr) && site) hr = impl->git->RegisterInterfaceInGlobal(site,IID_IUnknown,&impl->site);
        if (FAILED(hr)) return hr;
        const auto shared = impl->shared;
        const DWORD selectionCookie = impl->selection,siteCookie = impl->site;
        auto task = std::unique_ptr<NamespaceCommandStateTask>(new NamespaceCommandStateTask(std::move(impl)));
        std::thread([shared,name=std::wstring(command),verbs=std::move(selectionVerbs),selectionCookie,siteCookie,background,selectionVerb,independentVerbs,
                     lease=std::move(lease),workerCount=&workers,defaultMenuCount=defaultReservation.value] {
            struct WorkerRelease {
                std::atomic<unsigned>* value;
                void finish() noexcept{if(value){--*value;value=nullptr;}}
                ~WorkerRelease(){finish();}
            } release{workerCount},defaultRelease{defaultMenuCount};
            NamespaceCommandStateTimings timings;
            const auto started=std::chrono::steady_clock::now();
            HRESULT final = lease->attach();
            if (SUCCEEDED(final)) final = selectionVerb?OleInitialize(nullptr):CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
            const bool initialized = SUCCEEDED(final);
            NamespaceCommandState state;
            std::vector<NamespaceSelectionVerbState> states;
            try {
                if (initialized && !shared->cancelled.load()) {
                    // Obtain only this apartment's correctly marshaled proxies.
                    ComPtr<IGlobalInterfaceTable> git;
                    ComPtr<IShellItemArray> items;
                    ComPtr<IUnknown> view;
                    final = CoCreateInstance(CLSID_StdGlobalInterfaceTable,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&git));
                    if (SUCCEEDED(final) && selectionCookie) final = git->GetInterfaceFromGlobal(selectionCookie,IID_PPV_ARGS(&items));
                    if (SUCCEEDED(final) && siteCookie) final = git->GetInterfaceFromGlobal(siteCookie,IID_PPV_ARGS(&view));
                    if (SUCCEEDED(final) && !shared->cancelled.load()) {
                        if(selectionVerb) {
                            std::vector<std::wstring_view> names;names.reserve(verbs.size());
                            for(const auto& verb:verbs)names.emplace_back(verb);
                            final=readSelectionVerbState(items.Get(),view.Get(),names,&shared->cancelled,&state,true,name,independentVerbs?&states:nullptr,&timings);
                        } else final=registeredCommandState(name,items.Get(),view.Get(),TRUE,background,&state);
                    }
                }
            } catch (const std::bad_alloc&) { final = E_OUTOFMEMORY; }
              catch (...) { final = E_FAIL; }
            // All provider/proxy/GIT references above are gone before teardown.
            if (initialized) {if(selectionVerb)OleUninitialize();else CoUninitialize();}
            const auto released=lease->finish();
            if(SUCCEEDED(final)&&FAILED(released))final=released;
            if (shared->cancelled.load()) final = HRESULT_FROM_WIN32(ERROR_CANCELLED);
            defaultRelease.finish();
            timings.workerMicroseconds=static_cast<ULONGLONG>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-started).count());
            std::lock_guard lock(shared->mutex);
            shared->status = final;
            shared->result = state;
            shared->verbResults=std::move(states);
            shared->timings=timings;
            shared->ready = true;
        }).detach();
        reservation.transferred = true;
        defaultReservation.transferred=true;
        *result = std::move(task);
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (...) { return E_FAIL; }
}

HRESULT NamespaceCommandStateTask::poll(NamespaceCommandState* result) {
    if (!result) return E_POINTER;
    if (impl_->thread != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
    if(impl_->independentVerbs)return E_INVALIDARG;
    if (impl_->shared->cancelled.load()) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    std::lock_guard lock(impl_->shared->mutex);
    if (!impl_->shared->ready) return E_PENDING;
    if (FAILED(impl_->shared->status)) return impl_->shared->status;
    *result = impl_->shared->result;
    return impl_->shared->status;
}

HRESULT NamespaceCommandStateTask::pollSelectionVerbBatch(std::vector<NamespaceSelectionVerbState>* result) {
    if(!result)return E_POINTER;
    if(impl_->thread!=GetCurrentThreadId())return RPC_E_WRONG_THREAD;
    if(!impl_->independentVerbs)return E_INVALIDARG;
    if(impl_->shared->cancelled.load())return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    std::lock_guard lock(impl_->shared->mutex);
    if(!impl_->shared->ready)return E_PENDING;
    if(FAILED(impl_->shared->status))return impl_->shared->status;
    try{auto states=impl_->shared->verbResults;*result=std::move(states);return impl_->shared->status;}
    catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
    catch(...){return E_FAIL;}
}

HRESULT NamespaceCommandStateTask::pollTimings(NamespaceCommandStateTimings* result) {
    if(!result)return E_POINTER;
    if(impl_->thread!=GetCurrentThreadId())return RPC_E_WRONG_THREAD;
    if(impl_->shared->cancelled.load())return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    std::lock_guard lock(impl_->shared->mutex);
    if(!impl_->shared->ready)return E_PENDING;
    *result=impl_->shared->timings;return S_OK;
}

void NamespaceCommandStateTask::cancel() noexcept {
    impl_->shared->cancelled.store(true);
    impl_->revoke();
}

bool NamespaceCommandStateTask::completed() const {
    if(impl_->thread!=GetCurrentThreadId())return false;
    std::lock_guard lock(impl_->shared->mutex);return impl_->shared->ready;
}

struct NativeNamespaceCommandChildren::Impl {
    HWND owner = nullptr;
    DWORD thread = GetCurrentThreadId();
    bool background = false;
    ComPtr<IShellItemArray> selection;
    ComPtr<IUnknown> site;
    RegisteredProvider provider;
    struct Child {
        ComPtr<IExplorerCommand> command;
        ComPtr<IObjectWithSite> withSite;
        std::vector<std::unique_ptr<Child>> children;
        bool attached = false;
        ~Child() { children.clear(); if (attached && withSite) withSite->SetSite(nullptr); }
    };
    std::vector<std::unique_ptr<Child>> children;
    std::vector<NamespaceSubcommandMetadata> metadata;

    HRESULT capture(IExplorerCommand* command,unsigned depth,unsigned& budget,
                    std::vector<std::unique_ptr<Child>>& commands,
                    std::vector<NamespaceSubcommandMetadata>& presentation) {
        if (depth > 8) return HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES);
        ComPtr<IEnumExplorerCommand> enumerator;
        HRESULT hr = command->EnumSubCommands(&enumerator);
        if (hr == E_NOTIMPL || hr == E_NOINTERFACE || hr == S_FALSE) return S_FALSE;
        if (FAILED(hr)) return hr;
        if (!enumerator) return S_FALSE;
        for (;;) {
            auto child = std::make_unique<Child>();
            ULONG fetched = 0;
            hr = enumerator->Next(1,&child->command,&fetched);
            if (hr == S_FALSE && !fetched) break;
            if (FAILED(hr)) return hr;
            if (fetched != 1 || !child->command) return E_UNEXPECTED;
            if (!budget--) return HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES);
            hr = child->command.As(&child->withSite);
            if (FAILED(hr) && hr != E_NOINTERFACE) return hr;
            if (site && child->withSite) {
                child->attached = true;
                hr = child->withSite->SetSite(site.Get());
                if (FAILED(hr)) return hr;
            }
            NamespaceSubcommandMetadata entry;
            child->command->GetCanonicalName(&entry.canonicalName);
            hr = child->command->GetFlags(&entry.flags);
            if (FAILED(hr)) return hr;
            IShellItemArray* items = background ? nullptr : selection.Get();
            for (unsigned field=0;field<3;++field) {
                PWSTR raw = nullptr;
                hr = field == 0 ? child->command->GetTitle(items,&raw)
                    : field == 1 ? child->command->GetIcon(items,&raw) : child->command->GetToolTip(items,&raw);
                OwnedText text(raw);
                if (field == 0 && FAILED(hr) && !(entry.flags & ECF_ISSEPARATOR)) return hr;
                if (SUCCEEDED(hr) && text) {
                    auto& destination = field == 0 ? entry.label : field == 1 ? entry.icon : entry.description;
                    destination = text.get();
                }
            }
            entry.stateStatus = child->command->GetState(items,FALSE,&entry.state);
            if ((entry.flags & ECF_HASSUBCOMMANDS) && SUCCEEDED(entry.stateStatus) &&
                !(entry.state & (ECS_DISABLED | ECS_HIDDEN))) {
                hr = capture(child->command.Get(),depth+1,budget,child->children,entry.children);
                if (FAILED(hr)) return hr;
            }
            presentation.push_back(std::move(entry));
            commands.push_back(std::move(child));
        }
        return S_OK;
    }
};

NativeNamespaceCommandChildren::NativeNamespaceCommandChildren(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
NativeNamespaceCommandChildren::~NativeNamespaceCommandChildren() = default;
const std::vector<NamespaceSubcommandMetadata>& NativeNamespaceCommandChildren::entries() const noexcept { return impl_->metadata; }

HRESULT NativeNamespaceCommandChildren::invoke(size_t index,bool headless) {
    return invokePath(std::span<const size_t>(&index,1),headless);
}

HRESULT NativeNamespaceCommandChildren::invokePath(std::span<const size_t> path,bool headless) {
    if (headless || !impl_->owner || !IsWindow(impl_->owner) || !IsWindowVisible(impl_->owner)) return E_ACCESSDENIED;
    if (impl_->thread != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
    DWORD process = 0;
    if (GetWindowThreadProcessId(impl_->owner,&process) != impl_->thread || process != GetCurrentProcessId()) return E_ACCESSDENIED;
    if (path.empty() || path.size() > 9) return E_INVALIDARG;
    EXPCMDSTATE parentState = ECS_DISABLED;
    auto hr = impl_->provider.command->GetState(impl_->background ? nullptr : impl_->selection.Get(),FALSE,&parentState);
    if (FAILED(hr)) return hr;
    if (parentState & (ECS_DISABLED | ECS_HIDDEN)) return HRESULT_FROM_WIN32(ERROR_ACCESS_DISABLED_BY_POLICY);
    const auto* children = &impl_->children;
    for (size_t depth=0;depth<path.size();++depth) {
        if (path[depth] >= children->size()) return E_INVALIDARG;
        auto& child = *(*children)[path[depth]];
        EXPCMDFLAGS flags{};
        hr = child.command->GetFlags(&flags);
        if (FAILED(hr)) return hr;
        if (flags & ECF_ISSEPARATOR) return E_INVALIDARG;
        EXPCMDSTATE state = ECS_DISABLED;
        hr = child.command->GetState(impl_->background ? nullptr : impl_->selection.Get(),FALSE,&state);
        if (FAILED(hr)) return hr;
        if (state & (ECS_DISABLED | ECS_HIDDEN)) return HRESULT_FROM_WIN32(ERROR_ACCESS_DISABLED_BY_POLICY);
        if (depth+1 == path.size()) {
            if (flags & ECF_HASSUBCOMMANDS) return E_INVALIDARG;
            return child.command->Invoke(impl_->background ? nullptr : impl_->selection.Get(),nullptr);
        }
        if (!(flags & ECF_HASSUBCOMMANDS)) return E_INVALIDARG;
        children = &child.children;
    }
    return E_UNEXPECTED;
}

HRESULT namespaceCommandChildren(std::wstring_view command, IShellItemArray* selection,
                                 IUnknown* site,
                                 std::vector<NamespaceSubcommandMetadata>* result) {
    if (!result) return E_POINTER;
    if (!validCommand(command)) return E_INVALIDARG;
    RegisteredProvider provider;
    HRESULT hr = loadRegisteredProvider(command,site,false,provider);
    if (FAILED(hr)) return hr;
    // Library providers initialize their current-library context in GetState;
    // EnumSubCommands requires that context on this same provider instance.
    if (command.starts_with(L"Windows.Library")) {
        EXPCMDSTATE state = ECS_DISABLED;
        hr = provider.command->GetState(selection,FALSE,&state);
        if (FAILED(hr)) return hr;
        if (state & (ECS_DISABLED | ECS_HIDDEN)) return HRESULT_FROM_WIN32(ERROR_ACCESS_DISABLED_BY_POLICY);
    }
    unsigned budget = 256;
    std::vector<NamespaceSubcommandMetadata> metadata;
    hr = commandChildren(provider.command.Get(), selection, 0, budget, metadata);
    if (FAILED(hr)) return hr;
    *result = std::move(metadata);
    return hr;
}

HRESULT planNamespaceAction(NamespaceAction action, const NamespaceFacts& facts,
                            const std::vector<ContextMenuEntry>& selectionEntries,
                            const std::vector<ContextMenuEntry>& commandStoreEntries,
                            NamespaceInvocationPlan* result) {
    if (!result) return E_POINTER;
    if (static_cast<size_t>(action) >= actions.size()) return E_INVALIDARG;
    if (!applicable(action, facts)) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    NamespaceInvocationPlan plan;
    plan.action = action;
    plan.target = facts.singlePath;
    if (action == NamespaceAction::RestoreAll || action == NamespaceAction::EmptyRecycleBin) {
        if (FAILED(facts.recycleStatus)) return facts.recycleStatus;
        plan.route = action == NamespaceAction::RestoreAll ? NamespaceInvocationRoute::RestoreAllItems :
                                                            NamespaceInvocationRoute::EmptyRecycleBin;
        plan.scope = NamespaceMenuScope::Background;
        plan.canonicalVerb = namespaceActionCommand(action);
        plan.enabled = facts.recycleItems > 0;
        plan.status = plan.enabled ? S_OK : HRESULT_FROM_WIN32(ERROR_NO_MORE_ITEMS);
        plan.nativeConfirmation = true;
        *result = std::move(plan);
        return S_OK;
    }
    for (const auto verb : nativeVerbs(action)) {
        const HRESULT hr = planVerb(selectionEntries, verb, NamespaceInvocationRoute::SelectionMenu, plan);
        if (SUCCEEDED(hr)) {
            plan.nativeConfirmation = action == NamespaceAction::FormatDrive || action == NamespaceAction::RestoreSelected;
            *result = std::move(plan);
            return S_OK;
        }
        if (hr != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) return hr;
    }
    const auto command = namespaceActionCommand(action);
    if (command.empty()) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    HRESULT hr = planVerb(commandStoreEntries, command, NamespaceInvocationRoute::CommandStoreMenu, plan);
    if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) && action == NamespaceAction::IncludeInLibrary)
        hr = planVerb(commandStoreEntries, L"Windows.LibraryIncludeInLibrary", NamespaceInvocationRoute::CommandStoreMenu, plan);
    if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) && action == NamespaceAction::AlwaysAvailableOffline &&
        facts.detailedTargetsKnown && facts.offlineActive) {
        plan.route = NamespaceInvocationRoute::OfflineFilesPin;
        plan.canonicalVerb = facts.offlinePinnedForUser ? L"IOfflineFilesCache.Unpin" : L"IOfflineFilesCache.Pin";
        plan.enabled = true;
        plan.checked = facts.offlinePinnedForUser;
        plan.status = S_OK;
        *result = std::move(plan);
        return S_OK;
    }
    if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) && action == NamespaceAction::Email &&
        facts.detailedTargetsKnown && facts.physicalFiles && facts.mailRecipientAvailable) {
        plan.route = NamespaceInvocationRoute::SendToMailRecipient;
        plan.canonicalVerb = L"SendTo.MailRecipient";
        plan.target = facts.mailRecipientPath;
        plan.enabled = true;
        plan.status = S_OK;
        *result = std::move(plan);
        return S_OK;
    }
    if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) && action == NamespaceAction::CastToDevice && facts.castHandlerAvailable) {
        if (!facts.castCommandId || facts.castCommandId > 0x7fff) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        plan.route = NamespaceInvocationRoute::RegisteredHandlerMenu;
        plan.canonicalVerb = L"ContextMenuHandlers.PlayTo";
        plan.commandId = facts.castCommandId;
        plan.submenu = facts.castHandlerSubmenu;
        plan.enabled = facts.castHandlerEnabled;
        plan.status = plan.enabled ? S_OK : E_ACCESSDENIED;
        *result = std::move(plan);
        return S_OK;
    }
    if (FAILED(hr)) return hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) ? HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) : hr;
    plan.nativeConfirmation = action == NamespaceAction::FormatDrive || action == NamespaceAction::RestoreSelected;
    *result = std::move(plan);
    return S_OK;
}

struct NativeNamespaceActions::Impl {
    HWND owner = nullptr;
    DWORD thread = 0;
    NamespaceTarget target;
    ComPtr<IShellItemArray> targets;
    NamespaceFacts facts;
    NativeContextMenu selection;
    NativeContextMenu commands;
    NativeContextMenu backgroundCommands;
    std::vector<ContextMenuEntry> selectionEntries;
    std::vector<ContextMenuEntry> commandsEntries;
    std::vector<ContextMenuEntry> backgroundEntries;
    RegistryKey commandStore;
    bool selectionLoaded = false;
    bool commandsLoaded = false;
    bool backgroundLoaded = false;
    HRESULT selectionStatus = E_PENDING;
    HRESULT commandsStatus = E_PENDING;
    HRESULT backgroundStatus = E_PENDING;
    NativeContextMenu* active = nullptr;
    unsigned long long menuGeneration = 1;
    std::vector<std::shared_ptr<OfflineCompletion>> offlineOperations;
    ComPtr<IShellItem> mailRecipient;
    ComPtr<IDropTarget> mailDropTarget;
    bool mailLoaded = false;
    HRESULT mailStatus = E_PENDING;
    ComPtr<IShellItem> zipRecipient;
    ComPtr<IDropTarget> zipDropTarget;
    bool zipLoaded = false;
    HRESULT zipStatus = E_PENDING;
    ComPtr<IShellItem> faxRecipient;
    ComPtr<IDropTarget> faxDropTarget;
    bool faxLoaded = false;
    HRESULT faxStatus = E_PENDING;
    RegistryKey extractKey;
    NativeContextMenu extractMenu;
    bool extractLoaded = false;
    HRESULT extractStatus = E_PENDING;
    UINT extractCommand = 0;
    bool extractEnabled = false;
    RegistryKey castKey;
    NativeContextMenu castMenu;
    bool castLoaded = false;
    HRESULT castStatus = E_PENDING;

    ~Impl() { for (auto& operation : offlineOperations) operation->cancelled.store(true); }

    HRESULT onThread() const noexcept {
        if (!thread || !target.folder || !targets) return E_UNEXPECTED;
        return thread == GetCurrentThreadId() ? S_OK : RPC_E_WRONG_THREAD;
    }

    HRESULT visibleInteraction(bool headless) const noexcept {
        if (headless || !owner || !IsWindow(owner) || !IsWindowVisible(owner)) return E_ACCESSDENIED;
        if (active) return HRESULT_FROM_WIN32(ERROR_BUSY);
        return onThread();
    }

    void clearMenus() noexcept {
        ++menuGeneration;
        selection.reset();
        commands.reset();
        backgroundCommands.reset();
        selectionEntries.clear();
        commandsEntries.clear();
        backgroundEntries.clear();
        commandStore.reset();
        castMenu.reset();
        castKey.reset();
        castLoaded = false;
        castStatus = E_PENDING;
        facts.castHandlerAvailable = facts.castHandlerEnabled = facts.castHandlerSubmenu = false;
        facts.castCommandId = 0;
        selectionLoaded = commandsLoaded = backgroundLoaded = false;
        selectionStatus = commandsStatus = backgroundStatus = E_PENDING;
        mailRecipient.Reset();
        mailDropTarget.Reset();
        mailLoaded = false;
        mailStatus = E_PENDING;
        zipRecipient.Reset();
        zipDropTarget.Reset();
        zipLoaded = false;
        zipStatus = E_PENDING;
        faxRecipient.Reset();
        faxDropTarget.Reset();
        faxLoaded = false;
        faxStatus = E_PENDING;
        extractMenu.reset();
        extractKey.reset();
        extractLoaded = false;
        extractStatus = E_PENDING;
        extractCommand = 0;
        extractEnabled = false;
        facts.mailRecipientAvailable = false;
        facts.mailRecipientPath.clear();
    }

    HRESULT loadMailRecipient() {
        if (mailLoaded) return mailStatus;
        mailLoaded = true;
        ComPtr<IShellItem> sendTo;
        HRESULT hr = SHGetKnownFolderItem(FOLDERID_SendTo, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&sendTo));
        if (FAILED(hr)) { mailStatus = hr; return hr; }
        ComPtr<IEnumShellItems> enumerator;
        hr = sendTo->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&enumerator));
        if (FAILED(hr)) { mailStatus = hr; return hr; }
        unsigned budget = 4096;
        for (;;) {
            ComPtr<IShellItem> candidate;
            ULONG fetched = 0;
            hr = enumerator->Next(1, &candidate, &fetched);
            if (hr == S_FALSE && !fetched) break;
            if (FAILED(hr)) { mailStatus = hr; return hr; }
            if (fetched != 1 || !candidate) { mailStatus = E_UNEXPECTED; return mailStatus; }
            if (!budget--) { mailStatus = HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES); return mailStatus; }
            std::wstring path;
            if (FAILED(itemPath(candidate.Get(), path))) continue;
            // MAPIMail is the registered native SendTo file type. Its display
            // name is localized and its filename is customizable; use neither.
            if (!equal(PathFindExtensionW(path.c_str()), L".MAPIMail")) continue;
            if (mailRecipient) { mailRecipient.Reset(); mailStatus = E_UNEXPECTED; return mailStatus; }
            mailRecipient = candidate;
            facts.mailRecipientPath = std::move(path);
        }
        if (!mailRecipient) { mailStatus = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED); return mailStatus; }
        hr = mailRecipient->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&mailDropTarget));
        facts.mailRecipientAvailable = SUCCEEDED(hr) && mailDropTarget != nullptr;
        if (SUCCEEDED(hr) && !mailDropTarget) hr = E_UNEXPECTED;
        mailStatus = hr;
        return hr;
    }

    HRESULT loadZipRecipient() {
        if (zipLoaded) return zipStatus;
        zipLoaded = true;
        ComPtr<IShellItem> sendTo;
        HRESULT hr = SHGetKnownFolderItem(FOLDERID_SendTo,KF_FLAG_DEFAULT,nullptr,IID_PPV_ARGS(&sendTo));
        ComPtr<IEnumShellItems> enumerator;
        if (SUCCEEDED(hr)) hr = sendTo->BindToHandler(nullptr,BHID_EnumItems,IID_PPV_ARGS(&enumerator));
        unsigned budget = 4096;
        while (SUCCEEDED(hr)) {
            ComPtr<IShellItem> candidate;
            ULONG fetched = 0;
            hr = enumerator->Next(1,&candidate,&fetched);
            if (hr == S_FALSE && !fetched) { hr = S_OK; break; }
            if (FAILED(hr)) break;
            if (fetched != 1 || !candidate) { hr = E_UNEXPECTED; break; }
            if (!budget--) { hr = HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES); break; }
            std::wstring path;
            if (FAILED(itemPath(candidate.Get(),path)) || !equal(PathFindExtensionW(path.c_str()),L".ZFSendToTarget")) continue;
            if (zipRecipient) { hr = E_UNEXPECTED; break; }
            zipRecipient = candidate;
        }
        if (SUCCEEDED(hr) && !zipRecipient) hr = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        if (SUCCEEDED(hr)) hr = zipRecipient->BindToHandler(nullptr,BHID_SFUIObject,IID_PPV_ARGS(&zipDropTarget));
        if (SUCCEEDED(hr) && !zipDropTarget) hr = E_UNEXPECTED;
        if (FAILED(hr)) { zipRecipient.Reset(); zipDropTarget.Reset(); }
        zipStatus = hr;
        return hr;
    }

    HRESULT sendZip(POINT point) {
        HRESULT hr = loadZipRecipient();
        if (FAILED(hr)) return hr;
        ComPtr<IDataObject> data;
        hr = targets->BindToHandler(nullptr,BHID_DataObject,IID_PPV_ARGS(&data));
        if (FAILED(hr)) return hr;
        ComPtr<IObjectWithSite> withSite;
        zipDropTarget.As(&withSite);
        if (target.site && withSite) {
            hr = withSite->SetSite(target.site.Get());
            if (FAILED(hr)) return hr;
        }
        struct DetachSite {
            ComPtr<IObjectWithSite> value;
            bool attached;
            ~DetachSite() { if (attached && value) value->SetSite(nullptr); }
        } detach{withSite,target.site != nullptr && withSite != nullptr};
        DWORD effect = DROPEFFECT_COPY;
        const POINTL location{point.x,point.y};
        hr = zipDropTarget->DragEnter(data.Get(),MK_LBUTTON | MK_CONTROL,location,&effect);
        if (FAILED(hr)) { zipDropTarget->DragLeave(); return hr; }
        if (!(effect & DROPEFFECT_COPY)) { zipDropTarget->DragLeave(); return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED); }
        effect = DROPEFFECT_COPY;
        return zipDropTarget->Drop(data.Get(),MK_CONTROL,location,&effect);
    }

    HRESULT loadFaxRecipient() {
        if (faxLoaded) return faxStatus;
        faxLoaded = true;
        RegistryKey key;
        const std::wstring commandPath = std::wstring(commandStorePath) + L"\\shell\\Windows.fax";
        HRESULT hr = HRESULT_FROM_WIN32(RegOpenKeyExW(HKEY_LOCAL_MACHINE,commandPath.c_str(),0,KEY_READ,&key.value));
        std::wstring filename;
        if (SUCCEEDED(hr)) hr = readRegistryText(key.value,L"SendToVerb",filename);
        if (SUCCEEDED(hr) && (filename.empty() || filename.find_first_of(L"\\/:") != std::wstring::npos)) hr = E_INVALIDARG;
        ComPtr<IShellItem> sendTo;
        if (SUCCEEDED(hr)) hr = SHGetKnownFolderItem(FOLDERID_SendTo,KF_FLAG_DEFAULT,nullptr,IID_PPV_ARGS(&sendTo));
        if (SUCCEEDED(hr)) hr = SHCreateItemFromRelativeName(sendTo.Get(),filename.c_str(),nullptr,IID_PPV_ARGS(&faxRecipient));
        ComPtr<IShellItem> destination;
        if (SUCCEEDED(hr)) hr = faxRecipient->BindToHandler(nullptr,BHID_LinkTargetItem,IID_PPV_ARGS(&destination));
        std::wstring path;
        if (SUCCEEDED(hr)) hr = itemPath(destination.Get(),path);
        if (SUCCEEDED(hr) && GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) hr = HRESULT_FROM_WIN32(GetLastError());
        if (SUCCEEDED(hr)) hr = faxRecipient->BindToHandler(nullptr,BHID_SFUIObject,IID_PPV_ARGS(&faxDropTarget));
        if (SUCCEEDED(hr) && !faxDropTarget) hr = E_UNEXPECTED;
        if (FAILED(hr)) { faxRecipient.Reset(); faxDropTarget.Reset(); }
        faxStatus = hr;
        return hr;
    }

    HRESULT sendFax(POINT point) {
        HRESULT hr = loadFaxRecipient();
        if (FAILED(hr)) return hr;
        ComPtr<IDataObject> data;
        hr = targets->BindToHandler(nullptr,BHID_DataObject,IID_PPV_ARGS(&data));
        if (FAILED(hr)) return hr;
        DWORD effect = DROPEFFECT_COPY;
        const POINTL location{point.x,point.y};
        hr = faxDropTarget->DragEnter(data.Get(),MK_LBUTTON | MK_CONTROL,location,&effect);
        if (FAILED(hr)) { faxDropTarget->DragLeave(); return hr; }
        if (!(effect & DROPEFFECT_COPY)) { faxDropTarget->DragLeave(); return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED); }
        effect = DROPEFFECT_COPY;
        return faxDropTarget->Drop(data.Get(),MK_CONTROL,location,&effect);
    }

    HRESULT loadExtractHandler() {
        if (extractLoaded) return extractStatus;
        extractLoaded = true;
        // Read the installed compressed-folder registration before activating
        // this isolated provider. Its native canonical verb is queried at run
        // time; no translated label or composed-menu ordinal is assumed.
        constexpr wchar_t registeredClass[] = L"{b8cdcb65-b1bf-4b42-9428-1dfdb7ee92af}";
        RegistryKey registered;
        const std::wstring keyPath = std::wstring(L"CompressedFolder\\shellex\\ContextMenuHandlers\\") + registeredClass;
        HRESULT hr = HRESULT_FROM_WIN32(RegOpenKeyExW(HKEY_CLASSES_ROOT,keyPath.c_str(),0,KEY_READ,&registered.value));
        CLSID clsid{};
        if (SUCCEEDED(hr)) hr = CLSIDFromString(registeredClass,&clsid);
        ComPtr<IContextMenu> context;
        if (SUCCEEDED(hr)) hr = CoCreateInstance(clsid,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&context));
        ComPtr<IShellExtInit> initialize;
        if (SUCCEEDED(hr)) hr = context.As(&initialize);
        ComPtr<IDataObject> data;
        if (SUCCEEDED(hr)) hr = targets->BindToHandler(nullptr,BHID_DataObject,IID_PPV_ARGS(&data));
        if (SUCCEEDED(hr)) hr = HRESULT_FROM_WIN32(RegOpenKeyExW(HKEY_CLASSES_ROOT,L"CompressedFolder",0,KEY_READ,&extractKey.value));
        if (SUCCEEDED(hr)) hr = initialize->Initialize(nullptr,data.Get(),extractKey.value);
        if (SUCCEEDED(hr)) hr = extractMenu.create(owner,context.Get(),target.site.Get(),CMF_NORMAL);
        std::vector<ContextMenuEntry> entries;
        if (SUCCEEDED(hr)) hr = extractMenu.enumerate(entries);
        if (SUCCEEDED(hr)) {
            NamespaceInvocationPlan plan;
            hr = planVerb(entries,L"extract",NamespaceInvocationRoute::RegisteredHandlerMenu,plan);
            if (SUCCEEDED(hr)) { extractCommand = plan.commandId; extractEnabled = plan.enabled; }
        }
        extractStatus = hr;
        return hr;
    }

    HRESULT sendMail(POINT point) {
        HRESULT hr = loadMailRecipient();
        if (FAILED(hr)) return hr;
        ComPtr<IDataObject> data;
        hr = targets->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data));
        if (FAILED(hr)) return hr;
        ComPtr<IObjectWithSite> withSite;
        mailDropTarget.As(&withSite);
        if (target.site && withSite) {
            hr = withSite->SetSite(target.site.Get());
            if (FAILED(hr)) return hr;
        }
        struct DetachSite {
            ComPtr<IObjectWithSite> value;
            bool attached;
            ~DetachSite() { if (attached && value) value->SetSite(nullptr); }
        } detach{withSite, target.site != nullptr && withSite != nullptr};
        DWORD effect = DROPEFFECT_COPY;
        const POINTL location{point.x,point.y};
        hr = mailDropTarget->DragEnter(data.Get(), MK_LBUTTON | MK_CONTROL, location, &effect);
        if (FAILED(hr)) { mailDropTarget->DragLeave(); return hr; }
        if (!(effect & DROPEFFECT_COPY)) { mailDropTarget->DragLeave(); return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED); }
        effect = DROPEFFECT_COPY;
        return mailDropTarget->Drop(data.Get(), MK_CONTROL, location, &effect);
    }

    HRESULT loadCastHandler() {
        if (castLoaded) return castStatus;
        castLoaded = true;
        if (!facts.detailedTargetsKnown) {
            castStatus = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            return castStatus;
        }
        const wchar_t* association = facts.images ? L"SystemFileAssociations\\image" :
                                     facts.media ? L"SystemFileAssociations\\audio" : L"SystemFileAssociations\\video";
        const std::wstring path = std::wstring(association) + L"\\shellex\\ContextMenuHandlers\\PlayTo";
        RegistryKey handlerKey;
        LONG error = RegOpenKeyExW(HKEY_CLASSES_ROOT, path.c_str(), 0, KEY_READ, &handlerKey.value);
        if (error != ERROR_SUCCESS) { castStatus = HRESULT_FROM_WIN32(error); return castStatus; }
        std::wstring handler;
        HRESULT hr = readRegistryText(handlerKey.value, nullptr, handler);
        if (FAILED(hr)) { castStatus = hr; return hr; }
        if (!guidText(handler)) { castStatus = HRESULT_FROM_WIN32(ERROR_INVALID_DATA); return castStatus; }
        CLSID clsid{};
        hr = CLSIDFromString(handler.c_str(), &clsid);
        ComPtr<IContextMenu> context;
        if (SUCCEEDED(hr)) hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&context));
        ComPtr<IShellExtInit> initialize;
        if (SUCCEEDED(hr)) hr = context.As(&initialize);
        ComPtr<IDataObject> data;
        if (SUCCEEDED(hr)) hr = targets->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data));
        if (SUCCEEDED(hr)) {
            error = RegOpenKeyExW(HKEY_CLASSES_ROOT, association, 0, KEY_READ, &castKey.value);
            if (error != ERROR_SUCCESS) hr = HRESULT_FROM_WIN32(error);
        }
        if (SUCCEEDED(hr)) hr = initialize->Initialize(nullptr, data.Get(), castKey.value);
        if (SUCCEEDED(hr)) hr = castMenu.create(owner, context.Get(), target.site.Get(), CMF_NORMAL);
        std::vector<ContextMenuEntry> entries;
        if (SUCCEEDED(hr)) hr = castMenu.enumerate(entries);
        const ContextMenuEntry* cast = nullptr;
        if (SUCCEEDED(hr)) {
            // This is the isolated, registered PlayTo handler's own menu. Its
            // single command/cascade has no canonical verb on Windows 10. No
            // translated text or composed-menu command offsets are inferred.
            for (const auto& candidate : entries) {
                if (candidate.separator()) continue;
                if (cast) { hr = E_UNEXPECTED; break; }
                cast = &candidate;
            }
            if (!cast || !cast->id || cast->id > 0x7fff) hr = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        }
        if (SUCCEEDED(hr)) {
            facts.castHandlerAvailable = true;
            facts.castCommandId = cast->id;
            facts.castHandlerSubmenu = cast->submenu;
            unsigned budget = maximumMenuEntries;
            facts.castHandlerEnabled = cast->enabled() && (!cast->submenu || enabledLeaf(cast->children,0,budget));
        }
        castStatus = hr;
        return hr;
    }

    void describeOffline() {
        if (!facts.uncPaths || !facts.filesystem) return;
        BOOL componentActive = FALSE, enabled = FALSE;
        facts.offlineStatus = offlineStatus(componentActive, enabled);
        if (FAILED(facts.offlineStatus) || !componentActive || !enabled) return;
        ComPtr<IOfflineFilesCache> cache;
        facts.offlineStatus = CoCreateInstance(CLSID_OfflineFilesCache, nullptr, CLSCTX_INPROC_SERVER,
                                                IID_PPV_ARGS(&cache));
        if (FAILED(facts.offlineStatus)) return;
        facts.offlineActive = true;
        facts.offlinePinnedForUser = true;
        DWORD count = 0;
        if (FAILED(targets->GetCount(&count))) { facts.offlinePinnedForUser = false; return; }
        for (DWORD index = 0; index < count; ++index) {
            ComPtr<IShellItem> shellItem;
            std::wstring path;
            ComPtr<IOfflineFilesItem> item;
            ComPtr<IOfflineFilesPinInfo> pin;
            BOOL pinned = FALSE, inherited = FALSE;
            if (FAILED(targets->GetItemAt(index, &shellItem)) || FAILED(itemPath(shellItem.Get(), path)) ||
                FAILED(cache->FindItem(path.c_str(), 0, &item)) || FAILED(item.As(&pin)) ||
                FAILED(pin->IsPinnedForUser(&pinned, &inherited)) || !pinned) {
                facts.offlinePinnedForUser = false;
                break;
            }
        }
    }

    HRESULT pinOffline() {
        if (target.completionMessage < WM_APP || target.completionMessage > 0xbfff)
            return E_INVALIDARG;
        ComPtr<IOfflineFilesCache> cache;
        HRESULT hr = CoCreateInstance(CLSID_OfflineFilesCache, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&cache));
        if (FAILED(hr)) return hr;
        DWORD count = 0;
        hr = targets->GetCount(&count);
        if (FAILED(hr)) return hr;
        std::vector<std::wstring> paths;
        paths.reserve(count);
        for (DWORD index = 0; index < count; ++index) {
            ComPtr<IShellItem> item;
            hr = targets->GetItemAt(index, &item);
            if (FAILED(hr)) return hr;
            std::wstring path;
            hr = itemPath(item.Get(), path);
            if (FAILED(hr)) return hr;
            if (path.size() < 5 || path[0] != L'\\' || path[1] != L'\\' || path[2] == L'?' || path[2] == L'.')
                return HRESULT_FROM_WIN32(ERROR_INVALID_NAME);
            paths.push_back(std::move(path));
        }
        std::vector<LPCWSTR> names;
        names.reserve(paths.size());
        for (const auto& path : paths) names.push_back(path.c_str());
        offlineOperations.erase(std::remove_if(offlineOperations.begin(), offlineOperations.end(),
            [](const auto& operation) { return operation->finished.load(); }), offlineOperations.end());
        if (offlineOperations.size() >= 16) return HRESULT_FROM_WIN32(ERROR_BUSY);
        auto completion = std::make_shared<OfflineCompletion>();
        completion->owner = owner;
        completion->message = target.completionMessage;
        const auto progress = Microsoft::WRL::Make<OfflineProgress>(completion);
        if (!progress) return E_OUTOFMEMORY;
        const DWORD flags = OFFLINEFILES_PIN_CONTROL_FLAG_FORUSER | OFFLINEFILES_PIN_CONTROL_FLAG_INTERACTIVE |
                            (facts.offlinePinnedForUser ? 0 : OFFLINEFILES_PIN_CONTROL_FLAG_FILL);
        // Native service owns its asynchronous work; the callback retains first
        // per-item failure even if End reports only successful scheduling.
        if (facts.offlinePinnedForUser)
            hr = cache->Unpin(owner, names.data(), count, TRUE, TRUE, flags, progress.Get());
        else
            hr = cache->Pin(owner, names.data(), count, TRUE, TRUE, flags, progress.Get());
        if (SUCCEEDED(hr)) offlineOperations.push_back(std::move(completion));
        else completion->cancelled.store(true);
        return hr;
    }

    HRESULT loadSelection() {
        HRESULT hr = onThread();
        if (FAILED(hr)) return hr;
        if (selectionLoaded) return selectionStatus;
        selectionLoaded = true;
        hr = selection.createSelection(owner, targets.Get(), target.site.Get(), CMF_EXTENDEDVERBS);
        if (SUCCEEDED(hr)) hr = selection.enumerate(selectionEntries);
        selectionStatus = hr;
        return hr;
    }

    HRESULT loadCommands(NamespaceMenuScope scope) {
        HRESULT hr = onThread();
        if (FAILED(hr)) return hr;
        bool& loaded = scope == NamespaceMenuScope::Selection ? commandsLoaded : backgroundLoaded;
        HRESULT& status = scope == NamespaceMenuScope::Selection ? commandsStatus : backgroundStatus;
        if (loaded) return status;
        loaded = true;
        try {
        if (!commandStore.value) {
            const LONG error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, commandStorePath, 0, KEY_READ, &commandStore.value);
            if (error != ERROR_SUCCESS) { status = HRESULT_FROM_WIN32(error); return status; }
        }
        ComPtr<IShellFolder> parent;
        std::vector<OwnedPidl> absolute;
        std::vector<PCUITEMID_CHILD> children;
        OwnedPidl folderId;
        if (scope == NamespaceMenuScope::Background) {
            hr = target.folder->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(&parent));
            PIDLIST_ABSOLUTE raw = nullptr;
            if (SUCCEEDED(hr)) hr = SHGetIDListFromObject(target.folder.Get(), &raw);
            folderId.reset(raw);
        } else {
            DWORD count = 0;
            hr = targets->GetCount(&count);
            if (SUCCEEDED(hr) && !count) hr = E_INVALIDARG;
            if (SUCCEEDED(hr)) { absolute.reserve(count); children.reserve(count); }
            for (DWORD index = 0; SUCCEEDED(hr) && index < count; ++index) {
                ComPtr<IShellItem> item;
                hr = targets->GetItemAt(index, &item);
                PIDLIST_ABSOLUTE raw = nullptr;
                if (SUCCEEDED(hr)) hr = SHGetIDListFromObject(item.Get(), &raw);
                OwnedPidl owned(raw);
                if (FAILED(hr)) break;
                if (!raw || ILIsEmpty(raw)) { hr = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED); break; }
                OwnedPidl candidateParent(ILCloneFull(raw));
                if (!candidateParent) { hr = E_OUTOFMEMORY; break; }
                if (!ILRemoveLastID(candidateParent.get())) { hr = E_UNEXPECTED; break; }
                if (!index) {
                    PCUITEMID_CHILD unused = nullptr;
                    hr = SHBindToParent(raw, IID_PPV_ARGS(&parent), &unused);
                    folderId = std::move(candidateParent);
                } else if (!ILIsEqual(folderId.get(), candidateParent.get())) {
                    hr = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
                    break;
                }
                children.push_back(ILFindLastID(raw));
                absolute.push_back(std::move(owned));
            }
        }
        if (FAILED(hr)) { status = hr; return status; }
        DEFCONTEXTMENU definition{};
        definition.hwnd = owner;
        definition.pidlFolder = folderId.get();
        definition.psf = parent.Get();
        definition.cidl = static_cast<UINT>(children.size());
        definition.apidl = children.empty() ? nullptr : children.data();
        // Exactly one read-only key; DEFCONTEXTMENU's documented maximum is 16.
        definition.cKeys = 1;
        definition.aKeys = &commandStore.value;
        ComPtr<IContextMenu> context;
        hr = SHCreateDefaultContextMenu(&definition, IID_PPV_ARGS(&context));
        NativeContextMenu& menu = scope == NamespaceMenuScope::Selection ? commands : backgroundCommands;
        auto& entries = scope == NamespaceMenuScope::Selection ? commandsEntries : backgroundEntries;
        if (SUCCEEDED(hr)) hr = menu.create(owner, context.Get(), target.site.Get(), CMF_EXTENDEDVERBS);
        if (SUCCEEDED(hr)) hr = menu.enumerate(entries);
        status = hr;
        return hr;
        } catch (const std::bad_alloc&) { status=E_OUTOFMEMORY;return status; }
          catch (...) { status=E_FAIL;return status; }
    }

    HRESULT invokePlan(const NamespaceInvocationPlan& plan, POINT point) {
        NativeContextMenu& menu = plan.route == NamespaceInvocationRoute::RegisteredHandlerMenu ? castMenu :
            plan.route == NamespaceInvocationRoute::SelectionMenu ? selection :
            plan.scope == NamespaceMenuScope::Background ? backgroundCommands : commands;
        if (!plan.submenu) return menu.invoke(plan.commandId, point);
        unsigned budget = maximumMenuEntries;
        const HMENU popup = findSubmenu(menu.menu(), plan.commandId, 0, budget);
        if (!popup) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        if (!point.x && !point.y) {
            RECT rect{};
            if (!GetWindowRect(owner, &rect)) return HRESULT_FROM_WIN32(GetLastError());
            point = {rect.left + 24, rect.top + 100};
        }
        struct ActiveMenu {
            Impl& owner;
            explicit ActiveMenu(Impl& value, NativeContextMenu& native) : owner(value) { owner.active = &native; }
            ~ActiveMenu() { owner.active = nullptr; }
        } tracking(*this, menu);
        const UINT id = TrackPopupMenuEx(popup, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                         point.x, point.y, owner, nullptr);
        if (!id) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        return menu.invoke(id, point);
    }

    HRESULT restoreAll(POINT point) try {
        // Prefer the installed Windows RestoreAll command, which supplies its
        // own all-item confirmation and provider-specific restore behavior.
        if (SUCCEEDED(loadCommands(NamespaceMenuScope::Background))) {
            NamespaceInvocationPlan native;
            const HRESULT planned = planVerb(backgroundEntries, L"Windows.RecycleBin.RestoreAll",
                                               NamespaceInvocationRoute::CommandStoreMenu, native);
            if (SUCCEEDED(planned) && native.enabled) {
                native.scope = NamespaceMenuScope::Background;
                return invokePlan(native, point);
            }
        }
        // Public fallback still asks the user before applying the native
        // selection restore verb; it never moves $R files by guessed paths.
        const int answer = MessageBoxW(owner, L"Are you sure you want to restore all items from the Recycle Bin?",
                                        L"Restore all items", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        if (answer != IDYES) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        ComPtr<IEnumShellItems> enumerator;
        HRESULT hr = target.folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&enumerator));
        if (FAILED(hr)) return hr;
        std::vector<OwnedPidl> owned;
        for (;;) {
            ComPtr<IShellItem> item;
            ULONG fetched = 0;
            hr = enumerator->Next(1, &item, &fetched);
            if (hr == S_FALSE && !fetched) break;
            if (FAILED(hr)) return hr;
            if (!fetched || !item) return E_UNEXPECTED;
            if (owned.size() == static_cast<size_t>(std::numeric_limits<UINT>::max()))
                return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
            PIDLIST_ABSOLUTE raw = nullptr;
            hr = SHGetIDListFromObject(item.Get(), &raw);
            OwnedPidl pidl(raw);
            if (FAILED(hr)) return hr;
            if (!raw) return E_UNEXPECTED;
            owned.push_back(std::move(pidl));
        }
        if (owned.empty()) return HRESULT_FROM_WIN32(ERROR_NO_MORE_ITEMS);
        std::vector<PCIDLIST_ABSOLUTE> ids;
        ids.reserve(owned.size());
        for (const auto& item : owned) ids.push_back(item.get());
        ComPtr<IShellItemArray> all;
        hr = SHCreateShellItemArrayFromIDLists(static_cast<UINT>(ids.size()), ids.data(), &all);
        if (FAILED(hr)) return hr;
        NativeContextMenu menu;
        hr = menu.createSelection(owner, all.Get(), target.site.Get());
        if (FAILED(hr)) return hr;
        std::vector<ContextMenuEntry> entries;
        hr = menu.enumerate(entries);
        if (FAILED(hr)) return hr;
        NamespaceInvocationPlan plan;
        for (const auto verb : {std::wstring_view(L"undelete"), std::wstring_view(L"restore")}) {
            hr = planVerb(entries, verb, NamespaceInvocationRoute::SelectionMenu, plan);
            if (SUCCEEDED(hr)) return plan.enabled ? menu.invoke(plan.commandId, point) : plan.status;
            if (hr != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) return hr;
        }
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (...) { return E_FAIL; }
};

NativeNamespaceActions::NativeNamespaceActions() : impl_(std::make_unique<Impl>()) {}
NativeNamespaceActions::~NativeNamespaceActions() = default;

void NativeNamespaceActions::reset(bool cancelPending) noexcept {
    if (cancelPending) {
        for (auto& operation : impl_->offlineOperations) operation->cancelled.store(true);
        impl_->offlineOperations.clear();
    }
    if (impl_->active) return;
    ++impl_->menuGeneration;
    impl_->active = nullptr;
    impl_->selection.reset();
    impl_->commands.reset();
    impl_->backgroundCommands.reset();
    impl_->castMenu.reset();
    impl_->castKey.reset();
    impl_->commandStore.reset();
    impl_->selectionEntries.clear();
    impl_->commandsEntries.clear();
    impl_->backgroundEntries.clear();
    impl_->target = {};
    impl_->targets.Reset();
    impl_->facts = {};
    impl_->owner = nullptr;
    impl_->thread = 0;
    impl_->selectionLoaded = impl_->commandsLoaded = impl_->backgroundLoaded = false;
    impl_->selectionStatus = impl_->commandsStatus = impl_->backgroundStatus = E_PENDING;
    impl_->mailRecipient.Reset();
    impl_->mailDropTarget.Reset();
    impl_->mailLoaded = false;
    impl_->mailStatus = E_PENDING;
    impl_->zipRecipient.Reset();
    impl_->zipDropTarget.Reset();
    impl_->zipLoaded = false;
    impl_->zipStatus = E_PENDING;
    impl_->castLoaded = false;
    impl_->castStatus = E_PENDING;
}

HRESULT NativeNamespaceActions::initialize(HWND owner, const NamespaceTarget& target) {
    if (impl_->active) return HRESULT_FROM_WIN32(ERROR_BUSY);
    const NamespaceTarget retained = target;
    reset();
    if (!retained.folder) return E_INVALIDARG;
    APTTYPE apartment{};
    APTTYPEQUALIFIER qualifier{};
    HRESULT hr = CoGetApartmentType(&apartment, &qualifier);
    if (FAILED(hr)) return hr;
    if (apartment != APTTYPE_STA && apartment != APTTYPE_MAINSTA) return RPC_E_WRONG_THREAD;
    ComPtr<IShellItemArray> targets = retained.selection;
    DWORD count = 0;
    if (targets) hr = targets->GetCount(&count);
    if (FAILED(hr)) return hr;
    if (!count) {
        targets.Reset();
        hr = SHCreateShellItemArrayFromShellItem(retained.folder.Get(), IID_PPV_ARGS(&targets));
        if (FAILED(hr)) return hr;
    }
    NamespaceFacts facts;
    hr = describeTargets(retained.folder.Get(), targets.Get(), count, facts);
    if (FAILED(hr)) return hr;
    impl_->owner = owner;
    impl_->thread = GetCurrentThreadId();
    impl_->target = retained;
    impl_->targets = targets;
    impl_->facts = std::move(facts);
    impl_->describeOffline();
    return S_OK;
}

HRESULT NativeNamespaceActions::refresh() {
    HRESULT hr = impl_->onThread();
    if (FAILED(hr)) return hr;
    if (impl_->active) return HRESULT_FROM_WIN32(ERROR_BUSY);
    NamespaceFacts facts;
    hr = describeTargets(impl_->target.folder.Get(), impl_->targets.Get(), impl_->facts.selectionCount, facts);
    if (FAILED(hr)) return hr;
    impl_->clearMenus();
    impl_->facts = std::move(facts);
    impl_->describeOffline();
    return S_OK;
}

const NamespaceFacts& NativeNamespaceActions::facts() const noexcept { return impl_->facts; }

HRESULT NativeNamespaceActions::planInvocation(NamespaceAction action, NamespaceInvocationPlan* result) {
    if (!result) return E_POINTER;
    HRESULT hr = impl_->onThread();
    if (FAILED(hr)) return hr;
    if (static_cast<size_t>(action) >= actions.size()) return E_INVALIDARG;
    if (!applicable(action, impl_->facts)) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    if (action == NamespaceAction::RestoreAll || action == NamespaceAction::EmptyRecycleBin)
        return planNamespaceAction(action, impl_->facts, {}, {}, result);
    const HRESULT selectionStatus = impl_->loadSelection();
    hr = planNamespaceAction(action, impl_->facts, impl_->selectionEntries, {}, result);
    if (SUCCEEDED(hr) || hr != HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)) return hr;
    const HRESULT commandsStatus = impl_->loadCommands(NamespaceMenuScope::Selection);
    hr = planNamespaceAction(action, impl_->facts, impl_->selectionEntries, impl_->commandsEntries, result);
    if (hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) && action == NamespaceAction::Email) {
        const HRESULT recipientStatus = impl_->loadMailRecipient();
        if (SUCCEEDED(recipientStatus)) hr = planNamespaceAction(action, impl_->facts, impl_->selectionEntries, impl_->commandsEntries, result);
    }
    if (hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) && action == NamespaceAction::CastToDevice) {
        const HRESULT handlerStatus = impl_->loadCastHandler();
        if (SUCCEEDED(handlerStatus)) hr = planNamespaceAction(action, impl_->facts, impl_->selectionEntries, impl_->commandsEntries, result);
    }
    if (FAILED(hr) && FAILED(selectionStatus) && FAILED(commandsStatus)) return commandsStatus;
    return hr;
}

std::vector<NamespaceInvocationPlan> NativeNamespaceActions::capabilities() {
    std::vector<NamespaceInvocationPlan> result;
    result.reserve(actions.size());
    for (size_t index = 0; index < actions.size(); ++index) {
        NamespaceInvocationPlan plan;
        plan.action = static_cast<NamespaceAction>(index);
        const HRESULT hr = planInvocation(plan.action, &plan);
        if (FAILED(hr)) plan.status = hr;
        result.push_back(std::move(plan));
    }
    return result;
}

HRESULT NativeNamespaceActions::invoke(NamespaceAction action, bool headless, POINT point) {
    HRESULT hr = impl_->visibleInteraction(headless);
    if (FAILED(hr)) return hr;
    hr = refresh();
    if (FAILED(hr)) return hr;
    NamespaceInvocationPlan plan;
    hr = planInvocation(action, &plan);
    if (hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) && action == NamespaceAction::Fax &&
        impl_->facts.detailedTargetsKnown && namespaceActionApplicable(action,impl_->facts)) return impl_->sendFax(point);
    if (FAILED(hr)) return hr;
    if (!plan.enabled) return plan.status;
    if (plan.route == NamespaceInvocationRoute::RestoreAllItems) return impl_->restoreAll(point);
    if (plan.route == NamespaceInvocationRoute::EmptyRecycleBin)
        return SHEmptyRecycleBinW(impl_->owner, nullptr, 0);
    if (plan.route == NamespaceInvocationRoute::OfflineFilesPin) return impl_->pinOffline();
    if (plan.route == NamespaceInvocationRoute::SendToMailRecipient) return impl_->sendMail(point);
    return impl_->invokePlan(plan, point);
}

HRESULT NativeNamespaceActions::planCommandStore(std::wstring_view command,
                                                NamespaceInvocationPlan* result,
                                                NamespaceMenuScope scope) {
    if (!result) return E_POINTER;
    if (!validCommand(command)) return E_INVALIDARG;
    const HRESULT hr = impl_->loadCommands(scope);
    if (FAILED(hr)) return hr;
    NamespaceInvocationPlan plan;
    plan.scope = scope;
    plan.target = impl_->facts.singlePath;
    const HRESULT found = planVerb(scope == NamespaceMenuScope::Selection ? impl_->commandsEntries : impl_->backgroundEntries,
                                   command, NamespaceInvocationRoute::CommandStoreMenu, plan);
    if (FAILED(found)) return found;
    *result = std::move(plan);
    return S_OK;
}

HRESULT NativeNamespaceActions::commandStoreEntries(std::vector<ContextMenuEntry>& result,
                                                   NamespaceMenuScope scope) {
    const HRESULT hr = impl_->loadCommands(scope);
    if (FAILED(hr)) return hr;
    result = scope == NamespaceMenuScope::Selection ? impl_->commandsEntries : impl_->backgroundEntries;
    return S_OK;
}

HRESULT NativeNamespaceActions::commandMetadata(std::wstring_view command,
                                               NamespaceCommandMetadata* result,
                                               NamespaceMenuScope scope) {
    if (!result) return E_POINTER;
    HRESULT hr = impl_->onThread();
    if (FAILED(hr)) return hr;
    NamespaceCommandMetadata metadata;
    hr = namespaceCommandMetadata(command, &metadata,
        scope == NamespaceMenuScope::Background ? nullptr : impl_->targets.Get(),impl_->target.site.Get());
    if (FAILED(hr)) return hr;
    if (metadata.label.empty()) {
        hr = impl_->loadCommands(scope);
        if (FAILED(hr)) return hr;
        unsigned budget = maximumMenuEntries;
        MenuMatch match;
        findVerb(scope == NamespaceMenuScope::Selection ? impl_->commandsEntries : impl_->backgroundEntries,
                  command, true, 0, budget, match);
        if (match.item && !match.ambiguous) metadata.label = match.item->label;
    }
    *result = std::move(metadata);
    return S_OK;
}

HRESULT NativeNamespaceActions::invokeCommandStore(std::wstring_view command, bool headless, POINT point,
                                                  NamespaceMenuScope scope,std::wstring_view requiredAssociationVerb) {
    HRESULT hr = impl_->visibleInteraction(headless);
    if (FAILED(hr)) return hr;
    hr = refresh();
    if (FAILED(hr)) return hr;
    if (!requiredAssociationVerb.empty()) {
        NamespaceCommandState state;
        hr = queryStaticVerbState(requiredAssociationVerb,&state);
        if(hr==E_PENDING) {
            // Explicit visible interaction may build a fresh full native menu;
            // capability callbacks instead complete this work on the STA task.
            try {
                const std::array<std::wstring_view,1> verbs{requiredAssociationVerb};
                const bool ribbonOnly=equal(command,L"Windows.removeproperties")&&equal(requiredAssociationVerb,L"removeproperties");
                const std::array<std::wstring_view,1> registered{command};
                hr=readSelectionVerbState(impl_->targets.Get(),impl_->target.site.Get(),ribbonOnly?std::span(registered):std::span(verbs),
                    nullptr,&state,false,ribbonOnly?command:std::wstring_view{});
            }
            catch(const std::bad_alloc&) { hr=E_OUTOFMEMORY; }
            catch(...) { hr=E_FAIL; }
        }
        if (FAILED(hr)) return hr;
        if (!state.enabled()) return HRESULT_FROM_WIN32(ERROR_ACCESS_DISABLED_BY_POLICY);
    }
    NamespaceInvocationPlan plan;
    hr = planCommandStore(command, &plan, scope);
    if (FAILED(hr)) return hr;
    return plan.enabled ? impl_->invokePlan(plan, point) : plan.status;
}

HRESULT NativeNamespaceActions::invokeViewSelection(std::wstring_view command,IShellView* exactView,bool headless) {
    struct SelectionCommand { std::wstring_view name; const wchar_t* canonical; };
    static constexpr std::array selectionCommands{
        SelectionCommand{L"Windows.selectall",L"{B33BF5AF-76D5-4D10-93E7-D8E22E93798F}"},
        SelectionCommand{L"Windows.selectnone",L"{A3E5349F-8943-4CEC-BF26-03096D7B2244}"},
        SelectionCommand{L"Windows.invertselection",L"{DCE2BBAD-735B-4343-BFDB-A31D594737F6}"}};
    const SelectionCommand* allowed = nullptr;
    for (const auto& candidate : selectionCommands) if (equal(command,candidate.name)) { allowed = &candidate; break; }
    if (!allowed) return E_INVALIDARG;
    if (!exactView) return E_POINTER;
    if (!impl_->owner || !IsWindow(impl_->owner)) return E_ACCESSDENIED;
    HRESULT hr = impl_->onThread();
    if (FAILED(hr)) return hr;
    if (impl_->active) return HRESULT_FROM_WIN32(ERROR_BUSY);
    if (headless) {
        const auto desktop = PrivateDesktop::current();
        if (!desktop || !desktop->ready()) return E_ACCESSDENIED;
        hr = desktop->verifyIsolation();
        if (FAILED(hr)) return hr;
    } else if (!IsWindowVisible(impl_->owner)) return E_ACCESSDENIED;
    DWORD process = 0;
    const DWORD ownerThread = GetWindowThreadProcessId(impl_->owner,&process);
    if (process != GetCurrentProcessId() || ownerThread != GetCurrentThreadId()) return E_ACCESSDENIED;
    ComPtr<IShellView> siteView;
    if (!impl_->target.site) return E_ACCESSDENIED;
    hr = impl_->target.site.As(&siteView);
    if (FAILED(hr)) return hr;
    ComPtr<IUnknown> siteIdentity,requestedIdentity;
    hr = siteView.As(&siteIdentity);
    if (SUCCEEDED(hr)) hr = exactView->QueryInterface(IID_PPV_ARGS(&requestedIdentity));
    if (FAILED(hr)) return hr;
    if (siteIdentity.Get() != requestedIdentity.Get()) return E_ACCESSDENIED;
    HWND window = nullptr;
    hr = exactView->GetWindow(&window);
    if (FAILED(hr)) return hr;
    if (!window || !IsWindow(window) || !IsChild(impl_->owner,window)) return E_ACCESSDENIED;
    const DWORD viewThread = GetWindowThreadProcessId(window,&process);
    if (process != GetCurrentProcessId() || viewThread != ownerThread) return E_ACCESSDENIED;
    try {
        RegistryKey key;
        const auto path = std::wstring(commandStorePath) + L"\\shell\\" + std::wstring(allowed->name);
        hr = HRESULT_FROM_WIN32(RegOpenKeyExW(HKEY_LOCAL_MACHINE,path.c_str(),0,KEY_READ,&key.value));
        if (FAILED(hr)) return hr;
        std::wstring canonical;
        hr = readRegistryText(key.value,L"CanonicalName",canonical);
        if (FAILED(hr)) return hr;
        GUID actual{},expected{};
        hr = CLSIDFromString(canonical.c_str(),&actual);
        if (SUCCEEDED(hr)) hr = CLSIDFromString(allowed->canonical,&expected);
        if (FAILED(hr)) return hr;
        if (!IsEqualGUID(actual,expected)) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        NamespaceCommandState state;
        hr = queryCommandState(allowed->name,&state,NamespaceMenuScope::Background);
        if (FAILED(hr)) return hr; // Includes E_PENDING; never invoke from a guess.
        if (!state.enabled()) return HRESULT_FROM_WIN32(ERROR_ACCESS_DISABLED_BY_POLICY);
        // Selection changed since an earlier popup may have been cached. Build
        // and retain the native menu for this exact current view interaction.
        impl_->clearMenus();
        NamespaceInvocationPlan plan;
        hr = planCommandStore(allowed->name,&plan,NamespaceMenuScope::Background);
        if (FAILED(hr)) return hr;
        if (!plan.enabled || plan.submenu || plan.route != NamespaceInvocationRoute::CommandStoreMenu ||
            !equal(plan.canonicalVerb,allowed->name)) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        if (headless) {
            hr = PrivateDesktop::current()->verifyIsolation();
            if (FAILED(hr)) return hr;
        }
        return impl_->backgroundCommands.invoke(plan.commandId);
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (...) { return E_FAIL; }
}

HRESULT NativeNamespaceActions::queryCommandState(std::wstring_view command, NamespaceCommandState* result,
                                                  NamespaceMenuScope scope) {
    if (!result) return E_POINTER;
    if (!validCommand(command)) return E_INVALIDARG;
    const HRESULT hr = impl_->onThread();
    if (FAILED(hr)) return hr;
    if (scope == NamespaceMenuScope::Background) {
        try {
            ComPtr<IShellItemArray> folder;
            const HRESULT created = SHCreateShellItemArrayFromShellItem(impl_->target.folder.Get(),IID_PPV_ARGS(&folder));
            return FAILED(created) ? created : registeredCommandState(command,folder.Get(),impl_->target.site.Get(),FALSE,true,result);
        } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
          catch (...) { return E_FAIL; }
    }
    return namespaceCommandState(command,scope == NamespaceMenuScope::Background ? nullptr : impl_->targets.Get(),
                                 impl_->target.site.Get(),result);
}

HRESULT NativeNamespaceActions::startCommandStateTask(std::wstring_view command,std::unique_ptr<NamespaceCommandStateTask>* result,
                                                       NamespaceMenuScope scope) {
    if (!result) return E_POINTER;
    const HRESULT hr = impl_->onThread();
    if (FAILED(hr)) return hr;
    ComPtr<IShellItemArray> folder;
    if (scope == NamespaceMenuScope::Background) {
        const HRESULT created = SHCreateShellItemArrayFromShellItem(impl_->target.folder.Get(),IID_PPV_ARGS(&folder));
        if (FAILED(created)) return created;
    }
    return NamespaceCommandStateTask::start(command,scope == NamespaceMenuScope::Background ? folder.Get() : impl_->targets.Get(),
        impl_->target.site.Get(),scope == NamespaceMenuScope::Background,result);
}

HRESULT NativeNamespaceActions::startStaticVerbStateTask(std::wstring_view verb,std::unique_ptr<NamespaceCommandStateTask>* result) {
    if(!result)return E_POINTER;
    const auto hr=impl_->onThread();if(FAILED(hr))return hr;
    return NamespaceCommandStateTask::startSelectionVerb(verb,impl_->targets.Get(),impl_->target.site.Get(),result);
}

HRESULT NativeNamespaceActions::startStaticVerbStateBatch(std::span<const std::wstring_view> verbs,std::unique_ptr<NamespaceCommandStateTask>* result) {
    if(!result)return E_POINTER;
    const auto hr=impl_->onThread();if(FAILED(hr))return hr;
    return NamespaceCommandStateTask::startSelectionVerbBatch(verbs,impl_->targets.Get(),impl_->target.site.Get(),result);
}

HRESULT NativeNamespaceActions::startActionStateTask(NamespaceAction action,std::unique_ptr<NamespaceCommandStateTask>* result) {
    if(!result)return E_POINTER;
    const auto hr=impl_->onThread();if(FAILED(hr))return hr;
    if(!namespaceActionApplicable(action,impl_->facts))return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    try {
        const auto verbs=nativeVerbs(action);
        if(verbs.empty())return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        return NamespaceCommandStateTask::startSelectionVerbs(verbs,impl_->targets.Get(),impl_->target.site.Get(),result);
    }catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
     catch(...){return E_FAIL;}
}

HRESULT NativeNamespaceActions::startRegisteredMenuStateTask(std::wstring_view command,std::unique_ptr<NamespaceCommandStateTask>* result) {
    if(!result)return E_POINTER;
    const auto hr=impl_->onThread();if(FAILED(hr))return hr;
    return NamespaceCommandStateTask::startRegisteredMenu(command,impl_->targets.Get(),impl_->target.site.Get(),result);
}

HRESULT NativeNamespaceActions::queryCommandChildren(std::wstring_view command,std::unique_ptr<NativeNamespaceCommandChildren>* result,
                                                       NamespaceMenuScope scope) {
    if (!result) return E_POINTER;
    if (!validCommand(command)) return E_INVALIDARG;
    HRESULT hr = impl_->onThread();
    if (FAILED(hr)) return hr;
    try {
        auto retained = std::make_unique<NativeNamespaceCommandChildren::Impl>();
        retained->owner = impl_->owner;
        retained->background = scope == NamespaceMenuScope::Background;
        retained->selection = impl_->targets;
        retained->site = impl_->target.site;
        hr = loadRegisteredProvider(command,retained->site.Get(),false,retained->provider);
        if (FAILED(hr)) return hr;
        EXPCMDSTATE state = ECS_DISABLED;
        hr = retained->provider.command->GetState(retained->background ? nullptr : retained->selection.Get(),FALSE,&state);
        if (FAILED(hr)) return hr;
        // Some disabled providers, such as ExtractTo outside an archive, still
        // expose read-only destinations. Library providers need an applicable
        // current-library context before their child enumeration is valid.
        if (command.starts_with(L"Windows.Library") && (state & (ECS_DISABLED | ECS_HIDDEN)))
            return HRESULT_FROM_WIN32(ERROR_ACCESS_DISABLED_BY_POLICY);
        unsigned budget = 256;
        hr = retained->capture(retained->provider.command.Get(),0,budget,retained->children,retained->metadata);
        if (FAILED(hr)) return hr;
        if (hr == S_FALSE) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        auto snapshot = std::unique_ptr<NativeNamespaceCommandChildren>(new NativeNamespaceCommandChildren(std::move(retained)));
        *result = std::move(snapshot);
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (...) { return E_FAIL; }
}

HRESULT NativeNamespaceActions::queryStaticVerbState(std::wstring_view verb,NamespaceCommandState* result) {
    if (!result) return E_POINTER;
    if (!validAssociationVerb(verb)) return E_INVALIDARG;
    HRESULT hr = impl_->onThread();
    if (FAILED(hr)) return hr;
    try {
        DWORD count = 0;
        hr = impl_->targets->GetCount(&count);
        if (FAILED(hr)) return hr;
        if (!count) return E_INVALIDARG;
        // A multi-selection's native menu owns MultiSelectModel, mixed-type
        // intersection and policy. Association presence cannot establish
        // enabled state, even when every item registers the same verb.
        if (count > 1 || (impl_->facts.nativeAttributes & SFGAO_LINK)) return E_PENDING;
        const std::wstring name(verb);
        for (DWORD index = 0; index < count; ++index) {
            ComPtr<IShellItem> item;
            ComPtr<IQueryAssociations> associations;
            hr = impl_->targets->GetItemAt(index,&item);
            if (FAILED(hr)) return hr;
            hr = item->BindToHandler(nullptr,BHID_AssociationArray,IID_PPV_ARGS(&associations));
            if(hr==E_NOINTERFACE||hr==E_NOTIMPL||hr==HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED))return E_PENDING;
            if (FAILED(hr)) return hr;
            if(!associations)return E_UNEXPECTED;
            bool available = false;
            HRESULT last = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            for (const ASSOCSTR kind : {ASSOCSTR_COMMAND,ASSOCSTR_DELEGATEEXECUTE,ASSOCSTR_DROPTARGET}) {
                DWORD length = 0;
                last = associations->GetString(ASSOCF_NOTRUNCATE,kind,name.c_str(),nullptr,&length);
                if (FAILED(last) || length < 2) continue;
                if (length > 32768) return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
                std::wstring registered(length,L'\0');
                last = associations->GetString(ASSOCF_NOTRUNCATE,kind,name.c_str(),registered.data(),&length);
                if (FAILED(last)) continue;
                if (registered.front() != L'\0') { available = true; break; }
            }
            if (!available) return FAILED(last) ? last : HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        }
        // A single-item association is only a fast negative prefilter. Its
        // positive state comes from the exact sited native menu on the worker.
        return E_PENDING;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (...) { return E_FAIL; }
}

HRESULT NativeNamespaceActions::queryZipState(NamespaceCommandState* result) {
    if (!result) return E_POINTER;
    HRESULT hr = impl_->onThread();
    if (FAILED(hr)) return hr;
    if (!impl_->facts.selectionCount || !impl_->facts.filesystem || impl_->facts.driveRoot)
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    hr = impl_->loadZipRecipient();
    if (FAILED(hr)) return hr;
    NamespaceCommandState state;
    state.state = ECS_ENABLED;
    *result = state;
    return S_OK;
}

HRESULT NativeNamespaceActions::queryExtractState(NamespaceCommandState* result) {
    if (!result) return E_POINTER;
    HRESULT hr = impl_->onThread();
    if (FAILED(hr)) return hr;
    if (impl_->facts.selectionCount != 1 || !impl_->facts.physicalFiles ||
        !equal(PathFindExtensionW(impl_->facts.singlePath.c_str()),L".zip")) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    hr = impl_->loadExtractHandler();
    if (FAILED(hr)) return hr;
    NamespaceCommandState state;
    state.state = impl_->extractEnabled ? ECS_ENABLED : ECS_DISABLED;
    *result = state;
    return S_OK;
}

HRESULT NativeNamespaceActions::queryRegisteredComponentState(std::wstring_view command,NamespaceCommandState* result) {
    if (!result) return E_POINTER;
    if (!validCommand(command)) return E_INVALIDARG;
    HRESULT hr = impl_->onThread();
    if (FAILED(hr)) return hr;
    try {
    const bool indexing = equal(command,L"Windows.ChangeIndexedLocations");
    const bool networkLocation = equal(command,L"Windows.AddNetworkLocation");
    const bool networkSharing = equal(command,L"Windows.NetworkAndSharing");
    const bool deviceWebpage = equal(command,L"Windows.NetworkViewDeviceWebpage");
    if (!indexing && !networkLocation && !networkSharing && !deviceWebpage) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    const std::wstring path = std::wstring(commandStorePath) + L"\\shell\\" + std::wstring(command);
    RegistryKey key;
    hr = HRESULT_FROM_WIN32(RegOpenKeyExW(HKEY_LOCAL_MACHINE,path.c_str(),0,KEY_READ,&key.value));
    if (FAILED(hr)) return hr;
    if (deviceWebpage) {
        // The installed static property verb has no state COM object. Its
        // documented fast-property AppliesTo rule determines availability;
        // preserve the actual URL association while leaving invocation to the
        // native menu. Never read device contents or contact the URL.
        std::wstring property,condition,association,multiselect;
        hr = readRegistryText(key.value,L"Property",property);
        if (SUCCEEDED(hr)) hr = readRegistryText(key.value,L"AppliesTo",condition);
        if (SUCCEEDED(hr)) hr = readRegistryText(key.value,L"Class",association);
        if (SUCCEEDED(hr)) hr = readRegistryText(key.value,L"MultiSelectModel",multiselect);
        if (FAILED(hr)) return hr;
        if (!equal(property,L"System.Devices.PresentationUrl") || !equal(condition,L"System.Devices.PresentationUrl:<>[]") ||
            !equal(association,L"http") || !equal(multiselect,L"Single")) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        NamespaceCommandState state;
        if (impl_->facts.selectionCount != 1) { *result = state; return S_OK; }
        ComPtr<IPropertyStore> properties;
        hr = impl_->targets->GetPropertyStore(GPS_FASTPROPERTIESONLY | GPS_BESTEFFORT,IID_PPV_ARGS(&properties));
        if (FAILED(hr)) return hr;
        PROPVARIANT value{};
        struct Clear { PROPVARIANT& value;~Clear() { PropVariantClear(&value); } } clear{value};
        hr = properties->GetValue(PKEY_Devices_PresentationUrl,&value);
        if (FAILED(hr)) return hr;
        if (value.vt == VT_EMPTY || value.vt == VT_NULL) { *result = state; return S_OK; }
        const wchar_t* url = value.vt == VT_LPWSTR ? value.pwszVal : value.vt == VT_BSTR ? value.bstrVal : nullptr;
        if (!url) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        const size_t length = wcsnlen_s(url,32769);
        if (length > 32768) return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
        if (!length) { *result = state; return S_OK; }
        ComPtr<IQueryAssociations> associations;
        hr = AssocCreate(CLSID_QueryAssociations,IID_PPV_ARGS(&associations));
        if (SUCCEEDED(hr)) hr = associations->Init(ASSOCF_IS_PROTOCOL,association.c_str(),nullptr,nullptr);
        if (FAILED(hr)) return hr;
        HRESULT unavailable = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        bool registered = false;
        for (const auto kind : {ASSOCSTR_COMMAND,ASSOCSTR_DELEGATEEXECUTE,ASSOCSTR_DROPTARGET}) {
            DWORD units = 0;
            unavailable = associations->GetString(ASSOCF_NOTRUNCATE,kind,L"open",nullptr,&units);
            if (FAILED(unavailable) || units < 2) continue;
            if (units > 32768) return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
            std::wstring text(units,L'\0');
            unavailable = associations->GetString(ASSOCF_NOTRUNCATE,kind,L"open",text.data(),&units);
            if (SUCCEEDED(unavailable) && text.front()) { registered = true; break; }
        }
        if (!registered) return FAILED(unavailable) ? unavailable : HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        state.state = ECS_ENABLED;
        *result = state;
        return S_OK;
    }
    if (indexing || networkSharing) {
        ComPtr<IOpenControlPanel> controlPanel;
        hr = CoCreateInstance(CLSID_OpenControlPanel,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&controlPanel));
        std::array<wchar_t,32768> item{};
        if (SUCCEEDED(hr)) hr = controlPanel->GetPath(indexing ? L"Microsoft.IndexingOptions" : L"Microsoft.NetworkAndSharingCenter",
                                                   item.data(),static_cast<UINT>(item.size()));
        if (SUCCEEDED(hr) && !item[0]) hr = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        if (SUCCEEDED(hr) && networkSharing) {
            std::wstring handler,verb;
            hr = readRegistryText(key.value,L"VerbHandler",handler);
            if (SUCCEEDED(hr)) hr = readRegistryText(key.value,L"VerbName",verb);
            if (SUCCEEDED(hr) && !guidText(handler)) hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            if (SUCCEEDED(hr) && !equal(verb,L"OpenNetCenter")) hr = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            // VerbHandler also names Shell-owned factories that are not
            // externally CoCreate-able COM classes. The real registered menu
            // owns invocation; GetPath proves the named public component.
        }
    } else {
        // Validate the installed DLL/export named by the registered native
        // wizard. Never execute the export while querying a Ribbon capability.
        const HMODULE module = LoadLibraryExW(L"shwebsvc.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) return HRESULT_FROM_WIN32(GetLastError());
        hr = GetProcAddress(module,"AddNetPlaceRunDll") ? S_OK : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        FreeLibrary(module);
    }
    if (FAILED(hr)) return hr;
    NamespaceCommandState state;
    state.state = ECS_ENABLED;
    *result = state;
    return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (...) { return E_FAIL; }
}

HRESULT NativeNamespaceActions::queryActionState(NamespaceAction action,NamespaceCommandState* result,
                                                 std::vector<std::wstring_view>* selectionVerbs) {
    if (!result) return E_POINTER;
    HRESULT hr = impl_->onThread();
    if (FAILED(hr)) return hr;
    if (!namespaceActionApplicable(action,impl_->facts)) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    // The installed compatibility provider's direct FALSE/TRUE state does not
    // perform the native menu's application-shortcut redirection. Preserve the
    // original link and let its exact registered menu provide the state.
    if(action==NamespaceAction::TroubleshootCompatibility&&(impl_->facts.nativeAttributes&SFGAO_LINK))return E_PENDING;
    if (action == NamespaceAction::RestoreAll || action == NamespaceAction::EmptyRecycleBin ||
        (action == NamespaceAction::AlwaysAvailableOffline && impl_->facts.detailedTargetsKnown)) {
        NamespaceInvocationPlan plan;
        hr = planNamespaceAction(action,impl_->facts,{},{},&plan);
        if (FAILED(hr)) return hr;
        NamespaceCommandState state;
        state.state = static_cast<EXPCMDSTATE>((plan.enabled ? ECS_ENABLED : ECS_DISABLED) | (plan.checked ? ECS_CHECKED : 0));
        *result = state;
        return S_OK;
    }
    const auto command = namespaceActionCommand(action);
    hr = command.empty() ? HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) : queryCommandState(command,result);
    if (hr != HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) && hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) return hr;
    // Installed SendTo recipients prove component presence, not eligibility of
    // unknown filesystem/type facts. Only an actual full-selection provider
    // can authorize these commands when detailed inspection was skipped.
    if (!impl_->facts.detailedTargetsKnown && (action == NamespaceAction::Email || action == NamespaceAction::Fax))
        return hr;
    if (action == NamespaceAction::Email) {
        hr = impl_->loadMailRecipient();
        if (SUCCEEDED(hr)) { NamespaceCommandState state; state.state = ECS_ENABLED; *result = state; }
        return hr;
    }
    if (action == NamespaceAction::Fax) {
        hr = impl_->loadFaxRecipient();
        if (SUCCEEDED(hr)) { NamespaceCommandState state; state.state = ECS_ENABLED; *result = state; }
        return hr;
    }
    if (action == NamespaceAction::CastToDevice) {
        hr = impl_->loadCastHandler();
        if (SUCCEEDED(hr)) { NamespaceCommandState state; state.state = impl_->facts.castHandlerEnabled ? ECS_ENABLED : ECS_DISABLED; *result = state; }
        return hr;
    }
    const auto aliases=nativeVerbs(action);
    for (const auto verb : aliases) {
        hr = queryStaticVerbState(verb,result);
        if (SUCCEEDED(hr)) return hr;
        if (hr == E_PENDING) {if(selectionVerbs)*selectionVerbs=aliases;return hr;}
    }
    return hr;
}

HRESULT NativeNamespaceActions::invokeZip(bool headless,POINT point) {
    HRESULT hr = impl_->visibleInteraction(headless);
    if (FAILED(hr)) return hr;
    hr = refresh();
    if (FAILED(hr)) return hr;
    NamespaceCommandState state;
    hr = queryZipState(&state);
    if (FAILED(hr)) return hr;
    NamespaceInvocationPlan plan;
    hr = planCommandStore(L"Windows.zip",&plan,NamespaceMenuScope::Selection);
    if (SUCCEEDED(hr)) return plan.enabled ? impl_->invokePlan(plan,point) : plan.status;
    if (hr != HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) && hr != HRESULT_FROM_WIN32(ERROR_NOT_FOUND) &&
        hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) return hr;
    return impl_->sendZip(point);
}

HRESULT NativeNamespaceActions::invokeExtract(bool headless,POINT point) {
    HRESULT hr = impl_->visibleInteraction(headless);
    if (FAILED(hr)) return hr;
    hr = refresh();
    if (FAILED(hr)) return hr;
    NamespaceCommandState state;
    hr = queryExtractState(&state);
    if (FAILED(hr)) return hr;
    if (!state.enabled()) return HRESULT_FROM_WIN32(ERROR_ACCESS_DISABLED_BY_POLICY);
    NamespaceInvocationPlan plan;
    hr = planCommandStore(L"Windows.CompressedFolder.extract",&plan,NamespaceMenuScope::Selection);
    if (SUCCEEDED(hr)) return plan.enabled ? impl_->invokePlan(plan,point) : plan.status;
    if (hr != HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) && hr != HRESULT_FROM_WIN32(ERROR_NOT_FOUND) &&
        hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) return hr;
    struct ActiveMenu {
        Impl* owner;
        ~ActiveMenu() { owner->active = nullptr; }
    } active{impl_.get()};
    impl_->active = &impl_->extractMenu;
    return impl_->extractMenu.invoke(impl_->extractCommand,point);
}

HRESULT NativeNamespaceActions::queryCommandStorePopup(std::wstring_view command, NamespaceCommandPopup* result,
                                                       NamespaceMenuScope scope) {
    if (!result) return E_POINTER;
    NamespaceInvocationPlan plan;
    HRESULT hr = planCommandStore(command,&plan,scope);
    if (FAILED(hr)) return hr;
    if (!plan.submenu) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    NativeContextMenu& native = scope == NamespaceMenuScope::Background ? impl_->backgroundCommands : impl_->commands;
    unsigned budget = maximumMenuEntries;
    const HMENU menu = findSubmenu(native.menu(),plan.commandId,0,budget);
    if (!menu) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    budget = maximumMenuEntries;
    MenuMatch match;
    findVerb(scope == NamespaceMenuScope::Background ? impl_->backgroundEntries : impl_->commandsEntries,
             command,true,0,budget,match);
    if (!match.item || match.ambiguous) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    NamespaceCommandPopup popup;
    popup.menu = menu;
    popup.generation = impl_->menuGeneration;
    popup.plan = std::move(plan);
    popup.entries = match.item->children;
    *result = std::move(popup);
    return S_OK;
}

HRESULT NativeNamespaceActions::invokeCommandStorePopup(const NamespaceCommandPopup& popup,bool headless,POINT point) {
    HRESULT hr = impl_->visibleInteraction(headless);
    if (FAILED(hr)) return hr;
    if (!popup.menu || popup.generation != impl_->menuGeneration) return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    NamespaceInvocationPlan current;
    hr = planCommandStore(popup.plan.canonicalVerb,&current,popup.plan.scope);
    if (FAILED(hr)) return hr;
    if (!current.enabled) return current.status;
    if (!current.submenu || current.commandId != popup.plan.commandId) return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    auto& native = popup.plan.scope == NamespaceMenuScope::Background ? impl_->backgroundCommands : impl_->commands;
    unsigned budget = maximumMenuEntries;
    if (findSubmenu(native.menu(),current.commandId,0,budget) != popup.menu) return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    return impl_->invokePlan(current,point);
}

bool NativeNamespaceActions::handleMenuMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result) {
    return impl_->active && impl_->active->handleMessage(message, wParam, lParam, result);
}

} // namespace explorer
