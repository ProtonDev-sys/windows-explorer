# Headless verification

The target machine is Windows 10 Home 22H2, build 19045. Builds use Visual Studio
2022 Build Tools, MSVC 19.44, Windows SDK 10.0.26100.0, C++20 and the static x64
runtime. Verification runs without displaying anything on the input desktop.

```powershell
./scripts/build.ps1 -Configuration Release
./scripts/test.ps1 -Configuration Release -SkipBuild
python -m pip install -r scripts/requirements-visual.txt
./scripts/visual-check.ps1 -Configuration Release -InstalledRibbon
./scripts/benchmark.ps1 -Configuration Release -InstalledRibbon
```

The scripts preserve build output, CTest JUnit, the exact application smoke
report, executable checksum and environment in ignored `artifacts/`. Screenshot
and performance runs copy the executable into a unique directory and verify its
checksum before use. Reports describe the tested snapshot; a previous passing
run is not evidence for later source changes.

## Current evidence

Evidence belongs to the tested source and executable, rather than the latest
working tree. The v155 Release build is warning-free and records executable
hashes and 151 unchanged source files. Earlier signedness and SDK-macro build
failures were corrected before the affected native checks ran.

The v136 authored and installed General hosts pass in 65.86 and 73.04 seconds.
The focused Search screenshot passes the unchanged reference protocol at
98.24% pixel agreement. These are scoped results, not a complete application
or screenshot-suite pass.

The v143 eight-target run passes six targets: both original App search lifetime
targets, both 130-query resident-cache targets and both original RecentItems
layouts. The retained native cursor is consumed for the first time after close,
before separately rebinding the retained item and PIDL. Neither a fresh cursor
nor an unsupported Reset substitutes for that original cursor.

The v157 same-executable App debugger run reaches the original immediate-owner-
destruction case after the first nine cases complete their assertions, including
nested reset, initialize entry, replacement and reentered WM_CLOSE.
Deferring physical Ribbon retirement until its active native close returns
avoids the earlier DirectUI freed-listener fault in those observed cases.
The nested close now records one deferred request and one original native lower
call. The next immediate DestroyWindow case still crashes in native UIRibbon's
ToolWindowMgr while the original close remains active. Dump disassembly identifies
that enum-5 case precisely; buffered console output is not used to infer it.
This is not a passing App close target. The later pre-first-retain controls
remain unreached. Microsoft requires asynchronous Close/Exit from native Ribbon
callbacks; retaining native COM objects alone does not establish safe immediate
child-window destruction. No profile pins are changed by these tests.
[Microsoft Ribbon migration contract](https://learn.microsoft.com/en-us/windows/win32/windowsribbon/ribbon-migration).

Both original strict QAT preservation/settings controls pass in v152. Both v155
gesture targets skip before opening a menu with ERROR_NOT_SUPPORTED at the
context-action gate. Their actual Cut buttons supply empty UIA runtime-ID arrays.
Two fresh rooted walks establish the same unique path and actual native Legacy
receiver, role, name, bounds and state despite distinct COM wrapper identities.
That native bridge passes without promoting empty runtime IDs or wrapper
pointers into identity. The actual context action remains unverified.

The v152 seven-target control run passes namespace actions, both original QAT
controls, both original Ribbon layouts and both original RecentItems layouts
in 56.96 seconds. Source and the monitored application executable remain
unchanged throughout that run. The separate opt-in Hosted persistence fixture
compiles without warnings in v153; it has not been executed locally or on a
disposable runner at this snapshot. Its default test registration is disabled.

The default v155 image target passes in 28.74 seconds with the production
NativeApartmentOwner and without the temporary module-retention switch. It
requires all 184 comparable first-current outputs to match exactly, plus
both-size Home and Computer override/restoration checks. The real dynamic
gallery requests SmallImage only; its direct public property reads return
ERROR_NOT_SUPPORTED. Every supplied callback remains checked, while both sizes
of the public association cache are independently rasterized and compared.
No unused framework callback is fabricated. The v150 authored and installed
shared icon-cache checks also pass at the actual 96 DPI.

The v155 apartment contract passes its actual nested initialization, live-owner,
wrong-thread, failure and retirement checks. Its two-creator module target now
passes using two actual installed NativeRibbon hosts. Each creator acquires one
normal reference to the already-loaded verified ExplorerFrame code mapping,
retires its Ribbon/client, completes its real matching COM/OLE teardown, then
releases that reference exactly once. The worker's retirement preserves the
parent's mapping. The earlier exact-binary unheld
v142 probe records WinTypes accessing unloaded ExplorerFrame during OLE teardown.
The current default image pass establishes its observed teardown case, without
claiming arbitrary native-helper or external-consumer lifetime coverage.

The v155 read-only resource oracle completes its actual ownership/cleanup
contract in 2.62 seconds. It records three owned namespace views, real native
first-property callbacks, CommandStore/provider HRESULTs, MUI inventory and
independent Framework versus App color properties. The four missing first-image
resources and equivalent native label inputs remain unavailable. Its exit zero
establishes completed diagnosis, not Explorer-host artwork or palette parity.

| Snapshot / scope | Actual result | Remaining implication |
| --- | --- | --- |
| Local v158 image / gesture targets | Image passed; both gestures failed; 30.84 seconds | Actual native right-clicks open the genuine enabled Add menu. Popup path ownership is rejected before Invoke, so no Add/Remove coverage is claimed. Temporary fixture module-retention control is removed. |
| Local v156 complete core targets | Sixteen passed; 323.92 seconds | File/Shell operations, saved-search metadata, Library, namespace/App commands, all menu partitions, 10,000-item selection, keyboard focus, search handoff, capabilities and Cast pass on the recorded source. |
| Local v155 eight scoped targets | Six passed, two skipped; 64.58 seconds | Image provenance, resource-diagnostic cleanup, both RecentItems layouts and both apartment targets passed. Both genuine QAT gestures supplied no action coverage. Later v157 dump mapping identifies the App fault at immediate owner destruction. |
| Local v152 original Ribbon / namespace controls | Seven passed; 56.96 seconds | The original controls remain intact; this does not establish the new close, context-gesture or profile-persistence outcomes. |
| Local v151 seven scoped targets | Two passed, four failed, one skipped; 74.58 seconds | Default image provenance and the apartment contract passed. Both App close processes crashed; both native QAT gesture targets rejected Legacy identity. The separate module target supplied zero coverage. |
| Local v127 complete suite | 34 passed, one failed, five appropriately skipped; 621.76 seconds | Installed General failed the original delayed Kind/navigation check. This is not a complete passing suite. |
| Local v130 eleven scoped targets | Nine passed, two App pin-close targets failed; 67.77 seconds | Both original QAT controls, namespace, registered menus, authored Ribbon/RecentItems and Cast passed. Read-only App pin-close integration did not. |
| Local v130 additional installed / General targets | Installed Ribbon and RecentItems passed; both General targets failed; 182.67 seconds | The new Kind source-capture hook was cleared before completion. The next fixture revision still needs actual rejection evidence. |
| Local v131 namespace / General | Namespace passed 22 groups and 2,710 assertions; both General processes failed; 144.68 seconds | Both recorded actual old Music completion S_OK but failed the exact rejection aggregate. Authored Open File Location also failed with the correct native item selected and an outstanding pending-selection token. Neither failure is waived. |
| Local v133 scoped native checks | Four passed, three failed; 78.83 seconds | All 184 comparable first-native outputs are now SAME; extra Copy override setup fails. Both App pin-close tests still fail. Their stage receipts place the ordinary commit inside WM_CLOSE dispatch before App onMessage entry. |
| Local v130 image provenance | 182 comparable first-native-current images, zero identical outputs, 182 different outputs; 28 seconds | The measurement fixture passed its accounting, not UI parity. The application replaced authentic Windows artwork. The v133 source preserves those first native images and now requires every comparable output to be identical. |
| Local v131 read-only App pin-close | Both layouts failed; 4.16 seconds | The original receipt established entry before App onMessage, not before WM_CLOSE dispatch. v133 stage receipts locate the batch inside real WM_CLOSE dispatch, where it received headless E_ACCESSDENIED and did not recur during reset. The earliest owned-window close hook and controlled pre-retain reset/destruction checks require native acceptance. |
| Hosted commit 870d5c5 | Main: 34 targets, five failures, two skips; separate transfer/drop jobs passed | Native selected-array preservation, both registered-menu partitions and both QAT layout controls failed. Later local source/diagnostic changes are not a hosted fix until rerun. |

Hosted evidence is public at [run 37432847908](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/37432847908).
Its actual fresh-machine clipboard transfer and independent drop processes
passed in 5.793 and 0.661 seconds. The main job failure prevents publication of
its combined validated application. The local QAT tests continue to exercise
both real layouts; the Server 2022 admission correction runs their unchanged
strict authored layout rather than assuming installed Windows 10 resources.

The latest complete strict screenshot run is v134: eighteen native captures,
six comparisons passed, nine failed, three restricted references and one Search
capture failure. The subsequent focused Search run passes. Fresh v143 modern
captures use the corrected 512-row window bound; all three still fail chrome
at 97.50%, 97.38% and 97.39% pixel agreement against the unchanged 98% threshold.
Their separate dropdown and Refresh regions pass. Its exact scene
results are in [headless-visual.md](headless-visual.md).
A successful native-current image check cannot replace a new complete screenshot
comparison. The final 24-process interleaved performance comparison has not run;
the earlier slow 1,000-item navigation episode remains unresolved.

Search backing now bounds cache-owned records at 128 without a lifetime query
quota. Cache eviction and App close preserve published descriptor paths; they
do not claim a last-consumer census or transparently reclaim those files. The
v143 stress and delayed-native-use checks pass at their recorded snapshot. Native QAT context gestures,
actual App pin persistence, noncurrent-DPI production image callbacks, arbitrary
installed handlers and whole-application parity require their own evidence.

## Test boundaries

| CTest target | What it verifies |
| --- | --- |
| `core_and_shell_operations` | Owned file operations and recovery, preferences, input mapping, archives, shortcuts, context menus, native resources/lifetime, breadcrumbs and search history; SavedSearch, Library and namespace groups run separately below |
| `native_saved_search_metadata`, `native_library_operations`, `native_namespace_actions` | Original saved-query membership/metadata, native Library operations and complete namespace provider/state/lifetime groups in separate 60/90/45-second processes |
| `native_app_commands` | All original App-command routing/provider groups, complete targets/site, native capability/state and cancellation/lifetime guards |
| `native_menu_state_small`, `native_menu_state_large`, `native_menu_state_stress_files_native`, `native_menu_state_stress_files_registered`, `native_menu_state_stress_mixed_native`, `native_menu_state_stress_mixed_registered` | The same actual full-array native state/menu comparisons over 1/2/16, 5,001 and four 10,000-target partitions, preserving resource restrictions/restoration, missing/duplicate verbs and retained-provider lifetime |
| `native_search_window_handoff` | Complete validated search context, reduced read-only mapping, malformed output preservation and actual explicit-handle-list child inheritance; fresh native factories, exact scope/result identities and child lifetime |
| `hidden_shell_host` | The real application, native ItemsView, navigation, eight layouts, columns, complete selection changes, search/import/refine/history, search-window Content/List restoration and Close origin, atomic Clear History, ZIP contexts, native tree options, full-width footer, splitter, F6/Tab focus routes, Ribbon/QAT and owned UI Automation |
| `hidden_library_shell_host`, `installed_library_shell_host` | The same original authored/installed native Library App assertions in dedicated 90-second processes, with owned Unicode-member/source/descriptor identities and exact view/history preservation |
| `native_view_selection` | Complete actual selection identities and complements, focus and checkbox flags on an owned 10,000-item native view |
| `installed_ribbon_features` | Read-only edition, media, recording and policy-dependent native capabilities |
| `hidden_native_ribbon` | The compiled native Ribbon, pages, contextual state, collections and images, native customization, persistence, minimized/docking state, accessibility and bounded tab selection |
| `private_desktop_visual_capture` | Actual native window/control painting, PrintWindow/WIC output, geometry, text changes, invalid inputs, exclusive output creation and desktop isolation |
| `native_quick_access_preservation`, `native_quick_access_settings_envelope` | Actual twenty-command collections, retained whole-row edits, native settings order envelope, strict corruption/rollback/reentry and retired-generation cases; both real layouts |
| `native_recent_items` | Four original actual native Ribbon RecentItems cases plus genuine shutdown nested-reset/initialize-entry callback revocation; no personal destinations |
| `native_search_backing_lifetime`, `native_app_search_backing_lifetime`, `native_app_search_backing_lifetime_installed` | Four actual store ownership/replacement groups and real authored/installed App query/refinement/history/recreation/child/reentry/close cases; bounded resident cache and persistent original descriptor paths |
| `installed_native_ribbon`, `installed_recent_items`, `installed_shell_host` | Additional actual installed-resource tests configured on the target Windows 10 build |
| `headless_crash_diagnostics` | Exact opt-in dump target validation and original exception context in an owned hidden child |
| `saved_search_presentation` | Actual public query presentation and all eight app-owned companion layouts; atomic, stale-file and failure preservation |
| `live_search_policy_and_native_results` | Latest-only scheduling, explicit commit, stale/cancelled result handling and real owned native result identities |
| `native_shell_history` | Actual normal Shell Undo/Redo and registered-command state; restricted to an opted-in disposable GitHub runner |
| `native_search_options` | Actual native Contents/System/Compressed transitions, fresh-query result membership and exact settings restoration; restricted to an opted-in disposable GitHub runner |
| `native_view_persistence` | Real native folder property-bag restoration and no-persist control; restricted to an opted-in disposable GitHub runner |
| `native_shell_transfer` | Real Shell clipboard Copy/Paste, Cut/Paste, Paste Shortcut and native Copy/Move/Link drops with exact owned identities/completion; restricted to a fresh opted-in disposable GitHub runner; no current local execution proof |
| `native_shell_drops` | Independent native Copy/Move/Link drop process with unchanged clipboard owner/sequence/formats and exact owned identities/completion; same genuine disposable-runner gate; no current local execution proof |
| `native_theme`, `native_theme_installed` | Theme policy, actual native dark pixels on recognized Windows builds, Light restoration, ownership guards and unchanged system configuration; installed-resource coverage on build 19045 |

The five shared-state mutation targets check both `GITHUB_ACTIONS=true` and their
explicit test opt-in before COM, windows or fixtures. They report **skipped**
locally. Setting those variables on a personal machine is not an authorized test
method. All other fixtures use exclusively owned temporary paths and preserve
existing files, clipboard data, user Libraries, Recycle Bin contents and system
preferences.

The application and native-control tests attach their UI threads to a new private
Windows desktop before COM or HWND creation. The desktop has no switch access.
Native controls may be visible there so that Windows can paint them; the desktop
is never made interactive. Isolation is checked during message pumping and after
teardown. Read-only UI Automation runs on a windowless worker in that same
private desktop while the owner thread dispatches messages. No desktop-root UI
automation or input injection is used.

The core native-menu fixture creates a distinct fresh private desktop before its
worker STA initializes COM. Its visibility guard therefore measures those native
menu calls separately from earlier EDIT/InputSwitch helpers. The v42 log records
different creator/fixture desktop identities, zero visible windows at every
fixture stage, an empty window inventory after resource/STA release and unchanged
creator/input isolation. The creator pumps COM until the worker thread actually
exits; no menu is displayed or invoked. This passing fixture does not explain the
historical isolation failure retained below.

Shutdown normally drains native workers before releasing their borrowed sites,
COM apartments or desktops. If bounded draining fails, the failure path retains
those resources and terminates only its own process with a nonzero status, using
`TerminateProcess(GetCurrentProcess(), ...)` and a `std::_Exit` fallback. This
avoids the DLL-detach deadlock documented for `ExitProcess` when another thread
holds an unknown lock.
([TerminateProcess](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-terminateprocess),
[ExitProcess](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-exitprocess).)
Successful v42 teardown passed; forced drain failure was reviewed in source and
is not claimed as an exercised runtime branch.

## Native behavior evidence

Result tests compare strict cardinality and exact volume/128-bit file identities,
including duplicate rejection and outside-scope exclusion. Search-wrapped PIDLs
can differ from ordinary filesystem PIDLs, so actual file identity is the
membership authority. Tests distinguish shallow/deep scopes, equal basenames
in different locations, recursive exclusions, Library unions, relative dates,
generic word matching and saved-query discovery of new files. See the
[saved-search verification](saved-search-verification.md) for the supported
condition and provider combinations.

Search-window handoff preserves the complete query, every original scope and rule,
file metadata, native Content/List presentation and a distinct Close origin. The
production child consumes a bounded immutable packet through an inherited
read-only mapping and rebuilds a fresh public native search folder. The standalone
test verifies actual process inheritance, exact object identity for excluded and
consumed handles, malformed-packet output preservation and exact native result
FileIDs. The App checks use the same startup preparation path and prove native
Date gallery publication, Back/Close/Escape/clear behavior and unchanged parent
state/source files. The normal headless New Window command still refuses a launch.

Clear History saves the empty codec before publishing a cleared MRU or suggestion
list. Both App layouts prove that an owned directory-target write failure preserves
the exact codec, MRU, actual suggestion source, displayed snapshot and native view
state; a relative override is rejected unchanged. Successful owned persistence
then clears all four lists while retaining the same suggestion source. These tests
use a real suggestion object without attaching autocomplete or modifying personal
history.

Normal Shell commands retain their actual target array, provider and view site.
Fast native state is compared with installed context menus; full cascades are
enumerated when needed. The application's headless guards reject associated-app launches,
recipients, wizards, device operations, clipboard publication and native drops.
Mock handlers test routing, effects, failure and lifetime without treating a mock
invocation as proof that every installed extension works.

Disposable Windows Server 2022 evidence includes the earlier
[`928dfcd` isolated run](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/37194199690)
and the later `aecc063` complete run below. Search-option OFF/ON states came from
actual `IExplorerCommand::GetState(FALSE)`; content-only files and ZIP members
changed exact membership, and each original native setting/value/type/absence
was restored. These runs complement local Windows 10 evidence without establishing
identical rendering or arbitrary provider behavior. See
[native advanced search verification](search-options-verification.md).

## UI and performance evidence

The [visual workflow](headless-visual.md) pins online Windows 10 source URLs,
hashes, source crops, native dimensions, masks and strict comparison thresholds.
It captures the real application using PrintWindow and WIC. Every image also
passes independent checks for desktop isolation, actual widget inventory, native
DPI and a painted command band. An intact caption cannot conceal a blank Ribbon.
Source images, differences and private namespace captures are research
intermediates excluded from the repository and release.

Passing a native capture does not mean its pixels match the source. Per-scene
comparison reports retain pixel and geometry failures, publisher scaling,
unrecorded OS state and differing installed-resource revisions. Neither a shared
Windows control nor an unmatched screenshot establishes whole-application parity.
The [theme report](theme-compatibility.md) distinguishes documented APIs,
version-gated native compatibility paths, measured dark surfaces and unmeasured
states.

The [navigation benchmark](performance.md) measures actual native callbacks and
verified item counts over owned 10-, 1,000- and 10,000-file folders. It records
individual samples, cached command updates, view changes and process memory.
The measured application improvements do not establish superiority to stock
Explorer.exe or performance on another machine.

The [feature matrix](feature-matrix.md) distinguishes source bindings, native
delegation and established tests. Hardware, accounts, network/cloud providers,
interactive recipients, elevation, arbitrary preview/extension handlers,
screen-reader operation, mixed monitors and localization need appropriate
controlled fixtures before their behavior can be claimed as verified.

## Retained failures and limits

Original logs, reports, executable hashes and pre-cleanup documentation remain in
ignored `artifacts/`. Documentation cleanup does not turn any earlier failure
into a pass. The former chronological document is preserved byte for byte as
`artifacts/retired-docs/verification-before-v133.md`.

| Earlier evidence | Retained limitation |
| --- | --- |
| v108-r2 strict UI | Share, Computer, Network, Drive, Compressed, Recycle, Modern Home, Modern Share and Modern View failed comparison. Disk Image, Library and Shortcut references were restricted. Reference thresholds and masks are unchanged. |
| v93 / v92 native crashes | Cast enumeration and earlier unrelated Rotate-right planning crashed on those snapshots; later isolated passes do not establish the original causes. |
| v124 / v125 search failures | Original direct-literal equivalence failed in both v124 layouts; authored v125 saved-search Back failed with eleven cascading checks. Separate later passes do not prove the latter cause. |
| v108 / v110 / v113 QAT | Native raw-order assertions failed; v112 failed before Save/Load setup. Later strict envelope passes establish only their own snapshots. |
| v121 pin mock regressions | Original wrong-pin failures remain; direct Ribbon Destroy controls are distinct from App integration or native profile persistence. |
| v66 Preview / v94 Ready | Low-token desktop admission was measured, but actual Preview remained blank in v66. v94 failed source/Ready before pixel capture. Later native rendering passes do not attribute those earlier failures to the desktop label. |
| Earlier clipboard isolation | Shared window-station sequence changed; foreign publication versus delayed rendering was not distinguished. No writer is inferred from sequence alone. |
| Hosted f322 transfer | Paste timed out at its original bound and an unconditional COPY5 final oracle failed. Later fresh-runner passes preserve these negatives. |
| v87 performance | A slow 1,000-item navigation episode remains unresolved until the final source is measured with interleaved fresh-process controls. |

The source inventory is [feature-matrix.md](feature-matrix.md). Visible desktop
operation, real recipients/devices/accounts, mixed-monitor transitions and
third-party extension compatibility are not inferred from native delegation.
