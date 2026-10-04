# Headless verification

Verified on 2026-10-04 on Windows 10 Home 22H2 (build 19045), using Visual Studio 2022 Build Tools (MSVC 19.44), Windows SDK 10.0.26100.0, and a Release x64 build with a static C++ runtime.

```powershell
./scripts/build.ps1 -Configuration Release
./scripts/test.ps1 -Configuration Release -SkipBuild
```

The local Release build completed without compiler warnings. CTest passed **2 of 2 targets**. The separate hidden-host report passed **54 of 54 checks**; keyboard/model tests passed **3,151 assertions across eight groups**. Item actions passed **six groups**, and native search passed **five groups**. The configured hosted CI run is pending; these results are local evidence.

## Tested behavior

| Area | Evidence |
| --- | --- |
| Host and visibility | Real Win32/Shell initialization; application stays hidden; process-owned visible-window observation during message pumping |
| Folder fixture | 1,000 text files, nested/Unicode folders, a hidden file, and a protected hidden/system file; exact view counts and cleanup |
| Presentation | Read back all eight native modes and icon sizes; sorting direction/key, grouping/removal, checkbox flags; toggle/restore a native column, retain Name, and autosize columns; collapsed ribbon reclaims space |
| Window geometry | F11 changes hidden window style/geometry and restores both while staying hidden; View commands fit the 1,030-pixel minimum width at the tested 96 DPI |
| Navigation | Breadcrumb/history construction, subfolder, back, forward, up, invalid location, This PC namespace; failed navigation preserves committed search state |
| Hidden state | Show/hide normal hidden items while protected hidden/system items remain excluded; reset after search |
| Panes | Preview/Details policy and mutual exclusion; native preview content rendering is not asserted |
| Search host | Native navigation/background flag; contextual commands; current-folder/all-subfolders retain origin; category replacement; Close returns to origin; saved search reopens in hidden host |
| Search results and persistence | Actual `IShellFolder` fixture enumeration: shallow/deep membership and exclusion of outside files; saved queries rerun after new files; live/saved parity for 19 canonical kind/date/size tokens and numeric/string/wildcard/Boolean comparisons; Today excludes an old file; native PIDL identity, XML escaping/Unicode, no overwrite, and rejected inputs/scopes |
| Search compatibility guard | Parser-only tests reject explicit filename `$<` inside nested conditions before native execution; literals, default terms and other properties remain allowed; no crash reproduction is part of the passing suite |
| Saved-search classification | Native `PKEY_ItemType` is `.search-ms` for saved searches and differs for an ordinary directory with that suffix |
| Result identity and aliases | Exact volume/128-bit file identities plus strict cardinality and duplicate rejection compare native results with fixtures; deliberate 8.3 temp aliases reproduce Shell expansion to long paths without weakening outside-scope checks |
| Settings and helpers | Unicode round trips, atomic replacement, invalid/corrupt/bounded values, reserved Windows names, environment expansion, search-URI encoding, byte formatting, HRESULT fallback |
| Shell operations | Silent copy/collision, move, Unicode rename/new-folder, reserved-name rejection, permanent deletion restricted to fixtures; existing/source files preserved |
| ZIPs | Compression and Unicode round trips, nested/empty folders, read-only archive, missing/invalid inputs, collisions/no overwrite, self-inclusion rejection, unsafe/link/unsupported structures, staging cleanup |
| Shortcuts | Native file/folder `.lnk` targets with Unicode and no overwrite |
| Item actions | Quoted Unicode paths and canonical virtual-item fallback; reversible selected-item Hidden attributes; no folder recursion; preflight rejection; preservation of compressed/sparse attributes; real junction rejection without target mutation |
| Keyboard/focus model | Exact modifier routing, native editing exclusions, Alt+F4, AltGr and Ctrl+Alt+Delete preservation, view shortcuts, pane/new-folder precedence, F6/Shift+F6/F11 routing, order/wraparound and every focus-region availability subset |

## Coverage limits

All tests are headless. No visible app inspection or screenshot comparison was performed. Focus-model checks do not establish actual focus actuation or accessibility. Hidden F11 checks do not establish maximized-window placement restoration, and the 96-DPI geometry check does not validate mixed-DPI layouts or visual clipping. Native context-menu presentation, drag-and-drop gestures, associated applications, the system sharing UI, preview handlers, clipboard payload round trips, real Recycle Bin restore, mapped/live network shares, cloud placeholders, removable devices, elevation, screen readers, high contrast, dark mode, and localization require additional dedicated fixtures or inspection. Local search-result fixtures do not establish content-index completeness or remote/provider coverage. The matrix's acceptance criteria are not claims of completed testing.

Reopened saved searches execute natively but do not restore query/scope metadata or the Search contextual page in the host. Explicit filename word-prefix (`$<`) searches are rejected live before native execution, and saved-search persistence rejects general explicit word-prefix conditions and unsupported types/scopes. Starts-with (`~<`) and wildcard (`~`) result fixtures pass.

The tests do not replace the user's clipboard or change shared Shell settings. Silent file-operation fixtures omit Shell undo records. Test files are created under owned temporary directories, and ZIP publication/extraction never overwrites an existing destination.

The native search fixture suite can also exercise deliberately shortened fixture paths by setting `WINDOWSEXPLORER_SEARCH_TEST_SHORT_PATHS=1` for its process. This requires available 8.3 aliases and asserts that the Shell expands their spelling. Normal and deliberate-alias local runs both passed all five search groups; XML scope checks use the native canonical scope and verify its folder identity.

Local reports are generated in ignored `artifacts/`: `build.log`, `headless-tests.log`, `core-tests.xml`, and `headless-smoke.json`. The GitHub workflow is configured to upload reports and the executable as build artifacts; no hosted pass is claimed yet. Timings are environment-specific and do not establish that the application is faster than Explorer.exe.
