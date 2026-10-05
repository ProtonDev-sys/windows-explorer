# Native navigation and selection measurements

The application was measured headlessly on Windows 10 22H2, build 19045, on
2026-10-04 and 2026-10-05. These measurements compare revisions of this application. They
do not compare it with Explorer.exe or establish performance on other machines.

The first implementation eagerly constructed registered Shell command menus for
every navigation and selection change. The current implementation reads native
command state once per change and constructs full provider menus when requested.
Native Ribbon property callbacks use cached results.

## Interleaved optimization candidate

On 2026-10-05, twelve alternating-order baseline/candidate rounds ran serially
between 17:34 and 17:46 UTC, with no concurrent owned build or native probe.
Baseline source is `f909f69`; the candidate adds bounded parent-attribute
enumeration and paired native icon extraction. These measurements predate the
subsequent ownership/error/fallback revisions and their additional edge tests;
they do not validate that revised source. The unchanged measured executables
are baseline `38f1de781933ee2c99540c7ad49a89ee995662134e00bcef87b3efba1d840012`
and candidate `00f330dc92012a4e917f83fb9edf62ec3def138d6eaf30b988a5d8fa4158e4a2`.
Every report passes the benchmark's native selection, readiness and isolation
checks on Windows 10 Home 19045.6466.

The following medians have twelve independent process runs per side. Intervals
are the analysis script's 10,000-resample bootstrap for baseline minus candidate;
permutation values use 20,000 randomized label assignments. They are exploratory,
unadjusted comparisons across multiple metrics, not universal performance bounds.

| Metric | Baseline → candidate (ms) | 95% interval for difference (ms) | Permutation p |
| --- | ---: | ---: | ---: |
| Select all: namespace preparation | 212.5 → 22.1 | [179.2, 217.2] | 0/20,000 exceedances |
| Select all: creator host work | 661.7 → 427.7 | [33.6, 561.4] | 0.014 |
| Select all: creator queue drained | 1,446.2 → 1,000.5 | [-153.9, 842.2] | 0.061 |
| Select all: all command states ready | 7,123.1 → 6,973.3 | [-343.8, 884.9] | 0.617 |
| Invert none to all: namespace preparation | 207.2 → 22.0 | [177.8, 199.2] | 0/20,000 exceedances |
| Invert none to all: creator host work | 602.6 → 384.2 | [125.3, 284.6] | 0.001 |
| Invert none to all: creator queue drained | 938.2 → 795.5 | [-143.3, 341.7] | 0.079 |
| Invert none to all: all command states ready | 6,560.9 → 6,303.8 | [-168.3, 532.6] | 0.222 |
| Startup: native view ready | 704.0 → 659.2 | [-118.0, 294.4] | 0.634 |
| Navigation median: 10 files | 133.8 → 133.8 | [-50.9, 48.1] | 1.000 |
| Navigation median: 1,000 files | 136.8 → 197.3 | [-75.0, 0.2] | 0.050 |
| Navigation median: 10,000 files | 143.2 → 122.5 | [-55.2, 62.5] | 0.714 |

The namespace and creator-host phases improve in this experiment. Complete
command readiness, startup and queue-drained timings remain inconclusive.
The 1,000-file navigation result is a possible regression requiring investigation;
slow run episodes and the unchanged navigation code do not rule it out. No
samples were filtered. Private memory is 51.9 → 52.6 MB with a difference interval
of [-1.4, 0.7] MB and p=0.241. Invert all to none and Select none remain
inconclusive. The second batch reduces the apparent significance of some
first-batch queue improvements, so the combined results take precedence.

A separate eight-process-per-mode icon microbenchmark compares all 23 startup
specifications at 32/16 pixels through the production DrawIconEx path. All modes
are byte-identical across 117,760 compared bytes. Separate extraction's median
is 16.34 ms; paired extraction's is 11.70 ms. Keeping source modules mapped
gives no observed gain and is not used. This does not establish the production
cache's behavior at every DPI or a significant startup improvement.

Retained report lists are
`D:/Development/fe-micro/ab-20261005-173408/runs.txt` and
`D:/Development/fe-micro/ab-20261005-174056/runs.txt`; the reproducible calculation
is `D:/Development/fe-micro/analyze.py`. Each copied executable's actual hash
matches its environment record. Source changes require their own correctness
and timing validation before integration.

## Latest isolated main-checkout run

The 2026-10-05 17:12 UTC installed-layout v48 run used executable
`9cea818574f6301b47becd3edcbfd419fe94bb525e35506a180d1433b3dc4573`.
The accepted report retains complete native selections, command readiness and
887 desktop-isolation observations:
`artifacts/performance/run-20261005-171218-848677e46b2e4cb2886a9b0ab3f7ecbd/native-navigation.json`.
No other owned native probe or build ran concurrently. Startup native-view
readiness was 591.360 ms, with 51,867,648 private bytes. Navigation p95 was
131.176 / 168.035 / 886.978 ms for 10 / 1,000 / 10,000 items. The final
10,000-item sample took 886.978 ms; this slower observation remains in the
report and prevents treating this isolated run as a navigation speedup.

| Transition over 10,000 files | Actual count observed immediately after command | Creator queue drained | All native command states ready |
| --- | ---: | ---: | ---: |
| Select all | 10,000 at 450.869 ms | 1,136.775 ms | 6,984.496 ms |
| Invert all to none | 0 at 1.286 ms | 32.178 ms | 138.514 ms |
| Invert none to all | 10,000 at 220.692 ms | 856.546 ms | 6,440.745 ms |
| Select none | 0 at 1.197 ms | 35.895 ms | 154.796 ms |

This revision reuses exact-target native GIT registrations for queued state
work. Its separate lifetime regressions passed after correcting two assumptions
about native marshalling and implicit MTA behavior; the complete core target
passed at v50. The preceding v48 complete suite had three failing core
assertions, so its accepted performance report is not a complete functional
certification. Interleaved baseline/candidate measurements are required before
attributing a timing change to an optimization.

## Previous complete local checkpoint

The 2026-10-05 15:42 UTC installed-layout v42 run used executable
`e5796664dcfdc48d4e2081f85d3093fefac2880e4993ecd508ebc9fc9bf2e07d`,
matching the complete seventeen-active-test local checkpoint. No other owned
native probe or build ran concurrently. The three native folder cases, complete
selection-state checks and 853 desktop-isolation observations passed. Report:
`artifacts/performance/run-20261005-154226-0c040b1504b445b3b87b8b24fb00bfa6/native-navigation.json`.
Its environment records Windows 10 Home 22H2 build 19045.6466 and the native
binary versions without JSON truncation.

The first verified native startup view was ready at 758.411 ms; private-desktop
setup was ready at 2.950 ms, platform initialization at 4.681 ms and application
creation returned at 757.357 ms. Navigation p95 was 120.430 / 153.477 / 146.275 ms
for 10 / 1,000 / 10,000 files. Private memory was 51,982,336 bytes.

| Transition over 10,000 files | Actual count observed immediately after command | Creator queue drained | All native command states ready |
| --- | ---: | ---: | ---: |
| Select all | 10,000 at 1,004.726 ms | 2,187.767 ms | 7,919.048 ms |
| Invert all to none | 0 at 0.780 ms | 29.056 ms | 123.864 ms |
| Invert none to all | 10,000 at 201.164 ms | 1,801.294 ms | 7,242.882 ms |
| Select none | 0 at 0.847 ms | 29.469 ms | 139.202 ms |

Complete-selection native menu queries took 5,387.133 / 4,933.354 ms, with worker
totals of 5,961.304 / 5,832.496 ms. Each transition into a complete selection
changed generation once, observed two equivalent-selection notifications and
no uncertain notifications. Cached command-update samples were 0.095–0.201 ms.
The slower immediate Select-all sample is retained; these are measurements of
this app on this machine, not proof of universal optimality or a stock Explorer
comparison.

## Previous isolated run

The 2026-10-05 12:45 UTC installed-layout run used immutable v25 executable
`ed9ac231ae0bc70eb1aebb1acc796eb2ddf52cfe4396f1c3986f418bdfa463a6`.
No other owned native probe or build ran concurrently. All native result,
state-readiness and desktop-isolation checks passed. Its report is
`artifacts/performance/run-20261005-124506-27d9f15a1703447fa7ba7e23cff68be3/native-navigation.json`.

Process-entry startup reached the verified initial native folder view in
532.891 ms. The private desktop was ready at 2.722 ms, platform initialization
at 3.983 ms, and application creation returned at 531.990 ms. This measures the
first verified view observation; it excludes executable image loading and
visible painting. It does not replace the separate functional or visual gates.

| Transition over 10,000 files | Actual count observed immediately after command | Creator queue drained | All native command states ready |
| --- | ---: | ---: | ---: |
| Select all | 10,000 at 427.114 ms | 1,734.145 ms | 7,103.712 ms |
| Invert all to none | 0 at 0.877 ms | 29.927 ms | 152.363 ms |
| Invert none to all | 10,000 at 198.397 ms | 1,687.229 ms | 7,069.342 ms |
| Select none | 0 at 0.891 ms | 32.596 ms | 152.579 ms |

The state worker now requests the documented `DFMR_NO_RESOURCE_VERBS` only
for eligible non-resource leaves, retaining dynamic handlers, original site
and complete selection. It restores the original restrictions before reading
states. Native full-menu equivalence passed independently on 1/2/16/5,001/10,000-item
all-file and final-folder arrays. Normal menus remain complete.
Default-batch menu queries took 4,955.141 / 4,925.767 ms, compared with
26,605.896 / 26,641.145 ms in the earlier 04:04 isolated run. Total state readiness
fell from 31,141.138 / 30,595.799 ms to 7,103.712 / 7,069.342 ms. These are
measured app revisions, not an Explorer.exe comparison.

Navigation p95 was 113.435 / 153.842 / 212.296 ms for 10/1,000/10,000 files;
private memory was 52,137,984 bytes. Both large-selection transitions caused
one generation change, two equivalent-selection notifications and zero uncertain
notifications. Immediate count readback is a new observation before queue
draining; earlier reports did not instrument that boundary. Its small read cost
is included in the deferred-work phase. Historical measurements below retain
their original boundaries. This run made 835 desktop-isolation observations.
The preceding 12:12 v23 run measured 884.084 / 199.809 ms at the immediate
large-selection boundary and 7,650.251 / 6,805.953 ms at complete command
readiness. These separate samples retain system and cache variation; the newer
sample is not a further speedup claim. The complete functional suite and all
nineteen native captures passed independently on that earlier v23 executable;
the v25 performance pass does not certify its pending RTL fixes.

The 2026-10-05 expanded selection regression also measures the documented
fallback without a native command facade. Repeated indexed PIDL reads took
approximately 4.06–4.70 seconds for its 10,000-file SelectAll call. A batched
`IEnumIDList` snapshot with `SVGIO_FLAG_VIEWORDER` measured 171.407 ms in the
subsequent local run; independent exact identity readback took 817.315 ms.
The full selection suite passed in 11.77 seconds and retained exact PIDL and
volume/FileID sets, complements, focus, checkbox flags, clipboard and isolation
checks. These are development samples with concurrent-system effects, and do
not by themselves establish asynchronous command readiness. The later
completed benchmark below measures that boundary independently.
[Native view-order enumeration](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/ne-shobjidl_core-_svgio),
[batched child PIDL retrieval](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ienumidlist-next).

| Owned folder | Earlier navigation p95 | Cached command implementation p95 |
| --- | ---: | ---: |
| 10 files | 2,536.104 ms | 205.252 ms |
| 1,000 files | 1,694.527 ms | 233.443 ms |
| 10,000 files | 1,406.838 ms | 267.418 ms |

A later native-resource build completed the same navigation fixture with p95
latencies of 161.184, 120.409 and 132.935 ms respectively. Its private memory
was 46,075,904 bytes. The twenty cached command updates took 0.108–1.329 ms.
The corresponding development executable SHA-256 is
`065e982d2acc9f7c8bb7bc77ab9a54a6e0aa5af3b0c041fb50947a54084a0876`.
These snapshots ran during development on the same machine; concurrent work
and warm Shell caches limit causal comparisons between individual samples.

The October 4, 2026 23:11 UTC run of executable
`f01bc69d57541f86c432ff1055936b1331d801000852165524a22975f2b8a6ea`
failed the existing 45-second command-readiness wait after native Select all
over the 10,000-item view. The operation itself returned `S_OK`, but this run
produced no complete accepted performance report. Historical navigation or
selection timings cannot establish responsiveness of this newer build. The
failure is retained under ignored
`run-20261004-231115-8478a22bfdf3466aae4875b4167cc3f0`; subsequent diagnostics
record the exact pending command IDs and state rather than extending the bound.

The October 5, 2026 03:25 UTC installed-layout run completed the existing
protocol and all actual provider states within its unchanged bounds. Executable
SHA-256 was `9502121f364681b4e15f69d4d342ddc8fdb6e71f38fd3388bc88730c78179e3d`;
the immutable report is retained under ignored
`run-20261005-032509-5d8d2789f8064660b6484f963b028833`. On build 19045, native
navigation p95 was 207.981, 160.842 and 131.234 ms for 10, 1,000 and 10,000 files;
private memory was 50,540,544 bytes. The run made 28,204 desktop-isolation
observations, retained the original input desktop and exposed no owned window
there. No other owned probe or shared build ran during this sample.

Duplicate current-view selection notifications had previously cancelled and
restarted command-state workers. The host now compares the complete ordered
native PIDL selection, its actual aggregate attributes and retained view before
invalidating that work. Same-count item replacement, genuine rename/state
notifications and clipboard/navigation changes still rebuild. Both native App
layouts independently passed the real-selection regressions for those cases.

| Action over 10,000 files | Count observed after creator queue drain | All native command states ready |
| --- | ---: | ---: |
| Select all | 17,256.426 ms | 43,555.022 ms |
| Invert all to none | 43.193 ms | 141.184 ms |
| Invert none to all | 24,423.061 ms | 34,295.741 ms |
| Select none | 34.249 ms | 142.639 ms |

Completion fixes the earlier unbounded cancellation loop; these large-selection
latencies remain optimization targets. This count boundary follows draining
queued native/UI work, while command readiness additionally waits for every
asynchronous state. Neither boundary establishes a speedup over Explorer.exe.

The subsequent quiet installed-layout run at 04:04 UTC used executable
`4394242219da7f8bc4f4a003756ee6985312f2c8a610941e437651cc4a7dff41` and completed
the same protocol. Its report is
`run-20261005-040419-725382f45b7440598f0e70ae1558d605/native-navigation.json`.
Select all's post-drain exact count was observed at 23,391.745 ms and all command states in
31,141.138 ms; invert none to all measured 23,101.913 and 30,595.799 ms.
Each caused exactly one command-generation change, two equivalent-selection
refreshes and zero uncertain-selection refreshes. No other owned probe or build
ran during this sample.

Real completed-worker profiling locates the remaining delay in native menu
construction. The default selection batch's `QueryContextMenu` took
26,605.896 / 26,641.145 ms; complete native identity construction took
6.993 / 5.258 ms, data-object export 0.455 / 0.522 ms, and context binding
0.611 / 0.662 ms. Menu enumeration and state reduction together remained
below 0.5 ms. Creator-thread command-update totals were 1,106.749 / 1,048.260 ms.
These phases overlap with actual native/UI work and cannot be added to infer
the post-drain count-observation latency. The report retains each worker's real HRESULT,
selection-batch flag and nonblocking timing-readback status. The separate
Remove-properties provider returned native `ERROR_NOT_SUPPORTED` after
2,540.125 / 2,475.109 ms of menu construction; unsupported native state is
retained rather than presented as an enabled command.

The benchmark now includes real selection commands over the 10,000-file view.
Native selection providers and coalesced updates replaced repeated per-row
host calls. The earlier October 4 run independently verified every resulting count:

| Action | Earlier host path | Latest native path | Native command | Deferred UI work |
| --- | ---: | ---: | ---: | ---: |
| Select all | 5,819.068 ms | 1,967.134 ms | 410.437 ms | 1,556.693 ms |
| Invert all to none | 8,882.798 ms | 413.213 ms | 395.894 ms | 17.318 ms |
| Invert none to all | 9,678.068 ms | 780.277 ms | 169.760 ms | 610.516 ms |
| Select none | 13.940 ms | 213.121 ms | 198.869 ms | 14.251 ms |

Native menu preparation adds overhead to Select none compared with the earlier
single public view call. The first Select all still spends most of its time in
queued Shell/host work. These costs remain measured optimization targets. A
separate private-desktop correctness test verifies complete native item
identities for all, none, and a sparse selection's complement, together with
focus, checkbox flags, unchanged files and desktop isolation. Counts alone are
not that correctness proof.

Private memory after the benchmark changed from 71,544,832 to 37,318,656 bytes.
The later run's twenty cached command updates took 0.137–0.731 ms each. Native
view changes over the 10,000-file folder took 19.759–196.372 ms in that run.

The later executable SHA-256 was
`0163190e102f0038af4fa1bd7bd6159287d17d03228de0518ed2e6ab530bdabc`.
It is a development snapshot, not a release checksum. The machine reported
16 logical processors. Other processes, Shell caches, antivirus, storage, and
installed extensions can affect these values.

## Method and reproduction

```powershell
./scripts/build.ps1 -Configuration Release
./scripts/benchmark.ps1 -Configuration Release
# Target Windows 10 installed resource adapter:
./scripts/benchmark.ps1 -Configuration Release -InstalledRibbon
```

The script copies the executable into a unique ignored artifact directory and
records its checksum and environment. The executable creates a private Windows
desktop before COM initialization and never switches the input desktop. It
creates new owned folders containing 10, 1,000, and 10,000 small text files.
Each folder receives five real `IExplorerBrowser` navigations, with navigation
to an empty owned folder between samples. Filesystem and OS caches are not
flushed, so this is a first-navigation-plus-warm-samples measurement. With five
samples, the reported p95 is the maximum sample.

Navigation latency ends when navigation completes and the message pump returns.
The report also records the native navigation-complete callback, first item,
and complete item-count timestamps. The complete count must equal the fixture's
known count. In these runs, the first observation already contained every item;
the first-item timestamp is an observation, not an instrumented earliest Shell
render timestamp. Hidden capture and visible interaction are separate from this
navigation benchmark.

Startup uses the native performance-counter sample at `wWinMain` entry and
records private-desktop, platform and application-creation observations on the
same clock. A bounded STA pump then reads the actual view's native folder PIDL
and compares its canonical Shell identity with the retained requested target.
The view, completed-navigation count and current PIDL must remain unchanged
across those COM calls. Native callbacks can reenter navigation, so an earlier
view or replaced target cannot satisfy readiness. The script checks complete,
finite, nonnegative and chronologically ordered phase fields.

The report records six real native view changes, twenty cached command
updates and four selection transitions, plus process working-set and
private-memory counters. Selection timings separate command execution,
message-pump work and independent native count readback; their sum must match
the post-drain total. A separate bounded phase waits until all asynchronous
command states complete and records both extra wait and total readiness time.
Reaching the correct selection count alone cannot satisfy that phase. New reports also
record the actual native count immediately after command execution, before draining
creator callbacks, with its observation time and readback cost. That sample separates
selection completion from the later host/provider work without claiming an earliest
paint timestamp. Its read cost is included in the deferred-work phase. Cleanup releases
the native browser before deleting the owned fixture. The input desktop is
verified again after application and COM cleanup.

Reports remain under `artifacts/performance/`. The earlier report is
`run-20261004-084212-892fc04cebf04bf7af7b30b04bdc2091/native-navigation.json`;
the later report is
`run-20261004-093710-110825ccef4d4c9b9b4d882773cbbd2b/native-navigation.json`.
The selection baseline is
`run-20261004-132920-61e8f64a659b4b5b8e1d6884a825fee8/native-navigation.json`, and the
October 4 native-resource run is
`run-20261004-154506-c2c2487cbad04d969eaa5da64f0e3625/native-navigation.json`.
