# Windows 11 direction: first native implementation

The first implementation replaces the default Ribbon window with a compact native command bar and a tabbed frame. Each tab owns a separate `ExplorerApp` and actual `IExplorerBrowser`, including its navigation history, search, view, selection and teardown. `--classic` retains the earlier Ribbon interface.

This is an implementation checkpoint toward the [product plan](windows-11-product-plan.md), not a declaration of complete Windows 11 parity or improved benchmark results.

## Implemented behavior

- Native tab strip, New/Close buttons and stable tab identities; up to 32 browser sessions per window.
- Ctrl+T, Ctrl+W, Ctrl+Tab, Ctrl+Shift+Tab and Ctrl+1 through Ctrl+9; frame fullscreen and focus traversal.
- Labelled native command buttons with public font glyphs, theme hooks, accessibility providers and wrapping at narrow widths. Sort, View and More retain the existing command authority.
- Independent folder locations, Back/Forward history and selection. Inactive close preserves search focus. Nested activation gives the newest selection authority. Queued closes choose a live survivor.
- Retained COM owners across native callbacks, snapshot-based child dispatch, deferred frame reconciliation and rollback of partially constructed native windows.
- Compact selection actions use the existing documented bulk folder-view implementation. They do not build a CommandStore menu merely to select files. This avoids the native menu crash reproduced in the unchanged baseline on the local Windows 11 installation.
- Compact command discovery queries the commands it presents, rather than activating every legacy contextual Ribbon provider. Optional extension discovery and native context menus remain provider-owned.

The UI remains Win32/C++20. The executable embeds the C++ runtime and requires no web runtime. The existing Shell operations, namespace handling and search infrastructure are retained.

## Headless checks

The native fixtures initialize a guarded private non-input desktop before COM or window creation. They use actual native controls and Shell views without switching desktops or injecting system input.

| Check | Evidence exercised |
| --- | --- |
| `core_and_shell_operations` | Existing file identity, operation outcomes and worker lifecycle subset |
| `native_browser_tabs` | Stable IDs, native selection/notifications, button dispatch, accessibility, mutation failure atomicity, narrow/DPI layout and parent-first destruction |
| `native_modern_command_bar` | Native command/dropdown routing, enabled state, accessible names, 96/144/192 DPI wrapping and HWND/resource lifetime |
| `native_modern_window` | Actual folder navigation, exact selection counts/complements, independent history, inactive-close focus, reentrant activation/close, queued adjacent closes, allocation rollback and complete teardown |

### Verified checkpoint: 23 October 2026

Source commit: [`ec2600adc7d8c78811e1c852279c603a3d056bd9`](https://github.com/ProtonDev-sys/windows-explorer/commit/ec2600adc7d8c78811e1c852279c603a3d056bd9), on `codex/modern-native-tabs-20261023`.

[Targeted CI run 39984101758](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/39984101758) configured and compiled the application and test binaries with MSVC 19.44.35229 and SDK 10.0.26100.0. All four targeted tests passed on the Windows Server 2022 runner, with no skipped targeted checks.

The exact downloaded binaries also passed all four checks locally on Microsoft Windows 11 Home, build **26300.9711**, without timeouts:

| Local check | Result | Fixture elapsed time |
| --- | --- | --- |
| Core and Shell operations | Passed | 6.71 s |
| Browser tabs | Passed; 394 assertions | 0.05 s |
| Modern command bar | Passed | 0.04 s |
| Modern window | Passed; actual navigation, selection, queued closes and construction rollback | 1.28 s |

These times describe the test fixtures, not a performance comparison with Explorer. The native fixtures assert unchanged input desktop and owned window isolation.

`WindowsExplorer.exe` SHA256:

```text
AE351A33C12BD6C066AA1ADA173A5DBBA254569A8BD702915AE56864AE7CACFD
```

The downloaded binary archive matched GitHub's SHA256 `43214af1151cc534046f46e3af0bfd90faabf7762c399818634fc1bdc77a5818`. Locally, `artifacts/modern-native-ci-ec2600a/` preserves `source.zip`, `provenance.json`, `verification.json`, exact binaries, CI JUnit/log evidence and each local process's exit code, executable hash and stdout/stderr. The source ZIP SHA256 is `48F4BD29FC60F04D58F93E3FD12E3DF59297F4140D27638818AC6648906277E2`.

The [broad workflow for this commit](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/39984101911) also passed its broad behavioral test step and its isolated native transfer/destination-drop job. Later classic-interface captures and navigation benchmarks were still running at this checkpoint. Those captures do not certify the compact interface's visual parity.

Earlier passing reports do not certify later code changes. Subsequent documentation-only commits retain the code and binaries identified above.

### Retained failure ledger

The immutable pre-implementation baseline (`566ce07` plus its preserved dirty patch) passed the local core subset. Its native application-command and registered selection-menu checks crashed on Windows 11. The first compact implementation also crashed while constructing the registered selection menu. Compact mode now uses the existing bulk folder-view route for selection actions; classic mode and arbitrary menu extensions are not thereby certified.

The first new tab-control test failed because it clicked zero-sized native buttons before layout. The corrected fixture lays out the real hidden controls and still checks actual parent HWND/command notifications. It does not replace dispatch with a synthetic parent notification.

An earlier broad CI run also missed the existing backed-search fixture's five-second completion gate. The subsequent broad behavioral run passed. The failed run remains preserved at [39981233881](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/39981233881); the deadline was not relaxed.

Construction rollback is exercised by throwing after creation of a real private-desktop browser, with and without first sending synchronous `WM_CLOSE`. Allocation failure inside the later reconciliation itself is covered by exception-boundary inspection rather than a separate injected runtime case.

## Remaining scope

Session restoration, tab reordering/tear-off, dual pane, transfer queues, new search modes and batch rename are still roadmap work. The command implementation still shares legacy Ribbon identifiers; a presentation-independent command service remains an architectural follow-up.

Arbitrary extensions can still block or fail inside the retained Shell integration. This checkpoint does not establish process containment, bounded latency for every provider, real screen-reader behavior, or a speed advantage over Explorer. Windows 11 visual parity and performance targets require their own final-build evidence.

The broad regression suite remains a separate gate from the four targeted checks. Its outcomes, skips and unresolved failures must be reported explicitly.
