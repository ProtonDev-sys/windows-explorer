# Windows 11 Explorer alternative: research and execution plan

Research date: 2026-10-23. Status: proposed product direction and engineering plan.
This document does not certify a build or change the existing Windows 10 verification record.

## 1. Product contract

Build a native C++ file manager for people who know Windows Explorer and want faster, more predictable daily file work. Windows 11 provides the interaction baseline: familiar navigation, tabs, a compact command bar, native file behavior, and coherent light/dark/contrast themes. Improvements should reduce waiting, ambiguity, repeated navigation, and avoidable mistakes.

The primary audience is a Windows desktop user with large local folders, downloads, media, development trees, and occasional network/cloud locations. The initial emphasis is local x64 Windows 11. Retain Windows 10 behavior as documented compatibility work; decide its ongoing support commitment separately. ARM64 and device-specific integrations need their own validation.

“Better” is a scorecard: faster common workflows, fewer hangs/crashes, correct operations, lower resource cost, accessible interaction, and fewer steps for recurring tasks. Every claim needs a specified workload and comparison. C++ alone does not establish performance superiority.

Keep familiar single-pane browsing as the default. Make additional capability discoverable through a small set of useful controls. The initial release remains a standalone file manager alongside the Windows desktop shell. Desktop/taskbar replacement, changing system file dialogs, and registry interception of Explorer launches are separate decisions.

## 2. Research baseline and evidence limits

Microsoft's release table identifies Windows 11 **26H2, build 26300.9711**, as the current General Availability baseline on the research date. **25H2, build 26200.9711**, is a useful second compatibility target. Pin the actual OS patch, language, DPI, policy, extensions and feature rollout state in every future comparison. A build number alone does not establish that every staged feature is enabled. [Microsoft release information](https://learn.microsoft.com/en-us/windows/release-health/windows11-release-information).

Microsoft documents native tabs, Home, Gallery, modern Details, cloud status, archive support and modern context menus across its release/support material. These are part of the research baseline, rather than inventions of alternative managers. Additional account-dependent recommendation/activity surfaces need separate treatment. [Released Explorer changes](https://support.microsoft.com/en-us/servicing/os/configuration-updates/2023/08/september-26-2023-windows-configuration-update), [Explorer help](https://support.microsoft.com/en-us/windows/experience/fileexplorer/file-explorer-in-windows).

Microsoft's August 2026 announcement explicitly discusses context-menu delay, rename disruption and browsing/selection stalls. It introduces further menu customization as an Insider rollout; do not assume that announcement proves availability on the pinned release. This supports prioritizing those workflows, without supplying independent timing data. [Microsoft's performance and menu announcement](https://blogs.windows.com/windows-insider/2026/08/17/improving-file-explorer-context-menu-faster-simpler-and-more-customizable/).

This research used official documentation, release notes, published screenshots, a small public complaint sample, and read-only local source inspection. It included no app launch, build, native test run, SSH session, or desktop automation. T3 public-page screenshot capture timed out twice. Two published Microsoft support PNGs were downloaded and inspected headlessly under ignored `artifacts/research/windows11-20261023/`; they illustrate earlier Windows 11 layouts and are **not current-build pixel references**. A fresh reference set on the pinned Windows 11 runner is a milestone-0 deliverable. The optional Product Design context preflight could not execute because Python was unavailable; direct inspection found no saved context file.

## 3. Existing foundation: retain, separate, measure

Fresh local inspection found HEAD `566ce0784eb34e92a66e4717b55d78caaa5b48fa`, branch `feature/native-explorer-completion-20261006`, 15 modified tracked files and one untracked test header. Local tracking metadata reports seven commits behind origin; no fetch or remote synchronization was performed. Preserve this work and establish its provenance before implementation. No checked project/ancestor AGENTS.md was found; the conversation's instructions remain applicable.

| Area | Useful existing foundation | Required change or uncertainty |
| --- | --- | --- |
| Native integration | C++20, Win32, `IExplorerBrowser`, PIDLs and Shell item identity | Hosting Shell views inherits provider latency and some lifetime risks |
| Navigation | History, breadcrumbs, typed locations, native drop targets | Extract per-tab/per-pane session ownership |
| Commands | Capability snapshots, native availability, pending/error distinctions, revalidation | Remove Ribbon IDs and presentation dependencies from semantic commands |
| Operations | `IFileOperation`, per-item progress/error handling | Add job orchestration without weakening native semantics |
| Search | AQS, native search folders, refinements, saved searches | Add explicit search modes and coverage/status presentation |
| Verification | Owned fixtures, private-desktop hosts/rendering, native identity checks, benchmark scripts | Establish current green results and Windows 11-specific fixtures |
| Interface | Windows 10 installed/authored Ribbon | Build independent Windows 11 chrome; stop expanding Ribbon parity as the default direction |

Evidence: [architecture](architecture.md), [CMake targets](../CMakeLists.txt), [command model](../include/explorer/app_commands.hpp), [application owner](../include/explorer/app.hpp), [operations](../src/shell_operations.cpp), [Ribbon adapter](../src/ribbon.cpp).

The recorded verification includes unresolved native Ribbon teardown and QAT gesture failures. These historical failures do not establish the current uncommitted changes' result. They do prevent treating the checkout as a known-good baseline. [Verification](verification.md).

Historical v87 data records roughly 491 ms to observe selection of 10,000 items, but 8.27 seconds to complete all command states, including 7.21 seconds in native `QueryContextMenu`. A new toolbar cannot alone remove that provider cost. Existing measurements compare app revisions on Windows 10; they establish neither Windows 11 performance nor a speed advantage over Explorer.exe. [Measurement record and boundaries](performance.md).

## 4. What to retain from Windows 11, and what to improve

| Surface | Familiar baseline | Proposed improvement | Acceptance focus |
| --- | --- | --- | --- |
| Window/header | Tabs, navigation/address/search, compact commands | Reopen closed tab; lazy session restore; optional named workspaces | Independent history/selection, no startup dependency on inactive shares |
| Sidebar | Home, pins, known folders, This PC, Network, Recycle Bin | User-controlled order/visibility; obvious offline states | Stable canonical targets; no drive probing on startup critical path |
| Folder view | Details/icons, sort/group/columns, native selection/rename | Stable rows during refresh; deferred expensive metadata | Exact identity/selection and focus retained under file churn |
| Commands/menu | Cut/copy/paste/rename/delete/properties; extension access | Useful labels, customizable primary commands, deferred optional providers | Core actions ready quickly; pending/unsupported state remains truthful |
| Search | Scoped folder search and supported AQS | Distinct “Filter this view”, “Find filenames”, and “Search contents” modes | Visible scope/backend/completeness; cancellation and exact result sets |
| Transfers | Native progress, conflicts, recycle, supported undo | Job list, queue, clearer conflicts, retry of known failed items | Verified outcomes and partial failures; accurate destination |
| Details/preview | Metadata and provider previews | Fast optional preview with loading/error states | No obsolete preview after selection changes; cloud access remains explicit |
| Appearance/input | Windows typography, icons, density, themes, keyboard | Comfortable/compact density, clear focus, adaptable text | Keyboard/UIA, scaling, contrast, localization and RTL |

Released Windows 11 also supports creating 7z/TAR archives and archive extraction beyond ZIP; encrypted archive support has limitations. Define read, write, encryption and provider support independently rather than claiming “all archives.” [Released archive improvements](https://support.microsoft.com/en-us/windows/deployment/updates-lifecycle/inside-this-update), [Archive support and limits](https://support.microsoft.com/en-us/windows/experience/storage-filemanagement/zip-and-unzip-files).

The proposed visual hierarchy is tabs/title, navigation and editable breadcrumbs with search, a compact command row, then sidebar/content/optional details and a status footer. This is a design direction pending current-build reference measurement. Preserve system window resizing, snapping, caption behavior and focus conventions. Prefer installed Segoe UI Variable/Fluent assets and system theme values; use supported DWM materials with solid fallbacks. Avoid hardcoded colors and per-pixel styling that undermines accessibility. [Windows app design guidance](https://learn.microsoft.com/en-us/windows/apps/get-started/best-practices), [Supported Win32 backdrop API](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwm_systembackdrop_type).

## 5. Competitive lessons and ranked opportunities

| Reference | What it contributes | Product lesson |
| --- | --- | --- |
| Files | Familiar modern Windows UI, tabs, dual panes, column navigation | Optional power features can fit a familiar interface; its October 11, 2026 v4.3 release also targets startup/navigation/search/memory performance |
| Directory Opus | Copy queues, detailed conflicts, layouts, rename/viewer depth | Study operation safety and recurring workflows; control initial configuration complexity |
| OneCommander | Dual panes, tabs, Miller Columns and quick preview | Deep hierarchy navigation deserves an optional specialized view |
| Everything | Indexed filename discovery and a C/C++ SDK | Consider an optional filename-search adapter; retain a fallback and distinguish content search |

Sources: [Files v4.3](https://files.community/blog/posts/v4-3-0), [Files dual pane](https://www.files.community/docs/features/dual-pane), [Opus queues](https://docs.dopus.com/doku.php?id=file_operations:copying_moving_and_deleting_files:copy_queues), [Opus conflict comparison](https://docs.dopus.com/doku.php?id=file_operations:copying_moving_and_deleting_files:the_confirm_file_replace_dialog), [OneCommander features](https://www.onecommander.com/), [Everything SDK](https://www.voidtools.com/support/everything/sdk/), [Everything FAQ](https://www.voidtools.com/faq/).

The following ranking is a product judgment. Severity describes impact when the issue occurs; confidence describes applicability to this product. **Population frequency is unknown.** Official features establish feasibility precedents, not comparative usability or speed.

| Rank | User problem and goal | Impact / confidence | Recommended move and tradeoff |
| --- | --- | --- | --- |
| 1 | Browse/select/rename without waiting for metadata or an unavailable location | High / High | Progressive basic rows; cancel obsolete work; defer optional providers. Incomplete metadata needs a visible state |
| 2 | Understand where a transfer goes and which items actually succeeded | High / High | Native operations plus clear jobs/conflicts/failure summaries. Concurrency must consider shared resources |
| 3 | Know whether search is filtering, recursing or consulting an index | High / High | Explicit modes and scope/coverage. Index misses must not masquerade as exhaustive absence |
| 4 | Organize files between two locations without repeated navigation | Medium / High | Optional dual pane with strong active/destination cues. Preserve useful space in narrow windows |
| 5 | Return to recurring project folders | Medium / High | Persistent tabs/workspaces restored lazily. Bound memory and avoid reopening every network location eagerly |
| 6 | Rename a batch without surprising collisions | Medium / Medium | Proposed-name preview, collision checks, clear partial outcomes. Regex follows simpler rules |
| 7 | Inspect files without launching several apps | Medium / Medium | Fast optional preview; isolate handlers where supported. Observe cloud availability and download costs |
| 8 | Find large folders without slowing ordinary browsing | Low–Medium / Medium | Explicit cancellable folder-size calculation with incomplete/cached markers |

Everything's SDK requires the application to be running; Lite omits SDK/IPC. Its stable filename index is different from indexed content search. Defer building a new global indexer until integration and fallback measurements demonstrate a need. [SDK requirements](https://www.voidtools.com/support/everything/sdk/), [FAQ](https://www.voidtools.com/faq/).

### Complaint sample: test scenarios, not prevalence

- A September 2026 Explorer user described worsening navigation during image/archive churn. Their reported profile reset is one individual's diagnosis. Use file churn and profile-size fixtures; do not prescribe profile resets from this evidence. [Microsoft Q&A report](https://learn.microsoft.com/en-us/answers/questions/5991195/file-explorer-and-every-modern-open-save-dialog-be).
- Files issue #18710 reported interaction lag in July 2026 on 4.1.1.0 and was closed as a duplicate. It predates 4.3's performance work. [User report](https://github.com/files-community/Files/issues/18710), [later release](https://files.community/blog/posts/v4-3-0).
- An Opus support case investigated delayed UI and third-party hooks; module presence alone did not establish causality. A OneCommander case investigated video clipboard/menu stalls involving cloud integration. Exercise optional integrations under delay. [Opus support](https://resource.dopus.com/t/opus-slow/54439), [OneCommander support](https://groups.google.com/g/onecommander/c/9k-TMTz9CWk).
- An Everything 1.5 prerelease report on Windows 10 identified a blocked network-volume read. This is a useful unavailable-network scenario, not evidence against current stable Everything on Windows 11. [Developer/user discussion](https://www.voidtools.com/forum/viewtopic.php?t=17776).

Public evidence is a small convenience sample, with no user interviews, representative telemetry, controlled alternative-app benchmarks or internal support dataset. Pain severity and priorities should be revisited after measured use.

## 6. Feature tiers and parity accounting

**Daily-use baseline:** local drives; known folders; pins; This PC; Recycle Bin; browse/back/forward/up; editable addresses; details/icons; sort/group/columns; selection; rename; new items; copy/cut/paste/move/delete; recycle/restore where supported; drag/drop; associations/Open with/properties; tabs; keyboard routing; scoped search; preview/details; persistent preferences; theme/scaling/UIA support; error and empty states.

Known folders may already be redirected to SMB or backed by cloud placeholders. M1 must resolve their actual Shell targets, retain cached navigation while a target is unavailable, expose provider/offline/pending states, and allow another usable session to remain accessible. Do not enumerate inactive locations eagerly or hydrate content just to restore tabs or decorate rows. Explicit open/copy/preview may require provider retrieval; show that transition and respect provider consent/policy. Unsupported operations must remain unavailable with an explanation. Broader provider parity can come later; these basic failure/availability semantics cannot.

**Improvements after that baseline:** dual pane, closed-tab recovery, workspaces, transfer jobs/queue, richer conflict review, rename preview, optional Everything filename search, quick filter, useful shortcut discovery/command palette, on-demand folder sizes.

**Compatibility expansion:** Gallery collections, Libraries/saved searches, broader archives, cloud placeholders/overlays, provider sharing, network discovery/SMB, removable media, MTP/phones, provider-specific menus and enterprise/account surfaces. Existing implementations can be retained and revalidated earlier. This tier describes staged verification, not removal of existing capability.

**Later investigations:** Miller Columns, folder comparison/sync, hash-based duplicate tooling, tags/Git views, scripting/extensions and remote protocols. AI features have no demonstrated priority over file-management basics in the current evidence.

Before claiming Explorer parity, create a Windows 11 matrix with one row per behavior and separate columns for implemented, headlessly verified, OS/provider-dependent and known limits. Native virtual folders, account services and device integrations cannot be marked complete because a generic folder view opens. Gallery must remain an explicit incomplete parity row until verified. Use the existing Windows 10 matrix as inventory, not Windows 11 certification.

## 7. Native architecture and decision gates

Recommended starting point: **incremental C++/Win32 modernization**, keeping the Shell integration behind interfaces. Evaluate C++/WinUI 3 as a measured alternative for chrome. Both are native options; runtime choice needs evidence. WinUI offers platform styling/accessibility, while retaining Win32 may preserve the existing HWND/private-desktop test infrastructure. Test startup, memory, focus, child-window composition, DPI, deployment and headless rendering before choosing. [WinUI overview](https://learn.microsoft.com/en-us/windows/apps/winui), [XAML Islands integration responsibilities](https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/host-controls-existing-desktop-apps).

Proposed boundaries:

1. **Window/chrome:** tabs, commands, address/search, sidebar and status; no native operation implementation inside controls.
2. **Command service:** semantic IDs, capability snapshots and invocation revalidation. Expose pending/disabled/failure states consistently to every presentation.
3. **Browser session:** one tab/pane's location, travel history, selection, view state, search and cancellation generation. Avoid live view state leaking between sessions.
4. **Provider/view adapter:** retain `IExplorerBrowser` for compatibility. Spike a virtualized filesystem listing for large local directories if inherited provider stalls defeat budgets. Decide using both performance and a compatibility checklist; do not embark on two complete view engines immediately.
5. **Search service:** view filter, native AQS/content search and optional filename index. Report backend, scope and incomplete/offline status.
6. **Job service:** scheduling/history above `IFileOperation` and its progress sink. Persist intended/observed job state; reconcile after a crash before any retry.
7. **Optional helpers:** bounded thumbnail/preview/metadata work with compatible out-of-process hosting where possible. Workers return owned data or correctly marshalled COM interfaces, never borrowed apartment pointers.

M0 must record a containment ADR assigning every view, command provider, helper and operation to an apartment/process owner. The default policy is no forced thread termination, generation-tagged results rejected after session replacement, and no release of creator-owned objects while callbacks remain active. A deadline can mark a query unavailable; it cannot interrupt an arbitrary COM call. Use owned helper processes for optional work that needs enforceable isolation. An in-process Shell compatibility path must disclose its remaining hang/close limits. If an essential M1 path cannot meet the responsiveness/lifecycle gates with that design, change the view/provider boundary before accepting it. Process termination is not an operation-cancellation strategy; reconcile file outcomes after any abnormal worker loss.

`IExplorerBrowser` supplies native navigation/views, not a public API for embedding the entire modern Explorer interface. Its documented search-filter compatibility path can run on the UI thread; preserve the background-filtering contract. `IExplorerCommand` construction methods must remain fast and should not communicate with network resources. Some extensions are in-process and cannot be made safe by simply moving a call to a thread or applying a timeout. [ExplorerBrowser contract](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-iexplorerbrowser), [Command contract](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-iexplorercommand).

`IFileOperation` needs an STA. A dedicated operation STA with proper pumping/lifetimes is the starting design, not a generic MTA task. Queue control is different from pausing an active file operation: progress-sink timer methods do not provide supported pause/resume. `CopyFile2` is a possible specialized backend, but its security-attribute behavior differs. Prove semantics before changing the engine. [IFileOperation](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-ifileoperation), [Progress sink](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-ifileoperationprogresssink), [CopyFile2](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-copyfile2).

M1 retains native operation/conflict dialogs in production. Before expanding transfer UI, specify ownership and routing for conflicts, access denied, elevation, cancellation and partial success. Headless integration fixtures must exercise genuine supported decisions and verify resulting files through the project's isolated native harness. Suppressing prompts or installing an automatic replace policy does not certify the corresponding decision flow. Later application-owned decisions must use the same controller contract in production and deterministic fixtures. Elevation/secure-desktop flows remain explicitly uncovered until a suitable isolated runner can exercise them; never redirect them to the user's active desktop or report silent denial as success.

## 8. Measurable definition of better

These are **provisional engineering targets**, not achieved results. Milestone 0 must establish the target machine/workload and ratify or revise budgets with recorded rationale. Never lower a gate solely to turn a failing run green.

| Metric | Initial proposed budget | Measurement boundary |
| --- | --- | --- |
| Startup | Warm p95 <=750 ms; cold objective <=1.5 s | Controller launch request to correct usable native view plus core controls; separate image/runtime loading and first render |
| Local navigation | Warm p95 <=250 ms for 10,000 basic entries | Correct location, complete known count and ready core commands; record first batch separately |
| Input acknowledgment | p95 <=50 ms; investigate own-code UI stalls >100 ms | Queued command to observable state update, with background metadata/search/jobs active |
| Core context menu | p95 <=100 ms | Request to rendered actionable built-in entries; optional extension readiness measured separately |
| Selection and commands | Proposed core-ready p95 <=100 ms for small selections; <=500 ms at 5k/10k items | Selection request to exact selected identities/count and correct actionable core commands; pending/disabled placeholders do not satisfy readiness. Measure complete optional-provider discovery separately |
| Search | Indexed filename first results p95 <=150 ms on fixed 100k corpus | Excludes typing debounce; record final completion and exact expected set separately |
| Resource cost | Objective <=100 MiB private bytes for one settled 10k-entry tab; idle CPU <0.5% of one logical core | Include owned helper processes; bound caches and report each additional tab's cost |
| Reliability/correctness | No unexplained crash, false success, incorrect target or silent corruption in acceptance fixtures | Repeated lifecycle, selection, file churn, cancel/failure and recovery scenarios |

Use identical owned datasets and comparable released Explorer settings/hardware. Alternate benchmark order, serialize native tests, record AV/extensions/cache/storage conditions, retain raw distributions and at least 30 measured warm repetitions. Report cold runs separately; do not call a five-sample maximum a stable tail estimate. Include 10/1k/10k/100k-entry cases, mixed media, deep/Unicode/long paths, slow/offline shares and cloud placeholders.

A native Shell-host benchmark is not an Explorer.exe benchmark. Compare actual Explorer only when a dedicated isolated Windows runner can prove process/view ownership and equivalent measurement boundaries headlessly. Shell redirection must not attach tests to a user's visible session. If no valid Explorer measurement is available, publish app-only results and leave comparative claims open.

Aim for a material measured improvement, initially 2x on the worst repeatable daily-workflow bottlenecks, while investigating >10% regressions elsewhere. Record which workflows meet that goal. Throughput gains do not justify weakened permissions, integrity or cancellation behavior. Rendering to a private-desktop surface does not establish physical display latency or smoothness.

## 9. Headless acceptance strategy

All execution/testing remains headless. Use the project's native harness, owned disposable fixtures and isolated Windows runners/private desktops; never switch the input desktop or automate a user's mouse/keyboard. Cross-process tests must prove owned process/thread/window identity. Serialize tests that share native providers or desktop assumptions.

| Layer | Required evidence |
| --- | --- |
| Models/services | Session isolation, cancellation generations, command state, persistence bounds, search scopes, job state transitions |
| Native integration | PIDL/FileID identity, exact selection complements, COM apartment/lifetime behavior, real Shell operations and provider state |
| File outcomes | Expected names/counts/hashes and required attributes; locked files, permissions, name conflicts, partial copies, disk-full and cancellation |
| Views/accessibility | Native render/state snapshots, UIA names/roles/patterns, focus order and accelerators through the test harness |
| Presentation | Pinned references and app-native offscreen captures at selected sizes, 96/144/192 DPI, light/dark/contrast, 200% text and RTL |
| Performance/reliability | Timings above plus repeated open/close/tab switch, worker teardown, resource growth and slow-provider injection |

Include clipboard formats, drop target/effect, source/destination identity, selection while refresh/rename happens, reparse points/hard links, case-only changes, archive path traversal, stale search results, preview cancellation, network loss and cloud hydration boundaries. Validate supported undo; a job journal is not a promise of universal rollback or resumable copies.

Automated UIA and image checks provide bounded evidence. They do not certify real Narrator speech, physical touch/drag feel, HDR/monitor composition or arbitrary installed extensions. Record those limits; do not substitute screenshot similarity for functional or accessibility coverage. [Microsoft accessibility guidance](https://learn.microsoft.com/en-us/windows/apps/design/accessibility/accessibility-overview).

Every acceptance report must identify source commit plus dirty diff/hash, executable/helper hashes, exact OS/build, settings, fixture and sample size. Preserve failures. Existing passing reports cannot be assigned to the new interface.

## 10. Delivery sequence and exit gates

| Milestone | Deliverable | Exit gate |
| --- | --- | --- |
| M0 — Baseline and decisions | First: preserve source and obtain a green named core subset. Then: pinned Win11 behavior/reference inventory, performance harness, chrome/view spikes and ADRs | File identity, operations and shared lifecycle subset passes reproducibly on identified binaries; remaining Ribbon-only failures isolated/documented; measurement, containment, operation-decision and framework/view choices recorded |
| M1 — Familiar daily browser | Presentation-independent commands/session ownership; Win11 chrome; tabs; local browsing and basic operations; themes/keyboard/UIA | Complete core journeys with no regression in file identity, operations or lifecycle; provisional responsiveness gates met |
| M2 — Faster file work | Explicit search modes; transfer jobs/conflicts; large-folder and metadata work; batch-rename preview | Exact search/outcome fixtures; cancel/partial failure; measured bottleneck improvement and resource bounds |
| M3 — Power with low friction | Optional dual pane, workspaces, quick preview, on-demand sizes, command discovery | Destination/focus unambiguous; lazy restore and cache limits; baseline performance retained |
| M4 — Compatibility and release | Complete parity matrix including Gallery/archives/provider surfaces; packaging/update path; release evidence | Declared supported matrix passes; omissions/provider limits published; artifacts match immutable test reports |

These are dependency stages, not calendar promises. Estimate each after M0's measured spikes. Native compatibility can proceed alongside later stages once session/chrome seams are stable.

M0 has two bounded parts. **M0a** ends with a source manifest, binary hashes, the named green core subset and a failure ledger; unresolved shared-lifecycle or file-safety failures block its exit. **M0b** performs references, comparative measurements and architecture spikes after M0a. Explaining a shared failure is not equivalent to passing its gate.

## 11. First implementation work package

1. Inventory and preserve the existing dirty work; locate the exact remote continuation only when implementation resumes. Do not pull/reset/stash over it.
2. Complete M0a: establish a reproducible source/binary baseline and green core identity/operations/shared-lifecycle subset; triage command-state failures and document isolated Ribbon-specific failures. Resolve shared safety/lifetime failures before extraction or chrome experiments.
3. Add the Windows 11 behavior matrix and current-build references on an isolated headless runner. Prioritize folder browsing, tabs, right-click, rename, transfer conflicts and search.
4. Define semantic commands and a per-session state model; extract them while retaining the existing behavior as a regression oracle.
5. Spike one real tabbed window, one large local folder, one slow provider and a safe copy using the chrome candidates. Compare headless renderability, focus/UIA, startup/memory and provider coupling.
6. Record framework and view ADRs, freeze the first daily-use acceptance scope, then implement M1 in small reviewable changes.

This sequence is the recommended next step after this research turn. The present turn adds this plan only; it does not start those implementation tasks or revise historical verification claims.
