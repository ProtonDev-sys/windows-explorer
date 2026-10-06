#pragma once
#include "explorer/core.hpp"
#include "explorer/commands.hpp"
#include "explorer/input.hpp"
#include "explorer/context_menu.hpp"
#include "explorer/quick_access.hpp"
#include "explorer/library.hpp"
#include "explorer/headless_visual.hpp"
#include "explorer/ribbon.hpp"
#include "explorer/namespace_actions.hpp"
#include "explorer/breadcrumb.hpp"
#include "explorer/app_commands.hpp"
#include "explorer/search_history.hpp"
#include "explorer/address_history.hpp"
#include "explorer/search.hpp"
#include "explorer/search_backing.hpp"
#include "explorer/search_refinement.hpp"
#include "explorer/search_window.hpp"
#include "explorer/live_search.hpp"
#include "explorer/ui_direction.hpp"
#include "explorer/preview_host.hpp"
#include <shlobj.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <servprov.h>
#include <wrl/client.h>
#include <atomic>
#include <memory>
#include <vector>
#include <functional>
#include <exception>
#include <array>
#include <optional>
#include <map>

namespace explorer {
using Microsoft::WRL::ComPtr;
struct PidlDeleter {
    using pointer = LPITEMIDLIST;
    void operator()(pointer p) const noexcept { CoTaskMemFree(p); }
};
using Pidl = std::unique_ptr<ITEMIDLIST, PidlDeleter>;

// Dedicated headless pane-host provenance. Fixed storage, no interface/service
// behavior changes; counts include QueryInterface delegated by QueryService.
struct HeadlessPaneHostingRequests {
    struct FrameQuery { HRESULT result = E_PENDING; UINT calls = 0; };
    struct ServiceQuery { GUID service{}, iid{}; HRESULT result = E_PENDING; UINT calls = 0; };
    struct PaneQuery { GUID pane{}; HRESULT result = E_PENDING; DWORD flags = 0; bool outputPresent = false; UINT calls = 0; };
    std::array<FrameQuery, 32> frameQueries{};
    std::array<ServiceQuery, 64> services{};
    std::array<PaneQuery, 32> panes{};
    UINT frameCount = 0, serviceCount = 0, paneCount = 0;
    bool overflow = false;
    void increment(UINT& calls) noexcept {
        if (calls == ~UINT{0}) overflow = true; else ++calls;
    }
    void frame(REFIID iid, HRESULT result) noexcept {
        if (iid != IID_IPreviewHandlerFrame) return;
        for (UINT index = 0; index < frameCount; ++index) if (frameQueries[index].result == result) {
            increment(frameQueries[index].calls); return;
        }
        if (frameCount == frameQueries.size()) { overflow = true; return; }
        frameQueries[frameCount++] = {result, 1};
    }
    void service(REFGUID sid, REFIID iid, HRESULT result) noexcept {
        for (UINT index = 0; index < serviceCount; ++index) {
            auto& row = services[index];
            if (row.service == sid && row.iid == iid && row.result == result) { increment(row.calls); return; }
        }
        if (serviceCount == services.size()) { overflow = true; return; }
        services[serviceCount++] = {sid, iid, result, 1};
    }
    void pane(REFEXPLORERPANE paneId, HRESULT result, const EXPLORERPANESTATE* state) noexcept {
        const DWORD flags = state ? static_cast<DWORD>(*state) : 0;
        for (UINT index = 0; index < paneCount; ++index) {
            auto& row = panes[index];
            if (row.pane == paneId && row.result == result && row.flags == flags && row.outputPresent == (state != nullptr)) {
                increment(row.calls); return;
            }
        }
        if (paneCount == panes.size()) { overflow = true; return; }
        panes[paneCount++] = {paneId, result, flags, state != nullptr, 1};
    }
};
enum class AddressContextCommand : UINT { Edit = 1280, Copy = 1281, CopyText = 1282, DeleteHistory = 1283 };
struct AddressContextSnapshot {
    Pidl location;
    std::wstring text;
};
struct VisualScene {
    std::wstring page = L"Home";
    std::wstring select;
    int width = 735;
    int height = 503;
    UINT dpi = 96;
    ViewMode view = ViewMode::Details;
    bool details = false;
        bool collapsed = false;
    bool nativeView = false;
    bool openSearchDateMenu = false;
};
struct HeadlessStateWorkerTiming {
    UINT command = 0;
    bool selectionBatch = false;
    bool selectionKinds = false;
    HRESULT status = E_PENDING;
    HRESULT timingStatus = E_PENDING;
    NamespaceCommandStateTimings native;
};
struct HeadlessCommandTimings {
    HRESULT status = E_PENDING;
    unsigned long long updateCount = 0;
    double totalMs = 0;
    double viewReadbackMs = 0;
    double selectionCountAttributesMs = 0;
    double selectionStatusMs = 0;
    double selectionHostEligibilityMs = 0;
    double selectionKindsMs = 0;
    double selectionKindsSchedulingMs = 0;
    double selectionKindsPublicationMs = 0;
    double selectionKindsReadyDelayMs = 0;
    HRESULT selectionKindsStatus = S_OK;
    double namespacePreparationMs = 0;
    double providerCatalogMs = 0;
    double stateTaskSchedulingMs = 0;
    double contextMs = 0;
    double ribbonInvalidationMs = 0;
    std::vector<HeadlessStateWorkerTiming> completedStateWorkers;
};
struct HeadlessStartupTimings {
    double clockStartMs = 0;
    double desktopReadyMs = 0;
    double platformReadyMs = 0;
    double createReturnedMs = 0;
};

class ExplorerApp final : public IExplorerBrowserEvents, public IServiceProvider,
                          public IExplorerPaneVisibility, public ICommDlgBrowser3, public IFolderFilter {
public:
    ExplorerApp(HINSTANCE instance, bool headless, RibbonLayout ribbonLayout = RibbonLayout::Authored);
    HRESULT prepareHeadlessVisual(const VisualScene& scene);
    HRESULT create(const std::wstring& location);
    HRESULT prepareSearchWindowContext(const SearchWindowContext& context);
    int run(int showCommand);
    int headlessBenchmark(const std::filesystem::path& report, const HeadlessStartupTimings& startup);
    int headlessSmoke(const std::filesystem::path& report, bool libraryOnly = false);
    int headlessVisual(const PrivateDesktop& desktop, const std::filesystem::path& screenshot,
                       const std::filesystem::path& report, const VisualScene& scene);
    HWND window() const noexcept { return window_; }
    HRESULT shutdownStatus() const noexcept { return shutdownStatus_; }
    HRESULT shutdownPreview() noexcept;
    HeadlessCommandTimings headlessCommandTimings() const noexcept { return commandTimings_; }
    void resetHeadlessCommandTimings() noexcept { if(headless_)commandTimings_={}; }
    bool commandStatesPending() const noexcept {
        if(selectionKindsRequest_.pending)return true;
        if(selectionStateBatch_||!commandStateTasks_.empty())return true;
        for(const auto& entry:commandCapabilities_)
            if(entry.second.status==E_PENDING&&!entry.second.slowStateCompleted)return true;
        return false;
    }
    unsigned long long headlessDeferredCommandUpdates() const noexcept { return headless_?deferredCommandUpdates_:0; }
    bool preprocess(MSG& message);
    HRESULT navigate(const std::wstring& location, bool typedAddress = false);
    HRESULT execute(UINT command);
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE QueryService(REFGUID service, REFIID iid, void** out) override;
    HRESULT STDMETHODCALLTYPE GetPaneState(REFEXPLORERPANE pane, EXPLORERPANESTATE* state) override;
    HRESULT STDMETHODCALLTYPE OnNavigationPending(PCIDLIST_ABSOLUTE) override;
    HRESULT STDMETHODCALLTYPE OnViewCreated(IShellView*) override;
    HRESULT STDMETHODCALLTYPE OnNavigationComplete(PCIDLIST_ABSOLUTE) override;
    HRESULT STDMETHODCALLTYPE OnNavigationFailed(PCIDLIST_ABSOLUTE) override;
    HRESULT STDMETHODCALLTYPE OnDefaultCommand(IShellView*) override;
    HRESULT STDMETHODCALLTYPE OnStateChange(IShellView*, ULONG) override;
    HRESULT STDMETHODCALLTYPE IncludeObject(IShellView*, PCUITEMID_CHILD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Notify(IShellView*, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDefaultMenuText(IShellView*, LPWSTR text, int size) override;
    HRESULT STDMETHODCALLTYPE GetViewFlags(DWORD* flags) override;
    HRESULT STDMETHODCALLTYPE OnColumnClicked(IShellView*, int) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE GetCurrentFilter(LPWSTR text, int size) override;
    HRESULT STDMETHODCALLTYPE OnPreViewCreated(IShellView*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ShouldShow(IShellFolder*, PCIDLIST_ABSOLUTE, PCUITEMID_CHILD) override;
    HRESULT STDMETHODCALLTYPE GetEnumFlags(IShellFolder*, PCIDLIST_ABSOLUTE, HWND*, DWORD*) override;
private:
    static constexpr UINT PreviewResult = WM_APP + 7;
    static constexpr UINT PreviewChange = WM_APP + 8;
    static constexpr UINT PreviewLayout = WM_APP + 9;
    HRESULT openLongSavedSearch(IShellItem* item, const std::wstring& typedAddress = {});
    ~ExplorerApp();
    HRESULT shutdownStatus_ = S_OK;
    static LRESULT CALLBACK windowProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK editProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT onMessage(UINT, WPARAM, LPARAM);
    HRESULT createControls();
    void layout();
    HRESULT createPreviewPane();
    struct PreviewCallScope {
        ExplorerApp& owner;
        bool retained=false;
        explicit PreviewCallScope(ExplorerApp& value) noexcept;
        ~PreviewCallScope();
    };
    HRESULT layoutPreviewPane();
    HRESULT resetPreviewLayout() noexcept;
    HRESULT resetPreviewGrip() noexcept;
    bool previewGripCurrent() const noexcept;
    void queuePreviewLayout() noexcept;
    static LRESULT CALLBACK previewLayoutProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static LRESULT CALLBACK previewGripProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    std::uint64_t invalidatePreview(PreviewEmptyReason reason = PreviewEmptyReason::None) noexcept;
    void updatePreviewTarget();
    HRESULT updatePreviewVisuals();
    void pollPreview();
    bool previewSourceCurrent(bool readNative);
    HRESULT previewAccelerator(std::uint64_t epoch, const MSG& message);
    bool previewHasFocus() const;
    void registerPreviewChanges();
    void previewChanged(WPARAM, LPARAM);
    void rebuildRibbon();
    void rebuildQuickAccess();
    void updateCommands();
    void updateNamespaceImpl();
    void updateContextTabsImpl();
    void deferCommandRefresh();
    void finishCommandRefresh() noexcept;
    void cancelCommandStatesImpl();
    struct CommandRefreshScope {
        ExplorerApp& owner;
        int exceptions=std::uncaught_exceptions();
        explicit CommandRefreshScope(ExplorerApp& value):owner(value){owner.commandRefreshActive_=true;}
        ~CommandRefreshScope(){if(std::uncaught_exceptions()>exceptions)owner.namespaceDirty_=true;owner.finishCommandRefresh();}
    };
    void scheduleDeferredUpdate();
    double commandTimingNow();
    void updateBreadcrumbs();
    void updateContextTabs();
    RibbonCommandState ribbonState(UINT command);
    std::vector<RibbonItem> ribbonItems(UINT command);
    HRESULT appendSearchRefinementMenu(UINT command, HMENU menu, std::vector<std::wstring>* expressions) const;
    HRESULT executeRibbon(UINT command);
    HRESULT executeRibbonItem(UINT command, UINT item);
    void updateNamespace();
    AppCommandContext commandContext() const;
    HRESULT applyNavigationOptions(bool expandOnce = false);
    void advanceNavigationExpansion();
    void cancelCommandStates();
    void startPendingCommandStates();
    void startPendingSelectionKinds();
    void pollSelectionKinds();
    bool selectionKindsSourceCurrent() const noexcept;
    void refreshFrequentPlaces();
    void cancelFrequentPlaces();
    void pollFrequentPlaces();
    HRESULT pinFrequentPlace(UINT item,bool pinned);
    struct ShutdownPinTransaction;
    struct OwnedCloseFrame;
    HRESULT installOwnedCloseHook() noexcept;
    void detachOwnedCloseHook() noexcept;
    bool retireOwnedCloseHook() noexcept;
    static LRESULT CALLBACK ownedCloseProc(HWND,UINT,WPARAM,LPARAM);
    LRESULT dispatchOwnedClose(HWND,WPARAM,LPARAM,WNDPROC) noexcept;
    bool ownedCloseContinuation() const noexcept;
    bool consumeOwnedCloseContinuation() noexcept;
    void observeShutdownWindowMessage(UINT,bool originalCloseMessage) noexcept;
    bool captureShutdownPins(ShutdownPinTransaction&);
    HRESULT commitShutdownPin(ShutdownPinTransaction&,UINT,bool);
    void resetRibbonForClose() noexcept;
    bool shutdownPinSourceCurrent(const ShutdownPinTransaction& transaction) const noexcept;
    HRESULT pinShutdownFrequentPlace(ShutdownPinTransaction& transaction,UINT item,bool pinned);
    HRESULT resolveFrequentPlacePin(HWND owner,IShellItem* item,IShellView* site,bool pinned,
        const std::function<bool()>& current,
        const std::function<HRESULT(IShellItem*,const ContextMenuEntry&)>* resolved = nullptr);
    friend struct ShutdownPinsNativeFixture;
#if defined(EXPLORER_HOSTED_PIN_PERSISTENCE_FIXTURE)
    friend struct HostedPinPersistenceFixture;
#endif
    void pollCommandStates();
    void completeCommandState(UINT command,HRESULT status,const NamespaceCommandState* native);
    void initializeBreadcrumbDrop();
    void beginBreadcrumbMenu(UINT command);
    void pollBreadcrumbMenu();
    void fitBreadcrumbs(int width);
    std::optional<size_t> breadcrumbAncestor(UINT command) const;
    HRESULT breadcrumbDropdownTarget(UINT command, IShellItem** parent, IShellItem** selected) const;
    HRESULT createBreadcrumbOverflowMenu(HMENU* result, std::vector<Pidl>* targets) const;
    HRESULT showBreadcrumbOverflow();
    HRESULT browseBreadcrumb(UINT command);
    HRESULT browseBreadcrumbTarget(PCIDLIST_ABSOLUTE target, unsigned generation);
    HRESULT cycleFocus(bool backwards);
    HRESULT cycleToolbarFocus(bool backwards);
    std::optional<FocusRegion> currentFocusRegion() const;
    HRESULT toggleFullscreen();
    HRESULT sizeColumns();
    HRESULT saveSearch();
    HRESULT captureActiveSearchPresentation();
    HRESULT openFileLocation();
    HRESULT newLibrary();
    HRESULT includeLibraryFolder();
    HRESULT commitLibrary();
    void reloadLibrary();
    void applyPendingSelection();
    void pruneSearchCaches(size_t maximumCachedLocations = 100);
    void initializeSearchRefinementChoices();
    void refreshSearchRefinements();
    void rememberSearchCacheHistory(PCIDLIST_ABSOLUTE location);
    HRESULT clearSearchHistory(const std::filesystem::path* ownedHeadlessPath = nullptr);
    HRESULT buildSearchTarget(const std::wstring& query, const std::vector<SearchScopeRule>& rules, SearchFolderBuild* result);
    HRESULT closeSearchBackings();
    HRESULT startSearch(const std::wstring& query, bool recursive,
                        std::optional<size_t> category = {}, const std::wstring& filter = L"",
                        const LiveSearchRequest* liveRequest = nullptr);
    void setSearchText(const std::wstring& text, bool synchronizePolicy = true);
    void cancelLiveSearch();
    void scheduleLiveSearch();
    HRESULT processLiveSearch();
    friend struct SearchBackingNativeFixture;
    // Native call-stack lifetime only; unlike the live-query dispatch scope,
    // this does not suppress timer/Enter submission or reschedule a query.
    struct SearchNativeCallScope {
        ExplorerApp& owner;
        bool previous;
        explicit SearchNativeCallScope(ExplorerApp& value):owner(value),previous(value.searchNativeCallsActive_){owner.searchNativeCallsActive_=true;}
        ~SearchNativeCallScope(){
            owner.searchNativeCallsActive_=previous;
            if(!previous&&!owner.liveSearchDispatchActive_&&owner.searchClosePending_&&owner.window_&&IsWindow(owner.window_))
                PostMessageW(owner.window_,WM_CLOSE,0,0);
        }
    };
    struct LiveSearchDispatchScope {
        ExplorerApp& owner;
        bool previous;
        explicit LiveSearchDispatchScope(ExplorerApp& value):owner(value),previous(value.liveSearchDispatchActive_){owner.liveSearchDispatchActive_=true;}
        ~LiveSearchDispatchScope(){
            owner.liveSearchDispatchActive_=previous;
            if(!previous) {
                if(!owner.searchNativeCallsActive_&&owner.searchClosePending_&&owner.window_&&IsWindow(owner.window_))PostMessageW(owner.window_,WM_CLOSE,0,0);
                owner.scheduleLiveSearch();
            }
        }
    };
    bool completeLiveSearchNavigation(PCIDLIST_ABSOLUTE target, HRESULT result);
    void rememberQuery(const std::wstring& query);
    void editAddress();
    void finishAddress(bool navigateNow);
    HMENU createTypedAddressMenu() const;
    HRESULT addressContextSnapshot(AddressContextSnapshot* result) const;
    HRESULT createAddressContextMenu(HMENU* result) const;
    HRESULT executeAddressContext(AddressContextCommand command, const AddressContextSnapshot& target);
    HRESULT showAddressContextMenu(POINT point);
    void rememberAddressNavigation(PCIDLIST_ABSOLUTE location);
    void refreshAddressHistoryPolicy();
    HRESULT createBrowser();
    void destroyBrowser();
    void finishWindowDestruction();
    void reconcileMissingWindowDestruction();
    HRESULT recreateBrowser(UINT toggleCommand);
    HRESULT browseHistory(int offset);
    HRESULT browseHistoryLocation(PCIDLIST_ABSOLUTE location, int originalIndex);
    HRESULT setView(ViewMode mode);
    HRESULT setSort(const PROPERTYKEY& key);
    HRESULT setGroup(const PROPERTYKEY& key);
    HRESULT selection(ComPtr<IShellItemArray>& out, bool folderIfEmpty = false);
    HRESULT currentFolder(ComPtr<IShellItem>& out);
    HRESULT nativeVerb(const wchar_t* verb, bool folderIfEmpty = false);
    HRESULT showProperties(const wchar_t* page);
    void popup(UINT command, HWND anchor = nullptr);
    void showError(HRESULT hr, const wchar_t* action);
    HRESULT persist();
    void updateCaptionIcon();
    HRESULT refreshCabinetPolicy();
    void updateFrameTitle();
    void updateRibbonCollapseButton();
    HRESULT openNewWindow(const std::wstring& location);
    HRESULT currentSearchWindowContext(SearchWindowContext* result);
    int px(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }
    std::atomic<ULONG> references_{1};
    HINSTANCE instance_;
    bool headless_;
    bool headlessPaneHostingTrace_ = false;
    HeadlessPaneHostingRequests headlessPaneHostingRequests_;
    UiDirectionPolicy uiDirection_;
    // Declared only by an owned private headless fixture before any HWND exists.
    std::optional<bool> headlessDirectionOverride_;
    bool closing_ = false;
    bool windowDestructionCleanupStarted_=false;
    bool browserInitialized_ = false;
    bool addressEditing_ = false;
    bool navigating_ = false;
    bool searchActive_ = false;
    bool pendingSearchActive_ = false;
    bool searchBackground_ = false;
    bool pendingSearchBackground_ = false;
    bool checkboxes_ = false;
    bool ascending_ = true;
    PROPERTYKEY sortProperty_{};
    PROPERTYKEY groupProperty_{};
    bool sortPropertyValid_ = false;
    bool groupPropertyValid_ = false;
    IFolderView2* orderStateView_ = nullptr;
    unsigned long long orderStateNavigation_ = 0;
    bool selectionStateDirty_ = true;
    bool deferredUpdateQueued_ = false;
    bool commandRefreshActive_ = false;
    bool commandRefreshPending_ = false;
    bool commandStatesCancelPending_ = false;
    bool commandItemsRefreshPending_ = false;
    unsigned long long deferredCommandUpdates_ = 0;
    // Headless diagnostics: CDBOSC values 0..4, with other values at index 5.
    std::array<unsigned long long,6> headlessCurrentViewStateEvents_{},headlessStaleViewStateEvents_{};
    unsigned long long headlessEquivalentSelectionRefreshes_=0,headlessUncertainSelectionRefreshes_=0;
    std::function<void()> headlessCommandReentryProbe_;
    // Owned headless regression only: consume one finished native state query
    // as a provider that honestly returns E_PENDING even with slow work allowed.
    std::optional<UINT> headlessCompletedPendingCommand_;
    HRESULT headlessCompletedPendingOriginalStatus_ = E_PENDING;
    bool filesystemFolder_ = false;
    bool physicalDirectory_ = false;
    bool fullscreen_ = false;
    bool searchRecursive_ = true;
    bool showSuperHidden_ = false;
    bool fullPathTitle_ = false;
    bool newWindowMode_ = false;
    bool saveLocalView_ = true;
    HRESULT cabinetPolicyStatus_ = E_PENDING;
    bool quickAccessLocation_ = false;
    bool librariesRoot_ = false;
    bool nativeLibraryFactoryReady_ = false;
    bool searchSuggestionsAllowed_ = true;
    bool typedAddressHistoryAllowed_ = true;
    bool typedAddressHistoryLoaded_ = false;
    HRESULT addressHistoryStatus_ = S_OK;
    HRESULT searchHistorySaveStatus_ = E_PENDING;
    HRESULT pendingSearchHistorySaveError_ = S_OK;
    struct PersistenceStatus {
        HRESULT searchHistory = E_PENDING, addressHistory = E_PENDING;
        HRESULT windowPlacement = E_PENDING, ribbonState = E_PENDING;
        HRESULT preferences = E_PENDING, ribbonSettings = E_PENDING;
        HRESULT result = E_PENDING;
    } persistenceStatus_;
    HeadlessCommandTimings commandTimings_;
    bool searchResizing_ = false;
    int searchDragOffset_ = 0;
    LONG_PTR windowStyle_ = 0;
    WINDOWPLACEMENT windowPlacement_{sizeof(WINDOWPLACEMENT)};
    RECT windowRect_{};
    HWND window_ = nullptr, nav_ = nullptr, address_ = nullptr;
    HWND breadcrumbs_ = nullptr, search_ = nullptr, addressActions_ = nullptr;
    HWND ribbonCollapse_=nullptr,ribbonCollapseTooltip_=nullptr;
    HWND previewPane_=nullptr,previewRender_=nullptr,previewText_=nullptr;
    HWND previewGrip_=nullptr;
    bool previewGripRetiring_=false;
    HRESULT previewGripRetirementStatus_=S_OK;
    std::wstring previewSelectText_,previewUnavailableText_;
    std::unique_ptr<NativePreviewHost> previewHost_;
    struct PreviewTicket {
        ComPtr<IShellView> view;
        ComPtr<IFolderView2> folderView;
        Pidl location,item;
        std::uint64_t epoch=0,revision=0;
        unsigned navigation=0;
    } previewTicket_;
    std::uint64_t previewEpoch_=0;
    bool previewDirty_=true,previewResizing_=false,previewPaneCreating_=false;
    bool previewClosePending_=false,destroying_=false;
    unsigned previewCallsActive_=0;
    int previewDragOffset_=0;
    RECT previewSplitter_{};
    struct PreviewLayoutRecord {
        ComPtr<IShellView> view;
        HWND window=nullptr,parent=nullptr,frame=nullptr;
        Pidl location;
        unsigned navigation=0;
        UINT dpi=0;
        bool rtl=false,parentSlot=false,cropped=false,navigationPane=false,detailsPane=false;
        RECT parentClient{},nativeInsets{},uncropped{},crop{};
    } previewLayout_;
    RECT previewContentBounds_{};
    HRESULT previewLayoutStatus_=S_FALSE;
    HRESULT previewLayoutCleanupStatus_=S_FALSE;
    bool previewLayoutActive_=false,previewLayoutQueued_=false,previewLayoutAgain_=false;
    ULONG previewChangeCookie_=0;
    std::wstring ribbonCollapseTip_;
    std::map<UINT, std::wstring> navigationTooltipText_;
    HICON folderIcon_ = nullptr;
    HICON largeIcon_ = nullptr;
    HIMAGELIST breadcrumbImages_ = nullptr;
    NativeContextMenu* activeContextMenu_ = nullptr;
    ComPtr<SearchSuggestionList> searchSuggestions_;
    ComPtr<IAutoComplete2> searchAutocomplete_;
    NativeRibbon ribbon_;
    RibbonLayout requestedRibbonLayout_ = RibbonLayout::Authored;
    NativeNamespaceActions namespaceActions_;
    NativeNamespaceActions backgroundActions_;
    NativeNamespaceActions archiveActions_;
    std::unique_ptr<NativeNamespaceCommandChildren> extractDestinations_;
    std::unique_ptr<NativeNamespaceCommandChildren> newItemTypes_;
    std::map<UINT,std::unique_ptr<NativeNamespaceCommandChildren>> ribbonCommandChildren_;
    std::map<UINT,std::vector<std::vector<size_t>>> ribbonCommandPaths_;
    struct FrequentPlacesState;
    std::shared_ptr<FrequentPlacesState> frequentPlacesTask_;
    struct FrequentPlace {ComPtr<IShellItem> item;std::wstring label;std::wstring description;bool pinned=false;};
    std::vector<FrequentPlace> frequentPlaces_;
    std::vector<FrequentPlace> displayedFrequentPlaces_;
    std::uint64_t displayedFrequentPlacesRevision_=0,shutdownPinCloseEntry_=0;
    ShutdownPinTransaction* shutdownPinTransaction_=nullptr; // original owned close/reset stack only
    OwnedCloseFrame* ownedCloseFrame_=nullptr;
    HWND ownedCloseWindow_=nullptr;
    DWORD ownedCloseThread_=0;
    WNDPROC ownedCloseNext_=nullptr;
    std::uint64_t ownedCloseGeneration_=0;
    bool ownedCloseAttached_=false,ownedCloseRetired_=false;
    // Opt-in owned headless fixture only: after the production final-pin gate
    // and genuine Shell menu resolution, before any provider Invoke.
    std::function<HRESULT(UINT,IShellItem*,IShellView*,bool,const ContextMenuEntry&)> headlessShutdownPinResolved_;
    // Fixed diagnostic receipts after the complete production callback returns.
    struct HeadlessShutdownPinFacts {
        std::uint64_t epoch=0,closeEntry=0,displayRevision=0,hookGeneration=0;
        ULONGLONG ownedCloseEntryTick=0;
        bool hookAttached=false,hookTop=false,closeFrame=false,originalClose=false,originalReset=false,sourceRevoked=false;
        bool capturing=false,nativeCloseScope=false;
        UINT64 generation=0;
        unsigned navigation=0;
        UINT displayedCount=0;
        bool closing=false,destroying=false,navigating=false,commandRefresh=false;
        bool window=false,browser=false,view=false,folder=false,location=false,transaction=false;
    };
    HeadlessShutdownPinFacts headlessShutdownPinFacts() const noexcept;
    ULONGLONG headlessOwnedCloseEntryTick_=0;
    // Fixture-only controlled reentry after plain source capture, before its
    // first provider AddRef. Empty in ordinary App operation. No fake provider.
    std::function<void()> headlessBeforeShutdownPinRetain_;
    UINT headlessShutdownPinRetains_=0,headlessShutdownPinCaptureProbes_=0;
    UINT headlessShutdownOriginalLowerCalls_=0,headlessShutdownPlainCloseCalls_=0,headlessShutdownGoneCloseCalls_=0;
    UINT headlessShutdownDeferredCloseCalls_=0; // plain same-original active-native close observation
    ULONGLONG headlessShutdownDispatchTick_=0;
    std::uintptr_t headlessShutdownSavedLower_=0,headlessShutdownActualReceiver_=0;
    bool headlessShutdownCaptureDenied_=false,headlessShutdownCaptureReady_=false;
    // Fixed plain diagnostics of the original native close return/catch and
    // the existing short-circuit class-continuation gate. No provider retention.
    struct HeadlessOwnedCloseState {
        bool original=false,forwarded=false,destroyForwarded=false,hookAttached=false,hookRetired=false;
        std::uint64_t frameGeneration=0,hookGeneration=0;
        std::uintptr_t originalWindow=0,currentWindow=0,ownedWindow=0,originalFrame=0,activeFrame=0;
        DWORD creator=0,process=0;
    };
    bool headlessShutdownNativeCloseEntered_=false,headlessShutdownNativeCloseReturned_=false;
    bool headlessShutdownCaughtAfterDispatch_=false,headlessShutdownPostLowerGateReached_=false;
    UINT headlessShutdownPostLowerGateEvaluated_=0,headlessShutdownPostLowerGatePassed_=0;
    HeadlessOwnedCloseState headlessShutdownPostLowerState_,headlessShutdownCaughtState_;
    UINT headlessShutdownMissingWindowDestructions_=0;
    bool headlessShutdownMissingWindowCleanupCompleted_=false;
    // Admission: 0 no reset entry, 1 entered, 2 thread denied, 3 clone failed,
    // 4 captured fence denied, 5 override installed, 6 returned, 7 exception.
    UINT headlessShutdownPinAdmission_=0;
    HeadlessShutdownPinFacts headlessShutdownPinAdmissionFacts_{};
    struct HeadlessShutdownPinResult {
        UINT index=UI_COLLECTION_INVALIDINDEX;HRESULT result=E_PENDING;
        bool pinned=false;HeadlessShutdownPinFacts entry{},returned{};
    };
    std::array<HeadlessShutdownPinResult,64> headlessShutdownPinResults_{};
    UINT headlessShutdownPinResultCount_=0;
    bool headlessShutdownPinResultOverflow_=false;
    // Explicit fixture-only timing observation of the ordinary guarded path.
    // Never authorizes a headless action or bypasses the shutdown-only sink.
    bool headlessNormalPinDiagnostics_=false;
    UINT headlessNormalPinPhase_=0;
    struct HeadlessNormalPinResult {
        UINT index=UI_COLLECTION_INVALIDINDEX,phase=0;
        bool pinned=false;
        HRESULT result=E_PENDING;
        ULONGLONG entryTick=0,returnTick=0;
        HeadlessShutdownPinFacts entry{},returned{};
    };
    std::array<HeadlessNormalPinResult,64> headlessNormalPinResults_{};
    UINT headlessNormalPinResultCount_=0;
    bool headlessNormalPinResultOverflow_=false;
#if defined(EXPLORER_HOSTED_PIN_PERSISTENCE_FIXTURE)
    // Fixed, non-callback receipts in the separate Hosted fixture binary only.
    // Neither this switch nor the receipts authorize or replace a provider call.
    bool hostedPinReceiptsEnabled_=false;
    // Restrictive owned-only test authority. Populated from the verified GUID
    // target before normal App create; it never grants ordinary invocation.
    ComPtr<IShellItem> hostedPinOwnedTarget_;
    FILE_ID_INFO hostedPinOwnedFile_{};
    HWND hostedPinCreatedOwner_=nullptr;
    struct HostedPinReturn {
        UINT index=UI_COLLECTION_INVALIDINDEX; bool before=false,requested=false,currentAfter=false;
        HRESULT result=E_PENDING; HWND owner=nullptr; IShellItem* item=nullptr; IShellView* site=nullptr;
        UINT64 generation=0; unsigned navigation=0; std::uint64_t revision=0,closeEntry=0;
    };
    struct HostedPinInvoke {
        IShellItemArray* array=nullptr; IShellItem* item=nullptr; IShellView* site=nullptr;
        ComPtr<IShellItemArray> retainedArray;
        HWND owner=nullptr; UINT menuId=0; bool requested=false; HRESULT result=E_PENDING;
        wchar_t verb[32]{};
    };
    struct HostedNormalPinReturn {
        UINT index=UI_COLLECTION_INVALIDINDEX;bool requested=false,closingEntry=false,closingAfter=false;
        HRESULT result=E_PENDING;HWND owner=nullptr;IShellItem* item=nullptr;IShellView* site=nullptr;
        UINT64 generation=0;unsigned navigation=0;std::uint64_t revision=0;
    };
    std::array<HostedPinReturn,64> hostedPinReturns_{};
    std::array<HostedPinInvoke,64> hostedPinInvokes_{};
    std::array<HostedNormalPinReturn,64> hostedNormalPinReturns_{};
    UINT hostedPinReturnCount_=0,hostedPinInvokeCount_=0,hostedNormalPinReturnCount_=0;
    bool hostedPinReceiptOverflow_=false;
    UINT hostedNormalPostQuitCount_=0;
    HWND hostedNormalPostQuitOwner_=nullptr;
    DWORD hostedNormalPostQuitThread_=0;
#endif
    ULONGLONG frequentPlacesReadAt_=0;
    NativeNamespaceActions* activeNamespaceMenu_ = nullptr;
    bool archiveFolder_ = false;
    bool selectionArchive_ = false;
    bool archiveTargetValid_ = false;
    bool namespaceDirty_ = true;
    UINT64 namespaceGeneration_ = 0;
    bool expandCurrent_ = false, showAllFolders_ = false, showLibraries_ = true;
    RibbonContext ribbonContexts_ = RibbonContext::None;
    bool ribbonComputer_ = false;
    bool ribbonNetwork_ = false;
    bool ribbonNetworkActiveDirectory_ = true;
    bool namespaceNetwork_ = false;
    DWORD clipboardSequence_ = MAXDWORD;
    DWORD selectionCount_ = 0;
    SFGAOF selectionAttributes_ = 0;
    // Exact complete native identities/attributes of the committed command
    // target. The view pointer is an identity token only, never dereferenced.
    std::optional<std::vector<Pidl>> commandSelectionIdentities_;
    SFGAOF commandSelectionAttributes_=0;
    IShellView* commandSelectionView_=nullptr;
    NamespaceSelectionKinds selectionKinds_;
    struct SelectionKindsRequest {
        std::unique_ptr<NamespaceCommandStateTask> task;
        ComPtr<IShellItemArray> selection;
        ComPtr<IShellView> view;
        ComPtr<IFolderView2> folderView;
        Pidl location;
        UINT64 generation = 0, sourceRevision = 0;
        unsigned navigation = 0;
        DWORD count = 0;
        bool countKnown = false, useFacade = false, pending = false;
        HRESULT status = S_OK;
        double requestedAt = 0;
    } selectionKindsRequest_;
    // One-shot isolated native regression callback after real Kind worker
    // release and before its captured-source publication fence.
    std::function<void(HRESULT,const NamespaceSelectionKinds&,UINT64,UINT64)> headlessBeforeKindsPublication_;
    // Owned fixture arms a completion observer after real source cancellation
    // and capture, before its first task can finish through native COM pumping.
    std::function<void()> headlessAfterKindsSourceCapture_;
    UINT64 headlessRejectedKindsCompletions_=0,headlessRejectedKindsGeneration_=0,headlessRejectedKindsRevision_=0;
    enum class HeadlessKindsPublicationBoundary : unsigned {
        BeforeCallback,AfterCallback,BeforeNavigate,AfterNavigate,
        BeforeRejectRequest,AfterRejectRequest,BeforeRejectSource,AfterRejectSource,BeforePublish
    };
    struct HeadlessKindsPublicationFact {
        HeadlessKindsPublicationBoundary boundary{};
        UINT64 capturedGeneration=0,capturedRevision=0,requestGeneration=0,requestRevision=0;
        UINT64 namespaceGeneration=0,sourceRevision=0,rejections=0,lastRejectedGeneration=0,lastRejectedRevision=0;
        unsigned capturedNavigation=0,requestNavigation=0,currentNavigation=0;
        bool pending=false,task=false,current=false,refresh=false,cancel=false,navigating=false;
        bool namespaceDirty=false,selectionDirty=false,callback=false;
    };
    struct HeadlessKindsPublicationDiagnostics {
        std::array<HeadlessKindsPublicationFact,64> rows{};
        size_t count=0,dropped=0;
    };
    // Allocated only by the isolated genuine Kind fixture. Recording reads
    // plain owner fields and PIDL bytes; it performs no native calls or actions.
    std::unique_ptr<HeadlessKindsPublicationDiagnostics> headlessKindsPublicationDiagnostics_;
    void recordHeadlessKindsPublicationBoundary(HeadlessKindsPublicationBoundary,
        UINT64 generation,UINT64 revision,unsigned navigation) noexcept;
    UINT64 commandSourceRevision_ = 0;
    std::map<UINT, AppCommandCapability> commandCapabilities_;
    std::map<UINT,std::unique_ptr<NamespaceCommandStateTask>> commandStateTasks_;
    ULONGLONG commandStateStartAt_ = 0;
    std::unique_ptr<NamespaceCommandStateTask> selectionStateBatch_;
    std::vector<AppSelectionStateBinding> selectionStateBindings_;
    ComPtr<INameSpaceTreeControl2> navigationTree_;
    std::vector<ComPtr<IShellItem>> navigationExpansion_;
    size_t navigationExpansionIndex_ = 0;
    std::optional<size_t> navigationExpansionRequested_;
    unsigned navigationExpansionGeneration_ = 0;
    ULONGLONG navigationExpansionDeadline_ = 0;
    HRESULT navigationExpansionStatus_ = S_OK;
    bool navigationExpansionOneTime_ = false;
    std::optional<UINT> visualPage_;
    ShellLibrary library_;
    enum class ContextPage { None, Search, Library };
    ContextPage contextPage_ = ContextPage::None;
    HFONT font_ = nullptr;
    UINT dpi_ = 96;
    Preferences preferences_;
    QuickAccessToolbar quickAccessModel_;
    ComPtr<IExplorerBrowser> browser_;
    ComPtr<IShellView> view_;
    ComPtr<IFolderView2> folderView_;
    DWORD adviseCookie_ = 0;
    std::vector<Pidl> breadcrumbsPidls_;
    std::vector<std::wstring> breadcrumbLabels_;
    // Full native ancestry is independent of the rendered toolbar slots.
    // Width-driven overflow retains every omitted ancestor and adjacency.
    std::vector<size_t> breadcrumbButtons_, breadcrumbHiddenAncestors_;
    bool breadcrumbLayoutActive_ = false;
    ComPtr<BreadcrumbDropTarget> breadcrumbDrop_;
    std::unique_ptr<BreadcrumbEnumerationTask> breadcrumbTask_;
    unsigned breadcrumbGeneration_ = 0;
    POINT breadcrumbMenuPoint_{};
    std::vector<Pidl> history_;
    // Declared before records: ordinary destruction releases records first.
    // Explicit close also releases native aliases and proves worker drain.
    std::unique_ptr<SearchBackingStore> searchBackings_;
    bool searchClosePending_ = false;
    bool searchBackingTeardownActive_ = false;
    ULONGLONG searchTeardownDeadline_ = 0;
    struct SearchLocation {
        Pidl location; Pidl scope; std::wstring query; bool recursive;
        std::wstring base; std::array<std::wstring, 3> filters;
        ComPtr<IShellItemArray> scopes;
        std::vector<SearchScopeRule> scopeRules;
        std::optional<SearchViewPresentation> presentation;
        std::optional<SearchFileProperties> fileProperties;
        bool rememberOnComplete = false;
        bool remembered = false;
        bool importedPresentation = false;
        Pidl completedLocation;
        Pidl historyLocation;
        ComPtr<IShellItem> windowOrigin;
        std::shared_ptr<const SearchBackingLease> backing;
    };
    std::vector<SearchLocation> searchLocations_;
    struct SearchPresentationLocation {
        Pidl location;
        SearchViewPresentation presentation;
        Pidl completedLocation;
        Pidl historyLocation;
    };
    std::vector<SearchPresentationLocation> searchPresentationLocations_;
    int historyIndex_ = -1, pendingHistory_ = -1;
    Pidl currentPidl_;
    Pidl pendingPidl_;
    Pidl searchScope_;
    ComPtr<IShellItem> searchWindowOrigin_;
    Pidl preparedSearchWindowTarget_;
    ComPtr<IShellItemArray> searchScopes_;
    std::vector<SearchScopeRule> searchScopeRules_;
    std::optional<SearchViewPresentation> searchPresentation_;
    std::optional<SearchFileProperties> searchFileProperties_;
    bool searchPresentationPending_ = false;
    HRESULT searchPresentationStatus_ = S_OK;
    Pidl selectionDestination_, selectionChild_;
    ComPtr<IShellItem> selectionTarget_;
    ULONGLONG selectionDeadline_ = 0, selectionRetryAt_ = 0;
    std::vector<std::wstring> recentSearches_;
    std::vector<std::wstring> displayedRecentSearches_;
    std::vector<std::wstring> typedAddresses_;
    std::wstring pendingTypedAddress_;
    Pidl pendingTypedAddressTarget_;
    std::wstring activeQuery_;
    LiveSearchPolicy liveSearchPolicy_;
    bool suppressSearchChanges_ = false;
    bool liveSearchDispatchActive_ = false;
    bool searchNativeCallsActive_ = false;
    unsigned long long searchInteractionRevision_ = 0;
    Pidl pendingDirectSearchTarget_;
    unsigned long long pendingDirectSearchRevision_ = 0;
    std::function<void()> headlessSearchFactoryReentryProbe_;
    std::function<void()> headlessSearchReadbackReentryProbe_;
    std::function<void()> headlessSearchRecreateReentryProbe_;
    std::function<void()> headlessLiveNavigationProbe_;
    std::function<HRESULT()> headlessLiveBrowseProbe_;
    Pidl liveSearchOrigin_;
    int liveSearchHistoryIndex_ = -1;
    std::optional<LiveSearchRequest> pendingLiveSearch_;
    Pidl pendingLiveSearchTarget_;
    HRESULT liveSearchStatus_ = S_OK;
    std::wstring searchBase_;
    std::array<std::wstring, 3> searchFilters_;
    std::shared_ptr<NativeSearchRefinements> searchRefinements_;
    std::wstring searchRefinementQuery_;
    HRESULT searchRefinementStatus_ = E_PENDING;
    bool searchRefinementInspected_ = false;
    std::array<UINT, 3> searchRefinementSelected_{UI_COLLECTION_INVALIDINDEX, UI_COLLECTION_INVALIDINDEX, UI_COLLECTION_INVALIDINDEX};
    std::wstring currentLocation_, currentName_, lastError_;
    ULONGLONG navigationStarted_ = 0, lastNavigationMs_ = 0;
    unsigned navigationCount_ = 0;
};
}
