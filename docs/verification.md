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

The latest scoped General runs pass authored 298/298 checks in v126 (64.52
seconds) and installed 303/303 in v125 (74.17 seconds). The equivalent-query
fixture now derives local rules from the original actual scope array when
metadata rules are implicit; all original result, history and presentation
assertions remain. The v126 change adds read-only Back-transition diagnostics.
Its successful transition records exact completed/history aliases (0/1/1
factory/completed/history byte matches), all three rules and original members.
The v125 authored run still failed eleven checks starting at one saved-search
Back transition; its cause is unresolved and the negative is retained in
`artifacts/continuation-app-general-v125.LastTest.log`. These separate passing
processes do not establish repeatability or a new all-target pass. Receipts are
`artifacts/continuation-app-general-authored-v126.meta.json`,
`artifacts/continuation-app-general-v125.meta.json`, and their XML/logs.

The preceding v124 six-target full App/search run fails two General targets
in 236.54 seconds, with four passes and no skips. Authored General passes
297/298 checks in 64.14 seconds; installed General passes 302/303 in 70.25
(CTest process timings). Each fails only
`live_search_equivalent_direct_literal_commits_without_queued_native_navigation`.
Both original Content handoff and strict nonzero LTR/RTL footer resize checks
now pass. The backed-search targets pass authored/installed in 6.90/8.55
seconds; dedicated Library targets pass in 44.62/42.06. All 136 recorded source
files and the recorded App executable stay unchanged. Application SHA-256 is
`1241e1d451386210e6cee612d1693b738f49a9d6001fd936895b3357213f6d0b`.
Evidence is `artifacts/continuation-app-search-full-v124.LastTest.log`, its
XML/meta and full process log. A narrow v125 comparison-fixture correction uses
the original actual scopes and recursion when preserved explicit rules are
empty; every original assertion and deadline remains. Its WindowsExplorer
build completes without warnings with 136 unchanged recorded source files
(`artifacts/continuation-build-v125-equivalent-scope.meta.json`). The serial
authored/installed General rerun is superseded by the scoped results above.
This is not a complete App pass, new complete local suite or whole hosted
workflow. The latest complete local
all-target pass remains v102-r2, with thirty active targets passed and five
disposable-profile targets skipped in 567.41 seconds. Current source changes,
the final alternating 24-run performance A/B and whole-application functional
and visual parity remain unverified beyond the scopes stated here.

The v124 four-target QAT/RecentItems run passes with zero failures or skips in
25.15 seconds. QAT preservation passes both actual layouts with 5,158 assertions
in 1.88 seconds; the owned settings-envelope control passes both layouts with
10,682 assertions in 2.23. Authored/installed RecentItems pass in 10.48/10.55
seconds: all four original native cases and both actual Destroy-delivered
reentry cases pass in each layout. The callback captures the original owned
rows and permits one final pin transaction during synchronous Destroy;
creator/window/atomic-entry-epoch fences revoke remaining writes after nested
reset or a rejected initialize entry. Successful replacement-framework
initialization during Destroy is not exercised. The genuine v121 wrong-mock-pin
failures in both layouts remain negatives; this later pass does not erase them.
Evidence is `artifacts/continuation-recent-qat-v124.LastTest.log`, its XML/meta
receipt and `artifacts/continuation-ribbon-qat-v121.xml`. The immutable v124
test executable SHA-256 is
`bb6a90c827631b028d4cd5967ea4dbfe25b00d32dd4f8717c6c8a39327853799`;
source and the recorded executable are unchanged during that run. The separate
v124 build receipt records QAT executable SHA-256
`3f07809b29bafe2a87a1373e6d124696c6353c6c2d34d7227d3edbb3cfbb13ef`.
This direct Ribbon fixture does not establish App close-time pin integration:
the source audit finds the App sets `closing_` before reset and its pin callback
then returns E_ABORT. That is a source-proven integration gap, not an executed
personal-pin mutation. Native stale-row activation after catalog replacement
also remains unproved (`artifacts/native-item-binding-audit-source/REPORT.md`).

The earlier v119-r2 production QAT fixture already passes both layouts with
5,153 assertions and the envelope control with 10,682. Native settings bytes
stay opaque; an owned checksummed envelope records layout and at most twenty
ordered native command IDs, then reorders the actual loaded whole rows with
source/reentry checks. Corruption-before-mutation, exact loaded ID-set/count,
rollback, atomic replacement failure, legacy streams and retired-generation
cases pass. Legacy raw streams still follow native ordering; imported opaque
separators and context-menu customization gestures are not demonstrated.
Raw diagnostic controls were retired into ignored source after this product
pass; the strict `runLayout` source remains byte-identical with SHA-256
`fd6cda6829d2437f1e27bfaa6e3f19ad9542bbf22014087a761139c6db3a4a8f`.
Historical raw-order and presentation negatives remain retained. Evidence is
`artifacts/continuation-qat-v119-r2.LastTest.log`, its XML/meta receipt and
`artifacts/qat-settings-order-source/retire-raw-controls-v121.json`.

The v120 real App backed-search targets pass authored/installed in 5.94/7.84
seconds, with no failures or skips, actual requested layouts and unchanged
source/executable. Query/refinement/history, view recreation, child handoff,
native-reader/reentry and close retain exact owned membership and full FileIDs.
The fixture verifies search-context and physical identities separately through
native canonical comparisons; differing raw PIDL bytes are not an identity
failure. Evidence is `artifacts/continuation-app-search-installed-v120.LastTest.log`
and its XML/meta receipt. The v116-r2 store target independently passes its four
owned groups in 1.94 seconds, including all 128 descriptors, actual writer
FileID, native replacement/reentry and same-FileID rewrite with refused unsafe
cleanup (`artifacts/continuation-search-backing-v116-r2.*`). The 128 limit still
counts lifetime-total reservations; obsolete searches do not reclaim capacity.
The ignored `artifacts/search-app-result-identity-source/cap-lifetime-design/report.md`
is a source-only design behind a native-consumer lifetime-proof gate, not an
implemented or tested removal of that limit.

The v123 four-target owned-selection/menu run passes in 14.60 seconds:
22/22 namespace groups and 2,732 assertions in 6.70 seconds, registered
10,000-file and mixed menu state in 2.06/4.93, and actual Video Cast state in
0.90. The selection helper uses one native IFolderView SelectItem at a verified
native index, with canonical child/full identity, exact original volume/128-bit
FileID and original 2/5-second budgets. No provider state or original selection
assertion is waived. Evidence is `artifacts/continuation-owned-selection-menu-v123.LastTest.log`
and its XML/meta receipt. These local passes do not certify the pending hosted
counterpart of the later source changes.

The draft [pull request](https://github.com/ProtonDev-sys/windows-explorer/pull/1)
still has a failing latest hosted main job at e7d5d1 in
[run 37426164366](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/37426164366):
30 tests, five failures and two skips. Namespace/Video selection, both registered
menu targets and the authored App footer target fail. Its separate genuine
hosted transfer and drops processes pass in 4.94/0.58 seconds with no skips.
Evidence is `artifacts/ci-e7d5d1-37426164366/` and its build-job log. The registered
menu diagnostics locate the first observed all-count change from three to zero
across the unrestricted leaf-state query wrapper, before NORESOURCE creation;
the enclosed cause is unproved. Canonical owned-alias readback corrections add
actual COM calls and may change timing; they are diagnostics, not a demonstrated
production fix. The f322 Paste timeout and unconditional COPY5 final-oracle
failure remain genuine negatives.

The v122 authored full App run fails only its Content search handoff while its
new strict LTR/RTL footer shrink, drag, OFF/no-op/resize/restore and teardown
checks pass. The actual owned root shrinks from 1200x800 to 1160x780 in both
directions; the shrunk native frame measures exactly 1144 pixels wide. Evidence is
`artifacts/headless-smoke-v122-authored.json`,
`artifacts/continuation-footer-shrink-v122-authored.xml` and its meta receipt.
The full App failure is preserved. Source review finds normalized implicit
scope rules were stored as explicit metadata; the correction preserves original
implicit metadata and uses normalized rules only for execution. The later
v124 run passes both original Content handoff checks and both-layout footer
shrink/drag/teardown checks, but fails the direct-literal equivalence check in
both General layouts as recorded above. The remaining correction is pending.

The latest strict visual evidence remains v108-r2: nineteen native captures,
seven strict comparisons passed, nine failed and three restricted references.
Video passes on that exact binary. The installed-image contract audit is source
only: production image replacement policy is visible in code, but actual first
native-current image availability and displaced artwork are unknown. No new
image or screenshot pass is claimed. The retired isolated Preview prototype is
preserved in ignored source and is not an active passing substitute for the real
App. The old slow 1,000-item navigation result remains unresolved; a pending
final performance A/B does not dismiss it.

### Retained earlier checkpoints

The warning-free v107 build completes in 17.91 seconds with all 138 recorded
source files unchanged during compilation. Its four serial App processes pass:
authored General 298/298 checks in 64.44 seconds, installed General 303/303 in
70.46, authored Library 17/17 in 41.38, and installed Library 20/20 in 41.76.
General keeps its 150-second limit and Library its separate 90-second limit.
All reports are fresh, identify their exact scope/layout, and confirm the
unchanged input desktop with no visible input-desktop windows. Application
SHA-256 is
`373086bc7274bf97bedaa376ad0c50d54d5e1708e1e43858b1ad55706dfc33cb`.
Evidence is `artifacts/continuation-build-v107.meta.json`,
`artifacts/continuation-app-v107-authored.xml`,
`artifacts/continuation-app-v107-installed.xml`,
`artifacts/headless-smoke-v107-authored.json`,
`artifacts/headless-smoke-v107-installed.json`,
`artifacts/continuation-library-v107.xml`, its process receipt, and the two
`artifacts/v107-headless-smoke-library*.json` reports.

At v107, both General layouts validate the production full-width native footer,
exact content/pane partition, actual owned splitter-grip hit, Preview and
Details transitions, search-driven native view recreation, root resizing and
LTR/RTL layout at the measured native 96 DPI. Both original RTF sources pass native content/pixel checks. Four production
focus-transition checks use the accepted source B in each layout, preserving
source identities/bytes, selection, history, keyboard state and isolation.
The three v106 authored failures remain in their original report: the fixture
applied an effective-visibility requirement to an intentionally hidden root.
The narrowly reviewed v107 oracle checks each child's own WS_VISIBLE and
matches effective visibility to the root; production geometry, actual hit,
source/content/pixel assertions and deadlines were not weakened. Evidence is
`artifacts/headless-smoke-v106-authored.json` and both v107 reports above.
These four App passes are not a new complete thirty-target suite. The latest
complete local all-target pass remains v102-r2 below.

The later targeted v111 core run passes in 12.21 seconds. It includes the new
native known-folder depth/mixed-descriptor group and all ten native Search
groups: exact known-folder identities and recursion survive save/import/re-save,
and owned mixed-scope filesystem results retain full FileIDs. This is actual
proof of the new descriptor group, unlike v109's earlier saved-search metadata
run. It does not establish arbitrary virtual descendant membership or complete
the App's then-unverified shallow/mixed-scope backing lifetime; the later
v120 targets above establish the stated backed-search cases. The five Search source
files are committed locally as `bc4f1ee`; that commit is not yet pushed at this
checkpoint. Evidence is `artifacts/continuation-core-v111.LastTest.log` (the new
group at line 51), its JUnit and immutable process receipt.

Four separate v111 readiness targets pass in 12.44 seconds: native namespace
actions, files/registered and mixed/registered 10,000-item menu state, and Video
Cast state. Exact current native view/folder and complete owned membership are
proved before provider reads; no predicate, selection or expected menu state is
waived. Evidence is `artifacts/continuation-readiness-v111.xml`, its full/native
logs and process receipt. These targeted processes do not replace the latest
complete all-target pass. Their readiness source is pushed in `212732e`.

Four genuine disposable-hosted positive checks are separately established:
native Shell Undo/Redo, native search-option transitions/restoration, native
view persistence, and independent actual COPY/MOVE/LINK drops. The first three
are actual run-status passes in
`artifacts/ci-pr1-37411596266-headless/core-tests.xml`, corroborated by its
`test-environment.json`; drops pass the separate
`artifacts/ci-pr1-37411596266-transfer/native-drops.xml` with zero failures or
skips. These are receipts for the 0de5917 pull-request merge in
[run 37411596266](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/37411596266),
not a passing whole hosted workflow. The later e711 build log also records the
three main-job positives; its drops entry is skipped and is not a substitute
for the dedicated positive process.

The isolated COPY-preference-five experiment
[run 37414307684](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/37414307684)
creates and verifies the actual Paste Shortcut output, exact target and owned
source/clipboard cleanup. Its whole transfer case still fails the retained
generic-folder-menu presence assertion: both original native Copy and helper
publication observe zero matching generic leaves, while the actual view and
registered leaf are enabled. Output verification is a factual intermediate
receipt, not a case pass. Evidence is
`artifacts/clipboard-source/hosted-experiment-result.json` and
`artifacts/continuation-ci-copy-five-experiment.log`. The f322e9e production
change conditionally retains LINK with COPY only after exact whole-array
S_OK/SFGAO_CANLINK authority, keeps unknown Copy at COPY and Cut at MOVE, and
compares the generic oracle with the original native Copy control. Its fresh
hosted validation in
[run 37416577757](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/37416577757)
fails at the first normal registered Paste invocation after publishing the
link-capable Copy object. The complete producer/consumer CIDA and preferred
effect checks pass, as do the three controlled aggregate-capability cases;
the Paste call exceeds its unchanged 20-second native-call bound. Independent
native drops pass. Evidence is
`artifacts/continuation-ci-f322-transfer-job.log`; that negative receipt does not establish a full transfer pass.
The main job records 24 passes, four failures and two explicit transfer skips.
Namespace/Cast readiness rejects the native display-path string, while the
registered-menu diagnostics show an independently queried unrestricted menu
has the same three changed selection-command states. These negative receipts
are retained in `artifacts/continuation-ci-f322-build-job.log`.

The later dedicated disposable-hosted job for `212732e` succeeds in
[run 37419817884](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/37419817884).
Its downloaded JUnit records an actual `native_shell_transfer` pass in 6.2265
seconds and independent `native_shell_drops` pass in 0.725974, each with run
status, zero failures and zero skips. The whole workflow's main suite is still
in progress at this receipt checkpoint; the dedicated job is not a whole-workflow
pass. Evidence is `artifacts/clipboard-followup-37419817884/reports/native-transfer.xml`,
`native-drops.xml` and the retained job log. A separate push job at the same
commit also passes transfer in 5.44889 seconds and drops in 0.635548, each with
zero failures/skips, in
[run 37419813412](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/37419813412).
Its separate reports are under `artifacts/clipboard-followup-37419813412/reports`.
Successful JUnit stdout is truncated; it proves completion of the strict source
case, without exposing every numeric phase. The compared native-transfer source
inputs are Git-blob identical between f322 and 212732e, as recorded in
`artifacts/clipboard-source-comparison-f322.json`; this is not binary identity.
The earlier f322 Paste timeout remains a genuine negative. These different runs
do not establish a deterministic cause or justify an async-mode mutation.

An independent original-view Copy5-to-normal-Paste control also passes in
[run 37418936178](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/37418936178)
at diagnostic commit `65a81`, in 2.89329 seconds. That successful JUnit truncates
stdout before the optional async readbacks. A verbose rerun of the unchanged
native fixture at `d8f92ed`,
[run 37419756224](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/37419756224),
passes in 3.66787 seconds. Its retained LastTest log records nonnull async
interfaces, S_OK/GetAsyncMode false and S_OK/InOperation false for the original
view selection, unpublished array-bound helper and native clipboard consumer.
The native Copy publishes effect 5 with two exact source identities; the same
registered normal Paste completes and its exact outputs and owned cleanup pass.
These observations do not diagnose the earlier helper timeout. Evidence is
`artifacts/continuation-ci-native-copy-paste-control-37418936178-job.log` and
`artifacts/clipboard-followup-37419756224/reports/build/Testing/Temporary/LastTest.log`.

The QAT collection-preservation and twenty-command chooser-capacity changes
compile without warnings in v108-r2. The core/capacity checks pass in 6.10
seconds. The new native QAT regression fails in 0.56 seconds during the
twenty-command settings roundtrip: targeted Add/Remove/Move, exact retained
objects, stale-state rejection and the actual twenty-command limit pass before
the native reload changes command order. This is not a passing persistence
case. Evidence is `artifacts/continuation-build-v108-r2.meta.json`,
`artifacts/continuation-core-v108-r2.xml` and
`artifacts/continuation-qat-v108-r2.native.log`. The temporary layout experiment
has been removed from the source and build; its archived source and earlier
negative/positive receipts remain in ignored artifacts.
The later v110 hidden native persistence control completes all six fresh
framework arms with 20,526 assertions: authored and installed factory-two direct,
retained-twenty direct, and retained-twenty wrapper Save/Load. Every arm fails
exact restored command order while command readability, enabled state and dock
pass. This reproduces an actual native reload failure beyond the production
file wrapper; it does not establish a hidden-window cause. Evidence is
`artifacts/continuation-qat-control-v110.native.log`, its JUnit and process receipt.
The v112 presented control runs all six arms but fails its startup-show guard
before Save/Load (195 assertions), so it provides no persistence ordering result.
Evidence is `artifacts/continuation-qat-presented-v112.native.log` and its JUnit.
The corrected v113 SetWindowPos/nonactivation control completes all six rendered
arms in 2.64 seconds with 27,593 assertions. Actual owned native command bars
are visible and have positive geometry, while thread input/focus, private/input
desktop, command readability/state and docking remain intact. All six restored
command orders still fail. Presentation does not cure the measured order loss.
The old controls and original 30/60-second bounds remain unchanged. Evidence is
`artifacts/continuation-qat-presented-v113.LastTest.log`, its JUnit and immutable
source/executable receipt. At that checkpoint the source-only persistence correction had no runtime
result; later v119-r2 and v124 receipts above establish its exact tested scope.

Final alternating 24-run performance A/B remains pending. The fresh v108-r2
installed visual run verifies all 24 pinned online references and passes all
nineteen native captures. Seven strict comparisons pass, nine fail and three
scenes retain source restrictions, with unchanged masks and thresholds. Video
now passes its strict comparison. Evidence is
`artifacts/visual/run-20261006-052127-3809128d3528448588964474186ea939/summary.json`
and `artifacts/continuation-visual-v108-r2.meta.json`; functional footer checks
do not turn the remaining strict failures into passes.

Historically, the v101 App passes all four serial processes in 214.23 seconds: authored
General 288 checks/67.91 seconds, authored Library 17/39.06, installed General
293/68.42, and installed Library 20/38.82. All four JSON reports are fresh,
identify their exact General/Library scope and native layout, and confirm
unchanged input desktop with no visible input-desktop windows. General retains
its 150-second limit; Library has its own 90-second limit. All original Library
checks remain, with additional full FileID, Unicode-member and history guards.
Both layouts also pass four actual Preview focus transitions under neutral and
held Shift states, and both LTR/RTL splitter grips, limits and capture-loss cases.
Evidence is `artifacts/continuation-app-v101.xml`, its immutable four reports
and `artifacts/continuation-app-v101-summary.json`. App SHA-256 is
`27d4c2ce76537924d3eb3d9fd38dc7534fb12d11acd418467c0dd3f2519b8ec5`.

The warning-free v102-r2 full build removes an unread startup getter and unused
per-update diagnostic copies while preserving all accumulated benchmark
receipts. Its independently bounded namespace and Cast processes pass in 6.38
seconds: 21 namespace groups/2,494 assertions and the strict Cast comparison.
With the original synchronous query flags, all three registered image-handler
owner/site controls expose native parent 64 and its submenu; the owned-site
exact branch query succeeds. Independent audio/video handlers and production
now agree on the enabled parent, submenu and original whole-array facts.
No artificial-root popup notification or command invocation occurs. This
corrects the earlier absent-parent result caused by different query flags.
Evidence is `artifacts/continuation-cast-namespace-v102-r2.native.log` and JUnit.
The complete v102-r2 suite finishes in 567.41 seconds: thirty actual passes,
zero failures and five explicit disposable-profile skips. All four smoke reports
and JUnit are fresh, and every tested executable's digest remains unchanged.
The strengthened report runner accepts the exact General/Library scopes and
rejects stale results. Evidence is
`artifacts/continuation-full-v102-core-tests.xml`, its full native log,
`artifacts/continuation-full-v102-test-environment.json` and final process receipt.
Application SHA-256 is
`f5e2c386d700be378744e41379a85b11206ebe4da60df72054686d431d9bcd1a`.
That local snapshot skips native history, search options, view persistence,
transfers and drops; skips are not feature passes. The later hosted receipts
above establish four separate positives. Current full-transfer validation
remains distinct.

The separate v102-r7 reapply-layout experiment exits zero with actual LTR/RTL
A/B/A navigation, twelve post-SetRect public-view crops and exact selection,
focus, flags, sort, membership and source preservation. An independently
measured native frame client establishes the footer span and border insets;
the footer remains full width through actual root resizing. Earlier controlled
layout failures remain intact in their own modes. At this historical checkpoint
it is an observed compatibility experiment, not a production footer fix. The
later v107 App results above establish the production geometry checks. Evidence is
`artifacts/continuation-layout-v102-r7.stdout.log` and its process receipt.

The warning-free v99 core-only build runs twelve targets serially in 305.59
seconds: eleven pass and the dedicated Cast check fails without an access
violation. All four 10,000-item menu partitions pass (files/native 40.11,
files/registered 2.56, mixed/native 40.01, mixed/registered 1.70 seconds).
Their union retains every mixture and registration case from the original
single stress target. The Cast check fails because its query omits the original
synchronous-cascade flag and yields no retained parent. The v102 result above
validates the narrowly restored isolated-handler flags and production state.
This v99 checkpoint is not a complete application run.
Evidence is `artifacts/continuation-core-native-v99.xml` and its log; immutable
core SHA-256 is
`3a3663015d47c091ffc545d0c53f36f86d81357d4a7cbd2c58b73d15368814de`.

The separate v99-r5 layout observation control completes with exit zero and
preserves all owned sources. On the initial LTR and RTL views, the focused
item changes from absent to row zero after message pumping despite zero
explicit layout, activation or selection calls and zero observed position
messages. Folder, membership, selection, flags, view mode, icon size, sort and
geometry remain identical. Later B/A views retain focus. This establishes an
initialization effect independently of resizing; it does not certify layout
compatibility. The report explicitly retains `layoutCompatible: null` and
`productionLayoutPromise: 0`. Evidence is
`artifacts/continuation-layout-v99-r5.stdout.log` and its process receipt.

The warning-free v98 Release build passes all 288 authored App checks in
96.360 seconds (96.83 seconds including CTest). Both actual native RTF sources
reach Ready through Stream initialization, retain their exact FileIDs and
contents, expose the expected UIA text and produce different captured pixels.
The first source's Unload, site-clear and window-destroy receipts are all
`S_OK`, separately from the second source's successful rendering. The input
desktop is unchanged and no application window appears there. Evidence is
`artifacts/continuation-headless-smoke-v98.json` and
`artifacts/continuation-app-v98-authored.xml`; immutable application SHA-256 is
`506844da3212c84af9cb12f9961ccc8002e0962726c64f32fadc014d4154554b`.
The one fewer check reflects the automatic reference diagnostic being
conditional on an actual Preview failure; no assertion was removed.

The same v98 build passes the four separately bounded core targets in 130.90
seconds: core operations 6.35 seconds, native SavedSearch metadata 35.44,
native Library operations 81.36 and namespace actions 7.74. Every original
assertion remains; the process limits are 120/60/90/45 seconds respectively.
The namespace group still truthfully reports that the installed image Cast
provider exposes no enabled subgroup. Evidence is
`artifacts/continuation-core-parts-v98.xml` and its log; immutable core executable
SHA-256 is `b2611dab8778f97540942393e6506a81d6b90657c5f2cd680234b1d9fbd9f54f`.
These five passing targets are not a complete current suite.

The v98 installed App logs 295 passing checks and zero failed checks but times
out at its unchanged 150-second process limit before final completion. No fresh
smoke JSON is produced, so it is not a passing test. The largest measured gap
is 66.687 seconds before `library_context_in_hidden_host`; the last check occurs
at 148.907 seconds. The exact timeout remains in
`artifacts/continuation-app-v98-installed.xml` and its log. The derived
`artifacts/continuation-app-v98-installed-log-events.json` is timing evidence,
not a replacement success report.

The remaining v98 targets finish in 280.19 seconds: seventeen pass, two fail
and five disposable-CI-only checks skip. Both authored and installed Ribbon,
recent items, theme, native App commands, selection, focus keyboard, search
handoff, saved-search presentation, live search, visual capture and crash
diagnostics pass. The combined 10,000-item menu-state target times out at its
unchanged 90-second limit. The dedicated Cast fixture crashes after 3.11 seconds
while populating its artificial root menu. Evidence is
`artifacts/continuation-remaining-v98.xml` and its log. Across the complete
thirty-target v98 inventory this is 22 passes, three failures and five skips.

The retained headless CDB run reproduces that Cast access violation in
`playtomenu!CPlayToMenu::_GetDefaultDeviceIcon`, reached through an artificial
root `WM_INITMENUPOPUP`; the execute target is the legacy Common Controls
module base. Both Common Controls versions are loaded. This stack does not
prove the private provider's cause or the original parent state. The corrected
source reads the independent native parent before callbacks, skips disabled
parents and populates only an enabled actual subtree. Test hosts now declare
App's Common Controls 6 dependency, preserving their existing DPI/OS/long-path
settings. Four separately bounded stress processes retain the exact original
mixture/provider coverage and every assertion. These changes require their
own runtime evidence; the v98 failures remain in
`artifacts/continuation-cast-v98-debugger.log` and its diagnostic metadata.

Both real Preview sources reject optional `IPreviewHandlerVisuals` with
`E_NOINTERFACE`; no background/text/font method is attempted. Rendering passes,
but this does not establish native visual-method coverage. The public browser
and Shell view HWND diagnostics are source-fenced; `FCW_STATUS` returns
`E_NOTIMPL` and no HWND. At that v98 checkpoint the pane overlaps the footer's
23-pixel vertical extent. Focus and splitter/layout changes are then source-only;
the later v101 and v107 results above separately establish their runtime checks.
Complete cross-version/DPI and reference-pixel parity remains unresolved.

The v96 authored App completes 289 checks with one failure in 103.563 seconds
(104.359 seconds including CTest), with the same Sort/Group/Kind passes. Its
preserved primary Preview receipt identifies Source stage, raw `S_FALSE`, no
initialization or cleanup calls, and a final `E_FAIL`; no foreign content or
pixel read is admitted. The source rejects the successful non-folder
`IShellItem::GetAttributes(SFGAO_FOLDER)` mismatch documented by Microsoft.
The narrowly reviewed correction accepts exactly `S_OK` or `S_FALSE` for this
attribute read; the later v98 run above validates the actual handler. Evidence is
`artifacts/continuation-headless-smoke-v96.json` and
`artifacts/continuation-app-v96-authored.xml`; immutable executable SHA-256 is
`5aa8c237ea331af218d5caa801fbf483b3360b9e30e17ce248a6759dee57564a`.
The build is warning-free and includes width persistence, optional visual
suggestions and separate primary/cleanup receipts. These features do not turn
the failed Preview check into a pass.

The v97 namespace-only process passes all 21 groups and 2,516 assertions, exits
zero in 6.103 seconds with no watchdog stop, and retains exact source/view/CIDA
fences. The actual registered image Cast provider reports zero commands in all
three no-owner/owned-owner/owned-site configurations; the fixture validates
native absence and explicitly leaves enabled-subgroup coverage unestablished.
Actual owned 8.3 and FSCTL junction cases pass. The exact receipt and logs are
`artifacts/continuation-namespace-v97.meta.json` and its stdout/stderr logs;
executable SHA-256 is
`0d3bbc11f6e484c33c0d55534cb99be2ee85579cf956e0c9ed53b1758e5015cb`.
The combined v97 core run times out at the unchanged 120-second limit. Saved
search and Library phases pass in 35.219 and 74.344 seconds, respectively; the
remaining namespace groups are interrupted. `artifacts/continuation-core-v97.xml`
and its log retain this failure. The independently passing namespace process
does not turn this timeout into a complete core pass. The later v98 partitioned
run above passes fresh, separately bounded SavedSearch/Library/Namespace
processes; all original assertions and the core timeout remain unchanged.

The v94 authored App run completes 289 checks in 101.875 seconds (102.306 seconds
including CTest), with one failure. All complete-array Sort/Group checks pass:
ascending and descending preserve the native two-column array and its secondary
direction, the genuine expanded Ribbon rows report the correct BooleanValue,
grouping retains its own independent direction, and the fixture restores its
original sort/group/selection/focus state. All four original-array asynchronous
Kind publication checks also pass. Exact evidence is
`artifacts/continuation-app-v94-authored.xml`,
`artifacts/continuation-headless-smoke-v94.json` and
`artifacts/continuation-app-v94-authored.meta.json`; immutable application
SHA-256 is `87b2c275a8b091bb04b7334d968211c64330cf6e435f5bdf135174e8366a10d7`.

The remaining v94 RTF Preview check fails its creator-side source/Ready gate
with `E_FAIL` (`0x80004005`). Foreign UIA content reads do not start, pixel
capture is aborted, and the second source is not selected after that failure.
This run measures no Preview image and does not establish blank rendered pixels.
The creator private-window controls, selected-file Details content/pixels and
final source/original-view preservation all pass. At that historical checkpoint,
later width, optional Preview visual and cleanup changes are uncompiled and the
evolving 21-group core changes remain under review. Neither
the v94 App result nor an older passing run certifies those changes.

The v93 combined core run fails with an access violation after 112.335 seconds.
Its owned debugger records native Play-to menu enumeration during an explicit
Cast plan in the isolated namespace fixture, rather than the earlier v92
Rotate-right path. Evidence is `artifacts/continuation-core-v93.xml`, its metadata
and `artifacts/continuation-namespace-v93-debugger.log`. Core executable SHA-256
is `475b9c6272ecb12668a13de30e0bfa162b83cf8838d3657a74ec2d1a2e0086e6`.
Debugger exit zero records a controlled diagnostic stop, not a passing core run.

The v95 namespace-only process exits normally with code 1 after about seven
seconds, with 20/21 groups passing and 2,395 assertions. Its receipt records
actual kernel exit, an unchanged executable and no watchdog stop. The isolated
Cast snapshot fails with `ERROR_NOT_SUPPORTED` (`0x80070032`), no owner, no site
and no native parent ordinal; it does not prove enabled Cast authority or a
passing complete namespace suite. Exact stdout/stderr and receipt are
`artifacts/continuation-namespace-v95.stdout.log`,
`artifacts/continuation-namespace-v95.stderr.log` and
`artifacts/continuation-namespace-v95.meta.json`; immutable executable SHA-256
is `980d8db01d7ef0e5082c3b25d3929d36d48fb51dca62dc36656c38f4fee29309`.
This separate run has no access violation and does not supersede the failed
combined-core result.

The installed Library capture-only run at
`artifacts/visual/run-20261006-020708-bc845b714606407eac0c2c940598f6b1`
passes the genuine two-process fixture and owned Library capture. Native Save
and subsequent Load/Add/Commit stages record 12.508 and 21.254 seconds on the
parent launch/wait/readback stopwatches, respectively; these are not process CPU
times. Both actual process receipts report exit zero with no timeout or kill,
and the final report verifies all seven native fixture gates and the chained
descriptor/source identities. The initial report is explicitly incomplete and
cannot admit capture. The run uses the v94 application hash above and fixture
builder SHA-256 `7c3c964750015b54eb793142192fdd54d76e55632a27f0c739daf18d5dfd80d3`.
It performs zero reference comparisons, so it establishes no new strict visual
pass. The reference manifest and comparison tool hashes remain
`a49f3266a436bd794daface43799ae99b351237497f3ee07ab173486cb92af73` and
`51a9a5840ce7dc563f80527e1fd7a8571f2a18752911ed5e035dc49a1e20876b`;
thresholds and source assets are unchanged.

The following earlier reports remain historical evidence for their own snapshots.

The v91 authored App run finishes 288 checks in 120.422 seconds (121.05 seconds
including CTest), with three failures. All four original-array Kind publication
checks pass, including the actual 257-item media selection, a reentrant newer
selection and an empty navigation destination. Native descending sort succeeds
and preserves both columns and the source, but reading the collapsed Ribbon
toggle still returns false and gates the ascending/group checks. The RTF Preview
remains blank. Exact reports are `artifacts/continuation-app-v91-authored.xml`
and `artifacts/continuation-headless-smoke-v91.json`; immutable application
SHA-256 is `ad6f45351b00f267d5e4e60b7094c3ea408c45fe73de21aae6a3852929077e31`.
The corrected public toggle readback and new explicit native Preview host are
source changes requiring their own native validation.

The v92 combined core run passes the new keyboard policy group (10 groups,
7,726 assertions), then crashes after 109.97 seconds in a native namespace
fixture. A separately owned, headless debugger reproduces its first access
violation: eager menu enumeration initializes an unrelated Play-to cascade while
planning Rotate right. The stack is recorded in
`artifacts/continuation-core-v92-debugger.log`; the normal test result is
`artifacts/continuation-core-v92.xml`. Debugger exit zero describes its controlled
stop and does not establish a passing suite. Targeted menu planning, Preview,
isolation controls and the two-stage Library fixture still require new builds
and actual runs. A separate native Library diagnostic finishes without a timeout
but fails strict source preservation: directory ChangeTime and LastAccessTime
change, while its full FileID, attributes, creation/write times and empty contents
remain unchanged. The cause is unestablished.

The v88 all-target Release build is warning-free. Its authored App run finishes
288 checks in 114.359 seconds (115.14 seconds including CTest), with six
failures. Failure-only native readbacks identify two fixture prerequisites:
the requested 257-item media array actually selects one item, and both sort
direction commands are unregistered when their framework BooleanValue is
queried. Descending sort itself preserves both native columns, the selected
FileID, bytes, timestamp, location and history. No ascending/group commands
are accepted through the failed prerequisite. The media request has known
count one and a completed genuine task; this run does not prove the intended
257-item publication case. Preview remains blank. Exact fresh evidence is
`artifacts/continuation-app-v88-authored.xml` and
`artifacts/continuation-headless-smoke-v88.json`, with immutable executable
SHA-256 `72945868c8d76ea5b669fdba71572bc749db20ddf41a8a6d20843783c7cc5f02`.
The installed layout was not run for v88. Subsequent fixture corrections need
their own native validation.

The v87 all-target Release build is warning-free. Its namespace-only run passes
18/18 groups and 2,195 assertions in 10.039 seconds, including original-array
asynchronous Kind authority, native aggregate classification, cancellation and
the existing actual 8.3/junction/path mutation controls. The authored App run
completes 288 checks in 120 seconds with six failures. Native descending sort
preserves the complete two-column array; the subsequent framework BooleanValue
read for command 145 fails with `E_FAIL`, gating the ascending/group checks.
The new media publication fixture fails its retained-task readiness predicate;
its original-array namespace tests pass, but actual failure-state readbacks
are still needed to distinguish missed publication from a product failure.
The production two-mask F6 checks and real unavailable Close origin regression
pass. RTF Preview remains blank. The installed App hits its unchanged 150-second
CTest limit and writes no fresh JSON report. The prior copied report is retained
with an explicit `stale-prior-v85` name and supplies no v87 evidence. Exact logs
are `artifacts/continuation-app-v87.xml`, its native log, authored smoke JSON,
and `artifacts/continuation-app-v87-installed-incomplete.json`.
Application SHA-256 is
`9ae5e968242e43a10066413d2dcef9cb97e87890279d7c9e9fac437d27f531e9`.

The v85 all-target Release build is warning-free. The authored and installed
App reports complete 284 and 292 checks in 120.329 and 138.125 seconds, with
five failures each. Three new sorting checks fail before executing a command:
an earlier fixture deliberately leaves selection empty, and the new setup
incorrectly rejects native `ERROR_NOT_FOUND` instead of establishing its own
owned selection. The production reverse-focus checks pass in both layouts;
the bare retained LIST-root neutral `accSelect` control fails to move Header
focus, while its held-Shift control passes. Native RTF Preview remains blank.
All isolation, keyboard restoration and source preservation gates pass. The
immutable executable SHA-256 is
`22ab1eaafc2c3120f43e1abe1353f597f99d873aa07c81b8b78a7ca361646a56`;
exact reports are `artifacts/continuation-app-v85.xml` and
`artifacts/continuation-headless-smoke{,-installed}-v85.json`. The removed
experimental focus routes are no longer present; their earlier failures remain
in the immutable reports. Source corrections after this run are unverified.

The separate v5 native Preview-host prototype builds warning-free and exits 1
in 1,078.316 ms under a 60-second external watchdog, without a timeout. Both
registered native handler objects return exact `S_OK` for the single
SetSite/Initialize/SetWindow/DoPreview sequence and create a visible foreign
`RICHEDIT50W` child under the exact supplied private parent. The actual child
parent, `IsChild` and root identities agree. `GetThreadDesktop` for each foreign
renderer thread returns null with immediate error 5; the owned parent/root and
creator desktop queries independently identify the exact private desktop.
This proves a desktop-query failure, not a foreign desktop placement or a
rendering failure. Admission still fails, so UIA content and pixels are not
read. The first source's post-release selection fence also fails with
`ERROR_NOT_FOUND`; both sources' final original bytes and metadata pass.
Source SHA-256 is
`77393f04a92b380c06ef4c0247a39361846caec69fc34e9108917edf495d6f80`;
receipts are `artifacts/continuation-preview-prototype-v5.meta.json`, its stderr
log and `artifacts/preview-prototype-run-v5/protocol.json`. This isolated
protocol test does not replace the production automatic-pane assertion.

The v83 all-target Release build is warning-free. Authored and installed Ribbon
tests pass in 8.74 and 21.12 seconds after removing unused nested Ribbon
collections. The actual dynamic ACTION control returns `ERROR_NOT_SUPPORTED`,
`VT_EMPTY` and no collection; its native popup invocation retains original
index 17. Extract still passes the exact 54-row, twelve-generation availability
and original-index-2 invocation checks. Logs are
`artifacts/continuation-build-v83.log` and `artifacts/continuation-ribbon-v83-native.log`.
The immutable application SHA-256 is
`0ea4eb6c1ed2055d1a336058a80d0130fcd34b382cd09878db4fc6a5f481e736`.
The remaining v83 integration run passes all seventeen active targets in
195.51 seconds; five shared-state targets correctly skip locally. This run
excludes the two App targets, the separately verified Ribbon targets and the
earlier core target. Exact scope and output are retained in
`artifacts/continuation-integration-v83.xml` and its native log.

The earlier v84 App report completes 283 checks in 124.781 seconds with three
failures. Guarding the complete native activation/focus destination makes the
strict production reverse-focus check pass under both neutral keys and held
Shift. Its fixture completes in 6.094 seconds within the unchanged ten-second
bound, preserving all 256 keyboard bytes, exact selected/focused FileIDs,
history, generation and the complete 1,006-file corpus. Two unused experimental
routes still fail: `SelectItem(SVSI_FOCUSED)` leaves Header focus, and the real
row's `accSelect` reaches Content but a fresh accessibility wrapper has a
different canonical COM identity. Neither route is used by production. The
v82 exhausted-deadline and absent-mark fixture defects are corrected, without
loosening the production checks. Native RTF Preview remains blank; the actual
selection data object's `IPreviewItem` query is unsupported (`E_NOINTERFACE`).
Reports are `artifacts/continuation-headless-smoke-v84.json` and
`artifacts/continuation-app-v84-native.log`; executable SHA-256 is
`deadeae5cda15e692325fdbf6a046d8f3d2657db2325312dc1d544488054495e`.

The focused v83 Share run passes native capture/isolation but fails strict
source pixels (agreement 0.967848, edge F1 0.996356). The independent actual
selection menu loads successfully and contains zero exact canonical matches
for both Specific people and Remove access. Direct state is `E_NOTIMPL`, and
registered/raw lookup returns `ERROR_NOT_FOUND`; there is no native enabled
leaf authorizing a host eligibility change. Source, target and settings
preservation all pass. Evidence is
`artifacts/visual/run-20261005-230023-1d38418e8bd348ee8aa9ec46b49aefc8`.

Historically, the complete local v42 run on 2026-10-05 passed every then-active
target, executable SHA-256
`e5796664dcfdc48d4e2081f85d3093fefac2880e4993ecd508ebc9fc9bf2e07d`.
All seventeen active targets passed; four shared-state mutation targets correctly
skipped locally. Total time was 342.88 seconds, recorded in
`artifacts/continuation-full-tests-v42.log` and `artifacts/continuation-core-tests-v42.xml`. The
all-target Release build completed without warnings
(`artifacts/continuation-build-v42.log`). Both authored and installed App,
Ribbon, recent-item and theme targets passed, along with native search semantics,
menu equivalence, owned selection/operations and the real search-window child.

The App reports contain exactly 254 authored checks in 57.109 seconds and 262
installed checks in 58.672 seconds, with zero failures
(`artifacts/continuation-headless-smoke-v42.json`, `artifacts/continuation-headless-smoke-installed-v42.json`). Both
report a private desktop, an unchanged input desktop and no visible input-desktop
windows. Each includes ten complete search-window checks and three atomic Clear
History checks. The separate `native_search_window_handoff` target passed all
three groups, including a real child launched through the production explicit
`HANDLE_LIST` path, in 0.32 seconds.

The original v42 environment report remains preserved; its shallow JSON encoding
truncated the nested native-binary versions. A later read-only observation with
the same executable checksum is recorded in
`artifacts/continuation-environment-supplement-v42.json` (15:40:31 UTC), including
Windows 10 22H2 build 19045.6466 and complete native version fields. It supplements
the original report rather than replacing its test-time evidence.

The complete v42 visual run
`artifacts/visual/run-20261005-153919-bb5eb675b4814b8d92a9455e4d486b6b/summary.json`
passed native capture/isolation checks for all nineteen scenes. Strict source
comparison passed six, failed ten and restricted three. The new source-matched
Library setup returned `E_INVALIDARG`; that restriction is a fixture/API failure
under diagnosis, not proof that the profile lacks the source. The report explicitly
records that whole-application parity is not established. Earlier visual results
remain tied to their snapshots below.

The v48 all-target build is warning-free. Its full local run records sixteen
active targets passed, four correctly skipped and one failed core target in
365.18 seconds; three new assertions assumed COM behavior that the actual
Windows implementation does not guarantee. Both application reports pass all
255 authored and 263 installed checks, including a paired Share-site comparison
against a distinct native browser over the same file. Modern Share is enabled
in both. Specific people and Remove access return `E_NOTIMPL` and have no matching
registered menu leaf in both; this does not establish an edition or policy cause.
Exact file identities, owner/group/DACL, selection, history and isolation remain
unchanged. Reports are preserved as `artifacts/continuation-*-v48.*`.

The corrected v49 namespace test passes twelve groups and 618 assertions. It
observes the real standard GIT's creator-thread `GetUnmarshalClass` callback;
zero `MarshalInterface` calls are legitimate for the observed free-threaded
marshaler optimization. Full native CIDA/Properties results, exact selection/site
and separate background registrations, same-object reuse, standalone ownership,
generation replacement, cancellation and actual registration reentry remain
strict. The worker lifecycle test also passes: its complete native payload stays
alive until kernel termination and releases on the initialized creator STA
outside the bookkeeping lock. A fresh thread and a thread after balanced own
initialization may have an implicit MTA when another process thread initialized
the MTA; this follows the [documented apartment qualifier](https://learn.microsoft.com/en-us/windows/win32/api/objidlbase/ne-objidlbase-apttypequalifier).
Focused logs are `artifacts/continuation-namespace-v49.log` and
`artifacts/continuation-worker-v49.log`; they do not replace a full later run.
The complete core target then passes in 114.48 seconds on the clean v50 build
(`artifacts/continuation-core-tests-v50.log` and its matching JUnit report).
Independent unmodified installed BML renders empty Video group captions for
both `0x2c20` and `0x2c21`; the host's previous `Play` override fails that exact
comparison. Preserving the native empty caption fixes the regression and the
installed Ribbon target passes in 21.25 seconds on v51
(`artifacts/continuation-installed-caption-v51.log`). Command labels and native
eligibility are unchanged; this focused fix is not a new full visual result.

The v48 full visual report at
`artifacts/visual/run-20261005-165943-60d83fb583cc485395cc4dcec41330da/summary.json`
records nineteen native captures passed, six strict comparisons passed, ten
failed and three restricted, executable SHA-256
`9cea818574f6301b47becd3edcbfd419fe94bb525e35506a180d1433b3dc4573`.
The built-in Documents Library resolves under its protected read-sharing lease;
all byte/identity/metadata preservation checks pass before any application view
is constructed. Its display comparison remains restricted. Protocol and
comparison thresholds are unchanged; whole-application parity remains unproven.

The v42 core target passed nine native search groups and all saved-search metadata
groups, including protective shallow/equal-root/direct-child exclusions, aliases
and newly matching files across live/native-save/import/re-save routes. Native
Date, Kind and Size replacement retains complete predicate semantics, relative
and absolute dates, exact FileIDs and metadata; all twenty-three installed Kind
expressions pass full-field validation. Both App layouts verify actual native
refinement SelectedItem and complete ItemsSource after parent-only expansion,
metadata/history, re-save, localized UIA names, cues and bounded Unicode tooltips.
The installed Ribbon independently checks four group captions against raw
installed markup.

An earlier Windows 10 configuration had twenty-seven CTest targets: twenty-two
active and five shared-state targets that correctly skipped locally. Core retained a
120-second aggregate bound. App commands and each of the Small/Large/Stress
native menu-equivalence targets have 90 seconds; both application suites have
150 seconds, and search-window handoff
has 45 seconds. The disposable native-transfer target has 150 seconds. Individual
native worker deadlines are unchanged. The independent native-drop target also
has 150 seconds and requires the same genuine disposable-runner opt-in. Earlier
failures remain preserved below.

The earlier hosted run, [commit `f909f69`](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/37346525314),
passed fifteen active main-job targets and correctly skipped its ordinary
transfer target. The actual opted-in history, search-options and view-persistence
targets passed on that genuine disposable runner. The App target failed five
checks: two selection-generation checks, long saved-item default activation,
and two RTL margin checks. The saved-item result overwrote earlier stage failures
with an `E_UNEXPECTED` fixture sentinel; it does not identify a production API
failure. Subsequent source changes retain stage HRESULTs and wait for complete
native enumeration, selection and data-object readiness under common deadlines.
RTL margins are applied after native frame restoration and size/font processing.
The later local and hosted results below validate these changes.

The separate opted-in transfer job passed real native Copy/Paste and Cut/Paste,
including complete CIDA, preferred effects, content and exact source/destination
FileIDs. Paste Shortcut's native plan remained disabled, so direct drop stages
were not reached. New read-only diagnostics preserve the producer/consumer
formats, original view background menu and independent folder background menu;
they do not override native eligibility. The validated-application job was
correctly skipped because both prerequisites failed. No release is certified
by this run. Reports remain under `artifacts/ci/run-37346525314-{main,transfer}/`.

The v53 authored App run establishes the new actual Details-pane selection A/B
semantic and pixel check, and source/view preservation. Its native RTF Preview
check fails before capture; the old 90-second aggregate bound then terminates the
suite before final JSON. This incomplete run is retained in
`artifacts/continuation-app-v53.log`, executable SHA-256
`35e062b91a635d4d4cc38b47cc3ff7ec0509af51111ec1bb18804b1e79f12e25`.
Failure details now flush to stderr before teardown. The aggregate App bound is
150 seconds to include these added bounded phases; individual native/UIA
deadlines remain unchanged. Preview behavior and the complete new suite remain
unverified until the next run.

The warning-free v55 all-target build completes the authored App suite in
99.73 seconds: 258 of 259 checks pass, with native RTF Preview as the sole
failure. The selection-generation, saved-item activation and both RTL margin
checks pass locally. Its full report is
`artifacts/continuation-headless-smoke-v55.json`, executable SHA-256
`2ee33c0d0c87c6e6e87b84c7978c2c0790f2180dce9c765d0595b50171069927`.
Preview has an allocated pane, exact source selection and retained site/view;
input-desktop and HWND privacy checks pass. Both the App and independent native
reference expose zero contained preview elements or handler HWNDs. The fixture
omitted the native view activation performed by normal `run()`; a subsequent
fixture correction activates the actual private view and verifies the focused
source by FileID.

The v57 authored App run completes all 259 checks in 117.18 seconds, again with
native RTF Preview as the sole failure. Both the App and independent original
native browser have the exact selected and focused source FileID and successful
native view activation. Their pane exposes an empty `Thumbnail Module` document;
UI Automation's document-range read returns `E_PENDING`. The actual App pane
captures are uniform and unchanged between the two owned RTF files. These are
failed render proofs, not successful screenshots. Source files, view/site state
and input-desktop isolation remain preserved. Its report is
`artifacts/continuation-headless-smoke-v57.json`, executable SHA-256
`9e5051bdbeed5b131a489a0281a454c361d45e6558d6781ba6a6ba518148a9f8`.
No edition, policy or production cause is established by this observation.

The bounded no-UI diagnostic on v58 proves the effective original native
surrogate route before activation. Factory acquisition, instance creation,
both preview/stream interfaces and initialization with the owned read-only RTF
stream all return `S_OK`; neither `SetWindow` nor `DoPreview` is called by that
diagnostic. Reading only the owned desktop's integrity label succeeds and finds
no explicit label. The real App and reference panes still fail rendering. All
259 authored checks complete in 111.04 seconds, with that same sole failure,
executable SHA-256
`feaf62b6607142e5c597b67e3c9e0ef89ae0f4a6ba7cdbfbf1beb20ed54bb6e6`.
Reports are `artifacts/continuation-app-v58.log` and
`artifacts/continuation-headless-smoke-v58.json`. Successful activation does not
establish which integrity level or desktop performs the actual preview render.

The earlier hosted run, [commit `781fd49`](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/37352985840),
passes all five previously failing selection, saved-item and RTL checks. Fifteen
active main-job targets pass, the ordinary transfer target skips, and the App
target fails only native RTF Preview among 259 checks. The actual opted-in
history, search-options and view-persistence targets pass on that genuine
disposable runner. The separate transfer job again passes native Copy/Paste and
Cut/Paste; Paste Shortcut remains disabled after bounded normal dispatch, so
the direct-drop stages are not reached. Its original view background menu also
publishes that disabled native command. Producer and clipboard-consumer
identities and COPY preference remain exact, but their `DVASPECT_LINK`
`QueryGetData` results differ. Query results alone do not prove actual rendering
or identify the cause. Reports are preserved under
`artifacts/ci/run-37352985840-{main,transfer}/`. The combined application job
correctly skips; this run certifies no release.

The later [commit `ac238d5` run](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/37356273533)
again passes fifteen active main-job targets and the real opted-in history,
search-options and view-persistence checks. Native RTF Preview remains the sole
App failure. Its transfer job times out at 150 seconds before producing a native
phase trace; that result does not identify the blocked API. The relative JUnit
path resolved inside the build directory, so the transfer artifact contains only
its log. Subsequent workflow changes use absolute report paths, flushed native
call phases, a bounded active-call watchdog, and a separate drop-only process.
Those new genuine-CI branches remain unmeasured until their hosted run completes.

Local v60 and v61 each complete all 259 authored checks with only native Preview
failing, in 112.35 and 110.08 seconds respectively. The fresh diagnostic desktop
has no top-level windows, and both process-token access controls succeed. A
verified duplicate low-integrity impersonation token receives `E_ACCESSDENIED`
for both READOBJECTS and READOBJECTS|CREATEWINDOW|WRITEOBJECTS. The low-label
comparison does not execute: v60 rejects the documented implicit-MTA apartment;
v61 accepts that qualifier but its unscoped message-only lookup finds a different
thread's window. Neither result establishes that window's desktop. The original
thread desktop and process-token identity/policy are preserved; no label change
or preview rendering call is performed by this diagnostic. Reports are
`artifacts/continuation-app-v60.log`, `artifacts/continuation-app-v61.log` and their
matching smoke JSON/JUnit files. The v61 executable SHA-256 is
`240b14b73db0a7bf62b174112ca1b7b4f393bcb0e80683c285fba310e9613392`.

The reviewed optimization candidate separately passes sixteen native namespace
groups and 1,983 assertions, both public icon-cache fixtures at actual 96 DPI,
and both complete Ribbon suites. Exact raw four-byte icon comparison includes
706,560 bytes through the public cache and 920,000 bytes through independent
native size-pair extraction. Noncurrent DPI pairs do not establish production
cache coverage at those DPIs. Six fresh interleaved A/B rounds are retained in
[performance.md](performance.md), including inconclusive startup/navigation
results. The integrated v62 all-target build is warning-free; its two local
transfer gates correctly skip without clipboard or native-drop execution.

The complete integrated v62 local run records thirteen passed targets, five
correctly skipped targets and four failures in 454.24 seconds
(`artifacts/continuation-full-tests-v62.log`, `artifacts/continuation-core-tests-v62.xml`).
Authored/installed App reports complete 259/267 checks with only native RTF
Preview failing. Core reaches the sixteenth App-command group before its
120.31-second aggregate timeout; the all-count menu fixture reaches a 10,000-item
comparison before its 90.03-second timeout. Its printed times cover only two of
the four or five native queries in that iteration. Source review establishes
continued progress, without identifying a hang or its timing cause. New test
partitions retain every comparison and individual deadline, preserve the original
all-count/full-core diagnostic modes, and require fresh runtime validation.

The subsequent v63 authored App run completes 259 checks in 87.40 seconds with
the same sole Preview failure, executable SHA-256
`8cc5d768b67be4337a219b9364d76adff6e67984c6f47c0ffe41ea52f41f68e9`.
The message-only provenance guard now retains unknown desktop identity and
`0x8007001F` rather than excluding the window. No low-label write executes;
restoration and process-token preservation still pass. Reports are
`artifacts/continuation-app-v63.log` and its matching JSON/JUnit files.

Hosted [commit `df1aaca`](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/37361365765)
passes thirteen main-job targets and skips both ordinary transfer gates. Three
targets fail: native Preview, saved-search replacement membership (`first.txt`
instead of the distinct expected `second.bin` FileID), and an icon reference
whose original single-size native extraction produces no icon. The latter fails
before a paired-cache comparison, so it does not establish a production icon
regression. The separate opted-in transfer process fails its original-view Copy
publication's combined identity/effect/source predicate in 4.29 seconds. Its
independent drop process fails at the actual `IDropTarget::Drop` call after the
20-second active-call budget. Both absolute JUnit files and phase logs are
retained under `artifacts/ci/run-37361365765-{main,transfer}/`. The combined
application job correctly skips. Added diagnostics retain exact failing
predicates, native resource requests and source async/key state; they do not
relax eligibility, effects, output identities or deadlines.

The v64 all-target Release build is warning-free. Its focused nine-target run
passes all seven active targets and correctly skips both local transfer gates,
in 259.89 seconds (`artifacts/continuation-partitions-v64.log` and its JUnit).
Core passes in 94.40 seconds, all eighteen App-command groups in 44.06 seconds,
and all three menu buckets pass within their original 90-second bounds. Both
complete Ribbon layouts pass. The core executable SHA-256 is
`5dee08d01443f57c1640d07145cbff642012d2b11bb9fef4368b26191bde9883`.
This resolves the observed aggregate timeouts on this snapshot; it does not
replace a later full run or establish the cause of the changed timings.

The earlier [commit `317914b` run](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/37249626987)
failed Open File Location's folder-identity check; subsequent checks use actual
volume/128-bit FileIDs. The earlier `aecc063` hosted run established
actual Undo/Redo, native search-option transitions and folder-state restoration
on its own snapshot. Those mutation tests remain restricted to genuine opted-in
disposable runners.

The independent quiet benchmark of the same v42 executable passed all native
states and isolation checks, with 853 desktop-visibility observations
(`artifacts/performance/run-20261005-154226-0c040b1504b445b3b87b8b24fb00bfa6/native-navigation.json`).
Process-entry-to-native-view startup was 758.411 ms; navigation p95 was
120.430/153.477/146.275 ms for 10/1,000/10,000 files. Select all and invert none to
all reached complete native command readiness in 7,919.048/7,242.882 ms, distinct
from immediate selected-count readback at 1,004.726/201.164 ms and queue-drained
readback at 2,187.767/1,801.294 ms. See
[the performance report](performance.md) for immediate count readback, queue-drain
boundaries, worker profiling and prior failures. Strict visual comparison remains
a separate result from native capture and functional tests.

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
| `native_search_backing_lifetime`, `native_app_search_backing_lifetime`, `native_app_search_backing_lifetime_installed` | Four actual store ownership/replacement groups and real authored/installed App query/refinement/history/recreation/child/reentry/close cases; lifetime-total capacity 128 remains |
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

## Retained checkpoints and failures

The v70 all-target Release build is warning-free. Its authored App completes
277 checks with only Preview failing in 124.281 seconds, within the unchanged
150-second CTest bound. The three selected-file focus checks, six older focus
checks and fifteen actual keyboard-message checks pass, preserving all three
selected FileIDs, the focused FileID, PIDL history, all 1,006 owned files and the
complete thread keyboard table. Reports are
`artifacts/continuation-headless-v70*`; executable SHA256 is
`8f76186ee716e412da490f2ee3f71a459d67b8035525f2bd853aa612f68d0562`.
This tests the first Shift correction, before the subsequent fresh-state
transition guard, and does not certify that later source.

The same v70 Preview diagnostic binds the actual selected RTF item through
`BHID_Stream`, verifies all 83 bytes and the complete native FileID, initializes
the fresh original native handler successfully, then releases its interfaces
before the retained stream. File attributes, creation/write/change times and
bytes remain unchanged; the worker exits and restores its borrowed desktop
within the existing budgets. The actual App and independent native Browser
panes both remain blank. Read-only policy results distinguish absent values
from configured defaults; they do not establish a policy cause. No diagnostic
calls `SetWindow` or `DoPreview` and the native rendering assertion still fails.

The first v71 standalone focus-keyboard run compiles without warnings and
passes four of six groups, including the pure transition reducer, parser and
posted-message isolation. Two native provider-update/failure groups fail;
`artifacts/continuation-focus-v71.{log,xml}` retains these results. This is
unresolved evidence, not acceptance of the guard. Physical-input-positive
transitions remain unexercised under the headless input restriction.

The diagnostic rerun v72 identifies the two focus-fixture failures: native
`SetKeyboardState` succeeds but the immediate table read has A/B high bits
`128/0`. The simulated provider's strict round-trip assertion throws before
its intended action status. Every guard cleanup stage succeeds. The fixture
now uses modifier/toggle changes that must pass the same immediate strict
round-trip and final complete 256-byte comparisons; arbitrary byte patterns
remain covered by the pure reducer. V73 passes all six groups and 16,649
assertions in 0.15 seconds (`artifacts/continuation-focus-v73.{log,xml}`). This
does not establish a general Windows key-normalization rule.

The v71 authored App completes 280 checks in 137.609 seconds with two failures:
the selected-file Shift reverse-focus check and native RTF Preview. Exact
selection, focused FileID, history, keyboard table and corpus remain preserved;
the Shift route still fails its strict content-focus readback. Bounded hosting
counters pass without overflow. Neither the App nor independent Browser asks
its outer site for `IPreviewHandlerFrame`; both query Preview pane visibility
and receive `EPS_DEFAULT_ON | EPS_FORCE`. Adding an outer frame interface is
therefore not an evidenced fix. The immutable executable SHA256 is
`a84c5a0833eba45f8d74ad03e35c1da1fba754d8028885687bdc9d0b0d2fbb25`;
reports are `artifacts/continuation-headless-v71*`.

The v74 authored App completes 280 checks in 118.515 seconds, with only native
RTF Preview failing. All selected-file focus checks pass with exact selection,
focused FileID, history, complete keyboard table and corpus preservation. Its
executable SHA256 is
`a1b9e43b00fd2df13d7d28bbfd3f553344612bad8e94a01ad881d835e191a2e0`;
reports are `artifacts/continuation-headless-v74*`. Added failure diagnostics do
not establish the cause of the earlier intermittent application focus failure.

The v75 standalone focus fixture passes six groups and 16,965 assertions in
0.05 seconds, including forty native combinations of held Shift masks and all
three Shift low bits. Physical-input-positive transitions remain unexercised.
The installed Ribbon target also passes in 20.48 seconds; the authored target
fails its new Extract-to original-provider-index assertion in 3.46 seconds.
`artifacts/continuation-focused-v75.{log,xml}` preserves both outcomes. The
all-target Release build is warning-free. This focused run is not a complete
suite pass.

The v75 controlled startup-desktop comparison uses the same immutable executable
(`f656639a5519fda0973f8dc4a41cf93715452f0fb03864d8b31babd108ea7ec1`)
for two serialized children: one starts on the input desktop and attaches the
ordinary private desktop before COM; the other starts directly on a fresh
parent-owned private desktop and borrows its Windows-assigned initial handle.
Both handshakes, exact executable/parent/child identity checks, attachment,
preservation, actual kernel exit and teardown checks succeed. The parent exits
1 without timing out after 239.741 seconds. Each arm completes 280 checks with
the same two failures, Shift reverse focus and native RTF Preview, in
117.078/121.563 seconds. Initial-private startup does not rescue Preview; this
does not establish the native surrogate's PID or desktop. Reports and readbacks
are retained under `artifacts/startup-desktop-v75-20261005-2149*`.

The recurrent focus failure has all ten keyboard-guard stages `S_OK`, no consumed
Shift events and exact full-table restoration. The native LIST-root
`accSelect(SELFLAG_TAKEFOCUS)` succeeds, but the final provider focus remains the
Details header. A successful method result is therefore not accepted as proof of
the requested content focus. The earlier passing v74 check remains distinct.

The v76 Extract-to diagnostic observes that native SelectionItem selection
returns `S_OK` without invoking the row. A single native Invoke activates the
authored row exactly once in v77, retaining provider index 2. The installed v77
fixture then crashes: its optional SelectionItem query returns `S_OK` with a
null interface, and the new diagnostic dereferences it. The retained owned dump
identifies the null virtual call. The fixture now records the raw result and
pointer presence without dereferencing optional interfaces; actual activation
still requires one nonnull Invoke interface and one exact callback. The dump
is a private diagnostic artifact and is excluded from publication.

With that correction, v78 installed Ribbon passes in 18.67 seconds. It verifies
native disabled and re-enabled destination rows, zero callbacks from availability
refresh, and one Invoke at the original provider index. Authored Ribbon fails
in 3.40 seconds because its disabled item-gallery row remains enabled. This is
a production gap, not a relaxed test: the source changes Extract-to to a native
command gallery with per-row Enabled properties, preserving authored Layout and
Share command IDs. At v78 its strict native validation had not yet passed. Exact output
is retained in `artifacts/continuation-ribbon-availability-v78-native.log` and
the matching focused log/JUnit report.

The warning-free v80 all-target build validates that repair: authored and
installed Ribbon pass in 9.01 and 19.48 seconds. Both layouts retain disabled
and re-enabled row checks, twelve refresh generations, 54 destination rows and
exactly one native Invoke at original provider index 2. The focused reports are
`artifacts/continuation-ribbon-v80.{log,xml}` and the preserved native log.

The v79 combined core target reaches its unchanged 120-second timeout during
the native large-array cases. Its six preferences groups, including real invalid,
read-only and locked destinations with unchanged previous bytes, pass before
the timeout. A separate v80 namespace-only run passes all sixteen groups and
1,987 assertions, including native 100,001-item final-target counterexamples,
actual 8.3 aliases and a native FSCTL junction. Its exact stdout and exit status
are retained; a wrapper receipt error prevented recording that run's process ID
and exact elapsed time. Neither focused result is a complete core-suite pass.

The v81 combined core target then passes in 110.69 seconds at the same bound.
Phase receipts preserve every original test and its order: native Library
checks take 65.546 seconds, saved-search metadata 32.235 and namespace checks
6.813. All six settings groups and sixteen namespace groups pass. The earlier
timeout remains retained without assigning an unmeasured cause; this passing
run is `artifacts/continuation-core-v81.{log,xml}` with the exact native log.

The v80 authored App completes 282 checks in 122.219 seconds, with three
failures: Shift reverse focus, the new focused-item native control and Preview.
Its headless persistence sentinel passes, preserving preferences, receipts and
pending errors without calling any writer. Native `SelectItem` on the already
focused item succeeds under both modifier states and preserves selection,
FileIDs, flags, keyboard and the 1,006-file corpus, but leaves Header focus.
The production Shift path also observes Header inside the neutral-keyboard
guard, with every guard stage `S_OK`. That alternate route is therefore not
adopted. The immutable executable is
`d693b3a8078c1ee76bd74d6febb9aeda026fe531ee2d1293ae96a4da77277593`;
reports are `artifacts/continuation-app-v80*` and
`artifacts/continuation-headless-smoke-v80.json`.

The v69 non-application run passes all nineteen active targets, with five
disposable-CI-only targets skipped, in 346.89 seconds. Core completes in 114.04
seconds; Small/Large/Stress native menu equivalence completes in
7.46/21.52/80.10 seconds, preserving the existing bounds. Both Ribbon layouts,
native selection, search-window handoff, recent items, desktop capture, crash
diagnostics, saved presentation, live results, Cast and themes pass. This run
explicitly excludes the two application suites and does not certify subsequent
source changes. Logs and JUnit are `artifacts/continuation-nonapp-v69.{log,xml}`.
The separate v69 authored App has 277 checks with four failures in 172.547
seconds: both Shift focus aggregates, their fixture's exhausted 10-second
budget, and Preview. Its completed owned-process run exceeds the normal
150-second CTest application bound; that limit is unchanged. The added numeric
focus diagnostics identify native Shift root-focus success retaining the
header, with the selected FileIDs and focused item preserved.

The v65 all-target Release build is warning-free. Core passes in 103.15 seconds,
including nine input groups with 3,166 assertions and sixteen namespace groups
with 1,930 assertions; real 8.3 aliases and a native junction are covered. Both
standalone Ribbon layouts pass. The authored App records 274 checks with seven
failures in 95.875 seconds: six focus checks and native RTF Preview. Its fifteen
new actual keyboard-message checks pass, including exact thread-keyboard
restoration, headless Undo/Redo denial and unchanged owned file identities.
These are `artifacts/continuation-core-v65.{log,xml}` and
`artifacts/continuation-focused-v65.{log,xml}`.

The explicit low-desktop diagnostic in v66 records 274 checks with only Preview
failing in 146 seconds, executable SHA256
`131fbc2358815e6eb9d6b3e150f4c67aed3d719da28dc59df4ea10cdacb11d6d`.
Atomic creation proves that the low token can obtain desktop read/write access
on the low-labelled sibling but cannot on its default-labelled sibling. Owner,
group, DACL, process token and input desktop are preserved; exact desktop
restoration succeeds. The actual native Preview pane still renders blank on
the low-labelled desktop. Admission is therefore measured, but changing the
normal desktop label is not an established rendering fix. The six focus checks
also pass in this run; their change is not attributed to the label.

The v67 authored App records 277 checks with three failures in 145.062 seconds,
executable SHA256
`0f51eb8faec8a3c4c2956536bd8ea4d290bf77510d579838ad82a532ad9d2969`.
All six original focus checks pass. The neutral-modifier direct native LIST-root
focus and production reverse-F6 cases preserve the complete three-file
selection, focused FileID, history and owned corpus. The Shift case fails its
strict provider readback, so both new aggregate focus checks fail; Preview is
the third failure. Reports remain in `artifacts/continuation-headless-v67*`.

The isolated v68 Cast fixture passes. Both real audio- and video-initialized
native menus return disabled parent state `3`, no submenu and no enabled
leaves; production reports the same disabled state. Its valid native AVI,
complete selection CIDA/FileID, bytes and settings are unchanged. A settled
baseline identifies Windows input-indicator helpers on the owned private
desktop before any Cast provider is created; every later checkpoint preserves
that exact window set and exposes no input-desktop UI. This does not establish
Cast behavior with available recipient hardware. Logs are
`artifacts/continuation-cast-v68-{stdout,stderr}.log`.

The integrated v64 installed-layout visual run on October 5, 2026
(`artifacts/visual/run-20261005-195124-4045f61663d24dda83be3f01a05df76a`)
passed all nineteen native captures. Six strict source comparisons passed,
ten failed and three remained restricted. Its executable is
`8cc5d768b67be4337a219b9364d76adff6e67984c6f47c0ffe41ea52f41f68e9`;
the reference protocol and comparer were unchanged. A preceding authored run
recorded eighteen successful captures, a Search popup timeout and fourteen
comparison failures. Those different backends cannot establish an installed
layout regression. An explicit installed five-scene control passed Home,
View, Music, Application and Search, including the native Date popup.

Hosted [commit `f0c70f5`](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/37364528682)
passed seventeen targets, failed Preview and the icon fixture, and correctly
skipped two clipboard/transfer targets in 181.52 seconds. Native history,
search options and view persistence passed. The prior saved-rule replacement
failure did not recur; that does not establish its cause. The new icon diagnostic
identified `imageres.dll,-8208`, 32 pixels, native `S_FALSE` and no HICON on
Server 2022. The transfer job was canceled while queued and executed no steps;
this run supplies no fresh transfer or Drop measurements.

Dates and results belong to the listed snapshots. Abbreviated local identifiers
are executable SHA-256 prefixes; public identifiers are commits. Local reports
are ignored research artifacts, while the linked hosted runs remain public.

| Date / snapshot | Retained result |
| --- | --- |
| Earlier local `797d8295` | 13 passed, 1 failed, 3 skipped in 202.81 s. The large native-menu fixture's combined isolation guard failed; subsequent runs did not reproduce it or identify its original cause. |
| 2026-10-04 local `f01bc69d` | 13 passed, 1 failed, 3 skipped in 206.33 s. An unsupported Ribbon group-label property probe failed; removing that invalid probe gave a separate installed-Ribbon pass in 11.81 s. Full SHA-256: `f01bc69d57541f86c432ff1055936b1331d801000852165524a22975f2b8a6ea`. |
| Later local `e40e8c2c` | 12 passed, 2 failed, 3 skipped in 209.86 s. Clipboard sequence rose by 13 with the same foreign owner, unchanged payloads and no owned publication; external publication versus delayed rendering was not distinguished. Installed RecentItems also missed an immediate row; bounded exact-row waiting later passed both layouts. Neither assertion was removed. |
| 2026-10-05 CI [`1d91605`](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/37243073658), [`e344c76`](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/37244491657) | Each passed 10/14. Breadcrumb spelling, selection fallback, app selection/QAT and native persistence failed. |
| 2026-10-05 CI [`8191269`](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/37245948002) | 11/14 passed; bulk selection fallback, actual QAT name lookup and persistence membership failed. Subsequent fixtures retained exact native selection/focus and accepted the observed owned Windows 10/Server QAT names. |
| 2026-10-05 CI [`7f06241`](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/37247491672) | 12/14 passed; Open File Location folder identity and restoration after the next browse failed. |
| 2026-10-05 `aecc063` | Local: 14 passed, 3 skipped in 206.13 s, executable `72627afa9b464166d52e2b59aeb36e6d67b06dce48c84338012416f4b77b247a`. [Hosted](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/37248318031): all 14 configured functional targets passed in 132 s, including Undo/Redo, search options and restoration. The workflow still failed its 45-second large-selection command-readiness benchmark; native capture success did not establish strict visual matching. |
| 2026-10-05 CI [`317914b`](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/37249626987) | Open File Location folder identity failed. Later local App tests pass the native volume/128-bit FileID fix; hosted verification of that later source remains distinct. |
| 2026-10-05 local v11 | `artifacts/continuation-full-tests-v11.log`: 14 passed, 2 failed, 3 skipped in 290.25 s. The escaped Title test assumed a scalar XML root; installed App live-search navigation also failed. |
| 2026-10-05 local v12 | `artifacts/continuation-full-tests-v12.log`: 12 passed, 4 failed, 3 skipped in 284.54 s. Title comparison rejected native duplicate OR expansion; authored/installed standalone Ribbon and installed App accessibility checks also failed. |
| 2026-10-05 local v13 focused gates | App passed 209 authored/217 installed checks; native search passed eight groups. Installed standalone Ribbon failed a caption fixture that expected authored `Organize` instead of native British `Organise`. Independent UI Automation now compares the actual resource-specific group caption; both layouts pass in v15. Reports: `artifacts/app-gates-v13.log`, `artifacts/title-semantic-v13.log`, `artifacts/ribbon-label-gates-v13.log`. |
| 2026-10-05 local v23 | Full functional suite: 16 passed, 3 skipped in 325.11 s, executable `ae94ed350901158f8a4a45ffb198eb2654e74b730d965cfa25f83d92866d15b3`, `artifacts/continuation-full-tests-v23.log`. Complete visual run `run-20261005-120823-3ad69938256a4a988ef4edd1d82fc2e7`: 19 native captures passed; strict comparison 6 passed, 11 failed, 2 restricted. Its quiet benchmark recorded 7,650.251/6,805.953 ms full large-selection state readiness. |

The clipboard sequence is shared across the window station, including other
desktops. Its historical isolation failures are retained without attributing
them to a writer or treating a later pass as proof of their cause. Subsequent
native-state cancellation and resource-menu improvements have accepted local
benchmark reports in [performance.md](performance.md).
