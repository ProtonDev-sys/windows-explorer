# Native navigation and selection measurements

The application was measured headlessly on Windows 10 22H2, build 19045, on
2026-10-04. These measurements compare revisions of this application. They
do not compare it with Explorer.exe or establish performance on other machines.

The first implementation eagerly constructed registered Shell command menus for
every navigation and selection change. The current implementation reads native
command state once per change and constructs full provider menus when requested.
Native Ribbon property callbacks use cached results.

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

The benchmark now includes real selection commands over the 10,000-file view.
Native selection providers and coalesced updates replaced repeated per-row
host calls. The latest run independently verified every resulting count:

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

The report records six real native view changes, twenty cached command
updates and four selection transitions, plus process working-set and
private-memory counters. Selection timings separate command execution,
message-pump work and independent native count readback; their sum must match
the count-visible total. A separate bounded phase waits until all asynchronous
command states complete and records both extra wait and total readiness time.
Reaching the correct selection count alone cannot satisfy that phase. Cleanup releases
the native browser before deleting the owned fixture. The input desktop is
verified again after application and COM cleanup.

Reports remain under `artifacts/performance/`. The earlier report is
`run-20261004-084212-892fc04cebf04bf7af7b30b04bdc2091/native-navigation.json`;
the later report is
`run-20261004-093710-110825ccef4d4c9b9b4d882773cbbd2b/native-navigation.json`.
The selection baseline is
`run-20261004-132920-61e8f64a659b4b5b8e1d6884a825fee8/native-navigation.json`, and the
latest complete native-resource run is
`run-20261004-154506-c2c2487cbad04d969eaa5da64f0e3625/native-navigation.json`.
