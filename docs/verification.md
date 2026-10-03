# Headless verification

Verified on 2026-10-04 on Windows 10 Home 22H2 (build 19045), using Visual Studio 2022 Build Tools (MSVC 19.44), Windows SDK 10.0.26100.0, and a Release x64 build with a static C++ runtime.

```powershell
./scripts/build.ps1 -Configuration Release
./scripts/test.ps1 -Configuration Release -SkipBuild
```

The build completed without compiler warnings. CTest passed both targets. The separate hidden-host report passed **39 of 39 checks**; the shortcut suite passed **356 assertions** across five groups.

## Tested behavior

| Area | Evidence |
| --- | --- |
| Host and visibility | Real Win32/Shell initialization; application stays hidden; process-owned visible-window observation during message pumping |
| Folder fixture | 1,000 text files, nested/Unicode folders, a hidden file, and a protected hidden/system file; exact view counts and cleanup |
| Presentation | Read back all eight native modes and icon sizes; sorting direction/key, grouping/removal, checkbox flags, native visible columns; collapsed ribbon reclaims space |
| Navigation | Breadcrumb/history construction, subfolder, back, forward, up, invalid location, This PC namespace |
| Hidden state | Show/hide normal hidden items while protected hidden/system items remain excluded; reset after search |
| Panes | Preview/Details policy and mutual exclusion; native preview content rendering is not asserted |
| Search | Native AQS search-folder navigation and background-filtering flag; index completeness is not asserted |
| Settings and helpers | Unicode round trips, atomic replacement, invalid/corrupt/bounded values, reserved Windows names, environment expansion, search-URI encoding, byte formatting, HRESULT fallback |
| Shell operations | Silent copy/collision, move, Unicode rename/new-folder, reserved-name rejection, permanent deletion restricted to fixtures; existing/source files preserved |
| ZIPs | Compression and Unicode round trips, nested/empty folders, read-only archive, missing/invalid inputs, collisions/no overwrite, self-inclusion rejection, unsafe/link/unsupported structures, staging cleanup |
| Shortcuts | Native file/folder `.lnk` targets with Unicode and no overwrite |
| Keyboard | Exact modifier routing, native editing exclusions, Alt+F4, AltGr and Ctrl+Alt+Delete preservation, view shortcuts, and pane/new-folder precedence |

## Coverage limits

All tests are headless. No visible app inspection or screenshot comparison was performed. Native context-menu presentation, drag-and-drop gestures, associated applications, the system sharing UI, preview handlers, clipboard payload round trips, real Recycle Bin restore, mapped/live network shares, cloud placeholders, removable devices, elevation, screen readers, high contrast, dark mode, localization, and mixed-DPI displays require additional dedicated fixtures or inspection. The matrix's acceptance criteria are not claims of completed testing.

The tests do not replace the user's clipboard or change shared Shell settings. Silent file-operation fixtures omit Shell undo records. Test files are created under owned temporary directories, and ZIP publication/extraction never overwrites an existing destination.

Local reports are generated in ignored `artifacts/`: `build.log`, `headless-tests.log`, `core-tests.xml`, and `headless-smoke.json`. The GitHub workflow publishes reports and the executable as build artifacts. Timings are environment-specific and do not establish that the application is faster than Explorer.exe.
