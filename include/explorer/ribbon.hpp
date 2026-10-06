#pragma once

#include "explorer/ribbon_commands.hpp"
#include "explorer/ribbon_features.hpp"
#include <windows.h>
#include <uiribbon.h>
#include <atomic>
#include <filesystem>
#include <functional>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace explorer {
struct NativePopupCapture;

struct RibbonQuickAccessItem {
    UINT command = 0; // Zero means the installed native ID is not mapped.
    UINT nativeCommand = 0;
    HRESULT commandRead = E_PENDING;
};

// Creator-STA snapshot of the actual framework collection. Its opaque state
// retains every native row, including unmapped rows and their native metadata.
// Destroy or replace it on the same STA, before resetting the Ribbon.
class RibbonQuickAccessSnapshot {
public:
    RibbonQuickAccessSnapshot();
    ~RibbonQuickAccessSnapshot();
    RibbonQuickAccessSnapshot(RibbonQuickAccessSnapshot&&) noexcept;
    RibbonQuickAccessSnapshot& operator=(RibbonQuickAccessSnapshot&&) noexcept;
    std::span<const RibbonQuickAccessItem> items() const noexcept;
    bool belowRibbon() const noexcept;
private:
    friend class NativeRibbon;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

enum class RibbonQuickAccessEditKind { Add, Remove, Move };
struct RibbonQuickAccessEdit {
    RibbonQuickAccessEditKind kind;
    UINT command = 0; // Add only.
    UINT index = 0; // Remove or Move: actual snapshot row index.
    UINT destination = 0; // Move: final row index.
};
enum class RibbonContext : UINT {
    None = 0, Picture = 1, Drive = 2, Compressed = 4, Search = 8,
    Library = 16, Recycle = 32, Application = 64, Music = 128, Video = 256, DiscImage = 512,
    Shortcut = 1024
};
constexpr RibbonContext operator|(RibbonContext a, RibbonContext b) noexcept {
    return static_cast<RibbonContext>(static_cast<UINT>(a) | static_cast<UINT>(b));
}
struct RibbonCommandState {
    bool enabled = true;
    bool checked = false;
    UINT selectedIndex = UI_COLLECTION_INVALIDINDEX;
    std::wstring label; // Empty keeps the compiled Windows 10 label.
};
struct RibbonItem {
    UINT command = 0;
    std::wstring label;
    bool pinned = false;
    std::wstring image;
    std::wstring description;
    bool enabled = true;
    bool checked = false;
    UINT invocationIndex = UI_COLLECTION_INVALIDINDEX;
    bool checkable = false;
    UINT category = UI_COLLECTION_INVALIDINDEX;
};
struct RibbonCallbacks {
    std::function<HRESULT(UINT)> execute;
    std::function<RibbonCommandState(UINT)> query;
    // Native gallery, frequent places and recent-search collections.
    std::function<std::vector<RibbonItem>(UINT)> items;
    std::function<HRESULT(UINT, UINT)> executeItem;
    std::function<HRESULT(UINT, bool)> pinItem;
    std::function<void(UINT)> heightChanged;
};
struct RibbonCollectionReadback {
    bool registered=false;
    UINT nativeType=UI_COMMANDTYPE_UNKNOWN;
    UINT sourceRequests=0;
    UINT currentVariantType=VT_EMPTY;
    HRESULT lastInvalidation=E_PENDING;
    UINT selectedRequests=0;
    UINT lastSelectedIndex=UI_COLLECTION_INVALIDINDEX;
    UINT publishedItems=0;
    UINT pendingInvalidations=0;
};

enum class RibbonLayout { Authored, InstalledWindows10 };

std::wstring_view ribbonCommandStoreName(UINT command) noexcept;

// Windows Ribbon Framework owns drawing, caption QAT, scaling, keyboard keytips,
// tooltips, accessibility objects, and built-in customization/context menus.
// All methods run on the initializing STA; reset precedes DestroyWindow/OleUninitialize.
class NativeRibbon {
public:
    NativeRibbon();
    ~NativeRibbon();
    NativeRibbon(const NativeRibbon&) = delete;
    NativeRibbon& operator=(const NativeRibbon&) = delete;
    HRESULT initialize(HWND window, HINSTANCE instance, RibbonCallbacks callbacks,
                       RibbonLayout layout = RibbonLayout::Authored);
    void reset() noexcept;
    bool valid() const noexcept;
    UINT height() const noexcept;
    HRESULT invalidate(UINT command = 0);
    HRESULT invalidateState(UINT command = 0);
    // Refresh only sources actually requested by the native framework.
    // ItemsSource is separate from value/state invalidation in the framework.
    HRESULT invalidateItems(UINT command = 0);
    HRESULT collectionReadback(UINT command,RibbonCollectionReadback& output) const;
    HRESULT flush();
    HRESULT setContexts(RibbonContext contexts, bool activate = false);
    HRESULT setComputerMode(bool computer);
    HRESULT setNetworkMode(bool network, bool activeDirectory = true);
    HRESULT setDriveType(UINT nativeDriveType);
    RibbonFeatures features() const noexcept;
    HRESULT contextAvailable(RibbonContext context,UINT& nativeIdentifier,UINT& availability) const;
    RibbonLayout layout() const noexcept;
    HRESULT installedLayoutStatus() const noexcept;
    HRESULT selectTab(UINT tab);
    // Capture-only: expand the actual Date modified parent on the guarded
    // private desktop and read all requested physical native rows. No Invoke.
    HRESULT expandSearchDateMenu(std::span<const std::wstring> expectedRows, UINT& matchedRows,
                                 NativePopupCapture* popup = nullptr);
    HRESULT setMinimized(bool minimized);
    HRESULT minimized(bool& value) const;
    HRESULT setQuickAccessBelow(bool below);
    HRESULT quickAccessBelow(bool& value) const;
    HRESULT quickAccessCommands(std::vector<UINT>& commands) const;
    HRESULT setQuickAccessCommands(std::span<const UINT> commands);
    HRESULT quickAccessSnapshot(RibbonQuickAccessSnapshot& output) const;
    // Rejects a stale collection/dock/row binding. Untouched rows retain their
    // exact native objects; no native labels, flags or row types are rebuilt.
    // A failed Move insertion rolls back only an unchanged intermediate list.
    HRESULT editQuickAccess(const RibbonQuickAccessSnapshot&, const RibbonQuickAccessEdit&);
    HRESULT saveSettings(const std::filesystem::path& path) const;
    // Versioned application envelope retains ordered actual native IDs around
    // opaque native settings. Legacy native streams still load, but contain no
    // recoverable custom-order manifest. No native stream bytes are interpreted.
    HRESULT loadSettings(const std::filesystem::path& path);
    // Native image property is invalidation-only in IUIFramework. This returns
    // the same cached system-resource IUIImage used by UpdateProperty.
    HRESULT commandImage(UINT command, bool large, IUIImage** output);
    HRESULT itemImage(const std::wstring& specification, bool large, IUIImage** output);
    HRESULT commandLabel(UINT command, std::wstring& output) const;
    // Selection-dependent native handlers (for example Open) supply their real
    // association icon. Empty restores the initially registered system image.
    HRESULT setCommandImageSpec(UINT command, const std::wstring& specification);
    // Borrowed pointer for headless property readback and host diagnostics.
    IUIFramework* framework() const noexcept;
    // Borrowed native object/identity for owner-STA diagnostic property reads.
    IUIFramework* nativeFramework() const noexcept;
    UINT nativeCommandId(UINT command) const noexcept;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    std::uint64_t bindingGeneration_ = 0;
    // Independent entry epoch survives NativeRibbon destruction in retired callbacks.
    std::shared_ptr<std::atomic<std::uint64_t>> callbackEpoch_;
    HRESULT setViewSetting(REFPROPERTYKEY key,const PROPVARIANT& value);
};
}
