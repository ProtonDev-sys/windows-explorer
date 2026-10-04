# windows-explorer

A native C++20 file manager for Windows 10, built with Win32 controls and the Windows Shell. It runs in its own process and hosts native folder views through `IExplorerBrowser`. No Electron, WebView, or JavaScript runtime is involved.

The implementation covers navigation, file operations, Home/Share/View commands, and a contextual Search page. **It is a working application, not complete Explorer parity.** The [feature matrix](docs/feature-matrix.md) separates implemented commands, Shell-provided behavior, and remaining work.

## Run

Download the x64 executable from the repository's Releases page, extract the ZIP, and run `WindowsExplorer.exe`. Windows 10 22H2 or newer is the target; the build includes the C++ runtime. ZIP creation and extraction use Windows' bundled `tar.exe` through a hidden background process.

```powershell
./WindowsExplorer.exe
./WindowsExplorer.exe --path 'C:\Users'
```

The application starts at This PC to avoid eagerly loading recent files or cloud providers. It does not replace the desktop, taskbar, or the Windows shortcut association. Settings are stored in `%LOCALAPPDATA%\WindowsExplorer\settings.ini`. The file-name-extension toggle uses Windows' shared Shell setting, matching Explorer's system-wide behavior.

## Features

- Native file views with thumbnails, context menus, drag-and-drop, in-place rename, and association-based opening.
- Back, forward, up, refresh, location history, clickable breadcrumbs, editable paths, and environment-variable expansion.
- Commands for Quick access, known folders, This PC, Libraries, Network, and Recycle Bin.
- Eight layouts, sorting/grouping, native column toggles and autosizing, item checkboxes, hidden items, hide/unhide selection, navigation/preview/details pane controls, and a collapsible command band.
- Copy/cut/paste, copy paths, copy/move destination pickers, new folders/text files/shortcuts, properties, recycle deletion, and confirmed permanent deletion.
- Native AQS search folders with current-folder or recursive scope, kind/date/size refinements, 20 recent query entries per session, native saved searches, and return to the search's original folder.
- F6/Shift+F6 region routing and F11 fullscreen, with hidden-window style/geometry restoration checks.
- ZIP creation/extraction on a background worker, Unicode paths, and no-overwrite staging. Extraction supports classic stored/deflate ZIPs; ZIP64, encrypted, multipart, self-extracting, linked, or unsupported-metadata archives are rejected.
- Native sharing/security properties, printing through file associations, network-drive dialogs, Folder Options, and File History settings.

Shell-provided capabilities depend on Windows, installed handlers, policy, and devices. The application can still encounter slow network locations or Shell extensions. There is no measured speedup claim against Explorer.exe.

Important remaining work includes the other contextual ribbon pages, Quick Access Toolbar customization, full Share/New/Easy access menus, advanced search options, explicit Undo/Redo integration, dark mode, localization, and accessibility validation. New Shortcut currently chooses file targets; Paste Shortcut supports file and folder targets. The File History button opens settings rather than an individual file's version history.

Saved `.search-ms` files reopen through Windows and rerun their queries. Their query/scope metadata is not imported into the application's Search page, so refinement controls are unavailable on reopened saved searches. Saving supports filesystem folders and This PC, preserves relative dates, and never overwrites a file. Explicit filename word-prefix queries (`System.FileName:$<...`) are unsupported live; saving any explicit `$<` condition, unsupported value types, arbitrary virtual scopes, and literal-percent scope paths is also unsupported. Filename starts-with (`~<`) and wildcard (`~`) queries are supported.

## Build

Install Visual Studio 2022 Build Tools with **Desktop development with C++**, a Windows SDK, and CMake 3.24 or newer.

```powershell
./scripts/build.ps1 -Configuration Release
```

The executable is written to `build/Release/WindowsExplorer.exe`. To configure directly:

```powershell
cmake -S . -B build -G 'Visual Studio 17 2022' -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release --parallel
```

## Headless testing

All development tests run without showing the application or operating visible UI.

```powershell
./scripts/test.ps1 -Configuration Release
# After a build:
./scripts/test.ps1 -Configuration Release -SkipBuild
```

CTest runs console tests and a hidden-window Shell host. The host never calls `ShowWindow` on the application, blocks interactive operations, and observes process-owned top-level windows while pumping messages. Tests operate on owned temporary fixtures and preserve the user's clipboard, shared settings, and Shell undo history. Reports are written to `artifacts/`.

The current local verification includes 54 hidden-host checks, 3,151 keyboard/model assertions across eight groups, six item-action groups, five native search groups, settings/error cases, silent Shell operations, and ZIP/shortcut round trips. Search tests execute actual owned-fixture queries and compare live with reopened saved results. See [verification details](docs/verification.md) for the tested environment and coverage limits. A Windows CI workflow is configured; hosted CI results are pending.

## Design and research

- [Windows 10 research and UI specification](docs/windows-10-research.md)
- [Feature matrix and parity backlog](docs/feature-matrix.md)
- [Architecture and performance choices](docs/architecture.md)
- [Verification record](docs/verification.md)

Licensed under MIT. This independent project is not affiliated with Microsoft.
