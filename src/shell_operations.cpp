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
#include <cstddef>
#include <limits>
#include <new>
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
thread_local unsigned long long clipboardPublication = 0;

HRESULT publishClipboard(IDataObject* data) {
    // Acquire the incoming reference before any native clipboard boundary.
    // AddRef, OleSetClipboard and the ownership query can each reenter a host.
    ComPtr<IDataObject> candidate = data;
    const HRESULT hr = OleSetClipboard(candidate.Get());
    if (SUCCEEDED(hr)) {
        ++clipboardPublication;
        const auto publication = clipboardPublication;
        const auto previous = ownedClipboard.Get();
        const auto current = OleIsCurrentClipboard(candidate.Get());
        if (current == S_OK && publication == clipboardPublication && previous == ownedClipboard.Get()) {
            // Swap performs no incoming AddRef after the native-state fence.
            // Releasing the previous producer at exit can publish again; no
            // later assignment will overwrite that newer retained producer.
            ownedClipboard.Swap(candidate);
        }
    }
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

// PerformOperations may report success despite failed/skipped individual items.
class OperationSink final : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
                                               IFileOperationProgressSink> {
public:
    explicit OperationSink(bool requireRecycle) : requireRecycle_(requireRecycle) {}
    HRESULT result = S_OK;
    void record(HRESULT hr) { if (FAILED(hr) && SUCCEEDED(result)) result = hr; }
    IFACEMETHODIMP StartOperations() override { return S_OK; }
    IFACEMETHODIMP FinishOperations(HRESULT hr) override { record(hr); return S_OK; }
    IFACEMETHODIMP PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT hr,
                                 IShellItem*) override { record(hr); return S_OK; }
    IFACEMETHODIMP PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT hr,
                               IShellItem*) override { record(hr); return S_OK; }
    IFACEMETHODIMP PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT hr,
                               IShellItem*) override { record(hr); return S_OK; }
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
    IFACEMETHODIMP PostDeleteItem(DWORD, IShellItem*, HRESULT hr,
                                 IShellItem*) override { record(hr); return S_OK; }
    IFACEMETHODIMP PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT hr,
                              IShellItem*) override { record(hr); return S_OK; }
    IFACEMETHODIMP UpdateProgress(UINT, UINT) override { return S_OK; }
    IFACEMETHODIMP ResetTimer() override { return S_OK; }
    IFACEMETHODIMP PauseTimer() override { return S_OK; }
    IFACEMETHODIMP ResumeTimer() override { return S_OK; }
private:
    bool requireRecycle_;
};

template<class Queue>
HRESULT perform(HWND owner, bool silent, DWORD extraFlags, bool undoable, Queue&& queue,
                IFileOperationProgressSink* progress = nullptr) {
    ComPtr<IFileOperation> operation;
    HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&operation));
    if (FAILED(hr)) return hr;
    DWORD flags = FOF_NOCONFIRMMKDIR | extraFlags;
    if (undoable && !silent) flags |= FOFX_ADDUNDORECORD;
    if (silent) {
        flags |= FOF_NO_UI | FOFX_EARLYFAILURE | FOFX_NOCOPYHOOKS;
    }
    hr = operation->SetOperationFlags(flags);
    if (FAILED(hr)) return hr;
    hr = operation->SetOwnerWindow(owner);
    if (FAILED(hr)) return hr;
    auto sink = Make<OperationSink>(silent && (extraFlags & FOFX_RECYCLEONDELETE) != 0);
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
    return aborted ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : hr;
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

HRESULT ShellOperations::copyToClipboard(HWND, IShellItemArray* selection, bool cut, IDataObject** published) {
    HRESULT hr = checkSelection(selection);
    if (FAILED(hr)) return hr;
    ComPtr<IDataObject> data;
    hr = selection->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data));
    if (FAILED(hr)) return hr;
    hr = setEffect(data.Get(), CFSTR_PREFERREDDROPEFFECT,
                   cut ? DROPEFFECT_MOVE : (DROPEFFECT_COPY | DROPEFFECT_LINK));
    if (SUCCEEDED(hr)) hr = publishClipboard(data.Get());
    if (SUCCEEDED(hr) && published) *published = data.Detach();
    return hr;
}

HRESULT ShellOperations::clearClipboardIfOwned(IDataObject* published, HWND owner) {
    if (!published) return E_INVALIDARG;
    // The producer can be compared only on the STA which retained it. A
    // consumer wrapper or a producer belonging to another STA cannot clear.
    if (!ownedClipboard || ownedClipboard.Get() != published) return S_FALSE;
    DWORD process = 0;
    if (!owner || GetWindowThreadProcessId(owner, &process) != GetCurrentThreadId() ||
        process != GetCurrentProcessId()) return E_ACCESSDENIED;
    const auto publication = clipboardPublication;
    const auto retained = ownedClipboard;
    if (!OpenClipboard(owner)) return lastError();
    const auto current = OleIsCurrentClipboard(retained.Get());
    HRESULT result = current;
    const bool samePublication = ownedClipboard.Get() == retained.Get() && clipboardPublication == publication;
    if (!samePublication) result = S_FALSE;
    else if (current == S_OK && !EmptyClipboard()) result = lastError();
    const BOOL closed = CloseClipboard();
    const DWORD closeError = closed ? ERROR_SUCCESS : GetLastError();
    // EmptyClipboard can synchronously notify an OLE owner and dispatch host
    // callbacks. Do not release a later producer published by such a callback.
    // A stale producer can be released without altering another owner's data.
    // This also releases retained cut data before its publishing STA exits.
    if (samePublication && (current == S_FALSE || (current == S_OK && SUCCEEDED(result))) &&
        ownedClipboard.Get() == retained.Get() && clipboardPublication == publication) ownedClipboard.Reset();
    if (!closed && SUCCEEDED(result)) result = HRESULT_FROM_WIN32(closeError ? closeError : ERROR_GEN_FAILURE);
    return result;
}

void ShellOperations::flushClipboardIfOwned() {
    const auto publication = clipboardPublication;
    const auto retained = ownedClipboard;
    if (retained && OleIsCurrentClipboard(retained.Get()) == S_OK &&
        ownedClipboard.Get() == retained.Get() && clipboardPublication == publication) {
        OleFlushClipboard();
    }
    // Rendering/Release callbacks must not discard a newer publication while
    // normal application shutdown is flushing the producer captured here.
    if (ownedClipboard.Get() == retained.Get() && clipboardPublication == publication) ownedClipboard.Reset();
}

HRESULT ShellOperations::copyOrMove(HWND owner, IShellItemArray* selection,
                                    IShellItem* destination, bool move, bool silent,
                                    IFileOperationProgressSink* progress) {
    const HRESULT hr = checkSelection(selection);
    if (FAILED(hr) || !destination) return FAILED(hr) ? hr : E_INVALIDARG;
    const DWORD flags = silent ? FOF_RENAMEONCOLLISION | FOFX_PRESERVEFILEEXTENSIONS : 0;
    return perform(owner, silent, flags, true, [&](IFileOperation* operation) {
        return move ? operation->MoveItems(selection, destination)
                    : operation->CopyItems(selection, destination);
    }, progress);
}

HRESULT ShellOperations::remove(HWND owner, IShellItemArray* selection,
                                bool permanent, bool silent, IFileOperationProgressSink* progress) {
    const HRESULT hr = checkSelection(selection);
    if (FAILED(hr)) return hr;
    DWORD flags = permanent ? 0 : FOFX_RECYCLEONDELETE;
    if (!permanent && !silent) flags |= FOF_WANTNUKEWARNING;
    // Production recycle undo belongs to Windows.undo/redo and its native Shell record.
    return perform(owner, silent, flags, !permanent, [&](IFileOperation* operation) {
        return operation->DeleteItems(selection);
    }, progress);
}

HRESULT ShellOperations::rename(HWND owner, IShellItem* item, const std::wstring& name,
                                bool silent, IFileOperationProgressSink* progress) {
    if (!item || !validLeafName(name)) return E_INVALIDARG;
    if (silent) {
        ComPtr<IShellItem> parent;
        HRESULT hr = item->GetParent(&parent);
        if (FAILED(hr)) return hr;
        hr = checkExistingName(parent.Get(), name, item);
        if (FAILED(hr)) return hr;
    }
    const DWORD flags = silent ? FOF_RENAMEONCOLLISION | FOFX_PRESERVEFILEEXTENSIONS : 0;
    return perform(owner, silent, flags, true, [&](IFileOperation* operation) {
        return operation->RenameItem(item, name.c_str(), nullptr);
    }, progress);
}

HRESULT ShellOperations::newFolder(HWND owner, IShellItem* destination,
                                   const std::wstring& name, bool silent,
                                   IFileOperationProgressSink* progress) {
    if (!destination || !validLeafName(name)) return E_INVALIDARG;
    if (silent) {
        const HRESULT hr = checkExistingName(destination, name);
        if (FAILED(hr)) return hr;
    }
    return perform(owner, silent, 0, true, [&](IFileOperation* operation) {
        return operation->NewItem(destination, FILE_ATTRIBUTE_DIRECTORY, name.c_str(), nullptr, nullptr);
    }, progress);
}

HRESULT ShellOperations::copyPaths(HWND owner, IShellItemArray* selection) {
    std::wstring text;
    const auto hr = ItemActions::quotedPaths(selection, text);
    if (FAILED(hr)) return hr;
    return copyText(owner, text);
}
HRESULT ShellOperations::copyText(HWND, const std::wstring& text) {
    if (text.find(L'\0') != std::wstring::npos ||
        text.size() >= (std::numeric_limits<SIZE_T>::max)() / sizeof(wchar_t)) return E_INVALIDARG;
    try {
        auto data = Make<TextDataObject>(text);
        return data ? publishClipboard(data.Get()) : E_OUTOFMEMORY;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}
} // namespace explorer
