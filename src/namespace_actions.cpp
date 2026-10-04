#include "explorer/namespace_actions.hpp"

#include <shlobj.h>
#include <shlwapi.h>
#include <propkey.h>
#include <propsys.h>
#include <cscobj.h>
#include <wrl.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace explorer {
namespace {
using Microsoft::WRL::ComPtr;
constexpr wchar_t commandStorePath[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\CommandStore";
constexpr DWORD maximumSelection = 100000;
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
        if (metadata.flags & ECF_HASSUBCOMMANDS) {
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
    case A::RotateLeft: case A::RotateRight: case A::SlideShow: return selected && facts.physicalFiles && facts.images;
    case A::SetWallpaper: return selected && one && facts.physicalFiles && facts.images;
    case A::RestoreSelected: return facts.recycleBin && selected;
    case A::RestoreAll: case A::EmptyRecycleBin: return facts.recycleBin;
    case A::RunAsAdministrator: case A::TroubleshootCompatibility:
        return selected && one && facts.physicalFiles && facts.applications;
    case A::IncludeInLibrary: return one && facts.physicalFolders && !facts.driveRoot;
    case A::AlwaysAvailableOffline: case A::WorkOffline: case A::SyncOffline: return facts.filesystem && facts.uncPaths;
    case A::MapAsDrive: return one && facts.physicalFolders && facts.uncPaths;
    case A::ShareSpecificPeople: case A::RemoveAccess: case A::AdvancedSecurity: return facts.filesystem && one;
    case A::FileHistory: return facts.filesystem && one && !facts.driveRoot;
    case A::Email: case A::Fax: return selected && facts.physicalFiles;
    case A::BurnToDisc: return selected && facts.filesystem && !facts.driveRoot;
    case A::Play: case A::PlayAll: case A::AddToPlaylist:
        return selected && facts.physicalFiles && facts.media;
    case A::CastToDevice: return selected && facts.physicalFiles && (facts.media || facts.images || facts.castItems);
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

HRESULT describeTargets(IShellItem* folder, IShellItemArray* targets, DWORD selected,
                        NamespaceFacts& facts) {
    facts.selectionCount = selected;
    DWORD count = 0;
    HRESULT hr = targets->GetCount(&count);
    if (FAILED(hr)) return hr;
    if (!count || count > maximumSelection) return E_INVALIDARG;
    facts.filesystem = facts.physicalFiles = facts.physicalFolders = true;
    facts.images = facts.applications = facts.discImages = facts.media = facts.castItems = facts.uncPaths = true;
    for (DWORD index = 0; index < count; ++index) {
        ComPtr<IShellItem> item;
        hr = targets->GetItemAt(index, &item);
        if (FAILED(hr)) return hr;
        SFGAOF flags = 0;
        hr = item->GetAttributes(SFGAO_FILESYSTEM, &flags);
        if (FAILED(hr)) return hr;
        std::wstring path;
        if (!(flags & SFGAO_FILESYSTEM) || FAILED(itemPath(item.Get(), path))) {
            facts.filesystem = facts.physicalFiles = facts.physicalFolders = false;
            facts.images = facts.applications = facts.discImages = facts.media = facts.castItems = facts.uncPaths = false;
            continue;
        }
        const DWORD attributes = GetFileAttributesW(path.c_str());
        const bool exists = attributes != INVALID_FILE_ATTRIBUTES;
        const bool directory = exists && (attributes & FILE_ATTRIBUTE_DIRECTORY);
        facts.physicalFiles &= exists && !directory;
        facts.physicalFolders &= directory;
        facts.uncPaths &= path.size() > 4 && path[0] == L'\\' && path[1] == L'\\' && path[2] != L'?' && path[2] != L'.';
        const auto extension = std::wstring_view(PathFindExtensionW(path.c_str()));
        PERCEIVED type = PERCEIVED_TYPE_UNSPECIFIED;
        PERCEIVEDFLAG perceivedFlags = 0;
        AssocGetPerceivedType(std::wstring(extension).c_str(), &type, &perceivedFlags, nullptr);
        ComPtr<IShellItem2> item2;
        if (SUCCEEDED(item.As(&item2))) {
            PROPVARIANT perceived{};
            if (SUCCEEDED(item2->GetProperty(PKEY_PerceivedType, &perceived)) && perceived.vt == VT_I4)
                type = static_cast<PERCEIVED>(perceived.lVal);
            PropVariantClear(&perceived);
        }
        facts.images &= exists && !directory && type == PERCEIVED_TYPE_IMAGE;
        facts.media &= exists && !directory && (type == PERCEIVED_TYPE_AUDIO || type == PERCEIVED_TYPE_VIDEO);
        facts.castItems &= exists && !directory && (type == PERCEIVED_TYPE_IMAGE || type == PERCEIVED_TYPE_AUDIO || type == PERCEIVED_TYPE_VIDEO);
        facts.applications &= exists && !directory && extensionIs(extension, {L".exe", L".com", L".msi", L".bat", L".cmd", L".lnk"});
        facts.discImages &= exists && !directory && extensionIs(extension, {L".iso", L".img", L".vhd", L".vhdx"});
        if (count == 1) facts.singlePath = std::move(path);
    }
    if (count == 1 && facts.physicalFolders && facts.singlePath.size() == 3 &&
        ((facts.singlePath[0] >= L'A' && facts.singlePath[0] <= L'Z') ||
         (facts.singlePath[0] >= L'a' && facts.singlePath[0] <= L'z')) &&
        facts.singlePath[1] == L':' && facts.singlePath[2] == L'\\') {
        facts.driveRoot = true;
        facts.driveType = GetDriveTypeW(facts.singlePath.c_str());
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

HRESULT namespaceCommandMetadata(std::wstring_view command, NamespaceCommandMetadata* result) {
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
    *result = std::move(metadata);
    return S_OK;
}

HRESULT namespaceCommandState(std::wstring_view command, IShellItemArray* selection,
                              IUnknown* site, NamespaceCommandState* result) {
    if (!result) return E_POINTER;
    if (!validCommand(command)) return E_INVALIDARG;
    APTTYPE apartment{};
    APTTYPEQUALIFIER qualifier{};
    HRESULT hr = CoGetApartmentType(&apartment,&qualifier);
    if (FAILED(hr)) return hr;
    if (apartment != APTTYPE_STA && apartment != APTTYPE_MAINSTA) return RPC_E_WRONG_THREAD;
    try {
        const std::wstring name(command);
        const std::wstring path = std::wstring(commandStorePath) + L"\\shell\\" + name;
        RegistryKey key;
        const LONG error = RegOpenKeyExW(HKEY_LOCAL_MACHINE,path.c_str(),0,KEY_READ,&key.value);
        if (error != ERROR_SUCCESS) return HRESULT_FROM_WIN32(error);
        std::wstring handler;
        NamespaceCommandState state;
        hr = readRegistryText(key.value,L"ExplorerCommandHandler",handler);
        state.explorerCommand = SUCCEEDED(hr);
        if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
            hr = readRegistryText(key.value,L"CommandStateHandler",handler);
        if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        if (FAILED(hr)) return hr;
        if (!guidText(handler)) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        hr = CLSIDFromString(handler.c_str(),&state.handler);
        if (FAILED(hr)) return hr;
        ComPtr<IExplorerCommand> native;
        ComPtr<IExplorerCommandState> nativeState;
        ComPtr<IUnknown> provider;
        if (state.explorerCommand) {
            hr = CoCreateInstance(state.handler,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&native));
            if (SUCCEEDED(hr)) hr = native.As(&provider);
        } else {
            hr = CoCreateInstance(state.handler,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&nativeState));
            if (SUCCEEDED(hr)) hr = nativeState.As(&provider);
        }
        if (FAILED(hr)) return hr;
        ComPtr<IInitializeCommand> initialize;
        hr = provider.As(&initialize);
        if (SUCCEEDED(hr)) {
            auto bag = Microsoft::WRL::Make<RegistryPropertyBag>(path);
            if (!bag) return E_OUTOFMEMORY;
            hr = bag->status();
            if (SUCCEEDED(hr)) hr = initialize->Initialize(name.c_str(),bag.Get());
            if (FAILED(hr)) return hr;
            state.initialized = true;
        } else if (hr != E_NOINTERFACE) return hr;
        ComPtr<IObjectWithSite> withSite;
        hr = provider.As(&withSite);
        if (FAILED(hr) && hr != E_NOINTERFACE) return hr;
        if (site && withSite) {
            hr = withSite->SetSite(site);
            if (FAILED(hr)) return hr;
            state.siteAttached = true;
        }
        struct DetachSite {
            ComPtr<IObjectWithSite> value;
            bool attached;
            ~DetachSite() { if (attached && value) value->SetSite(nullptr); }
        } detach{withSite,state.siteAttached};
        hr = native ? native->GetState(selection,FALSE,&state.state)
                    : nativeState->GetState(selection,FALSE,&state.state);
        if (FAILED(hr)) return hr;
        *result = state;
        return hr;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (...) { return E_FAIL; }
}

HRESULT namespaceCommandChildren(std::wstring_view command, IShellItemArray* selection,
                                 IUnknown* site,
                                 std::vector<NamespaceSubcommandMetadata>* result) {
    if (!result) return E_POINTER;
    if (!validCommand(command)) return E_INVALIDARG;
    const std::wstring path = std::wstring(commandStorePath) + L"\\shell\\" + std::wstring(command);
    RegistryKey key;
    LONG error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &key.value);
    if (error != ERROR_SUCCESS) return HRESULT_FROM_WIN32(error);
    std::wstring handler;
    HRESULT hr = readRegistryText(key.value, L"ExplorerCommandHandler", handler);
    if (FAILED(hr)) return hr;
    // CLSIDFromString also accepts a ProgID, which can modify registration. Only
    // accept an already registered literal GUID and never use that side path.
    if (!guidText(handler)) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    CLSID clsid{};
    hr = CLSIDFromString(handler.c_str(), &clsid);
    if (FAILED(hr)) return hr;
    ComPtr<IExplorerCommand> native;
    hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&native));
    if (FAILED(hr)) return hr;
    ComPtr<IObjectWithSite> withSite;
    native.As(&withSite);
    if (site && withSite) {
        hr = withSite->SetSite(site);
        if (FAILED(hr)) return hr;
    }
    struct DetachSite {
        ComPtr<IObjectWithSite> value;
        bool attached;
        ~DetachSite() { if (attached && value) value->SetSite(nullptr); }
    } detach{withSite, site != nullptr && withSite != nullptr};
    unsigned budget = 256;
    std::vector<NamespaceSubcommandMetadata> metadata;
    hr = commandChildren(native.Get(), selection, 0, budget, metadata);
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
    if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) && action == NamespaceAction::AlwaysAvailableOffline && facts.offlineActive) {
        plan.route = NamespaceInvocationRoute::OfflineFilesPin;
        plan.canonicalVerb = facts.offlinePinnedForUser ? L"IOfflineFilesCache.Unpin" : L"IOfflineFilesCache.Pin";
        plan.enabled = true;
        plan.checked = facts.offlinePinnedForUser;
        plan.status = S_OK;
        *result = std::move(plan);
        return S_OK;
    }
    if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) && action == NamespaceAction::Email && facts.mailRecipientAvailable) {
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
            if (SUCCEEDED(hr) && (!count || count > maximumSelection)) hr = E_INVALIDARG;
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

    HRESULT restoreAll(POINT point) {
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
            if (owned.size() == maximumSelection) return HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES);
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
    }
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
    if (count > maximumSelection) return E_INVALIDARG;
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
    NamespaceCommandMetadata metadata;
    HRESULT hr = namespaceCommandMetadata(command, &metadata);
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
                                                  NamespaceMenuScope scope) {
    HRESULT hr = impl_->visibleInteraction(headless);
    if (FAILED(hr)) return hr;
    hr = refresh();
    if (FAILED(hr)) return hr;
    NamespaceInvocationPlan plan;
    hr = planCommandStore(command, &plan, scope);
    if (FAILED(hr)) return hr;
    return plan.enabled ? impl_->invokePlan(plan, point) : plan.status;
}

HRESULT NativeNamespaceActions::queryCommandState(std::wstring_view command, NamespaceCommandState* result,
                                                  NamespaceMenuScope scope) {
    const HRESULT hr = impl_->onThread();
    if (FAILED(hr)) return hr;
    return namespaceCommandState(command,scope == NamespaceMenuScope::Background ? nullptr : impl_->targets.Get(),
                                 impl_->target.site.Get(),result);
}

HRESULT NativeNamespaceActions::queryStaticVerbState(std::wstring_view verb,NamespaceCommandState* result) {
    if (!result) return E_POINTER;
    if (verb.empty() || verb.size() > 128 || verb.find_first_of(L"\\/\0",0,3) != std::wstring_view::npos) return E_INVALIDARG;
    HRESULT hr = impl_->onThread();
    if (FAILED(hr)) return hr;
    try {
        DWORD count = 0;
        hr = impl_->targets->GetCount(&count);
        if (FAILED(hr)) return hr;
        if (!count) return E_INVALIDARG;
        // Mixed selections require every item's actual association, since the
        // array's association handler is documented to cover its first item.
        if (count > 4096) return E_PENDING;
        const std::wstring name(verb);
        for (DWORD index = 0; index < count; ++index) {
            ComPtr<IShellItem> item;
            ComPtr<IQueryAssociations> associations;
            hr = impl_->targets->GetItemAt(index,&item);
            if (SUCCEEDED(hr)) hr = item->BindToHandler(nullptr,BHID_AssociationArray,IID_PPV_ARGS(&associations));
            if (FAILED(hr)) return hr;
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
        NamespaceCommandState state;
        state.state = ECS_ENABLED;
        *result = state;
        return S_OK;
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

HRESULT NativeNamespaceActions::queryActionState(NamespaceAction action,NamespaceCommandState* result) {
    if (!result) return E_POINTER;
    HRESULT hr = impl_->onThread();
    if (FAILED(hr)) return hr;
    if (!namespaceActionApplicable(action,impl_->facts)) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    if (action == NamespaceAction::RestoreAll || action == NamespaceAction::EmptyRecycleBin ||
        action == NamespaceAction::AlwaysAvailableOffline) {
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
    if (action == NamespaceAction::Email) {
        hr = impl_->loadMailRecipient();
        if (SUCCEEDED(hr)) { NamespaceCommandState state; state.state = ECS_ENABLED; *result = state; }
        return hr;
    }
    if (action == NamespaceAction::CastToDevice) {
        hr = impl_->loadCastHandler();
        if (SUCCEEDED(hr)) { NamespaceCommandState state; state.state = impl_->facts.castHandlerEnabled ? ECS_ENABLED : ECS_DISABLED; *result = state; }
        return hr;
    }
    for (const auto verb : nativeVerbs(action)) {
        hr = queryStaticVerbState(verb,result);
        if (SUCCEEDED(hr)) return hr;
        if (hr == E_PENDING) return hr;
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
