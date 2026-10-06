#pragma once

#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <string>
#include <string_view>
#include <span>
#include <cstdint>
#include <vector>

namespace explorer {

struct ContextMenuEntry {
    UINT id = 0;
    UINT type = 0;
    UINT state = 0;
    bool submenu = false;
    std::wstring label;
    std::wstring canonicalVerb;
    std::vector<ContextMenuEntry> children;

    bool separator() const noexcept { return (type & MFT_SEPARATOR) != 0; }
    bool enabled() const noexcept { return (state & (MFS_DISABLED | MFS_GRAYED)) == 0; }
};

// Owns a native Shell menu for one STA interaction. Keep it alive while tracking
// the popup and invoking its result. Returned HMENUs are borrowed; never destroy
// or reparent a submenu. This class never displays a menu or invokes a default.
class NativeContextMenu final {
public:
    NativeContextMenu() = default;
    ~NativeContextMenu();
    NativeContextMenu(const NativeContextMenu&) = delete;
    NativeContextMenu& operator=(const NativeContextMenu&) = delete;

    HRESULT createBackground(HWND owner, IShellItem* folder, IUnknown* site = nullptr,
                             UINT flags = CMF_NORMAL);
    HRESULT createSelection(HWND owner, IShellItemArray* selection, IUnknown* site = nullptr,
                            UINT flags = CMF_NORMAL);
    // Normal native selection provider, without requesting synchronous
    // population of EVERY cascade. Exact planning below can initialize the
    // requested native branch; invocation still uses its original numeric ID.
    HRESULT createSelectionForPlanning(HWND owner, IShellItemArray* selection,
                                      IUnknown* site = nullptr, UINT flags = CMF_NORMAL);
    HRESULT createForPlanning(HWND owner, IContextMenu* context, IUnknown* site = nullptr,
                              UINT flags = CMF_NORMAL);
    // Uses the SDK's CLSID_NewMenu and registered ShellNew handlers. Its popup
    // contains Folder, Shortcut and the file types installed on this computer.
    // The optional site can supply SID_SNewMenuClient and Shell-view services.
    HRESULT createNewItems(HWND owner, IShellItem* folder, IUnknown* site = nullptr,
                           UINT flags = CMF_NORMAL);
    // Also supports IContextMenu obtained from a native view's SVGIO_BACKGROUND.
    HRESULT create(HWND owner, IContextMenu* context, IUnknown* site = nullptr,
                   UINT flags = CMF_NORMAL);
    // Read-only native leaf-state query. Omits synchronous cascade population;
    // enumerate(..., false) reads only metadata already supplied by the handler.
    // Invoke and menu-message/delayed population are rejected for this instance.
    // Normal popup and invocation paths must continue to use create().
    // omitResourceVerbs may be used only when none of the requested canonical
    // leaves is cut/copy/paste/link/delete/rename/properties or an alias thereof.
    // Dynamic handlers remain enabled; an unsupported restriction interface
    // retains the ordinary native query, while supported failures propagate.
    // Any added restriction is restored after this query, including its error
    // path, so a retained provider remains suitable for a later normal menu.
    HRESULT createLeafState(IContextMenu* context, IUnknown* site = nullptr,
                            UINT flags = CMF_NORMAL, bool omitResourceVerbs = false);
    void reset() noexcept;

    HMENU menu() const noexcept { return menu_; }
    HMENU popup() const noexcept { return popup_; }
    UINT firstCommand() const noexcept { return firstCommand_; }
    UINT commandCount() const noexcept { return commandCount_; }
    bool supportsMenuMessages() const noexcept { return context2_ != nullptr || context3_ != nullptr; }
    bool supportsMenuCharacters() const noexcept { return context3_ != nullptr; }

    // Populate delayed submenus using WM_INITMENUPOPUP without showing them.
    // Labels and verbs are descriptive: invoke only the selected numeric ID.
    HRESULT enumerate(std::vector<ContextMenuEntry>& entries, bool populateSubmenus = true);
    // Ordered canonical aliases. Inspect already supplied native metadata
    // first; a known leaf never initializes an unrelated submenu. Only a
    // uniquely matched enabled native cascade is populated (once per menu
    // lifetime), with strict native errors and original hierarchy/ordinals.
    // Unknown branch names are never inferred from labels or menu positions.
    // enumerate(...,true) remains the explicit full-menu inspection API.
    HRESULT enumerateForVerbs(std::span<const std::wstring_view> verbs,
                              std::vector<ContextMenuEntry>& entries);
    // An isolated registered provider can expose one native anonymous cascade.
    // Its actual unique numeric ID is authority, never a synthesized verb or
    // translated label. Disabled parents do not receive initialization.
    HRESULT enumerateForCommand(UINT actualNativeId, std::vector<ContextMenuEntry>& entries);
    // Invoke an enabled leaf actually belonging to this menu, using its ordinal
    // offset. Zero/cancel, stale/foreign IDs, disabled items and group headers fail.
    // Native handlers may launch applications or dialogs; this is not a silent
    // testing API. control/shift preserve the modifiers used for the interaction.
    HRESULT invoke(UINT commandId, POINT screenPoint = {}, bool control = false,
                   bool shift = false);
    // Call before the owner's regular message dispatch while a popup is active.
    // On true, return result from WndProc. Native controls' owner-draw is ignored.
    bool handleMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result);

private:
    HRESULT createImpl(HWND owner, IContextMenu* context, IUnknown* site, UINT flags,
                       bool leafStateOnly, bool omitResourceVerbs = false,
                       bool synchronousCascades = true);
    HRESULT enumerateMenu(HMENU menu, UINT position, unsigned depth, unsigned& budget,
                          std::vector<ContextMenuEntry>& entries, bool populate,
                          bool strictMessages = false);
    HRESULT initializeForPlanning(HMENU menu, UINT position);
    HRESULT enumerateForTarget(std::span<const std::wstring_view> verbs, UINT actualNativeId,
                               std::vector<ContextMenuEntry>& entries);
    std::wstring canonicalVerb(UINT id) const;
    bool ownsMenu(HMENU candidate) const noexcept;
    bool selectableCommand(UINT id) const noexcept;

    static constexpr UINT firstCommand_ = 1;
    static constexpr UINT lastCommand_ = 0x7fff;
    HWND owner_ = nullptr;
    DWORD thread_ = 0;
    HMENU menu_ = nullptr;
    HMENU popup_ = nullptr;
    UINT commandCount_ = 0;
    bool siteAttached_ = false;
    bool leafStateOnly_ = false;
    std::uint64_t generation_ = 0;
    struct PlannedPopup { HMENU menu; HRESULT status; };
    std::vector<PlannedPopup> plannedPopups_;
    Microsoft::WRL::ComPtr<IContextMenu> context_;
    Microsoft::WRL::ComPtr<IContextMenu2> context2_;
    Microsoft::WRL::ComPtr<IContextMenu3> context3_;
    Microsoft::WRL::ComPtr<IObjectWithSite> objectWithSite_;
};

} // namespace explorer
