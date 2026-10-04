#pragma once

#include <windows.h>
#include <shobjidl.h>
#include <string>
#include <vector>

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
    static HRESULT copyToClipboard(HWND owner, IShellItemArray* selection, bool cut);
    // Call on the publishing STA before OleUninitialize. Never flush another
    // application's clipboard, and release retained COM data before STA exit.
    static void flushClipboardIfOwned();
    static HRESULT paste(HWND owner, IShellItem* destination);
    // Silent operations suppress Shell UI, preserve collisions under new names,
    // and do not add fixture operations to the user's Shell undo history.
    // An optional public progress sink is advised only for this call; its native
    // callbacks can cancel the job and expose actual post-collision targets.
    // The native progress dialog remains available when silent is false.
    static HRESULT copyOrMove(HWND owner, IShellItemArray* selection,
                              IShellItem* destination, bool move, bool silent = false,
                              bool recordUndo = false,
                              IFileOperationProgressSink* progress = nullptr);
    static HRESULT remove(HWND owner, IShellItemArray* selection,
                          bool permanent = false, bool silent = false,
                          IFileOperationProgressSink* progress = nullptr);
    static HRESULT rename(HWND owner, IShellItem* item, const std::wstring& name,
                          bool silent = false, bool recordUndo = false,
                          IFileOperationProgressSink* progress = nullptr);
    static HRESULT newFolder(HWND owner, IShellItem* destination, const std::wstring& name,
                             bool silent = false, bool recordUndo = false,
                             IFileOperationProgressSink* progress = nullptr);
    // The isolated journal belongs to the calling STA. Production Undo/Redo use
    // the registered Windows.undo/redo handlers and their native Shell records.
    // Silent fixture operations enter this journal only with recordUndo=true.
    // Inverses refuse replacements, stale identity/metadata, active writers and
    // foreign holding-folder contents. Directory relocation releases descendant
    // locks and verifies its resulting metadata; races preserve recovery data.
    static HRESULT undo(HWND owner, bool silent = false);
    static HRESULT redo(HWND owner, bool silent = false);
    static bool canUndo();
    static bool canRedo();
    // Normal operations always keep native Shell history. recordUndo is accepted
    // only with silent=true so fixture and production histories cannot mix.
    // Removes only empty, identity-checked holding folders and releases history.
    // Retained objects are preserved; recoveryPaths identifies their directories.
    static void clearHistory();
    static std::vector<std::wstring> recoveryPaths();
    static HRESULT copyPaths(HWND owner, IShellItemArray* selection);
};

} // namespace explorer
