# windows-explorer

A native C++20 file manager targeting Windows 10 22H2. It runs in its own process, hosts actual Windows folder views through `IExplorerBrowser`, and uses the native Windows Ribbon Framework. The executable has no web runtime or Python dependency.

Normal launches use the installed Windows 10 ribbon layout on build 19045 after checking its resource signature. An incompatible system uses the authored native fallback. Windows resources are loaded from the operating system at runtime and are not included in this repository.

The [feature matrix](docs/feature-matrix.md) records each Windows 10 feature, its implementation, and its verification requirements. [Headless visual comparisons](docs/headless-visual.md) compare real private-desktop captures with pinned online Windows 10 references. Source wiring, passing behavioral tests, and matching pixels are separate evidence.

## Run

Build the current source using the instructions below. [Releases](https://github.com/ProtonDev-sys/windows-explorer/releases) currently provides the earlier v0.1.0 preview; its executable does not include subsequent source fixes. A newer executable must be matched to its own test reports and source commit.

```powershell
./WindowsExplorer.exe
./WindowsExplorer.exe --path 'C:\Users'
```

The executable is unsigned and includes the C++ runtime. It starts at Windows' configured Quick access or This PC location unless an explicit application startup location or command-line path is supplied. It runs alongside the Windows desktop shell. Application preferences and recent searches are stored under `%LOCALAPPDATA%\WindowsExplorer`; normal extension/hidden-item commands use Windows' shared settings.

## Features

- Native folder views, thumbnails, preview/details panes, overlays, association-based opening, inline rename, context menus, and drag-and-drop.
- Home, Share, View, Computer, and Network ribbon pages; Picture, Drive, Disc Image, Compressed, Search, Library, Recycle Bin, Application, Shortcut, Music, and Video contextual pages.
- Actual caption Quick Access Toolbar, customization, native keytips, ribbon collapse, and framework settings persistence.
- PIDL navigation history, ancestor breadcrumbs, asynchronous sibling menus, destination drop targets, editable Shell paths, and native address completion.
- Quick access, known folders, This PC, Libraries, Network, and Recycle Bin; configurable navigation tree expansion and roots.
- Eight native layouts, per-folder view persistence, native sorting/grouping and complete property-column menus, checkboxes, visibility settings, and the native status footer.
- Native copy/cut/paste, paste shortcut, copy/move destination menus, new-item handlers, properties, recycling, permanent deletion, native cancellation/progress/conflict handling, and Shell undo/redo.
- Actual installed Share, email, fax, ZIP, security/sharing, offline-files, network, media, drive, picture, application, and selected-file history commands with provider-owned availability.
- Native compressed-folder browsing, archive creation, Extract All, and extraction destinations. Normal archive commands use Windows' installed handlers.
- Localized AQS search, current-folder/subfolder scopes, kind/date/size/property refinements, native advanced search options, recent searches and suggestions, saved queries, and Open file location.
- Saved generic terms, phrases, numeric comparisons, relative and absolute date ranges, and multiple include locations, including actual Library locations; native saved results are checked against live results and exact file identities.
- Native inline Library creation, included locations, private/public default save locations, content templates, and native management commands.
- Keyboard navigation, region routing, fullscreen, process-local appearance, and native accessibility providers.

Installed providers, Windows policy, device support, and edition determine command availability. The application respects their actual states. Provider integration and arbitrary hardware/cloud behavior are tracked separately in the feature matrix.

## Build

Install Visual Studio 2022 Build Tools with **Desktop development with C++**, a Windows SDK, and CMake 3.24 or newer.

```powershell
./scripts/build.ps1 -Configuration Release
```

The executable is written to `build/Release/WindowsExplorer.exe`. A direct build uses:

```powershell
cmake -S . -B build -G 'Visual Studio 17 2022' -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release --parallel
```

The SDK's `UICC` compiles the ribbon markup during the build. Generated resources have one shared build owner. The application requires no elevation to start.

## Headless verification

All development verification stays off the user's visible desktop.

```powershell
./scripts/test.ps1 -Configuration Release
# After a build:
./scripts/test.ps1 -Configuration Release -SkipBuild

# Native screenshots and navigation benchmark:
./scripts/visual-check.ps1 -Configuration Release -CaptureOnly
./scripts/benchmark.ps1 -Configuration Release
```

CTest covers console fixtures, the hidden Shell host, actual native ribbon/accessibility controls, private-desktop capture, appearance, and guarded native history/search-option/folder-view persistence fixtures. Private desktops are created before COM/window initialization and cannot be switched onto the user's desktop. Capture validation checks actual PNG content, native control geometry, and expanded ribbon painting. Source comparisons preserve declared masks and strict thresholds.

Local tests preserve the user's clipboard, Shell history, shared settings, and libraries. Tests that must exercise real shared search options or global undo/redo require both explicit opt-in and disposable GitHub Actions execution; local skips are reported as skips. [Native search/history CI](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/37194199690) verifies those real operations. Current complete-run evidence is recorded in [verification](docs/verification.md).

The [performance report](docs/performance.md) measures navigation distributions, native callbacks, cached command latency, and memory for owned folders containing 10, 1,000, and 10,000 files. These app measurements do not establish a controlled speed comparison with `Explorer.exe`.

## Research and design

- [Windows 10 research and UI requirements](docs/windows-10-research.md)
- [Feature matrix](docs/feature-matrix.md)
- [Architecture](docs/architecture.md)
- [Verification](docs/verification.md)
- [Headless visual comparisons](docs/headless-visual.md)
- [Native ribbon resources](docs/windows10-ribbon-icons.md)
- [Installed Windows 10 ribbon adapter](docs/installed-ribbon.md)
- [Saved-query verification](docs/saved-search-verification.md)
- [Native folder-view persistence](docs/view-persistence-verification.md)
- [Theme compatibility](docs/theme-compatibility.md)

Licensed under MIT. This independent project is not affiliated with Microsoft.
