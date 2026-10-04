#include "explorer/shell_operations.hpp"
#include "explorer/item_actions.hpp"
#include "explorer/context_menu.hpp"
#include "explorer/core.hpp"

#include <shlobj.h>
#include <shellapi.h>
#include <sherrors.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <cstring>
#include <memory>
#include <utility>
#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <map>
#include <vector>

namespace explorer {
namespace {
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;

// OLE and Shell operations belong to the calling STA. Retain only data we
// published, then explicitly release it before that apartment is uninitialized.
thread_local ComPtr<IDataObject> ownedClipboard;

HRESULT publishClipboard(IDataObject* data) {
    const HRESULT hr = OleSetClipboard(data);
    if (SUCCEEDED(hr)) ownedClipboard = data;
    return hr;
}

HRESULT lastError() {
    const DWORD error = GetLastError();
    return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
}

HRESULT checkSelection(IShellItemArray* selection) {
    if (!selection) return E_INVALIDARG;
    DWORD count = 0;
    const HRESULT hr = selection->GetCount(&count);
    return FAILED(hr) ? hr : count ? S_OK : E_INVALIDARG;
}

struct CoTaskDeleter {
    void operator()(wchar_t* value) const { CoTaskMemFree(value); }
};
using TaskString = std::unique_ptr<wchar_t, CoTaskDeleter>;

struct HandleCloser { void operator()(void* value) const { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
using Handle = std::unique_ptr<void, HandleCloser>;
namespace fs = std::filesystem;

struct PathLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(),
                                    static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
    }
};
bool samePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(),
                                static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
HRESULT filesystemPath(IShellItem* item, std::wstring& path) {
    if (!item) return E_INVALIDARG;
    PWSTR raw = nullptr;
    const HRESULT hr = item->GetDisplayName(SIGDN_FILESYSPATH, &raw);
    TaskString value(raw);
    if (FAILED(hr)) return hr;
    if (!value || !*value.get()) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    path = value.get();
    return S_OK;
}

// FileBasicInfo.ChangeTime catches data or metadata changes even if a caller
// restores LastWriteTime. No file content is read for journaling. Reparse trees
// and filesystems without stable 128-bit identities are left to native history.
struct Stamp {
    FILE_ID_INFO id{};
    FILE_BASIC_INFO basic{};
    LONGLONG bytes = 0;
};
bool sameIdentity(const Stamp& a, const Stamp& b) {
    return a.id.VolumeSerialNumber == b.id.VolumeSerialNumber &&
           std::memcmp(&a.id.FileId, &b.id.FileId, sizeof(a.id.FileId)) == 0;
}
bool sameStamp(const Stamp& a, const Stamp& b) {
    return sameIdentity(a, b) && a.bytes == b.bytes &&
        a.basic.CreationTime.QuadPart == b.basic.CreationTime.QuadPart &&
        a.basic.LastWriteTime.QuadPart == b.basic.LastWriteTime.QuadPart &&
        a.basic.ChangeTime.QuadPart == b.basic.ChangeTime.QuadPart &&
        a.basic.FileAttributes == b.basic.FileAttributes;
}
HRESULT stamp(HANDLE file, Stamp& result) {
    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandleEx(file, FileIdInfo, &result.id, sizeof(result.id)) ||
        !GetFileInformationByHandleEx(file, FileBasicInfo, &result.basic, sizeof(result.basic)) ||
        !GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard))) return lastError();
    if (result.basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    result.bytes = standard.EndOfFile.QuadPart;
    return S_OK;
}
struct Node { std::wstring relative; Stamp value; };
struct Snapshot { std::vector<Node> nodes; };
struct LockedTree { Snapshot snapshot; std::vector<Handle> handles; };
HRESULT readTree(const std::wstring& path, const std::wstring& relative, bool lock,
                 LockedTree& tree, unsigned depth = 0) {
    if (depth > 256 || tree.snapshot.nodes.size() >= 100000)
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    Handle file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | (lock && relative.empty() ? DELETE : 0),
        lock ? FILE_SHARE_READ | (relative.empty() ? 0 : FILE_SHARE_DELETE)
             : FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) return lastError();
    Stamp info{};
    HRESULT hr = stamp(file.get(), info);
    if (FAILED(hr)) return hr;
    tree.snapshot.nodes.push_back({relative, info});
    if (lock) tree.handles.push_back(std::move(file));
    if (!(info.basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return S_OK;
    WIN32_FIND_DATAW data{};
    const std::wstring pattern = (fs::path(path) / L"*").wstring();
    HANDLE search = FindFirstFileW(pattern.c_str(), &data);
    if (search == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(error);
    }
    std::vector<std::wstring> children;
    do {
        if (std::wcscmp(data.cFileName, L".") && std::wcscmp(data.cFileName, L".."))
            children.emplace_back(data.cFileName);
    } while (FindNextFileW(search, &data));
    const DWORD error = GetLastError();
    FindClose(search);
    if (error != ERROR_NO_MORE_FILES) return HRESULT_FROM_WIN32(error);
    std::sort(children.begin(), children.end());
    for (const auto& child : children) {
        hr = readTree((fs::path(path) / child).wstring(), (fs::path(relative) / child).wstring(),
                      lock, tree, depth + 1);
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}
HRESULT takeSnapshot(const std::wstring& path, Snapshot& output) {
    LockedTree tree;
    const HRESULT hr = readTree(path, L"", false, tree);
    if (SUCCEEDED(hr)) output = std::move(tree.snapshot);
    return hr;
}
bool sameSnapshot(const Snapshot& a, const Snapshot& b) {
    if (a.nodes.size() != b.nodes.size()) return false;
    for (size_t i = 0; i < a.nodes.size(); ++i)
        if (a.nodes[i].relative != b.nodes[i].relative || !sameStamp(a.nodes[i].value, b.nodes[i].value)) return false;
    return true;
}
bool sameAfterRename(const Snapshot& before, const Snapshot& after) {
    if (before.nodes.size() != after.nodes.size() || before.nodes.empty()) return false;
    for (size_t i = 0; i < before.nodes.size(); ++i) {
        if (before.nodes[i].relative != after.nodes[i].relative) return false;
        Stamp previous = before.nodes[i].value;
        // The journal itself changes this field by renaming the root object.
        // Descendant changes and root content/membership metadata stay guarded.
        if (!i) previous.basic.ChangeTime = after.nodes[i].value.basic.ChangeTime;
        if (!sameStamp(previous, after.nodes[i].value)) return false;
    }
    return true;
}
HRESULT directoryStamp(const fs::path& path, Stamp& output) {
    Handle directory(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (directory.get() == INVALID_HANDLE_VALUE) return lastError();
    HRESULT hr = stamp(directory.get(), output);
    return SUCCEEDED(hr) && !(output.basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        ? HRESULT_FROM_WIN32(ERROR_DIRECTORY) : hr;
}
HRESULT absentOrSame(const std::wstring& path, const Stamp& source) {
    Handle existing(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (existing.get() == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(error);
    }
    Stamp info{};
    const HRESULT hr = stamp(existing.get(), info);
    return FAILED(hr) ? hr : sameIdentity(info, source) ? S_OK : HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
}
HRESULT renameHandle(HANDLE file, const std::wstring& destination) {
    const size_t bytes = offsetof(FILE_RENAME_INFO, FileName) + (destination.size() + 1) * sizeof(wchar_t);
    if (bytes > MAXDWORD) return E_INVALIDARG;
    std::vector<BYTE> memory(bytes);
    auto info = reinterpret_cast<FILE_RENAME_INFO*>(memory.data());
    info->ReplaceIfExists = FALSE;
    info->RootDirectory = nullptr;
    info->FileNameLength = static_cast<DWORD>(destination.size() * sizeof(wchar_t));
    std::memcpy(info->FileName, destination.data(), info->FileNameLength);
    return SetFileInformationByHandle(file, FileRenameInfo, info, static_cast<DWORD>(bytes)) ? S_OK : lastError();
}

enum class ActionKind { Copy, Move, Rename, NewFolder };
struct Action {
    ActionKind kind{};
    std::wstring before, after, holding;
    Stamp beforeParent{}, afterParent{}, holdingOwner{};
    Snapshot expected;
};
struct Transaction { std::vector<Action> actions; bool poisoned = false; };
struct Journal {
    std::vector<Transaction> undo, redo;
    std::vector<std::wstring> preserved;
    bool executing = false;
};
thread_local Journal journal;

struct RecordRequest {
    ActionKind kind{};
    std::vector<std::wstring> sources;
    std::vector<Stamp> parents;
    std::map<std::wstring, bool, PathLess> existingTargets;
    std::map<IShellItem*, size_t> sourceIndexes;
    std::vector<Action> completed;
    bool valid = true;
    void prepare(IShellItem* source) {
        if (!valid || !source) return;
        std::wstring path;
        if (FAILED(filesystemPath(source, path))) return;
        for (size_t i = 0; i < sources.size(); ++i)
            if (samePath(path, sources[i])) { sourceIndexes[source] = i; break; }
    }
    void capture(IShellItem* source, IShellItem* created) {
        if (!valid) return;
        std::wstring before;
        size_t index = 0;
        if (kind != ActionKind::NewFolder) {
            const auto known = sourceIndexes.find(source);
            if (known != sourceIndexes.end()) {
                index = known->second;
                before = sources[index];
            } else {
                if (FAILED(filesystemPath(source, before))) { valid = false; return; }
                for (; index < sources.size(); ++index) if (samePath(before, sources[index])) break;
            }
            // Advise also receives child callbacks of directory transfers.
            if (index == sources.size()) return;
        }
        std::wstring after;
        if (FAILED(filesystemPath(created, after))) { valid = false; return; }
        if (existingTargets.contains(after) && !(kind == ActionKind::Rename && samePath(before, after))) {
            valid = false; return; // A replacement/merge cannot be reversed without its previous contents.
        }
        for (const auto& action : completed) if (samePath(action.after, after)) { valid = false; return; }
        Action action;
        action.kind = kind;
        action.before = std::move(before);
        action.after = std::move(after);
        if (kind != ActionKind::NewFolder) action.beforeParent = parents[index];
        if (FAILED(directoryStamp(fs::path(action.after).parent_path(), action.afterParent))) { valid = false; return; }
        completed.push_back(std::move(action));
    }
};
void prepareSources(RecordRequest& request, IShellItemArray* selection) {
    DWORD count = 0;
    if (!selection || FAILED(selection->GetCount(&count)) || count > 4096) { request.valid = false; return; }
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IShellItem> source;
        std::wstring path;
        Stamp parent{};
        if (FAILED(selection->GetItemAt(i, &source)) || FAILED(filesystemPath(source.Get(), path)) ||
            FAILED(directoryStamp(fs::path(path).parent_path(), parent))) { request.valid = false; return; }
        request.sources.push_back(std::move(path));
        request.parents.push_back(parent);
    }
}
void prepareDestination(RecordRequest& request, IShellItem* destination) {
    std::wstring path;
    if (!destination || FAILED(filesystemPath(destination, path))) { request.valid = false; return; }
    std::error_code error;
    fs::directory_iterator entries(path, error);
    if (error) { request.valid = false; return; }
    const fs::directory_iterator end;
    while (entries != end) {
        if (request.existingTargets.size() >= 100000) { request.valid = false; return; }
        request.existingTargets.emplace(entries->path().wstring(), true);
        entries.increment(error);
        if (error) { request.valid = false; return; }
    }
}

void preserveHolding(Transaction& transaction) {
    for (auto& action : transaction.actions) {
        if (action.holding.empty()) continue;
        const fs::path directory = fs::path(action.holding).parent_path();
        Stamp current{};
        if (FAILED(directoryStamp(directory, current)) || !sameIdentity(current, action.holdingOwner)) continue;
        // RemoveDirectory cannot delete nonempty data. Never recursively delete
        // retained copies or an unexpected object in a holding container.
        if (!RemoveDirectoryW(directory.c_str())) {
            const std::wstring path = directory.wstring();
            if (std::find(journal.preserved.begin(), journal.preserved.end(), path) == journal.preserved.end())
                journal.preserved.push_back(path);
        }
    }
}
void finishRecord(RecordRequest* request, HRESULT result) {
    if (!request || FAILED(result)) return;
    for (auto& transaction : journal.redo) preserveHolding(transaction);
    journal.redo.clear();
    if (!request->valid || request->completed.empty() ||
        (request->kind != ActionKind::NewFolder && request->completed.size() != request->sources.size())) return;
    for (auto& action : request->completed)
        if (FAILED(takeSnapshot(action.after, action.expected)) ||
            ((action.kind == ActionKind::Move || action.kind == ActionKind::Rename) &&
             action.beforeParent.id.VolumeSerialNumber != action.afterParent.id.VolumeSerialNumber)) return;
    if (journal.undo.size() >= 100) {
        preserveHolding(journal.undo.front());
        journal.undo.erase(journal.undo.begin());
    }
    journal.undo.push_back({std::move(request->completed), false});
}

HRESULT makeHolding(Action& action) {
    if (!action.holding.empty()) return S_OK;
    GUID id{};
    HRESULT hr = CoCreateGuid(&id);
    if (FAILED(hr)) return hr;
    wchar_t text[40]{};
    if (!StringFromGUID2(id, text, 40)) return E_FAIL;
    const fs::path directory = fs::path(action.after).parent_path() /
        (std::wstring(L".windows-explorer-undo-") + text);
    if (!CreateDirectoryW(directory.c_str(), nullptr)) return lastError();
    if (!SetFileAttributesW(directory.c_str(), FILE_ATTRIBUTE_HIDDEN)) {
        hr = lastError();
        RemoveDirectoryW(directory.c_str());
        return hr;
    }
    hr = directoryStamp(directory, action.holdingOwner);
    if (FAILED(hr)) { RemoveDirectoryW(directory.c_str()); return hr; }
    action.holding = (directory / fs::path(action.after).filename()).wstring();
    return S_OK;
}
HRESULT lockParent(const fs::path& path, const Stamp& expected, std::vector<Handle>& locks) {
    Handle parent(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (parent.get() == INVALID_HANDLE_VALUE) return lastError();
    Stamp current{};
    const HRESULT hr = stamp(parent.get(), current);
    if (FAILED(hr)) return hr;
    if (!sameIdentity(current, expected) || !(current.basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
    locks.push_back(std::move(parent));
    return S_OK;
}
HRESULT verifyHolding(const Action& action, bool containsObject) {
    std::error_code error;
    fs::directory_iterator entries(fs::path(action.holding).parent_path(), error);
    if (error) return HRESULT_FROM_WIN32(static_cast<DWORD>(error.value()));
    const fs::directory_iterator end;
    size_t count = 0;
    while (entries != end) {
        if (!containsObject || count++ || !samePath(entries->path().wstring(), action.holding))
            return HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
        entries.increment(error);
        if (error) return HRESULT_FROM_WIN32(static_cast<DWORD>(error.value()));
    }
    return containsObject && count != 1 ? HRESULT_FROM_WIN32(ERROR_FILE_INVALID) : S_OK;
}
HRESULT executeJournal(bool redo) {
    auto& source = redo ? journal.redo : journal.undo;
    auto& destination = redo ? journal.undo : journal.redo;
    if (source.empty()) return HRESULT_FROM_WIN32(ERROR_NO_MORE_ITEMS);
    if (journal.executing || source.back().poisoned) return HRESULT_FROM_WIN32(ERROR_BUSY);
    struct ExecutionGuard {
        ExecutionGuard() { journal.executing = true; }
        ~ExecutionGuard() { journal.executing = false; }
    } executionGuard;
    Transaction& transaction = source.back();
    std::vector<LockedTree> trees(transaction.actions.size());
    std::vector<Handle> parentLocks;
    std::vector<std::wstring> from, to;
    from.reserve(trees.size());
    to.reserve(trees.size());
    HRESULT hr = S_OK;
    for (auto& action : transaction.actions) {
        const bool created = action.kind == ActionKind::Copy || action.kind == ActionKind::NewFolder;
        if (created) {
            hr = makeHolding(action);
            if (FAILED(hr)) return hr;
            hr = lockParent(fs::path(action.holding).parent_path(), action.holdingOwner, parentLocks);
            if (FAILED(hr)) return hr;
            hr = verifyHolding(action, redo);
            if (FAILED(hr)) return hr;
        } else {
            hr = lockParent(fs::path(action.before).parent_path(), action.beforeParent, parentLocks);
            if (FAILED(hr)) return hr;
        }
        hr = lockParent(fs::path(action.after).parent_path(), action.afterParent, parentLocks);
        if (FAILED(hr)) return hr;
        from.push_back(redo ? (created ? action.holding : action.before) : action.after);
        to.push_back(redo ? action.after : (created ? action.holding : action.before));
        const size_t index = from.size() - 1;
        hr = readTree(from.back(), L"", true, trees[index]);
        if (FAILED(hr)) return hr;
        if (!sameSnapshot(trees[index].snapshot, action.expected)) return HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
        hr = absentOrSame(to.back(), trees[index].snapshot.nodes.front().value);
        if (FAILED(hr)) return hr;
        // Exact handle renames cannot cross a volume. Such transactions stay
        // unadvertised until a verified copy-and-delete implementation is used.
        const Stamp& targetParent = redo ? action.afterParent : (created ? action.holdingOwner : action.beforeParent);
        if (targetParent.id.VolumeSerialNumber != trees[index].snapshot.nodes.front().value.id.VolumeSerialNumber)
            return HRESULT_FROM_WIN32(ERROR_NOT_SAME_DEVICE);
    }
    size_t completed = 0;
    for (; completed < trees.size(); ++completed) {
        // NTFS refuses directory relocation while descendant directory handles
        // are open, including handles that share DELETE. Keep the root protected
        // and release descendant handles immediately before the atomic rename.
        if (trees[completed].snapshot.nodes.front().value.basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            trees[completed].handles.resize(1);
        hr = renameHandle(trees[completed].handles.front().get(), to[completed]);
        if (FAILED(hr)) break;
    }
    const auto rollback = [&](HRESULT failure) {
        bool rollbackFailed = false;
        while (completed) {
            --completed;
            if (FAILED(renameHandle(trees[completed].handles.front().get(), from[completed]))) rollbackFailed = true;
            else {
                Stamp restored{};
                if (FAILED(stamp(trees[completed].handles.front().get(), restored))) rollbackFailed = true;
                else transaction.actions[completed].expected.nodes.front().value.basic.ChangeTime = restored.basic.ChangeTime;
            }
        }
        if (rollbackFailed) {
            transaction.poisoned = true;
            preserveHolding(transaction);
            return HRESULT_FROM_WIN32(ERROR_RECOVERY_FAILURE);
        }
        return failure;
    };
    if (FAILED(hr)) return rollback(hr);
    // Renaming changes the root ChangeTime; take the next state's metadata while
    // write-excluding handles still protect every object in the transaction.
    std::vector<Snapshot> next(trees.size());
    for (size_t i = 0; i < trees.size(); ++i) {
        hr = takeSnapshot(to[i], next[i]);
        if (FAILED(hr)) return rollback(hr);
        if (!sameAfterRename(transaction.actions[i].expected, next[i]))
            return rollback(HRESULT_FROM_WIN32(ERROR_FILE_INVALID));
    }
    for (size_t i = 0; i < trees.size(); ++i) {
        transaction.actions[i].expected = std::move(next[i]);
        SHChangeNotify(SHCNE_RENAMEITEM, SHCNF_PATHW, from[i].c_str(), to[i].c_str());
        SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW, fs::path(from[i]).parent_path().c_str(), nullptr);
        SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW, fs::path(to[i]).parent_path().c_str(), nullptr);
    }
    destination.push_back(std::move(transaction));
    source.pop_back();
    return S_OK;
}

// PerformOperations may report success despite failed/skipped individual items.
class OperationSink final : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
                                               IFileOperationProgressSink> {
public:
    explicit OperationSink(bool requireRecycle, RecordRequest* request = nullptr)
        : requireRecycle_(requireRecycle), request_(request) {}
    HRESULT result = S_OK;
    void record(HRESULT hr) { if (FAILED(hr) && SUCCEEDED(result)) result = hr; }
    IFACEMETHODIMP StartOperations() override { return S_OK; }
    IFACEMETHODIMP FinishOperations(HRESULT hr) override { record(hr); return S_OK; }
    IFACEMETHODIMP PreRenameItem(DWORD, IShellItem* source, LPCWSTR) override {
        if (request_) request_->prepare(source); return S_OK;
    }
    IFACEMETHODIMP PostRenameItem(DWORD, IShellItem* source, LPCWSTR, HRESULT hr,
                                 IShellItem* created) override { completed(source, created, hr); return S_OK; }
    IFACEMETHODIMP PreMoveItem(DWORD, IShellItem* source, IShellItem*, LPCWSTR) override {
        if (request_) request_->prepare(source); return S_OK;
    }
    IFACEMETHODIMP PostMoveItem(DWORD, IShellItem* source, IShellItem*, LPCWSTR, HRESULT hr,
                               IShellItem* created) override { completed(source, created, hr); return S_OK; }
    IFACEMETHODIMP PreCopyItem(DWORD, IShellItem* source, IShellItem*, LPCWSTR) override {
        if (request_) request_->prepare(source); return S_OK;
    }
    IFACEMETHODIMP PostCopyItem(DWORD, IShellItem* source, IShellItem*, LPCWSTR, HRESULT hr,
                               IShellItem* created) override { completed(source, created, hr); return S_OK; }
    IFACEMETHODIMP PreDeleteItem(DWORD flags, IShellItem*) override {
        // A silent recycle request must not turn into an unprompted permanent
        // delete when the Shell retries an item without recycling enabled.
        if (requireRecycle_ && !(flags & TSF_DELETE_RECYCLE_IF_POSSIBLE)) {
            const HRESULT hr = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            record(hr);
            return hr;
        }
        return S_OK;
    }
    IFACEMETHODIMP PostDeleteItem(DWORD, IShellItem* source, HRESULT hr,
                                 IShellItem* created) override { completed(source, created, hr); return S_OK; }
    IFACEMETHODIMP PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT hr,
                              IShellItem* created) override { completed(nullptr, created, hr); return S_OK; }
    IFACEMETHODIMP UpdateProgress(UINT, UINT) override { return S_OK; }
    IFACEMETHODIMP ResetTimer() override { return S_OK; }
    IFACEMETHODIMP PauseTimer() override { return S_OK; }
    IFACEMETHODIMP ResumeTimer() override { return S_OK; }
private:
    void completed(IShellItem* source, IShellItem* created, HRESULT hr) {
        record(hr);
        if (request_) {
            if (SUCCEEDED(hr) && created) request_->capture(source, created);
            else request_->valid = false;
        }
    }
    bool requireRecycle_;
    RecordRequest* request_;
};

template<class Queue>
HRESULT perform(HWND owner, bool silent, DWORD extraFlags, bool undoable, Queue&& queue,
                RecordRequest* request = nullptr, IFileOperationProgressSink* progress = nullptr) {
    ComPtr<IFileOperation> operation;
    HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&operation));
    if (FAILED(hr)) return hr;
    DWORD flags = FOF_NOCONFIRMMKDIR | extraFlags;
    if (undoable && !silent && !request) flags |= FOFX_ADDUNDORECORD;
    if (silent) {
        flags |= FOF_NO_UI | FOFX_EARLYFAILURE | FOFX_NOCOPYHOOKS;
    }
    hr = operation->SetOperationFlags(flags);
    if (FAILED(hr)) return hr;
    hr = operation->SetOwnerWindow(owner);
    if (FAILED(hr)) return hr;
    auto sink = Make<OperationSink>(silent && (extraFlags & FOFX_RECYCLEONDELETE) != 0, request);
    if (!sink) return E_OUTOFMEMORY;
    DWORD cookie = 0;
    hr = operation->Advise(sink.Get(), &cookie);
    if (FAILED(hr)) return hr;
    DWORD progressCookie = 0;
    if (progress) {
        hr = operation->Advise(progress, &progressCookie);
        if (FAILED(hr)) { operation->Unadvise(cookie); return hr; }
    }
    hr = queue(operation.Get());
    if (SUCCEEDED(hr)) hr = operation->PerformOperations();
    BOOL aborted = FALSE;
    const HRESULT abortedHr = operation->GetAnyOperationsAborted(&aborted);
    if (progress) operation->Unadvise(progressCookie);
    operation->Unadvise(cookie);
    if (FAILED(hr)) return hr;
    if (FAILED(sink->result)) return sink->result;
    if (FAILED(abortedHr)) return abortedHr;
    const HRESULT result = aborted ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : hr;
    finishRecord(request, result);
    return result;
}

HRESULT setGlobalData(IDataObject* object, CLIPFORMAT format, const void* data, SIZE_T bytes) {
    if (!format) return lastError();
    const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return E_OUTOFMEMORY;
    void* target = GlobalLock(memory);
    if (!target) { GlobalFree(memory); return lastError(); }
    std::memcpy(target, data, bytes);
    GlobalUnlock(memory);
    FORMATETC etc{format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    medium.tymed = TYMED_HGLOBAL;
    medium.hGlobal = memory;
    const HRESULT hr = object->SetData(&etc, &medium, TRUE);
    if (FAILED(hr)) ReleaseStgMedium(&medium);
    return hr;
}

HRESULT setEffect(IDataObject* object, const wchar_t* name, DWORD effect) {
    return setGlobalData(object, static_cast<CLIPFORMAT>(RegisterClipboardFormatW(name)),
                         &effect, sizeof(effect));
}

HRESULT preferredEffect(IDataObject* object, DWORD& effect) {
    const UINT format = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    if (!format) return lastError();
    FORMATETC etc{static_cast<CLIPFORMAT>(format), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    const HRESULT hr = object->GetData(&etc, &medium);
    if (FAILED(hr)) return hr;
    HRESULT result = DV_E_TYMED;
    if (medium.tymed == TYMED_HGLOBAL && GlobalSize(medium.hGlobal) >= sizeof(DWORD)) {
        const void* data = GlobalLock(medium.hGlobal);
        if (data) {
            std::memcpy(&effect, data, sizeof(effect));
            GlobalUnlock(medium.hGlobal);
            result = S_OK;
        } else result = lastError();
    }
    ReleaseStgMedium(&medium);
    return result;
}

// Exact names should not unexpectedly replace another file during silent tests.
HRESULT checkExistingName(IShellItem* folder, const std::wstring& name,
                          IShellItem* current = nullptr) {
    PWSTR raw = nullptr;
    HRESULT hr = folder->GetDisplayName(SIGDN_FILESYSPATH, &raw);
    TaskString folderName(raw);
    if (FAILED(hr)) return S_OK; // Virtual namespace providers perform their own checks.
    std::wstring path = folderName.get();
    if (path.back() != L'\\') path += L'\\';
    path += name;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
            ? S_OK : HRESULT_FROM_WIN32(error);
    }
    if (current) {
        ComPtr<IShellItem> existing;
        hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&existing));
        int order = 1;
        if (SUCCEEDED(hr) && SUCCEEDED(current->Compare(existing.Get(),
                SICHINT_CANONICAL | SICHINT_TEST_FILESYSPATH_IF_NOT_EQUAL, &order))
            && order == 0) return S_OK; // Case-only rename of the same object is allowed.
    }
    return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
}

class TextDataObject final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IDataObject> {
public:
    explicit TextDataObject(std::wstring text) : text_(std::move(text)) {}
    IFACEMETHODIMP GetData(FORMATETC* format, STGMEDIUM* medium) override {
        if (!medium) return E_POINTER;
        *medium = {};
        const HRESULT hr = QueryGetData(format);
        if (FAILED(hr)) return hr;
        const SIZE_T bytes = (text_.size() + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (!memory) return E_OUTOFMEMORY;
        void* target = GlobalLock(memory);
        if (!target) { GlobalFree(memory); return lastError(); }
        std::memcpy(target, text_.c_str(), bytes);
        GlobalUnlock(memory);
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = memory;
        return S_OK;
    }
    IFACEMETHODIMP GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    IFACEMETHODIMP QueryGetData(FORMATETC* format) override {
        if (!format) return E_POINTER;
        if (format->cfFormat != CF_UNICODETEXT) return DV_E_FORMATETC;
        if (!(format->tymed & TYMED_HGLOBAL)) return DV_E_TYMED;
        if (format->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;
        return format->lindex == -1 ? S_OK : DV_E_LINDEX;
    }
    IFACEMETHODIMP GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
        if (!out) return E_POINTER;
        out->ptd = nullptr;
        return DATA_S_SAMEFORMATETC;
    }
    IFACEMETHODIMP SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    IFACEMETHODIMP EnumFormatEtc(DWORD direction, IEnumFORMATETC** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (direction != DATADIR_GET) return E_NOTIMPL;
        FORMATETC format{CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        return SHCreateStdEnumFmtEtc(1, &format, output);
    }
    IFACEMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    IFACEMETHODIMP DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    IFACEMETHODIMP EnumDAdvise(IEnumSTATDATA** output) override {
        if (output) *output = nullptr;
        return OLE_E_ADVISENOTSUPPORTED;
    }
private:
    std::wstring text_;
};
} // namespace

bool isShellOperationCancelled(HRESULT result) noexcept {
    return result == HRESULT_FROM_WIN32(ERROR_CANCELLED) || result == E_ABORT ||
        result == COPYENGINE_E_USER_CANCELLED;
}

HRESULT ShellOperations::invoke(HWND owner, IShellItemArray* selection, const wchar_t* verb, IUnknown* site) {
    HRESULT hr = checkSelection(selection);
    if (FAILED(hr) || !verb || !*verb) return FAILED(hr) ? hr : E_INVALIDARG;
    NativeContextMenu menu;
    hr = menu.createSelection(owner, selection, site, CMF_NORMAL | CMF_EXTENDEDVERBS);
    if (FAILED(hr)) return hr;
    std::vector<ContextMenuEntry> entries;
    hr = menu.enumerate(entries);
    if (FAILED(hr)) return hr;
    UINT selected = 0;
    unsigned matches = 0;
    bool enabled = false;
    const auto find = [&](const auto& self, const std::vector<ContextMenuEntry>& items) -> void {
        for (const auto& entry : items) {
            if (entry.submenu) self(self, entry.children);
            else if (!entry.separator() && CompareStringOrdinal(entry.canonicalVerb.c_str(), -1,
                         verb, -1, TRUE) == CSTR_EQUAL) {
                ++matches;
                selected = entry.id;
                enabled = entry.enabled();
            }
        }
    };
    find(find, entries);
    if (!matches) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    if (matches != 1) return HRESULT_FROM_WIN32(ERROR_DUP_NAME);
    return enabled ? menu.invoke(selected) : E_ACCESSDENIED;
}

HRESULT ShellOperations::copyToClipboard(HWND, IShellItemArray* selection, bool cut) {
    HRESULT hr = checkSelection(selection);
    if (FAILED(hr)) return hr;
    ComPtr<IDataObject> data;
    hr = selection->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data));
    if (FAILED(hr)) return hr;
    hr = setEffect(data.Get(), CFSTR_PREFERREDDROPEFFECT,
                   cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY);
    return FAILED(hr) ? hr : publishClipboard(data.Get());
}

void ShellOperations::flushClipboardIfOwned() {
    if (ownedClipboard && OleIsCurrentClipboard(ownedClipboard.Get()) == S_OK) {
        OleFlushClipboard();
    }
    ownedClipboard.Reset();
}

HRESULT ShellOperations::paste(HWND owner, IShellItem* destination) {
    if (!destination) return E_INVALIDARG;
    ComPtr<IDataObject> data;
    HRESULT hr = OleGetClipboard(&data);
    if (FAILED(hr)) return hr;
    ComPtr<IShellItemArray> selection;
    hr = SHCreateShellItemArrayFromDataObject(data.Get(), IID_PPV_ARGS(&selection));
    if (FAILED(hr)) return hr;
    DWORD effect = DROPEFFECT_COPY;
    preferredEffect(data.Get(), effect); // Absent preference means Copy.
    const bool move = effect == DROPEFFECT_MOVE;
    hr = copyOrMove(owner, selection.Get(), destination, move);
    if (FAILED(hr)) return hr;
    // This is an optimized move: IFileOperation already removed the original.
    // Reporting MOVE as performed would tell the source to delete it again.
    setEffect(data.Get(), CFSTR_PERFORMEDDROPEFFECT, move ? DROPEFFECT_NONE : DROPEFFECT_COPY);
    setEffect(data.Get(), CFSTR_PASTESUCCEEDED, move ? DROPEFFECT_MOVE : DROPEFFECT_COPY);
    return hr;
}

HRESULT ShellOperations::copyOrMove(HWND owner, IShellItemArray* selection,
                                    IShellItem* destination, bool move, bool silent, bool recordUndo,
                                    IFileOperationProgressSink* progress) {
    if (recordUndo && !silent) return E_INVALIDARG;
    const HRESULT hr = checkSelection(selection);
    if (FAILED(hr) || !destination) return FAILED(hr) ? hr : E_INVALIDARG;
    const DWORD flags = silent ? FOF_RENAMEONCOLLISION | FOFX_PRESERVEFILEEXTENSIONS : 0;
    RecordRequest record;
    RecordRequest* request = recordUndo ? &record : nullptr;
    if (request) {
        record.kind = move ? ActionKind::Move : ActionKind::Copy;
        prepareSources(record, selection);
        prepareDestination(record, destination);
    }
    return perform(owner, silent, flags, true, [&](IFileOperation* operation) {
        return move ? operation->MoveItems(selection, destination)
                    : operation->CopyItems(selection, destination);
    }, request, progress);
}

HRESULT ShellOperations::remove(HWND owner, IShellItemArray* selection,
                                bool permanent, bool silent, IFileOperationProgressSink* progress) {
    const HRESULT hr = checkSelection(selection);
    if (FAILED(hr)) return hr;
    DWORD flags = permanent ? 0 : FOFX_RECYCLEONDELETE;
    if (!permanent && !silent) flags |= FOF_WANTNUKEWARNING;
    // Production recycle undo belongs to Windows.undo/redo and its native Shell
    // record. The isolated journal never reaches into the user's Recycle Bin.
    return perform(owner, silent, flags, !permanent, [&](IFileOperation* operation) {
        return operation->DeleteItems(selection);
    }, nullptr, progress);
}

HRESULT ShellOperations::rename(HWND owner, IShellItem* item, const std::wstring& name,
                                bool silent, bool recordUndo, IFileOperationProgressSink* progress) {
    if (recordUndo && !silent) return E_INVALIDARG;
    if (!item || !validLeafName(name)) return E_INVALIDARG;
    if (silent) {
        ComPtr<IShellItem> parent;
        HRESULT hr = item->GetParent(&parent);
        if (FAILED(hr)) return hr;
        hr = checkExistingName(parent.Get(), name, item);
        if (FAILED(hr)) return hr;
    }
    const DWORD flags = silent ? FOF_RENAMEONCOLLISION | FOFX_PRESERVEFILEEXTENSIONS : 0;
    RecordRequest record;
    RecordRequest* request = recordUndo ? &record : nullptr;
    if (request) {
        record.kind = ActionKind::Rename;
        ComPtr<IShellItemArray> selected;
        ComPtr<IShellItem> parent;
        if (FAILED(SHCreateShellItemArrayFromShellItem(item, IID_PPV_ARGS(&selected))) ||
            FAILED(item->GetParent(&parent))) record.valid = false;
        else { prepareSources(record, selected.Get()); prepareDestination(record, parent.Get()); }
    }
    return perform(owner, silent, flags, true, [&](IFileOperation* operation) {
        return operation->RenameItem(item, name.c_str(), nullptr);
    }, request, progress);
}

HRESULT ShellOperations::newFolder(HWND owner, IShellItem* destination,
                                   const std::wstring& name, bool silent, bool recordUndo,
                                   IFileOperationProgressSink* progress) {
    if (recordUndo && !silent) return E_INVALIDARG;
    if (!destination || !validLeafName(name)) return E_INVALIDARG;
    if (silent) {
        const HRESULT hr = checkExistingName(destination, name);
        if (FAILED(hr)) return hr;
    }
    RecordRequest record;
    RecordRequest* request = recordUndo ? &record : nullptr;
    if (request) { record.kind = ActionKind::NewFolder; prepareDestination(record, destination); }
    return perform(owner, silent, 0, true, [&](IFileOperation* operation) {
        return operation->NewItem(destination, FILE_ATTRIBUTE_DIRECTORY, name.c_str(), nullptr, nullptr);
    }, request, progress);
}

HRESULT ShellOperations::undo(HWND, bool) { return executeJournal(false); }
HRESULT ShellOperations::redo(HWND, bool) { return executeJournal(true); }
bool ShellOperations::canUndo() { return !journal.undo.empty() && !journal.undo.back().poisoned && !journal.executing; }
bool ShellOperations::canRedo() { return !journal.redo.empty() && !journal.redo.back().poisoned && !journal.executing; }
void ShellOperations::clearHistory() {
    for (auto& transaction : journal.undo) preserveHolding(transaction);
    for (auto& transaction : journal.redo) preserveHolding(transaction);
    journal.undo.clear();
    journal.redo.clear();
}
std::vector<std::wstring> ShellOperations::recoveryPaths() { return journal.preserved; }

HRESULT ShellOperations::copyPaths(HWND, IShellItemArray* selection) {
    std::wstring text;
    const auto hr = ItemActions::quotedPaths(selection, text);
    if (FAILED(hr)) return hr;
    auto data = Make<TextDataObject>(std::move(text));
    return data ? publishClipboard(data.Get()) : E_OUTOFMEMORY;
}
} // namespace explorer
