# Architecture

`WindowsExplorer.exe` is a Unicode, x64, C++20 Win32 process. The manifest declares per-monitor DPI awareness, long-path awareness, common-controls version 6, and `asInvoker` privileges. The MSVC runtime is linked statically. Windows supplies the Shell, common controls, property system, COM, and archive utility.

## Native host

`ExplorerApp` owns the window, native command controls, address/search edits, breadcrumbs, status bar, and `IExplorerBrowser`. It implements service discovery, browser events, pane visibility, common-dialog browser callbacks, and folder filtering. Browser initialization failures and window destruction detach the event subscription, filter, and site before releasing COM references.

The Shell owns folder enumeration, presentation, native menus, thumbnails, and namespace providers. This allows the application to work with virtual locations and existing extensions without implementing a second filesystem-only view. Behavior varies by installed Windows components; Shell reuse does not establish complete Explorer.exe feature parity. Microsoft's [ExplorerBrowser documentation](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-iexplorerbrowser) describes this hosting model.

Navigation history stores absolute PIDLs, not filesystem path strings. History commits only when the completed PIDL matches the requested destination. Direct navigation discards a pending history request, and new navigation truncates the forward branch. Pane changes rebuild the native browser while retaining the host's history.

`IFolderView2` controls the eight layouts, sort/group properties, selection, and checkboxes. The Shell controls details columns and native property display. Preview and Details are mutually exclusive. Hidden files use the app's filter; hidden system files remain excluded. File extensions use the shared Windows Shell setting only after an explicit user command.

## Operations

`ShellOperations` uses [IFileOperation](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-ifileoperation) on the UI's STA for native file-operation semantics. It checks per-item results and aborted status. Normal user operations retain native progress/conflict dialogs; silent fixture operations disable UI and do not add undo records. Delete requests recycling; permanent deletion is a separate confirmed command. No public undo-stack execution interface is assumed.

Copy/cut publish native `IDataObject` objects with preferred drop effects. Paste interprets the clipboard through the Shell, then reports optimized-move feedback. Shutdown flushes only clipboard data owned by the application before its COM apartment ends.

`ExtraOperations` uses `IShellLinkW`/`IPersistFile` for `.lnk` files. ZIP work runs on a background task. The native wide-path code stages inputs because Windows 10's bundled tar has limitations with Unicode archive and directory arguments. Tar receives ASCII relative paths and `CREATE_NO_WINDOW`; no command shell is involved. Validation rejects unsafe or unsupported ZIP structures, and final output is published without replacing an existing destination. Staging consumes additional disk space; closing during archive work can wait for the worker to finish.

## Search and responsiveness

Search parses AQS with `IQueryParserManager`, resolves dates and virtual properties, and creates an `ISearchFolderItemFactory` result scoped to the current location. It navigates the native result object directly, following Microsoft's [ExplorerBrowserSearch sample](https://learn.microsoft.com/en-us/windows/win32/shell/samples-explorerbrowsersearch). Search locations are tracked by PIDL so history and pane reconstruction retain the background-search policy.

The host returns `CDB2GVF_NOINCLUDEITEM` for search views to avoid the documented compatibility path that filters searches on the UI thread. Ordinary folders use a lightweight attribute filter for app-local hidden-item state. Startup defaults to This PC. The status bar reports view counts and navigation timing; it does not repeatedly invoke selected-item property handlers or recursively total folder sizes.

This design removes the need for an additional web runtime and avoids some unnecessary work. Shell extensions, network providers, thumbnail handlers, synchronous namespace parsing, and Shell-owned operations can still block. A 1,000-file fixture verifies basic behavior and records timing; it is not a comparison with Explorer.exe. Provider isolation, cancellable archive jobs, metadata workers, and broader performance measurements remain future work.

## Tests

The console suite checks core/settings logic, shortcut dispatch, and silent operations on owned fixtures. The application has a separate hidden-window smoke mode that initializes the real native controls and Shell view. It pumps messages, reads back view/sort/group/selection state, and observes visible process-owned top-level windows. It does not display the app, invoke interactive commands, take visible screenshots, or automate the desktop.
