#pragma once

#include <windows.h>
#include <shobjidl.h>
#include <string>

namespace explorer {

// Call from an STA initialized with OleInitialize. Shell extensions and file
// operations retain their native Windows behavior; no explorer.exe is launched.
class ShellOperations final {
public:
    static HRESULT invoke(HWND owner, IShellItemArray* selection, const wchar_t* verb);
    static HRESULT copyToClipboard(HWND owner, IShellItemArray* selection, bool cut);
    // Call on the publishing STA before OleUninitialize. Never flush another
    // application's clipboard, and release retained COM data before STA exit.
    static void flushClipboardIfOwned();
    static HRESULT paste(HWND owner, IShellItem* destination);
    // Silent operations suppress Shell UI, preserve collisions under new names,
    // and do not add fixture operations to the user's Shell undo history.
    static HRESULT copyOrMove(HWND owner, IShellItemArray* selection,
                              IShellItem* destination, bool move, bool silent = false);
    static HRESULT remove(HWND owner, IShellItemArray* selection,
                          bool permanent = false, bool silent = false);
    static HRESULT rename(HWND owner, IShellItem* item, const std::wstring& name,
                          bool silent = false);
    static HRESULT newFolder(HWND owner, IShellItem* destination, const std::wstring& name,
                             bool silent = false);
    // IFileOperation records native undo, but exposes no public undo-stack API.
    static HRESULT undo(HWND owner);
    static HRESULT redo(HWND owner);
    static bool canUndo();
    static bool canRedo();
    static HRESULT copyPaths(HWND owner, IShellItemArray* selection);
};

} // namespace explorer
