# Headless verification

The Windows 10 reference machine is Home 22H2, build 19045. Remote v180 and v185
checks run on the other PC's Windows 11 build 26200; they do not replace Windows
10 UI or fresh Windows 2022 hosted evidence. Builds use Visual Studio
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

The other PC's v180 authored focused run passes all four targets in 45.58 seconds:
`native_app_shutdown_pin_resolution` (23.47 seconds), App search backing lifetime,
Ribbon and RecentItems. The App target passes all eleven original close cases
and both first-retain controls. Its real native callbacks admit original decoded
values; actual host/Shell resolution runs only after the once-original native
lower returns. Receipt ordering establishes native-call inactive and host-drain
active at each queued host entry. Native S_OK records admission, not the later
host result or profile persistence. Reset, initialize and immediate owner
DestroyWindow retain actual ERROR_RETRY and epoch-tail rejection; App-only
source revocation retains the second actual E_ABORT result. DestroyOwner still
proves actual HWND/WM_NCDESTROY death inside that host callback. Six authored
RecentItems controls pass; their model-only FrameworkDestroy reentry does not
certify the separate direct FrameworkDestroy-origin App/Shell callback path.
Installed App close on Windows 11 and fresh Windows 2022 acceptance are unmeasured.

The v185 dedicated `native_multi_folder_pin_state` target passes in 1.45 seconds
(1.49-second recorded batch): five actual cases, five available native references,
zero host mismatches and zero provider failures. App's nonempty selection binding
now agrees independently with both ordinary `pintohome` and direct public
`Windows.PinToHome` state for one folder, two folders, folder-plus-file,
file-selected-first and one file. The first four are enabled; one-file is disabled.
Both mixed gestures deliver the same actual folder,file array order. Exact original
EnumDone, complete three-member readiness, FileIDs/attributes, selection, CIDA and
view site remain strict. The known-faulting full registered-menu route is not
queried; its explicit blocked diagnostic is not provider success. Native Invokes
remain zero. This establishes Windows 11 state parity for these cases, not actual
Pin persistence, Windows 10 parity or arbitrary provider/array-order coverage.

Both runs record 152 unchanged sources and unchanged executable sets. Their exact
receipts are `remote-v180-close-serialization-and-controls.{meta.json,LastTest.log}`
and `remote-v185-pin-native-applicability.{meta.json,LastTest.log}`. The v185 build
records `src/app_commands.cpp` SHA256
`22962e018055cdf06838dcd77a8ad29945b24e45aa82d9a3a19a09d206718b3e`.
These focused passes do not close the older visual/performance or hosted negatives.
The v177 rejected initialization trial faults all eight original factory-route
targets; it was reverted before v179. Retained matching dump evidence does not
establish the cause or acceptance of the restored routes.

The v186 Windows 11 authored strict visual run requests all nineteen scenes,
records unchanged sources/executables and fails eighteen in 106.34 seconds.
Five primary captures succeed. Only Home, Computer, Network and Drive reach
reference comparison; all four fail. Library retains its genuine reference
restriction. Thirteen capture processes exit with access violations and Search
exits 7. Individual Home tabs and navigation/address/search glyph regions pass,
without a whole-scene pass. The private captures and inventories are retained
locally for diagnosis and are not public-commit inputs. The v187 core target
passes; its authored genuine QAT gestures complete Add/Remove/order and 19/20
transitions, but the actual twenty-first Add yields count 21 and the existing
strict capacity assertion fails. The 23.04-second batch is not a suite pass.
Neither observation erases the older Windows 10 visuals or final performance
comparison gap.

Evidence belongs to the tested source and executable, rather than the latest
working tree. The v165b Release application/theme build is warning-free and
records executable hashes and 151 unchanged source files. The v166 QAT and v167 enumeration-control runs each record unchanged source
and their own immutable executable. The v167 build is warning-free across
152 unchanged source files. Earlier
signedness and SDK-macro build failures were corrected before native checks ran.

The v136 authored and installed General hosts pass in 65.86 and 73.04 seconds.
The focused Search screenshot passes the unchanged reference protocol at
98.24% pixel agreement. These are scoped results, not a complete application
or screenshot-suite pass.

The v143 eight-target run passes six targets: both original App search lifetime
targets, both 130-query resident-cache targets and both original RecentItems
layouts. The retained native cursor is consumed for the first time after close,
before separately rebinding the retained item and PIDL. Neither a fresh cursor
nor an unsupported Reset substitutes for that original cursor. The v160 subset
also passes authored App lifetime, authored 130-query resident cache and installed
App lifetime in 67.89 seconds; installed resident cache is not part of that rerun.

The v157 same-executable App debugger run reaches the original immediate-owner-
destruction case after the first nine cases complete their assertions, including
nested reset, initialize entry, replacement and reentered WM_CLOSE.
Deferring physical Ribbon retirement until its active native close returns
avoids the earlier DirectUI freed-listener fault in those observed cases.
The nested close now records one deferred request and one original native lower
call. The next immediate DestroyWindow case still crashes in native UIRibbon's
ToolWindowMgr while the original close remains active. Dump disassembly identifies
that enum-5 case precisely; buffered console output is not used to infer it.
The v162 same-executable debugger control also retains the exact original
native Ribbon view COM object and still faults in that enum-5 destruction case.
Its debugger exit zero is not a test pass. Physical App cleanup while the original
native close is active was unresolved on that snapshot. Its later pre-first-retain
controls remain unreached there; v180 subsequently passes the original authored
matrix by moving actual host callbacks after the native lower return. The later v168 physical-App-retirement experiment instead
faults in the first original enum-0 ordinary close, before the later cases.
Its CDB run observes DUI70 DirectUI::Element::RemoveListener reading FEEE-poisoned
state in 2.73 seconds; the raw authored App target exits 0xc000041d in
5.80 seconds. Source (152 files) and App executable 158c08…b0a9d remain
unchanged during these runs. The complete production experiment was reversed;
source review did not establish runtime safety. The retained source's v162
enum-5 UIRibbon fault remains retained historical evidence. The v180 queue pass
covers original native WM_CLOSE batches; direct FrameworkDestroy-origin App/Shell
callbacks remain unmeasured. Microsoft requires asynchronous Close/Exit from native Ribbon
callbacks; retaining native COM objects alone does not establish safe immediate
child-window destruction. No profile pins are changed by these tests.
[Microsoft Ribbon migration contract](https://learn.microsoft.com/en-us/windows/win32/windowsribbon/ribbon-migration).

Both original strict QAT preservation/settings controls pass in v152. Both v155
gesture targets skip before opening a menu with ERROR_NOT_SUPPORTED at the
context-action gate. Their actual Cut buttons supply empty UIA runtime-ID arrays.
Two fresh rooted walks establish the same unique path and actual native Legacy
receiver, role, name, bounds and state despite distinct COM wrapper identities.
That native bridge passes without promoting empty runtime IDs or wrapper
pointers into identity. The v158 actual right-clicks then open genuine Add menus
but reject popup ownership before Invoke. The v163 scoped original-popup authority
permits genuine Add Cut/Paste, Remove, remove/readd order, 19-to-20 Add and
20-to-19 Remove in both layouts. Both targets still fail the final assumption
that the twenty-first Add row must be disabled; the actual row is enabled and
no Invoke occurs in that case.

The v166 capacity observation accepts the actual enabled row and invokes it once
through the same native/UIA/Legacy/source fences. Both raw native Invoke results
are S_OK, before the unchanged gesture deadline, with completed worker retirement.
The independent actual native collection then reports more than twenty items and
both strict targets fail (authored 8.60 seconds; installed 7.78 seconds). No count
is truncated, row removed or HRESULT normalized to obtain acceptance. Earlier
ordinary Add/Remove/order checks remain successful, but neither full gesture
target passes its capacity check. Microsoft documents up to twenty QAT commands;
it does not promise a disabled twenty-first context-menu row.
[QAT contract](https://learn.microsoft.com/en-us/windows/win32/windowsribbon/windowsribbon-controls-quickaccesstoolbar).

The v152 seven-target control run passes namespace actions, both original QAT
controls, both original Ribbon layouts and both original RecentItems layouts
in 56.96 seconds. Source and the monitored application executable remain
unchanged throughout that run. The opt-in Hosted persistence fixture now has
genuine disposable-runner negative evidence at commit 566ce07 below. The v164
revision using an actual native setup pin followed by a fresh App's unpin callback
compiles without warnings; it has no new Hosted runtime evidence. That revision
explicitly leaves App pin persistence unmeasured. Default registration is disabled.
Both original RecentItems layouts pass again in v164 in 20.43 seconds.

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
The v160 Auto and Light oracle controls complete in 10.24 seconds. Corrected
top-down raw four-byte raster comparisons match three actual CommandStore
fallback specifications at both sizes, including combined versus separate native
extraction. Missing first images remain genuine native unavailability; these
fallback comparisons do not identify stock Explorer's host artwork. The v164
single public Light-highlight request is accepted independently by the actual
framework, while its diagnostic explicitly claims no Explorer palette parity.
Both original theme targets pass on the v165 experimental snapshot in 2.45 seconds.
Its actual File-interior diagnostic changes zero pixels. The ineffective public
highlight production experiment was subsequently reverted; these receipts
belong to experimental executable 92be03…, not the later restored source.

| Snapshot / scope | Actual result | Remaining implication |
| --- | --- | --- |
| Remote Windows 11 v187 core / authored QAT | Core passed; authored QAT failed; 23.04-second batch | Genuine Add/Remove/order and 19/20 transitions complete; actual twenty-first Add yields native count 21, failing the unchanged capacity assertion. |
| Remote Windows 11 v186 strict visuals | Nineteen scenes requested; eighteen failed; 106.34 seconds | Five primary captures; four comparisons, all failed; Library restricted. Thirteen access-violation capture exits and Search exit 7. Individual region passes do not establish whole-scene parity. Private source captures stay out of public commits. |
| Remote Windows 11 v185 Pin state | Five cases/native references passed; zero mismatches/provider failures; 1.45-second target | Both independent native states match App on full original selections. Mixed gestures both deliver folder,file order; one-file is disabled. No provider Invoke/persistence; full registered-menu route remains blocked. |
| Remote Windows 11 v180 authored close / controls | Four targets passed; 45.58 seconds; eleven original App close cases plus two first-retain controls | Actual queued host entry follows once-original native lower return. Six authored RecentItems controls pass. Direct FrameworkDestroy-origin App/Shell path, installed Windows 11 App close and fresh Windows 2022 are unmeasured. |
| Remote Windows 11 v177 rejected initialization trial | All eight original factory-route targets faulted; 44.21 seconds | Namespace actions, App commands, four menu partitions, native selection and hidden Shell host remain retained negatives. Trial reverted before v179; dump does not prove cause or restored-route acceptance. |
| Local v168 physical App retirement trial | First enum-0 close faults; CDB 2.73 seconds; raw authored target 0xc000041d / 5.80 seconds | Production trial completely reversed; later original cases are unmeasured in this trial. The retained v162 fault belongs to its original snapshot; v180 later passes the authored WM_CLOSE queue matrix. |
| Local v167 original enumeration controls | Four passed; 59.58 seconds | Namespace: 22 groups / 2,669 assertions; App commands: 18 groups / 3,664 assertions; both registered-menu partitions pass with genuine original creator EnumDone receipts. Fresh Hosted rerun is still required. |
| Local v166 genuine QAT capacity | Both targets failed; authored 8.60 seconds, installed 7.78 seconds | Actual enabled twenty-first Add invokes once and returns raw S_OK before deadline; actual collection exceeds twenty. Previous genuine Add/Remove/order checks complete, but strict capacity acceptance fails. |
| Local v165 experimental theme / complete visuals | Both theme targets passed, 2.45 seconds; all nineteen captures passed; four comparisons passed, twelve failed, three references restricted | Modern chrome remains below the unchanged 98% threshold. Accepted public highlight changes zero File-interior pixels; production experiment subsequently reverted. |
| Local v164 RecentItems | Both layouts passed; 20.43 seconds | Four original cases and genuine shutdown revocation controls; Hosted profile persistence is a separate unverified revision. |
| Local v163 genuine QAT gestures | Both failed; 20.26 seconds | Ordinary Add/Remove/order and 19/20 transitions work; actual enabled capacity row violates the fixture's disabled-row assumption, before any capacity Invoke. |
| Local v162 exact native-view close lease | Debugger observed the enum-5 native access violation | Holding the exact native Ribbon COM view does not resolve immediate owner-window destruction; debugger exit zero is not acceptance. |
| Local v161 search / theme controls | Five passed, one appropriately skipped; 13.41 seconds | Search lifetime, presentation, native result policy and both theme controls pass. Native search-options profile mutation is correctly skipped locally. |
| Local v160 App search / oracle subsets | Three App targets passed, 67.89 seconds; two diagnostic controls passed, 10.24 seconds | Scoped App lifetime/resident checks and Auto/Light oracle cleanup, not full App close or visual parity. |
| Hosted commit 566ce07 ordinary jobs | Each main job: 31 passed, three failed, three skipped out of 37 | Namespace selection preservation and both registered-menu partitions fail. Separate push transfer/drop pass; PR transfer fails and drop passes. |
| Hosted commit 566ce07 owned-profile jobs | Both failed; normal 2.50 seconds, final-only 1.95 seconds | Actual GUID folder is absent from native App Home; zero Invokes/callback returns. Complete original four-pin inventory is restored. |
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

The newer commit 566ce07 [push](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/37465670548)
and [PR](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/37465676548)
main jobs each record 37 cases: 31 passed, three failed and three skipped. The
mandatory App 130-query resident-cache target and strict QAT preservation/settings
controls pass in both. Namespace selection disappears after the first actual
Kind worker completes; both registered-menu partitions retain three real
selection-command state differences. These observations establish failure
boundaries, not causes. The v167 local enumeration revision then passes the
four original namespace/App-command/registered-menu targets in 59.58 seconds
with actual EnumDone/creator callback receipts and unchanged source/executable.
This local pass does not resolve the hosted negatives until a fresh Hosted run.
Separate push transfer/drop pass; PR transfer enters
invokeCommandStore without returning before its original twenty-second deadline,
while PR drop passes. No timeout extension or selection restoration is acceptance.

The 566ce07 [normal](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/37465713960)
and [final-only](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/37465718199)
owned-profile jobs pass fresh admission, warning-free build and immutable source/
executable checks, then fail actual native Home admission. SHAddToRecentDocs does
not produce the owned GUID row. Both record zero native Invokes and zero normal/
final callback returns, then prove the complete original four-pin inventory
restored after App/OLE teardown and exact owned-folder cleanup. Successful
restoration does not establish App pin persistence.

The latest complete strict screenshot run is experimental v165: all nineteen
native captures pass, four comparisons pass, twelve fail and three references
remain restricted. Picture, Application, Music and Video pass. Home, Share, View,
Computer, Network, Drive, Compressed, Search, Recycle and all three modern scenes
fail. DiskImage, Library and Shortcut remain restricted. No crash dump is
produced. Experimental executable 92be03… includes the later-reverted Light
highlight request; these results are not acceptance of the restored source.
The previous v134 run retains eighteen captures, six comparisons passed, nine
failed, three restricted references and one Search capture failure. The later
focused Search pass does not erase v165's new Search comparison failure. A
fresh v167 focused Search run after Theme rollback passes again at
0.9824210375875375 agreement, mean RGB error 0.9870261858256714 and edge F1
0.9888858613893967 on restored-source executable fd7cc…2dadda1. This is a
scoped comparison, not a new complete screenshot-suite pass. The later one-row
caret change has been adopted but has no native verification yet. Fresh v143 modern
captures use the corrected 512-row window bound; all three still fail chrome
at 97.50%, 97.38% and 97.39% pixel agreement against the unchanged 98% threshold.
Their separate dropdown and Refresh regions pass. The later v165 experimental
Light-highlight run again passes all three native captures but fails all three
chrome comparisons: 97.577610%, 97.464068% and 97.460900% agreement. Dropdown and
Refresh agreement remains 100%; the original 98% chrome threshold is unchanged.
Exact scene results are in [headless-visual.md](headless-visual.md).
A successful native-current image check cannot replace a new complete screenshot
comparison. The final 24-process interleaved performance comparison has not run;
the earlier slow 1,000-item navigation episode remains unresolved.

Search backing now bounds cache-owned records at 128 without a lifetime query
quota. Cache eviction and App close preserve published descriptor paths; they
do not claim a last-consumer census or transparently reclaim those files. The
v143 stress and delayed-native-use checks pass at their recorded snapshot. Full
native QAT gesture capacity, actual App pin persistence, noncurrent-DPI
production image callbacks, arbitrary
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
