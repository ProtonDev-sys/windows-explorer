# windows-explorer

A native C++20 file manager for Windows 10, built with Win32 controls and the Windows Shell. It runs in its own process and hosts native folder views through `IExplorerBrowser`. No Electron, WebView, or JavaScript runtime is involved.

The first implementation focuses on Windows 10 Explorer's navigation, file operations, and Home/Share/View layout. **It is an initial working application, not complete Explorer parity.** The [feature matrix](docs/feature-matrix.md) separates implemented commands, Shell-provided behavior, and remaining work.

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
- Eight layouts, sorting/grouping, item checkboxes, hidden items, navigation/preview/details pane controls, and a collapsible command band.
- Copy/cut/paste, copy paths, copy/move destination pickers, new folders/text files/shortcuts, properties, recycle deletion, and confirmed permanent deletion.
- Native AQS search folders with scoped search and background enumeration.
- ZIP creation/extraction on a background worker, Unicode paths, and no-overwrite staging. Extraction supports classic stored/deflate ZIPs; ZIP64, encrypted, multipart, self-extracting, linked, or unsupported-metadata archives are rejected.
- Native sharing/security properties, printing through file associations, network-drive dialogs, Folder Options, and File History settings.

Shell-provided capabilities depend on Windows, installed handlers, policy, and devices. The application can still encounter slow network locations or Shell extensions. There is no measured speedup claim against Explorer.exe.

Important remaining work includes contextual ribbon pages, Quick Access Toolbar customization, full Share/New/Easy access menus, advanced search controls and saved searches, explicit Undo/Redo integration, dark mode, localization, and accessibility validation. New Shortcut currently chooses file targets; Paste Shortcut supports file and folder targets. The File History button opens settings rather than an individual file's version history.

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

The current verification includes 39 hidden-host checks, 356 shortcut assertions, settings/error cases, silent Shell operations, and ZIP/shortcut round trips. See [verification details](docs/verification.md) for the tested environment and coverage limits. CI builds on Windows and runs the same headless checks.

## Design and research

- [Windows 10 research and UI specification](docs/windows-10-research.md)
- [Feature matrix and parity backlog](docs/feature-matrix.md)
- [Architecture and performance choices](docs/architecture.md)
- [Verification record](docs/verification.md)

Licensed under MIT. This independent project is not affiliated with Microsoft.
