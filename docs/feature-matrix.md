# Windows 10 feature matrix

This matrix compares the source-backed [Windows 10 baseline](windows-10-research.md) with the current implementation. It records code-level state, not an automated parity score. Headless test reports are the authority for runtime results; listed acceptance criteria are requirements, not claims that those tests passed.

States: **Wired** = explicit application code exists, runtime verification must be reported separately. **Shell** = delegated to the Windows Shell and unverified outside tested fixtures. **Partial** = some behavior exists with stated limits. **Gap** = missing explicit functionality or parity. Hardware, edition, policy, associations, and installed providers can affect Shell features.

Reconciled on 2026-10-04. The local Release headless suite passed CTest 2/2; the separate hidden-host report passed 54/54 checks; keyboard/model tests passed 3,151 assertions across eight groups. Item actions passed six groups and native search passed five groups. The host/process-window observer found no visible application windows during message pumping. These results establish the fixture behaviors described below, not full Explorer parity. No visible application screenshots or interactive tests were used. Hosted CI is pending. [Test runner](../scripts/test.ps1), [Hidden-host checks](../src/app.cpp), [Keyboard tests](../tests/input_tests.cpp), [Search fixtures](../tests/search_tests.cpp), [Item-action fixtures](../tests/item_actions_tests.cpp)

Verified coverage includes local enumeration/navigation, selection all/none/invert, eight mode/icon-size readbacks, specific sort/group readbacks, checkbox flags, native column toggling/autosizing, ribbon collapse, hidden F11 restoration, 96-DPI minimum-width View bounds, hidden/protected filtering, Search context/state regressions, and actual live/saved search fixture results. Selected-item attribute/path helpers have separate fixtures. Preview/Details checks assert requested policy and mutual exclusion only. Focus actuation, clipboard publication, Recycle Bin deletion, network/cloud/device providers, handler rendering, visual layout, mixed DPI, dark theme, and accessibility remain untested.

## Window, navigation, and views

| Feature | State | Current behavior / remaining work | Headless acceptance criterion |
| --- | --- | --- | --- |
| Native C++ desktop process | Wired | Win32 controls and COM Shell browser; no browser runtime; hidden initialization verified | Build executable; assert hidden host and no observed visible process windows |
| Home/Share/View ribbon layout | Partial | Native buttons grouped into a ribbon-like band; simplified Share/New sections; always-visible Computer tab | Validate command IDs, enabled state, and DPI-derived control bounds |
| Quick Access Toolbar | Gap | No title-bar QAT customization, relocation, or per-command pinning | Serialize order/placement and verify hidden control structure |
| Minimize ribbon | Wired | Host collapses its command band; hidden test verifies reclaimed view space and removed ribbon controls | Check preference and browser rectangle before/after |
| Contextual ribbon tabs | Partial | Search page exists for in-session host searches; Picture/Drive/Library/Recycle Bin/Application/Media pages remain gaps; reopened saved searches lack host context metadata | Search commands/state checked in hidden host; other context rules need fixtures |
| Back/Forward/Up/Refresh | Wired | Host keeps PIDL history; fixture back/forward/parent identities verified; Refresh routed to native view | Navigate fixture/subfolder, back/forward/parent; separately verify refreshed contents |
| Recent-location dropdown | Wired | Host history menu | Verify history model, truncation after back, disabled boundaries |
| Breadcrumb ancestors | Partial | Clickable ancestors and editable path; no sibling dropdowns or breadcrumb drop targets | Resolve each ancestor PIDL; require sibling-menu/drop-target fixtures later |
| Typed locations | Wired | Unicode, environment expansion, relative paths, Shell parsing | Parse quoted/local/relative/UNC/virtual cases; reject invalid locations |
| Quick access | Shell | Explicit native namespace entry; pin action invokes Shell verb; privacy/history supplied by OS | Inspect namespace contents and enumerate supported verbs without invoking UI |
| Known folders / This PC | Wired | Commands for Desktop/Documents/Downloads/Pictures/Music/Videos and This PC; This PC navigation verified | Hidden navigation to all known folders still requires fixtures |
| Network / UNC / mapped drives | Shell | Namespace navigation plus map/disconnect dialogs; no live network fixture | Controlled share fixture verifies identity, failure, and disconnect behavior |
| Libraries | Shell | Navigate native Libraries; no host management ribbon | Fixture library aggregates folders and resolves its default save location |
| Recycle Bin | Shell | Native namespace and item context menus; no dedicated restore/empty ribbon | Recycled fixture can be identified/restored without unrelated items |
| Navigation pane | Wired | Host requests native frame visibility; rendered/native-tree behavior unverified | Check pane policy and actual native tree creation/visibility |
| Expand to folder / show all / show libraries | Shell | Relies on native navigation-pane behavior; no explicit View dropdown | Verify tree state through native tree APIs on a declared OS |
| Eight folder layouts | Wired | All eight `IFolderView2` mode/icon-size mappings verified by readback | Read back mode/icon size; visual presentation remains unverified |
| Sorting | Partial | Name/date/type/size and direction; name/ascending readback verified | Test other keys/directions and actual fixture ordering |
| Grouping | Partial | Name/date/type/size/none; type/ascending and removal readbacks verified | Test other keys and rendered grouping behavior |
| Details columns/filtering | Partial | Add columns menu and Size all columns command use `IColumnManager`; toggle/restore Size, Name protection and autosizing verified | Arbitrary columns, ordering, filters and visual widths remain unverified |
| Item checkboxes | Wired | `FWF_CHECKSELECT` toggle verified by native flag readback | Check visible checkbox interaction separately |
| Hidden items | Wired | Per-app `IFolderFilter`; hidden fixture appears/disappears, hidden+system fixture stays excluded; return from search verified | Verify protected-file exclusion and hidden policy without changing global settings |
| File extensions | Wired | OS-global Shell setting toggle, unavailable in headless mode | Test settings abstraction without changing the user's global preference |
| Hide selected items | Wired | Toggle Hidden on selected items only; no recursion; preflight/reparse rejection and unrelated compressed/sparse attribute preservation verified | Isolated fixtures pass; actual ribbon selection interaction remains unverified |
| Folder/view preferences | Partial | App persists window, startup, view, pane and ribbon preferences; native property bag used | Round-trip config; verify corrupt input defaults and view restoration |
| Folder Options | Shell | Launches OS Folder Options | Validate routing in tests; do not open dialog |
| Status counts/timing | Partial | Counts and navigation timing only; no periodic size-property reads, selected byte totals, or view buttons | Compare fixture counts; verify status model without eager metadata reads |

Sources: [ExplorerBrowser](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-iexplorerbrowser), [Browser frames](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/ne-shobjidl_core-explorer_browser_options), [Folder view control](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-ifolderview2), [Columns](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-icolumnmanager), [Windows 10 Quick access](https://support.microsoft.com/en-gb/windows/experience/fileexplorer/file-explorer-in-windows?nochrome=true), [Libraries](https://learn.microsoft.com/en-us/windows/client-management/client-tools/windows-libraries).

## File operations and providers

| Feature | State | Current behavior / remaining work | Headless acceptance criterion |
| --- | --- | --- | --- |
| Single/range/multiple selection | Shell | Native view; host all/none/invert verified with exact fixture counts; range gestures untested | Verify gesture/range semantics through a suitable headless fixture |
| Open / Edit / Print | Shell | Canonical Shell verbs; association-dependent | Query availability and routing; never launch associated apps in tests |
| Open with / Send to / extension menus | Shell | Native view menus; no host Open-with button | Enumerate installed context-menu verbs; no parity claim for arbitrary extensions |
| Rename | Wired | Native in-place command; lower-level silent Unicode rename and collision behavior verified | In-place editor interaction remains unverified |
| New folder | Wired | Native operation and selection for inline edit; silent helper creation/collision verified | Inline edit and command interaction remain unverified |
| New text document | Wired | Creates unique empty text file; sends Shell notification | Verify exclusive creation and no overwrite |
| New shortcut / paste shortcut | Partial | Native `.lnk` creation with `IShellLinkW`/`IPersistFile`; New chooser selects files; Paste accepts clipboard files/folders | File/folder link persistence, Unicode targets, missing targets and no-overwrite verified; chooser/global clipboard untested |
| Full New item / Easy access menus | Gap | Only folder/text/shortcut shortcuts | Enumerate registered New handlers; model permissions/offline/library actions |
| Copy/Cut/Paste | Wired | Shell clipboard objects/operation helper; tests do not publish or replace the user's clipboard | Argument rejection tested; global clipboard/cut/paste behavior remains unverified |
| Copy path | Wired | Quoted Unicode paths and canonical Shell-name fallback verified in helper fixtures; clipboard publication untested | Global clipboard publication and arbitrary provider paths remain unverified |
| Copy to / Move to | Partial | Native picker/operations; silent file copy/move/content/collision fixtures pass; no recent-destination submenu | Recursive, permission and interactive-picker cases remain unverified |
| Delete / permanent delete | Wired | Native deletion and normal-mode permanent confirmation; silent permanent fixture deletion verified | Recycle Bin and confirmation behavior remain untested |
| Undo / Redo | Gap | Helpers return `E_NOTIMPL`; capability flags false and controls disabled; native-view Ctrl+Z unverified | Verify a real reversible undo implementation before advertising it |
| Conflicts / cancel / progress / elevation | Shell | Native operation system; complex cases need fixtures | Inspect aborted/completed results; no interactive elevation or progress dialogs |
| Drag/drop | Shell | Native view/data objects; no custom breadcrumb target | Test data-object formats/effects silently; gesture behavior remains unverified |
| Properties / security / sharing pages | Shell | Native property pages | Verify target/page routing without showing dialogs |
| Share picker / nearby sharing / email | Gap | Sharing button opens property page, not Windows Share picker | Mock share payload and recipient-independent routing |
| Specific people / remove access | Shell | May be available through native properties/context menus; no dedicated ribbon controls | Verify capability; do not alter real shares |
| ZIP create / extract | Partial | Explicit `ExtraOperations` with native wide-path staging and Windows-bundled `tar.exe`; app runs work asynchronously; classic stored/deflate ZIP only | Unicode round-trip, empty/read-only archives, no-overwrite and unsafe/unsupported fixture rejection verified; see archive limits below |
| ISO mount / burn | Shell | Native handlers/context menus only; no contextual ribbon | Inspect registered handlers; actual mounting requires isolated headless fixture |
| Drive format / optimize / cleanup / BitLocker / eject | Shell | Native drive commands only; no dedicated ribbon | Verify capability/routing without destructive device operations |
| File History / previous versions | Partial | Launches File History control panel; selected-item history is a gap | Validate routing; backup fixture needed for restore behavior |
| Preview / Details panes | Shell | Host requests native panes; policy and mutual exclusion verified only | Native frame rendering, handlers and metadata editing remain unverified |
| Thumbnails / metadata / icon overlays | Shell | Native view and installed handlers | Controlled handler fixture validates metadata without user files |
| OneDrive placeholders/sharing/status | Shell | OS/provider integration; no live account fixture | Declared provider fixture verifies status, offline failure, and hydration semantics |
| MTP / devices / namespace extensions | Shell | Browser accepts Shell locations; no device/extension fixture | Fake or controlled provider enumerates/navigates virtual items |
| Live filesystem updates | Shell | Native view notification behavior | Add/rename/remove fixture externally, await exact updated view contents |

Sources: [Native file operations](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-ifileoperation), [Operation flags](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifileoperation-setoperationflags), [Context-menu contracts](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-icontextmenu), [Clipboard/drag/drop](https://learn.microsoft.com/en-us/windows/win32/shell/dragdrop), [ZIP](https://support.microsoft.com/en-us/windows/experience/storage-filemanagement/zip-and-unzip-files), [Sharing](https://support.microsoft.com/en-us/windows/experience/connectivity-networking/share-files-in-windows), [File History](https://support.microsoft.com/en-au/windows/experience/backup-recovery/backup-and-restore-with-file-history), [Preview handlers](https://learn.microsoft.com/en-us/windows/win32/shell/preview-handlers), [OneDrive](https://support.microsoft.com/en-us/onedrive/save-disk-space-with-onedrive-files-on-demand-for-windows).

## Search, accessibility, appearance, and performance

| Feature | State | Current behavior / remaining work | Headless acceptance criterion |
| --- | --- | --- | --- |
| Search box / scoped results | Partial | Native AQS with recursive scope or filesystem-only shallow restriction; actual owned-fixture membership and live/saved parity verified; no suggestions | Content/index/provider completeness remains unverified |
| AQS / content / indexed search | Partial | Localized parser resolves generic terms/dates/properties; canonical filters and operators have result fixtures; explicit filename `$<` rejected before native execution | Content indexing and arbitrary providers need fixtures; defaults/literals and other properties are not blanket-rejected |
| Search ribbon filters / close search | Partial | Contextual current-folder/all-subfolders, six kind/six date/seven size filters, Save and Close; origin preservation, category replacement and failed-navigation state verified | Advanced content/property/index options and open-file-location controls remain gaps |
| Saved searches | Partial | Native `.search-ms` XML preserves relative dates, Unicode and recursion; exclusive creation; native reopening/results/identity verified | Reopened files lack host query/scope metadata and Search controls; unsupported types, explicit `$<`, arbitrary virtual or percent-path scopes reject before writing |
| Recent searches | Partial | Up to 20 unique query strings per session; no persisted history | Verify session model separately from persistent Explorer history |
| Keyboard shortcuts | Partial | Navigation/edit/selection/pane/view/F6/F11 routing and focus model verified with 3,151 assertions; full baseline not covered | Actual focus and native interaction remain untested |
| F6 region cycle | Partial | Availability-aware forward/backward model and native control mapping exist; wraparound/subsets verified | Focus actuation and accessible focus order remain unverified |
| Ribbon keytips | Gap | No Explorer ribbon keytip system | Verify accelerator/command discovery through an accessible fixture |
| F11 fullscreen | Partial | Hidden style/geometry change and restoration verified without displaying the window | Maximized placement restoration and visible monitor behavior remain unverified |
| Native content accessibility | Shell | Native view providers inherited, unverified in screen-reader use | Inspect provider roles/name/state through a headless-capable provider fixture |
| Host accessibility | Partial | Standard labeled Win32 controls; full focus/name audit outstanding | Inspect control names, focus order, enabled states and keyboard activation |
| DPI scaling | Partial | DPI-aware sizes/fonts and change handling; hidden View controls fit 1,030-pixel minimum width at tested 96 DPI | Other DPIs, mixed monitors, text scaling and visual clipping remain unverified |
| Dark theme / RTL / localization | Gap | Host does not reproduce Windows 10 dark theme; English labels | Theme/resource/layout unit tests, high-contrast fallback |
| High contrast | Partial | System brushes/native controls; no complete verification | Check color policy and hidden control state with injected settings |
| Responsiveness optimization | Partial | `NOINCLUDEITEM` only for native search PIDLs; ordinary folders use per-app attribute filter; status reads counts only; archive worker is asynchronous | Search policy verified; no comparative responsiveness benchmark |
| Faster than stock Explorer | Unproven | No controlled comparative benchmark | Same OS/storage/fixture/cold-warm conditions; report distributions and limits |
| Headless-only tests | Wired | Hidden host/process-window observer, native COM fixture enumeration and silent operations; interactive/global settings paths blocked in smoke | Local CTest 2/2 and smoke 54/54 passed; no UI screenshots or interactive testing; hosted CI pending |

Sources: [AQS](https://learn.microsoft.com/en-us/windows/win32/search/-search-3x-advancedquerysyntax), [Saved-search format](https://learn.microsoft.com/en-us/windows/win32/search/-search-savedsearchfileformat), [Canonical size ranges](https://learn.microsoft.com/en-us/windows/win32/properties/props-system-size), [Canonical item type](https://learn.microsoft.com/en-us/windows/win32/properties/props-system-itemtype), [Search sample](https://learn.microsoft.com/en-us/windows/win32/shell/samples-explorerbrowsersearch), [Keyboard shortcuts](https://support.microsoft.com/en-au/windows/keyboard-shortcuts-in-windows-dcc61a57-8ff0-cffe-9796-cb9706c75eec), [Windows 10 accessibility](https://support.microsoft.com/en-us/accessibility/windows/use-a-screen-reader-to-explore-and-navigate-file-explorer-in-windows), [Dark theme](https://blogs.windows.com/windowsexperience/2019/04/01/windows-10-tip-dark-theme-in-file-explorer/), [DPI](https://blogs.windows.com/windowsdeveloper/2016/10/24/high-dpi-scaling-improvements-for-desktop-applications-and-mixed-mode-dpi-scaling-in-the-windows-10-anniversary-update/).

## Explicit parity backlog

Archive support intentionally rejects ZIP64, encrypted or self-extracting archives, unsupported methods/extra records, unsafe entry names, links/reparse paths, and existing outputs. Creation copies sources into staging; extraction copies the input archive and stages expanded contents. This consumes additional disk space and is a compatibility limit, not full archive parity. UI progress is an activity message; interactive cancellation is not implemented. [Archive implementation](../src/extra_operations.cpp), [Archive/shortcut fixtures](../tests/extra_operations_tests.cpp)

The largest remaining parity work is the other contextual ribbons; QAT customization; real Undo/Redo; full Share/New/Easy access commands; breadcrumb sibling menus/drop targets; advanced search options and reopened-query host metadata; richer column filtering/status view buttons; selected-item File History; complete keyboard/accessibility behavior; dark/localized/RTL presentation; and controlled cloud/network/device/extension fixtures. These gaps remain even when the underlying Windows Shell supports a corresponding operation.

Explorer's desktop/taskbar/start-menu hosting and replacement of the system-wide Win+E association are separate system integration tasks. This application currently runs alongside the Windows shell. HomeGroup is excluded because it was removed from the target version; Windows 11-only features are excluded because the requested baseline is Windows 10.
