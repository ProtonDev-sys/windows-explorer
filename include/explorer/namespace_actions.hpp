#pragma once

#include "explorer/context_menu.hpp"

#include <memory>
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
    // Registered Windows Icon specification, e.g. imageres.dll,-5344. The caller
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
    bool enabled() const noexcept { return (state & (ECS_DISABLED | ECS_HIDDEN)) == 0; }
    bool checked() const noexcept { return (state & ECS_CHECKED) != 0; }
};

// Direct registered IExplorerCommand/IExplorerCommandState GetState(FALSE).
// Initializes the exact command with a read-only registry property bag when it
// implements IInitializeCommand. Never builds a context menu or invokes a verb.
// Static/SendTo verbs without a state handler return ERROR_NOT_SUPPORTED;
// E_PENDING and provider failures are retained, never assumed enabled.
HRESULT namespaceCommandState(std::wstring_view command, IShellItemArray* selection,
                              IUnknown* site, NamespaceCommandState* result);
bool namespaceActionApplicable(NamespaceAction action,const NamespaceFacts& facts) noexcept;

struct NamespaceCommandPopup {
    // Borrowed, never destroy/reparent. Invalidated by initialize/reset/refresh.
    HMENU menu = nullptr;
    unsigned long long generation = 0;
    NamespaceInvocationPlan plan;
    std::vector<ContextMenuEntry> entries;
};

std::wstring_view namespaceActionCommand(NamespaceAction action) noexcept;
std::wstring_view namespaceActionLabel(NamespaceAction action) noexcept;
// Reads installed Windows command metadata only; creates no registry entries.
HRESULT namespaceCommandMetadata(std::wstring_view command,
                                 NamespaceCommandMetadata* result);
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
                               NamespaceMenuScope scope = NamespaceMenuScope::Selection);
    // Fast capability query on this instance's actual selection/view site. Does
    // not load menus; Background supplies null selection to the view provider.
    HRESULT queryCommandState(std::wstring_view command, NamespaceCommandState* result,
                              NamespaceMenuScope scope = NamespaceMenuScope::Selection);
    // Fast semantic/provider state for a contextual action. Static verbs are
    // checked against each actual item's association array; registered SendTo
    // targets and public Offline Files/Recycling APIs use their real capability.
    HRESULT queryActionState(NamespaceAction action, NamespaceCommandState* result);
    HRESULT queryStaticVerbState(std::wstring_view verb, NamespaceCommandState* result);
    HRESULT queryZipState(NamespaceCommandState* result);
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
