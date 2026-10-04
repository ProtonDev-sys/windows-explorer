# Headless verification

Verified on 2026-10-04 on Windows 10 Home 22H2 (build 19045), using Visual Studio 2022 Build Tools (MSVC 19.44), Windows SDK 10.0.26100.0, and a Release x64 build with a static C++ runtime.

```powershell
./scripts/build.ps1 -Configuration Release
./scripts/test.ps1 -Configuration Release -SkipBuild
```

The final local Release build completed without compiler warnings. CTest passed **2/2** tests: console **8.21 seconds**, hidden host **5.69 seconds**, **13.93 seconds** total. An independent hidden-host run passed **72/72 checks**. The integrated console suite passed keyboard/model **3,151 assertions/eight groups**, item actions **six groups**, native search **five groups**, saved metadata **five groups/35 query fixtures**, Share **five groups**, context menus **six groups/91 assertions**, toolbar **four groups**, and Library **three groups**.

[Hosted CI](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/37169510842) passed for commit `ce231210f02757df636145bcd32e7e0a3df7d0ee` on `windows-2022` (Windows Server 2022): CTest **2/2**, console **3.98 seconds**, hidden host **4.67 seconds**, **8.69 seconds** total. Its separate smoke artifact reports `headless=true`, `passed=true`, **72 checks**, **0 failed**, and **3,562 ms** elapsed. Hosted Server results complement the local Windows 10 run; they do not establish identical provider availability or UI behavior across those systems.

## Tested behavior

| Area | Evidence |
| --- | --- |
| Host and visibility | Real Win32/Shell initialization; application stays hidden; process-owned visible-window observation during message pumping |
| Folder fixture | 1,000 text files, nested/Unicode folders, a hidden file, and a protected hidden/system file; exact view counts and cleanup |
| Presentation | Read back all eight native modes and icon sizes; sorting direction/key, grouping/removal, checkbox flags; toggle/restore a native column, retain Name, and autosize columns; collapsed ribbon reclaims space |
| Quick Access Toolbar | Four model/persistence groups: supported catalog/bounds, add/remove/reorder/reset, Unicode/atomic settings, corrupt/unknown/duplicate input; five hidden-control checks verify default buttons, order, above/below placement, reset and permanent-delete selection guard; exact title-bar layout remains unverified |
| Window geometry | F11 changes hidden window style/geometry and restores both while staying hidden; View commands fit the 1,030-pixel minimum width at the tested 96 DPI |
| Navigation | Breadcrumb/history construction, subfolder, back, forward, up, invalid location, This PC namespace; failed navigation preserves committed search state |
| Hidden state | Show/hide normal hidden items while protected hidden/system items remain excluded; reset after search |
| Panes | Preview/Details policy and mutual exclusion; native preview content rendering is not asserted |
| Search host | Native navigation/background flag; contextual commands; current-folder/all-subfolders retain origin; category replacement; Close returns to origin; saved search reopens, restores host query/scope, and refines its original scope |
| Search results and persistence | Actual `IShellFolder` fixture enumeration: shallow/deep membership and exclusion of outside files; saved queries rerun after new files; live/saved parity for 19 canonical kind/date/size tokens and numeric/string/wildcard/Boolean comparisons; Today excludes an old file; native PIDL identity, XML escaping/Unicode, no overwrite, and rejected inputs/scopes |
| Saved metadata | Five groups/35 actual live/saved/restored query fixtures, with known exact identities/cardinality for the first 11; Unicode/ampersand filenames, numeric/string/wildcard/Boolean operations, all 19 filters, scope/recursion, preserved raw Today token, This PC identity without enumeration; unsupported shapes/types/ProgID text and malformed/oversize/deep/node-bound/DTD inputs reject with output preserved |
| Generic persistence | Four unspecified-property query shapes return `ERROR_NOT_SUPPORTED` before creating a file; live default/literal queries remain allowed; existing collision/scope/XML tests use supported explicit properties |
| Open file location | Hidden search-result fixture resolves the real filesystem parent and selects exactly the owned child; one-result guard checked; virtual results are unsupported |
| Search compatibility guard | Parser-only tests reject explicit filename `$<` inside nested conditions before native execution; literals, default terms and other properties remain allowed; no crash reproduction is part of the passing suite |
| Saved-search classification | Native `PKEY_ItemType` is `.search-ms` for saved searches and differs for an ordinary directory with that suffix |
| Result identity and aliases | Exact volume/128-bit file identities plus strict cardinality and duplicate rejection compare native results with fixtures; deliberate 8.3 temp aliases reproduce Shell expansion to long paths without weakening outside-scope checks |
| Settings and helpers | Unicode round trips, atomic replacement, invalid/corrupt/bounded values, reserved Windows names, environment expansion, search-URI encoding, byte formatting, HRESULT fallback |
| Shell operations | Silent copy/collision, move, Unicode rename/new-folder, reserved-name rejection, permanent deletion restricted to fixtures; existing/source files preserved |
| ZIPs | Compression and Unicode round trips, nested/empty folders, read-only archive, missing/invalid inputs, collisions/no overwrite, self-inclusion rejection, unsafe/link/unsupported structures, staging cleanup |
| Shortcuts | Native file/folder `.lnk` targets with Unicode and no overwrite |
| Registered New / context menus | Six groups/91 assertions: real background/item/New menu enumeration on hidden fixtures; physical ZIP destination rejection, old-menu cleanup and unchanged archive bytes; command IDs, CM2/CM3 message routing, invocation arguments, apartment ownership and lifetime tested with fake handlers; no native handler execution or visible popup |
| Physical folder commands | Hidden checks enable New folder/text/shortcut, registered New menus and terminal commands for actual filesystem directories and disable them in ZIP namespace folders; execution revalidates the directory; Paste remains a separate native Shell capability |
| Windows Share | Five groups: bounded Unicode filesystem-file planning, regular ZIP acceptance despite the Shell folder attribute, invalid/missing/physical-directory/virtual/reparse rejection, hidden HWND desktop interop, STA ownership/event cleanup, actual asynchronous StorageItems, missing-file HRESULTs and source-shutdown lifetime; hidden host verifies directory rejection/ZIP eligibility; picker and recipients never shown |
| Libraries | Three integrated native groups passed: Unicode Save/load/Commit, included/default identities, all five content templates, invalid/dot-path duplicate/no-overwrite behavior, read-only policies and output/HRESULT preservation; hidden host verifies Library context creation, included/default location readback, and context removal after navigation; files saved only under owned temp paths |
| Item actions | Quoted Unicode paths and canonical virtual-item fallback; reversible selected-item Hidden attributes; no folder recursion; preflight rejection; preservation of compressed/sparse attributes; real junction rejection without target mutation |
| Keyboard/focus model | Exact modifier routing, native editing exclusions, Alt+F4, AltGr and Ctrl+Alt+Delete preservation, view shortcuts, pane/new-folder precedence, F6/Shift+F6/F11 routing, order/wraparound and every focus-region availability subset |

## Coverage limits

All tests are headless. No visible app inspection or screenshot comparison was performed. Focus-model checks do not establish actual focus actuation or accessibility. Hidden F11 checks do not establish maximized-window placement restoration, and the 96-DPI geometry check does not validate mixed-DPI layouts or visual clipping. Toolbar checks do not establish caption/title-bar parity. Native menus are enumerated, while installed handler invocation and visible presentation are untested; real Share payloads do not establish picker display or recipient delivery. Drag gestures, associated apps, preview handlers, global clipboard round trips, Recycle Bin restore, live network/cloud/devices, elevation, screen readers, high contrast, dark mode, and localization require additional fixtures or inspection. Library aggregation/provider behavior and interactive pickers are unverified. Local search fixtures do not establish content-index completeness or remote/provider coverage. Matrix acceptance criteria are requirements, not completed tests.

Reopened supported saved searches restore a faithful query, scope, recursion, and Search contextual page after successful native navigation. The imported condition becomes the base query; previous filter-category state is not inferred. The entire external `.search-ms` format is not supported: multiple/excluded scopes, non-item kind unions, internal/provider shapes, Blurb/unknown types, and other virtual scopes remain native-view-only. Explicit filename word-prefix (`$<`) searches reject live before execution; persistence also rejects general explicit word-prefix conditions, unspecified-property terms, and unsupported values/scopes. Starts-with (`~<`) and wildcard (`~`) result fixtures pass.

The tests do not replace the user's clipboard, change shared settings or user libraries, show Share recipients, or execute installed New handlers. Silent operations omit Shell undo records. Headless browsers set documented `EBO_NOPERSISTVIEWSTATE`; its native host flag readback passed. Libraries save only to explicit owned temporary directories. ZIP publication/extraction never overwrites an existing destination. [Browser options](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/ne-shobjidl_core-explorer_browser_options), [Native Library Save](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ishelllibrary-save)

The native search fixture suite can also exercise deliberately shortened fixture paths by setting `WINDOWSEXPLORER_SEARCH_TEST_SHORT_PATHS=1` for its process. This requires available 8.3 aliases and asserts that the Shell expands their spelling. Normal and deliberate-alias local runs both passed all five search groups; XML scope checks use the native canonical scope and verify its folder identity.

Local reports are generated in ignored `artifacts/`: `build.log`, `headless-tests.log`, `core-tests.xml`, and `headless-smoke.json`. The passing hosted workflow linked above uploads reports and the executable. Timings are environment-specific and do not establish that the application is faster than Explorer.exe.
