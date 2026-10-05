# Headless verification

The target machine is Windows 10 Home 22H2, build 19045. Builds use Visual Studio
2022 Build Tools, MSVC 19.44, Windows SDK 10.0.26100.0, C++20 and the static x64
runtime. Verification runs without displaying anything on the input desktop.

```powershell
./scripts/build.ps1 -Configuration Release
./scripts/test.ps1 -Configuration Release -SkipBuild
python -m pip install -r scripts/requirements-visual.txt
./scripts/visual-check.ps1 -Configuration Release
./scripts/benchmark.ps1 -Configuration Release
```

The scripts preserve build output, CTest JUnit, the exact application smoke
report, executable checksum and environment in ignored `artifacts/`. Screenshot
and performance runs copy the executable into a unique directory and verify its
checksum before use. Reports describe the tested snapshot; a previous passing
run is not evidence for later source changes.

## Current evidence

The latest proven complete local checkpoint is 2026-10-05 v42, executable SHA-256
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

The current Windows 10 configuration has twenty-two CTest targets: seventeen
active and five shared-state targets that correctly skip locally. Core has a
120-second aggregate bound, the separate native menu-equivalence target has
90 seconds, both application suites have 150 seconds, and search-window handoff
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
| `core_and_shell_operations` | Owned file operations and recovery; native search and saved-query membership; preferences, input mapping, archives, shortcuts, context menus, Libraries, native command state/resources, asynchronous lifetime, breadcrumbs and search history |
| `native_menu_state_equivalence` | Actual full-array native state/menu equivalence, resource restrictions and restoration, missing/duplicate verbs and retained-provider lifetime |
| `native_search_window_handoff` | Complete validated search context, reduced read-only mapping, malformed output preservation and actual explicit-handle-list child inheritance; fresh native factories, exact scope/result identities and child lifetime |
| `hidden_shell_host` | The real application, native ItemsView, navigation, eight layouts, columns, complete selection changes, search/import/refine/history, search-window Content/List restoration and Close origin, atomic Clear History, ZIP/Library contexts, native tree options, splitter, F6/Tab focus routes, Ribbon/QAT and owned UI Automation |
| `native_view_selection` | Complete actual selection identities and complements, focus and checkbox flags on an owned 10,000-item native view |
| `installed_ribbon_features` | Read-only edition, media, recording and policy-dependent native capabilities |
| `hidden_native_ribbon` | The compiled native Ribbon, pages, contextual state, collections and images, native customization, persistence, minimized/docking state, accessibility and bounded tab selection |
| `private_desktop_visual_capture` | Actual native window/control painting, PrintWindow/WIC output, geometry, text changes, invalid inputs, exclusive output creation and desktop isolation |
| `native_recent_items` | Native Ribbon recent-item collection and metadata contracts without invoking personal destinations |
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

The four shared-state mutation targets check both `GITHUB_ACTIONS=true` and their
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
