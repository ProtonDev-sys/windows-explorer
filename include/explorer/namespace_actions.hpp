#pragma once

#include "explorer/context_menu.hpp"

#include <memory>
#include <span>
#include <string_view>

namespace explorer {

enum class NamespaceAction {
    FormatDrive, OptimizeDrive, CleanUpDrive, EjectDrive, BitLocker,
    MountDiscImage, BurnDiscImage,
    RotateLeft, RotateRight, SlideShow, SetWallpaper,
    RestoreSelected, RestoreAll, EmptyRecycleBin,
    RunAsAdministrator, TroubleshootCompatibility,
    IncludeInLibrary, AlwaysAvailableOffline, WorkOffline, SyncOffline,
    MapAsDrive, ShareSpecificPeople, RemoveAccess, AdvancedSecurity,
    FileHistory, Email, Fax, BurnToDisc,
    Play, PlayAll, AddToPlaylist, CastToDevice,
    Count
};

enum class NamespaceMenuScope { Selection, Background };
enum class NamespaceInvocationRoute {
    None, SelectionMenu, CommandStoreMenu, RestoreAllItems, EmptyRecycleBin, OfflineFilesPin, SendToMailRecipient, RegisteredHandlerMenu
};

struct NamespaceTarget {
    Microsoft::WRL::ComPtr<IShellItem> folder;
    Microsoft::WRL::ComPtr<IShellItemArray> selection;
    Microsoft::WRL::ComPtr<IUnknown> site;
    // Optional WM_APP-range result notification for asynchronous Offline Files
    // operations. wParam is NamespaceAction; lParam preserves the native HRESULT.
    UINT completionMessage = 0;
};

// An immutable description of the actual Shell/filesystem targets. Public for
// deterministic planner tests; production instances derive it from Shell items.
struct NamespaceFacts {
    DWORD selectionCount = 0;
    // Native AND result across the complete original array. S_FALSE is a
    // successful partial mask; no first-item or capped-selection inference.
    SFGAOF nativeAttributes = 0;
    HRESULT nativeAttributesStatus = E_PENDING;
    // Filesystem paths, residency and perceived types are optional detailed
    // facts. Above the inspection budget they remain unknown, while the full
    // array and native provider state remain available. Never interpret false
    // optional fields below as a verified negative when this flag is false.
    bool detailedTargetsKnown = true;
    bool filesystem = false;
    bool physicalFiles = false;
    bool physicalFolders = false;
    bool images = false;
    bool applications = false;
    bool discImages = false;
    bool media = false;
    bool castItems = false;
    bool uncPaths = false;
    bool driveRoot = false;
    UINT driveType = DRIVE_UNKNOWN;
    bool recycleBin = false;
    ULONGLONG recycleItems = 0;
    HRESULT recycleStatus = E_PENDING;
    bool offlineActive = false;
    bool offlinePinnedForUser = false;
    HRESULT offlineStatus = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    bool mailRecipientAvailable = false;
    std::wstring mailRecipientPath;
    bool castHandlerAvailable = false;
    UINT castCommandId = 0;
    bool castHandlerEnabled = false;
    bool castHandlerSubmenu = false;
    std::wstring singlePath;
};

struct NamespaceInvocationPlan {
    NamespaceAction action = NamespaceAction::Count;
    NamespaceInvocationRoute route = NamespaceInvocationRoute::None;
    NamespaceMenuScope scope = NamespaceMenuScope::Selection;
    UINT commandId = 0;
    std::wstring canonicalVerb;
    std::wstring target;
    bool submenu = false;
    bool enabled = false;
    bool checked = false;
    // Describes native UI/confirmation, never permission to bypass it.
    bool nativeConfirmation = false;
    HRESULT status = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
};

struct NamespaceCommandMetadata {
    std::wstring command;
    std::wstring label;
    std::wstring description;
    // Actual provider/registered Icon specification, e.g. imageres.dll,-5344. The caller
    // resolves the resource at its current DPI rather than replacing the glyph.
    std::wstring icon;
};

struct NamespaceSubcommandMetadata {
    GUID canonicalName = GUID_NULL;
    std::wstring label;
    std::wstring description;
    std::wstring icon;
    EXPCMDFLAGS flags = ECF_DEFAULT;
    EXPCMDSTATE state = ECS_DISABLED;
    HRESULT stateStatus = E_PENDING;
    std::vector<NamespaceSubcommandMetadata> children;
};

struct NamespaceCommandState {
    CLSID handler = CLSID_NULL;
    EXPCMDSTATE state = ECS_DISABLED;
    bool explorerCommand = false;
    bool initialized = false;
    bool siteAttached = false;
    // Read-only full native selection-menu evidence for static association
    // verbs that have no registered public command-state handler.
    bool contextMenu = false;
    bool identitySnapshot = false;
    DWORD selectionCount = 0;
    // A registered composite without a public initializer can delegate its
    // read-only state to its installed leaf. Invocation still uses the native
    // parent menu. Empty for a directly queried provider.
    std::wstring delegatedCommand;
    bool enabled() const noexcept { return (state & (ECS_DISABLED | ECS_HIDDEN)) == 0; }
    bool checked() const noexcept { return (state & ECS_CHECKED) != 0; }
};

struct NamespaceSelectionVerbState {
    std::wstring verb;
    HRESULT status = E_PENDING;
    NamespaceCommandState native;
};

struct NamespaceCommandStateTimings {
    ULONGLONG dataObjectExportMicroseconds = 0;
    ULONGLONG identityConstructionMicroseconds = 0;
    ULONGLONG contextBindMicroseconds = 0;
    ULONGLONG menuQueryMicroseconds = 0;
    ULONGLONG menuEnumerationMicroseconds = 0;
    ULONGLONG stateReductionMicroseconds = 0;
    ULONGLONG workerMicroseconds = 0;
};

struct NamespaceSelectionKinds {
    DWORD count = 0;
    bool music = false;
    bool video = false;
};

// PKEY_Kind is multi-valued. The public array property store returns its
// intersection across every selected item. Fast-only properties prevent disk,
// network and content-handler reads; no per-item loop or selection-count cap.
// Empty/null selection succeeds with no media kind. Failure preserves output.
HRESULT namespaceSelectionKinds(IShellItemArray* selection,NamespaceSelectionKinds* result);

// Direct registered IExplorerCommand/IExplorerCommandState GetState(FALSE).
// Initializes the exact command with a read-only registry property bag when it
// implements IInitializeCommand. Never builds a context menu or invokes a verb.
// Static/SendTo verbs without a state handler return ERROR_NOT_SUPPORTED;
// E_PENDING and provider failures are retained, never assumed enabled.
// SearchOpenLocation's non-initializable native composite uses the exact
// registered, validated VerbList leaves for read-only capability state. Any
// pending/failed leaf preserves failure output; native invocation remains the
// actual parent menu's ordinal.
HRESULT namespaceCommandState(std::wstring_view command, IShellItemArray* selection,
                              IUnknown* site, NamespaceCommandState* result);

// Completes a native E_PENDING state query with GetState(TRUE) on a separate
// STA. Actual selection/site interfaces cross apartments through the standard
// GIT; no provider interface is called from a foreign apartment. Poll/cancel and
// destruction belong to the creating STA; none waits for provider completion.
// Worker owns a handle to the exact creator desktop and attaches before COM.
// Hosts drain their STA workers while sites/apartment remain alive at shutdown;
// navigation cancellation and task destruction remain nonblocking.
// The host must discard results after navigation/selection/clipboard changes.
// Background Explorer commands receive null selection; state-only handlers
// receive the actual current-folder item array required by their public API.
class NamespaceCommandStateTask final {
public:
    ~NamespaceCommandStateTask();
    static HRESULT start(std::wstring_view command,IShellItemArray* selection,IUnknown* site,
                         bool background,std::unique_ptr<NamespaceCommandStateTask>* result);
    // Reads the complete original selection's native IContextMenu on the
    // worker STA. Exact unique canonical leaf only; never invokes anything.
    static HRESULT startSelectionVerb(std::wstring_view verb,IShellItemArray* selection,IUnknown* site,
                                      std::unique_ptr<NamespaceCommandStateTask>* result);
    static HRESULT startSelectionVerbs(std::span<const std::wstring_view> verbs,IShellItemArray* selection,IUnknown* site,
                                       std::unique_ptr<NamespaceCommandStateTask>* result);
    // One complete native menu/snapshot for independent canonical verbs.
    // Each entry keeps its own missing/ambiguous/disabled/error result. Global
    // preparation failure/cancellation preserves the caller's whole output.
    static HRESULT startSelectionVerbBatch(std::span<const std::wstring_view> verbs,IShellItemArray* selection,IUnknown* site,
                                           std::unique_ptr<NamespaceCommandStateTask>* result);
    // Ribbon-only static commands can be absent from the default item menu.
    // Reads their exact registered CommandStore leaf with the full selection.
    static HRESULT startRegisteredMenu(std::wstring_view command,IShellItemArray* selection,IUnknown* site,
                                       std::unique_ptr<NamespaceCommandStateTask>* result);
    HRESULT poll(NamespaceCommandState* result);
    HRESULT pollSelectionVerbBatch(std::vector<NamespaceSelectionVerbState>* result);
    // Read-only performance evidence after actual completion, including a
    // missing/failed native leaf. Does not reinterpret that capability result.
    HRESULT pollTimings(NamespaceCommandStateTimings* result);
    // Nonblocking creator-STA readback used to drain isolated test/provider
    // resources before their private desktop/apartment is destroyed. Cancel
    // still rejects a stale result immediately and never waits for a provider.
    bool completed() const;
    void cancel() noexcept;
private:
    struct Impl;
    static HRESULT startImpl(std::wstring_view name,IShellItemArray* selection,IUnknown* site,
                             bool background,std::vector<std::wstring> selectionVerbs,
                             std::unique_ptr<NamespaceCommandStateTask>* result,bool independentVerbs = false);
    explicit NamespaceCommandStateTask(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
bool namespaceActionApplicable(NamespaceAction action,const NamespaceFacts& facts) noexcept;

struct NamespaceCommandPopup {
    // Borrowed, never destroy/reparent. Invalidated by initialize/reset/refresh.
    HMENU menu = nullptr;
    unsigned long long generation = 0;
    NamespaceInvocationPlan plan;
    std::vector<ContextMenuEntry> entries;
};

// Native gallery snapshot: retains the actual enumerated child commands and
// original selection/site on their creating STA. It never reconstructs a
// changing MRU destination from an index, label or translated path. The host
// discards the snapshot when its navigation/selection generation changes.
class NativeNamespaceCommandChildren final {
public:
    ~NativeNamespaceCommandChildren();
    const std::vector<NamespaceSubcommandMetadata>& entries() const noexcept;
    HRESULT invoke(size_t index,bool headless);
    // Indexes follow the immutable metadata hierarchy, including separators.
    // Every ancestor is revalidated against its retained native object/site;
    // only a non-cascade leaf can run. No labels or MRU paths are re-resolved.
    HRESULT invokePath(std::span<const size_t> path,bool headless);
private:
    friend class NativeNamespaceActions;
    struct Impl;
    explicit NativeNamespaceCommandChildren(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

std::wstring_view namespaceActionCommand(NamespaceAction action) noexcept;
std::wstring_view namespaceActionLabel(NamespaceAction action) noexcept;
// Reads installed Windows command metadata; an initialized IExplorerCommand's
// native GetTitle/GetIcon/GetToolTip takes precedence over registry fallbacks.
// Never invokes a command or creates registry entries. Optional real view/site.
HRESULT namespaceCommandMetadata(std::wstring_view command,
                                 NamespaceCommandMetadata* result,
                                 IShellItemArray* selection = nullptr,IUnknown* site = nullptr);
// Reads IExplorerCommand::EnumSubCommands/GetTitle/GetIcon/GetState(FALSE) only.
// Windows.IconSize exposes the OS's eight localized View gallery items here.
// Enumeration order is native; GUID_NULL is retained when a provider has no ID.
HRESULT namespaceCommandChildren(std::wstring_view command, IShellItemArray* selection,
                                 IUnknown* site,
                                 std::vector<NamespaceSubcommandMetadata>* result);

// Pure planner: verifies semantic applicability before accepting an exact native
// verb. Disabled ancestors, duplicate IDs, missing handlers and translated menu
// labels are never used to guess an invocation. Output is unchanged on failure.
HRESULT planNamespaceAction(NamespaceAction action, const NamespaceFacts& facts,
                            const std::vector<ContextMenuEntry>& selectionEntries,
                            const std::vector<ContextMenuEntry>& commandStoreEntries,
                            NamespaceInvocationPlan* result);

// Retains the selection, native context menus and site on their creating STA.
// No method displays UI except invoke()/invokeCommandStore(). These methods
// explicitly reject headless callers and hidden/invalid owners before invoking
// anything. Enumerating menus does not execute registered verbs.
class NativeNamespaceActions final {
public:
    NativeNamespaceActions();
    ~NativeNamespaceActions();
    NativeNamespaceActions(const NativeNamespaceActions&) = delete;
    NativeNamespaceActions& operator=(const NativeNamespaceActions&) = delete;

    HRESULT initialize(HWND owner, const NamespaceTarget& target);
    // Keep ongoing service work across navigation resets; pass true on shutdown
    // to cancel callbacks/operations before the owner's HWND can be destroyed.
    void reset(bool cancelPending = false) noexcept;
    // Re-queries installed native states after operations/history changes.
    // Returns ERROR_BUSY while a borrowed native submenu is being tracked.
    HRESULT refresh();
    const NamespaceFacts& facts() const noexcept;
    HRESULT planInvocation(NamespaceAction action, NamespaceInvocationPlan* result);
    std::vector<NamespaceInvocationPlan> capabilities();
    HRESULT invoke(NamespaceAction action, bool headless, POINT point = {});

    // Broad native fallback for existing Windows 10 commands. Generic callers
    // must retain their own command-specific applicability checks: a static verb
    // can be present in CommandStore without applying to the current file type.
    HRESULT planCommandStore(std::wstring_view command,
                             NamespaceInvocationPlan* result,
                             NamespaceMenuScope scope = NamespaceMenuScope::Selection);
    HRESULT commandStoreEntries(std::vector<ContextMenuEntry>& result,
                                NamespaceMenuScope scope = NamespaceMenuScope::Selection);
    HRESULT commandMetadata(std::wstring_view command, NamespaceCommandMetadata* result,
                            NamespaceMenuScope scope = NamespaceMenuScope::Selection);
    HRESULT invokeCommandStore(std::wstring_view command, bool headless, POINT point = {},
                               NamespaceMenuScope scope = NamespaceMenuScope::Selection,
                               std::wstring_view requiredAssociationVerb = {});
    // Only SelectAll/SelectNone/InvertSelection, on the identical native view
    // attached as this instance's site. Owner/view HWNDs must belong to this
    // process/STA and the view must be its actual child. Headless use requires
    // the initialized, currently isolated PrivateDesktop guard. No generic
    // command, files, clipboard, destinations or external UI is authorized.
    HRESULT invokeViewSelection(std::wstring_view command,IShellView* exactView,bool headless);
    // Fast capability query on this instance's actual selection/view site. Does
    // not load menus; Background supplies null selection to the view provider.
    HRESULT queryCommandState(std::wstring_view command, NamespaceCommandState* result,
                              NamespaceMenuScope scope = NamespaceMenuScope::Selection);
    HRESULT startCommandStateTask(std::wstring_view command,std::unique_ptr<NamespaceCommandStateTask>* result,
                                  NamespaceMenuScope scope = NamespaceMenuScope::Selection);
    HRESULT startStaticVerbStateTask(std::wstring_view verb,std::unique_ptr<NamespaceCommandStateTask>* result);
    HRESULT startStaticVerbStateBatch(std::span<const std::wstring_view> verbs,std::unique_ptr<NamespaceCommandStateTask>* result);
    HRESULT startActionStateTask(NamespaceAction action,std::unique_ptr<NamespaceCommandStateTask>* result);
    HRESULT startRegisteredMenuStateTask(std::wstring_view command,std::unique_ptr<NamespaceCommandStateTask>* result);
    HRESULT queryCommandChildren(std::wstring_view command,std::unique_ptr<NativeNamespaceCommandChildren>* result,
                                 NamespaceMenuScope scope = NamespaceMenuScope::Selection);
    // Fast semantic/provider state for a contextual action. A static single
    // association is only a negative prefilter; positive/multiple states pend
    // until the complete native selection-menu worker finishes. Registered SendTo
    // targets and public Offline Files/Recycling APIs use their real capability.
    // Optional alias metadata is populated only when the exact default native
    // selection-menu fallback returns E_PENDING; registered state is separate.
    HRESULT queryActionState(NamespaceAction action, NamespaceCommandState* result,
                             std::vector<std::wstring_view>* selectionVerbs = nullptr);
    HRESULT queryStaticVerbState(std::wstring_view verb, NamespaceCommandState* result);
    HRESULT queryZipState(NamespaceCommandState* result);
    // Isolated installed compressed-folder handler, actual canonical extract
    // verb/state; does not construct the full selection/CommandStore menus.
    HRESULT queryExtractState(NamespaceCommandState* result);
    HRESULT invokeExtract(bool headless, POINT point = {});
    // Narrow public capabilities for registered static Control Panel/wizard
    // commands that have no IExplorerCommandState. Does not open any UI.
    HRESULT queryRegisteredComponentState(std::wstring_view command,NamespaceCommandState* result);
    // Native Windows.zip command first; registered .ZFSendToTarget Drop fallback
    // receives the original native IDataObject, including folders/multiselect.
    // Explicit normal interaction only; never a tar/staging/Save-dialog fallback.
    HRESULT invokeZip(bool headless, POINT point = {});
    // Full native property/provider menus only when explicitly requested. The
    // copied labels/IDs retain native order, hidden/disabled state and hierarchy.
    HRESULT queryCommandStorePopup(std::wstring_view command, NamespaceCommandPopup* result,
                                   NamespaceMenuScope scope = NamespaceMenuScope::Background);
    // Tracks the exact borrowed native submenu and invokes only its selected
    // ordinal. The owner's WndProc must call handleMenuMessage before dispatch
    // for WM_INITMENUPOPUP/MENUCHAR/DRAWITEM/MEASUREITEM. Headless is rejected.
    HRESULT invokeCommandStorePopup(const NamespaceCommandPopup& popup, bool headless, POINT point = {});
    bool handleMenuMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace explorer
