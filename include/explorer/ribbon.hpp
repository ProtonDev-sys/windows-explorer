#pragma once

#include "explorer/ribbon_commands.hpp"
#include "explorer/ribbon_features.hpp"
#include <windows.h>
#include <uiribbon.h>
#include <atomic>
#include <array>
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
struct RibbonImageObservationStats {
    std::uint64_t requests = 0, delivered = 0, dropped = 0, reentrant = 0;
};
// Diagnostic-only completion: observer rows are valid after callback return
// only when bindingStable is true and result equals normalUpdateResult.
struct RibbonImageObservationCompletion {
    HRESULT result = E_PENDING;
    bool bindingStable = false;
    bool observerThrew = false;
};
enum class RibbonObservedImagePath { Unmapped, CommandMetadata, DynamicItemMetadata, InstalledNativeFirstCurrent, InstalledNativeCached };
struct RibbonImageObservation {
    UINT nativeCommand = 0, applicationCommand = 0;
    UINT nativeType = UI_COMMANDTYPE_UNKNOWN;
    bool large = false, currentPresent = false, installedLayout = false;
    bool nativeFirstObservation = false, nativeImageCached = false, nativeFirstAdmissionFailed = false;
    VARTYPE currentType = VT_EMPTY, returnedType = VT_EMPTY;
    HRESULT currentImageQuery = E_PENDING, returnedImageQuery = E_PENDING;
    HRESULT normalUpdateResult = E_PENDING;
    RibbonObservedImagePath path = RibbonObservedImagePath::Unmapped;
    DWORD creatorThread = 0;
    HWND window = nullptr;
    UINT hwndDpi = 0;
    std::uint64_t windowGeneration = 0, callbackEpoch = 0, ordinal = 0;
    // Borrowed only for the observer invocation. Retain by AddRef on this STA
    // for later GetBitmap/raw-DIB reads; never read pixels inside the callback.
    IUIImage* currentImage = nullptr;
    IUIImage* returnedImage = nullptr;
    std::shared_ptr<const RibbonImageObservationCompletion> completion;
};

// Opt-in bounded plain text receipts. No observer callback, COM retention,
// framework reentry, resource replacement or change to normal label precedence.
struct RibbonTextValueReceipt {
    VARTYPE type=VT_EMPTY;
    bool present=false,stringPresent=false,truncated=false;
    std::array<wchar_t,1024> text{};
};
struct RibbonTextReceipt {
    UINT nativeCommand=0,applicationCommand=0,nativeType=UI_COMMANDTYPE_UNKNOWN;
    bool label=false,bindingStable=false;
    DWORD creatorThread=0;
    HWND window=nullptr;
    std::uint64_t ordinal=0,windowGeneration=0,entryEpoch=0,exitEpoch=0,entryRevision=0,exitRevision=0;
    HRESULT normalResult=E_PENDING;
    RibbonTextValueReceipt current,returned;
};
struct RibbonTextDiagnostics {
    // Fixture configures an immutable exact physical-ID filter before LoadUI.
    std::array<UINT,16> nativeCommands{};
    UINT filterCount=0,count=0;
    std::uint64_t requests=0;
    bool overflow=false;
    std::array<RibbonTextReceipt,64> receipts{};
};

struct RibbonCallbacks {
    std::function<HRESULT(UINT)> execute;
    std::function<RibbonCommandState(UINT)> query;
    // Native gallery, frequent places and recent-search collections.
    std::function<std::vector<RibbonItem>(UINT)> items;
    std::function<HRESULT(UINT, UINT)> executeItem;
    std::function<HRESULT(UINT, bool)> pinItem;
    std::function<void(UINT)> heightChanged;
    // Empty by default. Observer must only retain interfaces/metadata: no native
    // framework calls, pixel reads, command execution or property mutations.
    // Creator-STA methods return E_PENDING during delivery; reset is fenced.
    std::function<void(const RibbonImageObservation&)> observeImageRequest;
    // Empty by default. Plain first-physical-current/normal-return receipts
    // survive reset for after-return diagnostics on the original creator STA.
    std::shared_ptr<RibbonTextDiagnostics> textDiagnostics;
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

// Fixed opt-in observations of actual native RecentItems callbacks. The arrays
// contain only values decoded by the existing production path; no SAFEARRAY is
// read or manufactured by diagnostics. No observer callback or provider ref.
enum class RibbonRecentItemsReceiptKind { Source, NormalCommit, NormalPinned, RetiredCommit, Reset };
struct RibbonRecentItemsReceipt {
    RibbonRecentItemsReceiptKind kind=RibbonRecentItemsReceiptKind::Source;
    std::uint64_t entryEpoch=0,exitEpoch=0,entryRevision=0,exitRevision=0;
    UINT initialCount=0,decodedCount=0,callbackCount=0,requestedIndex=UI_COLLECTION_INVALIDINDEX;
    ULONGLONG entryTick=0,exitTick=0;
    std::array<ULONGLONG,64> callbackReturnTicks{};
    LONG first=0,last=-1;
    VARTYPE variantType=VT_EMPTY;
    bool retired=false,exitRetired=false,windowDestroyed=false,exitWindowDestroyed=false;
    bool recentItemsKey=false,finalOverride=false,shutdownActive=false,shutdownStarted=false,requestedPin=false;
    HRESULT result=E_PENDING;
    std::array<bool,64> initialPins{},decodedPins{},decoded{},dispatched{};
    std::array<HRESULT,64> callbackResults{};
};
struct RibbonRecentItemsDiagnostics {
    std::array<RibbonRecentItemsReceipt,64> receipts{};
    UINT count=0;
    bool overflow=false;
};

enum class RibbonLayout { Authored, InstalledWindows10 };

std::wstring_view ribbonCommandStoreName(UINT command) noexcept;

// Windows Ribbon Framework owns drawing, caption QAT, scaling, keyboard keytips,
// tooltips, accessibility objects, and built-in customization/context menus.
// All methods run on the initializing STA; reset precedes owner WM_NCDESTROY/OleUninitialize.
class NativeRibbon {
public:
    NativeRibbon();
    ~NativeRibbon();
    NativeRibbon(const NativeRibbon&) = delete;
    NativeRibbon& operator=(const NativeRibbon&) = delete;
    HRESULT initialize(HWND window, HINSTANCE instance, RibbonCallbacks callbacks,
                       RibbonLayout layout = RibbonLayout::Authored);
    void reset() noexcept;
    // Owns only the original creator-STA RecentItems batch around a genuine
    // host close continuation. Never creates an Execute or SAFEARRAY.
    LRESULT dispatchCloseWithFinalPinCallback(const std::function<HRESULT(UINT,bool)>& callback,
        const std::function<LRESULT()>& continuation);
    // Creator/HWND matched owner cleanup before the binding's lower native
    // window chain retires, including reset outside a close dispatch.
    HRESULT setOwnerWindowRetirementHook(HWND,const std::function<bool()>&);
    // Creator-only: drain a deferred original lower chain after its complete
    // owner-window destruction/close continuation. Never retires a newer Impl.
    void finishOwnerWindowRetirement() noexcept;
    bool closeDispatchActive() const noexcept; // fixed metadata, no native calls
    // Retire only the original close framework. A genuine newer publication
    // on another owned host must survive the old App window's cleanup.
    void resetClosingFramework() noexcept;
    void resetClosingFrameworkWithFinalPinCallback(const std::function<HRESULT(UINT,bool)>& callback) noexcept;
    // Reset/initialize entry generation, including an entry with no live Impl.
    std::uint64_t callbackEntryEpoch() const noexcept;
    // Creator-STA, fixture opt-in only. Fixed receipts survive native reset;
    // readback performs no native calls and never supplies source/pin values.
    HRESULT enableRecentItemsDiagnostics();
    void recentItemsDiagnostics(RibbonRecentItemsDiagnostics& output) const noexcept;
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
    // Explicit system-resource fallback image extraction/cache. Installed
    // UpdateProperty can instead return retained first-native-current artwork.
    // This API does not expose or prove the installed native resource cache.
    HRESULT commandImage(UINT command, bool large, IUIImage** output);
    // Opt-in diagnostics only; all zero when no observer was registered.
    HRESULT imageObservationStats(RibbonImageObservationStats& output) const;
    HRESULT itemImage(const std::wstring& specification, bool large, IUIImage** output);
    HRESULT commandLabel(UINT command, std::wstring& output) const;
    // Selection-dependent native handlers (for example Open) supply their real
    // association icon. Empty restores native-first artwork when available,
    // otherwise the initially registered system-resource fallback.
    HRESULT setCommandImageSpec(UINT command, const std::wstring& specification);
    // Borrowed pointer for headless property readback and host diagnostics.
    IUIFramework* framework() const noexcept;
    // Borrowed native object/identity for owner-STA diagnostic property reads.
    IUIFramework* nativeFramework() const noexcept;
    UINT nativeCommandId(UINT command) const noexcept;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    std::shared_ptr<Impl> closingImpl_; // only the original dispatchClose stack
    std::shared_ptr<Impl> delayedWindowImpl_; // one registered original App binding
    bool ownerHookRegistered_=false,deferredRetirementDrainActive_=false;
    std::uint64_t bindingGeneration_ = 0;
    // Independent entry epoch survives NativeRibbon destruction in retired callbacks.
    std::shared_ptr<std::atomic<std::uint64_t>> callbackEpoch_;
    std::shared_ptr<RibbonRecentItemsDiagnostics> recentItemsDiagnostics_;
    void resetImpl(const std::function<HRESULT(UINT, bool)>* finalPinCallback) noexcept;
    void resetClosingImpl(const std::function<HRESULT(UINT,bool)>* finalPinCallback) noexcept;
    void retireImpl(std::shared_ptr<Impl>,const std::function<HRESULT(UINT,bool)>*,std::uint64_t,bool) noexcept;
    HRESULT setViewSetting(REFPROPERTYKEY key,const PROPVARIANT& value);
};
}
