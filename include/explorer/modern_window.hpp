#pragma once
#include "explorer/app.hpp"
#include "explorer/browser_tabs.hpp"
#include <vector>

namespace explorer {
// A native frame containing independent ExplorerApp browser sessions. Inactive
// sessions retain their actual views/history, rather than reconstructing paths.
class ModernExplorerWindow final {
public:
    using TabId=BrowserTabs::TabId;
    explicit ModernExplorerWindow(HINSTANCE instance,bool headless=false) noexcept;
    ~ModernExplorerWindow();
    ModernExplorerWindow(const ModernExplorerWindow&)=delete;
    ModernExplorerWindow& operator=(const ModernExplorerWindow&)=delete;
    HRESULT create(const std::wstring& location,const SearchWindowContext* search=nullptr);
    HRESULT addTab(const std::wstring& location,TabId* result=nullptr,const SearchWindowContext* search=nullptr);
    HRESULT selectTab(TabId id);
    HRESULT closeTab(TabId id);
    HRESULT shutdown(DWORD timeout=10000) noexcept;
    bool preprocess(MSG& message);
    int run(int showCommand);
    HWND window() const noexcept {return window_;}
    size_t tabCount() const noexcept {return entries_.size();}
    TabId activeTab() const noexcept {return active_;}
    std::vector<TabId> tabIds() const {return tabs_.ids();}
    ExplorerApp* activeBrowser() const noexcept;
    static constexpr UINT ToggleFullscreen=ExplorerApp::EmbeddedToggleFullscreen;
private:
    struct Entry {TabId id=0;ComPtr<ExplorerApp> app;HWND child=nullptr;bool closing=false;};
    static LRESULT CALLBACK windowProc(HWND,UINT,WPARAM,LPARAM);
    LRESULT onMessage(UINT,WPARAM,LPARAM);
    void layout();
    void refreshFont();
    void updateTitle();
    void collectDestroyed(HWND child,ExplorerApp* app);
    void collectRetired();
    void retireFailedTab(TabId id,ExplorerApp* app) noexcept;
    void showFailure(HRESULT status);
    HINSTANCE instance_;
    bool headless_=false,closing_=false,busy_=false,closeRequested_=false,fullscreen_=false;
    HWND window_=nullptr;
    HFONT font_=nullptr;
    UINT dpi_=96;
    BrowserTabs tabs_;
    std::vector<Entry> entries_;
    TabId active_=BrowserTabs::NoTab;
    std::uint64_t activationRevision_=0;
    HRESULT shutdownStatus_=S_OK;
    WINDOWPLACEMENT placement_{sizeof(WINDOWPLACEMENT)};
    LONG_PTR windowStyle_=0;
};
}
