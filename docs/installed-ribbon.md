# Installed Windows 10 Ribbon

The Windows 10 22H2 adapter uses the installed `ExplorerFrame.dll` resource through the public Windows Ribbon Framework. Windows owns the tab, group, gallery and split-button layout, title-bar Quick Access Toolbar, keytips, tooltips, accessibility providers and native customization menu. The application supplies command state, localized command metadata, system images and native Shell actions.

The adapter accepts Windows build 19045 only, then checks the installed resource's size, binary header and expected command symbols. An incompatible or unavailable resource selects the authored Ribbon fallback. Reports distinguish `InstalledWindows10` from `Authored` and retain the attempted installed-layout HRESULT; a fallback is never reported as an installed-layout success. No Windows DLL, compiled Windows UI resource or extracted artwork is shipped in the repository.

The resource is loaded as data and an image resource. Its command identifiers are translated by [ribbon_stock.inc](../src/ribbon_stock.inc). The application continues to use its own command catalogue and public framework facade, so a stock identifier never reaches a generic executable-command dispatcher without an explicit binding.

## State and collections

The application binds commands to the current Shell folder, original selected-item array and actual Shell view site. Registered providers supply their enabled, hidden and checked state. State providers returning `E_PENDING` are continued on a separate STA using the same marshalled interfaces; selection, navigation and clipboard changes invalidate their generation. Implemented host commands use an explicit host contract when no public native state provider exists.

Commands that require the default selection menu share one worker snapshot for the complete original array. Each canonical verb and alias group retains its own missing, ambiguous, disabled, checked or failed result. Duplicate Open controls reuse that same result. Registered-only, background and current-archive commands keep their distinct providers. The catalogue marks this fallback explicitly; the host does not guess a menu route from a command name.

Native COM providers can pump owner-thread messages during a query. The host keeps refresh, collection and state-task iterations in one owner scope; nested refreshes and cancellations are deferred until that scope exits. It consumes dirty requests before entering native calls, publishes a completed capability map together and preserves notifications received during the query for the next coalesced update. This protects retained provider and map references without changing native enabled or checked results.

Dropdown and extraction destinations retain the original `IExplorerCommand` objects and hierarchical index paths. Invocation rechecks the current provider state and uses those retained commands. It does not reconstruct a personal recent-place command from a translated label or a later MRU index.

Runtime command-gallery identifiers must fit the native framework's 16-bit range on this installed layout. The adapter allocates its dynamic commands in the disjoint `0x9000..0xfffe` range and recycles only a matching immutable native command type after its old collection is cleared. It invalidates all recycled properties, including an explicitly empty image, to prevent stale labels, state or artwork. The native test exercises 54 destinations over 12 changing generations and confirms both command registration and actual visible enabled/disabled rows.

`ItemsSource` is refreshed separately from state and value properties. Sources requested by the native framework are refreshed when their target generation changes; unopened menus retain native lazy loading. Expanding Open's actual dropdown arrow requests its Open-with collection through `UpdateProperty`, which populates the framework-provided `IUICollection` through documented `Clear`/`Add` methods. Invalidations requested during a property callback are queued until that callback returns, as required by the Ribbon Framework. Native regressions verify actual popup rows, source-demand diagnostics and a state change requested during a collection callback.

Replacing the Layout item collection clears its native `SelectedItem`. The adapter queues a separate selected-property refresh after the collection callback returns, using the actual folder-view mode. The public [gallery property contract](https://learn.microsoft.com/en-us/windows/win32/windowsribbon/ribbon-controls-galleries) distinguishes this index-based item selection from command-gallery checked state.

Ribbon popup focus retains the original selection and command snapshot. The host distinguishes the documented focus notifications from selection, rename and item-state notifications in [ICommDlgBrowser::OnStateChange](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-icommdlgbrowser-onstatechange); opening a popup therefore does not discard its live native children.

The File menu's native frequent-place callback can contain ten padded slots for a source with only two rows. The adapter records the supplied row count, ignores padding and updates only changed pins. Authored and installed regressions invoke the pin rail through its public MSAA push-button interface on an owned mock model; no Shell pin, mouse-input or desktop operation is performed.

## Installed feature variants

The feature detector runs once and records each public API's HRESULT. It checks the Windows product edition, balanced public Media Foundation startup/shutdown, the CD-burning restriction and the installed cleanup executable. Unknown failures retain the control family; only verified absence removes it. Drive cleanup also uses the actual native drive type.

These facts select the installed standard or No-BitLocker, No-Cleanup, No-Media-Foundation and No-Burn context variants. The native context and tab identifiers are selected together. The native tests confirm the actual context property and real UIA omission of BitLocker on Home and cleanup on a non-fixed drive. A separate 924-case read-only investigation confirmed that contextual group geometry is selected by context identity rather than ordinary Home mode modifiers.

Reports preserve these facts and actual context identifiers. A Home-edition screenshot is not compared as though it had Pro's BitLocker group. Locale, OS preferences, selection eligibility and personal MRU data are also real state, not screenshot-matching overrides.

## Headless verification

Rendering occurs only on the guarded private desktop; the input desktop is never switched. UI Automation runs on a separate windowless MTA while the owner STA pumps messages. A native split control can expose same-name primary and dropdown providers; tests identify the narrow right-hand arrow before expansion and require zero unrelated actions. The native tests verify actual Open-with rows, Copy-to's real `ExpandCollapse` states `0 -> 1 -> 0`, registered collection commands, all eight view layouts, context variants, Quick Access persistence and the absence of visible owned windows on the input desktop.

The installed Home band passes the existing pinned Microsoft screenshot thresholds without changing those thresholds. Other scene results remain in the generated comparison report, including native state differences and failures. Pixel agreement alone does not establish command behavior; command, provider, popup and exact file-identity checks are independent.

Microsoft documents the interfaces used here in [IUIFramework::LoadUI](https://learn.microsoft.com/en-us/windows/win32/api/uiribbon/nf-uiribbon-iuiframework-loadui), [IUICommandHandler::UpdateProperty](https://learn.microsoft.com/en-us/windows/win32/api/uiribbon/nf-uiribbon-iuicommandhandler-updateproperty), [dynamic galleries](https://learn.microsoft.com/en-us/windows/win32/windowsribbon/ribbon-controls-galleries), and [UI Automation threading](https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-threading). The build-specific identifier range and padded recent-item behavior above are observations verified against the installed native implementation, not a promise about future Windows resources.
