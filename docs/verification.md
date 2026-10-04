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

The latest functional Release checkpoint has SHA-256
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
