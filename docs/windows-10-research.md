# Windows 10 File Explorer research

This document defines the Windows 10 baseline and its sources. Current implementation and measured results live in the [feature matrix](feature-matrix.md), [verification record](verification.md), [headless visual report](headless-visual.md), and [performance report](performance.md). Research requirements are not passing test results.

Research date: 2026-10-03. Target: desktop File Explorer on Windows 10 22H2, with release-, edition-, hardware-, policy-, and installed-handler-dependent behavior identified separately. This document is a requirements baseline, not a claim that the application has reached parity. Implementation state is in [feature-matrix.md](feature-matrix.md).

## Source and version discipline

Microsoft's current support pages often contain Windows 11 instructions first. Only their Windows 10 sections are used for Windows 10-specific behavior. Windows 11 Home, Gallery, tabbed windows, the modern abbreviated context menu, and additional archive formats are outside this baseline. Microsoft's Windows 10 section documents Quick access, optional Libraries, OneDrive integration, and the Share ribbon. [Microsoft Support: File Explorer in Windows](https://support.microsoft.com/en-gb/windows/experience/fileexplorer/file-explorer-in-windows?nochrome=true)

The illustrated primary references are Microsoft's *Windows 10 Tools* ebook, Chapter 2 (PDF pages 24-37; printed pages 16-29), and *Introducing Windows 10 for IT Professionals*, Chapter 2 (PDF pages 43-45; printed pages 31-33). Their screenshots were rendered and inspected headlessly. They establish the Windows 10 ribbon structure; their 2015 screenshots are not evidence that obsolete features remain in 22H2. Downloaded source PDFs and rendered images are research intermediates, not repository assets. [Windows 10 Tools](https://download.microsoft.com/download/7/3/8/7381E0E8-CE72-4366-9849-13B2BAFBBA3C/Microsoft_Press_ebook_Windows_10_Tools_8.5x11.pdf), [Introducing Windows 10](https://download.microsoft.com/DOWNLOAD/F/4/2/F42AE0AD-A9CB-4EE7-A209-D9A399604A72/MICROSOFT_PRESS_EBOOK_INTRODUCING_WINDOWS_10_PDF.PDF)

HomeGroup must not be rebuilt as an active feature: Microsoft removed it in Windows 10 version 1803. Network shares remain supported. [HomeGroup removed](https://support.microsoft.com/en-us/windows/experience/connectivity-networking/homegroup-removed-from-windows-10-version-1803)

## Window hierarchy and interaction

The visual order is title bar and Quick Access Toolbar, File menu and ribbon tabs, expanded ribbon commands when enabled, navigation/address/search row, folder content with a navigation tree on the left, optional Preview or Details pane on the right, and a status bar below. Microsoft's accessibility guide establishes separate content, column-header, status, toolbar, navigation-tree, and ribbon focus targets. F6 and Shift+F6 cycle major regions; Alt enters ribbon navigation. Native control use is only a starting point for accessibility: the host still needs coherent keyboard routing, names, focus, and state. [Microsoft's Windows 10 screen-reader instructions](https://support.microsoft.com/en-us/accessibility/windows/use-a-screen-reader-to-explore-and-navigate-file-explorer-in-windows)

The title-bar Quick Access Toolbar (QAT) and the Quick access folder are different features. QAT has customizable command shortcuts, can move above or below the ribbon, and works with a collapsed ribbon. The ribbon can be minimized. The navigation tree offers Navigation pane, Expand to open folder, Show all folders, and Show libraries. Preview and Details are alternative right-hand panes. These are documented and illustrated in *Windows 10 Tools*, printed pages 16-19. [Microsoft's illustrated guide](https://download.microsoft.com/download/7/3/8/7381E0E8-CE72-4366-9849-13B2BAFBBA3C/Microsoft_Press_ebook_Windows_10_Tools_8.5x11.pdf)

Address behavior includes clickable ancestor breadcrumbs, sibling navigation menus, an editable location, history, and navigation to Shell locations as well as filesystem paths. Navigation needs Back, Forward, recent locations, Up, Refresh, and sensible disabled states. A textual path is not a universal item identity: Shell namespace extensions can represent virtual objects. Use PIDLs and `IShellItem` when passing locations between components. [Understanding Shell namespace extensions](https://learn.microsoft.com/en-us/windows/win32/shell/nse-works), [IShellItem](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-ishellitem)

## Ribbon inventory

The following Home inventory was read from the Microsoft Windows 10 screenshot, printed page 16. Labels describe baseline requirements; implementation and verification are maintained in the feature matrix.

| Group | Windows 10 commands |
| --- | --- |
| Clipboard | Pin to Quick access; Copy; Paste; Cut; Copy path; Paste shortcut |
| Organize | Move to; Copy to; Delete; Rename |
| New | New folder; New item; Easy access |
| Open | Properties; Open; Edit; History |
| Select | Select all; Select none; Invert selection |

[Windows 10 Home ribbon screenshot](https://download.microsoft.com/download/7/3/8/7381E0E8-CE72-4366-9849-13B2BAFBBA3C/Microsoft_Press_ebook_Windows_10_Tools_8.5x11.pdf)

The Share area covers application sharing, email, ZIP packaging, printing, optical-media burning where available, network access grants/removal, and security access. Windows 10 Support specifically documents Share > Share, Share > Email, and Specific people. The actual Windows share picker is distinct from the file/folder Sharing property page. [Share files in Windows, Windows 10 section](https://support.microsoft.com/en-us/windows/experience/connectivity-networking/share-files-in-windows), [Network sharing and access removal](https://support.microsoft.com/en-gb/windows/experience/connectivity-networking/file-sharing-over-a-network-in-windows)

The View inventory has Panes; Layout; Current view; and Show/hide groups. Requirements include eight layouts (Extra large/Large/Medium/Small icons, List, Details, Tiles, Content), Sort by, Group by, Add columns, Size all columns to fit, item checkboxes, extensions, hidden items, Hide selected items, and Folder Options. Details columns support resize, reorder, metadata-specific columns, and filters. The ribbon's historical design rationale confirms grouping and column controls, visibility toggles, and contextual commands. This source describes Windows 8, so its obsolete Favorites and HomeGroup details are not copied into the Windows 10 baseline. [Microsoft's original Explorer ribbon design](https://learn.microsoft.com/en-us/archive/blogs/b8/improvements-in-windows-explorer), [Folder view modes](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/ne-shobjidl_core-folderviewmode), [IColumnManager](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-icolumnmanager)

Contextual ribbon requirements vary with selection/location:

| Context | Required capability family | Evidence and qualification |
| --- | --- | --- |
| Search Tools / Search | Scope, date/kind/size/property filters, recent searches, advanced options, save search, open file location, close search | Microsoft Windows 10 screenshot, printed page 33 |
| Picture Tools / Manage | Rotate left/right, slideshow, desktop background | Microsoft Windows 10 screenshot, printed page 32 |
| This PC / Computer | Properties/system commands, map/disconnect network drive, network-location management | Microsoft Windows 10 network-drive instructions |
| Libraries | Included locations, save location, content-type management | Windows Libraries documentation |
| Drives and optical/removable media | Drive properties, format, optimize/cleanup, BitLocker when available, eject/burn | OS/hardware/edition-dependent Shell command family; exact 22H2 ribbon labels need build-specific validation |
| ZIP and disc images | Browse/extract ZIP; mount/burn ISO; eject mounted image | Documented native operation families; contextual presentation still needs verification |
| Recycle Bin, applications, music/video, Network | Restore/empty, application verbs, play, network-management commands | Requirements for selection-aware presentation; no claim that a generic Shell host provides these ribbon tabs |

[Windows 10 contextual screenshots](https://download.microsoft.com/DOWNLOAD/F/4/2/F42AE0AD-A9CB-4EE7-A209-D9A399604A72/MICROSOFT_PRESS_EBOOK_INTRODUCING_WINDOWS_10_PDF.PDF), [Map network drive](https://support.microsoft.com/en-gb/windows/experience/connectivity-networking/file-sharing-over-a-network-in-windows), [Windows Libraries](https://learn.microsoft.com/en-us/windows/client-management/client-tools/windows-libraries), [Managing the file system](https://learn.microsoft.com/en-us/windows/win32/shell/manage)

File menu requirements include new window, current-folder command-line launch, folder/search options, and exit. Exact command-line entries changed across Windows 10 releases; an always-visible Computer tab is an implementation simplification, not contextual ribbon parity. The native Windows Ribbon Framework supplies a ribbon framework, but its presence does not supply Explorer's own command markup or implementation. [Windows Ribbon Framework](https://learn.microsoft.com/en-us/windows/win32/windowsribbon/windowsribbon-introduction)

## Folder content and Shell integration

Quick access combines pinned folders with frequently visited folders and recent files. Pin/unpin and remove-from-history differ from file deletion. Folder Options can independently disable frequent folders/recent files. Libraries aggregate multiple locations, can include local and eligible remote folders, have a default save location, and expose rich metadata/search. [Windows 10 Quick access options](https://support.microsoft.com/en-gb/windows/experience/fileexplorer/file-explorer-in-windows?nochrome=true), [Windows Libraries](https://learn.microsoft.com/en-us/windows/client-management/client-tools/windows-libraries)

The content view needs single/multiple/range selection, checkboxes, in-place rename, activation with associations, keyboard navigation, thumbnails, overlays, tooltips, sortable details, grouping, folder templates, and view-state persistence. Native `IFolderView2` exposes view mode/icon size, selection, sorting, grouping, flags, and properties; it does not remove the host's responsibility to map commands or restore state. [IFolderView2](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-ifolderview2)

Context menus must preserve installed Shell verbs such as Open with, Send to, Properties, provider commands, and registered extension actions. A command present in Explorer may be unavailable for another selection, provider, or installation. `IContextMenu` supports querying and invoking those commands; it is not a promise that every arbitrary canonical verb string exists. Clipboard and drag/drop must preserve Shell data objects and operation effects, including virtual items. [IContextMenu](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-icontextmenu), [Extending shortcut menus](https://learn.microsoft.com/en-us/windows/win32/shell/context), [Shell clipboard and drag/drop](https://learn.microsoft.com/en-us/windows/win32/shell/dragdrop)

File operations require recursive copy/move, rename, new items, recycling/permanent deletion, conflict decisions, cancel/progress, permissions, and undo where supported. `IFileOperation` is the native operation API. Its flags control recycling, collision handling, undo records, error UI, and elevation; successful queuing is not proof that all items completed. Undo state can depend on the user's Explorer session. Tests must inspect completion/abortion and affected files. [IFileOperation](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-ifileoperation), [Operation flags](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifileoperation-setoperationflags)

ZIP folders support browsing, adding files, and extraction. Do not import Windows 11's RAR/7z/TAR capabilities into Windows 10 requirements. ISO mounting is documented for Windows 10; handler availability and image validity still matter. [ZIP operations and version distinctions](https://support.microsoft.com/en-us/windows/experience/storage-filemanagement/zip-and-unzip-files), [Microsoft Windows 10 ISO mounting reference](https://download.microsoft.com/DOWNLOAD/F/4/2/F42AE0AD-A9CB-4EE7-A209-D9A399604A72/MICROSOFT_PRESS_EBOOK_INTRODUCING_WINDOWS_10_PDF.PDF)

Preview and metadata depend on installed handlers. Preserve the OS preview host instead of parsing every format in the application. Native property pages can expose General, Sharing, Security, Details, Previous Versions, and item-specific pages. File History/previous-version restoration requires an existing backup. Launching File History's control panel is not equivalent to opening selected-item history. [Preview handlers](https://learn.microsoft.com/en-us/windows/win32/shell/preview-handlers), [Shell extension handler families](https://learn.microsoft.com/en-us/windows/win32/shell/handlers), [File History](https://support.microsoft.com/en-au/windows/experience/backup-recovery/backup-and-restore-with-file-history)

OneDrive's sync provider owns account state, placeholders, availability badges, sharing verbs, and hydration. Files On-Demand requires Windows 10 1709 or later; online-only content is not available for content search until downloaded. A Shell host may inherit these integrations, but provider installation and behavior must be tested before claiming support. [OneDrive Files On-Demand](https://support.microsoft.com/en-us/onedrive/save-disk-space-with-onedrive-files-on-demand-for-windows)

## Search, keyboard, and appearance

Search needs scoped results, filename and content/property queries, date/type/size refinement, saved searches, and return-to-folder behavior. Windows Search AQS is a structured query language, not just substring matching. Generate canonical property queries programmatically; user-language aliases are localized. An in-memory search folder can be created with `ISearchFolderItemFactory` and navigated inside ExplorerBrowser. [AQS](https://learn.microsoft.com/en-us/windows/win32/search/-search-3x-advancedquerysyntax), [Search folder factory](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-isearchfolderitemfactory), [Microsoft's ExplorerBrowser search sample](https://learn.microsoft.com/en-us/windows/win32/shell/samples-explorerbrowsersearch)

| Shortcut | Windows 10 baseline behavior |
| --- | --- |
| Alt+D; Ctrl+E / Ctrl+F | Address; search |
| F4; Ctrl+L | Address history list; select the editable address |
| Alt+Left / Alt+Right / Alt+Up; Backspace | Back / forward / parent; back |
| Ctrl+N / Ctrl+W; Ctrl+Shift+N | New/close window; new folder |
| Alt+P; Alt+Enter | Preview; Properties |
| Ctrl+mouse wheel | Icon-size/view changes |
| Tree arrows, numpad plus/minus/asterisk | Expand/collapse tree and descendants |
| F11 | Full-screen toggle |
| F6 / Shift+F6 | Forward/backward cycle through major regions |
| F2; Ctrl+C / X / V / A; Delete | Rename; clipboard/selection; deletion |
| Ctrl+R; Ctrl+D; Ctrl+numpad plus | Refresh; recycle selection; fit Details columns |
| Ctrl+Shift+E | Expand the current folder's ancestors once |

[Microsoft keyboard shortcuts](https://support.microsoft.com/en-au/windows/keyboard-shortcuts-in-windows-dcc61a57-8ff0-cffe-9796-cb9706c75eec)

Windows 10 File Explorer gained dark mode in 1809. High contrast, dynamic DPI, text scaling, localized labels, and RTL are separate requirements. The host must scale its controls and fonts, not merely its outer window. Appearance must be verified using headless private-desktop captures at declared native DPI and dimensions. [Windows 10 dark theme](https://blogs.windows.com/windowsexperience/2019/04/01/windows-10-tip-dark-theme-in-file-explorer/), [Windows 10 desktop DPI improvements](https://blogs.windows.com/windowsdeveloper/2016/10/24/high-dpi-scaling-improvements-for-desktop-applications-and-mixed-mode-dpi-scaling-in-the-windows-10-anniversary-update/)

## Architecture and performance implications

`CLSID_ExplorerBrowser` is a documented embeddable Shell browser, with navigation, view events, and travel-log support. It is a practical compatibility-first foundation for a native C++ application. An embedded host must provide its address/search/ribbon controls and travel history while retaining the actual Shell view and status frame. [IExplorerBrowser](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-iexplorerbrowser), [ExplorerBrowser events](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-iexplorerbrowserevents)

`EBO_SHOWFRAMES` permits native command/navigation/details/preview frames, and pane visibility is configured through a site service. Headless browser instances also set `EBO_NOPERSISTVIEWSTATE`; Microsoft defines that flag as disabling view-state persistence, and headless hosts must verify that flag by native readback. The `EP_Ribbon` GUID does not prove a host reproduces Explorer's ribbon. Some pane descriptions retain older layouts, so build-specific checks matter. [Browser options](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/ne-shobjidl_core-explorer_browser_options), [Pane visibility](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-iexplorerpanevisibility-getpanestate)

Microsoft documents a DefView compatibility path that performs filtering on the UI thread. The host requests `CDB2GVF_NOINCLUDEITEM` only for active native search PIDLs. Ordinary folders need consistent hidden/protected-item policy; explicit normal setting commands must correspond to the shared Windows setting, while headless fixtures isolate their policy. This is an optimization opportunity, not a measured claim that all Shell search, attribute filtering, or extension work runs in the background. Native extensions can still affect responsiveness and process stability. [IExplorerBrowser filtering remarks](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-iexplorerbrowser), [In-process extension guidance](https://learn.microsoft.com/en-us/windows/win32/shell/shell-and-managed-code), [Host implementation](../src/app.cpp)

Avoid eager recursive size calculation, immediate thumbnail/property hydration of every item, duplicate directory enumeration, and polling remote namespaces. Observe navigation completion separately from complete folder population. Measure startup, first usable content, enumeration, interaction latency, CPU, and memory against a declared fixture and OS. A hidden-window navigation timing alone cannot prove the application is faster than Explorer.

Use Unicode throughout, retain item identities across refreshes, and distinguish local/UNC/virtual/placeholder paths. `longPathAware` does not make every Shell operation accept all extended paths: Microsoft explicitly distinguishes filesystem and Shell path behavior. [Maximum path length](https://learn.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation)

## Headless verification contract

All automated verification must remain headless. A hidden HWND with a pumped STA message loop can exercise the real Shell control without showing a window. Tests may inspect COM view state, callbacks, hidden-window visibility, parsed item identities, filesystem results, and operation progress/abortion. Use isolated temporary fixtures and noninteractive flags. Do not invoke association apps, modal property/share/format dialogs, interactive UAC, drive mapping, printers, or visible Shell menus during tests.

Separate pure model/serialization tests, silent operation tests, and hidden-host integration tests. A command being wired, a verb being delegated, or a pane-policy bit being set is weaker evidence than an end-to-end behavior test. Handler previews, cloud providers, live network/MTP devices, layout appearance, screen-reader behavior, contextual ribbon parity, and drag gestures remain unverified unless a suitable headless fixture explicitly establishes them. Preserve that distinction in release notes and the feature matrix.

Modern Windows 10 reference images supplement the older Microsoft illustrations. Every image's original bytes, source URL, SHA-256, dimensions, capture limitations, and annotated regions are declared in the [reference manifest](../tests/visual/windows10-reference.json). The [comparison workflow](headless-visual.md) uses actual rendered native HWNDs and explicitly separates unknown-DPI sources from exact pixel evidence. Downloaded Windows artwork and publisher images remain ignored research intermediates.

Windows 10 version 2004 introduced an adjustable search-box width and enhanced suggestions. A normal host needs a draggable left edge, persisted width, and recent-search suggestions, while respecting the DisableSearchBoxSuggestions policy. Microsoft's API provides native suggestions through IAutoComplete and an IEnumString source. [Windows 10 20H1 changes](https://learn.microsoft.com/en-us/windows-insider/archive/new-in-20h1), [Search suggestions policy](https://learn.microsoft.com/en-us/windows/client-management/mdm/policy-csp-admx-windowsexplorer#disablesearchboxsuggestions), [IAutoComplete](https://learn.microsoft.com/en-us/windows/win32/api/shldisp/nn-shldisp-iautocomplete)

Native ribbon command metadata is not always identical to a raw registry value. Exact titles, icons, availability, and checked state must come from the selected-item/background provider with its real view site. Fast queries may return E_PENDING; complete them asynchronously and reject stale selection generations. Popups retain the full native menu or exact enumerated child objects. [IExplorerCommand state](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-iexplorercommand-getstate), [Native command/resource audit](windows10-ribbon-icons.md)

The target-build layout is loaded through the public Ribbon Framework from the installed Windows 10 ExplorerFrame resource, after checking its command signature. Its resource module stays loaded for the framework's lifetime. This preserves the installed adaptive layout and native contextual templates without copying Microsoft artwork into the repository. Command handlers still belong to this application; loading a layout does not provide file-manager behavior. Other resource signatures use the authored fallback. [IUIFramework::LoadUI](https://learn.microsoft.com/en-us/windows/win32/api/uiribbon/nf-uiribbon-iuiframework-loadui), [Commands and controls](https://learn.microsoft.com/en-us/windows/win32/windowsribbon/windowsribbon-commandscontrols)

Runtime command galleries require each item to expose a command ID, command type and category, and each command must have its own registered handler. Populating ItemsSource alone is insufficient. The target framework accepts action and Boolean gallery rows; provider cascades retain their actual child hierarchy through native menus. Recent Items use the framework's original property objects and pin callback rather than an emulated list. [Microsoft gallery model](https://learn.microsoft.com/en-us/windows/win32/windowsribbon/ribbon-controls-galleries)

Modern flat navigation glyphs can be resolved from the installed Segoe MDL2 Assets font. Older ExplorerFrame Back/Forward bitmaps use visibly different artwork, so the existence of a resource alone does not establish fidelity. [Microsoft's glyph catalog](https://learn.microsoft.com/en-us/windows/apps/design/style/segoe-ui-symbol-font), [Actual scene comparisons](headless-visual.md)
