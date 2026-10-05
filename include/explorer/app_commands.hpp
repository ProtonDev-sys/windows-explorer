#pragma once

#include "explorer/commands.hpp"
#include "explorer/ribbon_commands.hpp"
#include "explorer/namespace_actions.hpp"
#include <optional>
#include <span>

namespace explorer {

enum class AppCommandRoute { Host, NamespaceAction, CommandStore, NativeZip };
enum AppCommandRequirement : unsigned {
    RequireNone = 0, RequireFolderView = 1u << 0, RequireSelection = 1u << 1,
    RequireSingleSelection = 1u << 2, RequireCopy = 1u << 3, RequireMove = 1u << 4,
    RequireDelete = 1u << 5, RequireRename = 1u << 6, RequirePhysicalDirectory = 1u << 7,
    RequireFilesystemSelection = 1u << 8, RequireLibrary = 1u << 9,
    RequireWritableLibrary = 1u << 10, RequireSearch = 1u << 11,
    RequireSearchBackground = 1u << 12, RequireDetailsView = 1u << 13,
    RequireArchive = 1u << 14, RequireComputer = 1u << 15, RequireDriveRoot = 1u << 16,
    RequireNetwork = 1u << 17
};

struct AppCommandBinding {
    UINT command = 0;
    AppCommandRoute route = AppCommandRoute::Host;
    NamespaceAction action = NamespaceAction::Count;
    std::wstring_view commandStore;
    NamespaceMenuScope scope = NamespaceMenuScope::Selection;
    unsigned requirements = RequireNone;
};

struct AppCommandContext {
    bool folderView = false;
    bool navigating = false;
    bool physicalDirectory = false;
    bool library = false;
    bool writableLibrary = false;
    bool search = false;
    bool searchBackground = false;
    bool detailsView = false;
    bool archive = false;
    bool computer = false;
    bool network = false;
    bool driveRoot = false;
    DWORD selectionCount = 0;
    SFGAOF selectionAttributes = 0;
};

struct AppCommandCapability {
    AppCommandBinding binding;
    NamespaceCommandState native;
    bool enabled = false;
    bool checked = false;
    // Missing component, unavailable native state, E_PENDING and applicability
    // are kept separate from a successfully queried disabled provider.
    HRESULT status = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    // The slow-state attempt has settled for this exact capability generation.
    // Start failures also settle an attempt; ERROR_BUSY remains retryable. Its
    // actual provider status may still be E_PENDING: completion is not a
    // successful native state or permission to enable/invoke the command.
    bool slowStateCompleted = false;
    // Exact default-selection-menu fallback selected by this query. Empty for
    // actual registered/background/Ribbon-only state routes; never inferred
    // later from an arbitrary CommandStore name.
    std::wstring_view selectionVerb;
    std::vector<std::wstring_view> selectionVerbs;
};

struct AppSelectionStateBinding {
    UINT command = 0;
    std::wstring verb;
    std::vector<std::wstring> aliases;
};

std::span<const AppCommandBinding> appCommandCatalog() noexcept;
std::optional<AppCommandBinding> appCommandBinding(UINT command) noexcept;
std::wstring_view appCommandStoreName(UINT command) noexcept;
bool appCommandApplicable(const AppCommandBinding& binding,const AppCommandContext& context) noexcept;

// Fast read-only query: actual registered GetState(FALSE), a static association
// negative prefilter, or actual native ZIP SendTo target. Static positive/multi
// states remain pending until their actual worker menu supplies authority.
// It never builds full menus.
// The host should cache this once per selection/navigation/provider change.
// A mapped unavailable command returns S_OK with enabled=false and exact status;
// invalid ID/null output fails without changing output. Host-owned checked state
// and clipboard readiness still belong to the host.
HRESULT queryAppCommand(NativeNamespaceActions& actions,UINT command,
                        const AppCommandContext& context,AppCommandCapability* result);
// Completes E_PENDING on the actual registered provider, or reads a static
// association leaf from the full native selection menu on the worker STA.
// Callers retain their existing navigation/selection generation cancellation.
HRESULT startAppCommandStateTask(NativeNamespaceActions& actions,UINT command,
                                std::unique_ptr<NamespaceCommandStateTask>* result);
// Batch only cached E_PENDING capabilities from this exact native selection.
// Duplicate Open controls map to one canonical leaf. Registered-only and
// background commands are rejected; caller retains its selection generation.
// Failure preserves task/mapping outputs.
HRESULT startAppSelectionStateBatch(NativeNamespaceActions& actions,std::span<const AppCommandCapability> capabilities,
                                   std::unique_ptr<NamespaceCommandStateTask>* result,
                                   std::vector<AppSelectionStateBinding>* mapping);
// Selects the exact first enabled native alias, retaining missing/disabled and
// ambiguity precedence of the individual action worker. No inferred state.
HRESULT applyAppSelectionStateBatch(const AppSelectionStateBinding& binding,
                                   std::span<const NamespaceSelectionVerbState> states,NamespaceCommandState* result);

// Revalidates actual native command state/menu before invoking; no UI in
// headless mode. Host commands return E_NOTIMPL to the caller's own dispatcher.
HRESULT invokeAppNativeCommand(NativeNamespaceActions& actions,UINT command,
                              const AppCommandContext& context,bool headless,POINT point = {});

// Native Sort/Group/Columns and other registered cascades, with exact provider
// labels, ordinals, checked/disabled state and full property menu hierarchy.
// Borrowed popup rules and WndProc forwarding are in namespace_actions.hpp.
HRESULT queryAppCommandPopup(NativeNamespaceActions& actions,UINT command,
                             const AppCommandContext& context,NamespaceCommandPopup* result);

} // namespace explorer
