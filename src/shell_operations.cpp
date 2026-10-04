#include "explorer/shell_operations.hpp"
#include "explorer/item_actions.hpp"
#include "explorer/core.hpp"

#include <shlobj.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <cstring>
#include <memory>
#include <utility>

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
HRESULT perform(HWND owner, bool silent, DWORD extraFlags, bool undoable, Queue&& queue) {
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
    hr = queue(operation.Get());
    if (SUCCEEDED(hr)) hr = operation->PerformOperations();
    BOOL aborted = FALSE;
    const HRESULT abortedHr = operation->GetAnyOperationsAborted(&aborted);
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
        if (SUCCEEDED(hr) && SUCCEEDED(current->Compare(existing.Get(), SICHINT_CANONICAL, &order))
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

HRESULT ShellOperations::invoke(HWND owner, IShellItemArray* selection, const wchar_t* verb) {
    HRESULT hr = checkSelection(selection);
    if (FAILED(hr) || !verb || !*verb) return FAILED(hr) ? hr : E_INVALIDARG;
    ComPtr<IContextMenu> context;
    hr = selection->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&context));
    if (FAILED(hr)) return hr;
    const HMENU menu = CreatePopupMenu();
    if (!menu) return lastError();
    hr = context->QueryContextMenu(menu, 0, 1, 0x7fff, CMF_NORMAL | CMF_EXTENDEDVERBS);
    if (SUCCEEDED(hr)) {
        CMINVOKECOMMANDINFOEX command{};
        command.cbSize = sizeof(command);
        command.fMask = CMIC_MASK_UNICODE;
        command.hwnd = owner;
        command.lpVerbW = verb;
        char ansiVerb[256]{};
        if (!WideCharToMultiByte(CP_ACP, 0, verb, -1, ansiVerb,
                                 static_cast<int>(sizeof(ansiVerb)), nullptr, nullptr)) {
            hr = lastError();
        } else {
            command.lpVerb = ansiVerb;
            command.nShow = SW_SHOWNORMAL;
            hr = context->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&command));
        }
    }
    DestroyMenu(menu);
    return hr;
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
                                    IShellItem* destination, bool move, bool silent) {
    const HRESULT hr = checkSelection(selection);
    if (FAILED(hr) || !destination) return FAILED(hr) ? hr : E_INVALIDARG;
    const DWORD flags = silent ? FOF_RENAMEONCOLLISION | FOFX_PRESERVEFILEEXTENSIONS : 0;
    return perform(owner, silent, flags, true, [&](IFileOperation* operation) {
        return move ? operation->MoveItems(selection, destination)
                    : operation->CopyItems(selection, destination);
    });
}

HRESULT ShellOperations::remove(HWND owner, IShellItemArray* selection,
                                bool permanent, bool silent) {
    const HRESULT hr = checkSelection(selection);
    if (FAILED(hr)) return hr;
    DWORD flags = permanent ? 0 : FOFX_RECYCLEONDELETE;
    if (!permanent && !silent) flags |= FOF_WANTNUKEWARNING;
    return perform(owner, silent, flags, !permanent, [&](IFileOperation* operation) {
        return operation->DeleteItems(selection);
    });
}

HRESULT ShellOperations::rename(HWND owner, IShellItem* item, const std::wstring& name,
                                bool silent) {
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
    });
}

HRESULT ShellOperations::newFolder(HWND owner, IShellItem* destination,
                                   const std::wstring& name, bool silent) {
    if (!destination || !validLeafName(name)) return E_INVALIDARG;
    if (silent) {
        const HRESULT hr = checkExistingName(destination, name);
        if (FAILED(hr)) return hr;
    }
    return perform(owner, silent, 0, true, [&](IFileOperation* operation) {
        return operation->NewItem(destination, FILE_ATTRIBUTE_DIRECTORY, name.c_str(), nullptr, nullptr);
    });
}

HRESULT ShellOperations::undo(HWND) { return E_NOTIMPL; }
HRESULT ShellOperations::redo(HWND) { return E_NOTIMPL; }
bool ShellOperations::canUndo() { return false; }
bool ShellOperations::canRedo() { return false; }

HRESULT ShellOperations::copyPaths(HWND, IShellItemArray* selection) {
    std::wstring text;
    const auto hr = ItemActions::quotedPaths(selection, text);
    if (FAILED(hr)) return hr;
    auto data = Make<TextDataObject>(std::move(text));
    return data ? publishClipboard(data.Get()) : E_OUTOFMEMORY;
}
} // namespace explorer
