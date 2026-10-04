#pragma once
#include "explorer/core.hpp"
#include "explorer/commands.hpp"
#include "explorer/context_menu.hpp"
#include "explorer/quick_access.hpp"
#include "explorer/share.hpp"
#include "explorer/library.hpp"
#include <shlobj.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <servprov.h>
#include <wrl/client.h>
#include <atomic>
#include <memory>
#include <vector>
#include <future>
#include <array>
#include <optional>

namespace explorer {
using Microsoft::WRL::ComPtr;
struct PidlDeleter {
    using pointer = LPITEMIDLIST;
    void operator()(pointer p) const noexcept { CoTaskMemFree(p); }
};
using Pidl = std::unique_ptr<ITEMIDLIST, PidlDeleter>;

class ExplorerApp final : public IExplorerBrowserEvents, public IServiceProvider,
                          public IExplorerPaneVisibility, public ICommDlgBrowser3, public IFolderFilter {
public:
    ExplorerApp(HINSTANCE instance, bool headless);
    HRESULT create(const std::wstring& location);
    int run(int showCommand);
    int headlessSmoke(const std::filesystem::path& report);
    HWND window() const noexcept { return window_; }
    bool preprocess(MSG& message);
    HRESULT navigate(const std::wstring& location);
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
    HRESULT STDMETHODCALLTYPE OnDefaultCommand(IShellView*) override { return S_FALSE; }
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
    static LRESULT CALLBACK windowProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK editProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT onMessage(UINT, WPARAM, LPARAM);
    void createControls();
    void layout();
    void rebuildRibbon();
    void rebuildQuickAccess();
    void updateCommands();
    void updateStatus();
    void updateBreadcrumbs();
    void updateContextTabs();
    HRESULT cycleFocus(bool backwards);
    HRESULT toggleFullscreen();
    HRESULT sizeColumns();
    HRESULT toggleColumn(const PROPERTYKEY& key);
    HRESULT saveSearch();
    HRESULT openFileLocation();
    HRESULT newItemMenu();
    HRESULT shareFiles();
    HRESULT newLibrary();
    HRESULT includeLibraryFolder();
    HRESULT commitLibrary();
    void reloadLibrary();
    void applyPendingSelection();
    HRESULT startSearch(const std::wstring& query, bool recursive,
                        std::optional<size_t> category = {}, const std::wstring& filter = L"");
    void editAddress();
    void finishAddress(bool navigateNow);
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
    bool selectionFilesystem_ = false;
    bool selectionHidden_ = false;
    bool selectionShareable_ = false;
    bool filesystemFolder_ = false;
    bool physicalDirectory_ = false;
    bool fullscreen_ = false;
    bool searchRecursive_ = true;
    LONG_PTR windowStyle_ = 0;
    WINDOWPLACEMENT windowPlacement_{sizeof(WINDOWPLACEMENT)};
    RECT windowRect_{};
    HWND window_ = nullptr, tabs_ = nullptr, nav_ = nullptr, address_ = nullptr;
    HWND breadcrumbs_ = nullptr, search_ = nullptr, status_ = nullptr, file_ = nullptr;
    HWND quickAccess_ = nullptr;
    HIMAGELIST quickAccessImages_ = nullptr;
    NativeContextMenu* activeContextMenu_ = nullptr;
    NativeShare nativeShare_;
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
    std::vector<HWND> ribbonControls_;
    std::vector<HIMAGELIST> ribbonImages_;
    std::vector<Pidl> breadcrumbsPidls_;
    std::vector<Pidl> history_;
    struct SearchLocation {
        Pidl location; Pidl scope; std::wstring query; bool recursive;
        std::wstring base; std::array<std::wstring, 3> filters;
    };
    std::vector<SearchLocation> searchLocations_;
    int historyIndex_ = -1, pendingHistory_ = -1;
    Pidl currentPidl_;
    Pidl searchScope_;
    Pidl selectionDestination_, selectionChild_;
    ComPtr<IShellItem> selectionTarget_;
    ULONGLONG selectionDeadline_ = 0, selectionRetryAt_ = 0;
    std::vector<std::wstring> recentSearches_;
    std::wstring activeQuery_;
    std::wstring searchBase_;
    std::array<std::wstring, 3> searchFilters_;
    std::wstring currentLocation_, currentName_, lastError_;
    ULONGLONG navigationStarted_ = 0, lastNavigationMs_ = 0;
    unsigned navigationCount_ = 0;
    std::future<HRESULT> archiveTask_;
    std::wstring archiveAction_;
};
}
