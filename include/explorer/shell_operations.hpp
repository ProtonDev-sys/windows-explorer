#pragma once

#include <windows.h>
#include <shobjidl.h>
#include <string>

namespace explorer {

// User cancellation is not an operation error. Engine cancellation can carry
// a genuine failure, so COPYENGINE_E_CANCELLED is deliberately not included.
bool isShellOperationCancelled(HRESULT result) noexcept;

// Call from an STA initialized with OleInitialize. Shell extensions and file
// operations retain their native Windows behavior; no explorer.exe is launched.
class ShellOperations final {
public:
    // Resolve a unique enabled canonical leaf in the retained native menu, then
    // invoke its numeric ordinal. The actual Shell view site preserves provider
    // navigation/default-association services; it is detached after the call.
    static HRESULT invoke(HWND owner, IShellItemArray* selection, const wchar_t* verb,
                          IUnknown* site = nullptr);
    // Optional output is the exact producer supplied to OleSetClipboard, not
    // OleGetClipboard's consumer wrapper; unchanged when publication fails.
    static HRESULT copyToClipboard(HWND owner, IShellItemArray* selection, bool cut,
                                   IDataObject** published = nullptr);
    // Explicit producer-owned cleanup only, on its publishing STA. The exact
    // retained producer and current OLE ownership are checked while the Win32
    // clipboard is locked; unrelated/replaced clipboard data is never emptied.
    // S_FALSE means this producer no longer owns the clipboard.
    static HRESULT clearClipboardIfOwned(IDataObject* published, HWND owner);
    // Call on the publishing STA before OleUninitialize. Never flush another
    // application's clipboard, and release retained COM data before STA exit.
    static void flushClipboardIfOwned();
    // Silent operations suppress Shell UI, preserve collisions under new names,
    // and do not add fixture operations to the user's Shell undo history.
    // An optional public progress sink is advised only for this call; its native
    // callbacks can cancel the job and expose actual post-collision targets.
    // The native progress dialog remains available when silent is false.
    static HRESULT copyOrMove(HWND owner, IShellItemArray* selection,
                              IShellItem* destination, bool move, bool silent = false,
                              IFileOperationProgressSink* progress = nullptr);
    static HRESULT remove(HWND owner, IShellItemArray* selection,
                          bool permanent = false, bool silent = false,
                          IFileOperationProgressSink* progress = nullptr);
    static HRESULT rename(HWND owner, IShellItem* item, const std::wstring& name,
                          bool silent = false,
                          IFileOperationProgressSink* progress = nullptr);
    static HRESULT newFolder(HWND owner, IShellItem* destination, const std::wstring& name,
                             bool silent = false,
                             IFileOperationProgressSink* progress = nullptr);
    static HRESULT copyPaths(HWND owner, IShellItemArray* selection);
    // Explicit normal interaction only; callers must reject headless use.
    static HRESULT copyText(HWND owner, const std::wstring& text);
};

} // namespace explorer
