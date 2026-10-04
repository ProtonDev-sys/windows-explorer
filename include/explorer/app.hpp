#pragma once
#include "explorer/core.hpp"
#include "explorer/commands.hpp"
#include "explorer/context_menu.hpp"
#include "explorer/quick_access.hpp"
#include "explorer/share.hpp"
#include "explorer/library.hpp"
#include "explorer/headless_visual.hpp"
#include "explorer/ribbon.hpp"
#include "explorer/namespace_actions.hpp"
#include "explorer/status.hpp"
#include "explorer/breadcrumb.hpp"
#include "explorer/app_commands.hpp"
#include "explorer/search_history.hpp"
#include "explorer/address_history.hpp"
#include "explorer/search.hpp"
#include "explorer/live_search.hpp"
#include <shlobj.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <servprov.h>
#include <wrl/client.h>
#include <atomic>
#include <memory>
#include <vector>
#include <future>
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
    double namespacePreparationMs = 0;
    double providerCatalogMs = 0;
    double stateTaskSchedulingMs = 0;
    double contextMs = 0;
    double ribbonInvalidationMs = 0;
};

class ExplorerApp final : public IExplorerBrowserEvents, public IServiceProvider,
                          public IExplorerPaneVisibility, public ICommDlgBrowser3, public IFolderFilter {
public:
    ExplorerApp(HINSTANCE instance, bool headless, RibbonLayout ribbonLayout = RibbonLayout::Authored);
    HRESULT create(const std::wstring& location);
    int run(int showCommand);
    int headlessBenchmark(const std::filesystem::path& report);
    int headlessSmoke(const std::filesystem::path& report);
    int headlessVisual(const PrivateDesktop& desktop, const std::filesystem::path& screenshot,
                       const std::filesystem::path& report, const VisualScene& scene);
    HWND window() const noexcept { return window_; }
    HRESULT shutdownStatus() const noexcept { return shutdownStatus_; }
    HeadlessCommandTimings headlessCommandTimings() const noexcept { return commandTimings_; }
    HeadlessCommandTimings lastHeadlessCommandTimings() const noexcept { return lastCommandTimings_; }
    void resetHeadlessCommandTimings() noexcept { if(headless_){commandTimings_={};lastCommandTimings_={};} }
    bool commandStatesPending() const noexcept {
        if(selectionStateBatch_||!commandStateTasks_.empty())return true;
        for(const auto& entry:commandCapabilities_)if(entry.second.status==E_PENDING)return true;
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
    ~ExplorerApp();
    HRESULT shutdownStatus_ = S_OK;
    static LRESULT CALLBACK windowProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK editProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT onMessage(UINT, WPARAM, LPARAM);
    HRESULT createControls();
    void layout();
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
    void updateStatus();
    void updateBreadcrumbs();
    void updateContextTabs();
    RibbonCommandState ribbonState(UINT command);
    std::vector<RibbonItem> ribbonItems(UINT command);
    HRESULT executeRibbon(UINT command);
    HRESULT executeRibbonItem(UINT command, UINT item);
    void updateNamespace();
    AppCommandContext commandContext() const;
    HRESULT applyNavigationOptions();
    void advanceNavigationExpansion();
    void cancelCommandStates();
    void startPendingCommandStates();
    void refreshFrequentPlaces();
    void cancelFrequentPlaces();
    void pollFrequentPlaces();
    HRESULT pinFrequentPlace(UINT item,bool pinned);
    void pollCommandStates();
    void completeCommandState(UINT command,HRESULT status,const NamespaceCommandState* native);
    void initializeBreadcrumbDrop();
    void beginBreadcrumbMenu(UINT command);
    void pollBreadcrumbMenu();
    void fitBreadcrumbs(int width);
    HRESULT cycleFocus(bool backwards);
    HRESULT toggleFullscreen();
    HRESULT sizeColumns();
    HRESULT toggleColumn(const PROPERTYKEY& key);
    HRESULT saveSearch();
    HRESULT captureActiveSearchPresentation();
    HRESULT openFileLocation();
    HRESULT newItemMenu();
    HRESULT shareFiles();
    HRESULT newLibrary();
    HRESULT includeLibraryFolder();
    HRESULT commitLibrary();
    void reloadLibrary();
    void applyPendingSelection();
    HRESULT startSearch(const std::wstring& query, bool recursive,
                        std::optional<size_t> category = {}, const std::wstring& filter = L"",
                        const LiveSearchRequest* liveRequest = nullptr);
    void setSearchText(const std::wstring& text, bool synchronizePolicy = true);
    void cancelLiveSearch();
    void scheduleLiveSearch();
    HRESULT processLiveSearch();
    struct LiveSearchDispatchScope {
        ExplorerApp& owner;
        bool previous;
        explicit LiveSearchDispatchScope(ExplorerApp& value):owner(value),previous(value.liveSearchDispatchActive_){owner.liveSearchDispatchActive_=true;}
        ~LiveSearchDispatchScope(){owner.liveSearchDispatchActive_=previous;if(!previous)owner.scheduleLiveSearch();}
    };
    bool completeLiveSearchNavigation(PCIDLIST_ABSOLUTE target, HRESULT result);
    void rememberQuery(const std::wstring& query);
    void editAddress();
    void finishAddress(bool navigateNow);
    HMENU createTypedAddressMenu() const;
    void rememberAddressNavigation(PCIDLIST_ABSOLUTE location);
    void refreshAddressHistoryPolicy();
    HRESULT createBrowser();
    void destroyBrowser();
    HRESULT recreateBrowser();
    HRESULT browseHistory(int offset);
    HRESULT setView(ViewMode mode);
    HRESULT setSort(const PROPERTYKEY& key);
    HRESULT setGroup(const PROPERTYKEY& key);
    HRESULT selection(ComPtr<IShellItemArray>& out, bool folderIfEmpty = false);
    HRESULT currentFolder(ComPtr<IShellItem>& out);
    HRESULT chooseDestination(bool move);
    HRESULT nativeVerb(const wchar_t* verb, bool folderIfEmpty = false);
    HRESULT newFolder();
    HRESULT newText();
    HRESULT showProperties(const wchar_t* page);
    HRESULT archive(bool extract);
    HRESULT makeShortcut(bool fromClipboard);
    void popup(UINT command, HWND anchor = nullptr);
    void showError(HRESULT hr, const wchar_t* action);
    void persist();
    void setStatus(const std::wstring& text);
    void updateCaptionIcon();
    HRESULT refreshCabinetPolicy();
    void updateFrameTitle();
    void updateRibbonCollapseButton();
    HRESULT openNewWindow(const std::wstring& location);
    int px(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }
    std::atomic<ULONG> references_{1};
    HINSTANCE instance_;
    bool headless_;
    bool closing_ = false;
    bool browserInitialized_ = false;
    bool addressEditing_ = false;
    bool navigating_ = false;
    bool searchActive_ = false;
    bool pendingSearchActive_ = false;
    bool searchBackground_ = false;
    bool pendingSearchBackground_ = false;
    bool checkboxes_ = false;
    bool ascending_ = true;
    bool selectionStateDirty_ = true;
    bool deferredUpdateQueued_ = false;
    bool commandRefreshActive_ = false;
    bool commandRefreshPending_ = false;
    bool commandStatesCancelPending_ = false;
    bool commandItemsRefreshPending_ = false;
    unsigned long long deferredCommandUpdates_ = 0;
    std::function<void()> headlessCommandReentryProbe_;
    bool selectionFilesystem_ = false;
    bool selectionHidden_ = false;
    bool selectionShareable_ = false;
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
    HeadlessCommandTimings commandTimings_,lastCommandTimings_;
    bool searchResizing_ = false;
    int searchDragOffset_ = 0;
    LONG_PTR windowStyle_ = 0;
    WINDOWPLACEMENT windowPlacement_{sizeof(WINDOWPLACEMENT)};
    RECT windowRect_{};
    HWND window_ = nullptr, nav_ = nullptr, address_ = nullptr;
    HWND breadcrumbs_ = nullptr, search_ = nullptr, status_ = nullptr, statusView_ = nullptr, addressActions_ = nullptr;
    HWND ribbonCollapse_=nullptr,ribbonCollapseTooltip_=nullptr;
    std::wstring ribbonCollapseTip_;
    HICON folderIcon_ = nullptr;
    HICON largeIcon_ = nullptr;
    HIMAGELIST breadcrumbImages_ = nullptr;
    NativeContextMenu* activeContextMenu_ = nullptr;
    NativeShare nativeShare_;
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
    ULONGLONG frequentPlacesReadAt_=0;
    NativeNamespaceActions* activeNamespaceMenu_ = nullptr;
    bool archiveFolder_ = false;
    bool selectionArchive_ = false;
    bool archiveTargetValid_ = false;
    bool namespaceDirty_ = true;
    UINT64 namespaceGeneration_ = 0;
    bool undoAvailable_ = false, redoAvailable_ = false;
    bool expandCurrent_ = false, showAllFolders_ = false, showLibraries_ = true;
    RibbonContext ribbonContexts_ = RibbonContext::None;
    bool ribbonComputer_ = false;
    bool ribbonNetwork_ = false;
    bool ribbonNetworkActiveDirectory_ = true;
    bool namespaceNetwork_ = false;
    DWORD clipboardSequence_ = MAXDWORD;
    bool clipboardFiles_ = false;
    DWORD selectionCount_ = 0;
    SFGAOF selectionAttributes_ = 0;
    SelectionStatus selectionStatus_;
    NamespaceSelectionKinds selectionKinds_;
    std::map<UINT, AppCommandCapability> commandCapabilities_;
    std::map<UINT,std::unique_ptr<NamespaceCommandStateTask>> commandStateTasks_;
    std::unique_ptr<NamespaceCommandStateTask> selectionStateBatch_;
    std::vector<AppSelectionStateBinding> selectionStateBindings_;
    ComPtr<INameSpaceTreeControl2> navigationTree_;
    std::vector<ComPtr<IShellItem>> navigationExpansion_;
    size_t navigationExpansionIndex_ = 0;
    std::optional<size_t> navigationExpansionRequested_;
    unsigned navigationExpansionGeneration_ = 0;
    ULONGLONG navigationExpansionDeadline_ = 0;
    HRESULT navigationExpansionStatus_ = S_OK;
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
    ComPtr<BreadcrumbDropTarget> breadcrumbDrop_;
    std::unique_ptr<BreadcrumbEnumerationTask> breadcrumbTask_;
    unsigned breadcrumbGeneration_ = 0;
    POINT breadcrumbMenuPoint_{};
    std::vector<Pidl> history_;
    struct SearchLocation {
        Pidl location; Pidl scope; std::wstring query; bool recursive;
        std::wstring base; std::array<std::wstring, 3> filters;
        ComPtr<IShellItemArray> scopes;
        std::vector<SearchScopeRule> scopeRules;
        std::optional<SearchViewPresentation> presentation;
        bool rememberOnComplete = false;
        bool remembered = false;
    };
    std::vector<SearchLocation> searchLocations_;
    struct SearchPresentationLocation {
        Pidl location;
        SearchViewPresentation presentation;
    };
    std::vector<SearchPresentationLocation> searchPresentationLocations_;
    int historyIndex_ = -1, pendingHistory_ = -1;
    Pidl currentPidl_;
    Pidl pendingPidl_;
    Pidl searchScope_;
    ComPtr<IShellItemArray> searchScopes_;
    std::vector<SearchScopeRule> searchScopeRules_;
    std::optional<SearchViewPresentation> searchPresentation_;
    bool searchPresentationPending_ = false;
    HRESULT searchPresentationStatus_ = S_OK;
    Pidl selectionDestination_, selectionChild_;
    ComPtr<IShellItem> selectionTarget_;
    ULONGLONG selectionDeadline_ = 0, selectionRetryAt_ = 0;
    std::vector<std::wstring> recentSearches_;
    std::vector<std::wstring> typedAddresses_;
    std::wstring pendingTypedAddress_;
    Pidl pendingTypedAddressTarget_;
    std::wstring activeQuery_;
    LiveSearchPolicy liveSearchPolicy_;
    bool suppressSearchChanges_ = false;
    bool liveSearchDispatchActive_ = false;
    unsigned long long searchInteractionRevision_ = 0;
    Pidl pendingDirectSearchTarget_;
    unsigned long long pendingDirectSearchRevision_ = 0;
    std::function<void()> headlessSearchFactoryReentryProbe_;
    Pidl liveSearchOrigin_;
    int liveSearchHistoryIndex_ = -1;
    std::optional<LiveSearchRequest> pendingLiveSearch_;
    Pidl pendingLiveSearchTarget_;
    HRESULT liveSearchStatus_ = S_OK;
    std::wstring searchBase_;
    std::array<std::wstring, 3> searchFilters_;
    std::wstring currentLocation_, currentName_, lastError_;
    ULONGLONG navigationStarted_ = 0, lastNavigationMs_ = 0;
    unsigned navigationCount_ = 0;
    std::future<HRESULT> archiveTask_;
    std::wstring archiveAction_;
};
}
