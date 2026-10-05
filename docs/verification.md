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

An earlier functional Release checkpoint has SHA-256
`f01bc69d57541f86c432ff1055936b1331d801000852165524a22975f2b8a6ea`.
Its 2026-10-04 full seventeen-target run took 206.33 seconds: thirteen active
targets passed, one failed, and three shared-state targets were correctly
skipped. The failure was an added test attempting an unsupported native Ribbon
group-label property read. After removing that invalid probe, the actual
installed-Ribbon suite passed separately in 11.81 seconds. This is a full run
plus a corrected focused recheck, not a single all-green full run.

Both application suites passed: 130 authored checks in 35.88 seconds and 137
installed checks in 29.73 seconds. The core suite passed in 96.18 seconds,
including all six saved-search ACL profiles, native large-array menu equivalence,
retained-handler reentrancy and actual kernel worker termination. Quiet complete
core runs measured approximately 88–96 seconds; its aggregate bound is now 120
seconds with per-worker deadlines unchanged. Application bounds remain 90 seconds.

The preceding immutable `797d8295` full run passed thirteen active targets,
failed the large native-menu fixture's combined isolation guard, and skipped
three targets in 202.81 seconds. Subsequent separate and full core runs passed,
but did not reproduce that failure. The guard now reports payload, visibility
and clipboard-sequence conditions separately without dropping any assertion.
The clipboard sequence belongs to the window station, shared with other
desktops; the original failure has not been attributed to a particular condition
or writer. Historical failures remain evidence, not silently corrected reports.

The later `e40e8c2c` complete local run took 209.86 seconds: twelve active
targets passed, two failed, and three were skipped. Both application hosts
passed all 130/137 checks. The core diagnostic retained unchanged payloads,
an empty owned directory and a hidden owner; the window-station clipboard
sequence increased by 13, with the same foreign owner and no owner in this
process. That evidence does not distinguish external publication from delayed
format rendering triggered by native menu inspection. Its assertion remains.
The installed RecentItems fixture also failed on an immediately absent row
after opening its menu. A bounded wait for that exact owned row, without
repeating its action, subsequently passed both layouts in 10.35 seconds.

At public checkpoint `1d91605`, [the Windows Server 2022 CI run](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/37243073658)
built successfully and passed ten of fourteen targets. Breadcrumb path spelling,
the documented selection fallback, app selection/QAT accessibility, and hidden
folder-view persistence failed. Actual Undo/Redo and native search options passed.
The following [CI run at `e344c76`](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/37244491657)
again passed ten targets and failed the same four. A second breadcrumb assertion
still compared path spelling; per-item selection repair did not fix the fallback;
app selection and QAT accessibility still failed; whole-view item enumeration
returned `E_INVALIDARG` in the hidden persistence fixture. The ineffective
per-item repair is removed. The second breadcrumb now compares filesystem
identity. Revised selection and persistence fixtures explicitly realize only
their owned private frame, and the app reentry check uses the same guarded
presentation phase. These changes require new disposable-runner evidence.

The revised private-presentation fixtures passed their focused local run on
2026-10-05: native selection in 11.93 seconds, the authored app in 31.33 seconds,
and the installed app in 32.95 seconds (76.25 seconds total). The app adds an
explicit isolation assertion around the reentrant-selection presentation.
Subsequent diagnostics report toolbar names only inside the exact owned Ribbon
when its required QAT accessibility assertion fails. They do not relax that
assertion. Native view-state persistence remains skipped locally and unverified
until the new disposable-runner execution.

The [new disposable run at `8191269`](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/37245948002)
passed eleven of fourteen targets. Core operations passed, and the app's
reentrant-selection check now observed one exact item with real Copy eligibility.
Three targets still failed: documented bulk selection fallback, QAT name lookup,
and native view-persistence membership. The actual owned Ribbon exposed the QAT
as `Quick Access` on Server 2022, rather than Windows 10's `Quick Access Toolbar`.
Both observed native names are now accepted within that Ribbon and with the
required toolbar type; its Properties and New folder controls remain required.
The selection fallback now tries preflighted, revalidated native view indices
only when a successful bulk PIDL operation has not applied the requested count.
Its large fixture now also proves all 10,000 exact identities without a native
command facade. The persistence fixture reads each actual child PIDL from the
view and binds it through that view's native parent folder. These subsequent
changes passed focused local application checks (29.43/31.70 seconds). The
expanded selection regression initially failed its small fixture's focus setup:
three items were selected, but the focused item was -1. Painting the exact owned
private viewport and establishing focus through the original Shell view produced
the required item focus of 2. The complete selection suite then passed in 17.03
seconds, including all 10,000 exact identities without a native facade. The
assertions and deadlines remain intact. Disposable-runner results are pending.

The [disposable run at `7f06241`](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/37247491672)
passed twelve of fourteen targets. Core passed in 54.69 seconds and the complete
selection target in 20.17 seconds. Native QAT accessibility passed. The app's
Open File Location assertion failed despite observing the intended selected
file; its location helper still compared PIDL bytes rather than canonical Shell
identity. The revised helper uses `IShellItem::Compare(SICHINT_CANONICAL)` and
retains the separate exact selected-file identity check. Both local app layouts
then passed in 37.34/44.75 seconds. CI persistence reached and recorded owned
folder A's applied native state, then failed after the next browse. Its fixture
now clears completion before `BrowseToObject`, since `OnNavigationPending` can
arrive asynchronously. Full restoration still requires new hosted evidence.

The fallback snapshot now uses batched native view-order enumeration, retaining
indexed reads for interfaces that explicitly do not support it. Native selection
passed locally in 11.77 seconds, and with a descending native sort in 12.19
seconds. The two 10,000-file fallback calls measured 171.407/204.403 ms, while
independent identity readback remained separate. The subsequent app suites also
passed in 30.55/33.60 seconds before the canonical-location helper changed.
These focused results do not constitute an all-green full local or hosted run.

At `aecc063`, the 2026-10-05 complete local suite passed all fourteen active
targets and correctly skipped the three shared-state targets (206.13 seconds).
Its executable SHA-256 is
`72627afa9b464166d52e2b59aeb36e6d67b06dce48c84338012416f4b77b247a`.
The [hosted run](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/37248318031)
passed all fourteen configured functional targets with no skips in 132 seconds,
including actual Undo/Redo (1.71 seconds), search options (2.38 seconds) and
per-folder restoration (1.03 seconds). Its native capture step also passed.
The overall workflow still failed its navigation benchmark: Edit, Print, Remove
Properties and Run as another user remained pending after Select All within the
unchanged 45-second readiness bound. Capture-only success does not establish
Windows 10 reference-image matching. The strict visual failures remain recorded
in the visual report, and the benchmark failure remains unresolved.

## Test boundaries

| CTest target | What it verifies |
| --- | --- |
| `core_and_shell_operations` | Owned file operations and recovery; native search and saved-query membership; preferences, input mapping, archives, shortcuts, context menus, Libraries, native command state/resources, asynchronous lifetime, breadcrumbs and search history |
| `hidden_shell_host` | The real application, native ItemsView, navigation, eight layouts, columns, selection, search/import/refine/history, ZIP and Library contexts, native tree options, splitter, Ribbon/QAT and read-only UI Automation |
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
| `native_theme` | Theme policy, actual native dark pixels on recognized Windows builds, Light restoration, ownership guards and unchanged system configuration |

The three shared-state mutation targets check both `GITHUB_ACTIONS=true` and their
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

## Native behavior evidence

Result tests compare strict cardinality and exact volume/128-bit file identities,
including duplicate rejection and outside-scope exclusion. Search-wrapped PIDLs
can differ from ordinary filesystem PIDLs, so actual file identity is the
membership authority. Tests distinguish shallow/deep scopes, equal basenames
in different locations, recursive exclusions, Library unions, relative dates,
generic word matching and saved-query discovery of new files. See the
[saved-search verification](saved-search-verification.md) for the supported
condition and provider combinations.

Normal Shell commands retain their actual target array, provider and view site.
Fast native state is compared with installed context menus; full cascades are
enumerated when needed. Headless guards reject associated-app launches,
recipients, wizards, device operations, clipboard publication and native drops.
Mock handlers test routing, effects, failure and lifetime without treating a mock
invocation as proof that every installed extension works.

The [disposable hosted verification at `928dfcd`](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/37194199690)
passed its four isolated targets. Real native Undo/Redo passed in 2.79 seconds;
Contents/System/Compressed passed in 2.25 seconds. OFF/ON states came from the
actual `IExplorerCommand::GetState(FALSE)`, rather than guessed host flags. The
content-only file and ZIP member changed membership as required, and freshly
created searches matched the active native option state and exact owned results.
The fixture restored each original native state and registry value/type/absence.
This Windows Server 2022 run complements local Windows 10 evidence; it does not
establish identical rendering or arbitrary provider behavior on both systems.
The detailed method is in [native advanced search verification](search-options-verification.md).

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
